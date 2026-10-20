// Mac front end: game controllers (GameController.framework) → dm_pad_set.
// GameController delivers input on the main queue: each controller gets a valueChangedHandler there (as MacNativeGames' TotK launcher does). Polling the element values
// from a background thread read every button as released and the sticks at 0 (Dark Souls Remastered: the DualSense was connected but did nothing).
// Face buttons follow the Switch layout by position (bottom = B, right = A, left = Y, top = X) on every controller; DM_FACE_BUTTONS=label maps them by label.
#import <Foundation/Foundation.h>
#import <GameController/GameController.h>
#include <stdlib.h>
#include <string.h>
#include "dm_input.h"

static int g_by_label;
static int32_t ax(float v) { if (v > 1) v = 1; if (v < -1) v = -1; return (int32_t)(v * DM_STICK_MAX); }
static void publish(GCExtendedGamepad *g) {
    uint64_t b = 0; int32_t a[4] = {0};
    if (g) {
        if (g_by_label) { if (g.buttonA.pressed) b |= DM_BTN_A; if (g.buttonB.pressed) b |= DM_BTN_B; if (g.buttonX.pressed) b |= DM_BTN_X; if (g.buttonY.pressed) b |= DM_BTN_Y; }
        else { if (g.buttonA.pressed) b |= DM_BTN_B; if (g.buttonB.pressed) b |= DM_BTN_A; if (g.buttonX.pressed) b |= DM_BTN_Y; if (g.buttonY.pressed) b |= DM_BTN_X; }
        if (g.leftShoulder.pressed) b |= DM_BTN_L; if (g.rightShoulder.pressed) b |= DM_BTN_R;
        if (g.leftTrigger.pressed) b |= DM_BTN_ZL; if (g.rightTrigger.pressed) b |= DM_BTN_ZR;
        if (g.leftThumbstickButton.pressed) b |= DM_BTN_LS; if (g.rightThumbstickButton.pressed) b |= DM_BTN_RS;
        if (g.buttonMenu.pressed) b |= DM_BTN_PLUS; if (g.buttonOptions.pressed) b |= DM_BTN_MINUS;
        if (g.dpad.up.pressed) b |= DM_BTN_UP; if (g.dpad.down.pressed) b |= DM_BTN_DOWN; if (g.dpad.left.pressed) b |= DM_BTN_LEFT; if (g.dpad.right.pressed) b |= DM_BTN_RIGHT;
        a[0] = ax(g.leftThumbstick.xAxis.value); a[1] = ax(g.leftThumbstick.yAxis.value);
        a[2] = ax(g.rightThumbstick.xAxis.value); a[3] = ax(g.rightThumbstick.yAxis.value);
    }
    dm_pad_set(b, a[0], a[1], a[2], a[3]);
}
static void attach(GCController *c) {
    GCExtendedGamepad *g = c.extendedGamepad; if (!g) return;
    c.handlerQueue = dispatch_get_main_queue();
    g.valueChangedHandler = ^(GCExtendedGamepad *p, GCControllerElement *e) { static int n; if (n++ < 5) fprintf(stderr, "[dm] controller input: %s\n", e.localizedName.UTF8String ?: "?"); publish(p); };
    fprintf(stderr, "[dm] controller connected: %s\n", c.vendorName.UTF8String ?: "?");
}
void front_gamepad_start(void) {   // called on the main thread before the app's run loop starts
    static int started; if (started++) return;
    const char *fb = getenv("DM_FACE_BUTTONS"); g_by_label = fb && !strcmp(fb, "label");
    GCController.shouldMonitorBackgroundEvents = YES;
    NSNotificationCenter *nc = NSNotificationCenter.defaultCenter;
    [nc addObserverForName:GCControllerDidConnectNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *n) { attach(n.object); }];
    [nc addObserverForName:GCControllerDidDisconnectNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *n) { (void)n; publish(nil); }];
    for (GCController *c in GCController.controllers) attach(c);
    [GCController startWirelessControllerDiscoveryWithCompletionHandler:nil];
}
