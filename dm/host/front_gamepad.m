// Mac front end: game controllers (GameController.framework) → dm_pad_set, polled 250 times a second on a host thread.
// Face buttons follow the Switch layout by position (bottom = B, right = A, left = Y, top = X) on every controller, as MacNativeGames does: the game's prompts then match
// the button where it sits. DM_FACE_BUTTONS=label maps Xbox / PlayStation controllers by their label instead (Xbox A = Switch A).
// Without this file the Mac host read only the keyboard: Dark Souls Remastered ignored a connected DualSense / Xbox controller.
#import <Foundation/Foundation.h>
#import <GameController/GameController.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include "dm_input.h"

static int32_t ax(float v) { if (v > 1) v = 1; if (v < -1) v = -1; return (int32_t)(v * DM_STICK_MAX); }
static void *pad_main(void *arg) {
    (void)arg;
    const char *fb = getenv("DM_FACE_BUTTONS"); int by_label = fb && !strcmp(fb, "label");
    uint64_t last = ~0ull; int32_t lastax[4] = {0};
    for (;;) {
        @autoreleasepool {
            GCController *c = GCController.current;
            if (!c) for (GCController *k in GCController.controllers) if (k.extendedGamepad) { c = k; break; }
            GCExtendedGamepad *g = c.extendedGamepad;
            uint64_t b = 0; int32_t a[4] = {0};
            if (g) {
                if (by_label) {
                    if (g.buttonA.pressed) b |= DM_BTN_A; if (g.buttonB.pressed) b |= DM_BTN_B; if (g.buttonX.pressed) b |= DM_BTN_X; if (g.buttonY.pressed) b |= DM_BTN_Y;
                } else {   // by position: GameController's A is the bottom button, B the right one, X the left one, Y the top one
                    if (g.buttonA.pressed) b |= DM_BTN_B; if (g.buttonB.pressed) b |= DM_BTN_A; if (g.buttonX.pressed) b |= DM_BTN_Y; if (g.buttonY.pressed) b |= DM_BTN_X;
                }
                if (g.leftShoulder.pressed) b |= DM_BTN_L; if (g.rightShoulder.pressed) b |= DM_BTN_R;
                if (g.leftTrigger.pressed) b |= DM_BTN_ZL; if (g.rightTrigger.pressed) b |= DM_BTN_ZR;
                if (g.leftThumbstickButton.pressed) b |= DM_BTN_LS; if (g.rightThumbstickButton.pressed) b |= DM_BTN_RS;
                if (g.buttonMenu.pressed) b |= DM_BTN_PLUS; if (g.buttonOptions.pressed) b |= DM_BTN_MINUS;
                if (g.dpad.up.pressed) b |= DM_BTN_UP; if (g.dpad.down.pressed) b |= DM_BTN_DOWN; if (g.dpad.left.pressed) b |= DM_BTN_LEFT; if (g.dpad.right.pressed) b |= DM_BTN_RIGHT;
                a[0] = ax(g.leftThumbstick.xAxis.value); a[1] = ax(g.leftThumbstick.yAxis.value);
                a[2] = ax(g.rightThumbstick.xAxis.value); a[3] = ax(g.rightThumbstick.yAxis.value);
            }
            if (b != last || memcmp(a, lastax, sizeof a)) { dm_pad_set(b, a[0], a[1], a[2], a[3]); last = b; memcpy(lastax, a, sizeof a); }
        }
        usleep(4000);
    }
    return NULL;
}
void front_gamepad_start(void) {
    static int started; if (started++) return;
    GCController.shouldMonitorBackgroundEvents = YES;   // keep reading the controller when the window is not in front
    [[NSNotificationCenter defaultCenter] addObserverForName:GCControllerDidConnectNotification object:nil queue:nil usingBlock:^(NSNotification *n) {
        GCController *c = n.object; fprintf(stderr, "[dm] controller connected: %s\n", c.vendorName.UTF8String ?: "?"); }];
    [GCController startWirelessControllerDiscoveryWithCompletionHandler:nil];
    pthread_t t; pthread_create(&t, NULL, pad_main, NULL); pthread_detach(t);
}
