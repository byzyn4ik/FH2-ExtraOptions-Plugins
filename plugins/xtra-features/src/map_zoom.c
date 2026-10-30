// Xtra Features for Forza Horizon 2 (Extra Options plugin): pause map zoom.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ported from the Extra Options fork (features/map_zoom.c). Inspected game:
// - The pause map is a MapViewControllerInteractive (vtable 0x2d9cd48, ctor
//   0xb7bc90, factory 0xb778e0). A_Map.xaml / A_IslandMap.xaml set its XAML
//   properties through one-line setters (0xb8a3a0..0xb8a4d0):
//     MinMapDistance +0x370 (A_Map 900, island 600)
//     MaxMapDistance +0x374 (A_Map 2500, island 1100)
//     MinCursorSpeed +0x37c, MaxCursorSpeed +0x380 (pan speed)
//     MinCursorZoom +0x384 (0), MaxCursorZoom +0x388 (1)
//     ZoomSpeed +0x38c (3.0)
//   The initial zoom +0x390 is not a property: the ctor sets it to 1.0.
// - Zoom is a 0..1 value t at +0x2e0. The camera distance (height above the
//   map; 90 degree FOV, top-down) is MinMapDistance + (MaxMapDistance -
//   MinMapDistance) * t (0xb8db80 at 0xb8dc22). The pan speed is the same
//   lerp of Min/MaxCursorSpeed. Input (0xb8d790, fed by the pause map page
//   input 0xd867b0) moves t by ZoomSpeed * zoom axis * dt and clamps it to
//   [MinCursorZoom, MaxCursorZoom].
// - The pause map page setup 0xd7fd80 calls 0xb803f0 (ResetView: centre on
//   the given position, t = [+0x390], reset the springs, update) at
//   0xd80442; it is its only caller. With t = 1.0 the map therefore always
//   opens at MaxMapDistance, which is also the farthest zoom-out: 2500 m on
//   the mainland, 1100 m on Storm Island.
// - Race/story scripts can narrow the t range through the ConstrainMapZoom
//   Min/Max component properties (0xd91630 writes +0x384/+0x388).
// - The camera's far plane is +0xc4 (10000, base ctor 0x192fd00), so the
//   distance must stay well below it. The map's low tile covers the whole
//   world and is always loaded; the high tiles only draw below 2000 m
//   (MapProfileTest.xml VisibleRange), so a larger distance shows the
//   low-resolution tile, as the game's own max zoom-out already does.
//
// The ResetView call at 0xd80442 is chained (patch.c chain_call: its
// displacement is not 4-byte aligned; the fork used the int3 padding after
// ResetView instead). Before the previous target runs, the hook raises
// MaxMapDistance (and MaxCursorSpeed with it so the pan speed at a given
// distance is unchanged), scales ZoomSpeed so zooming moves at the game's
// metres per second, and sets the initial t for the requested opening
// distance. The zoom range is still t 0..1, so the zoom controls cover the
// full new range. The Storm Island map (MaxMapDistance 1100) has its own
// percentages: its map texture ends close to the island.
#include <stdio.h>

#include "xtra.h"

#define RESET_VIEW 0xb803f0u
#define RESET_VIEW_CALL 0xd80442u
#define CONTROLLER_VTABLE 0x2d9cd48u

#define FAR_PLANE 0xc4
#define MIN_DISTANCE 0x370
#define MAX_DISTANCE 0x374
#define MIN_PAN_SPEED 0x37c
#define MAX_PAN_SPEED 0x380
#define MIN_ZOOM 0x384
#define MAX_ZOOM 0x388
#define ZOOM_SPEED 0x38c
#define INITIAL_ZOOM 0x390

// Above the island map's 1100 m, below the mainland's 2500 m.
#define ISLAND_MAX_DISTANCE 1500.0f

typedef void (*ResetView)(void *controller, const void *centre);

static const Code expected[] = {
    // ResetView: [+0x2c0] = centre, [+0x2c4] = 0, [+0x2e0] = [+0x390],
    // [+0x2d0] = [+0x2c0], call 0xb80680, jmp 0xb8db80 (dt 0, snap)
    CODE(RESET_VIEW, "ResetView",
         0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0xc5, 0xf8, 0x28, 0x02, 0x8b, 0x81, 0x90, 0x03, 0x00, 0x00, 0x48, 0x8b,
         0xd9, 0xc5, 0xf8, 0x29, 0x81, 0xc0, 0x02, 0x00, 0x00, 0xc7, 0x81, 0xc4, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00,
         0x00, 0x89, 0x81, 0xe0, 0x02, 0x00, 0x00, 0xc5, 0xf8, 0x28, 0x89, 0xc0, 0x02, 0x00, 0x00, 0xc5, 0xf8, 0x29,
         0x89, 0xd0, 0x02, 0x00, 0x00, 0xe8, 0x50, 0x02, 0x00, 0x00, 0x41, 0xb0, 0x01, 0x48, 0x8b, 0xcb, 0xc5, 0xf0,
         0x57, 0xc9, 0x48, 0x83, 0xc4, 0x20, 0x5b, 0xe9, 0x3c, 0xd7, 0x00, 0x00),
    // page setup: lea rdx,[rbp-0x30]; mov rcx,[r14+0x410]; call (its target is checked apart)
    CODE(RESET_VIEW_CALL - 11, "the pause map page setup's ResetView call",
         0x48, 0x8d, 0x55, 0xd0, 0x49, 0x8b, 0x8e, 0x10, 0x04, 0x00, 0x00, 0xe8),
    // camera distance = [+0x370] + ([+0x374] - [+0x370]) * [+0x2e0]
    CODE(0xb8dc22, "the map camera distance",
         0xc5, 0xfa, 0x10, 0x99, 0x70, 0x03, 0x00, 0x00, 0xc5, 0xfa, 0x10, 0x81, 0x74, 0x03, 0x00, 0x00, 0xc5, 0xfa,
         0x5c, 0xcb, 0xc5, 0xf2, 0x59, 0x91, 0xe0, 0x02, 0x00, 0x00, 0xc5, 0xea, 0x58, 0xdb, 0xc5, 0xe2, 0x59, 0x81),
    // input: t -= [+0x38c] * axis * dt, clamped to [+0x384], [+0x388]
    CODE(0xb8d937, "the map zoom input",
         0xc5, 0xfa, 0x10, 0x8b, 0x8c, 0x03, 0x00, 0x00, 0xc5, 0xf2, 0x59, 0x93, 0x74, 0x02, 0x00, 0x00, 0xc5, 0xfa,
         0x10, 0x8b, 0x88, 0x03, 0x00, 0x00, 0xc5, 0xf8, 0x29, 0x83, 0xc0, 0x02, 0x00, 0x00, 0xc5, 0xfa, 0x10, 0x83,
         0xe0, 0x02, 0x00, 0x00, 0xc4, 0xc1, 0x6a, 0x59, 0xda, 0xc5, 0xfa, 0x10, 0x93, 0x84, 0x03, 0x00, 0x00, 0xc5,
         0xfa, 0x5c, 0xe3, 0xc5, 0xe0, 0x57, 0xdb, 0xc5, 0xda, 0x5c, 0xc1, 0xc5, 0xfa, 0x11, 0xa3, 0xe0, 0x02, 0x00,
         0x00, 0xc5, 0xf8, 0x2f, 0xc3, 0x73, 0x04, 0xc5, 0xf8, 0x28, 0xcc, 0xc5, 0xf2, 0x5c, 0xc2, 0xc5, 0xf8, 0x2f,
         0xc3, 0x72, 0x0a, 0xc5, 0xfa, 0x11, 0x8b, 0xe0, 0x02, 0x00, 0x00, 0xeb, 0x08, 0xc5, 0xfa, 0x11, 0x93, 0xe0,
         0x02, 0x00, 0x00),
    // XAML setters: mov eax,[r8+0x28]; mov [rdx+field],eax; ret
    CODE(0xb8a480, "the MinMapDistance setter", 0x41, 0x8b, 0x40, 0x28, 0x89, 0x82, 0x70, 0x03, 0x00, 0x00, 0xc3),
    CODE(0xb8a450, "the MaxMapDistance setter", 0x41, 0x8b, 0x40, 0x28, 0x89, 0x82, 0x74, 0x03, 0x00, 0x00, 0xc3),
    CODE(0xb8a460, "the MinCursorSpeed setter", 0x41, 0x8b, 0x40, 0x28, 0x89, 0x82, 0x7c, 0x03, 0x00, 0x00, 0xc3),
    CODE(0xb8a420, "the MaxCursorSpeed setter", 0x41, 0x8b, 0x40, 0x28, 0x89, 0x82, 0x80, 0x03, 0x00, 0x00, 0xc3),
    CODE(0xb8a470, "the MinCursorZoom setter", 0x41, 0x8b, 0x40, 0x28, 0x89, 0x82, 0x84, 0x03, 0x00, 0x00, 0xc3),
    CODE(0xb8a430, "the MaxCursorZoom setter", 0x41, 0x8b, 0x40, 0x28, 0x89, 0x82, 0x88, 0x03, 0x00, 0x00, 0xc3),
    CODE(0xb8a4d0, "the ZoomSpeed setter", 0x41, 0x8b, 0x40, 0x28, 0x89, 0x82, 0x8c, 0x03, 0x00, 0x00, 0xc3),
    // ctor: initial zoom [+0x390] = 1.0
    CODE(0xb7be44, "the map view's initial zoom", 0xc7, 0x83, 0x90, 0x03, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3f),
    // far plane read by the camera projection (cam +0x44 = controller +0xc4)
    CODE(0x19440c1, "the map camera's far plane read", 0xc5, 0xfa, 0x10, 0x63, 0x44),
};

static unsigned char *image;
static ResetView previous_reset_view;
static int start_percent, max_percent, island_start_percent, island_max_percent;

// Controllers already adjusted, with the distance written, so a second
// ResetView on the same controller does not scale it again.
typedef struct {
    unsigned char *controller;
    float max_distance;
    float initial_zoom;
} Adjusted;
static Adjusted adjusted[8];
static unsigned adjusted_next;
// Original max distances already logged (mainland, island).
static float logged[4];
static unsigned logged_count;
static volatile LONG warned_constrained, warned_values;

static float *field(unsigned char *controller, unsigned offset) { return (float *)(controller + offset); }

static int valid_number(float value) {
    uint32_t bits;
    fh2_copy_bytes(&bits, &value, sizeof(bits));
    return (bits & 0x7f800000u) != 0x7f800000u;
}

static Adjusted *find_adjusted(unsigned char *controller) {
    for (unsigned i = 0; i < ARRAY_COUNT(adjusted); ++i)
        if (adjusted[i].controller == controller) return &adjusted[i];
    return NULL;
}

static void log_once(float original_max, float minimum, float open_distance, float start, float new_max) {
    for (unsigned i = 0; i < logged_count; ++i)
        if (logged[i] == original_max) return;
    if (logged_count < ARRAY_COUNT(logged)) logged[logged_count++] = original_max;
    say("MapZoom: pause map opened; opening distance %d m (game %d m), farthest zoom-out %d m (game %d m), "
        "closest zoom-in %d m (unchanged).", (int)start, (int)open_distance, (int)new_max, (int)original_max,
        (int)minimum);
}

static void adjust(unsigned char *controller) {
    Adjusted *previous = find_adjusted(controller);
    if (previous && *field(controller, MAX_DISTANCE) == previous->max_distance) {
        *field(controller, INITIAL_ZOOM) = previous->initial_zoom;
        return;
    }
    float minimum = *field(controller, MIN_DISTANCE);
    float maximum = *field(controller, MAX_DISTANCE);
    float min_speed = *field(controller, MIN_PAN_SPEED);
    float max_speed = *field(controller, MAX_PAN_SPEED);
    float min_zoom = *field(controller, MIN_ZOOM);
    float max_zoom = *field(controller, MAX_ZOOM);
    float zoom_speed = *field(controller, ZOOM_SPEED);
    float initial = *field(controller, INITIAL_ZOOM);
    float far_plane = *field(controller, FAR_PLANE);
    if (!valid_number(minimum) || !valid_number(maximum) || !valid_number(min_speed) || !valid_number(max_speed) ||
        !valid_number(min_zoom) || !valid_number(max_zoom) || !valid_number(zoom_speed) || !valid_number(initial) ||
        !valid_number(far_plane) || minimum <= 0.0f || maximum <= minimum || far_plane <= maximum ||
        zoom_speed <= 0.0f || initial < 0.0f || initial > 1.0f) {
        if (!InterlockedExchange(&warned_values, 1))
            say("MapZoom: the map view's values are out of the expected range; the game's zoom is kept.");
        return;
    }
    if (min_zoom != 0.0f || max_zoom != 1.0f) {
        // A script narrowed the zoom range in t; leave the distances it meant.
        if (!InterlockedExchange(&warned_constrained, 1))
            say("MapZoom: the zoom range is constrained by the game; the game's zoom is kept.");
        return;
    }
    int island = maximum < ISLAND_MAX_DISTANCE;
    int start_wanted = island ? island_start_percent : start_percent;
    int max_wanted = island ? island_max_percent : max_percent;
    if (start_wanted == 100 && max_wanted == 100) return;
    float open_distance = minimum + (maximum - minimum) * initial;
    float start = open_distance * (float)start_wanted / 100.0f;
    float new_max = maximum * (float)max_wanted / 100.0f;
    if (new_max < start) new_max = start;
    // Keep the map plane well inside the camera's far plane.
    float limit = far_plane * 0.9f;
    if (new_max > limit) new_max = limit;
    if (new_max < maximum) new_max = maximum;
    if (start > new_max) start = new_max;
    if (start < minimum) start = minimum;
    float ratio = (new_max - minimum) / (maximum - minimum);
    float start_zoom = (start - minimum) / (new_max - minimum);
    *field(controller, MAX_DISTANCE) = new_max;
    *field(controller, MAX_PAN_SPEED) = min_speed + (max_speed - min_speed) * ratio;
    *field(controller, ZOOM_SPEED) = zoom_speed / ratio;
    *field(controller, INITIAL_ZOOM) = start_zoom;
    Adjusted *entry = previous ? previous : &adjusted[adjusted_next++ % ARRAY_COUNT(adjusted)];
    entry->controller = controller;
    entry->max_distance = new_max;
    entry->initial_zoom = start_zoom;
    log_once(maximum, minimum, open_distance, start, new_max);
}

static void reset_view_hook(void *controller, const void *centre) {
    if (controller && read_pointer(controller) == image + CONTROLLER_VTABLE) adjust((unsigned char *)controller);
    previous_reset_view(controller, centre);
}

const char *map_zoom_check(const unsigned char *game, char *why, size_t size) {
    const char *problem = check_code(game, expected, ARRAY_COUNT(expected), why, size);
    if (problem) return problem;
    // The setup's call: to ResetView, or to another module's hook (chained).
    const unsigned char *target = fh2_call_target(game + RESET_VIEW_CALL);
    if (target != game + RESET_VIEW && (!target || in_game(game, target) || !fh2_is_code(target))) {
        snprintf(why, size, "the ResetView call at 0x%x goes neither to ResetView 0x%x nor to another module's code",
                 RESET_VIEW_CALL, RESET_VIEW);
        return why;
    }
    return NULL;
}

int map_zoom_install(unsigned char *game, const XtraSettings *settings) {
    image = game;
    start_percent = settings->start;
    max_percent = settings->max;
    island_start_percent = settings->island_start;
    island_max_percent = settings->island_max;
    if (start_percent == 100 && max_percent == 100 && island_start_percent == 100 && island_max_percent == 100) {
        say("MapZoom: every [MapZoom] percentage is 100; the game's map zoom is kept and nothing is installed.");
        return 0;
    }
    void *previous = NULL;
    const unsigned char *before = fh2_call_target(game + RESET_VIEW_CALL);
    const char *problem = chain_call(game + RESET_VIEW_CALL, game + RESET_VIEW, game,
                                     (void *)(uintptr_t)reset_view_hook, &previous);
    if (problem) {
        say("MapZoom: the ResetView call at 0x%x could not be chained (%s); the pause map is left as it is.",
            RESET_VIEW_CALL, problem);
        return 0;
    }
    previous_reset_view = (ResetView)previous;
    char where[160];
    if (before == game + RESET_VIEW)
        snprintf(where, sizeof(where), "the game's ResetView 0x%x", RESET_VIEW);
    else
        describe(before, where, sizeof(where));
    say("MapZoom: ResetView call 0x%x chained onto %s; the pause map opens at %d%% and zooms out to %d%% of the "
        "game's distance (Storm Island %d%% and %d%%).", RESET_VIEW_CALL, where, start_percent, max_percent,
        island_start_percent, island_max_percent);
    return 1;
}

#ifdef XTRA_TEST
// The adjustment alone, on a stand-in controller (percentages as given).
__declspec(dllexport) void XtraTestMapZoomAdjust(unsigned char *controller, int start, int max, int island_start,
                                                 int island_max) {
    start_percent = start;
    max_percent = max;
    island_start_percent = island_start;
    island_max_percent = island_max;
    adjust(controller);
}

__declspec(dllexport) void *XtraTestMapZoomHook(void) { return (void *)(uintptr_t)reset_view_hook; }
#endif
