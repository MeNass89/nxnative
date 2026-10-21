// Dark Souls Remastered 1.0.3 (main build df3766a2bb651a3e...): bonfire warp on request, for test agents (process/specs/WARP.md in switch-mac-native).
// The game's own warp path: main+0x123a24(obj = *(main+0x22b6d90), 1) is what the bonfire menu's "Warp" result runs (caller main+0x2fee00). With a bonfire id in GameMan+0xb14
// (GameMan = *(main+0x22c7930); PC DSR-Gadget calls it LastBonfire at +0xb24) it converts the id to the map (area = id/100000 % 100, block = id/10000 % 10, main+0x130428),
// stores the map in GameMan+0xc, sets the warp request GameMan+9 and starts the load; the player spawns at the bonfire entity of that id.
// The host calls it on the game's own thread: a brk at the entry of the in-game per-frame step main+0x6bb348 (probe.c), and when a request is due the trap sends the thread
// through a stub that calls the warp function and then resumes the step (caller-saved registers are dead at a function entry; x0 and lr are kept).
// Requests: DM_WARP=<id>[@<frame>],... (@ = absolute guest present frame; without @ the first runs 300 frames after the step first runs and each next one 900 frames after the previous),
// and a file polled every 30 frames (DM_WARP_FILE, default $DM_ROOT/run/warp.txt): it holds one bonfire id, is deleted when read, and the warp runs at once.
// DM_DSR_STATE=<n>: every n frames log the last bonfire, the target map and the warp flag. Off unless one of these is set.
#include "dm.h"
#include "hle.h"
#include <TargetConditionals.h>
#include <sys/ucontext.h>
#include <sys/mman.h>
#include <libkern/OSCacheControl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#if !TARGET_OS_IPHONE
void probe_add_fn(const char *mod, uint64_t off, void (*fn)(ucontext_t *uc));
enum { HOOK = 0x6bb348, WARP_FN = 0x123a24, OBJ_PTR = 0x22b6d90, GAMEMAN_PTR = 0x22c7930,
       GM_REQ = 0x9, GM_MAP = 0xc, GM_ENTRY = 0x10, GM_BONFIRE = 0xb14 };
static uint8_t *g_base; static uint32_t *g_stub;
enum { FIRST = 300, GAP = 900 };   // requests without @: first 300 frames after the step first runs (in game), then every 900 frames
static struct { int id, rel; uint64_t frame; } g_req[64]; static uint64_t g_f0 = ~0ull; static int g_nreq, g_next;
static char g_file[1024]; static long g_state_every; static uint64_t g_last_poll, g_last_state;
static int rd_file_id(void) {
    FILE *f = fopen(g_file, "r"); if (!f) return 0;
    int id = 0; if (fscanf(f, "%d", &id) != 1) id = 0; fclose(f); unlink(g_file); return id;
}
static void on_step(ucontext_t *uc) {
    uint64_t fr = dm_guest_frame(); uint8_t *gm = *(uint8_t **)(g_base + GAMEMAN_PTR);
    if (!gm) return;
    if (g_f0 == ~0ull) { g_f0 = fr; for (int i = 0; i < g_nreq; i++) if (g_req[i].rel) g_req[i].frame += fr; LOG("[dsr] in game at frame %llu", (unsigned long long)fr); }
    if (g_state_every > 0 && fr - g_last_state >= (uint64_t)g_state_every) { g_last_state = fr;
        uint32_t map = *(uint32_t *)(gm + GM_MAP);
        LOG("[dsr] state frame %llu last bonfire %d map m%02u_%02u warp request %u", (unsigned long long)fr, *(int32_t *)(gm + GM_BONFIRE), map >> 24, (map >> 16) & 0xff, gm[GM_REQ]); }
    int id = 0;
    if (g_next < g_nreq && fr >= g_req[g_next].frame) id = g_req[g_next++].id;
    else if (g_file[0] && fr - g_last_poll >= 30) { g_last_poll = fr; id = rd_file_id(); }
    if (!id) return;
    uint8_t *obj = *(uint8_t **)(g_base + OBJ_PTR);
    if (gm[GM_REQ] || !obj || !*(uint64_t *)(obj + 8)) { LOG("[RED] [dsr] warp %d at frame %llu deferred (warp in progress or no event object)", id, (unsigned long long)fr);
        if (g_next > 0 && g_req[g_next - 1].id == id) g_req[--g_next].frame = fr + 60; return; }
    *(int32_t *)(gm + GM_BONFIRE) = id;
    LOG("[dsr] warp %d at frame %llu (map m%02d_%02d)", id, (unsigned long long)fr, (id / 100000) % 100, (id / 10000) % 10);
    // The probe would resume at its slot (original instruction + jump back), which it left in pc; the stub calls warp(obj, 1) first, then goes there
    __typeof__(uc->uc_mcontext->__ss) *s = &uc->uc_mcontext->__ss;
    // probe.c has already pointed pc at the slot
    s->__x[9] = (uint64_t)(g_base + WARP_FN); s->__x[10] = (uint64_t)obj; s->__x[11] = 1; s->__x[12] = s->__pc; s->__pc = (uint64_t)g_stub;
}
void dsr_warp_install(void) {
    const char *w = getenv("DM_WARP"), *fe = getenv("DM_WARP_FILE"), *st = getenv("DM_DSR_STATE");
    if (!w && !fe && !st) return;
    if (strncmp(dm_main_build(), "df3766a2bb651a3e", 16)) { LOG("[RED] [dsr] DM_WARP: main is not Dark Souls Remastered 1.0.3 (df3766a2bb651a3e), ignored"); return; }
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) g_base = g_mod[i].base;
    if (!g_base) return;
    uint64_t at = 0;
    if (w) { char spec[1024]; strlcpy(spec, w, sizeof spec);
        for (char *t = strtok(spec, ","); t && g_nreq < 64; t = strtok(NULL, ",")) {
            char *a = strchr(t, '@'); g_req[g_nreq].id = atoi(t); g_req[g_nreq].rel = !a; at = a ? strtoull(a + 1, 0, 10) : (g_nreq ? at + GAP : FIRST);
            g_req[g_nreq].frame = at; if (g_req[g_nreq].id > 0) g_nreq++; } }
    if (fe) strlcpy(g_file, fe, sizeof g_file);
    else snprintf(g_file, sizeof g_file, "%s/run/warp.txt", getenv("DM_ROOT") ?: ".");
    g_state_every = st ? atol(st) : 0;
    g_stub = mmap(NULL, 0x4000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    static const uint32_t code[] = {
        0xd10083ff,   // sub sp,sp,#0x20
        0xa9007be0,   // stp x0,x30,[sp]
        0xf9000bec,   // str x12,[sp,#0x10]
        0xaa0a03e0,   // mov x0,x10
        0xaa0b03e1,   // mov x1,x11
        0xd63f0120,   // blr x9
        0xf9400bf0,   // ldr x16,[sp,#0x10]
        0xa9407be0,   // ldp x0,x30,[sp]
        0x910083ff,   // add sp,sp,#0x20
        0xd61f0200 }; // br x16
    memcpy(g_stub, code, sizeof code); mprotect(g_stub, 0x4000, PROT_READ | PROT_EXEC); sys_icache_invalidate(g_stub, sizeof code);
    probe_add_fn("main", HOOK, on_step);
    LOG("[dsr] warp hook main+%#x: %d scheduled, file %s, state every %ld frames", HOOK, g_nreq, g_file, g_state_every);
}
#else
void dsr_warp_install(void) {}
#endif
