/**
 * 007: Nightfire - Recompiled Game Entry Point
 *
 * One source for both executables: nightfire.exe (default.xbe) and, built
 * with NIGHTFIRE_DRIVING, nightfire_driving.exe (Driving.xbe).
 *
 * 1. Find the game files and load the XBE
 * 2. Initialize the Xbox memory layout (map data sections to original VAs)
 * 3. Initialize the Xbox kernel replacement layer
 * 4. Initialize the kernel bridge (thunk table in Xbox memory)
 * 5. Set up game file paths for I/O redirection
 * 6. Initialize the stack pointer
 * 7. Install VEH crash handler for diagnostics
 * 8. Call the game's original entry point (recompiled)
 *
 * XBEs (European PAL disc, base 0x00010000):
 *   default.xbe  entry 0x000EB2AC
 *   Driving.xbe  entry 0x0010E777
 * The North-American disc's XBEs are the same bytes apart from the header's
 * signature and certificate (game region, rating, version), so every address
 * here and in manual_*.c holds for both; what differs is the console the
 * disc expects (nf_disc_is_pal) and the files it carries.
 */

#include <windows.h>
#include <shlobj.h>
#include <dbghelp.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <io.h>

/* xboxrecomp runtime headers */
#include <xbox/xboxrecomp.h>
#include "nv2a/nv2a_mmio_hook.h"
#include "ohci.h"

/*
 * If xboxrecomp.h is not an umbrella header in your setup, include
 * the individual headers directly:
 *
 * #include "kernel.h"
 * #include "xbox_memory_layout.h"
 * #include "d3d8_xbox.h"
 * #include "dsound_xbox.h"
 * #include "xinput_xbox.h"
 */

/* ── Global register state (defined in xbox_memory_layout.c) ── */

/* RECOMP_TLS is not optional here. The runtime defines these thread-local, and
 * a plain `extern` referencing a __declspec(thread) variable does not resolve
 * to the calling thread's copy -- it resolves to the image's TLS template. The
 * host side then writes g_esp somewhere the generated code never reads, so the
 * guest starts with every register at zero and faults immediately, having
 * apparently ignored the setup that visibly ran. */
extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
extern RECOMP_TLS uint32_t g_ebx, g_esi, g_edi;
extern RECOMP_TLS uint32_t g_seh_ebp;
/* x87 and SSE state. Global for the same reason the volatile GPRs are: one
 * guest routine can lift to several C functions, so a value written in one
 * body is read in the next. Defined in xbox_memory_layout.c like the rest of
 * the register file. */
extern RECOMP_TLS double g_fp_stack[8];
extern RECOMP_TLS int g_fp_top;
extern RECOMP_TLS uint16_t g_fp_control_word;
extern RECOMP_TLS uint32_t g_mxcsr;
extern RECOMP_TLS int g_fp_cmp;
extern RECOMP_TLS RecompXmm g_xmm0, g_xmm1, g_xmm2, g_xmm3;
extern RECOMP_TLS RecompXmm g_xmm4, g_xmm5, g_xmm6, g_xmm7;
extern ptrdiff_t g_xbox_mem_offset;

/* ── XBE Constants ─────────────────────────────────────────── */

/*
 * TODO: Set these from your xbe_parser output.
 * Run: py -3 -m tools.xbe_parser game/default.xbe
 */
#ifdef NIGHTFIRE_DRIVING
#define YOUR_GAME_ENTRY_POINT   0x0010E777
#define YOUR_GAME_XBE_NAME      "Driving.xbe"
#else
#define YOUR_GAME_ENTRY_POINT   0x000EB2AC  /* XBE entry point VA */
#define YOUR_GAME_XBE_NAME      "default.xbe"
#endif

/* ── Game files ────────────────────────────────────────────── */

/* The extracted disc, found once at start-up. In order:
 *   RECOMP_GAME_DIR                   another copy of the game files
 *   game\ in the working directory    started from the repository
 *   game\ beside the executable       a build copied somewhere with the game
 *   ..\..\game from the executable    build\Release\ inside the repository
 * The handoff to the other executable inherits the working directory and
 * RECOMP_GAME_DIR, so both always agree on the folder. */
static char g_game_dir[MAX_PATH];

static int nf_dir_has_xbe(const char *dir)
{
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\" YOUR_GAME_XBE_NAME, dir);
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

static int nf_find_game_dir(void)
{
    const char *env = getenv("RECOMP_GAME_DIR");
    char exe_dir[MAX_PATH], *slash;

    if (env && *env) {
        snprintf(g_game_dir, sizeof g_game_dir, "%s", env);
        return nf_dir_has_xbe(g_game_dir);
    }
    snprintf(g_game_dir, sizeof g_game_dir, "game");
    if (nf_dir_has_xbe(g_game_dir))
        return 1;
    if (!GetModuleFileNameA(NULL, exe_dir, (DWORD)sizeof exe_dir))
        return 0;
    if ((slash = strrchr(exe_dir, '\\')) != NULL)
        *slash = '\0';
    snprintf(g_game_dir, sizeof g_game_dir, "%s\\game", exe_dir);
    if (nf_dir_has_xbe(g_game_dir))
        return 1;
    snprintf(g_game_dir, sizeof g_game_dir, "%s\\..\\..\\game", exe_dir);
    return nf_dir_has_xbe(g_game_dir);
}

/* Which console the disc in game\ was made for, from its XBE certificate's
 * game region (cert +0xA0): 1 North America, 2 Japan, 4 rest of the world.
 * The European disc says 4 and gets a PAL console (50 Hz, eurocom\25_fps,
 * driving pal\<lang>), the North-American one 1 and an NTSC console (60 Hz,
 * eurocom\30_fps, driving ntsc\eng). An unreadable header counts as the
 * European disc, the one this port was made on. */
static int nf_disc_is_pal(void)
{
    char path[MAX_PATH];
    unsigned char h[0x1000];
    uint32_t base, cert, region;
    size_t n = 0;
    FILE *f;

    if (!nf_find_game_dir())
        return 1;
    snprintf(path, sizeof path, "%s\\" YOUR_GAME_XBE_NAME, g_game_dir);
    if ((f = fopen(path, "rb")) != NULL) {
        n = fread(h, 1, sizeof h, f);
        fclose(f);
    }
    if (n < 0x120 || memcmp(h, "XBEH", 4) != 0)
        return 1;
    memcpy(&base, h + 0x104, 4);
    memcpy(&cert, h + 0x118, 4);
    if (cert < base || cert - base + 0xA4 > n)
        return 1;
    memcpy(&region, h + (cert - base) + 0xA0, 4);
    return (region & 4) || !(region & 3);
}

#ifdef NIGHTFIRE_DRIVING
/* Whether the EA BIGF archive game\<viv> holds a file whose name starts with
 * prefix (case ignored). The header: "BIGF", total size, then big-endian
 * entry count and header size; each entry a big-endian offset and size and
 * the NUL-terminated name. */
static int nf_viv_has(const char *viv, const char *prefix)
{
    char path[MAX_PATH];
    unsigned char h[16], *dir;
    uint32_t count, hsize, i, o = 16;
    size_t plen = strlen(prefix);
    int found = 0;
    FILE *f;

    snprintf(path, sizeof path, "%s\\%s", g_game_dir, viv);
    if ((f = fopen(path, "rb")) == NULL)
        return 0;
    if (fread(h, 1, sizeof h, f) != sizeof h || memcmp(h, "BIGF", 4) != 0) {
        fclose(f);
        return 0;
    }
    count = (uint32_t)h[8] << 24 | h[9] << 16 | h[10] << 8 | h[11];
    hsize = (uint32_t)h[12] << 24 | h[13] << 16 | h[14] << 8 | h[15];
    if (hsize <= 16 || hsize > (1u << 20) || (dir = malloc(hsize + 1)) == NULL) {
        fclose(f);
        return 0;
    }
    rewind(f);
    if (fread(dir, 1, hsize, f) == hsize) {
        dir[hsize] = '\0';
        for (i = 0; i < count && o + 8 < hsize && !found; i++) {
            const char *name = (const char *)dir + o + 8;
            found = _strnicmp(name, prefix, plen) == 0;
            o += 8 + (uint32_t)strlen(name) + 1;
        }
    }
    free(dir);
    fclose(f);
    return found;
}
#endif

/* ── Forward declarations ──────────────────────────────────── */

static BOOL load_xbe(const char *path, void **out_data, size_t *out_size);

/* Recompiled entry point (generated by recomp pipeline) */
extern void xbe_entry_point(void);
/* The flat dispatch table (generated, recomp_types.h declares it too). */
extern int recomp_dispatch_init(void);

/* ── VEH crash handler ─────────────────────────────────────── */

/*
 * Vectored Exception Handler for crash diagnostics.
 *
 * When the recompiled game hits an access violation, this handler prints
 * the faulting address, all Xbox register values, and a native stack trace.
 * This is your primary debugging tool during bring-up.
 *
 * Customize this for your game:
 *   - Add game-specific address checks (GPU register probes, etc.)
 *   - Add dumps of game-specific globals (heap handles, state flags)
 *   - Add SEH simulation if your game uses __try/__except
 */
/* Name the guest function a fault happened in, and recover the call chain.
 *
 * Recompiled code faults as ordinary native code, so the exception record
 * carries a host RIP and nothing else -- there is no guest program counter to
 * report, and the host address changes every build. Two things recover the
 * guest view:
 *
 *   - every generated function is a real symbol in the image (sub_005A03C0
 *     and so on), so the linker's PDB already maps host address back to guest
 *     function. dbghelp turns an anonymous RIP into that name.
 *
 *   - every lifted call pushes its guest return address onto the guest stack
 *     before jumping, so the stack still holds the chain. Scanning up from esp
 *     for values inside the code sections recovers it.
 *
 * ponytail: the stack scan is a scan, not a frame walk -- these are FPO frames
 * with no reliable ebp chain, so there is nothing to walk. It over-reports,
 * since addresses from returned-from calls linger below esp, but naming the
 * guest function is the whole question at a fault.
 *
 * Requires linking dbghelp and keeping the .pdb beside the .exe.
 */
static void print_guest_context(void *rip)
{
    /* SYMBOL_INFO is variable-length: the name is written past the struct, so
     * it must be over-allocated with MaxNameLen set to the slack. */
    char buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
    DWORD64 disp = 0;

    memset(buf, 0, sizeof(buf));
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    if (SymFromAddr(GetCurrentProcess(), (DWORD64)(uintptr_t)rip, &disp, sym))
        fprintf(stderr, "  in %s+0x%llX\n",
                sym->Name, (unsigned long long)disp);

    if (g_xbox_mem_offset && g_esp) {
        const uint32_t *sp =
            (const uint32_t *)((uintptr_t)g_xbox_mem_offset + g_esp);
        int shown = 0, i;
        fprintf(stderr, "  guest stack (return addresses, innermost first):\n");
        for (i = 0; i < 256 && shown < 24; i++) {
            uint32_t v = sp[i];
            if (v > g_xbox_code_lo && v < g_xbox_code_hi) {
                fprintf(stderr, "    [esp+%-4d] 0x%08X\n", i * 4, v);
                shown++;
            }
        }
        /* When the frame is gone, say so, and fall back on what is left.
         *
         * A fault inside a block copy leaves no return address in range: the
         * copy is a host memmove and its caller's frame is further up than
         * this reaches. Worse, a copy that ran past its buffer overwrites the
         * stack itself, so the list comes out empty exactly when it is most
         * needed -- and an empty list under a promising heading reads like
         * "nothing to see" rather than "the evidence is destroyed". */
        if (!shown)
            fprintf(stderr, "    (no return addresses in range)\n");

        /* Everything below only pays for itself when the scan came back
         * empty. A crash where the chain was recovered already says what
         * happened, and burying that under another twenty lines makes the
         * common report worse to read. */
        if (!shown) {
            uint32_t k;

            /* The indirect-call history, which the stack cannot overwrite.
             * recomp_manual.c prints this too, but only from
             * recomp_icall_fail_log -- a page fault inside a block copy
             * never reaches that path, and this is the same information by
             * the route the fault actually took.
             *
             * Declared here rather than included: main.c does not pull in
             * recomp_types.h, and recomp_manual.c spells it the same way. */
            extern volatile uint32_t g_icall_trace[16];
            extern volatile uint32_t g_icall_trace_idx;

            fprintf(stderr, "  recent ICALL targets:");
            for (k = 0; k < 16; k++)
                fprintf(stderr, " %08X",
                        g_icall_trace[(g_icall_trace_idx + k) & 15]);
            fprintf(stderr, "\n");

            /* The frame unfiltered. The pointers and lengths handed to
             * whatever faulted live here and look nothing like code, so the
             * filter above drops exactly what is wanted. On the crash this
             * was written for, one of these words was the end of the mapped
             * RAM mirror -- which is what identified the fault as a copy
             * running off its buffer rather than a stray pointer. */
            for (i = 0; i < 20; i++)
                fprintf(stderr, "    raw[esp+%-4d] 0x%08X\n", i * 4, sp[i]);
        }
    }
}

/* The emulated APU.
 *
 * Two entry points that had no callers anywhere in the tree. apu_mmio_hook.c
 * documents apu_hook_handle_mmio as "called from VEH in main.c" and no main.c
 * called it; apu_decode_and_handle's first line is `if (!g_apu_state) return
 * false` and nothing assigned g_apu_state. So with RECOMP_AC97_READY set the
 * APU's registers were unmapped to be trapped, and every trap was then
 * declined and surfaced as an access violation on the first register
 * DirectSound touched. The VEH below now reaches apu_hook_handle_mmio
 * through the toolkit's xbox_VehMmioFault.
 *
 * Declared rather than included so this does not depend on src/apu being on
 * the include path. */
typedef struct MCPXAPUState MCPXAPUState;
extern MCPXAPUState *mcpx_apu_init_standalone(uint8_t *ram_ptr);
extern MCPXAPUState *g_apu_state;

/* Tell the pushbuffer executor where D3D's fence semaphore lives.
 *
 * Both XBEs link the XDK's D3D, which waits for GPU progress by spinning on
 * the word that BACK_END_WRITE_SEMAPHORE_RELEASE (0x1D70) writes, and keeps
 * the pointer to it at device+0x34. Without a target the executor drops every
 * release, the word stays at its first value, and the first fence wait after
 * D3D init (driving: sub_0016CAE0) never returns.
 *
 * The device does not exist until the title creates it, so a host thread waits
 * for it rather than the guest code being patched -- the generated code is
 * regenerated, and an edit there would be lost. It keeps watching, in case a
 * device is created again (the main game's first lift re-registered on every
 * kick). */
#ifdef NIGHTFIRE_DRIVING
#define NF_D3D_DEVICE_VA  0x00175418u
#else
#define NF_D3D_DEVICE_VA  0x001117C0u
#endif
#define NF_D3D_SEM_PTR    0x34u
/* The device's pushbuffer-space event. D3D's fence wait (driving 0x0016CAE0,
 * main 0x00106650) patches NO_OPERATION 5 into the pushbuffer at the fence it
 * waits for and sleeps here; on the console only that interrupt sets it. */
#define NF_D3D_SPACE_EVENT 0x2564u
#define NF_SOFTMETHOD_SPACE 5u

extern void nv2a_pb_set_semaphore_target(uint32_t guest_va);
extern void xbox_Nv2aBindSoftwareMethodEvent(uint32_t method, uint32_t guest_va);

static DWORD WINAPI d3d_semaphore_thread(LPVOID unused)
{
    const uint8_t *mem = (const uint8_t *)g_xbox_mem_offset;
    uint32_t current = 0;
    (void)unused;

    for (;;) {
        uint32_t dev = *(volatile const uint32_t *)(mem + NF_D3D_DEVICE_VA);
        uint32_t sem = dev ? *(volatile const uint32_t *)
                                 (mem + dev + NF_D3D_SEM_PTR) : 0;
        if (sem && sem != current) {
            nv2a_pb_set_semaphore_target(sem);
            xbox_Nv2aBindSoftwareMethodEvent(NF_SOFTMETHOD_SPACE, dev + NF_D3D_SPACE_EVENT);
            fprintf(stderr, "[BOOT] D3D device 0x%08X: fence semaphore at 0x%08X\n",
                    dev, sem);
            current = sem;
        }
        Sleep(current ? 50 : 1);
    }
}

#ifdef NIGHTFIRE_DRIVING

/* RECOMP_STACK_SAMPLE_SECS=<n>: every n seconds, the guest return addresses
 * on the main thread's stack. The watchdog only fires when the registers stop
 * changing; a title that keeps drawing a loading screen while it waits for
 * something never trips it, and this is what says where it waits. */
static volatile uint32_t *s_sample_esp;

static DWORD WINAPI driving_stack_sampler(LPVOID arg)
{
    DWORD period = (DWORD)(uintptr_t)arg * 1000;
    const uint8_t *mem = (const uint8_t *)g_xbox_mem_offset;

    for (;;) {
        uint32_t esp, i;
        int shown = 0;
        Sleep(period);
        esp = *s_sample_esp;
        fprintf(stderr, "[SAMPLE] main esp=%08X:", esp);
        for (i = 0; i < 1024 && shown < 32; i++) {
            uint32_t a = esp + i * 4, v;
            if (a < 0x00010000u || a >= 0x04000000u)
                break;
            v = *(const uint32_t *)(mem + a);
            if (v > g_xbox_code_lo && v < g_xbox_code_hi) {
                fprintf(stderr, " %08X", v);
                shown++;
            }
        }
        fprintf(stderr, "\n");

    }
}
#endif

static LONG CALLBACK veh_handler(PEXCEPTION_POINTERS ep)
{
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        uintptr_t fault_addr = ep->ExceptionRecord->ExceptionInformation[1];

        /*
         * GPU register probe at 0xFD000000 range.
         * Some games probe NV2A registers directly. On real hardware this
         * returns GPU state; here we just skip the instruction.
         * TODO: Implement mini x86-64 decoder for instruction skipping,
         * or connect to the xbox_nv2a library for proper handling.
         */
        if (fault_addr >= 0xFD000000 && fault_addr < 0xFE000000) {
            if (xbox_VehMmioFault(ep))
                return EXCEPTION_CONTINUE_EXECUTION;
            return EXCEPTION_CONTINUE_SEARCH;
        }

        /* Trapped hardware registers (APU, USB OHCI) -> their models.
         *
         * The other half of RECOMP_AC97_READY: reporting the codec ready is
         * what lets DirectSoundCreate past its first gate, and from there it
         * drives the APU directly, so the registers have to fault to be seen.
         * "Handled" means the access was decoded and the instruction stepped
         * over, so execution resumes instead of unwinding. */
        if (xbox_VehMmioFault(ep))
            return EXCEPTION_CONTINUE_EXECUTION;

        fprintf(stderr, "[CRASH] Access violation at RIP=0x%llX, fault addr=0x%llX (%s)\n",
            (unsigned long long)ep->ContextRecord->Rip,
            (unsigned long long)fault_addr,
            ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read");
        fprintf(stderr, "  Xbox regs: eax=0x%08X ecx=0x%08X edx=0x%08X esp=0x%08X\n",
            g_eax, g_ecx, g_edx, g_esp);
        fprintf(stderr, "  Xbox regs: ebx=0x%08X esi=0x%08X edi=0x%08X\n",
            g_ebx, g_esi, g_edi);
        fprintf(stderr, "  Xbox VA of fault: 0x%08X\n",
            (uint32_t)(fault_addr - (uintptr_t)g_xbox_mem_offset));
        print_guest_context((void *)ep->ContextRecord->Rip);

        /*
         * TODO: Add game-specific diagnostics here. Examples:
         *
         * Dump CRT heap handle:
         *   uint32_t heap = *(uint32_t *)((uint8_t *)g_xbox_mem_offset + HEAP_HANDLE_VA);
         *   fprintf(stderr, "  CRT heap handle: 0x%08X\n", heap);
         *
         * Dump game state:
         *   uint32_t state = *(uint32_t *)((uint8_t *)g_xbox_mem_offset + GAME_STATE_VA);
         *   fprintf(stderr, "  Game state: %u\n", state);
         */

        /* Print native stack return addresses for debugging */
        {
            uintptr_t *sp = (uintptr_t *)ep->ContextRecord->Rsp;
            fprintf(stderr, "  Native stack (first 8 return addrs):\n");
            for (int i = 0; i < 64 && sp[i]; i++) {
                if (sp[i] >= 0x140000000ULL && sp[i] < 0x150000000ULL) {
                    fprintf(stderr, "    [%d] 0x%llX\n", i, (unsigned long long)sp[i]);
                }
            }
        }
        fflush(stderr);
    } else if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_INT_DIVIDE_BY_ZERO
            || ep->ExceptionRecord->ExceptionCode == EXCEPTION_INT_OVERFLOW
            || ep->ExceptionRecord->ExceptionCode == EXCEPTION_ILLEGAL_INSTRUCTION) {
        /* A guest idiv by zero faults the same way it would on the console;
         * without this the process vanishes with nothing in the log. */
        fprintf(stderr, "[CRASH] exception 0x%08lX at RIP=0x%llX\n",
            (unsigned long)ep->ExceptionRecord->ExceptionCode,
            (unsigned long long)ep->ContextRecord->Rip);
        fprintf(stderr, "  Xbox regs: eax=0x%08X ecx=0x%08X edx=0x%08X esp=0x%08X\n",
            g_eax, g_ecx, g_edx, g_esp);
        fprintf(stderr, "  Xbox regs: ebx=0x%08X esi=0x%08X edi=0x%08X\n",
            g_ebx, g_esi, g_edi);
        print_guest_context((void *)ep->ContextRecord->Rip);
        fflush(stderr);
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

static DWORD WINAPI log_flush_thread(LPVOID unused)
{
    (void)unused;
    for (;;) {
        Sleep(250);
        fflush(stderr);
    }
}

/* The game mode, NF_MODE: "original" (the default) plays the game as it is
 * on the console, "enhanced" adds what the original does not have (mouse and
 * keyboard as in the PC version, ...). Every enhancement asks nf_enhanced()
 * first and, in the original mode, calls the title's own code and nothing
 * else; the toolkit's own extras stay off (RECOMP_MKB is cleared). Fixes
 * that make the recomp play like the console are not enhancements and run
 * in both modes. The variable is inherited by nightfire_driving.exe at the
 * handoff, so both images play in the same mode. */
static int s_nf_enhanced = -1;

int nf_enhanced(void)
{
    if (s_nf_enhanced < 0) {
        const char *m = getenv("NF_MODE");
        s_nf_enhanced = m && _stricmp(m, "enhanced") == 0;
    }
    return s_nf_enhanced;
}

/* A [Graphics] setting of nightfire.ini, or the [Grafik] one the file had
 * before it was in English. */
static int nf_ini_int(const char *path, const char *key, const char *old, int def)
{
    const int none = -0x7FFF;
    int v = (int)GetPrivateProfileIntA("Graphics", key, none, path);
    return v != none ? v : (int)GetPrivateProfileIntA("Grafik", old, def, path);
}

static void nf_default(const char *name, const char *value)
{
    char e[MAX_PATH + 64];
    if (!getenv(name)) {
        snprintf(e, sizeof e, "%s=%s", name, value);
        _putenv(e);
    }
}

/* The saves (UDATA, with TDATA and the title's cache) in Documents\My Games\
 * Nightfire Recomp, where a player looks for them, instead of the toolkit's
 * %LOCALAPPDATA%\xboxrecomp (whose partition images stay there). Saves made
 * there before are not taken over.
 *
 * The toolkit takes RECOMP_SAVE_DIR as UTF-8 from
 * a narrow environment that nightfire_driving.exe inherits, so a path with
 * characters outside ASCII goes by its 8.3 name; without one the saves stay
 * where they were rather than in a folder the driving part might not find. */
static void nf_save_dir(void)
{
    wchar_t docs[MAX_PATH], dir[MAX_PATH];
    char utf8[MAX_PATH * 3];
    const wchar_t *p;

    if (getenv("RECOMP_SAVE_DIR"))
        return;
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_PERSONAL | CSIDL_FLAG_CREATE, NULL, 0, docs)))
        return;
    swprintf(dir, MAX_PATH, L"%s\\My Games", docs);
    CreateDirectoryW(dir, NULL);
    swprintf(dir, MAX_PATH, L"%s\\My Games\\Nightfire Recomp", docs);
    CreateDirectoryW(dir, NULL);
    if (GetFileAttributesW(dir) == INVALID_FILE_ATTRIBUTES)
        return;

    for (p = dir; *p && *p < 0x80; p++)
        ;
    if (*p) {
        wchar_t sh[MAX_PATH];
        DWORD n = GetShortPathNameW(dir, sh, MAX_PATH);
        if (n == 0 || n >= MAX_PATH)
            return;
        for (p = sh; *p && *p < 0x80; p++)
            ;
        if (*p)
            return;
        wcscpy(dir, sh);
    }

    strcpy(utf8, "RECOMP_SAVE_DIR=");
    if (WideCharToMultiByte(CP_UTF8, 0, dir, -1, utf8 + strlen(utf8),
                            (int)(sizeof utf8 - strlen(utf8)), NULL, NULL) > 0)
        _putenv(utf8);
}

/* Everything a start needs, so the exe plays from a double click or from the
 * launcher alike. Values already in the environment win (startPerf.bat, a
 * debugging session), and nightfire_driving.exe inherits them all at the
 * handoff, with the working directory and the log.
 *
 * The folder: the one holding game\, the working directory from here on, so
 * mkb.ini, nightfire.ini, assets\ and logs\ are found beside it.
 *
 * The mode: "original" or "enhanced" on the command line, else NF_MODE, else
 * nightfire.ini [Launcher] Mode, else original.
 *
 * The log: unless the caller redirected it (RECOMP_STDERR names the file),
 * stderr goes to logs\stderr_last.txt; the one before becomes
 * stderr_prev.txt, and one that ends in a crash is kept in logs\crashes\
 * under the time it was written. */
static void nf_start_setup(const char *cmdline)
{
    char root[MAX_PATH], v[MAX_PATH + 32], *slash;

    if (!getenv("RECOMP_GAME_DIR") && nf_find_game_dir() && _fullpath(root, g_game_dir, sizeof root)
            && (slash = strrchr(root, '\\')) != NULL) {
        *slash = '\0';
        SetCurrentDirectoryA(root);
        snprintf(g_game_dir, sizeof g_game_dir, "game");
    }

    if (!getenv("NF_MODE")) {
        char mode[32] = "";
        if (cmdline && strstr(cmdline, "enhanced"))
            strcpy(mode, "enhanced");
        else if (cmdline && strstr(cmdline, "original"))
            strcpy(mode, "original");
        else if (_fullpath(v, getenv("NF_INI") ? getenv("NF_INI") : "nightfire.ini", MAX_PATH))
            GetPrivateProfileStringA("Launcher", "Mode", "original", mode, sizeof mode, v);
        snprintf(v, sizeof v, "NF_MODE=%s", mode[0] ? mode : "original");
        _putenv(v);
    }
    if (_fullpath(v, "nightfire.ini", MAX_PATH))
        nf_default("NF_INI", v);
    if (_fullpath(v, "mkb.ini", MAX_PATH))
        nf_default("RECOMP_MKB_INI", v);

    /* The console the disc expects (nf_disc_is_pal): on a PAL one the main
     * game takes eurocom\25_fps and the driving image pal\eng (videos, menu
     * texts), on an NTSC one eurocom\30_fps and ntsc\eng, and the vblanks
     * come at 50 or 60 Hz to match (the toolkit's vblank_hz). */
    nf_default("RECOMP_CONSOLE_REGION", nf_disc_is_pal() ? "pal" : "ntsc");
    nf_default("RECOMP_AC97_READY", "1");
    /* A hang (60 s without progress) is reported with a stack in the log. */
    nf_default("RECOMP_WATCHDOG_SECS", "60");
    nf_default("RECOMP_PB_EXEC", "1");
    nf_default("RECOMP_TITLE_KEVENTS", "1");
    nf_default("RECOMP_PB_REPORT_MS", "10000");
    nf_default("RECOMP_FB_WINDOW", "1");
    nf_default("RECOMP_D3D_PRESENT", "1");
    nf_default("RECOMP_USB", "1");
    /* both modes: four controller ports as on the console, for split-screen;
     * each host pad takes the next free one (xboxrecomp xinput_device.c). */
    nf_default("RECOMP_USB_PADS", "4");
    nf_default("RECOMP_KEYBOARD", "1");
    nf_default("RECOMP_AUDIO_BUF_SAMPLES", "2048");
    nf_default("RECOMP_AUDIO_BUF_COUNT", "8");
    nf_default("RECOMP_MISSING_FILES", "1");
    /* As on the console's one core: DPCs never run beside code that raised
     * the IRQL. Without it the DirectSound DPC and the game thread went
     * through the same voice lists at once (null calls in DSOUND, crashes in
     * The Exchange and in Driving). DPCs stay on the timer thread: on the
     * game thread they cost the main game 3-4 fps. */
    nf_default("RECOMP_DISPATCH_GATE", "1");
    nf_default("RECOMP_DPC_ON_RAISE", "0");
    nf_save_dir();

    if (!getenv("RECOMP_STDERR")) {
        const char *last = "logs\\stderr_last.txt";
        FILE *f;
        CreateDirectoryA("logs", NULL);
        if ((f = fopen(last, "rb")) != NULL) {
            static char buf[1 << 16];
            int crashed = 0;
            size_t n;
            while (!crashed && (n = fread(buf, 1, sizeof buf - 1, f)) > 0) {
                buf[n] = '\0';
                crashed = strstr(buf, "[CRASH]") != NULL;
                if (n < sizeof buf - 1)
                    break;
                fseek(f, -8, SEEK_CUR);         /* a marker across the cut */
            }
            fclose(f);
            if (crashed) {
                WIN32_FILE_ATTRIBUTE_DATA a;
                SYSTEMTIME t;
                FILETIME lt;
                if (GetFileAttributesExA(last, GetFileExInfoStandard, &a)
                        && FileTimeToLocalFileTime(&a.ftLastWriteTime, &lt)
                        && FileTimeToSystemTime(&lt, &t)) {
                    CreateDirectoryA("logs\\crashes", NULL);
                    snprintf(v, sizeof v, "logs\\crashes\\%04u%02u%02u_%02u%02u%02u_stderr.txt",
                             t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
                    CopyFileA(last, v, FALSE);
                }
            }
            MoveFileExA(last, "logs\\stderr_prev.txt", MOVEFILE_REPLACE_EXISTING);
        }
        if (freopen(last, "w", stderr)) {
            /* The handle the driving image inherits at the handoff. */
            HANDLE h = (HANDLE)_get_osfhandle(_fileno(stderr));
            if (h != INVALID_HANDLE_VALUE) {
                SetHandleInformation(h, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
                SetStdHandle(STD_ERROR_HANDLE, h);
            }
            if (_fullpath(v, last, MAX_PATH))
                nf_default("RECOMP_STDERR", v);
        }
    }
}

/* ── WinMain ───────────────────────────────────────────────── */

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nCmdShow)
{
    void *xbe_data = NULL;
    size_t xbe_size = 0;

    (void)hInstance;
    (void)hPrevInstance;
    (void)nCmdShow;

    nf_start_setup(lpCmdLine);

    /* stdout unbuffered; stderr, the log, buffered and written out four times
     * a second (log_flush_thread). Unbuffered, every log line was a write of
     * its own, 3-5 ms each to the log file here, made under the stream lock
     * every thread shares: the ten-second report held the GPU thread ~250 ms
     * and the kernel summary held the game ~50 ms every two seconds -- the
     * hitches in the driving missions. The crash handler and the exit paths
     * flush. RECOMP_STDERR_UNBUFFERED=1 restores line-at-a-time output. */
    setvbuf(stdout, NULL, _IONBF, 0);
    if (getenv("RECOMP_STDERR_UNBUFFERED")) {
        setvbuf(stderr, NULL, _IONBF, 0);
    } else {
        static char errbuf[1 << 20];
        setvbuf(stderr, errbuf, _IOFBF, sizeof errbuf);
        CloseHandle(CreateThread(NULL, 0, log_flush_thread, NULL, 0, NULL));
    }

    /* The game mode (nf_enhanced above). The toolkit's mouse and keyboard
     * layer runs in the enhanced mode only, whatever the caller set. The
     * original mode is played with a pad, as the console is: no key reaches
     * the game (RECOMP_KEYBOARD, the keys as a pad, is cleared too) and the
     * pointer is hidden over the window. The window's own keys -- F11 and
     * Alt+Enter for fullscreen, Alt+F4 -- stay.
     *
     * The enhanced mode's settings, which a launcher edits, are in
     * nightfire.ini (NF_INI, else in the working directory): [Graphics]
     * Resolution = <w>x<h> draws the picture at that size, n (1-8) at n
     * times the console's 640x480 (RECOMP_GPUBE_SIZE, RECOMP_GPUBE_SCALE,
     * src/nv2a_d3d11_backend.c); Widescreen = 1 sets the
     * console's widescreen setting, for which both images draw a 16:9
     * picture into their 640x480 as for a widescreen TV (the main game's
     * cameras, sub_00025190; the driving image's display mode, sub_0007CFB0),
     * and shows it at 16:9; Anisotropy = n filters textures anisotropically
     * (RECOMP_GPUBE_ANISO); FlickerFilter = 1 keeps the console's vertical
     * softening, which the enhanced mode otherwise leaves off; VSync = 0
     * shows each frame at once, at the cost of tearing
     * (RECOMP_PRESENT_VSYNC). Files from before the settings were in English
     * have [Grafik] Aufloesung, Breitbild, Anisotropie and Flimmerfilter,
     * read where the English key is missing. The original mode draws at the
     * console's size for a 4:3 TV. */
    if (nf_enhanced()) {
        const char *ini = getenv("NF_INI");
        char path[MAX_PATH], v[32];
        int have = _fullpath(path, ini && *ini ? ini : "nightfire.ini", sizeof path) != NULL;
        if (!getenv("RECOMP_MKB"))
            _putenv("RECOMP_MKB=1");
        /* The menus' mouse pointer, assets\cursor.cur (made from cursor.png). */
        if (!getenv("RECOMP_MKB_CURSOR")) {
            char cur[MAX_PATH], e[MAX_PATH + 24];
            if (_fullpath(cur, "assets\\cursor.cur", sizeof cur)
                    && GetFileAttributesA(cur) != INVALID_FILE_ATTRIBUTES) {
                snprintf(e, sizeof e, "RECOMP_MKB_CURSOR=%s", cur);
                _putenv(e);
            }
        }
        /* Resolution = <w>x<h>, the size the picture is drawn at (the
         * launcher's list of monitor sizes), or n (1-8), the older steps of
         * n times 640x480. */
        if (!getenv("RECOMP_GPUBE_SCALE") && !getenv("RECOMP_GPUBE_SIZE")) {
            char r[32] = "";
            unsigned rw = 0, rh = 0;
            if (have) {
                GetPrivateProfileStringA("Graphics", "Resolution", "", r, sizeof r, path);
                if (!r[0])
                    GetPrivateProfileStringA("Grafik", "Aufloesung", "", r, sizeof r, path);
            }
            if (sscanf(r, "%u x %u", &rw, &rh) == 2 && rw >= 320 && rh >= 240
                    && rw <= 7680 && rh <= 4320) {
                snprintf(v, sizeof v, "RECOMP_GPUBE_SIZE=%ux%u", rw, rh);
            } else {
                int sc = atoi(r);
                if (sc < 1 || sc > 8)
                    sc = 1;
                snprintf(v, sizeof v, "RECOMP_GPUBE_SCALE=%d", sc);
            }
            _putenv(v);
        }
        /* DrivingAA = 1 keeps the driving missions' anti-aliasing (two
         * samples a pixel, averaged); 0, the default, draws them at the
         * shown size instead (RECOMP_GPUBE_NO_AA, src/nv2a_d3d11_backend.c). */
        if (!getenv("RECOMP_GPUBE_NO_AA")
                && !(have && nf_ini_int(path, "DrivingAA", "DrivingAA", 0)))
            _putenv("RECOMP_GPUBE_NO_AA=1");
        if (!getenv("RECOMP_GPUBE_ANISO")) {
            int an = have ? (int)nf_ini_int(path, "Anisotropy", "Anisotropie", 1) : 1;
            snprintf(v, sizeof v, "RECOMP_GPUBE_ANISO=%d", an);
            _putenv(v);
        }
        if (!getenv("RECOMP_FLICKER_FILTER")) {
            int fl = have ? (int)nf_ini_int(path, "FlickerFilter", "Flimmerfilter", 0) : 0;
            _putenv(fl ? "RECOMP_FLICKER_FILTER=1" : "RECOMP_FLICKER_FILTER=0");
        }
        if (!getenv("RECOMP_PRESENT_VSYNC") && have
                && !nf_ini_int(path, "VSync", "VSync", 1))
            _putenv("RECOMP_PRESENT_VSYNC=0");
        /* Widescreen = 1 is 16:9, 2 ultrawide (21:9): the game's own 16:9
         * mode, widened to the shape of Resolution (3440x1440 is 2.389:1;
         * 21:9 without a size) by the main game's cameras (manual_main.c,
         * sub_00025190) and the driving image's aspect (below, at boot). */
        if (!getenv("RECOMP_VIDEO_FLAGS") && have) {
            int ws = nf_ini_int(path, "Widescreen", "Breitbild", 0);
            if (ws) {
                _putenv("RECOMP_VIDEO_FLAGS=0x10000");   /* XC_VIDEO widescreen */
                if (!getenv("RECOMP_DISPLAY_ASPECT")) {
                    char r[32] = "";
                    unsigned rw = 0, rh = 0;
                    GetPrivateProfileStringA("Graphics", "Resolution", "", r, sizeof r, path);
                    if (ws == 2 && sscanf(r, "%u x %u", &rw, &rh) == 2 && rh
                            && (float)rw / (float)rh > 2.0f)
                        snprintf(v, sizeof v, "RECOMP_DISPLAY_ASPECT=%u:%u", rw, rh);
                    else
                        snprintf(v, sizeof v, "RECOMP_DISPLAY_ASPECT=%s", ws == 2 ? "21:9" : "16:9");
                    _putenv(v);
                }
            }
        }
    } else {
        _putenv("RECOMP_MKB=");
        _putenv("RECOMP_KEYBOARD=");
        _putenv("RECOMP_HIDE_CURSOR=1");
        _putenv("RECOMP_GPUBE_SCALE=");
        _putenv("RECOMP_GPUBE_SIZE=");
        _putenv("RECOMP_GPUBE_NO_AA=");
        _putenv("RECOMP_GPUBE_ANISO=");
        _putenv("RECOMP_VIDEO_FLAGS=");
        _putenv("RECOMP_DISPLAY_ASPECT=");
    }
    printf("Mode: %s\n", nf_enhanced() ? "enhanced" : "original");

    /* The console's pace, for both images: vblanks on the display clock (50 Hz
     * for the PAL disc, see vblank_hz in the toolkit; RECOMP_VBLANK_HZ=60
     * changes it) and each flip held until its vblank, as the flip waits for
     * one on the console. Without them the main game saw no vblanks at all and
     * rendered as fast as the host allowed: over 60 fps on the CPU
     * rasteriser, over 1000 on the D3D11 back end. Values set by the caller
     * win. */
    if (!getenv("RECOMP_VBLANK"))
        _putenv("RECOMP_VBLANK=paced");
    if (!getenv("RECOMP_FLIP_PACING"))
        _putenv("RECOMP_FLIP_PACING=1");
    /* Read the pushbuffer the way the DMA engine does (JUMP, CALL, RETURN)
     * instead of PUT-to-PUT segments with a guessed ring end. The segment scan
     * lost commands at every wrap of the ring, several times a second: text
     * lines broken off mid-batch in the menus and stray, screen-covering
     * geometry in the driving missions, one frame at a time, on both render
     * paths. Measured in the main menu: 6 single-frame glitches in 300 frames
     * with the segment scan, none in 900 with the walk. */
    if (!getenv("RECOMP_PB_WALK"))
        _putenv("RECOMP_PB_WALK=dma");

#ifdef NIGHTFIRE_DRIVING
    /* Driving's D3D waits on the vblank event (device+0x2554) while it sets
     * the video mode, and the runtime raises no vblanks unless asked to. A
     * value set by the caller still wins. */
    if (!getenv("RECOMP_VBLANK")) {
        _putenv("RECOMP_VBLANK=paced");
        /* So the handoff back to default.xbe can take it away again. */
        _putenv("NF_DRIVING_SET_VBLANK=1");
    }
    /* A driving mission streams the track: D3D frees vertex buffers and
     * textures as it goes and allocates new ones. Without reclaim the runtime
     * never reuses freed contiguous memory, the 64 MB window runs out about
     * a minute and a half into the mission, D3D gets NULL and the title
     * faults. Freed memory being free again is what the console does. */
    if (!getenv("RECOMP_HEAP_RECLAIM")) {
        _putenv("RECOMP_HEAP_RECLAIM=1");
        _putenv("NF_DRIVING_SET_RECLAIM=1");
    }
#endif

    printf("=== 007: Nightfire - Static Recompilation ===\n");
    printf("Loading XBE...\n");

    /* Install VEH handler (first handler in chain) */
    /* Load symbols up front rather than from inside the handler: at fault
     * time the process is already in a bad way, and SymInitialize
     * allocates. Failure is not fatal -- the handler prints no name. */
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    SymInitialize(GetCurrentProcess(), NULL, TRUE);
    AddVectoredExceptionHandler(1, veh_handler);

    /* Step 1: Load XBE */
    {
        char xbe_path[MAX_PATH];
        if (!nf_find_game_dir()) {
            MessageBoxA(NULL, "Game files not found (" YOUR_GAME_XBE_NAME ").\n\n"
                        "Put your extracted Nightfire disc in a 'game' folder next to "
                        "the executable or the repository, or set RECOMP_GAME_DIR.",
                        "Nightfire", MB_ICONERROR);
            return 1;
        }
        snprintf(xbe_path, sizeof xbe_path, "%s\\" YOUR_GAME_XBE_NAME, g_game_dir);
        fprintf(stderr, "[BOOT] game files: %s\n", g_game_dir);
        if (!load_xbe(xbe_path, &xbe_data, &xbe_size)) {
            MessageBoxA(NULL, "Failed to load " YOUR_GAME_XBE_NAME ".",
                        "Nightfire", MB_ICONERROR);
            return 1;
        }
    }
    printf("XBE loaded: %zu bytes\n", xbe_size);

    /* Step 2: Initialize Xbox memory layout */
    printf("Initializing Xbox memory layout...\n");
    if (!xbox_MemoryLayoutInit(xbe_data, xbe_size)) {
        MessageBoxA(NULL, "Failed to initialize Xbox memory layout.\n"
                    "The required virtual address range may be unavailable.",
                    "Recomp", MB_ICONERROR);
        free(xbe_data);
        return 1;
    }

    g_xbox_mem_offset = xbox_GetMemoryOffset();
    printf("Xbox memory mapped. Offset: 0x%llX\n", (unsigned long long)g_xbox_mem_offset);

    xbox_OhciInit();

    if (getenv("RECOMP_FB_WINDOW"))
        nv2a_hook_init(g_xbox_mem_offset);

    /* Bring up the emulated APU.
     *
     * Gated on the same variable that unmaps its registers, because the two
     * halves are useless apart: a trap with no model declines every access,
     * and a model nothing traps into never sees a register. The APU walks
     * Xbox physical RAM to find voice buffers, so it gets the guest RAM
     * base. */
    if (getenv("RECOMP_AC97_READY")) {
        g_apu_state = mcpx_apu_init_standalone((uint8_t *)xbox_GetMemoryBase());
        fprintf(stderr, "[BOOT] emulated APU %s\n",
                g_apu_state ? "up" : "FAILED to initialise");
    }

    /* Step 3: Initialize Xbox kernel */
    printf("Initializing Xbox kernel replacement...\n");
    xbox_kernel_init();

    /* Step 4: Set game directory for file I/O path translation */
    {
        extern void xbox_path_init(const char *game_dir, const char *save_dir);
        xbox_path_init(g_game_dir, NULL);
    }

    /* Step 5: Initialize kernel bridge (thunk table in Xbox memory) */
    printf("Initializing kernel bridge...\n");
    xbox_kernel_bridge_init();

    /* Step 6: Initialize stack */
    g_esp = XBOX_STACK_TOP;

    /*
     * TODO: Pre-initialize CRT globals if needed.
     *
     * Many Xbox games use the MSVC CRT. The CRT's __heap_init sets up a
     * heap descriptor at a game-specific address. You may need to
     * pre-initialize __active_heap to avoid small-block heap issues:
     *
     *   uint32_t *active_heap = (uint32_t *)((uint8_t *)g_xbox_mem_offset + ACTIVE_HEAP_VA);
     *   *active_heap = 1;  // 1 = system heap (HeapAlloc), avoids SBH init
     *
     * Find ACTIVE_HEAP_VA by searching the disassembly for __heap_init
     * or by looking for the CRT's __active_heap global in the data section.
     */

    printf("\n=== Initialization complete ===\n");
    printf("Entry point: 0x%08X\n", YOUR_GAME_ENTRY_POINT);
    printf("ESP: 0x%08X\n", g_esp);

    /* Build the flat dispatch table before the guest runs.
     *
     * Without this recomp_lookup falls back to a binary search over the
     * whole function table -- roughly log2(n) branches on *every*
     * indirect call, which for a 45,000-function C++ title is about 16
     * every time the game goes through a vtable. Half-Life 2 spent most
     * of its static initialisation inside recomp_lookup for exactly this
     * reason, and it read as a hang.
     *
     * Optional by design: if the allocation fails the search still works,
     * so a failure is worth one line and not a fatal error. */
    if (!recomp_dispatch_init())
        fprintf(stderr, "[BOOT] flat dispatch unavailable; "
                        "indirect calls will use the binary search\n");

    /* Arm the hang watchdog. Does nothing unless RECOMP_WATCHDOG_SECS is set,
     * and must be called from this thread -- the guest registers it samples are
     * thread-local, so it has to be handed the copies belonging to the thread
     * that runs guest code.
     *
     * Not optional boilerplate: without this call RECOMP_WATCHDOG_SECS is
     * silently inert, and the one diagnostic that tells a hang from slowness
     * does nothing while appearing to be set. */
    xbox_WatchdogStart();

    /* Draw on the host GPU (src/nv2a_d3d11_backend.c) by default; the frame
     * comparison (startCompare.bat) puts it within edge pixels of the
     * executor's CPU rasteriser in the menus, the main game's levels and the
     * driving missions, at three to four times the speed -- the main game's
     * first level runs at its 25 fps with it and at 7 without. Set
     * RECOMP_GPU_BACKEND=0 for the CPU rasteriser. */
    {
        extern void nightfire_gpu_backend_register(void);
        const char *gb = getenv("RECOMP_GPU_BACKEND");
        if (!gb || !*gb || *gb != '0')
            nightfire_gpu_backend_register();
    }

    CloseHandle(CreateThread(NULL, 0, d3d_semaphore_thread, NULL, 0, NULL));

#ifdef NIGHTFIRE_DRIVING
    /* Driving plays its own mission intro/outro movies (NTSC\eng\*.mad,
     * gated by the byte at 0x1B7057, which is 1 in the image and never
     * written). The European disc ships no NTSC directory -- its driving
     * movies are the 0710xxxx/0730xxxx XMVs, which only default.xbe can
     * play -- and with the file missing the stream's pending count never
     * drops, so the mission sits on its loading screen for good. Turn the
     * driving image's own movies off unless the movies are there (e.g.
     * copied from the US disc into game\NTSC). */
    /* With RECOMP_CONSOLE_REGION=pal the image asks for pal\eng\*.mad, which
     * the European disc does carry (inside driving\misc.viv), so the movies
     * stay on there, as on a European console. The North-American disc
     * carries its ntsc\eng\*.mad the same way, so they stay on there too. */
    if (!(getenv("RECOMP_CONSOLE_REGION") && (getenv("RECOMP_CONSOLE_REGION")[0] | 0x20) == 'p')) {
        char ntsc[MAX_PATH];
        snprintf(ntsc, sizeof ntsc, "%s\\NTSC", g_game_dir);
        if (GetFileAttributesA(ntsc) == INVALID_FILE_ATTRIBUTES
                && !nf_viv_has("driving\\misc.viv", "ntsc\\eng\\")) {
            *((uint8_t *)g_xbox_mem_offset + 0x1B7057) = 0;
            fprintf(stderr, "[BOOT] no %s: driving movies off\n", ntsc);
        }
    }
    /* Ultrawide (RECOMP_DISPLAY_ASPECT wider than 16:9, enhanced only): the
     * driving image takes its widescreen picture's shape from the float at
     * 0x19245C (16/9 in .rdata, read by sub_00096960 and sub_00096EB0 when
     * its display mode is widescreen); that shape instead. */
    if (nf_enhanced()) {
        const char *a = getenv("RECOMP_DISPLAY_ASPECT");
        float ax = 0.0f, ay = 0.0f;
        if (a && sscanf(a, "%f:%f", &ax, &ay) == 2 && ax > 0.0f && ay > 0.0f
                && ax / ay > 16.0f / 9.0f + 0.01f) {
            float as = ax / ay;
            memcpy((uint8_t *)g_xbox_mem_offset + 0x19245C, &as, 4);
            fprintf(stderr, "[BOOT] driving picture at %.3f:1\n", as);
        }
    }
    {
        const char *s = getenv("RECOMP_STACK_SAMPLE_SECS");
        int secs = s ? atoi(s) : 0;
        if (secs > 0) {
            s_sample_esp = &g_esp;   /* this thread's copy: g_esp is TLS */
            CloseHandle(CreateThread(NULL, 0, driving_stack_sampler,
                                     (LPVOID)(uintptr_t)secs, 0, NULL));
        }
    }
#endif

    /* Step 7: Call the recompiled entry point */
    printf("\nStarting game...\n");
    fflush(stdout);

    xbe_entry_point();

    printf("\nGame returned. Cleaning up...\n");

    /* Cleanup */
    xbox_kernel_shutdown();
    xbox_MemoryLayoutShutdown();
    free(xbe_data);

    return 0;
}

/* ── XBE Loading ───────────────────────────────────────────── */

static BOOL load_xbe(const char *path, void **out_data, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Cannot open XBE: %s\n", path);
        return FALSE;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0) {
        fclose(f);
        return FALSE;
    }

    void *data = malloc((size_t)size);
    if (!data) {
        fclose(f);
        return FALSE;
    }

    if (fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data);
        fclose(f);
        return FALSE;
    }

    fclose(f);
    *out_data = data;
    *out_size = (size_t)size;
    return TRUE;
}

/* Console entry point (for debugging -- lets you see printf output) */
int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return WinMain(GetModuleHandle(NULL), NULL, GetCommandLineA(), SW_SHOW);
}
