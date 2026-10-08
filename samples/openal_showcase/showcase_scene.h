#ifndef SHOWCASE_SCENE_H
#define SHOWCASE_SCENE_H

#include <stdint.h>
#include <stdbool.h>
#include "showcase_modes.h"
#include "showcase_input.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * NV2A 3D view of the showcase: a floor grid around the listener, the
 * listener's head and ears, every active OpenAL source of the current mode,
 * plus a HUD (radar, spatial-model readouts, controller legend).
 *
 * The frame is built on the CPU into one vertex buffer (screen-space
 * positions, ARGB colours) and drawn back to front, so no depth buffer or
 * culling state is needed. Building is plain C and also runs on the host
 * (tests); only showcase_scene_submit() talks to the GPU, through pbkit.
 */

#define SHOWCASE_SCENE_WIDTH  640
#define SHOWCASE_SCENE_HEIGHT 480
/* Screen x of the 3D view's center (the HUD panels sit left and right of it) */
#define SHOWCASE_SCENE_VIEW_CX 288.0f

/* Vertex layout shared with the GPU: position (NDC) + D3D colour (0xAARRGGBB) */
typedef struct {
    float x;
    float y;
    float z;
    uint32_t color;
} showcase_scene_vertex_t;

typedef struct {
    const showcase_scene_vertex_t *vertices;
    uint32_t vertex_count;
    uint32_t vertex_capacity;
    uint32_t batch_count;
    bool overflow;      /* geometry was dropped because the buffer was full */
} showcase_scene_stats_t;

/**
 * Allocate the vertex buffer and, on the Xbox, load the shaders.
 * Call after showcase_ui_init() (pbkit must be up).
 * @return 0 on success.
 */
int showcase_scene_init(void);

/**
 * Build the frame for the current application state.
 * @param app   Application state (after showcase_app_update()).
 * @param input Input state of this frame (controller status for the HUD).
 */
void showcase_scene_build(const showcase_app_t *app, const showcase_input_t *input);

/**
 * Draw the frame built by showcase_scene_build(). Call between
 * showcase_ui_begin_frame() and showcase_ui_end_frame(). No-op on the host.
 */
void showcase_scene_submit(void);

/* Frame timing and audio health shown on the HUD */
typedef struct {
    float fps;            /* main-loop frames per second */
    float scene_ms;       /* CPU time to build and submit the 3D frame */
    float audio_ms;       /* CPU time mixing audio in the frame */
    uint32_t voices;      /* sources in the last mixed chunk */
    uint32_t underruns;   /* AC97 chunks played as silence because the mix fell behind */
} showcase_perf_t;

/**
 * Update the performance readout (shown on the status panel).
 */
void showcase_scene_set_perf(const showcase_perf_t *perf);

/**
 * Tell the HUD whether the software audio mixer is playing the sources
 * (otherwise it notes that the default backend is silent).
 */
void showcase_scene_set_audio_output(bool playing);

/**
 * Release the vertex buffer.
 */
void showcase_scene_shutdown(void);

/**
 * Statistics of the last built frame (host tests).
 */
void showcase_scene_get_stats(showcase_scene_stats_t *out);

/**
 * Project a world-space point with the camera of the last built frame.
 * @param world  Point in OpenAL world coordinates.
 * @param out_px Screen x in pixels (0 = left edge).
 * @param out_py Screen y in pixels (0 = top edge).
 * @return false if the point is behind the camera's near plane.
 */
bool showcase_scene_project(const float world[3], float *out_px, float *out_py);

#ifdef __cplusplus
}
#endif

#endif /* SHOWCASE_SCENE_H */
