/*
 * hw/kestrel/kestrel.c -- XLibre display server backend (DDX) for Kestrel
 *
 * One X screen per Kestrel monitor (/dev/fb0, /dev/fb1, ...). Each screen is
 * rendered in software by fb/ into an ordinary memory buffer; damage
 * tracking collects what changed, and before the server goes idle (block
 * handler) the changed boxes are handed to the kernel with KFB_BLIT, which
 * draws them on the monitor whatever its driver (boot frame buffer, Bochs
 * DISPI, NVIDIA through PRAMIN). KDSETMODE(KD_GRAPHICS) stops the kernel
 * terminal on that monitor while the server runs; it comes back by itself
 * when the server exits.
 *
 * Input comes from /dev/kinput: Linux-style key and relative-pointer events
 * (struct kinput_event), keycodes in the evdev numbering, so the default
 * "evdev" XKB rules apply unchanged.
 *
 * Based on hw/vfb (Copyright 1993, 1998 The Open Group; MIT/X Consortium
 * licence, see COPYING). Kestrel parts: MIT licence.
 */
#include <dix-config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <X11/X.h>
#include <X11/Xproto.h>
#include <X11/Xos.h>

#include "dix/colormap_priv.h"
#include "dix/dix_priv.h"
#include "dix/input_priv.h"
#include "dix/inpututils_priv.h"
#include "dix/screenint_priv.h"
#include "include/extinit.h"
#include "mi/mi_priv.h"
#include "mi/mipointer_priv.h"
#include "miext/extinit_priv.h"
#include "os/cmdline.h"
#include "os/ddx_priv.h"
#include "os/osdep.h"

#include "scrnintstr.h"
#include "servermd.h"
#define PSZ 8
#include "fb.h"
#include "gcstruct.h"
#include "input.h"
#include "inputstr.h"
#include "mipointer.h"
#include "micmap.h"
#include "damage.h"
#include "dixstruct.h"
#include "xkbsrv.h"
#include "xserver-properties.h"
#include "exevents.h"
#include "globals.h"

/* ---- kernel interfaces (Kestrel include/kernel/kfb.h, kinput.h) ---------- */
#define KFB_GET_INFO    0x4b00
#define KFB_BLIT        0x4b01
#define KDSETMODE       0x4b3a
#define KD_TEXT         0x00
#define KD_GRAPHICS     0x01

struct kfb_info {
    uint32_t width, height, pitch, bpp;
    uint8_t  r_pos, g_pos, b_pos, index;
    char     name[32];
    char     monitor[16];
};

struct kfb_blit {
    int32_t  x, y, w, h;
    uint64_t src;
    uint32_t src_pitch, reserved;
};

struct kinput_event {
    uint64_t time_us;
    uint16_t type, code;
    int32_t  value;
};
#define KINPUT_GRAB     0x4b10
#define KI_EV_KEY       1
#define KI_EV_REL       2
#define KI_REL_X        0
#define KI_REL_Y        1
#define KI_REL_WHEEL    8
#define KI_BTN_LEFT     0x110
#define KI_BTN_RIGHT    0x111
#define KI_BTN_MIDDLE   0x112

/* ---- screens ---------------------------------------------------------------- */
#define KMAX_SCREENS    4

typedef struct {
    int         fd;
    struct kfb_info info;
    uint8_t    *bits;           /* the screen pixmap, 32 bpp */
    int         stride;         /* bytes */
    DamagePtr   damage;
    CloseScreenProcPtr closeScreen;
    CreateScreenResourcesProcPtr createScreenResources;
    ScreenBlockHandlerProcPtr blockHandler;
    uint64_t    blits, pixels;
} KScreen;

static KScreen kscreens[KMAX_SCREENS];
static int knum;
static int kwant = KMAX_SCREENS;        /* -screens N */

static void
kOpenScreens(void)
{
    if (knum)
        return;
    for (int i = 0; i < KMAX_SCREENS && knum < kwant; i++) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/fb%d", i);
        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0)
            break;
        KScreen *k = &kscreens[knum];
        if (ioctl(fd, KFB_GET_INFO, &k->info) < 0 || k->info.bpp != 32) {
            ErrorF("kestrel: %s: unusable (%s)\n", path, strerror(errno));
            close(fd);
            continue;
        }
        k->fd = fd;
        LogMessage(X_INFO, "kestrel: %s: %ux%u, R/G/B at bits %u/%u/%u, %s%s%s\n", path,
                   k->info.width, k->info.height, k->info.r_pos, k->info.g_pos, k->info.b_pos,
                   k->info.name, k->info.monitor[0] ? ", monitor " : "", k->info.monitor);
        knum++;
    }
    if (!knum)
        FatalError("kestrel: no monitor (/dev/fb0) available\n");
}

/* Hand every changed box to the kernel, then forget the damage. */
static void
kFlush(KScreen *k)
{
    RegionPtr r = DamageRegion(k->damage);
    int n = RegionNumRects(r);
    BoxPtr b = RegionRects(r);

    for (int i = 0; i < n; i++, b++) {
        struct kfb_blit bl = {
            .x = b->x1, .y = b->y1, .w = b->x2 - b->x1, .h = b->y2 - b->y1,
            .src = (uint64_t)(uintptr_t)(k->bits + (size_t)b->y1 * k->stride + (size_t)b->x1 * 4),
            .src_pitch = (uint32_t)k->stride,
        };
        if (bl.w <= 0 || bl.h <= 0)
            continue;
        if (ioctl(k->fd, KFB_BLIT, &bl) < 0)
            ErrorF("kestrel: blit %dx%d+%d+%d failed: %s\n", bl.w, bl.h, bl.x, bl.y, strerror(errno));
        k->blits++;
        k->pixels += (uint64_t)bl.w * bl.h;
    }
    DamageEmpty(k->damage);
}

/* Push damage to the display just before the server sleeps. This wraps the
 * screen's BlockHandler rather than using RegisterBlockAndWakeupHandlers():
 * dix runs those first, while the software cursor (misprite) puts the pointer
 * back on screen in its own screen BlockHandler, which then calls down to
 * this one -- so flushing here includes the cursor. */
static void
kBlockHandler(ScreenPtr pScreen, void *timeout)
{
    KScreen *k = &kscreens[pScreen->myNum];

    pScreen->BlockHandler = k->blockHandler;
    if (pScreen->BlockHandler)
        pScreen->BlockHandler(pScreen, timeout);
    k->blockHandler = pScreen->BlockHandler;
    pScreen->BlockHandler = kBlockHandler;

    if (k->damage)
        kFlush(k);
}

static Bool
kCreateScreenResources(ScreenPtr pScreen)
{
    KScreen *k = &kscreens[pScreen->myNum];

    pScreen->CreateScreenResources = k->createScreenResources;
    if (!pScreen->CreateScreenResources(pScreen))
        return FALSE;
    PixmapPtr root = pScreen->GetScreenPixmap(pScreen);
    k->damage = DamageCreate(NULL, NULL, DamageReportNone, TRUE, pScreen, root);
    if (!k->damage)
        return FALSE;
    DamageRegister(&root->drawable, k->damage);
    /* the whole screen once: the server starts with its root weave */
    RegionRec all;
    BoxRec box = { 0, 0, (short)k->info.width, (short)k->info.height };
    RegionInit(&all, &box, 1);
    DamageReportDamage(k->damage, &all);
    RegionUninit(&all);
    return TRUE;
}

static Bool
kCloseScreen(ScreenPtr pScreen)
{
    KScreen *k = &kscreens[pScreen->myNum];

    pScreen->CloseScreen = k->closeScreen;
    pScreen->BlockHandler = k->blockHandler;
    if (k->damage) {
        DamageUnregister(k->damage);
        DamageDestroy(k->damage);
        k->damage = NULL;
    }
    dixDestroyPixmap(pScreen->devPrivate, 0);
    pScreen->devPrivate = NULL;
    LogMessage(X_INFO, "kestrel: screen %d: %llu blits, %llu pixels sent\n", pScreen->myNum,
               (unsigned long long)k->blits, (unsigned long long)k->pixels);
    return pScreen->CloseScreen(pScreen);
}

/* The pointer moves from monitor to monitor left to right, in /dev/fbN order. */
static Bool
kCursorOffScreen(ScreenPtr *ppScreen, int *x, int *y)
{
    int n = (*ppScreen)->myNum;

    if (*x < 0 && n > 0) {
        *ppScreen = screenInfo.screens[n - 1];
        *x += (*ppScreen)->width;
        return TRUE;
    }
    if (*x >= (*ppScreen)->width && n + 1 < screenInfo.numScreens) {
        *x -= (*ppScreen)->width;
        *ppScreen = screenInfo.screens[n + 1];
        return TRUE;
    }
    return FALSE;
}

static void
kCrossScreen(ScreenPtr pScreen, Bool entering)
{
}

static miPointerScreenFuncRec kPointerCursorFuncs = {
    kCursorOffScreen,
    kCrossScreen,
    miPointerWarpCursor
};

static bool
kScreenInit(ScreenPtr pScreen, int argc, char **argv, void *closure)
{
    KScreen *k = closure;
    int dpi = monitorResolution ? monitorResolution : 96;

    k->stride = (int)PixmapBytePad((int)k->info.width, 24);
    k->bits = calloc((size_t)k->stride, k->info.height);
    if (!k->bits)
        return FALSE;

    if (!miSetVisualTypesAndMasks(24, (1 << TrueColor) | (1 << DirectColor), 8, TrueColor,
                                  0xffu << k->info.r_pos, 0xffu << k->info.g_pos, 0xffu << k->info.b_pos))
        return FALSE;
    miSetPixmapDepths();

    if (!fbScreenInit(pScreen, k->bits, (int)k->info.width, (int)k->info.height, dpi, dpi,
                      k->stride / 4, 32))
        return FALSE;
    if (!noRenderExtension)
        fbPictureInit(pScreen, 0, 0);

    miDCInitialize(pScreen, &kPointerCursorFuncs);
    pScreen->blackPixel = 0;
    pScreen->whitePixel = (0xffu << k->info.r_pos) | (0xffu << k->info.g_pos) | (0xffu << k->info.b_pos);

    k->createScreenResources = pScreen->CreateScreenResources;
    pScreen->CreateScreenResources = kCreateScreenResources;
    k->closeScreen = pScreen->CloseScreen;
    pScreen->CloseScreen = kCloseScreen;
    k->blockHandler = pScreen->BlockHandler;
    pScreen->BlockHandler = kBlockHandler;

    if (!fbCreateDefColormap(pScreen))
        return FALSE;

    if (ioctl(k->fd, KDSETMODE, KD_GRAPHICS) < 0)
        ErrorF("kestrel: KD_GRAPHICS on screen %d: %s\n", pScreen->myNum, strerror(errno));
    return TRUE;
}

/* ---- DDX entry points ---------------------------------------------------------- */
void
InitOutput(int argc, char **argv)
{
    static const int depths[] = { 1, 4, 8, 15, 16, 24, 32 };
    int n = 0;

    kOpenScreens();
    if (!monitorResolution)
        monitorResolution = 96;

    for (size_t i = 0; i < sizeof(depths) / sizeof(depths[0]); i++) {
        screenInfo.formats[n].depth = depths[i];
        screenInfo.formats[n].bitsPerPixel = depths[i] == 1 ? 1 : depths[i] <= 8 ? 8 :
                                             depths[i] <= 16 ? 16 : 32;
        screenInfo.formats[n].scanlinePad = BITMAP_SCANLINE_PAD;
        n++;
    }
    screenInfo.imageByteOrder = IMAGE_BYTE_ORDER;
    screenInfo.bitmapScanlineUnit = BITMAP_SCANLINE_UNIT;
    screenInfo.bitmapScanlinePad = BITMAP_SCANLINE_PAD;
    screenInfo.bitmapBitOrder = BITMAP_BIT_ORDER;
    screenInfo.numPixmapFormats = n;

    for (int i = 0; i < knum; i++)
        if (AddScreen(kScreenInit, argc, argv, &kscreens[i]) == -1)
            FatalError("kestrel: couldn't add screen %d\n", i);
}

void
ddxGiveUp(enum ExitCode error)
{
    for (int i = 0; i < knum; i++) {
        if (kscreens[i].fd > 0) {
            ioctl(kscreens[i].fd, KDSETMODE, KD_TEXT);
            close(kscreens[i].fd);
            kscreens[i].fd = -1;
        }
    }
}

void
ddxInit(void)
{
}

void
ddxFatalError(const char *f, va_list args)
{
}

void
ddxUseMsg(void)
{
    ErrorF("-screens N             use at most N monitors (default: all /dev/fbN)\n");
}

int
ddxProcessArgument(int argc, char *argv[], int i)
{
    if (strcmp(argv[i], "-screens") == 0) {
        CHECK_FOR_REQUIRED_ARGUMENTS(1);
        kwant = atoi(argv[i + 1]);
        if (kwant < 1 || kwant > KMAX_SCREENS)
            UseMsg();
        return 2;
    }
    return 0;
}

void
ddxBeforeReset(void)
{
}

#if INPUTTHREAD
void
ddxInputThreadInit(void)
{
}
#endif

/* ---- input --------------------------------------------------------------------- */
static DeviceIntPtr kKeyboard, kPointer;
static int kInputFd = -1;

void
ProcessInputEvents(void)
{
    mieqProcessInputEvents();
}

void
DDXRingBell(int volume, int pitch, int duration)
{
}

static void
kReadInput(int fd, int ready, void *data)
{
    struct kinput_event ev[64];
    ssize_t n;

    while ((n = read(fd, ev, sizeof(ev))) > 0) {
        int dx = 0, dy = 0;
        for (size_t i = 0; i < (size_t)n / sizeof(ev[0]); i++) {
            const struct kinput_event *e = &ev[i];
            if (e->type == KI_EV_KEY && e->code < 0x100 && kKeyboard) {
                if (e->value == 2)          /* autorepeat: X repeats by itself */
                    continue;
                QueueKeyboardEvents(kKeyboard, e->value ? KeyPress : KeyRelease, e->code + 8);
            }
            else if (e->type == KI_EV_KEY && kPointer) {
                int b = e->code == KI_BTN_LEFT ? 1 : e->code == KI_BTN_MIDDLE ? 2 :
                        e->code == KI_BTN_RIGHT ? 3 : 0;
                if (b) {
                    ValuatorMask mask;
                    valuator_mask_zero(&mask);
                    QueuePointerEvents(kPointer, e->value ? ButtonPress : ButtonRelease, b, 0, &mask);
                }
            }
            else if (e->type == KI_EV_REL && kPointer) {
                if (e->code == KI_REL_X) dx += e->value;
                else if (e->code == KI_REL_Y) dy += e->value;
                else if (e->code == KI_REL_WHEEL && e->value) {
                    ValuatorMask mask;
                    int b = e->value > 0 ? 4 : 5;
                    valuator_mask_zero(&mask);
                    QueuePointerEvents(kPointer, ButtonPress, b, 0, &mask);
                    QueuePointerEvents(kPointer, ButtonRelease, b, 0, &mask);
                }
            }
        }
        if (dx || dy) {
            ValuatorMask mask;
            valuator_mask_zero(&mask);
            valuator_mask_set(&mask, 0, dx);
            valuator_mask_set(&mask, 1, dy);
            QueuePointerEvents(kPointer, MotionNotify, 0, POINTER_RELATIVE | POINTER_ACCELERATE, &mask);
        }
    }
}

static int
kKeybdProc(DeviceIntPtr pDevice, int onoff)
{
    DevicePtr pDev = (DevicePtr) pDevice;

    switch (onoff) {
    case DEVICE_INIT:
        InitKeyboardDeviceStruct(pDevice, NULL, NULL, NULL);
        break;
    case DEVICE_ON:
        pDev->on = TRUE;
        break;
    case DEVICE_OFF:
        pDev->on = FALSE;
        break;
    case DEVICE_CLOSE:
        break;
    }
    return Success;
}

static int
kMouseProc(DeviceIntPtr pDevice, int onoff)
{
#define NBUTTONS 7
#define NAXES 2
    BYTE map[NBUTTONS + 1];
    DevicePtr pDev = (DevicePtr) pDevice;
    Atom btn_labels[NBUTTONS] = { 0 };
    Atom axes_labels[NAXES] = { 0 };

    switch (onoff) {
    case DEVICE_INIT:
        for (int i = 1; i <= NBUTTONS; i++)
            map[i] = i;
        btn_labels[0] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_LEFT);
        btn_labels[1] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_MIDDLE);
        btn_labels[2] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_RIGHT);
        btn_labels[3] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_WHEEL_UP);
        btn_labels[4] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_WHEEL_DOWN);
        btn_labels[5] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_HWHEEL_LEFT);
        btn_labels[6] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_HWHEEL_RIGHT);
        axes_labels[0] = XIGetKnownProperty(AXIS_LABEL_PROP_REL_X);
        axes_labels[1] = XIGetKnownProperty(AXIS_LABEL_PROP_REL_Y);
        InitPointerDeviceStruct(pDev, map, NBUTTONS, btn_labels, (PtrCtrlProcPtr) NoopDDA,
                                GetMotionHistorySize(), NAXES, axes_labels);
        break;
    case DEVICE_ON:
        pDev->on = TRUE;
        break;
    case DEVICE_OFF:
        pDev->on = FALSE;
        break;
    case DEVICE_CLOSE:
        break;
    }
    return Success;
#undef NBUTTONS
#undef NAXES
}

void
InitInput(int argc, char *argv[])
{
    kPointer = AddInputDevice(serverClient, kMouseProc, TRUE);
    kKeyboard = AddInputDevice(serverClient, kKeybdProc, TRUE);
    AssignTypeAndName(kPointer, dixAddAtom(XI_MOUSE), "Kestrel pointer");
    AssignTypeAndName(kKeyboard, dixAddAtom(XI_KEYBOARD), "Kestrel keyboard");
    (void) mieqInit();

    kInputFd = open("/dev/kinput", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (kInputFd < 0) {
        LogMessage(X_WARNING, "kestrel: /dev/kinput: %s (no keyboard or mouse)\n", strerror(errno));
        return;
    }
    /* Take keyboard and mouse away from the text consoles while we run. */
    int on = 1;
    if (ioctl(kInputFd, KINPUT_GRAB, &on) < 0)
        LogMessage(X_WARNING, "kestrel: cannot grab /dev/kinput: %s\n", strerror(errno));
    SetNotifyFd(kInputFd, kReadInput, X_NOTIFY_READ, NULL);
}

void
CloseInput(void)
{
    if (kInputFd >= 0) {
        RemoveNotifyFd(kInputFd);
        close(kInputFd);
        kInputFd = -1;
    }
    mieqFini();
}
