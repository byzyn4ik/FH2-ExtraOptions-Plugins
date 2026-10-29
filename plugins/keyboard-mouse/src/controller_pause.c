// Keyboard & Mouse for Forza Horizon 2 (Extra Options plugin).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "controller_pause.h"
#include "patch.h"

#include <stdio.h>

/* The pause flow's WaitForPause behaviour (vtable 0x2d2f6f8, per-frame slot
   14, 0x964810) fires onPause (0x1925260(behavior, behavior+0x28) at 0x9648fa)
   whenever the signed-in user's controller (user->vt+0x120(0)) reports itself
   not connected ([controller+4] == 0): the console's "controller lost: pause"
   rule. With no pad paired to the signed-in user (playing without a pad, or a
   pad the game never paired) that rule fires again on every close of the
   pause hub, so the game pauses over and over. With the option on, the fire is
   kept only for a real unplug: a pad that was attached and is now gone pauses
   once. Esc, Menu and every other pause request are untouched. The call is
   pointed at an absolute jump near the game (patch.c), and the hook calls
   what the call went to before (the game's 0x1925260 or another module's
   hook). */
#define PAUSE_CHECK 0x9648edu
#define PAUSE_FIRE_CALL 0x9648fau
#define PAUSE_FIRE 0x1925260u

/* CControllerManager: eight pad slots; a slot's gamepad is set while a pad is
   attached to it. */
#define MANAGER_RVA 0x365d7c8u
#define MANAGER_VTABLE_RVA 0x2e7a2e8u
#define MANAGER_FIRST_CONTROLLER 0x10u
#define CONTROLLER_STRIDE 0x1300u
#define CONTROLLER_SLOTS 8u
#define CONTROLLER_GAMEPAD 0x60u

typedef void (*TransitionFireFn)(void *, void *);

/* cmp byte [rbx+4],0; jne +0xc; lea rdx,[rdi+0x28]; mov rcx,rdi */
static const unsigned char pause_check_bytes[] = {0x80, 0x7b, 0x04, 0x00, 0x75, 0x0c, 0x48,
                                                  0x8d, 0x57, 0x28, 0x48, 0x8b, 0xcf};
static unsigned char *image;
static int (*suppressed)(void);
static volatile LONG installed, unplug_only;
static void *previous_fire;

static int any_pad_attached(void) {
    unsigned char *owner;
    void *vtable;
    if (!memory_read_pointer(image + MANAGER_RVA, (void **)&owner) ||
        !memory_read_pointer(owner, &vtable) || vtable != image + MANAGER_VTABLE_RVA) {
        return 0;
    }
    for (unsigned index = 0; index < CONTROLLER_SLOTS; ++index) {
        void *gamepad = NULL;
        if (memory_read(owner + MANAGER_FIRST_CONTROLLER + (size_t)index * CONTROLLER_STRIDE +
                            CONTROLLER_GAMEPAD,
                        &gamepad, sizeof(gamepad)) &&
            gamepad) {
            return 1;
        }
    }
    return 0;
}

static void pause_fire_hook(void *behavior, void *transition) {
    /* Keyboard & mouse active with no pad: never a controller-missing pause. */
    if (suppressed && suppressed()) {
        static int logged;
        if (!logged) {
            logged = 1;
            log_append("CONTROLLER PAUSE: controller-missing pause suppressed (keyboard & mouse, "
                       "no pad).\r\n");
        }
        return;
    }
    static int pad_was_attached = -1;
    int attached = any_pad_attached();
    int unplugged = pad_was_attached == 1 && !attached;
    pad_was_attached = attached;
    if (unplug_only && !unplugged) {
        static int logged;
        if (!logged) {
            logged = 1;
            log_append("CONTROLLER PAUSE: controller-missing pause skipped (no pad was unplugged).\r\n");
        }
        return;
    }
    ((TransitionFireFn)previous_fire)(behavior, transition);
}

void controller_pause_set_suppressed(int (*callback)(void)) {
    suppressed = callback;
}

int controller_pause_install(unsigned char *game_image, int only_on_unplug) {
    if (only_on_unplug) {
        InterlockedExchange(&unplug_only, 1);
    }
    if (installed) {
        return 1;
    }
    image = game_image;
    if (!memory_equal(image + PAUSE_CHECK, pause_check_bytes, sizeof(pause_check_bytes))) {
        log_append("CONTROLLER PAUSE: WaitForPause controller check 0x9648ed changed; the "
                   "controller-missing pause is left alone.\r\n");
        return 0;
    }
    const char *problem = patch_call(image + PAUSE_FIRE_CALL, image + PAUSE_FIRE, image, GAME_IMAGE_SIZE,
                                     (void *)(uintptr_t)pause_fire_hook, &previous_fire);
    if (problem) {
        log_line("CONTROLLER PAUSE: the onPause call 0x9648fa could not be chained (%s); the controller-missing "
                 "pause is left alone.", problem);
        return 0;
    }
    InterlockedExchange(&installed, 1);
    char where[128] = "the game's 0x1925260";
    if (previous_fire != image + PAUSE_FIRE) {
        char module[96];
        patch_describe(previous_fire, module, sizeof(module));
        snprintf(where, sizeof(where), "a hook at %s", module);
    }
    log_line(unplug_only ? "CONTROLLER PAUSE: the onPause call 0x9648fa chained onto %s; the controller-missing "
                           "pause now needs a real unplug."
                         : "CONTROLLER PAUSE: the onPause call 0x9648fa chained onto %s; the controller-missing "
                           "pause is skipped only while keyboard & mouse is active with no pad.",
             where);
    return 1;
}

#ifdef KBM_TEST
__declspec(dllexport) void *KbmTestPauseHook(void **previous) {
    *previous = previous_fire;
    return (void *)(uintptr_t)pause_fire_hook;
}
#endif

#ifdef KBM_TEST
__declspec(dllexport) int KbmTestPauseInstall(unsigned char *game_image, int unplug_only, int (*callback)(void)) {
    controller_pause_set_suppressed(callback);
    return controller_pause_install(game_image, unplug_only);
}
#endif
