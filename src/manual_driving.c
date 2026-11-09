/**
 * Manual function overrides for Driving.xbe (nightfire_driving.exe)
 *
 * Same rules as manual_main.c, with Driving.xbe's addresses: a fix says
 * "both modes" in its body, an enhancement asks nf_enhanced() first.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "recomp_types.h"
#include "recomp_funcs.h"

typedef void (*recomp_func_t)(void);

/* Direct mouse look in the gun passages (Alpine Escape's opening, Paris
 * Prelude), the way the main game's mouse turns: the view turns by the mouse
 * counts since the last frame, sensitivity * 0.022 degrees per count, with no
 * stick, ramp or smoothing in between. The pad keeps its own path untouched.
 *
 * sub_000857D0 runs the aiming camera: it eases the pitch [cam+0x1E8] and the
 * yaw [cam+0x1EC] (both in turns, 1.0 = 360 degrees; yaw grows to the right,
 * pitch downward) toward where the right stick steers them, then hands both to
 * sub_00083190 (this, limits, &pitch, &yaw), which clamps them to the
 * passage's range. The mouse is added just before that clamp, so the title's
 * limits hold for it too. Free driving uses another camera and never gets
 * here, so the mouse aims only where the stick does. The clamp runs twice a
 * frame; the second call finds no counts left. */
extern void sub_00083190_gen(void);
extern void sub_00051B40_gen(void);
int mkb_mouse_look(double *yaw_deg, double *pitch_deg);  /* xboxrecomp mkb_input.h */
int mkb_action(const char *name);                         /* xboxrecomp mkb_input.h */
int nf_enhanced(void);  /* main.c: 0 in the original mode, where every
                         * enhancement below calls the title's code only */

static unsigned s_input_frame;   /* sub_00051B40 calls, pad 1 */
static unsigned s_gun_frame;     /* s_input_frame when the gun camera last ran */
static int s_mouse_leads;        /* the mouse moved last, not the right stick */

/* Auto aim: before the clamp, sub_000857D0 also eases the gun toward a target
 * it picks itself (the camera's maxAutoaimYaw/Pitch, autoaimInterpolSpeed)
 * and zeroes the stick's turn speeds while it does. A mouse aims by itself,
 * as in the main game: while it leads, the gun starts each frame from where
 * the last one left it, plus the mouse, so nothing else turns it. The right
 * stick hands the gun back to the title. */
static uint32_t s_last_cam;
static float s_last_pitch, s_last_yaw;

/* The very first Paris Prelude, played before a profile exists, aims by
 * itself by design: the game hands it auto aim whatever a profile would say.
 * The mouse then aims no more than the stick may (it does not take the gun
 * over). Told by the launch data the main game passes (the handoff's
 * RECOMP_LAUNCH_DATA_FILE): +0x480 holds 999 there, a profile's slot
 * otherwise (measured: 999 on a first start, 2 with a profile). */
static int race_autoaim_forced(void)
{
    static int v = -1;
    if (v < 0) {
        const char *f = getenv("RECOMP_LAUNCH_DATA_FILE");
        FILE *h = f && *f ? fopen(f, "rb") : NULL;
        unsigned char b[4];
        v = 0;
        if (h) {
            if (fseek(h, 0x480, SEEK_SET) == 0 && fread(b, 1, 4, h) == 4)
                v = (b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24) == 999;
            fclose(h);
        }
    }
    return v;
}

void sub_00083190(void)
{
    uint32_t cam = g_ecx, pitch = MEM32(g_esp + 8), yaw = MEM32(g_esp + 12);
    double dyaw = 0.0, dpitch = 0.0;

    if (!nf_enhanced()) {
        sub_00083190_gen();
        return;
    }
    s_gun_frame = s_input_frame;
    if (race_autoaim_forced()) {
        mkb_mouse_look(&dyaw, &dpitch);     /* the counts are dropped */
        s_mouse_leads = 0;
        sub_00083190_gen();
        return;
    }
    if (!mkb_mouse_look(&dyaw, &dpitch))
        s_mouse_leads = 0;
    else if (dyaw != 0.0 || dpitch != 0.0)
        s_mouse_leads = 1;
    if (pitch && yaw) {
        if (s_mouse_leads && cam == s_last_cam) {
            MEMF(yaw) = s_last_yaw;
            MEMF(pitch) = s_last_pitch;
        }
        MEMF(yaw) += (float)(dyaw / 360.0);
        MEMF(pitch) -= (float)(dpitch / 360.0);
    }
    sub_00083190_gen();
    if (pitch && yaw) {
        s_last_cam = cam;
        s_last_yaw = MEMF(yaw);
        s_last_pitch = MEMF(pitch);
    }
}

/* Firing: on the gun the title fires with the right trigger, which drives
 * the gas pedal everywhere else (and the A button fires there). mkb.ini's
 * +attack (the mouse's left button) therefore presses the right trigger only
 * while the gun camera runs. sub_00051B40 copies a pad's state into the
 * device's axes at [dev+0x0C]: four sticks, then the eight analog buttons
 * (A, B, X, Y, black, white, left and right trigger) as 0..1, the right
 * trigger at +0x2C. */
static void race_menu_mouse(uint32_t pad);

void sub_00051B40(void)
{
    uint32_t dev = g_ecx, cur;

    sub_00051B40_gen();
    if (!nf_enhanced() || !dev || MEM32(dev + 0x18) != 0)
        return;
    s_input_frame++;
    if ((cur = MEM32(dev + 0xC)) != 0 && (MEMF(cur + 8) != 0.0f || MEMF(cur + 12) != 0.0f))
        s_mouse_leads = 0;   /* the right stick, past its deadzone */
    if (s_input_frame - s_gun_frame <= 3 && mkb_action("attack") &&
            (cur = MEM32(dev + 0xC)) != 0)
        MEMF(cur + 0x2C) = 1.0f;
    /* The scope: every gun layout (povcfg0-6.def) looks through it while the
     * left trigger is held (GAMEACTION_AIMZOOMRIGHT), as the main game's
     * right mouse does on a scoped weapon. mkb.ini's +attack2 (the right
     * button, which drives X -- a gadget -- elsewhere) holds the left trigger
     * while the gun camera runs, and keeps X out of it there. */
    if (s_input_frame - s_gun_frame <= 3 && mkb_action("attack2") &&
            (cur = MEM32(dev + 0xC)) != 0) {
        MEMF(cur + 0x28) = 1.0f;
        MEMF(cur + 0x18) = 0.0f;
    }
    if ((cur = MEM32(dev + 0xC)) != 0)
        race_menu_mouse(cur);
}

/* Auto aim on the crosshair: the aim object at 0x23E1B0 (sub_000CE620) keeps
 * the enemies near the crosshair as candidates (+0x30, count +0x70) and locks
 * the first one (+0x28); the crosshair and the aim point (+0x10, on screen
 * +0x20) then drift onto it while the gun holds still. Measured in Alpine
 * Escape: locked, the crosshair sat at 343/255 with the gun at rest; with the
 * list cleared it stays at the centre, 320/240, and the aim point straight
 * ahead. While the mouse leads on the gun, the list and the lock are cleared
 * around the update, so the gun hits where it points. */
extern void sub_000CE620_gen(void);

void sub_000CE620(void)
{
    const uint32_t aim = 0x23E1B0;
    int off = nf_enhanced() && s_mouse_leads && s_input_frame - s_gun_frame <= 3;

    if (off) {
        MEM32(aim + 0x28) = 0;
        MEM32(aim + 0x70) = 0;
    }
    sub_000CE620_gen();
    if (off) {
        MEM32(aim + 0x28) = 0;
        MEM32(aim + 0x70) = 0;
    }
}

/* The in-race menus' object (pause, mission end): sub_000E0140 updates it
 * every frame, sub_000DCC10 takes its FRONTENDACTIONs (0x41 up .. 0x49
 * invert): +0x470 the tab, 0 to 3, left and right turn it round; +0x474 the
 * entry on the first tab; +0x484 a question asked. */
extern void sub_000E0140_gen(void);
static uint32_t s_race_menu;

/* The mouse on that menu. It looks as the main game's pause menu does, 12
 * higher (clicks aimed at the tabs came at y 114 against the main game's
 * 128), and the click areas sit 4 above even that, which hit better in play;
 * the places measured there, in the menu's 640x480: tabs at y 120 to 136, x 84,
 * 199, 339 and 465 to 191, 331, 457 and 555; CONTINUE, RESTART and QUIT with
 * their feet at y 214, 245 and 277 around x 273 to 369; the question's YES
 * and NO 17 higher than measured there, at about y 210 to 222 and 241 to 254
 * (a click on NO's middle came at y 247; +0x484: 1 YES, 2 NO, NO first; up
 * and down swap them); the scheme's arrows at 84,152 24x34. A click becomes the D-pad
 * presses and the A that get there, one press every other poll through the
 * pad's channels at [dev+0x0C] (D-pad up +0x30, down +0x34, left +0x38,
 * right +0x3C, A +0x10), as the pad gives them. */
int xbox_FramebufferWindowPointer(int *x, int *y);   /* xboxrecomp video_player.h */
void mkb_pointer_show(void);                         /* xboxrecomp mkb_input.h */
int mkb_pointer_click(void);

static float race_squeeze(void)
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

static void race_menu_mouse(uint32_t pad)
{
    static int lx, ly, armed, on, was, vsteps, hsteps, accept, phase;
    static const int tab_l[4] = { 84, 199, 339, 465 }, tab_r[4] = { 191, 331, 457, 555 };
    static const int row_foot[3] = { 214, 245, 277 };
    uint32_t m = s_race_menu;
    int x, y, down, press, i;

    if (!m || !MEM8(m + 0x2B0) || MEM32(m + 0x478) != 1) {
        armed = on = vsteps = hsteps = accept = 0;
        return;
    }
    /* The left button is also the A in mkb.ini's driving layout: in this menu
     * a click means where it lands, not the chosen entry. */
    if (mkb_pointer_click())
        MEMF(pad + 0x10) = 0.0f;
    /* the queued presses: one every other poll, the A last */
    if ((phase ^= 1) != 0) {
        if (hsteps) {
            MEMF(pad + (hsteps > 0 ? 0x3C : 0x38)) = 1.0f;
            hsteps += hsteps > 0 ? -1 : 1;
        } else if (vsteps) {
            MEMF(pad + (vsteps > 0 ? 0x34 : 0x30)) = 1.0f;
            vsteps += vsteps > 0 ? -1 : 1;
        } else if (accept) {
            MEMF(pad + 0x10) = 1.0f;
            accept = 0;
        }
    }
    if (!xbox_FramebufferWindowPointer(&x, &y))
        return;
    if (race_squeeze() < 1.0f)
        x = (int)(320.0f + (x - 320) / race_squeeze());
    y += 13;                                 /* onto the main game's places */
    if (armed && (x != lx || y != ly))
        on = 1;
    lx = x;
    ly = y;
    armed = 1;
    if (!on)
        return;
    mkb_pointer_show();
    down = mkb_pointer_click();
    press = down && !was;
    was = down;
    if (!press || vsteps || hsteps || accept)
        return;
    if (y >= 112 && y <= 144) {                                  /* a tab */
        for (i = 0; i < 4; i++)
            if (x >= tab_l[i] && x <= tab_r[i])
                hsteps = i - (int)MEM32(m + 0x470);
        return;
    }
    if (MEM32(m + 0x470) == 0 && MEM32(m + 0x484)) {            /* YES / NO */
        if (x < 280 || x > 370)
            return;
        if (y >= 203 && y < 232)
            i = 1;
        else if (y >= 232 && y < 263)
            i = 2;
        else
            return;
        vsteps = i - (int)MEM32(m + 0x484);
        accept = 1;
        return;
    }
    if (MEM32(m + 0x470) == 0) {                                 /* the entries */
        if (x < 249 || x > 393)
            return;
        for (i = 0; i < 3; i++) {
            int top = i ? (row_foot[i - 1] + row_foot[i] - 16) / 2 : row_foot[0] - 20;
            int bottom = i < 2 ? (row_foot[i] + row_foot[i + 1] - 16) / 2 : row_foot[2] + 4;
            if (y >= top && y < bottom) {
                vsteps = i - (int)MEM32(m + 0x474);
                accept = 1;
            }
        }
        return;
    }
    if (x >= 64 && x <= 112 && y >= 148 && y <= 190)            /* the scheme's arrows */
        vsteps = y < 169 ? -1 : 1;
}

void sub_000E0140(void)
{
    uint32_t self = g_ecx;

    sub_000E0140_gen();
    if (!nf_enhanced())
        return;
    s_race_menu = self;
}

recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
    (void)xbox_va;
    return (recomp_func_t)0;
}
