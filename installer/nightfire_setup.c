/* Nightfire installer: puts the project into a folder of the player's
 * choosing, copies their own game files beside it and builds the game there.
 *
 * It carries the project and the toolkit (made by installer\build.ps1 from the
 * committed state) and the build tools -- Zig, the embeddable Python with
 * capstone, CMake, Ninja, with their licences -- as three zips in its
 * resources, and nothing of the game. The player points it at the default.xbe
 * of their extracted disc; the folder holding it is copied to <install>\game,
 * the tools go to <install>\toolchain (kept: the launcher's updates build with
 * them too), and the install then runs the build:
 * scripts\recompile.ps1 (the XBEs to C) and scripts\build.ps1 (the exes and
 * "Nightfire Launcher.exe"), which use toolchain\ when it is there. Nothing
 * else needs to be on the PC. Everything the scripts print goes to
 * <install>\logs\install.log.
 *
 * Installing again into the same folder keeps nightfire.ini, mkb.ini, the
 * saves and the build, so it only rebuilds what changed.
 *
 * Command line (for testing): /xbe <path> /dir <path> picks both, /go starts
 * at once, /test leaves out the desktop shortcut and the registry entry; both
 * together close the window at the end, with 0 (done), 1 (failed) or 2
 * (cancelled) as the exit code. */
#define UNICODE
#define _UNICODE
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <tlhelp32.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <math.h>

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' " \
                        "version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

/* The resources setup.rc.in embeds (keep the numbers in step). */
#define IDR_PROJECT  101
#define IDR_TOOLKIT  102
#define IDR_MANIFEST 103
#define IDR_TOOLCHAIN 104

#define NF_TITLE_ID  0x45410026u     /* both discs' certificates */
#define BUILD_BYTES  (1600ull << 20) /* toolchain\, build\, src\recomp, the pipeline */
#define REG_KEY      L"Software\\Nightfire Recomp"

#define WM_SETUP_DONE (WM_APP + 1)  /* wParam: result */

enum { ID_XBE = 100, ID_XBE_BROWSE, ID_DEST, ID_DEST_BROWSE, ID_SHORTCUT, ID_GO, ID_TIMER,
       ID_L_XBE, ID_L_DEST, ID_NOTE, ID_SPACE, ID_STEP, ID_DETAIL };
enum { S_FORM, S_RUN, S_DONE, S_FAIL };
enum { R_OK, R_FAIL, R_CANCEL };

/* Layout, in pixels at 96 dpi. */
#define WIN_W     420
#define BANNER_X  16
#define BANNER_Y  12
#define BANNER_W  388
#define BANNER_H  140
#define PANEL_X   16
#define PANEL_Y   166
#define PANEL_W   388
#define PANEL_H   274

static const COLORREF k_panel = RGB(251, 251, 251), k_border = RGB(214, 214, 214),
                       k_accent = RGB(47, 85, 151), k_grey = RGB(110, 110, 110), k_red = RGB(190, 40, 30);

static HINSTANCE s_inst;
static HWND s_win, s_xbe, s_dest, s_shortcut, s_go, s_space, s_note, s_step_w, s_detail_w, s_bar, s_status;
static HWND s_form_ctl[12];             /* shown only with the form, NULL-ended */
static HFONT s_font, s_title_f, s_note_f, s_btn_f;
static HBRUSH s_panel_br;
static HBITMAP s_banner;
static int s_dpi = 96, s_state = S_FORM, s_installed, s_test, s_auto, s_exit;
static wchar_t s_title[128], s_sub[512];

/* The chosen game files. */
static struct {
    wchar_t dir[MAX_PATH];
    int ok;
    DWORD region;
    ULONGLONG bytes;
} s_game;

/* The install, shared between the worker and the window. */
static CRITICAL_SECTION s_cs;
static double s_lo, s_hi, s_frac;
static DWORD s_seg_t0, s_seg_ms, s_t0, s_took;
static wchar_t s_step[128], s_detail[256];
static wchar_t s_dir[MAX_PATH], s_fail_msg[256], s_fail_adv[512];
static volatile LONG s_cancel;
static BOOL s_cancel_copy;
static HANDLE s_job, s_log = INVALID_HANDLE_VALUE;
static int s_want_shortcut;
static int s_compiled, s_compile_total;
static char s_first_error[300];

static int px(int v)
{
    return MulDiv(v, s_dpi, 96);
}

static int exists(const wchar_t *path)
{
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

static int is_dir(const wchar_t *path)
{
    DWORD a = GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static void path_join(wchar_t *out, const wchar_t *dir, const wchar_t *name)
{
    swprintf(out, MAX_PATH, L"%s\\%s", dir, name);
}

/* A full path without a trailing backslash (a drive root keeps its own). */
static void path_full(const wchar_t *in, wchar_t *out)
{
    size_t n;
    if (!GetFullPathNameW(in, MAX_PATH, out, NULL))
        wcscpy(out, in);
    n = wcslen(out);
    while (n > 3 && out[n - 1] == L'\\')
        out[--n] = 0;
}

static void gb(ULONGLONG bytes, wchar_t *out, size_t cap)
{
    swprintf(out, cap, L"%.1f GB", bytes / 1073741824.0);
}

/* ---- Log -------------------------------------------------------------- */

static void log_raw(const char *s, DWORD n)
{
    DWORD w;
    if (s_log != INVALID_HANDLE_VALUE)
        WriteFile(s_log, s, n, &w, NULL);
}

static void logw(const wchar_t *fmt, ...)
{
    wchar_t w[2048];
    char u[6144];
    SYSTEMTIME t;
    va_list ap;
    int n;

    GetLocalTime(&t);
    n = swprintf(w, 32, L"[%02u:%02u:%02u] ", t.wHour, t.wMinute, t.wSecond);
    va_start(ap, fmt);
    vswprintf(w + n, 2048 - n - 2, fmt, ap);
    va_end(ap);
    wcscat(w, L"\r\n");
    n = WideCharToMultiByte(CP_UTF8, 0, w, -1, u, sizeof u, NULL, NULL);
    if (n > 1)
        log_raw(u, (DWORD)n - 1);
}

/* ---- Progress --------------------------------------------------------- */

/* A stage of the install: lo..hi of the bar, the time it is expected to take
 * (the bar creeps towards hi meanwhile), and what the player is told. */
static void seg(double lo, double hi, DWORD ms, const wchar_t *step, const wchar_t *detail)
{
    EnterCriticalSection(&s_cs);
    s_lo = lo;
    s_hi = hi;
    s_frac = -1;
    s_seg_t0 = GetTickCount();
    s_seg_ms = ms ? ms : 1;
    if (step)
        wcsncpy(s_step, step, 127);
    wcsncpy(s_detail, detail ? detail : L"", 255);
    LeaveCriticalSection(&s_cs);
    if (step)
        logw(L"== %s ==", step);
}

/* The stage's real progress, 0..1, where it is known. */
static void seg_frac(double f)
{
    EnterCriticalSection(&s_cs);
    s_frac = f < 0 ? 0 : f > 1 ? 1 : f;
    LeaveCriticalSection(&s_cs);
}

static void detail(const wchar_t *fmt, ...)
{
    va_list ap;
    EnterCriticalSection(&s_cs);
    va_start(ap, fmt);
    vswprintf(s_detail, 256, fmt, ap);
    va_end(ap);
    LeaveCriticalSection(&s_cs);
}

static double progress(void)
{
    double f;
    EnterCriticalSection(&s_cs);
    f = s_frac >= 0 ? s_frac
                    : 0.95 * (1.0 - exp(-(double)(GetTickCount() - s_seg_t0) / s_seg_ms));
    f = s_lo + (s_hi - s_lo) * f;
    LeaveCriticalSection(&s_cs);
    return f;
}

/* ---- Running tools ---------------------------------------------------- */

typedef void (*LineFn)(const char *line);

/* Runs cmd in cwd, its output into the log and, line by line, to fn; with out,
 * the output is also collected there. Inside the job object, so Cancel ends
 * it and everything it started. Returns the exit code, -1 if it did not
 * start. */
static int run(const wchar_t *cmd, const wchar_t *cwd, LineFn fn, char *out, size_t cap)
{
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    HANDLE rd, wr, nul;
    wchar_t *buf;
    char chunk[4096], line[1024];
    size_t ll = 0, on = 0;
    DWORD got, code = (DWORD)-1;

    if (out && cap)
        out[0] = 0;
    if (!CreatePipe(&rd, &wr, &sa, 0))
        return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = wr;
    si.hStdError = wr;
    buf = _wcsdup(cmd);
    if (!buf || !CreateProcessW(NULL, buf, NULL, NULL, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
                                NULL, cwd, &si, &pi)) {
        free(buf);
        CloseHandle(rd);
        CloseHandle(wr);
        if (nul != INVALID_HANDLE_VALUE)
            CloseHandle(nul);
        return -1;
    }
    if (s_job)
        AssignProcessToJobObject(s_job, pi.hProcess);
    ResumeThread(pi.hThread);
    free(buf);
    CloseHandle(wr);
    if (nul != INVALID_HANDLE_VALUE)
        CloseHandle(nul);
    for (;;) {
        DWORD i;
        int last = !ReadFile(rd, chunk, sizeof chunk, &got, NULL) || !got;
        if (last) {
            chunk[0] = '\n';
            got = 1;
        } else {
            log_raw(chunk, got);
        }
        for (i = 0; i < got; i++) {
            char c = chunk[i];
            if (c == '\r' || c == '\n') {
                if (ll) {
                    line[ll] = 0;
                    if (fn)
                        fn(line);
                    if (out && on + ll + 2 < cap) {
                        memcpy(out + on, line, ll);
                        on += ll;
                        out[on++] = '\n';
                        out[on] = 0;
                    }
                    ll = 0;
                }
            } else if (ll + 1 < sizeof line) {
                line[ll++] = c;
            }
        }
        if (last)
            break;
    }
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)code;
}

static void new_job(void)
{
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li;
    if (s_job)
        CloseHandle(s_job);
    s_job = CreateJobObjectW(NULL, NULL);
    if (s_job) {
        memset(&li, 0, sizeof li);
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(s_job, JobObjectExtendedLimitInformation, &li, sizeof li);
    }
}

static int game_running(void)
{
    PROCESSENTRY32W e;
    HANDLE h = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    int found = 0;

    if (h == INVALID_HANDLE_VALUE)
        return 0;
    e.dwSize = sizeof e;
    if (Process32FirstW(h, &e))
        do {
            if (!_wcsicmp(e.szExeFile, L"nightfire.exe") || !_wcsicmp(e.szExeFile, L"nightfire_driving.exe"))
                found = 1;
        } while (!found && Process32NextW(h, &e));
    CloseHandle(h);
    return found;
}

/* ---- The game files --------------------------------------------------- */

/* Title ID and game region from an XBE's certificate; 0 if it is none. */
static int xbe_read(const wchar_t *path, DWORD *tid, DWORD *region)
{
    unsigned char h[0x1000];
    DWORD n = 0, base, cert;
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);

    if (f == INVALID_HANDLE_VALUE)
        return 0;
    ReadFile(f, h, sizeof h, &n, NULL);
    CloseHandle(f);
    if (n < 0x120 || memcmp(h, "XBEH", 4))
        return 0;
    memcpy(&base, h + 0x104, 4);
    memcpy(&cert, h + 0x118, 4);
    if (cert < base || cert - base + 0xA4 > n)
        return 0;
    memcpy(tid, h + cert - base + 0x08, 4);
    memcpy(region, h + cert - base + 0xA0, 4);
    return 1;
}

static ULONGLONG dir_bytes(const wchar_t *dir)
{
    WIN32_FIND_DATAW d;
    wchar_t pat[MAX_PATH], sub[MAX_PATH];
    ULONGLONG n = 0;
    HANDLE h;

    path_join(pat, dir, L"*");
    if ((h = FindFirstFileW(pat, &d)) == INVALID_HANDLE_VALUE)
        return 0;
    do {
        if (!wcscmp(d.cFileName, L".") || !wcscmp(d.cFileName, L".."))
            continue;
        if (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            path_join(sub, dir, d.cFileName);
            n += dir_bytes(sub);
        } else {
            n += ((ULONGLONG)d.nFileSizeHigh << 32) | d.nFileSizeLow;
        }
    } while (FindNextFileW(h, &d));
    FindClose(h);
    return n;
}

/* The game folder that will be installed, dest\game: no copy when the chosen
 * files already are that folder (installing again). */
static int game_in_place(const wchar_t *dest)
{
    wchar_t d[MAX_PATH], g[MAX_PATH];
    path_full(dest, d);
    wcscat(d, L"\\game");
    path_full(s_game.dir, g);
    return !_wcsicmp(d, g);
}

static void space_update(void)
{
    wchar_t dest[MAX_PATH], t[64], s[96];
    ULONGLONG need;

    GetWindowTextW(s_dest, dest, MAX_PATH);
    need = BUILD_BYTES + (s_game.ok && !game_in_place(dest) ? s_game.bytes : 0);
    gb(need, t, 64);
    swprintf(s, 96, L"Required space: %s", t);
    SetWindowTextW(s_space, s);
    if (s_state == S_FORM)
        SendMessageW(s_status, SB_SETTEXTW, 1, (LPARAM)t);
}

/* The files both discs carry (European and North-American), beside the
 * region's video folder. A copy without them builds, and then the game
 * stops on "There's a problem with the disc you're using": filesys.d00-d10
 * hold nearly all of the on-foot game. */
static const wchar_t *const k_disc_files[] = {
    L"Driving.xbe",
    L"eurocom\\filesys.d00", L"eurocom\\filesys.d01", L"eurocom\\filesys.d02",
    L"eurocom\\filesys.d03", L"eurocom\\filesys.d04", L"eurocom\\filesys.d05",
    L"eurocom\\filesys.d06", L"eurocom\\filesys.d07", L"eurocom\\filesys.d08",
    L"eurocom\\filesys.d09", L"eurocom\\filesys.d10",
    L"driving\\bond.dat", L"driving\\debug.xfn", L"driving\\misc.viv", L"driving\\race.viv",
    L"driving\\mis01.mus", L"driving\\mis01.viv", L"driving\\mis01en.spe",
    L"driving\\mis11.mus", L"driving\\mis11.viv", L"driving\\mis11en.spe",
    L"driving\\mis13a.mus", L"driving\\mis13a.viv", L"driving\\mis13aen.spe",
    L"driving\\mis13b.mus", L"driving\\mis13b.viv", L"driving\\mis13ben.spe",
    L"driving\\mis13c.mus", L"driving\\mis13c.viv", L"driving\\mis13cen.spe",
    L"driving\\mis3.mus", L"driving\\mis3.viv", L"driving\\mis3en.spe",
    L"driving\\mis4.mus", L"driving\\mis4.viv", L"driving\\mis4en.spe",
};

/* The first file of a complete disc that the folder lacks, or NULL. */
static const wchar_t *disc_missing(const wchar_t *dir)
{
    wchar_t p[MAX_PATH], v[MAX_PATH];
    size_t i;

    for (i = 0; i < sizeof k_disc_files / sizeof k_disc_files[0]; i++) {
        path_join(p, dir, k_disc_files[i]);
        if (!exists(p))
            return k_disc_files[i];
    }
    path_join(p, dir, L"eurocom\\25_fps");
    path_join(v, dir, L"eurocom\\30_fps");
    return is_dir(p) || is_dir(v) ? NULL : L"eurocom\\25_fps (or 30_fps)";
}

/* Takes the XBE (or any file of the disc's top folder) the player chose. */
static void game_pick(const wchar_t *file)
{
    wchar_t full[MAX_PATH], p[MAX_PATH], size[32], why[160], *slash;
    DWORD tid = 0, region = 0;
    const wchar_t *err = NULL, *where, *missing;

    path_full(file, full);
    SetWindowTextW(s_xbe, full);
    memset(&s_game, 0, sizeof s_game);
    wcscpy(s_game.dir, full);
    if (is_dir(full)) {
        path_join(p, full, L"default.xbe");
        SetWindowTextW(s_xbe, p);
    } else if ((slash = wcsrchr(s_game.dir, L'\\')) != NULL) {
        *slash = 0;
    }
    path_join(p, s_game.dir, L"default.xbe");
    if (!exists(p))
        err = L"There is no default.xbe in this folder.";
    else if (!xbe_read(p, &tid, &region) || tid != NF_TITLE_ID)
        err = L"This default.xbe is not 007: Nightfire.";
    else if ((missing = disc_missing(s_game.dir)) != NULL) {
        swprintf(why, 160, L"Incomplete copy of the disc: %s is missing.", missing);
        err = why;
    }
    if (err) {
        SetWindowTextW(s_note, err);
    } else {
        s_game.ok = 1;
        s_game.region = region;
        s_game.bytes = dir_bytes(s_game.dir);
        where = region == 1 ? L"USA (NTSC)" : region & 4 ? L"Europe (PAL)" : L"other region";
        gb(s_game.bytes, size, 32);
        swprintf(p, MAX_PATH, L"007: Nightfire, %s, %s", where, size);
        SetWindowTextW(s_note, p);
    }
    InvalidateRect(s_note, NULL, TRUE);
    space_update();
}

/* Why the folder cannot take the install, or NULL. */
static const wchar_t *dest_problem(const wchar_t *dest)
{
    static const KNOWNFOLDERID *const k_no[] = { &FOLDERID_ProgramFiles, &FOLDERID_ProgramFilesX86, &FOLDERID_Windows };
    wchar_t full[MAX_PATH];
    size_t i;

    if (!dest[0])
        return L"Choose a folder to install into.";
    if (!((iswalpha(dest[0]) && dest[1] == L':' && dest[2] == L'\\') || (dest[0] == L'\\' && dest[1] == L'\\')))
        return L"Choose a full path, for example C:\\Games\\Nightfire.";
    path_full(dest, full);
    if (wcslen(full) <= 3)
        return L"Choose a folder, not a whole drive.";
    for (i = 0; i < sizeof k_no / sizeof k_no[0]; i++) {
        PWSTR k = NULL;
        if (SUCCEEDED(SHGetKnownFolderPath(k_no[i], 0, NULL, &k)) && k) {
            size_t n = wcslen(k);
            int under = !_wcsnicmp(full, k, n) && (full[n] == 0 || full[n] == L'\\');
            CoTaskMemFree(k);
            if (under)
                return L"Windows only lets programs write here as an administrator, and the game "
                       L"writes its settings and logs beside itself. Choose another folder, "
                       L"for example C:\\Games\\Nightfire.";
        }
    }
    return NULL;
}

/* ---- The install (worker thread) -------------------------------------- */

static int fail(const wchar_t *msg, const wchar_t *advice)
{
    if (s_cancel)
        return R_CANCEL;
    wcsncpy(s_fail_msg, msg, 255);
    wcsncpy(s_fail_adv, advice ? advice : L"", 511);
    logw(L"FAILED: %s %s", msg, advice ? advice : L"");
    return R_FAIL;
}

static int write_resource(int id, const wchar_t *path)
{
    HRSRC r = FindResourceW(NULL, MAKEINTRESOURCEW(id), (LPCWSTR)RT_RCDATA);
    HGLOBAL g = r ? LoadResource(NULL, r) : NULL;
    const void *p = g ? LockResource(g) : NULL;
    DWORD n = r ? SizeofResource(NULL, r) : 0, w = 0;
    HANDLE f;

    if (!p || !n)
        return 0;
    f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return 0;
    WriteFile(f, p, n, &w, NULL);
    CloseHandle(f);
    return w == n;
}

/* A file's bytes, to put back after the payload overwrote it. */
static char *slurp(const wchar_t *path, DWORD *n)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    char *b = NULL;
    DWORD size;

    *n = 0;
    if (f == INVALID_HANDLE_VALUE)
        return NULL;
    size = GetFileSize(f, NULL);
    if (size != INVALID_FILE_SIZE && (b = (char *)malloc(size + 1)) != NULL)
        ReadFile(f, b, size, n, NULL);
    CloseHandle(f);
    return b;
}

static void spill(const wchar_t *path, const char *b, DWORD n)
{
    DWORD w;
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        WriteFile(f, b, n, &w, NULL);
        CloseHandle(f);
    }
}

/* Deletes a folder inside the install folder with everything in it. */
static int remove_tree(const wchar_t *path)
{
    wchar_t from[MAX_PATH + 2];
    SHFILEOPSTRUCTW op;
    size_t n = wcslen(s_dir);

    if (_wcsnicmp(path, s_dir, n) || path[n] != L'\\' || !is_dir(path))
        return !exists(path);
    memset(from, 0, sizeof from);
    wcsncpy(from, path, MAX_PATH);
    memset(&op, 0, sizeof op);
    op.wFunc = FO_DELETE;
    op.pFrom = from;                      /* double-NUL ended */
    op.fFlags = FOF_NO_UI;
    return SHFileOperationW(&op) == 0 && !exists(path);
}

static int stage_unpack(void)
{
    static const wchar_t *const k_keep[] = { L"nightfire.ini", L"mkb.ini" };
    static const wchar_t *const k_what[] = { L"Unpacking the recompiler", L"Unpacking the toolkit",
                                             L"Unpacking the build tools (Zig, Python, CMake, Ninja)" };
    wchar_t tmp[MAX_PATH], zip[MAX_PATH], tar[MAX_PATH], cmd[3 * MAX_PATH], p[MAX_PATH];
    char *kept[2] = { NULL, NULL };
    DWORD kept_n[2] = { 0, 0 };
    int i, ids[3] = { IDR_PROJECT, IDR_TOOLKIT, IDR_TOOLCHAIN }, ok = 1;

    seg(0.0, 0.06, 40000, L"Step 1 of 5: Unpacking", k_what[0]);
    GetSystemDirectoryW(tar, MAX_PATH);
    wcscat(tar, L"\\tar.exe");
    if (!exists(tar))
        return fail(L"This installer needs Windows 10 (version 1803) or newer.", NULL);
    /* The player's settings survive a new install into the same folder. */
    for (i = 0; i < 2; i++) {
        path_join(p, s_dir, k_keep[i]);
        kept[i] = slurp(p, &kept_n[i]);
    }
    /* The build tools of an earlier install: replaced whole, so none of a
     * different version is left among the new ones. */
    path_join(p, s_dir, L"toolchain");
    if (!remove_tree(p))
        ok = 0;
    GetTempPathW(MAX_PATH, tmp);
    for (i = 0; i < 3 && ok; i++) {
        detail(L"%s", k_what[i]);
        swprintf(zip, MAX_PATH, L"%snightfire_setup_%lu_%d.zip", tmp, GetCurrentProcessId(), i);
        ok = write_resource(ids[i], zip);
        /* -m: the files get the time of the install, not the commit's. With
         * the commit's, a changed source in a newer installer could be older
         * than its object file from the last install, and not be rebuilt. */
        if (ok) {
            swprintf(cmd, 3 * MAX_PATH, L"\"%s\" -xmf \"%s\" -C \"%s\"", tar, zip, s_dir);
            ok = run(cmd, s_dir, NULL, NULL, 0) == 0;
        }
        DeleteFileW(zip);
        seg_frac(i == 0 ? 0.02 : i == 1 ? 0.1 : 1.0);
    }
    /* The new default key layout, which the launcher's "Default" button reads
     * (a git checkout has it in git instead). */
    if (ok) {
        wchar_t def[MAX_PATH];
        path_join(p, s_dir, L"mkb.ini");
        path_join(def, s_dir, L"mkb.default.ini");
        CopyFileW(p, def, FALSE);
    }
    for (i = 0; i < 2; i++)
        if (kept[i]) {
            path_join(p, s_dir, k_keep[i]);
            spill(p, kept[i], kept_n[i]);
            free(kept[i]);
        }
    if (!ok)
        return fail(L"The recompiler couldn't be unpacked.",
                    L"Check that the folder is writable and the drive has space, then try again.");
    path_join(p, s_dir, L"scripts\\recompile.ps1");
    path_join(tmp, s_dir, L"toolchain\\zig\\zig.exe");
    return exists(p) && exists(tmp) ? R_OK
                                    : fail(L"The recompiler is incomplete.", L"Download the installer again.");
}

static ULONGLONG s_copy_done, s_copy_total;

static DWORD CALLBACK copy_progress(LARGE_INTEGER total, LARGE_INTEGER done, LARGE_INTEGER s1, LARGE_INTEGER s2,
                                    DWORD id, DWORD reason, HANDLE src, HANDLE dst, LPVOID ctx)
{
    wchar_t a[32], b[32];
    (void)total; (void)s1; (void)s2; (void)id; (void)reason; (void)src; (void)dst; (void)ctx;
    if (s_copy_total) {
        ULONGLONG now = s_copy_done + (ULONGLONG)done.QuadPart;
        seg_frac((double)now / s_copy_total);
        gb(now, a, 32);
        gb(s_copy_total, b, 32);
        detail(L"Copied %s of %s", a, b);
    }
    return s_cancel ? PROGRESS_CANCEL : PROGRESS_CONTINUE;
}

/* Copies a folder tree; a file already there with the same size and time is
 * kept (installing again). */
static int copy_tree(const wchar_t *src, const wchar_t *dst)
{
    WIN32_FIND_DATAW d;
    wchar_t pat[MAX_PATH], s[MAX_PATH], t[MAX_PATH];
    HANDLE h;
    int ok = 1;

    SHCreateDirectoryExW(NULL, dst, NULL);
    path_join(pat, src, L"*");
    if ((h = FindFirstFileW(pat, &d)) == INVALID_HANDLE_VALUE)
        return 0;
    do {
        WIN32_FILE_ATTRIBUTE_DATA have;
        ULONGLONG size = ((ULONGLONG)d.nFileSizeHigh << 32) | d.nFileSizeLow;
        if (!wcscmp(d.cFileName, L".") || !wcscmp(d.cFileName, L".."))
            continue;
        path_join(s, src, d.cFileName);
        path_join(t, dst, d.cFileName);
        if (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            ok = copy_tree(s, t);
            continue;
        }
        if (GetFileAttributesExW(t, GetFileExInfoStandard, &have)
                && have.nFileSizeHigh == d.nFileSizeHigh && have.nFileSizeLow == d.nFileSizeLow
                && !CompareFileTime(&have.ftLastWriteTime, &d.ftLastWriteTime)) {
            s_copy_done += size;
            continue;
        }
        SetFileAttributesW(t, FILE_ATTRIBUTE_NORMAL);
        ok = CopyFileExW(s, t, copy_progress, NULL, &s_cancel_copy, 0);
        if (!ok)
            logw(L"copy %s failed (%lu)", s, GetLastError());
        s_copy_done += size;
    } while (ok && !s_cancel && FindNextFileW(h, &d));
    FindClose(h);
    return ok && !s_cancel;
}

/* Removes what dst holds and src does not: game\ is the chosen disc exactly,
 * also when an install switches discs (the US disc's eurocom\30_fps and
 * config.txt, say, must not stay beside the European disc's files). */
static void prune_tree(const wchar_t *src, const wchar_t *dst)
{
    WIN32_FIND_DATAW d;
    wchar_t pat[MAX_PATH], s[MAX_PATH], t[MAX_PATH];
    HANDLE h;

    path_join(pat, dst, L"*");
    if ((h = FindFirstFileW(pat, &d)) == INVALID_HANDLE_VALUE)
        return;
    do {
        if (!wcscmp(d.cFileName, L".") || !wcscmp(d.cFileName, L".."))
            continue;
        path_join(s, src, d.cFileName);
        path_join(t, dst, d.cFileName);
        if (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (is_dir(s))
                prune_tree(s, t);
            else if (remove_tree(t))
                logw(L"removed %s (not on the disc)", t);
        } else if (!exists(s) || is_dir(s)) {
            SetFileAttributesW(t, FILE_ATTRIBUTE_NORMAL);
            if (DeleteFileW(t))
                logw(L"removed %s (not on the disc)", t);
        }
    } while (FindNextFileW(h, &d));
    FindClose(h);
}

static int stage_copy(void)
{
    wchar_t game[MAX_PATH];

    seg(0.06, 0.18, 60000, L"Step 2 of 5: Copying the game files", L"");
    if (game_in_place(s_dir)) {
        detail(L"The game files are already in place");
        return R_OK;
    }
    path_join(game, s_dir, L"game");
    s_copy_done = 0;
    s_copy_total = s_game.bytes;
    logw(L"copy %s -> %s (%llu bytes)", s_game.dir, game, s_copy_total);
    if (!copy_tree(s_game.dir, game))
        return fail(L"The game files couldn't be copied.",
                    L"Check that the drive has enough free space, then try again.");
    prune_tree(s_game.dir, game);
    return R_OK;
}

/* recompile.ps1's lines for the main image, which takes the longest (the
 * driving image runs beside it, and its lines can end up on the same line as
 * main's): where each tool starts within the stage, how
 * long it takes on an average PC, and what the player is told meanwhile. */
static const struct { const char *tool; double at, next; DWORD ms; const wchar_t *text; } k_lift[] = {
    { "tools.xbe_parser",   0.00, 0.01,   3000, L"Reading the game's program files" },
    { "tools.disasm",       0.01, 0.62, 120000, L"Finding every function in the game's code" },
    { "tools.func_id",      0.62, 0.63,   3000, L"Recognising library code" },
    { "tools.abi_analysis", 0.63, 0.64,   3000, L"Working out how the functions call each other" },
    { "tools.recomp",       0.64, 1.00,  70000, L"Writing the game's code as C" },
};
#define LIFT_LO 0.18
#define LIFT_HI 0.60

static void lift_line(const char *l)
{
    const char *m = strstr(l, "main: ");
    size_t i;

    if (!m)
        return;
    m += 6;
    if (strstr(m, "unchanged, skipped")) {
        seg(LIFT_LO + (LIFT_HI - LIFT_LO) * 0.99, LIFT_HI, 1000, NULL, L"The game's code is already translated");
        return;
    }
    for (i = 0; i < sizeof k_lift / sizeof k_lift[0]; i++) {
        size_t n = strlen(k_lift[i].tool);
        if (!strncmp(m, k_lift[i].tool, n) && (m[n] == 0 || m[n] == ' ' || m[n] == '\r')
                && strncmp(m + n, " done", 5))
            seg(LIFT_LO + (LIFT_HI - LIFT_LO) * k_lift[i].at, LIFT_LO + (LIFT_HI - LIFT_LO) * k_lift[i].next,
                k_lift[i].ms, NULL, k_lift[i].text);
    }
}

static int stage_lift(void)
{
    wchar_t cmd[2 * MAX_PATH];

    seg(LIFT_LO, LIFT_LO + 0.005, 3000, L"Step 3 of 5: Translating the game's code", L"Starting");
    swprintf(cmd, 2 * MAX_PATH, L"powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"%s\\scripts\\recompile.ps1\"", s_dir);
    if (run(cmd, s_dir, lift_line, NULL, 0) != 0)
        return fail(L"The game's code couldn't be translated.",
                    L"The details are in logs\\install.log in the install folder.");
    return R_OK;
}

static int count_c(const wchar_t *dir, int deep)
{
    WIN32_FIND_DATAW d;
    wchar_t pat[MAX_PATH], sub[MAX_PATH];
    HANDLE h;
    int n = 0;

    path_join(pat, dir, L"*");
    if ((h = FindFirstFileW(pat, &d)) == INVALID_HANDLE_VALUE)
        return 0;
    do {
        size_t len = wcslen(d.cFileName);
        if (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (deep && wcscmp(d.cFileName, L".") && wcscmp(d.cFileName, L"..")) {
                path_join(sub, dir, d.cFileName);
                n += count_c(sub, 1);
            }
        } else if (len > 2 && !_wcsicmp(d.cFileName + len - 2, L".c")) {
            n++;
        }
    } while (FindNextFileW(h, &d));
    FindClose(h);
    return n;
}

/* MSBuild names each C file as the compiler takes it, alone on its line. */
static void build_line(const char *l)
{
    const char *p = l, *e;
    while (*p == ' ' || *p == '\t')
        p++;
    e = p + strlen(p);
    while (e > p && (e[-1] == ' ' || e[-1] == '\r'))
        e--;
    if (e - p > 2 && e[-2] == '.' && (e[-1] == 'c' || e[-1] == 'C') && !memchr(p, ' ', (size_t)(e - p))
            && !memchr(p, ':', (size_t)(e - p))) {
        s_compiled++;
        seg_frac(s_compile_total ? (double)s_compiled / s_compile_total : 0);
        detail(L"Compiled %d of %d code files", s_compiled < s_compile_total ? s_compiled : s_compile_total,
               s_compile_total);
    }
    if (!s_first_error[0] && (strstr(l, ": error ") || strstr(l, ": fatal error ")))
        strncpy(s_first_error, l, sizeof s_first_error - 1);
}

static int stage_build(void)
{
    wchar_t cmd[2 * MAX_PATH], p[MAX_PATH];

    seg(0.60, 0.99, 240000, L"Step 4 of 5: Compiling", L"Setting up the build");
    s_compiled = 0;
    s_first_error[0] = 0;
    path_join(p, s_dir, L"src\\recomp\\main\\gen");
    s_compile_total = count_c(p, 0);
    path_join(p, s_dir, L"src\\recomp\\driving\\gen");
    s_compile_total += count_c(p, 0);
    path_join(p, s_dir, L"external\\xboxrecomp\\src");
    s_compile_total += count_c(p, 1) + 9;      /* main.c and the others twice, the launcher */
    swprintf(cmd, 2 * MAX_PATH, L"powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"%s\\scripts\\build.ps1\"", s_dir);
    if (run(cmd, s_dir, build_line, NULL, 0) != 0) {
        wchar_t adv[512];
        if (s_first_error[0])
            swprintf(adv, 512, L"%S\n\nThe details are in logs\\install.log in the install folder.", s_first_error);
        else
            wcscpy(adv, L"The details are in logs\\install.log in the install folder.");
        return fail(L"The game couldn't be compiled.", adv);
    }
    path_join(p, s_dir, L"Nightfire Launcher.exe");
    return exists(p) ? R_OK : fail(L"The build finished without the launcher.",
                                   L"The details are in logs\\install.log in the install folder.");
}

static int shortcut(const wchar_t *target, const wchar_t *dir)
{
    wchar_t desk[MAX_PATH], lnk[MAX_PATH];
    IShellLinkW *sl = NULL;
    IPersistFile *pf = NULL;
    int ok = 0;

    if (FAILED(SHGetFolderPathW(NULL, CSIDL_DESKTOPDIRECTORY, NULL, 0, desk)))
        return 0;
    path_join(lnk, desk, L"Nightfire.lnk");
    if (FAILED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&sl)))
        return 0;
    sl->lpVtbl->SetPath(sl, target);
    sl->lpVtbl->SetWorkingDirectory(sl, dir);
    sl->lpVtbl->SetDescription(sl, L"007: Nightfire");
    if (SUCCEEDED(sl->lpVtbl->QueryInterface(sl, &IID_IPersistFile, (void **)&pf))) {
        ok = SUCCEEDED(pf->lpVtbl->Save(pf, lnk, TRUE));
        pf->lpVtbl->Release(pf);
    }
    sl->lpVtbl->Release(sl);
    return ok;
}

static int stage_finish(void)
{
    wchar_t exe[MAX_PATH], p[MAX_PATH];
    HRSRC r = FindResourceW(NULL, MAKEINTRESOURCEW(IDR_MANIFEST), (LPCWSTR)RT_RCDATA);
    HGLOBAL g = r ? LoadResource(NULL, r) : NULL;

    seg(0.99, 1.0, 2000, L"Step 5 of 5: Finishing", L"");
    if (g) {
        path_join(p, s_dir, L"install-info.txt");
        spill(p, (const char *)LockResource(g), SizeofResource(NULL, r));
    }
    path_join(exe, s_dir, L"Nightfire Launcher.exe");
    if (!s_test) {
        RegSetKeyValueW(HKEY_CURRENT_USER, REG_KEY, L"InstallDir", REG_SZ, s_dir,
                        (DWORD)((wcslen(s_dir) + 1) * sizeof(wchar_t)));
        if (s_want_shortcut && !shortcut(exe, s_dir))
            logw(L"the desktop shortcut could not be made");
    }
    return R_OK;
}

static DWORD WINAPI install_thread(void *arg)
{
    wchar_t p[MAX_PATH];
    int r = R_OK;
    (void)arg;

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED);      /* no sleep mid-build */
    SHCreateDirectoryExW(NULL, s_dir, NULL);
    path_join(p, s_dir, L"logs");
    CreateDirectoryW(p, NULL);
    path_join(p, s_dir, L"logs\\install.log");
    s_log = CreateFileW(p, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);
    logw(L"Nightfire installer: game files %s (region %lu), install folder %s", s_game.dir, s_game.region, s_dir);

    r = stage_unpack();
    if (r == R_OK && !s_cancel)
        r = stage_copy();
    if (r == R_OK && !s_cancel)
        r = stage_lift();
    if (r == R_OK && !s_cancel)
        r = stage_build();
    if (r == R_OK && !s_cancel)
        r = stage_finish();
    if (s_cancel)
        r = R_CANCEL;
    s_took = GetTickCount() - s_t0;
    logw(L"%s after %lu s", r == R_OK ? L"Done" : r == R_CANCEL ? L"Cancelled" : L"Failed", s_took / 1000);
    if (s_log != INVALID_HANDLE_VALUE) {
        CloseHandle(s_log);
        s_log = INVALID_HANDLE_VALUE;
    }
    SetThreadExecutionState(ES_CONTINUOUS);
    CoUninitialize();
    PostMessageW(s_win, WM_SETUP_DONE, (WPARAM)r, 0);
    return 0;
}

/* ---- Window ----------------------------------------------------------- */

static void set_texts(const wchar_t *title, const wchar_t *sub)
{
    wcsncpy(s_title, title, 127);
    wcsncpy(s_sub, sub, 511);
    InvalidateRect(s_win, NULL, TRUE);
}

static void show_state(int state)
{
    int i, form = state == S_FORM, run_ = !form;
    s_state = state;
    for (i = 0; s_form_ctl[i]; i++)
        ShowWindow(s_form_ctl[i], form ? SW_SHOW : SW_HIDE);
    ShowWindow(s_step_w, run_ ? SW_SHOW : SW_HIDE);
    ShowWindow(s_bar, state == S_RUN || state == S_DONE ? SW_SHOW : SW_HIDE);
    ShowWindow(s_detail_w, run_ ? SW_SHOW : SW_HIDE);
    SetWindowTextW(s_go, form ? (s_installed ? L"Reinstall" : L"Install")
                     : state == S_RUN ? L"Cancel" : state == S_DONE ? L"Play" : L"Back");
    EnableWindow(s_go, TRUE);
    InvalidateRect(s_go, NULL, TRUE);
}

static void form_texts(void)
{
    wchar_t t[MAX_PATH], sub[512];
    if (s_installed) {
        GetWindowTextW(s_dest, t, MAX_PATH);
        swprintf(sub, 512, L"Installed in %s. Installing again rebuilds it and keeps your settings and saves.", t);
        set_texts(L"Reinstall Nightfire", sub);
    } else {
        set_texts(L"Install Nightfire", L"The game was not found on this computer.");
    }
    SendMessageW(s_status, SB_SETTEXTW, 0, (LPARAM)(s_installed ? L"Installed" : L"Not installed"));
    space_update();
}

static void start_install(void)
{
    wchar_t dest[MAX_PATH], p[MAX_PATH], t[64];
    const wchar_t *why;
    ULARGE_INTEGER free_b;
    ULONGLONG need;
    wchar_t root[4];

    if (!s_game.ok) {
        wchar_t note[200] = L"";
        if (s_game.dir[0])
            GetWindowTextW(s_note, note, 200);
        MessageBoxW(s_win, note[0] ? note : L"Choose the default.xbe of your extracted Nightfire disc first.",
                    L"Nightfire Setup", MB_OK | MB_ICONINFORMATION);
        return;
    }
    GetWindowTextW(s_dest, dest, MAX_PATH);
    if ((why = dest_problem(dest)) != NULL) {
        MessageBoxW(s_win, why, L"Nightfire Setup", MB_OK | MB_ICONWARNING);
        return;
    }
    path_full(dest, s_dir);
    if (game_running()) {
        MessageBoxW(s_win, L"Nightfire is running. Close the game, then install.", L"Nightfire Setup",
                    MB_OK | MB_ICONWARNING);
        return;
    }
    /* A folder of something else is not filled without asking. */
    path_join(p, s_dir, L"scripts\\build.ps1");
    if (is_dir(s_dir) && !exists(p) && !PathIsDirectoryEmptyW(s_dir)
            && MessageBoxW(s_win, L"This folder is not empty. Install into it anyway?", L"Nightfire Setup",
                           MB_YESNO | MB_ICONQUESTION) != IDYES)
        return;
    need = BUILD_BYTES + (game_in_place(s_dir) ? 0 : s_game.bytes);
    swprintf(root, 4, L"%c:\\", s_dir[0]);
    if (s_dir[1] == L':' && GetDiskFreeSpaceExW(root, &free_b, NULL, NULL) && free_b.QuadPart < need) {
        gb(need, t, 64);
        swprintf(p, MAX_PATH, L"The drive %s needs %s free for the install. Free some space or choose another drive.", root, t);
        MessageBoxW(s_win, p, L"Nightfire Setup", MB_OK | MB_ICONWARNING);
        return;
    }

    s_want_shortcut = SendMessageW(s_shortcut, BM_GETCHECK, 0, 0) == BST_CHECKED;
    s_cancel = 0;
    s_cancel_copy = FALSE;
    s_t0 = GetTickCount();
    new_job();
    seg(0, 0.02, 2000, L"Starting", L"");
    show_state(S_RUN);
    set_texts(L"Installing Nightfire", L"This takes a few minutes, most of it translating and compiling. "
                                       L"The PC stays awake meanwhile.");
    SendMessageW(s_status, SB_SETTEXTW, 0, (LPARAM)L"Installing ...");
    SendMessageW(s_bar, PBM_SETPOS, 0, 0);
    SetTimer(s_win, ID_TIMER, 250, NULL);
    CloseHandle(CreateThread(NULL, 0, install_thread, NULL, 0, NULL));
}

static void cancel_install(void)
{
    InterlockedExchange(&s_cancel, 1);
    s_cancel_copy = TRUE;
    if (s_job)
        TerminateJobObject(s_job, 1);
    EnableWindow(s_go, FALSE);
    SetWindowTextW(s_step_w, L"Cancelling ...");
}

static void install_done(int r)
{
    wchar_t t[512], took[32];
    KillTimer(s_win, ID_TIMER);
    swprintf(took, 32, L"%lu:%02lu", s_took / 60000, s_took / 1000 % 60);
    if (r == R_OK) {
        s_installed = 1;
        show_state(S_DONE);
        SendMessageW(s_bar, PBM_SETPOS, 1000, 0);
        swprintf(t, 512, L"It is in %s. Start it with \"Play\" or with the Nightfire Launcher in that folder%s.",
                 s_dir, s_want_shortcut && !s_test ? L" (and on the desktop)" : L"");
        set_texts(L"Nightfire is installed", t);
        SetWindowTextW(s_step_w, L"Done");
        swprintf(t, 512, L"Took %s.", took);
        SetWindowTextW(s_detail_w, t);
        SendMessageW(s_status, SB_SETTEXTW, 0, (LPARAM)L"Installed");
    } else if (r == R_CANCEL) {
        show_state(S_FORM);
        form_texts();
        SendMessageW(s_status, SB_SETTEXTW, 0, (LPARAM)L"Cancelled");
    } else {
        show_state(S_FAIL);
        set_texts(L"Installation failed", s_fail_msg);
        SetWindowTextW(s_step_w, s_step);
        SetWindowTextW(s_detail_w, s_fail_adv);
        SendMessageW(s_status, SB_SETTEXTW, 0, (LPARAM)L"Failed");
    }
    SendMessageW(s_status, SB_SETTEXTW, 1, (LPARAM)took);
    /* /test /go: a script waits for the result, as the exit code. */
    if (s_test && s_auto) {
        s_exit = r;
        DestroyWindow(s_win);
    }
}

static void tick(void)
{
    wchar_t t[64];
    DWORD s = (GetTickCount() - s_t0) / 1000;
    if (s_state != S_RUN)
        return;
    SendMessageW(s_bar, PBM_SETPOS, (WPARAM)(progress() * 1000), 0);
    EnterCriticalSection(&s_cs);
    if (!s_cancel)
        SetWindowTextW(s_step_w, s_step);
    SetWindowTextW(s_detail_w, s_detail);
    LeaveCriticalSection(&s_cs);
    swprintf(t, 64, L"%lu:%02lu", s / 60, s % 60);
    SendMessageW(s_status, SB_SETTEXTW, 1, (LPARAM)t);
}

static void browse_xbe(void)
{
    OPENFILENAMEW o;
    wchar_t f[MAX_PATH] = L"";

    memset(&o, 0, sizeof o);
    o.lStructSize = sizeof o;
    o.hwndOwner = s_win;
    o.lpstrFilter = L"Nightfire (default.xbe)\0default.xbe\0Xbox programs (*.xbe)\0*.xbe\0";
    o.lpstrFile = f;
    o.nMaxFile = MAX_PATH;
    o.lpstrTitle = L"The default.xbe of your extracted Nightfire disc";
    o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (GetOpenFileNameW(&o))
        game_pick(f);
}

static void browse_dest(void)
{
    BROWSEINFOW b;
    PIDLIST_ABSOLUTE id;
    wchar_t p[MAX_PATH];

    memset(&b, 0, sizeof b);
    b.hwndOwner = s_win;
    b.lpszTitle = L"The folder to install Nightfire into. A new folder \"Nightfire\" is made inside it.";
    b.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    if ((id = SHBrowseForFolderW(&b)) != NULL) {
        if (SHGetPathFromIDListW(id, p)) {
            size_t n = wcslen(p);
            const wchar_t *last = wcsrchr(p, L'\\');
            /* Inside the chosen folder, unless it already is a Nightfire folder. */
            if (!(last && !_wcsicmp(last + 1, L"Nightfire")))
                swprintf(p + n, MAX_PATH - n, L"%sNightfire", n && p[n - 1] == L'\\' ? L"" : L"\\");
            SetWindowTextW(s_dest, p);
        }
        CoTaskMemFree(id);
    }
}

static HWND add(const wchar_t *cls, const wchar_t *text, DWORD style, int x, int y, int w, int h, int id, HFONT f)
{
    HWND c = CreateWindowExW(!wcscmp(cls, L"EDIT") ? WS_EX_CLIENTEDGE : 0, cls, text, WS_CHILD | WS_VISIBLE | style,
                             px(x), px(y), px(w), px(h), s_win, (HMENU)(INT_PTR)id, s_inst, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)(f ? f : s_font), TRUE);
    return c;
}

/* The banner: a dark screen with a red glow on the right and fine scan lines,
 * the title in the middle. Drawn once into a bitmap. */
static void banner_make(void)
{
    BITMAPINFO bi;
    void *bits;
    HDC dc = CreateCompatibleDC(NULL);
    HFONT lite, big, bigi, old;     /* not "small": a macro in the Windows headers */
    RECT r;
    SIZE a, b;
    int w = px(BANNER_W), h = px(BANNER_H), x, y, line = px(3) < 3 ? 3 : px(3), top;
    double gx = w * 0.80, gy = h * 0.50, gr = h * 1.10;

    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    s_banner = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!s_banner) {
        DeleteDC(dc);
        return;
    }
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            double t = (double)y / h, dx = (x - gx) / gr, dy = (y - gy) / (gr * 0.75);
            double d = sqrt(dx * dx + dy * dy), rr = 9 + 6 * t, gg = 9 + 3 * t, bb = 13 + t;
            if (d < 1) {
                double k = 1 - d;
                k = k * k * (0.55 + 0.45 * k);
                rr += 200 * k;
                gg += 58 * k;
                bb += 24 * k;
            }
            if (y % line == 0) {
                rr *= 0.55;
                gg *= 0.55;
                bb *= 0.55;
            }
            if (x == 0 || y == 0 || x == w - 1 || y == h - 1)
                rr = gg = bb = 34;
            ((DWORD *)bits)[y * w + x] = RGB((int)(bb > 255 ? 255 : bb), (int)(gg > 255 ? 255 : gg),
                                             (int)(rr > 255 ? 255 : rr));
        }
    SelectObject(dc, s_banner);
    SetBkMode(dc, TRANSPARENT);
    lite = CreateFontW(-px(11), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    big = CreateFontW(-px(34), 0, 0, 0, FW_HEAVY, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Arial Black");
    bigi = CreateFontW(-px(34), 0, 0, 0, FW_HEAVY, 1, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Arial Black");

    old = (HFONT)SelectObject(dc, lite);
    SetTextColor(dc, RGB(196, 186, 168));
    SetRect(&r, 0, (int)(h * 0.16), w, (int)(h * 0.32));
    DrawTextW(dc, L"J A M E S    B O N D", -1, &r, DT_CENTER | DT_SINGLELINE | DT_VCENTER);

    SelectObject(dc, big);
    GetTextExtentPoint32W(dc, L"007", 3, &a);
    SelectObject(dc, bigi);
    GetTextExtentPoint32W(dc, L"NIGHTFIRE", 9, &b);
    x = (w - (a.cx + px(12) + b.cx)) / 2;
    top = (int)(h * 0.32);
    SelectObject(dc, big);
    SetTextColor(dc, RGB(0, 0, 0));
    TextOutW(dc, x + px(2), top + px(2), L"007", 3);
    SetTextColor(dc, RGB(222, 222, 222));
    TextOutW(dc, x, top, L"007", 3);
    SelectObject(dc, bigi);
    SetTextColor(dc, RGB(0, 0, 0));
    TextOutW(dc, x + a.cx + px(12) + px(2), top + px(2), L"NIGHTFIRE", 9);
    SetTextColor(dc, RGB(255, 255, 255));
    TextOutW(dc, x + a.cx + px(12), top, L"NIGHTFIRE", 9);

    SelectObject(dc, lite);
    SetTextColor(dc, RGB(226, 82, 58));
    SetRect(&r, 0, (int)(h * 0.72), w, (int)(h * 0.88));
    DrawTextW(dc, L"G A M E    I N S T A L L E R", -1, &r, DT_CENTER | DT_SINGLELINE | DT_VCENTER);

    SelectObject(dc, old);
    DeleteObject(lite);
    DeleteObject(big);
    DeleteObject(bigi);
    DeleteDC(dc);
}

/* The window icon: "NF" on a red tile. */
static HICON make_icon(int size)
{
    BITMAPINFO bi;
    void *bits;
    HDC dc = CreateCompatibleDC(NULL);
    HBITMAP color, mask;
    HFONT f, old;
    HBRUSH red = CreateSolidBrush(RGB(176, 36, 28));
    RECT r = { 0, 0, size, size };
    ICONINFO ii;
    HICON icon;
    int x, y, rad = size / 6;

    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = size;
    bi.bmiHeader.biHeight = -size;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    color = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    SelectObject(dc, color);
    FillRect(dc, &r, red);
    f = CreateFontW(-(size * 11 / 20), 0, 0, 0, FW_HEAVY, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Segoe UI");
    old = (HFONT)SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    DrawTextW(dc, L"NF", 2, &r, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    SelectObject(dc, old);
    DeleteObject(f);
    GdiFlush();
    /* Opaque inside the rounded tile, clear outside it. */
    for (y = 0; y < size; y++)
        for (x = 0; x < size; x++) {
            int cx = x < rad ? rad : x >= size - rad ? size - rad - 1 : x;
            int cy = y < rad ? rad : y >= size - rad ? size - rad - 1 : y;
            int in = (x - cx) * (x - cx) + (y - cy) * (y - cy) <= rad * rad;
            DWORD *p = (DWORD *)bits + y * size + x;
            *p = in ? (*p & 0x00FFFFFF) | 0xFF000000 : 0;
        }
    mask = CreateBitmap(size, size, 1, 1, NULL);
    ii.fIcon = TRUE;
    ii.xHotspot = ii.yHotspot = 0;
    ii.hbmMask = mask;
    ii.hbmColor = color;
    icon = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(color);
    DeleteObject(red);
    DeleteDC(dc);
    return icon;
}

static void draw_go(const DRAWITEMSTRUCT *d)
{
    int dis = (d->itemState & ODS_DISABLED) != 0, down = (d->itemState & ODS_SELECTED) != 0;
    COLORREF top = dis ? RGB(170, 176, 186) : down ? RGB(36, 66, 118) : RGB(76, 117, 184);
    COLORREF bot = dis ? RGB(150, 156, 166) : down ? RGB(30, 56, 102) : RGB(45, 82, 147);
    TRIVERTEX v[2];
    GRADIENT_RECT g = { 0, 1 };
    HRGN rgn = CreateRoundRectRgn(d->rcItem.left, d->rcItem.top, d->rcItem.right + 1, d->rcItem.bottom + 1, px(6), px(6));
    HBRUSH edge = CreateSolidBrush(dis ? RGB(140, 146, 156) : RGB(30, 56, 104));
    wchar_t t[32];
    RECT r = d->rcItem;

    FillRect(d->hDC, &r, s_panel_br);
    SelectClipRgn(d->hDC, rgn);
    v[0].x = r.left; v[0].y = r.top;
    v[0].Red = (COLOR16)(GetRValue(top) << 8); v[0].Green = (COLOR16)(GetGValue(top) << 8);
    v[0].Blue = (COLOR16)(GetBValue(top) << 8); v[0].Alpha = 0;
    v[1].x = r.right; v[1].y = r.bottom;
    v[1].Red = (COLOR16)(GetRValue(bot) << 8); v[1].Green = (COLOR16)(GetGValue(bot) << 8);
    v[1].Blue = (COLOR16)(GetBValue(bot) << 8); v[1].Alpha = 0;
    GradientFill(d->hDC, v, 2, &g, 1, GRADIENT_FILL_RECT_V);
    SelectClipRgn(d->hDC, NULL);
    FrameRgn(d->hDC, rgn, edge, 1, 1);
    GetWindowTextW(d->hwndItem, t, 32);
    SetBkMode(d->hDC, TRANSPARENT);
    SetTextColor(d->hDC, RGB(255, 255, 255));
    SelectObject(d->hDC, s_btn_f);
    DrawTextW(d->hDC, t, -1, &r, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    if ((d->itemState & ODS_FOCUS) && !(d->itemState & ODS_NOFOCUSRECT)) {
        InflateRect(&r, -px(3), -px(3));
        DrawFocusRect(d->hDC, &r);
    }
    DeleteObject(edge);
    DeleteObject(rgn);
}

static void paint(HDC dc)
{
    RECT r;
    HPEN pen = CreatePen(PS_SOLID, 1, k_border), oldp;
    HBRUSH accent = CreateSolidBrush(k_accent), oldb;
    HFONT oldf;
    int x = px(PANEL_X), y = px(PANEL_Y);

    if (s_banner) {
        HDC m = CreateCompatibleDC(dc);
        HBITMAP ob = (HBITMAP)SelectObject(m, s_banner);
        BitBlt(dc, px(BANNER_X), px(BANNER_Y), px(BANNER_W), px(BANNER_H), m, 0, 0, SRCCOPY);
        SelectObject(m, ob);
        DeleteDC(m);
    }
    oldp = (HPEN)SelectObject(dc, pen);
    oldb = (HBRUSH)SelectObject(dc, s_panel_br);
    RoundRect(dc, x, y, x + px(PANEL_W), y + px(PANEL_H), px(8), px(8));
    SetRect(&r, x + px(16), y + px(18), x + px(19), y + px(46));
    FillRect(dc, &r, accent);
    MoveToEx(dc, x + px(16), y + px(78), NULL);
    LineTo(dc, x + px(PANEL_W - 16), y + px(78));
    SelectObject(dc, oldb);
    SelectObject(dc, oldp);

    SetBkMode(dc, TRANSPARENT);
    oldf = (HFONT)SelectObject(dc, s_title_f);
    SetTextColor(dc, RGB(30, 30, 30));
    SetRect(&r, x + px(30), y + px(14), x + px(PANEL_W - 16), y + px(36));
    DrawTextW(dc, s_title, -1, &r, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    SelectObject(dc, s_note_f);
    SetTextColor(dc, k_grey);
    SetRect(&r, x + px(30), y + px(37), x + px(PANEL_W - 16), y + px(74));
    DrawTextW(dc, s_sub, -1, &r, DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS);
    SelectObject(dc, oldf);
    DeleteObject(pen);
    DeleteObject(accent);
}

static LRESULT CALLBACK win_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_CREATE: {
        int y = PANEL_Y, x = PANEL_X + 16, pads[2], n = 0;
        wchar_t dir[MAX_PATH];
        DWORD size = sizeof dir;

        s_win = h;
        s_form_ctl[n++] = add(L"STATIC", L"Game files (default.xbe of your extracted disc)", 0, x, y + 88, 300, 16, ID_L_XBE, NULL);
        s_form_ctl[n++] = s_xbe = add(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, x, y + 106, 270, 24, ID_XBE, NULL);
        s_form_ctl[n++] = add(L"BUTTON", L"Browse...", WS_TABSTOP, x + 276, y + 105, 80, 26, ID_XBE_BROWSE, NULL);
        s_form_ctl[n++] = s_note = add(L"STATIC", L"Or drop the file on this window.", SS_LEFT | SS_ENDELLIPSIS,
                                       x, y + 133, 356, 16, ID_NOTE, s_note_f);
        s_form_ctl[n++] = add(L"STATIC", L"Install folder", 0, x, y + 156, 300, 16, ID_L_DEST, NULL);
        s_form_ctl[n++] = s_dest = add(L"EDIT", L"C:\\Games\\Nightfire", ES_AUTOHSCROLL | WS_TABSTOP,
                                       x, y + 174, 270, 24, ID_DEST, NULL);
        s_form_ctl[n++] = add(L"BUTTON", L"Change...", WS_TABSTOP, x + 276, y + 173, 80, 26, ID_DEST_BROWSE, NULL);
        s_form_ctl[n++] = s_shortcut = add(L"BUTTON", L"Desktop shortcut", BS_AUTOCHECKBOX | WS_TABSTOP,
                                           x, y + 206, 200, 20, ID_SHORTCUT, NULL);
        SendMessageW(s_shortcut, BM_SETCHECK, BST_CHECKED, 0);
        s_form_ctl[n++] = s_space = add(L"STATIC", L"", SS_LEFT, x, y + 244, 200, 16, ID_SPACE, s_note_f);
        s_go = add(L"BUTTON", L"Install", BS_OWNERDRAW | WS_TABSTOP, PANEL_X + PANEL_W - 16 - 100, y + 234, 100, 32, ID_GO, NULL);

        s_step_w = add(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, x, y + 96, 356, 18, ID_STEP, s_btn_f);
        s_bar = CreateWindowExW(0, PROGRESS_CLASSW, NULL, WS_CHILD | PBS_SMOOTH,
                                px(x), px(y + 122), px(356), px(16), h, NULL, s_inst, NULL);
        SendMessageW(s_bar, PBM_SETRANGE32, 0, 1000);
        s_detail_w = add(L"STATIC", L"", SS_LEFT, x, y + 146, 356, 80, ID_DETAIL, s_note_f);

        s_status = CreateWindowExW(0, STATUSCLASSNAMEW, NULL, WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, h, NULL, s_inst, NULL);
        SendMessageW(s_status, WM_SETFONT, (WPARAM)s_font, TRUE);
        pads[0] = px(WIN_W - 110);
        pads[1] = -1;
        SendMessageW(s_status, SB_SETPARTS, 2, (LPARAM)pads);

        /* An earlier install: its folder, and its game files. */
        if (RegGetValueW(HKEY_CURRENT_USER, REG_KEY, L"InstallDir", RRF_RT_REG_SZ, NULL, dir, &size) == ERROR_SUCCESS) {
            wchar_t p[MAX_PATH];
            path_join(p, dir, L"build\\Release\\nightfire.exe");
            if (exists(p)) {
                s_installed = 1;
                SetWindowTextW(s_dest, dir);
                path_join(p, dir, L"game\\default.xbe");
                if (exists(p))
                    game_pick(p);
            }
        }
        show_state(S_FORM);
        form_texts();
        DragAcceptFiles(h, TRUE);
        return 0;
    }
    case WM_ERASEBKGND: {
        RECT r;
        GetClientRect(h, &r);
        FillRect((HDC)w, &r, GetSysColorBrush(COLOR_BTNFACE));
        return 1;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        paint(dc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        int id = GetDlgCtrlID((HWND)l);
        SetBkColor((HDC)w, k_panel);
        SetTextColor((HDC)w, id == ID_NOTE && !s_game.ok && s_game.dir[0] ? k_red
                             : id == ID_NOTE || id == ID_SPACE || id == ID_DETAIL ? k_grey : RGB(30, 30, 30));
        return (LRESULT)s_panel_br;
    }
    case WM_DRAWITEM:
        if (w == ID_GO) {
            draw_go((const DRAWITEMSTRUCT *)l);
            return TRUE;
        }
        break;
    case WM_DROPFILES: {
        wchar_t f[MAX_PATH];
        if (s_state == S_FORM && DragQueryFileW((HDROP)w, 0, f, MAX_PATH))
            game_pick(f);
        DragFinish((HDROP)w);
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case ID_XBE_BROWSE:
            browse_xbe();
            return 0;
        case ID_DEST_BROWSE:
            browse_dest();
            return 0;
        case ID_XBE:
            if (HIWORD(w) == EN_KILLFOCUS) {
                wchar_t f[MAX_PATH];
                GetWindowTextW(s_xbe, f, MAX_PATH);
                if (f[0] && _wcsicmp(f, s_game.dir))
                    game_pick(f);
            }
            return 0;
        case ID_DEST:
            if (HIWORD(w) == EN_CHANGE)
                space_update();
            return 0;
        case ID_GO:
            if (s_state == S_FORM)
                start_install();
            else if (s_state == S_RUN)
                cancel_install();
            else if (s_state == S_DONE) {
                wchar_t exe[MAX_PATH];
                path_join(exe, s_dir, L"Nightfire Launcher.exe");
                ShellExecuteW(h, L"open", exe, NULL, s_dir, SW_SHOWNORMAL);
                DestroyWindow(h);
            } else {
                show_state(S_FORM);
                form_texts();
            }
            return 0;
        }
        break;
    case WM_TIMER:
        tick();
        return 0;
    case WM_SETUP_DONE:
        install_done((int)w);
        return 0;
    case WM_SIZE:
        SendMessageW(s_status, WM_SIZE, 0, 0);
        return 0;
    case WM_CLOSE:
        if (s_state == S_RUN) {
            if (MessageBoxW(h, L"Cancel the installation?", L"Nightfire Setup", MB_YESNO | MB_ICONQUESTION) == IDYES)
                cancel_install();
            return 0;
        }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static HFONT font(int size, int weight)
{
    return CreateFontW(-px(size), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdline, int show)
{
    WNDCLASSEXW wc;
    INITCOMMONCONTROLSEX ic = { sizeof ic, ICC_PROGRESS_CLASS | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES };
    RECT wr = { 0, 0, 0, 0 };
    DWORD st = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    HANDLE one;
    HDC dc;
    MSG msg;
    wchar_t **argv;
    int argc, i, go = 0, status_h;

    (void)prev; (void)cmdline;
    s_inst = inst;
    one = CreateMutexW(NULL, FALSE, L"NightfireSetup");
    if (one && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(L"NfSetup", NULL);
        if (other)
            SetForegroundWindow(other);
        return 0;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    InitCommonControlsEx(&ic);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    InitializeCriticalSection(&s_cs);

    dc = GetDC(NULL);
    s_dpi = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(NULL, dc);
    s_font = font(12, FW_NORMAL);
    s_title_f = font(15, FW_SEMIBOLD);
    s_note_f = font(11, FW_NORMAL);
    s_btn_f = font(12, FW_SEMIBOLD);
    s_panel_br = CreateSolidBrush(k_panel);
    banner_make();

    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = win_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"NfSetup";
    wc.hIcon = make_icon(GetSystemMetrics(SM_CXICON));
    wc.hIconSm = make_icon(GetSystemMetrics(SM_CXSMICON));
    RegisterClassExW(&wc);

    status_h = 22;
    wr.right = px(WIN_W);
    wr.bottom = px(PANEL_Y + PANEL_H + 12 + status_h);
    AdjustWindowRect(&wr, st, FALSE);
    CreateWindowExW(0, L"NfSetup", L"Nightfire Setup", st, CW_USEDEFAULT, CW_USEDEFAULT,
                    wr.right - wr.left, wr.bottom - wr.top, NULL, NULL, inst, NULL);
    if (!s_win)
        return 1;

    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (i = 1; argv && i < argc; i++) {
        if (!_wcsicmp(argv[i], L"/xbe") && i + 1 < argc)
            game_pick(argv[++i]);
        else if (!_wcsicmp(argv[i], L"/dir") && i + 1 < argc)
            SetWindowTextW(s_dest, argv[++i]);
        else if (!_wcsicmp(argv[i], L"/go"))
            go = 1;
        else if (!_wcsicmp(argv[i], L"/test"))
            s_test = 1;
        else if (argc == 2)
            game_pick(argv[i]);        /* a file dropped on the exe */
    }
    if (argv)
        LocalFree(argv);
    if (s_test)
        SendMessageW(s_shortcut, BM_SETCHECK, BST_UNCHECKED, 0);
    form_texts();
    ShowWindow(s_win, show);
    UpdateWindow(s_win);
    if (go) {
        s_auto = 1;
        start_install();
    }

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(s_win, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (s_job)
        CloseHandle(s_job);
    CoUninitialize();
    return s_exit;
}
