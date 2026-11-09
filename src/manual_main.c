/**
 * Manual function overrides for default.xbe (nightfire.exe)
 *
 * Addresses here are default.xbe's; Driving.xbe has its own file
 * (manual_driving.c), since the same sub_XXXXXXXX name is a different
 * function in each executable.
 *
 * scripts/recompile.ps1 passes this file to the lifter as --exclude-manual:
 * every `void sub_XXXXXXXX(void)` defined here is left out of the generated
 * code, and this definition is the one that links. An override that wraps
 * the generated body instead declares `extern void sub_XXXXXXXX_gen(void);`.
 *
 * recomp_lookup_manual() is consulted before the generated dispatch table on
 * every indirect call, for the functions that are only reached that way.
 *
 * Two kinds of override live here, and scripts/check_original.ps1 (run by
 * build.ps1) tells them apart. A fix that makes the game play as on the
 * console runs in both modes and says so in its body: a comment
 * "both modes: <why>". An enhancement asks nf_enhanced() (main.c) before it
 * writes anything, and in the original mode calls only the title's code.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "recomp_types.h"
#include "recomp_funcs.h"

typedef void (*recomp_func_t)(void);

/* Reached only through the dispatch table: empty stubs, see
 * recomp_lookup_manual(). Direct calls still reach the generated sub_*, so
 * these carry names of their own (the same name as static was an error
 * outside MSVC, the generated one being declared in recomp_funcs.h). */
static void dispatch_0011A230(void)
{
    /* both modes: a hand translation the console needs, not an enhancement */
}

static void dispatch_0011A23B(void)
{
    /* both modes: a hand translation the console needs, not an enhancement */
}

static void dispatch_00114B94(void)
{
    /* both modes: a hand translation the console needs, not an enhancement */
    /* Original:
       MOV [0x0012FBC0], 0x0012F4EC
       RET
    */
}

static void dispatch_00114B9F(void)
{
    /* both modes: a hand translation the console needs, not an enhancement */
    /* Original:
       MOV [0x0012FBBC], 0x0012F4F0
       RET
    */
}

static float manual_i32_float(uint32_t state, uint32_t offset)
{
    int32_t value = (int32_t)MEM32(state + offset);
    float result = (float)value;

    if (value < 0)
        result += MEMF(0x0015D3F0);
    return result;
}

static void manual_00100610(uint32_t state, float *first, float *second)
{
    float x = manual_i32_float(state, 0xA90) * MEMF(state + 0x518)
            + MEMF(state + 0xAA8);
    float y = manual_i32_float(state, 0xA94) * MEMF(state + 0x51C)
            + MEMF(state + 0xAAC);
    float width = manual_i32_float(state, 0xA98) * MEMF(state + 0x518)
                * MEMF(0x0015D320);
    float height = manual_i32_float(state, 0xA9C) * MEMF(state + 0x51C)
                 * MEMF(0x0015D4F4);

    if ((MEM32(state + 8) & 0x8000) && MEM32(0x00111C30)) {
        x -= MEMF(0x0015D320);
        y -= MEMF(0x0015D320);
    }

    first[0] = width + x;
    first[1] = y - height;
    first[2] = MEMF(state + 0xAA0) * MEMF(state + 0x510);
    first[3] = 0.0f;
    second[0] = width;
    second[1] = height;
    second[2] = (MEMF(state + 0xAA4) - MEMF(state + 0xAA0))
              * MEMF(state + 0x510);
    second[3] = 0.0f;
}

static uint32_t manual_00101180(uint32_t state, uint32_t command)
{
    uint32_t flags = MEM32(MEM32(state + 0x380) + 4);
    uint32_t value1, value2;

    if ((flags & 0x12) == 0) {
        float x = manual_i32_float(state, 0xA90) * MEMF(state + 0x518)
                + MEMF(state + 0xAA8);
        float y = manual_i32_float(state, 0xA94) * MEMF(state + 0x51C)
                + MEMF(state + 0xAAC);

        if ((MEM32(state + 8) & 0x8000) && MEM32(0x00111C30)) {
            x -= MEMF(0x0015D320);
            y -= MEMF(0x0015D320);
        }

        MEM32(command) = 0x00100A20;
        MEMF(command + 4) = x;
        MEMF(command + 8) = y;
        MEM32(command + 12) = 0;
        MEM32(command + 16) = 0;
        command += 20;

        if ((MEM8(state + 0x23B8) & 1) == 0) {
            if (MEM32(0x00111C0C) == 2) {
                MEM32(state + 0x23AC) = MEM32(state + 0x510);
                MEMF(state + 0x23A8) = MEMF(state + 0x50C)
                    * MEMF(state + 0x504) * MEMF(state + 0x510);
            } else {
                MEMF(state + 0x23A8) = MEMF(state + 0xAA0)
                    * MEMF(state + 0x510);
                MEMF(state + 0x23AC) = MEMF(state + 0xAA4)
                    * MEMF(state + 0x510);
            }
        }
        value1 = MEM32(state + 0x23AC);
        value2 = MEM32(state + 0x23A8);
    } else {
        float first[4], second[4];

        manual_00100610(state, first, second);
        if ((MEM32(state + 8) & 0x200) == 0) {
            MEM32(command) = 0x00100A20;
            MEMF(command + 4) = first[0];
            MEMF(command + 8) = first[1];
            MEMF(command + 12) = first[2];
            MEMF(command + 16) = first[3];
            MEM32(command + 20) = 0x00100AF0;
            MEMF(command + 24) = second[0];
            MEMF(command + 28) = second[1];
            MEMF(command + 32) = second[2];
            MEMF(command + 36) = second[3];
            command += 40;
        }

        if ((MEM8(state + 0x23B8) & 2) == 0) {
            if (MEM32(0x00111C0C) == 2) {
                MEM32(state + 0x23B4) = MEM32(state + 0x510);
                MEMF(state + 0x23B0) = MEMF(state + 0x50C)
                    * MEMF(state + 0x504) * MEMF(state + 0x510);
            } else {
                MEM32(state + 0x23B0) = 0;
                MEM32(state + 0x23B4) = MEM32(state + 0x510);
            }
        }
        value1 = MEM32(state + 0x23B4);
        value2 = MEM32(state + 0x23B0);
    }

    MEM32(command) = 0x00080394;
    MEM32(command + 4) = value2;
    MEM32(command + 8) = value1;
    return command + 12;
}

void sub_00101180(void)
{
    /* both modes: a hand translation the console needs, not an enhancement */
    static int trace_enabled = -1;
    if (trace_enabled < 0)
        trace_enabled = getenv("RECOMP_TRACE_MANUAL") != NULL;
    if (trace_enabled)
        fprintf(stderr, "[MANUAL] 00101180 enter esp=%08X ecx=%08X edx=%08X\n",
                g_esp, g_ecx, g_edx);
    uint32_t command = manual_00101180(g_ecx, g_edx);
    g_eax = command;
    g_esp += 4;
    if (trace_enabled) {
        fprintf(stderr, "[MANUAL] 00101180 exit esp=%08X eax=%08X\n",
                g_esp, g_eax);
        fflush(stderr);
    }
}

void sub_001067C9(void)
{
    /* both modes: a hand translation the console needs, not an enhancement */
    g_edi = MEM32(g_esp);
    g_esp += 4;
    g_esi = MEM32(g_esp);
    g_esp += 4;
    g_esp += 8;
    g_esp += 12;
}

void sub_001019F0(void)
{
    /* both modes: a hand translation the console needs, not an enhancement */
    static int trace_enabled = -1;
    uint32_t entry_esp = g_esp;
    uint32_t param1 = MEM32(entry_esp + 4);
    uint32_t param2 = MEM32(entry_esp + 8);
    uint32_t param3 = MEM32(entry_esp + 12);
    uint32_t state = MEM32(0x001117C0);
    uint32_t command;

    if (trace_enabled < 0)
        trace_enabled = getenv("RECOMP_TRACE_MANUAL") != NULL;
    if (trace_enabled) {
        fprintf(stderr, "[MANUAL] 001019F0 enter esp=%08X args=%08X,%08X,%08X state=%08X\n",
                entry_esp, param1, param2, param3, state);
        fflush(stderr);
    }

    switch (param3) {
    case 1:
        MEM32(state + 0x8EC) = param1;
        MEM32(state + 0x8ED) = param2;
        MEM32(state + 0x8EE) |= 2;
        break;
    case 2:
        MEM32(state + 0x8EB) = param2;
        MEM32(state + 0x8EA) = param1;
        MEM32(state + 0x8EE) |= 1;
        break;
    case 3:
        MEM32(state + 0x8EE) &= ~2u;
        break;
    case 4:
        MEM32(state + 0x8EE) &= ~1u;
        break;
    default:
        break;
    }

    command = MEM32(state);
    if (MEM32(state + 4) <= command) {
        g_esp -= 4;
        MEM32(g_esp) = 0x00101A75;
        sub_00106910();
        command = g_eax;
    }

    g_ecx = state;
    g_edx = command;
    g_esp -= 4;
    MEM32(g_esp) = 0x00101A75;
    sub_00101180();
    MEM32(state) = g_eax;
    g_esp += 16;
    if (trace_enabled) {
        fprintf(stderr, "[MANUAL] 001019F0 exit esp=%08X eax=%08X\n",
                g_esp, g_eax);
        fflush(stderr);
    }
}

/* D3D device init (sub_00105AC0) allocates the fence semaphore, stores it at
 * device+0x34 and calls this function next (its only caller), with the device
 * in ecx, before it writes its first command. Registering the semaphore here
 * means the executor has the target before the first
 * BACK_END_WRITE_SEMAPHORE_RELEASE.
 *
 * main.c's watcher thread used to be the only registration, and it polls: in
 * about half the runs the first four releases came first and were dropped, the
 * word never got the values Present's frame wait (sub_00106650 from
 * sub_00103730) looks for, and the title spun there for good -- black screen,
 * 0 draws, during the second intro movie. The thread stays, for a device that
 * is created again. */
void sub_00106220_enter(void)
{
    /* both modes: the fence target D3D needs from the first command on */
    extern void nv2a_pb_set_semaphore_target(uint32_t guest_va);
    uint32_t sem = MEM32(g_ecx + 0x34);

    if (sem)
        nv2a_pb_set_semaphore_target(sem);
}

/* Present (sub_00103730, stdcall, one flags argument; 0 means 5) keeps two
 * frames in flight. After each swap sub_001036F0 inserts a fence and stores
 * it in a ring of two, [device+0x2124 + (count & 1) * 4], count being the
 * frame count [device+0x2C10] already bumped for that swap. Before the next
 * swap Present waits (sub_00106650, D3D's fence wait) on the other slot: the
 * fence after the swap two frames back. So the title builds a frame while
 * the two before it still wait for their vblanks, and what it reads from the
 * input reaches the screen two frames later -- ~47 ms from the look step to
 * the flip in The Exchange, against ~12 ms on the driving guns, where a
 * frame takes the whole 20 ms and nothing queues up.
 *
 * The enhanced mode keeps no frame in flight: after the swap it waits, the
 * same way, on the fence after that swap, so the next frame reads its input
 * once this one is on screen: ~16 ms to the flip at a steady 50 fps. Only in
 * the cases where Present waits at all (device flag 0x4000 with flags & 3,
 * else flags & 4). The original mode is the title's Present alone. */
extern void sub_00103730_gen(void);
extern void sub_00106650(void);
int nf_enhanced(void);  /* main.c */

void sub_00103730(void)
{
    uint32_t dev = 0, flags, fence, eax;
    int waits = 0;

    if (nf_enhanced() && (dev = MEM32(0x1117C0)) != 0) {
        flags = MEM32(g_esp + 4);
        if (!flags)
            flags = 5;
        waits = (MEM32(dev + 8) & 0x4000) ? (flags & 3) != 0 : (flags & 4) != 0;
    }
    sub_00103730_gen();
    if (waits) {
        fence = MEM32(dev + 0x2124 + (MEM32(dev + 0x2C10) & 1) * 4);
        if (fence) {
            eax = g_eax;
            g_esp -= 12;               /* sub_00106650(fence, 1), ret 8 */
            MEM32(g_esp + 8) = 1;
            MEM32(g_esp + 4) = fence;
            MEM32(g_esp) = 0x00103730;
            sub_00106650();
            g_eax = eax;               /* Present's own result */
        }
    }
}

/* Direct mouse look, the way the PC version turns: each frame the view turns
 * by the mouse counts since the last frame (sensitivity * 0.022 degrees per
 * count), with no stick, deadzone or smoothing in between. The pad keeps its
 * own path untouched.
 *
 * sub_000B7610 is the per-frame look step for the player entity in [esp+4];
 * its control block is [entity+0xBC]. In it the title's own manual aim moves
 * the view the same way this does: it subtracts from the frame's yaw step
 * [ctrl+0x28] (radians, positive turns left) and adds to the pitch
 * [ctrl+0x838] (+-1 for +-90 degrees, clamped by sub_000B7A50 right after),
 * both divided by the zoom [ctrl+0x860]. Movement modes 8-12 and 16 (no free
 * look) skip it, as the title does. Player 1 only: [ctrl+0x8DE] is the pad.
 *
 * The mouse owns the view from its last movement until pad 1's right stick
 * (0x2FF4C0/0x2FF4C4, as sub_000E76A0 scales it; the mouse no longer drives
 * it) moves again. */
extern void sub_000B7610_gen(void);
extern void sub_000AB180_gen(void);
int mkb_mouse_look(double *yaw_deg, double *pitch_deg);  /* xboxrecomp mkb_input.h */
void mkb_title_aiming(void);                               /* xboxrecomp mkb_input.h */
int nf_enhanced(void);  /* main.c: 0 in the original mode, where every
                         * enhancement below calls the title's code only */

static uint32_t s_mouse_look_ctrl;   /* control block the mouse owns, 0 none */
/* Whether player 1's right mouse button aims (weapon_scoped), read below while the title is using
 * the entity. The entity itself is not kept: it is freed when a level section
 * ends, and the input hook runs after that with nothing left to read. */
static int s_player_scoped;

/* Weightless (Equinox): modes 8 and 9 (sub_000AA6F0) have no pitch -- it
 * zeroes [ctrl+0x838] -- and turn the whole body instead, by the yaw step
 * [ctrl+0x28] and a pitch step [ctrl+0x24] it sets each frame straight from
 * the look commands (sub_0006CA20 hands their raw values back): -k * command
 * 0 and 5, k = [0x17C104] * [0x15D3A8] * [0x15D590]; while aiming
 * ([weapon+0x50] bit 0) k * [0x15D4B8] / zoom * command 3 and 4. The title's
 * look skips these modes and the steps are spent before it runs, so the
 * mouse, gathered there, goes into the commands the next frame reads. */
static double s_space_yaw, s_space_pitch, s_space_zoom = 1.0;
static int s_space_aim;

static void space_command(int command, float v)
{
    MEMF(0x1FE6D0 + 0x14 + command * 4) += v;
    MEM8(0x1FE6D0 + 0x104 + command) |= 1;
}

static void space_look(void)
{
    double k = (double)MEMF(0x17C104) * MEMF(0x15D3A8) * MEMF(0x15D590);

    if ((s_space_yaw != 0.0 || s_space_pitch != 0.0) && k != 0.0) {
        if (s_space_aim) {
            k *= MEMF(0x15D4B8) / s_space_zoom;
            space_command(3, (float)(-s_space_yaw / k));
            space_command(4, (float)(-s_space_pitch / k));
        } else {
            space_command(0, (float)(s_space_yaw / k));
            space_command(5, (float)(s_space_pitch / k));
        }
    }
    s_space_yaw = s_space_pitch = 0.0;
}

/* Whether the right mouse button aims (0x13) rather than fires the other way
 * (0x0C) for the item in hand: a weapon with a scope (flags 0x40 at +0x68 of
 * its type's entry, 0x18CFA0 + type * 0x10C), or a gadget (types from 0x36
 * on: the camera lighter's second function is its manual aim, the other
 * gadgets have none, and the alternative fire would use them). */
static int weapon_scoped(uint32_t entity)
{
    uint32_t weapon = MEM32(entity + 0xB8);
    int type;

    if (!weapon)
        return 0;
    type = (int8_t)MEM8(weapon + 0x52);
    if (type >= 0x36)
        return 1;
    return type >= 0 && (MEM32(0x18CFA0 + type * 0x10C + 0x68) & 0x40) != 0;
}

void sub_000B7610(void)
{
    uint32_t entity = MEM32(g_esp + 4);
    uint32_t ctrl;
    int16_t mode;
    double yaw, pitch, zoom;
    float p;

    sub_000B7610_gen();
    if (!nf_enhanced())
        return;

    if (!entity || !(ctrl = MEM32(entity + 0xBC)) || (int8_t)MEM8(ctrl + 0x8DE) != 0)
        return;
    s_player_scoped = weapon_scoped(entity);
    /* Aiming (the item's [+0x50] bit 0, set by the title's manual aim, the
     * scope and the lighter's camera too): the wheel zooms by itself. */
    if (MEM32(entity + 0xB8) && (MEM8(MEM32(entity + 0xB8) + 0x50) & 1))
        mkb_title_aiming();
    if (fabsf(MEMF(0x2FF4C0)) > 0.05f || fabsf(MEMF(0x2FF4C4)) > 0.05f)
        s_mouse_look_ctrl = 0;
    mode = (int16_t)MEM16(entity + 0xD2);
    if (mode >= 10 && (mode <= 12 || mode == 16))
        return;
    if (!mkb_mouse_look(&yaw, &pitch)) {
        s_mouse_look_ctrl = 0;
        return;
    }
    if (yaw != 0.0 || pitch != 0.0)
        s_mouse_look_ctrl = ctrl;
    zoom = MEMF(ctrl + 0x860);
    if (!(zoom >= 1.0))
        zoom = 1.0;
    /* Weightless (Equinox): space_look below. */
    if (mode == 8 || mode == 9) {
        s_space_yaw += yaw * (3.14159265358979 / 180.0) / zoom;
        s_space_pitch += pitch * (3.14159265358979 / 180.0) / zoom;
        s_space_aim = MEM32(entity + 0xB8) && (MEM8(MEM32(entity + 0xB8) + 0x50) & 1);
        s_space_zoom = zoom;
        return;
    }
    /* Turning is the mode's own: of the handlers the player update calls by
     * mode (jump table 0xADCA4), only those of 0-4 (through sub_000A9CF0)
     * and 5 set the yaw
     * step from the turn command. The others -- hanging on a rope in Chain
     * Reaction among them -- ignore the stick, and a turn there would only
     * pull the player off the climb. */
    if (mode >= 0 && mode <= 5)
        MEMF(ctrl + 0x28) -= (float)(yaw * (3.14159265358979 / 180.0) / zoom);
    p = MEMF(ctrl + 0x838) + (float)(pitch / 90.0 / zoom);
    MEMF(ctrl + 0x838) = p > 1.0f ? 1.0f : p < -1.0f ? -1.0f : p;
}

/* Look spring: once the look input stops ([ctrl+0xF0] 2 or 3), this pulls
 * the pitch [ctrl+0x838] 7.5% a frame toward [ctrl+0x83C], the middle of the
 * screen. Right for a stick that springs back to centre, wrong for a mouse,
 * which stays where it was put: while the mouse owns the view the pitch keeps
 * its value, as with the PC version's lookspring off. */
void sub_000AB180(void)
{
    uint32_t ctrl = g_ecx;
    float pitch;

    if (!nf_enhanced()) {
        sub_000AB180_gen();
        return;
    }
    pitch = MEMF(ctrl + 0x838);
    sub_000AB180_gen();
    if (ctrl && ctrl == s_mouse_look_ctrl)
        MEMF(ctrl + 0x838) = pitch;
}

/* Auto aim: sub_000B9950 picks the enemy nearest the crosshair inside a cone
 * and aims at it, when the profile's auto aim option is on (the player's
 * option byte at 0x1FE6D1, 0x1FE6D2 in multiplayer, [0x260018] set) and
 * manual aim is not held. A mouse aims by itself, as on the PC: while it
 * owns the view the option reads off for the call. Weapons that always lock
 * on (0x50, 0x51) do so regardless, as before. */
extern void sub_000B9950_gen(void);

void sub_000B9950(void)
{
    uint32_t entity = MEM32(g_esp + 4);
    uint32_t ctrl = entity ? MEM32(entity + 0xBC) : 0;
    uint32_t option;
    uint8_t keep;

    if (!nf_enhanced() || !ctrl || ctrl != s_mouse_look_ctrl) {
        sub_000B9950_gen();
        return;
    }
    option = 0x1FE6D0 + (int8_t)MEM8(ctrl + 0x8DE) * 0x158 + (MEM32(0x260018) ? 2 : 1);
    keep = MEM8(option);
    MEM8(option) = 0;
    sub_000B9950_gen();
    MEM8(option) = keep;
}

/* Keyboard movement as its own scheme. sub_0006CF50 fills the command table
 * every frame from the pads, through the player's control scheme: four
 * entries of 0x158 bytes from 0x1FE6D0, commands as floats from +0x14.
 * Command 0 turns (sub_000A9CF0 makes it the yaw step [ctrl+0x28]), 1
 * strafes (+ left, into [ctrl+0x0C]), 2 walks (+ forward, [ctrl+0x14]).
 * Which stick lands on which depends on the scheme -- in "Classic Bond" the
 * left stick turns -- so the PC's +forward/+back/+moveleft/+moveright
 * (mkb.ini) go straight to commands 2 and 1 of player 1's entry, the
 * first, on top of what the pad gave. A command also has a byte at
 * +0x104 + command whose bit 0 says it has input this frame; sub_0006CA20
 * reads 0 without it, so a key sets it as the pad's stick does. */
extern void sub_0006CF50_gen(void);
int mkb_action(const char *name);  /* xboxrecomp mkb_input.h */

static void mkb_command(int command, float keys)
{
    uint32_t va = 0x1FE6D0 + 0x14 + command * 4;
    float v = MEMF(va) + keys;
    MEMF(va) = v > 1.0f ? 1.0f : v < -1.0f ? -1.0f : v;
    MEM8(0x1FE6D0 + 0x104 + command) |= 1;
}

/* The keyboard's buttons as their own scheme. sub_000DE720 turns each pad
 * into commands through the player's control scheme, and after it
 * sub_0006CF50 makes the press edges, the gates and the menus' repeat from
 * the values. The keys write the values a pad button leaves there, after
 * sub_000DE720 and before all that, so a key behaves as its button does,
 * but each key gives one command: on the pad D-pad up is the menu's up
 * (0x1A), the next gadget (0x0B) and the zoom (0x15) at once.
 *
 * Who reads what: the menus 0x1A-0x1D up/down/left/right, the pause menu
 * 0x1E start, 0x1F accept, 0x22 back (sub_00072E20); 0x19 skips a movie
 * (Start, A or B; the fire button gives it too while no menu is open, so a
 * click skips as A does -- in a menu it cuts the menu's own animations
 * short, sub_00094370 -> sub_0007FEA0, and leaves a sliding pause menu half
 * way); 0x20/0x21 are the menus' Y and X.
 * In play: 0x07 jump,
 * 0x08 crouch, 0x09 fire, 0x0E use/reload, 0x0D vision (sub_000B4EC0), 0x0F
 * and 0x10 previous and next weapon, 0x0A and 0x0B previous and next gadget,
 * 0x0C alternative fire (sub_000BA9EC), 0x13 manual aim. sub_000DE720 makes
 * D-pad up and down (0x15, 0x14) the zoom axis 0x06 = 0x14 - 0x15 and
 * clears them, so the zoom keys write that axis. */
static const struct { const char *action; uint8_t cmd[2]; } k_mkb_cmds[] = {
    { "attack", { 0x09, 0xFF } }, { "aim", { 0x13, 0xFF } }, { "jump", { 0x07, 0xFF } },
    { "duck", { 0x08, 0xFF } }, { "use", { 0x0E, 0xFF } }, { "vision", { 0x0D, 0xFF } },
    { "nextweapon", { 0x10, 0xFF } }, { "prevweapon", { 0x0F, 0xFF } },
    { "nextgadget", { 0x0B, 0xFF } }, { "prevgadget", { 0x0A, 0xFF } },
    { "pause", { 0x1E, 0x19 } }, { "menu_accept", { 0x1F, 0x19 } }, { "menu_back", { 0x22, 0x19 } },
    { "menu_up", { 0x1A, 0xFF } }, { "menu_down", { 0x1B, 0xFF } },
    { "menu_left", { 0x1C, 0xFF } }, { "menu_right", { 0x1D, 0xFF } },
    { "menu_y", { 0x20, 0xFF } }, { "menu_x", { 0x21, 0xFF } },
};

/* The menus with the mouse. The frontend runs up to five windows of 0x1D8
 * bytes from 0x25F1D0 (sub_00094370): +0x1B4 counts its use, +0x1D1 is set
 * while it is open, and a window takes input unless the one before it is
 * open too. Its menu at [+0x1BC] lists elements from [menu+0x94] less
 * [menu+0xA0] * 4, each linked by its first dword, with a rectangle at
 * +0x70..+0x76 (x, y, w, h, relative to the menu's own +0x70/+0x72), +0x7A
 * set and +0x7B 0x0D for one that cannot be chosen. The chosen element is
 * whichever holds a point, the window's pointer at [+0x94] +0x24/+0x26
 * (sub_00073370); the D-pad moves that point from element to element
 * (sub_000734D0). So the mouse sets the point when it moves over an element,
 * and a left click there is the accept (0x1F). */
#define MENU_WINDOW(i) (0x25F1D0 + (uint32_t)(i) * 0x1D8)
int xbox_FramebufferWindowPointer(int *x, int *y);   /* xboxrecomp video_player.h */
void mkb_pointer_show(void);                         /* xboxrecomp mkb_input.h */
int mkb_pointer_click(void);

static int menus_open(void)
{
    int i;

    for (i = 0; i < 5; i++)
        if (MEM32(MENU_WINDOW(i) + 0x1B4))
            return 1;
    return 0;
}

static uint32_t menu_element_at(uint32_t win, int x, int y)
{
    uint32_t menu = MEM32(win + 0x1BC), e;
    int rx, ry, n;

    if (!menu || !(e = MEM32(menu + 0x94)))
        return 0;
    e -= MEM32(menu + 0xA0) * 4;
    rx = x - (int16_t)MEM16(menu + 0x70);
    ry = y - (int16_t)MEM16(menu + 0x72);
    for (n = 0; e && n < 512; e = MEM32(e), n++) {
        int ex = (int16_t)MEM16(e + 0x70), ey = (int16_t)MEM16(e + 0x72);
        if (rx > ex && rx < ex + (int16_t)MEM16(e + 0x74) && ry > ey && ry < ey + (int16_t)MEM16(e + 0x76)
                && !MEM8(e + 0x7A) && MEM8(e + 0x7B) != 0x0D)
            return e;
    }
    return 0;
}

/* The windows that take input (sub_00094370): open, the one before them not. */
static int menu_takes_input(int i)
{
    uint32_t win = MENU_WINDOW(i);
    return MEM8(win + 0x1D1) && !(i && MEM8(win - 7));
}

/* 2: something to choose; 1: only an element as large as the screen (the
 * title picture, any key to go on); 0: nothing. */
static int menu_choosable(int i)
{
    uint32_t menu = MEM32(MENU_WINDOW(i) + 0x1BC), e;
    int n, found = 0;

    if (!menu || !(e = MEM32(menu + 0x94)))
        return 0;
    e -= MEM32(menu + 0xA0) * 4;
    for (n = 0; e && n < 512; e = MEM32(e), n++) {
        int w = (int16_t)MEM16(e + 0x74), h = (int16_t)MEM16(e + 0x76);
        if (MEM8(e + 0x7A) || MEM8(e + 0x7B) == 0x0D || w <= 0 || h <= 0)
            continue;
        if (w < 600 || h < 440)
            return 2;
        found = 1;
    }
    return found;
}

/* The pointer shows once the mouse moves over a menu that has something to
 * choose, and stays until the menus close: not over a loading screen or a
 * title card, and not while the mouse rests. */
static int s_menu_wx[5], s_menu_wy[5];   /* the mouse where it last set each pointer */

/* Menus of rows (the options): the rows are texts (type 5) that cannot be
 * chosen themselves; a value chooser (type 10, sub_00097BE0) steps through
 * them with up and down, and the row it stands on shows in its style word
 * at +0x94, 0x64FC03FF against 0x64FC02FF for the others. */
typedef struct { uint32_t e; int x, y, w, h; } MenuRow;

static int menu_rows(uint32_t win, MenuRow *row, int cap, int *sel)
{
    uint32_t menu = MEM32(win + 0x1BC), e;
    int n = 0, i, j, chosen = 0, mx, my;

    *sel = -1;
    if (!menu || !(e = MEM32(menu + 0x94)))
        return 0;
    e -= MEM32(menu + 0xA0) * 4;
    mx = (int16_t)MEM16(menu + 0x70);
    my = (int16_t)MEM16(menu + 0x72);
    for (i = 0; e && i < 512 && n < cap; e = MEM32(e), i++) {
        uint8_t b = MEM8(e + 0x95);
        int h = (int16_t)MEM16(e + 0x76);
        if (MEM8(e + 0x7B) != 5 || (MEM32(e + 0x94) & 0xFFFF00FFu) != 0x64FC00FFu || (b != 2 && b != 3)
                || h <= 0 || h > 40)
            continue;
        row[n].e = e;
        row[n].x = mx + (int16_t)MEM16(e + 0x70);
        row[n].y = my + (int16_t)MEM16(e + 0x72);
        row[n].w = (int16_t)MEM16(e + 0x74);
        row[n].h = h;
        chosen += b == 3;
        n++;
    }
    for (i = 1; i < n; i++)                       /* top to bottom */
        for (j = i; j > 0 && row[j].y < row[j - 1].y; j--) {
            MenuRow t = row[j];
            row[j] = row[j - 1];
            row[j - 1] = t;
        }
    for (i = 0; i < n && chosen == 1; i++)
        if (MEM8(row[i].e + 0x95) == 3)
            *sel = i;
    return n >= 2 && *sel >= 0 ? n : 0;
}

/* The row under the mouse: within the rows' left and right edges, and up to
 * half the gap to the row above and below. */
static int menu_row_at(const MenuRow *row, int n, int x, int y)
{
    int i, l = row[0].x, r = row[0].x + row[0].w;

    for (i = 1; i < n; i++) {
        if (row[i].x < l)
            l = row[i].x;
        if (row[i].x + row[i].w > r)
            r = row[i].x + row[i].w;
    }
    if (x < l - 24 || x > r + 24)
        return -1;
    for (i = 0; i < n; i++) {
        int top = i ? (row[i - 1].y + row[i - 1].h + row[i].y) / 2 : row[i].y - 4;
        int bottom = i + 1 < n ? (row[i].y + row[i].h + row[i + 1].y) / 2 : row[i].y + row[i].h + 4;
        if (y >= top && y < bottom)
            return i;
    }
    return -1;
}

/* The prompt bar ("~A Select  ~Y Create  ~V Scroll  ~B Back"): one text
 * element (type 5) in the lower part (the pause menu's sits at y 341), its
 * string at [+0xBC], "~" and a letter
 * for a pad button's picture, the line centred on the element. A click on a
 * button or its word gives that button's command. The game measures the line
 * with its font; these widths, in menu units, were measured off the codename
 * menu (a picture 17, a space 4, capitals 9, most small letters 8, the narrow
 * ones less) and put each part within a few units of where it is drawn; the
 * parts meet halfway across the gaps between them. */
static int prompt_char_width(char c)
{
    if (c == ' ')
        return 4;
    if (c == 'i' || c == 'l' || c == 'I' || c == 'j' || c == '.' || c == ',' || c == '\'' || c == '!' || c == ':')
        return 4;
    if (c == 'f' || c == 'r' || c == 't')
        return 6;
    if (c == 'm' || c == 'w' || c == 'M' || c == 'W')
        return 11;
    if (c >= 'A' && c <= 'Z')
        return 9;
    return 8;
}

static uint8_t prompt_command(char button)
{
    switch (button) {
    case 'A': return 0x1F;   /* accept */
    case 'B': return 0x22;   /* back */
    case 'Y': return 0x20;
    case 'X': return 0x21;
    case 'S': return 0x1E;   /* start: continue */
    }
    return 0;
}

/* The command of the prompt under (x, y) in a window's menu, 0 for none. */
static uint8_t menu_prompt_at(uint32_t win, int x, int y)
{
    uint32_t menu = MEM32(win + 0x1BC), e, s;
    int n;

    if (!menu || !(e = MEM32(menu + 0x94)))
        return 0;
    e -= MEM32(menu + 0xA0) * 4;
    for (n = 0; e && n < 512; e = MEM32(e), n++) {
        int ex = (int16_t)MEM16(menu + 0x70) + (int16_t)MEM16(e + 0x70);
        int ey = (int16_t)MEM16(menu + 0x72) + (int16_t)MEM16(e + 0x72);
        int ew = (int16_t)MEM16(e + 0x74), eh = (int16_t)MEM16(e + 0x76);
        char text[128], part_button[16];
        int part_left[16], part_right[16], parts = 0, len, i, width, left;
        if (MEM8(e + 0x7B) != 5 || ey < 300 || y < ey - 4 || y > ey + eh + 4)
            continue;
        s = MEM32(e + 0xBC);
        if (!s || MEM8(s) != '~')
            continue;
        for (len = 0; len + 1 < (int)sizeof text && MEM8(s + len) >= 0x20 && MEM8(s + len) < 0x7F; len++)
            text[len] = (char)MEM8(s + len);
        text[len] = 0;
        width = 0;
        for (i = 0; i < len; i++)
            width += text[i] == '~' && i + 1 < len ? (i++, 17) : prompt_char_width(text[i]);
        left = ex + (ew - width) / 2;
        for (i = 0; i < len && parts < 16; i++) {
            if (text[i] == '~' && i + 1 < len) {
                part_button[parts] = text[i + 1];
                part_left[parts] = left;
                part_right[parts] = left + 17;
                parts++;
                left += 17;
                i++;
                continue;
            }
            left += prompt_char_width(text[i]);
            if (parts && text[i] != ' ')
                part_right[parts - 1] = left;
        }
        for (i = 0; i < parts; i++) {
            int l = i ? (part_right[i - 1] + part_left[i]) / 2 : part_left[i] - 6;
            int r = i + 1 < parts ? (part_right[i] + part_left[i + 1]) / 2 : part_right[i] + 6;
            if (x >= l && x < r)
                return prompt_command(part_button[i]);
        }
    }
    return 0;
}

static int s_menu_accept;   /* a click that began on the chosen row, held */
static uint8_t s_menu_prompt;   /* the command of the prompt clicked, held */

/* Widescreen (enhanced, RECOMP_DISPLAY_ASPECT): the back end squeezes what is
 * drawn over the world to 4:3 widths by this factor. */
void nf_backend_menu_box(int on, int x0, int y0, int x1, int y1);   /* nv2a_d3d11_backend.c */

static float display_squeeze(void)
{
    static float sq = -1.0f;
    if (sq < 0.0f) {
        const char *e = getenv("RECOMP_DISPLAY_ASPECT");
        float a = 0.0f, b = 0.0f;
        sq = 1.0f;
        if (e && sscanf(e, "%f:%f", &a, &b) == 2 && a > 0.0f && b > 0.0f && a / b > 4.0f / 3.0f + 0.01f)
            sq = (4.0f / 3.0f) / (a / b);
    }
    return sq;
}

/* A menu over the world (a window counting 3, the pause menu): its frame, for
 * the back end to squeeze the whole of it about the centre, and for the mouse
 * to undo that. */
static int s_menu_over_world;

static void menu_overlay(void)
{
    int i, n, x0 = 10000, y0 = 10000, x1 = -10000, y1 = -10000, found = 0;

    for (i = 0; i < 5; i++) {
        uint32_t win = MENU_WINDOW(i), menu = MEM32(win + 0x1BC), e;
        int mx, my;
        if (!MEM8(win + 0x1D1) || MEM32(win + 0x1B4) == 5 || !menu || !(e = MEM32(menu + 0x94)))
            continue;
        e -= MEM32(menu + 0xA0) * 4;
        mx = (int16_t)MEM16(menu + 0x70);
        my = (int16_t)MEM16(menu + 0x72);
        for (n = 0; e && n < 512; e = MEM32(e), n++) {
            int ex = mx + (int16_t)MEM16(e + 0x70), ey = my + (int16_t)MEM16(e + 0x72);
            int ew = (int16_t)MEM16(e + 0x74), eh = (int16_t)MEM16(e + 0x76);
            if (ew <= 0 || eh <= 0 || (ew >= 600 && eh >= 440))
                continue;
            if (ex < x0) x0 = ex;
            if (ey < y0) y0 = ey;
            if (ex + ew > x1) x1 = ex + ew;
            if (ey + eh > y1) y1 = ey + eh;
            found = 1;
        }
    }
    s_menu_over_world = found;
    nf_backend_menu_box(found, x0, y0, x1, y1);
}

static void menu_pointer(void)
{
    static int lx, ly, armed, on, was;
    int x, y, i, inside, any = 0, hit = 0, down, press;

    if (!menus_open()) {
        armed = on = 0;
        return;
    }
    for (i = 0; i < 5; i++)
        if (menu_takes_input(i) && menu_choosable(i) > any)
            any = menu_choosable(i);
    if (!any) {
        on = 0;
        return;
    }
    if (any == 1) {
        /* The title picture: no pointer, a click goes on. */
        on = 0;
        was = down = mkb_pointer_click();
        if (down)
            MEMF(0x1FE6E4 + 0x1F * 4) = 1.0f;
        return;
    }
    inside = xbox_FramebufferWindowPointer(&x, &y);
    if (inside && s_menu_over_world && display_squeeze() < 1.0f)
        x = (int)floorf(320.0f + (x - 320) / display_squeeze());
    if (inside) {
        if (armed && (x != lx || y != ly))
            on = 1;
        lx = x;
        ly = y;
        armed = 1;
    }
    if (!on)
        return;
    mkb_pointer_show();
    down = mkb_pointer_click();
    press = down && !was;
    was = down;
    if (!inside)
        return;
    for (i = 0; i < 5; i++) {
        uint32_t win = MENU_WINDOW(i), cur;
        MenuRow row[24];
        int n, sel, k;
        uint8_t prompt;
        if (!menu_takes_input(i))
            continue;
        if ((prompt = menu_prompt_at(win, x, y)) != 0) {
            if (press)
                s_menu_prompt = prompt;
            continue;
        }
        if ((n = menu_rows(win, row, 24, &sel)) != 0) {
            /* Rows move on a click, not under a passing mouse: a click on
             * another row presses down (or up) as many times as it is rows
             * away, one press every other frame so each is an edge; a click
             * on the chosen row accepts it. Counted, not chased: in a
             * carousel (the codenames) the chosen row stays in the middle and
             * the rows turn past it. */
            static int steps, phase;
            k = menu_row_at(row, n, x, y);
            if (press && k >= 0) {
                if (k == sel)
                    s_menu_accept = 1;
                else
                    steps = k - sel;
            }
            if (steps && (phase ^= 1)) {
                MEMF(0x1FE6E4 + (steps > 0 ? 0x1B : 0x1A) * 4) = 1.0f;
                steps += steps > 0 ? -1 : 1;
            }
            continue;
        }
        if (!(cur = MEM32(win + 0x94)))
            continue;
        if (MEM32(win + 0x1B4) != 5) {
            /* The menus in a mission (their window counts 3, the frontend's
             * 5): a row of tabs on top that slides as left and right turn it,
             * panes below that hold their own entries. A passing mouse
             * changes nothing; a click on a tab presses left or right as many
             * times as it is tabs away from the chosen one, the one holding
             * the window's pointer -- counted, as the row slides under the
             * mouse. */
            static int tab_steps, tab_phase, entry_steps, entry_phase, entry_goal = -1, entry_wait;
            static uint32_t entry_pane;
            uint32_t e = menu_element_at(win, x, y), menu = MEM32(win + 0x1BC), pane;
            int m;
            /* A pane's entries (CONTINUE / RESTART / QUIT): a pane (type 6)
             * is a value chooser, value at +0x258 from +0x250, +0x25C
             * entries, each at +0x280 + i * 0x14 with its text at +0x18, its
             * place at +0x24 (x, y) and its width at +0x2C. A click on an
             * entry walks the value there and then accepts it, as one click
             * on a tab turns to it (RESTART and QUIT ask again anyway). */
            pane = menu ? MEM32(menu + 0x94) - MEM32(menu + 0xA0) * 4 : 0;
            for (m = 0; pane && m < 512; pane = MEM32(pane), m++) {
                MenuRow ent[16];
                int c, j, k2, sel2;
                if (MEM8(pane + 0x7B) != 6 || MEM8(pane + 0x7A))
                    continue;
                c = (int)MEM32(pane + 0x25C);
                if (c < 2 || c > 16)
                    continue;
                for (j = 0; j < c; j++) {
                    uint32_t p = MEM32(pane + 0x280 + j * 0x14);
                    if (!p)
                        break;
                    ent[j].e = p;
                    /* +0x26 is the text's foot: measured on screen, CONTINUE
                     * at y 214 covers 199 to 212 */
                    ent[j].x = (int16_t)MEM16(p + 0x24);
                    ent[j].y = (int16_t)MEM16(p + 0x26) - 16;
                    ent[j].w = (int16_t)MEM16(p + 0x2C);
                    ent[j].h = 16;
                }
                if (j < c)
                    continue;
                sel2 = (int)(MEM32(pane + 0x258) - MEM32(pane + 0x250));
                if (press && sel2 >= 0 && sel2 < c && (k2 = menu_row_at(ent, c, x, y)) >= 0) {
                    entry_steps = k2 - sel2;
                    entry_goal = k2;
                    entry_pane = pane;
                    entry_wait = 2;
                }
            }
            /* The up/down arrows beside a value chooser (type 10, the control
             * scheme): a click on the upper half is one press of up, on the
             * lower half one of down. */
            pane = press && menu ? MEM32(menu + 0x94) - MEM32(menu + 0xA0) * 4 : 0;
            for (m = 0; pane && m < 512; pane = MEM32(pane), m++) {
                int ax = (int16_t)MEM16(menu + 0x70) + (int16_t)MEM16(pane + 0x70);
                int ay = (int16_t)MEM16(menu + 0x72) + (int16_t)MEM16(pane + 0x72);
                int ah = (int16_t)MEM16(pane + 0x76);
                if (MEM8(pane + 0x7B) != 10 || x < ax - 4 || x > ax + (int16_t)MEM16(pane + 0x74) + 4
                        || y < ay - 4 || y > ay + ah + 4)
                    continue;
                entry_steps = y < ay + ah / 2 ? -1 : 1;
                entry_goal = -1;
                break;
            }
            if (entry_steps && (entry_phase ^= 1)) {
                MEMF(0x1FE6E4 + (entry_steps > 0 ? 0x1B : 0x1A) * 4) = 1.0f;
                entry_steps += entry_steps > 0 ? -1 : 1;
            } else if (!entry_steps && entry_goal >= 0 && --entry_wait <= 0) {
                /* arrived: one press of accept, a frame after the last step */
                if ((int)(MEM32(entry_pane + 0x258) - MEM32(entry_pane + 0x250)) == entry_goal)
                    MEMF(0x1FE6E4 + 0x1F * 4) = 1.0f;
                entry_goal = -1;
            }
            uint32_t chosen = menu_element_at(win, (int16_t)MEM16(cur + 0x24), (int16_t)MEM16(cur + 0x26));
            if (press && e && chosen && e != chosen && MEM8(e + 0x7B) == 1 && MEM8(chosen + 0x7B) == 1
                    && abs((int16_t)MEM16(e + 0x72) - (int16_t)MEM16(chosen + 0x72)) <= 4) {
                uint32_t t;
                int ex = (int16_t)MEM16(e + 0x70), cx = (int16_t)MEM16(chosen + 0x70), between = 0;
                t = MEM32(menu + 0x94) - MEM32(menu + 0xA0) * 4;
                for (m = 0; t && m < 512; t = MEM32(t), m++) {   /* tabs strictly between */
                    int tx = (int16_t)MEM16(t + 0x70);
                    if (MEM8(t + 0x7B) == 1 && !MEM8(t + 0x7A)
                            && abs((int16_t)MEM16(t + 0x72) - (int16_t)MEM16(e + 0x72)) <= 4
                            && tx > (ex < cx ? ex : cx) && tx < (ex < cx ? cx : ex))
                        between++;
                }
                tab_steps = ex > cx ? between + 1 : -(between + 1);
            }
            if (tab_steps && (tab_phase ^= 1)) {
                MEMF(0x1FE6E4 + (tab_steps > 0 ? 0x1D : 0x1C) * 4) = 1.0f;
                tab_steps += tab_steps > 0 ? -1 : 1;
            }
            continue;
        }
        if (!menu_element_at(win, x, y))
            continue;
        hit = 1;
        /* Only when the mouse moved: the D-pad keeps the point it set. */
        if (x != s_menu_wx[i] || y != s_menu_wy[i]) {
            MEM16(cur + 0x24) = (uint16_t)x;
            MEM16(cur + 0x26) = (uint16_t)y;
            s_menu_wx[i] = x;
            s_menu_wy[i] = y;
        }
    }
    if (!down)
        s_menu_accept = s_menu_prompt = 0;
    if (down && s_menu_prompt)
        MEMF(0x1FE6E4 + s_menu_prompt * 4) = 1.0f;
    else if (down && (hit || s_menu_accept))
        MEMF(0x1FE6E4 + 0x1F * 4) = 1.0f;
}

extern void sub_000DE720_gen(void);

/* A front-end menu is open (the main menu, mission select, options: use count
 * 5; a mission's own windows count 3). */
static int menu_frontend_open(void)
{
    int k;
    for (k = 0; k < 5; k++)
        if (MEM8(MENU_WINDOW(k) + 0x1D1) && MEM32(MENU_WINDOW(k) + 0x1B4) == 5)
            return 1;
    return 0;
}

void sub_000DE720(void)
{
    size_t i;
    int k, zoom;
    const char *skip = NULL;

    sub_000DE720_gen();
    if (!nf_enhanced())
        return;
    /* One key on both +pause and +menu_back (Esc in mkb.ini): back in the
     * front-end menus, as Esc goes back on a PC; in a mission it pauses and
     * in the pause menu continues, as the PC version's Esc. Pressed alone,
     * either does its own job everywhere. */
    if (mkb_action("pause") && mkb_action("menu_back"))
        skip = menu_frontend_open() ? "pause" : "menu_back";
    for (i = 0; i < sizeof k_mkb_cmds / sizeof k_mkb_cmds[0]; i++)
        if ((!skip || strcmp(k_mkb_cmds[i].action, skip)) && mkb_action(k_mkb_cmds[i].action))
            for (k = 0; k < 2 && k_mkb_cmds[i].cmd[k] != 0xFF; k++)
                MEMF(0x1FE6E4 + k_mkb_cmds[i].cmd[k] * 4) = 1.0f;
    if (mkb_action("attack")) {
        for (k = 0; k < 5 && !MEM8(MENU_WINDOW(k) + 0x1D1); k++)
            ;
        if (k == 5)                       /* no window open */
            MEMF(0x1FE6E4 + 0x19 * 4) = 1.0f;
    }
    zoom = mkb_action("zoomout") - mkb_action("zoomin");
    if (zoom) {
        float z = MEMF(0x1FE6E4 + 0x06 * 4) + (float)zoom;
        MEMF(0x1FE6E4 + 0x06 * 4) = z > 1.0f ? 1.0f : z < -1.0f ? -1.0f : z;
    }
    /* The PC's second fire button (+attack2): the weapon's alternative fire
     * (0x0C: sub_000BA9EC switches to the weapon's partner type, the
     * pistol's silenced one) -- or, for a weapon with a scope (the title's
     * own manual aim zooms it) and a gadget (the camera lighter's second
     * function is its manual aim), manual aim: weapon_scoped. */
    if (mkb_action("attack2"))
        MEMF(0x1FE6E4 + (s_player_scoped ? 0x13 : 0x0C) * 4) = 1.0f;
    menu_overlay();
    menu_pointer();
}

void sub_0006CF50(void)
{
    int forward, left;
    float f, l;

    sub_0006CF50_gen();
    if (!nf_enhanced())
        return;

    forward = mkb_action("forward") - mkb_action("back");
    left = mkb_action("moveleft") - mkb_action("moveright");
    f = (float)forward;
    l = (float)left;
    /* sub_000A9CF0 limits each axis alone: forward speed 0.1 f^2 + 0.005
     * capped at 0.1, back 0.075 f^2 + 0.005, sideways 0.075 s. A stick cannot
     * hold 1 on both, two keys can, 25% over the straight speed (37% backward).
     * These values give 45 degrees at the straight speed. */
    if (forward && left) {
        f = forward > 0 ? 0.8106f : -0.8292f;
        l = (forward > 0 ? 0.9428f : 0.7543f) * left;
    }
    if (left)
        mkb_command(1, l);
    if (forward)
        mkb_command(2, f);
    space_look();
}

/* Return an override for the given Xbox VA, or NULL to fall through to the
 * generated dispatch table. Called on every indirect call: keep it short. */
recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
    if (xbox_va == 0x00114B94) return dispatch_00114B94;
    if (xbox_va == 0x00114B9F) return dispatch_00114B9F;
    if (xbox_va == 0x0011A230) return dispatch_0011A230;
    if (xbox_va == 0x0011A23B) return dispatch_0011A23B;
    if (xbox_va == 0x00101180) return sub_00101180;
    if (xbox_va == 0x001019F0) return sub_001019F0;
    if (xbox_va == 0x001067C9) return sub_001067C9;
    return (recomp_func_t)0;
}
