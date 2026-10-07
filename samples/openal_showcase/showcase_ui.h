#ifndef SHOWCASE_UI_H
#define SHOWCASE_UI_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize video mode and text rendering framebuffer.
 * @return 0 on success.
 */
int showcase_ui_init(void);

/**
 * Prepare next frame for rendering (VBL sync and screen clearing).
 */
void showcase_ui_begin_frame(void);

/**
 * Draw title banner, APU hardware status, active topology, and voice counts.
 */
void showcase_ui_draw_header(const char *mode_title, int mode_idx, int total_modes,
                             uint32_t active_hw_voices, uint32_t standby_voices,
                             bool dse_active, int topology, bool controller_connected);

/**
 * Draw mode-specific real-time telemetry box.
 */
void showcase_ui_draw_telemetry(const char *telemetry_text);

/**
 * Draw bottom navigation bar and controller button guide.
 */
void showcase_ui_draw_footer(const char *footer_help);

/**
 * Present frame buffer to the display.
 */
void showcase_ui_end_frame(void);

/**
 * Clean up display resources.
 */
void showcase_ui_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* SHOWCASE_UI_H */
