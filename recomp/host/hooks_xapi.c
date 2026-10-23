/* Host replacements for guest functions listed in hooks.csv. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

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

/* ---- input: one gamepad on port 0, no memory units ---------------------- */
/* XINPUT_STATE: dwPacketNumber, Gamepad { wButtons, bAnalogButtons[8], sThumbLX, sThumbLY, sThumbRX, sThumbRY } */
static uint32_t g_pad_handle_counter = 0x50000000;
HOOK(XInitDevices) { trace("XInitDevices"); RET(0, 2); }
HOOK(XGetDevices) {
    uint32_t type = ARG(ctx, 0);
    /* bit mask of ports with that device type; gamepad type is the first table entry */
    trace("XGetDevices(%08x)", type);
    RET(type == 0x003b4fa0 ? 1 : 0, 1);
}
HOOK(XGetDeviceChanges) {
    uint32_t pins = ARG(ctx, 1), prem = ARG(ctx, 2);
    static int first = 1;
    WR32(MEM(pins), first && ARG(ctx, 0) == 0x003b4fa0 ? 1 : 0);
    WR32(MEM(prem), 0);
    int changed = first && ARG(ctx, 0) == 0x003b4fa0;
    if (ARG(ctx, 0) == 0x003b4fa0) first = 0;
    RET(changed, 3);
}
HOOK(XInputOpen) {
    uint32_t port = ARG(ctx, 1);
    trace("XInputOpen port %u", port);
    RET(port == 0 ? g_pad_handle_counter++ : 0, 4);
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
/* RECOMP_INPUT="frame:BUTTON,frame:BUTTON,...": press that button for 10
   frames starting at that frame, for runs nobody is sitting at (the attract
   loop only ends on START). Buttons: START BACK A B X Y UP DOWN LEFT RIGHT. */
static uint16_t scripted_buttons(uint8_t *a, uint8_t *b, uint8_t *x, uint8_t *y) {
    const char *script = getenv("RECOMP_INPUT");
    if (!script) return 0;
    uint32_t frame = d3d_frame_count();
    uint16_t buttons = 0;
    const char *end = strchr(script, '|'); /* '|' starts the next run's script */
    for (const char *p = script; *p && (!end || p < end); ) {
        char name[16] = "";
        unsigned at = 0;
        if (sscanf(p, "%u:%15[A-Za-z]", &at, name) == 2 && frame >= at && frame < at + 10) {
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
        }
        p = strchr(p, ',');
        if (!p) break;
        p++;
    }
    return buttons;
}

HOOK(XInputGetState) {
    uint32_t state = ARG(ctx, 1);
    static uint32_t packet;
    if (!packet) fprintf(stderr, "input: the game polls the gamepad (frame %u)\n", d3d_frame_count());
    memset(MEM(state), 0, 0x16); /* XINPUT_STATE: dwPacketNumber, then the packed XINPUT_GAMEPAD at +4 */
    WR32(MEM(state), ++packet);
    /* keyboard as a stand-in until real input is wired: arrows = left stick, Enter = A, Esc = START;
       only while the game window has focus (GetAsyncKeyState sees keys typed anywhere) */
    int16_t lx = 0, ly = 0;
    uint8_t a = 0, b = 0, x = 0, y = 0;
    uint16_t buttons = 0;
    if (r_has_focus()) {
        if (GetAsyncKeyState(VK_LEFT) & 0x8000) lx = -32000;
        if (GetAsyncKeyState(VK_RIGHT) & 0x8000) lx = 32000;
        if (GetAsyncKeyState(VK_UP) & 0x8000) ly = 32000;
        if (GetAsyncKeyState(VK_DOWN) & 0x8000) ly = -32000;
        if (GetAsyncKeyState(VK_RETURN) & 0x8000) a = 255;
        if (GetAsyncKeyState(VK_SPACE) & 0x8000) b = 255;
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) buttons |= 0x0010; /* START */
        if (GetAsyncKeyState(VK_BACK) & 0x8000) buttons |= 0x0020;   /* BACK */
    }
    buttons |= scripted_buttons(&a, &b, &x, &y);
    WR16(MEM(state + 4), buttons);
    WR8(MEM(state + 6), a); /* A */
    WR8(MEM(state + 7), b); /* B */
    WR8(MEM(state + 8), x);
    WR8(MEM(state + 9), y);
    WR16(MEM(state + 14), (uint16_t)lx);
    WR16(MEM(state + 16), (uint16_t)ly);
    RET(0, 2);
}
HOOK(XInputSetState) { RET(0, 2); }
