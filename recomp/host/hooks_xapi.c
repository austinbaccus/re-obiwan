/* Host replacements for guest functions listed in hooks.csv. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xinput.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kernel.h"
#include "render.h"

#define HOOK(name) void hook_##name(Ctx *ctx)
#define RET(v, n) do { SET_EAX(ctx, (v)); RET_STDCALL(ctx, (n)); return; } while (0)

HOOK(OutputDebugStringA) {
    fprintf(stderr, "[debug] %s", (const char *)MEM(ARG(ctx, 0)));
    RET(0, 1);
}
HOOK(XapiBootToDash) {
    fprintf(stderr, "XapiBootToDash(%u, %u, %u): the game asked to return to the dashboard\n", ARG(ctx, 0),
            ARG(ctx, 1), ARG(ctx, 2));
    exit(0);
}
/* XLaunchNewImageA(pszImagePath, pLaunchData): the game relaunches its own
   XBE with 3 KB of launch data (after the attract loop, or to start a
   level). Save the data next to the executable and start a fresh process;
   boot.c turns the file into the LaunchDataPage the kernel exports. */
HOOK(XLaunchNewImageA) {
    const char *path = ARG(ctx, 0) ? (const char *)MEM(ARG(ctx, 0)) : "";
    uint32_t data = ARG(ctx, 1);
    fprintf(stderr, "XLaunchNewImageA(%s): relaunching with %s\n", path, data ? "launch data" : "no launch data");
    if (!path[0] || _stricmp(path, "d:\\default.xbe") != 0) { fprintf(stderr, "  not this title: exiting\n"); exit(0); }
    char exe[MAX_PATH], file[MAX_PATH];
    GetModuleFileNameA(NULL, exe, sizeof exe);
    strcpy(file, exe);
    char *slash = strrchr(file, '\\');
    strcpy(slash ? slash + 1 : file, "launch.bin");
    FILE *f = fopen(file, "wb");
    if (f) {
        uint8_t zero[3072] = {0};
        fwrite(data ? MEM(data) : zero, 1, 3072, f);
        fclose(f);
    }
    SetEnvironmentVariableA("RECOMP_LAUNCH", NULL); /* a replayed run relaunches with the new data, not the replayed file */
    /* scripted runs: RECOMP_INPUT and RECOMP_SHOT hold "this run|next run|...";
       the new process gets what follows the first '|' (nothing if none), and
       RECOMP_RUN counts the relaunches (it names the shots) */
    static const char *const per_run[] = {"RECOMP_INPUT", "RECOMP_SHOT"};
    for (int i = 0; i < 2; i++) {
        const char *v = getenv(per_run[i]), *bar = v ? strchr(v, '|') : NULL;
        SetEnvironmentVariableA(per_run[i], bar ? bar + 1 : NULL);
    }
    char run[16];
    snprintf(run, sizeof run, "%d", (getenv("RECOMP_RUN") ? atoi(getenv("RECOMP_RUN")) : 0) + 1);
    SetEnvironmentVariableA("RECOMP_RUN", run);
    STARTUPINFOA si = {sizeof si};
    PROCESS_INFORMATION pi;
    char cmd[2048];
    snprintf(cmd, sizeof cmd, "%s", GetCommandLineA());
    if (CreateProcessA(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) exit(0);
    fprintf(stderr, "  relaunch failed (error %lu)\n", GetLastError());
    exit(1);
}
HOOK(XapiInitAutoPowerDown) { RET_CDECL(ctx); }

/* ---- input: gamepads on ports 0..3, no memory units ---------------------- */
/* Windows XInput controllers 0..3 are Xbox ports 0..3. Port 0 is always
   present: the keyboard and RECOMP_INPUT scripts drive it too. Plugging and
   unplugging reach the game as XGetDeviceChanges insertions and removals
   (XBoxGenerator::vf01 opens and closes its pads on them). */
#define XDEVICE_TYPE_GAMEPAD 0x003b4fa0 /* the gamepad entry of XAPI's device type table */
typedef DWORD(WINAPI *XInputGetStateFn)(DWORD, XINPUT_STATE *);
typedef DWORD(WINAPI *XInputSetStateFn)(DWORD, XINPUT_VIBRATION *);
static XInputGetStateFn host_get_state;
static XInputSetStateFn host_set_state;
static struct { int connected; DWORD retry; XINPUT_STATE s; } g_pads[4];
static uint32_t g_reported; /* the ports the game has been told about */

static void xinput_load(void) {
    static int done;
    if (done++) return;
    const char *const dlls[] = {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"};
    for (int i = 0; i < 3 && !host_get_state; i++) {
        HMODULE m = LoadLibraryA(dlls[i]);
        if (!m) continue;
        host_get_state = (XInputGetStateFn)GetProcAddress(m, "XInputGetState");
        host_set_state = (XInputSetStateFn)GetProcAddress(m, "XInputSetState");
    }
    if (!host_get_state) fprintf(stderr, "input: no XInput on this system; keyboard only\n");
}

/* read one host controller; one that was absent is asked again at most once
   a second (polling a missing XInput device is slow) */
static int pad_read(int port) {
    xinput_load();
    if (!host_get_state) return 0;
    DWORD now = GetTickCount();
    if (!g_pads[port].connected && now - g_pads[port].retry < 1000 && g_pads[port].retry) return 0;
    int was = g_pads[port].connected;
    g_pads[port].connected = host_get_state(port, &g_pads[port].s) == ERROR_SUCCESS;
    if (!g_pads[port].connected) { g_pads[port].retry = now ? now : 1; memset(&g_pads[port].s, 0, sizeof g_pads[port].s); }
    if (was != g_pads[port].connected) fprintf(stderr, "input: controller %d %s\n", port + 1, g_pads[port].connected ? "connected" : "removed");
    return g_pads[port].connected;
}

static uint32_t ports_present(void) {
    uint32_t mask = 1; /* the keyboard */
    for (int p = 0; p < 4; p++) if (pad_read(p)) mask |= 1u << p;
    return mask;
}

HOOK(XInitDevices) { trace("XInitDevices"); RET(0, 2); }
HOOK(XGetDevices) {
    uint32_t type = ARG(ctx, 0);
    trace("XGetDevices(%08x)", type);
    if (type != XDEVICE_TYPE_GAMEPAD) RET(0, 1);
    g_reported = ports_present(); /* changes are reported from here on */
    RET(g_reported, 1);
}
HOOK(XGetDeviceChanges) {
    uint32_t pins = ARG(ctx, 1), prem = ARG(ctx, 2), ins = 0, rem = 0;
    if (ARG(ctx, 0) == XDEVICE_TYPE_GAMEPAD) {
        uint32_t now = ports_present();
        ins = now & ~g_reported;
        rem = g_reported & ~now;
        g_reported = now;
    }
    WR32(MEM(pins), ins);
    WR32(MEM(prem), rem);
    RET(ins || rem, 3);
}
/* handles carry their port in the low bits */
static uint32_t g_pad_handle_counter;
HOOK(XInputOpen) {
    uint32_t port = ARG(ctx, 1);
    trace("XInputOpen port %u", port);
    RET(port < 4 && (g_reported >> port & 1) ? 0x50000000 | (++g_pad_handle_counter << 4) | port : 0, 4);
}
HOOK(XInputClose) { RET(0, 1); }
HOOK(XInputGetCapabilities) {
    uint32_t caps = ARG(ctx, 1);
    /* XINPUT_CAPABILITIES is packed: SubType +0, Reserved +1, In.Gamepad +3
       {wButtons +3, bAnalogButtons[8] +5, sThumbLX/LY/RX/RY +13..+19}, Out +21.
       The game accepts a pad only if every digital and analog control and all
       four thumb axes are reported (XBoxGenerator's FUN_000cb950). */
    memset(MEM(caps), 0, 0x1c);
    WR8(MEM(caps), 1);            /* SubType: XINPUT_DEVSUBTYPE_GC_GAMEPAD */
    WR16(MEM(caps + 3), 0xffff);  /* wButtons */
    memset(MEM(caps + 5), 0xff, 8); /* bAnalogButtons */
    for (int i = 0; i < 4; i++) WR16(MEM(caps + 13 + 2 * i), 0xffff); /* thumb axes */
    RET(0, 2);
}
/* RECOMP_INPUT="frame:BUTTON,frame:BUTTON:frames,...": press that button for
   10 frames (or the given number) starting at that frame, for runs nobody is
   sitting at (the attract loop only ends on START). Buttons: START BACK A B X
   Y UP DOWN LEFT RIGHT, and the left stick pushed: LSUP LSDOWN LSLEFT LSRIGHT. */
static uint16_t scripted_buttons(uint8_t *a, uint8_t *b, uint8_t *x, uint8_t *y, int16_t *lx, int16_t *ly) {
    const char *script = getenv("RECOMP_INPUT");
    if (!script) return 0;
    uint32_t frame = d3d_frame_count();
    uint16_t buttons = 0;
    const char *end = strchr(script, '|'); /* '|' starts the next run's script */
    for (const char *p = script; *p && (!end || p < end); ) {
        char name[16] = "";
        unsigned at = 0, len = 10;
        if (sscanf(p, "%u:%15[A-Za-z]:%u", &at, name, &len) >= 2 && frame >= at && frame < at + len) {
            if (!strcmp(name, "START")) buttons |= 0x0010;
            else if (!strcmp(name, "BACK")) buttons |= 0x0020;
            else if (!strcmp(name, "UP")) buttons |= 0x0001;
            else if (!strcmp(name, "DOWN")) buttons |= 0x0002;
            else if (!strcmp(name, "LEFT")) buttons |= 0x0004;
            else if (!strcmp(name, "RIGHT")) buttons |= 0x0008;
            else if (!strcmp(name, "A")) *a = 255;
            else if (!strcmp(name, "B")) *b = 255;
            else if (!strcmp(name, "X")) *x = 255;
            else if (!strcmp(name, "Y")) *y = 255;
            else if (!strcmp(name, "LSUP")) *ly = 32000;
            else if (!strcmp(name, "LSDOWN")) *ly = -32000;
            else if (!strcmp(name, "LSLEFT")) *lx = -32000;
            else if (!strcmp(name, "LSRIGHT")) *lx = 32000;
        }
        p = strchr(p, ',');
        if (!p) break;
        p++;
    }
    return buttons;
}

/* a stick with a radial dead zone (RECOMP_DEADZONE, a fraction of full
   travel, default 0.15: the game uses the raw values), rescaled so the rest
   of the travel still reaches full deflection */
static void dead_zone(int16_t *x, int16_t *y) {
    static float dz = -1;
    if (dz < 0) dz = getenv("RECOMP_DEADZONE") ? (float)atof(getenv("RECOMP_DEADZONE")) : 0.15f;
    float fx = *x / 32767.0f, fy = *y / 32767.0f, m = sqrtf(fx * fx + fy * fy);
    if (m <= dz) { *x = *y = 0; return; }
    float s = ((m > 1 ? 1 : m) - dz) / (1 - dz) / m;
    float ox = fx * s * 32767, oy = fy * s * 32767;
    *x = (int16_t)(ox > 32767 ? 32767 : ox < -32768 ? -32768 : ox);
    *y = (int16_t)(oy > 32767 ? 32767 : oy < -32768 ? -32768 : oy);
}

HOOK(XInputGetState) {
    uint32_t handle = ARG(ctx, 0), state = ARG(ctx, 1), port = handle & 3;
    static uint32_t packet;
    if (!packet) fprintf(stderr, "input: the game polls the gamepad (frame %u)\n", d3d_frame_count());
    int pad = pad_read((int)port);
    if (!pad && port != 0) RET(ERROR_DEVICE_NOT_CONNECTED, 2);
    memset(MEM(state), 0, 0x16); /* XINPUT_STATE: dwPacketNumber, then the packed XINPUT_GAMEPAD at +4 */
    WR32(MEM(state), ++packet);
    /* Xbox XINPUT_GAMEPAD: wButtons (D-pad, START, BACK, thumb clicks: the
       PC's low byte), bAnalogButtons[8] (A, B, X, Y, BLACK, WHITE, left and
       right trigger; the game counts any non-zero value as pressed), sticks */
    int16_t lx = 0, ly = 0, rx = 0, ry = 0;
    uint8_t an[8] = {0};
    uint16_t buttons = 0;
    if (pad) {
        const XINPUT_GAMEPAD *g = &g_pads[port].s.Gamepad;
        static const WORD face[6] = {XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_B, XINPUT_GAMEPAD_X, XINPUT_GAMEPAD_Y,
                                     XINPUT_GAMEPAD_RIGHT_SHOULDER, XINPUT_GAMEPAD_LEFT_SHOULDER}; /* black, white */
        buttons = g->wButtons & 0xff;
        for (int i = 0; i < 6; i++) an[i] = (g->wButtons & face[i]) ? 255 : 0;
        an[6] = g->bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD ? g->bLeftTrigger : 0;
        an[7] = g->bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD ? g->bRightTrigger : 0;
        lx = g->sThumbLX; ly = g->sThumbLY; rx = g->sThumbRX; ry = g->sThumbRY;
        dead_zone(&lx, &ly);
        dead_zone(&rx, &ry);
    }
    if (port == 0) {
        /* keyboard: arrows = left stick, Enter = A, Space = B, Esc = START,
           Backspace = BACK; only while the game window has focus
           (GetAsyncKeyState sees keys typed anywhere) */
        if (r_has_focus()) {
            if (GetAsyncKeyState(VK_LEFT) & 0x8000) lx = -32000;
            if (GetAsyncKeyState(VK_RIGHT) & 0x8000) lx = 32000;
            if (GetAsyncKeyState(VK_UP) & 0x8000) ly = 32000;
            if (GetAsyncKeyState(VK_DOWN) & 0x8000) ly = -32000;
            if (GetAsyncKeyState(VK_RETURN) & 0x8000) an[0] = 255;
            if (GetAsyncKeyState(VK_SPACE) & 0x8000) an[1] = 255;
            if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) buttons |= 0x0010; /* START */
            if (GetAsyncKeyState(VK_BACK) & 0x8000) buttons |= 0x0020;   /* BACK */
        }
        buttons |= scripted_buttons(&an[0], &an[1], &an[2], &an[3], &lx, &ly);
    }
    WR16(MEM(state + 4), buttons);
    for (int i = 0; i < 8; i++) WR8(MEM(state + 6 + i), an[i]);
    WR16(MEM(state + 14), (uint16_t)lx);
    WR16(MEM(state + 16), (uint16_t)ly);
    WR16(MEM(state + 18), (uint16_t)rx);
    WR16(MEM(state + 20), (uint16_t)ry);
    RET(0, 2);
}
/* (hDevice, pFeedback): XINPUT_FEEDBACK is a 0x42-byte header (dwStatus,
   hEvent, reserved) then the left (heavy) and right motor speeds. The Xbox
   sends asynchronously; the game only sends again once dwStatus is no longer
   ERROR_IO_PENDING, so this completes at once. */
HOOK(XInputSetState) {
    uint32_t port = ARG(ctx, 0) & 3, fb = ARG(ctx, 1);
    XINPUT_VIBRATION v = {RD16(MEM(fb + 0x42)), RD16(MEM(fb + 0x44))};
    if (!getenv("RECOMP_NORUMBLE") && host_set_state && g_pads[port].connected) host_set_state(port, &v);
    WR32(MEM(fb), ERROR_SUCCESS);
    RET(ERROR_SUCCESS, 2);
}
