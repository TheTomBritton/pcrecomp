/*
 * dinput.c - win32hle DirectInput (dinput.dll, DirectX 3 to 7): a system
 * mouse and a system keyboard, read from user32's cursor and key state, so a
 * game that polls DirectInput sees what the window would have told it.
 *
 * Objects follow ddraw.c and dsound.c: a vtable of shims from hle_com_vtable,
 * an object from hle_com_new, host state behind the reference count. One
 * IDirectInput object serves every interface version (the vtables are
 * prefixes of each other); a device object answers IDirectInputDevice,
 * IDirectInputDevice2 and 7 the same way, the force-feedback and effect
 * methods as unsupported.
 *
 * The mouse reports relative motion by default (DIPROPAXISMODE_REL, as
 * DirectInput does): the distance the cursor moved since the last read, so a
 * game that integrates it keeps its own cursor under the real one as long as
 * it started there. A host that knows where the game keeps that cursor can
 * take over with hle_dinput_mouse_hook. With DIPROPAXISMODE_ABS set, lX and
 * lY are the cursor itself. Buttons are user32's VK_LBUTTON/RBUTTON/MBUTTON.
 * The keyboard is the 256-byte DIK array built from the VK state. Buffered
 * data (GetDeviceData) is made from the state changes between reads, so a
 * game that reads the buffer instead of the state sees the same keys and
 * buttons; axes are not buffered.
 *
 * Joysticks: none. CreateDevice of any other GUID is DIERR_DEVICENOTREG, and
 * EnumDevices names only the mouse and the keyboard.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "win32hle.h"

#define DI_OK                  0u
#define DI_NOEFFECT            1u          /* S_FALSE */
#define DIERR_UNSUPPORTED      0x80004001u
#define DIERR_INVALIDPARAM     0x80070057u
#define DIERR_NOTINITIALIZED   0x80070015u
#define DIERR_DEVICENOTREG     0x80040154u /* REGDB_E_CLASSNOTREG */
#define DIERR_NOTFOUND         0x80070002u
#define DIERR_NOTACQUIRED      0x8007000Cu
#define DIERR_INPUTLOST        0x8007001Eu

#define DIDEVTYPE_MOUSE        2u
#define DIDEVTYPE_KEYBOARD     3u
#define DIDEVTYPE_JOYSTICK     4u
#define DIDC_ATTACHED          1u
#define DIDC_EMULATED          4u
#define DIPROPAXISMODE_ABS     1u
#define DIK_MAX                256

enum { DEV_MOUSE = 1, DEV_KEYBOARD = 2 };

/* The device object's host state, behind the vtable pointer and the count. */
typedef struct {
    int kind;
    int acquired;
    int axis_abs;               /* DIPROP_AXISMODE */
    uint32_t data_size;         /* SetDataFormat's dwDataSize */
    uint32_t buffer_size;       /* DIPROP_BUFFERSIZE */
    int last_x, last_y;         /* the cursor at the last state read (relative motion) */
    int have_last;
    uint8_t snap[DIK_MAX];      /* the state at the last GetDeviceData, for the diffs */
    uint32_t seq;               /* dwSequence */
} didev_t;
#define D(self) ((didev_t *)hle_ptr((self) + 8))

/* A host that knows where the game keeps its cursor may replace the motion
 * (x, y: the cursor; dx, dy: in, what would be reported; out, what is). */
void (*hle_dinput_mouse_hook)(int x, int y, int *dx, int *dy);
int g_dinput_trace;

static uint32_t g_vt_di, g_vt_dev;
static uint32_t g_di;                                /* the one DirectInput object */

static const uint8_t GUID_SysMouse[16]    = { 0x60, 0x2B, 0x1D, 0x6F, 0xA0, 0xD5, 0xCF, 0x11, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 };
static const uint8_t GUID_SysKeyboard[16] = { 0x61, 0x2B, 0x1D, 0x6F, 0xA0, 0xD5, 0xCF, 0x11, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 };

/* VK -> DIK (PC/AT set 1 scancodes), the keys a game binds. 0: no key. */
static uint8_t vk_to_dik(uint32_t vk) {
    static const uint8_t row1[] = { 0x0B, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A };     /* '0'..'9' */
    static const uint8_t letters[26] = {                                                             /* 'A'..'Z' */
        0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
        0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C };
    if (vk >= '0' && vk <= '9') return row1[vk - '0'];
    if (vk >= 'A' && vk <= 'Z') return letters[vk - 'A'];
    if (vk >= 0x70 && vk <= 0x79) return (uint8_t)(0x3B + (vk - 0x70));   /* F1..F10 */
    if (vk >= 0x60 && vk <= 0x69) {                                        /* numpad 0..9 */
        static const uint8_t np[] = { 0x52, 0x4F, 0x50, 0x51, 0x4B, 0x4C, 0x4D, 0x47, 0x48, 0x49 };
        return np[vk - 0x60];
    }
    switch (vk) {
    case 0x1B: return 0x01;    /* escape */
    case 0xBD: return 0x0C;    /* - */
    case 0xBB: return 0x0D;    /* = */
    case 0x08: return 0x0E;    /* backspace */
    case 0x09: return 0x0F;    /* tab */
    case 0xDB: return 0x1A;    /* [ */
    case 0xDD: return 0x1B;    /* ] */
    case 0x0D: return 0x1C;    /* return */
    case 0x11: case 0xA2: return 0x1D;   /* control, left control */
    case 0xA3: return 0x9D;    /* right control */
    case 0xBA: return 0x27;    /* ; */
    case 0xDE: return 0x28;    /* ' */
    case 0xC0: return 0x29;    /* ` */
    case 0x10: case 0xA0: return 0x2A;   /* shift, left shift */
    case 0xA1: return 0x36;    /* right shift */
    case 0xDC: return 0x2B;    /* \ */
    case 0xBC: return 0x33;    /* , */
    case 0xBE: return 0x34;    /* . */
    case 0xBF: return 0x35;    /* / */
    case 0x6A: return 0x37;    /* numpad * */
    case 0x12: case 0xA4: return 0x38;   /* alt, left alt */
    case 0xA5: return 0xB8;    /* right alt */
    case 0x20: return 0x39;    /* space */
    case 0x14: return 0x3A;    /* caps lock */
    case 0x90: return 0x45;    /* num lock */
    case 0x91: return 0x46;    /* scroll lock */
    case 0x6D: return 0x4A;    /* numpad - */
    case 0x6B: return 0x4E;    /* numpad + */
    case 0x6E: return 0x53;    /* numpad . */
    case 0x7A: return 0x57;    /* F11 */
    case 0x7B: return 0x58;    /* F12 */
    case 0x6F: return 0xB5;    /* numpad / */
    case 0x24: return 0xC7;    /* home */
    case 0x26: return 0xC8;    /* up */
    case 0x21: return 0xC9;    /* page up */
    case 0x25: return 0xCB;    /* left */
    case 0x27: return 0xCD;    /* right */
    case 0x23: return 0xCF;    /* end */
    case 0x28: return 0xD0;    /* down */
    case 0x22: return 0xD1;    /* page down */
    case 0x2D: return 0xD2;    /* insert */
    case 0x2E: return 0xD3;    /* delete */
    case 0x13: return 0xC5;    /* pause */
    default: return 0;
    }
}

/* The keyboard as DirectInput reports it: 0x80 for each key down. */
static void keyboard_state(uint8_t *out) {
    memset(out, 0, DIK_MAX);
    for (uint32_t vk = 1; vk < 256; vk++) {
        if (vk <= 6) continue;                          /* the mouse buttons are not keys */
        uint8_t dik = vk_to_dik(vk);
        if (dik && hle_key_down(vk)) out[dik] = 0x80;
    }
}

/* The mouse: the cursor and the buttons, as the state read reports them. */
static void mouse_state(didev_t *d, int32_t *lx, int32_t *ly, uint8_t *buttons) {
    int x, y;
    hle_cursor_get(&x, &y);
    if (d->axis_abs) {
        *lx = x, *ly = y;
    } else {
        int dx = d->have_last ? x - d->last_x : 0, dy = d->have_last ? y - d->last_y : 0;
        if (hle_dinput_mouse_hook) hle_dinput_mouse_hook(x, y, &dx, &dy);
        *lx = dx, *ly = dy;
    }
    d->last_x = x, d->last_y = y, d->have_last = 1;
    buttons[0] = hle_key_down(0x01) ? 0x80 : 0;
    buttons[1] = hle_key_down(0x02) ? 0x80 : 0;
    buttons[2] = hle_key_down(0x04) ? 0x80 : 0;
    buttons[3] = 0;
}

/* ---- IDirectInputDevice ---- */
static void dev_GetCapabilities(void) {                   /* (this, lpDIDevCaps) */
    didev_t *d = D(A32(0));
    uint32_t c = A32(1);
    if (!c) RET(DIERR_INVALIDPARAM, 2);
    uint32_t size = MEM32(c);                             /* DIDEVCAPS_DX3 is 24 bytes, DirectX 5's 44 */
    if (size < 24) RET(DIERR_INVALIDPARAM, 2);
    for (uint32_t i = 4; i < size && i < 60; i += 4) MEM32(c + i) = 0;
    MEM32(c + 4) = DIDC_ATTACHED | DIDC_EMULATED;
    MEM32(c + 8) = d->kind == DEV_MOUSE ? (DIDEVTYPE_MOUSE | (2u << 8)) : (DIDEVTYPE_KEYBOARD | (4u << 8));
    MEM32(c + 12) = d->kind == DEV_MOUSE ? 3u : 0u;       /* dwAxes */
    MEM32(c + 16) = d->kind == DEV_MOUSE ? 4u : 128u;     /* dwButtons */
    MEM32(c + 20) = 0;                                    /* dwPOVs */
    RET(DI_OK, 2);
}
static void dev_EnumObjects(void) { RET(DI_OK, 4); }      /* (this, callback, ref, flags): nothing named */
static void dev_GetProperty(void) {                       /* (this, rguidProp, lpdiph) */
    didev_t *d = D(A32(0));
    uint32_t prop = A32(1), h = A32(2);                   /* a predefined property is a small integer, not a GUID */
    if (!h) RET(DIERR_INVALIDPARAM, 3);
    if (prop == 1) { MEM32(h + 16) = d->buffer_size; RET(DI_OK, 3); }           /* DIPROP_BUFFERSIZE: dwData after the 16-byte header */
    if (prop == 2) { MEM32(h + 16) = d->axis_abs ? DIPROPAXISMODE_ABS : 0u; RET(DI_OK, 3); }   /* DIPROP_AXISMODE */
    if (prop == 4) { MEM32(h + 16) = 1; RET(DI_OK, 3); }                         /* DIPROP_GRANULARITY */
    RET(DIERR_UNSUPPORTED, 3);
}
static void dev_SetProperty(void) {                       /* (this, rguidProp, lpcdiph) */
    didev_t *d = D(A32(0));
    uint32_t prop = A32(1), h = A32(2);
    if (!h) RET(DIERR_INVALIDPARAM, 3);
    if (prop == 1) { d->buffer_size = MEM32(h + 16); RET(DI_OK, 3); }
    if (prop == 2) { d->axis_abs = MEM32(h + 16) == DIPROPAXISMODE_ABS; RET(DI_OK, 3); }
    RET(DI_OK, 3);                                        /* ranges, dead zones, saturation: nothing to apply */
}
static void dev_Acquire(void) {
    didev_t *d = D(A32(0));
    uint32_t r = d->acquired ? DI_NOEFFECT : DI_OK;
    d->acquired = 1;
    d->have_last = 0;                                     /* motion starts from here */
    RET(r, 1);
}
static void dev_Unacquire(void) {
    didev_t *d = D(A32(0));
    uint32_t r = d->acquired ? DI_OK : DI_NOEFFECT;
    d->acquired = 0;
    RET(r, 1);
}
static void dev_GetDeviceState(void) {                    /* (this, cbData, lpvData) */
    didev_t *d = D(A32(0));
    uint32_t n = A32(1), p = A32(2);
    if (!p || !n) RET(DIERR_INVALIDPARAM, 3);
    if (!d->acquired) RET(DIERR_NOTACQUIRED, 3);
    uint8_t *out = (uint8_t *)hle_ptr(p);
    memset(out, 0, n);
    if (d->kind == DEV_MOUSE) {
        int32_t lx, ly;
        uint8_t b[4];
        mouse_state(d, &lx, &ly, b);
        if (n >= 12) { MEM32(p) = (uint32_t)lx; MEM32(p + 4) = (uint32_t)ly; MEM32(p + 8) = 0; }
        if (n >= 16) memcpy(out + 12, b, 4);              /* DIMOUSESTATE; DIMOUSESTATE2 has 8 buttons, the rest 0 */
        if (g_dinput_trace) fprintf(stderr, "[dinput] mouse %d,%d buttons %02X%02X%02X\n", lx, ly, b[0], b[1], b[2]);
    } else {
        uint8_t k[DIK_MAX];
        keyboard_state(k);
        memcpy(out, k, n < DIK_MAX ? n : DIK_MAX);
    }
    RET(DI_OK, 3);
}
/* Buffered data: the keys and buttons that changed since the last read, one
 * DIDEVICEOBJECTDATA each (dwOfs, dwData, dwTimeStamp, dwSequence; DirectX 7
 * adds uAppData, the caller's cbObjectData says which). A read with no
 * buffer (rgdod NULL) flushes. */
static void dev_GetDeviceData(void) {                     /* (this, cbObjectData, rgdod, pdwInOut, dwFlags) */
    didev_t *d = D(A32(0));
    uint32_t cb = A32(1), rgdod = A32(2), pn = A32(3), flags = A32(4);
    if (!pn || (cb != 16 && cb != 20)) RET(DIERR_INVALIDPARAM, 5);
    if (!d->acquired) RET(DIERR_NOTACQUIRED, 5);
    uint32_t room = MEM32(pn), n = 0;
    uint8_t now[DIK_MAX];
    if (d->kind == DEV_MOUSE) {
        int32_t lx, ly;
        memset(now, 0, sizeof now);
        mouse_state(d, &lx, &ly, now);                    /* buttons at 0..3; axes not buffered */
    } else {
        keyboard_state(now);
    }
    uint32_t t = hle_ticks_ms();
    for (int i = 0; i < DIK_MAX; i++) {
        if (now[i] == d->snap[i]) continue;
        if (rgdod && n < room) {
            uint32_t e = rgdod + n * cb;
            MEM32(e) = d->kind == DEV_MOUSE ? 12u + (uint32_t)i : (uint32_t)i;   /* DIMOFS_BUTTON0.. or the DIK */
            MEM32(e + 4) = now[i];
            MEM32(e + 8) = t;
            MEM32(e + 12) = ++d->seq;
            if (cb == 20) MEM32(e + 16) = 0;
        }
        if (rgdod && n >= room) break;                    /* the rest waits for the next read */
        if (!(flags & 1u) || !rgdod) d->snap[i] = now[i]; /* DIGDD_PEEK leaves them for next time */
        n++;
    }
    MEM32(pn) = rgdod ? n : 0;
    RET(DI_OK, 5);
}
static void dev_SetDataFormat(void) {                     /* (this, lpdf) */
    didev_t *d = D(A32(0));
    uint32_t f = A32(1);
    if (f) d->data_size = MEM32(f + 12);                  /* DIDATAFORMAT: dwSize, dwObjSize, dwFlags, dwDataSize */
    RET(DI_OK, 2);
}
static void dev_SetEventNotification(void) { RET(DI_OK, 2); }
static void dev_SetCooperativeLevel(void) { RET(DI_OK, 3); }
static void dev_GetObjectInfo(void) { RET(DIERR_NOTFOUND, 4); }
static void dev_GetDeviceInfo(void) {                     /* (this, pdidi) */
    didev_t *d = D(A32(0));
    uint32_t i = A32(1);
    if (!i) RET(DIERR_INVALIDPARAM, 2);
    uint32_t size = MEM32(i);
    if (size < 40) RET(DIERR_INVALIDPARAM, 2);
    uint8_t *p = (uint8_t *)hle_ptr(i);
    memset(p + 4, 0, size - 4);
    memcpy(p + 4, d->kind == DEV_MOUSE ? GUID_SysMouse : GUID_SysKeyboard, 16);
    memcpy(p + 20, d->kind == DEV_MOUSE ? GUID_SysMouse : GUID_SysKeyboard, 16);
    *(uint32_t *)(p + 36) = d->kind == DEV_MOUSE ? (DIDEVTYPE_MOUSE | (2u << 8)) : (DIDEVTYPE_KEYBOARD | (4u << 8));
    if (size >= 40 + 260 + 260) {
        strcpy((char *)p + 40, d->kind == DEV_MOUSE ? "Mouse" : "Keyboard");
        strcpy((char *)p + 300, d->kind == DEV_MOUSE ? "Mouse" : "Keyboard");
    }
    RET(DI_OK, 2);
}
static void dev_RunControlPanel(void) { RET(DI_OK, 3); }
static void dev_Initialize(void) { RET(DI_OK, 4); }
/* IDirectInputDevice2: force feedback and effects, none here */
static void dev_CreateEffect(void) { RET(DIERR_UNSUPPORTED, 5); }
static void dev_EnumEffects(void) { RET(DI_OK, 4); }
static void dev_GetEffectInfo(void) { RET(DIERR_UNSUPPORTED, 3); }
static void dev_GetForceFeedbackState(void) { RET(DIERR_UNSUPPORTED, 2); }
static void dev_SendForceFeedbackCommand(void) { RET(DIERR_UNSUPPORTED, 2); }
static void dev_EnumCreatedEffectObjects(void) { RET(DI_OK, 4); }
static void dev_Escape(void) { RET(DIERR_UNSUPPORTED, 2); }
static void dev_Poll(void) { RET(DI_NOEFFECT, 1); }
static void dev_SendDeviceData(void) { if (A32(3)) MEM32(A32(3)) = 0; RET(DI_OK, 5); }
/* IDirectInputDevice7 */
static void dev_EnumEffectsInFile(void) { RET(DIERR_UNSUPPORTED, 5); }
static void dev_WriteEffectToFile(void) { RET(DIERR_UNSUPPORTED, 5); }

static uint32_t new_device(int kind) {
    uint32_t obj = hle_com_new(g_vt_dev, 8 + sizeof(didev_t));
    didev_t *d = D(obj);
    d->kind = kind;
    d->buffer_size = 0;
    return obj;
}

/* ---- IDirectInput ---- */
static void di_CreateDevice(void) {                       /* (this, rguid, lplpDirectInputDevice, pUnkOuter) */
    const uint8_t *g = (const uint8_t *)APTR(1);
    uint32_t out = A32(2);
    if (!g || !out) RET(DIERR_INVALIDPARAM, 4);
    int kind = !memcmp(g, GUID_SysMouse, 16) ? DEV_MOUSE : !memcmp(g, GUID_SysKeyboard, 16) ? DEV_KEYBOARD : 0;
    if (g_dinput_trace || win32hle_trace)
        fprintf(stderr, "[dinput] CreateDevice -> %s\n", kind == DEV_MOUSE ? "mouse" : kind == DEV_KEYBOARD ? "keyboard" : "no such device");
    if (!kind) { MEM32(out) = 0; RET(DIERR_DEVICENOTREG, 4); }
    MEM32(out) = new_device(kind);
    RET(DI_OK, 4);
}
/* The callback takes (LPCDIDEVICEINSTANCEA, LPVOID) and returns DIENUM_STOP (0) or CONTINUE (1). */
static int enum_one(uint32_t cb, uint32_t ref, int kind) {
    uint32_t mark = hle_scratch_mark();
    uint32_t inst = hle_scratch(40 + 260 + 260);
    uint8_t *p = (uint8_t *)hle_ptr(inst);
    *(uint32_t *)p = 40 + 260 + 260;
    memcpy(p + 4, kind == DEV_MOUSE ? GUID_SysMouse : GUID_SysKeyboard, 16);
    memcpy(p + 20, kind == DEV_MOUSE ? GUID_SysMouse : GUID_SysKeyboard, 16);
    *(uint32_t *)(p + 36) = kind == DEV_MOUSE ? (DIDEVTYPE_MOUSE | (2u << 8)) : (DIDEVTYPE_KEYBOARD | (4u << 8));
    strcpy((char *)p + 40, kind == DEV_MOUSE ? "Mouse" : "Keyboard");
    strcpy((char *)p + 300, kind == DEV_MOUSE ? "Mouse" : "Keyboard");
    uint32_t args[2] = { inst, ref };
    uint32_t go = hle_call_guest(cb, 2, args);
    hle_scratch_release(mark);
    return go != 0;
}
static void di_EnumDevices(void) {                        /* (this, dwDevType, callback, pvRef, dwFlags) */
    uint32_t type = A32(1), cb = A32(2), ref = A32(3);
    if (!cb) RET(DIERR_INVALIDPARAM, 5);
    if ((type == 0 || type == DIDEVTYPE_MOUSE) && !enum_one(cb, ref, DEV_MOUSE)) RET(DI_OK, 5);
    if (type == 0 || type == DIDEVTYPE_KEYBOARD) enum_one(cb, ref, DEV_KEYBOARD);
    RET(DI_OK, 5);                                        /* joysticks: none to name */
}
static void di_GetDeviceStatus(void) {                    /* (this, rguid) */
    const uint8_t *g = (const uint8_t *)APTR(1);
    RET(g && (!memcmp(g, GUID_SysMouse, 16) || !memcmp(g, GUID_SysKeyboard, 16)) ? DI_OK : DIERR_DEVICENOTREG, 2);
}
static void di_RunControlPanel(void) { RET(DI_OK, 3); }
static void di_Initialize(void) { RET(DI_OK, 3); }
static void di_FindDevice(void) { RET(DIERR_DEVICENOTREG, 4); }        /* IDirectInput2 */
static void di_CreateDeviceEx(void) {                     /* IDirectInput7: (this, rguid, riid, lplpObj, pUnkOuter) */
    const uint8_t *g = (const uint8_t *)APTR(1);
    uint32_t out = A32(3);
    if (!g || !out) RET(DIERR_INVALIDPARAM, 5);
    int kind = !memcmp(g, GUID_SysMouse, 16) ? DEV_MOUSE : !memcmp(g, GUID_SysKeyboard, 16) ? DEV_KEYBOARD : 0;
    if (!kind) { MEM32(out) = 0; RET(DIERR_DEVICENOTREG, 5); }
    MEM32(out) = new_device(kind);
    RET(DI_OK, 5);
}

static void build(void) {
    static const win32hle_shim di[] = {
        { "IDirectInput::QueryInterface", hle_com_QueryInterface }, { "IDirectInput::AddRef", hle_com_AddRef },
        { "IDirectInput::Release", hle_com_Release }, { "IDirectInput::CreateDevice", di_CreateDevice },
        { "IDirectInput::EnumDevices", di_EnumDevices }, { "IDirectInput::GetDeviceStatus", di_GetDeviceStatus },
        { "IDirectInput::RunControlPanel", di_RunControlPanel }, { "IDirectInput::Initialize", di_Initialize },
        { "IDirectInput2::FindDevice", di_FindDevice }, { "IDirectInput7::CreateDeviceEx", di_CreateDeviceEx },
        { 0, 0 } };
    static const win32hle_shim dev[] = {
        { "IDirectInputDevice::QueryInterface", hle_com_QueryInterface }, { "IDirectInputDevice::AddRef", hle_com_AddRef },
        { "IDirectInputDevice::Release", hle_com_Release }, { "IDirectInputDevice::GetCapabilities", dev_GetCapabilities },
        { "IDirectInputDevice::EnumObjects", dev_EnumObjects }, { "IDirectInputDevice::GetProperty", dev_GetProperty },
        { "IDirectInputDevice::SetProperty", dev_SetProperty }, { "IDirectInputDevice::Acquire", dev_Acquire },
        { "IDirectInputDevice::Unacquire", dev_Unacquire }, { "IDirectInputDevice::GetDeviceState", dev_GetDeviceState },
        { "IDirectInputDevice::GetDeviceData", dev_GetDeviceData }, { "IDirectInputDevice::SetDataFormat", dev_SetDataFormat },
        { "IDirectInputDevice::SetEventNotification", dev_SetEventNotification }, { "IDirectInputDevice::SetCooperativeLevel", dev_SetCooperativeLevel },
        { "IDirectInputDevice::GetObjectInfo", dev_GetObjectInfo }, { "IDirectInputDevice::GetDeviceInfo", dev_GetDeviceInfo },
        { "IDirectInputDevice::RunControlPanel", dev_RunControlPanel }, { "IDirectInputDevice::Initialize", dev_Initialize },
        { "IDirectInputDevice2::CreateEffect", dev_CreateEffect }, { "IDirectInputDevice2::EnumEffects", dev_EnumEffects },
        { "IDirectInputDevice2::GetEffectInfo", dev_GetEffectInfo }, { "IDirectInputDevice2::GetForceFeedbackState", dev_GetForceFeedbackState },
        { "IDirectInputDevice2::SendForceFeedbackCommand", dev_SendForceFeedbackCommand }, { "IDirectInputDevice2::EnumCreatedEffectObjects", dev_EnumCreatedEffectObjects },
        { "IDirectInputDevice2::Escape", dev_Escape }, { "IDirectInputDevice2::Poll", dev_Poll },
        { "IDirectInputDevice2::SendDeviceData", dev_SendDeviceData }, { "IDirectInputDevice7::EnumEffectsInFile", dev_EnumEffectsInFile },
        { "IDirectInputDevice7::WriteEffectToFile", dev_WriteEffectToFile },
        { 0, 0 } };
    if (!g_vt_di) g_vt_di = hle_com_vtable(di);
    if (!g_vt_dev) g_vt_dev = hle_com_vtable(dev);
}

/* DirectInputCreateA(hinst, dwVersion, lplpDirectInput, punkOuter) */
static void d_DirectInputCreateA(void) {
    uint32_t out = A32(2);
    if (!out) RET(DIERR_INVALIDPARAM, 4);
    build();
    if (!g_di) g_di = hle_com_new(g_vt_di, 8);
    else MEM32(g_di + 4)++;
    MEM32(out) = g_di;
    if (win32hle_trace) fprintf(stderr, "[dinput] DirectInputCreateA version %04X\n", A32(1));
    RET(DI_OK, 4);
}
/* DirectInputCreateEx(hinst, dwVersion, riid, lplpDirectInput, punkOuter): every IDirectInput is this one */
static void d_DirectInputCreateEx(void) {
    uint32_t out = A32(3);
    if (!out) RET(DIERR_INVALIDPARAM, 5);
    build();
    if (!g_di) g_di = hle_com_new(g_vt_di, 8);
    else MEM32(g_di + 4)++;
    MEM32(out) = g_di;
    RET(DI_OK, 5);
}

static const win32hle_shim g_dinput_shims[] = {
    { "DirectInputCreateA",  d_DirectInputCreateA },
    { "DirectInputCreateW",  d_DirectInputCreateA },
    { "DirectInputCreateEx", d_DirectInputCreateEx },
    { 0, 0 } };

void win32hle_register_dinput(void) { win32hle_register(g_dinput_shims); }
