/* Host replacements for guest functions listed in hooks.csv. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

#include "kernel.h"

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
HOOK(XLaunchNewImageA) {
    const char *path = ARG(ctx, 0) ? (const char *)MEM(ARG(ctx, 0)) : "(dashboard)";
    fprintf(stderr, "XLaunchNewImageA(%s): the game exited\n", path);
    exit(0);
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
    memset(MEM(caps), 0, 0x20);
    WR8(MEM(caps), 1);      /* SubType: XINPUT_DEVSUBTYPE_GC_GAMEPAD */
    WR16(MEM(caps + 4), 0xffff); /* In.Gamepad.wButtons: all supported */
    RET(0, 2);
}
HOOK(XInputGetState) {
    uint32_t state = ARG(ctx, 1);
    static uint32_t packet;
    memset(MEM(state), 0, 0x14);
    WR32(MEM(state), ++packet);
    /* keyboard as a stand-in until real input is wired: arrows = left stick, Enter = A, Esc = START */
    int16_t lx = 0, ly = 0;
    if (GetAsyncKeyState(VK_LEFT) & 0x8000) lx = -32000;
    if (GetAsyncKeyState(VK_RIGHT) & 0x8000) lx = 32000;
    if (GetAsyncKeyState(VK_UP) & 0x8000) ly = 32000;
    if (GetAsyncKeyState(VK_DOWN) & 0x8000) ly = -32000;
    uint16_t buttons = 0;
    if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) buttons |= 0x0010; /* START */
    if (GetAsyncKeyState(VK_BACK) & 0x8000) buttons |= 0x0020;   /* BACK */
    WR16(MEM(state + 4), buttons);
    WR8(MEM(state + 6), (GetAsyncKeyState(VK_RETURN) & 0x8000) ? 255 : 0); /* A */
    WR8(MEM(state + 7), (GetAsyncKeyState(VK_SPACE) & 0x8000) ? 255 : 0);  /* B */
    WR16(MEM(state + 14), (uint16_t)lx);
    WR16(MEM(state + 16), (uint16_t)ly);
    RET(0, 2);
}
HOOK(XInputSetState) { RET(0, 2); }
