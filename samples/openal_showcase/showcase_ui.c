#include "showcase_ui.h"
#include <stdio.h>
#include <string.h>

#if defined(NXDK) || defined(__NXDK__) || defined(_XBOX)
#  include <hal/video.h>
#  include <hal/debug.h>
#  include <pbkit/pbkit.h>
#  include <windows.h>
#  define HAS_PBKIT 1
#endif

static bool s_pbkit_active = false;
static uint32_t s_frame_count = 0;

int showcase_ui_init(void) {
    s_frame_count = 0;
    s_pbkit_active = false;

#if defined(HAS_PBKIT)
    XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);
    if (pb_init() == 0) {
        pb_show_front_screen();
        s_pbkit_active = true;
    } else {
        debugPrint("[WARN] pbkit initialization failed, falling back to debug output\n");
    }
#endif

    return 0;
}

void showcase_ui_begin_frame(void) {
    s_frame_count++;

#if defined(HAS_PBKIT)
    if (s_pbkit_active) {
        pb_wait_for_vbl();
        pb_target_back_buffer();
        pb_reset();
        pb_fill(0, 0, 640, 480, 0x000F1520); /* Dark slate background */
        pb_erase_text_screen();
    }
#endif
}

void showcase_ui_draw_header(const char *mode_title, int mode_idx, int total_modes,
                             uint32_t active_hw_voices, uint32_t standby_voices,
                             bool dse_active, int topology, bool controller_connected) {
    char header_buf[512];
    const char *topo_str = (topology == 1) ? "Surround 5.1" : "Stereo 2.0";
    const char *dse_str = dse_active ? "configured (no AC-3 encoder)" : "not configured";
    const char *ctrl_str = controller_connected ? "GAMEPAD CONNECTED" : "AUTO-TOUR MODE";

    snprintf(header_buf, sizeof(header_buf),
             "======================================================================\n"
             " nxdk-ex OpenAL Showcase (MCPX APU voice-layer model)\n"
             "======================================================================\n"
             " Mode [%d/%d]: %-30s | %s\n"
             " Topology: %s | Dolby Digital: %s\n"
             " Voice slots: %2u / 64 active | Standby Queue: %u virtual sources\n"
             "----------------------------------------------------------------------\n",
             mode_idx, total_modes, mode_title, ctrl_str,
             topo_str, dse_str,
             active_hw_voices, standby_voices);

#if defined(HAS_PBKIT)
    if (s_pbkit_active) {
        pb_print("%s", header_buf);
    }
#endif

    if ((s_frame_count % 30) == 0) {
#if defined(HAS_PBKIT)
        debugPrint("%s", header_buf);
#else
        printf("%s", header_buf);
#endif
    }
}

void showcase_ui_draw_telemetry(const char *telemetry_text) {
    if (!telemetry_text) {
        return;
    }

#if defined(HAS_PBKIT)
    if (s_pbkit_active) {
        pb_print("%s\n", telemetry_text);
    }
#endif

    if ((s_frame_count % 30) == 0) {
#if defined(HAS_PBKIT)
        debugPrint("%s\n", telemetry_text);
#else
        printf("%s\n", telemetry_text);
#endif
    }
}

void showcase_ui_draw_footer(const char *footer_help) {
    char footer_buf[512];
    snprintf(footer_buf, sizeof(footer_buf),
             "----------------------------------------------------------------------\n"
             " Controls: [White/Black or DPad L/R] Change Mode | [Start] Auto-Tour\n"
             " %s\n"
             "======================================================================\n",
             footer_help ? footer_help : "");

#if defined(HAS_PBKIT)
    if (s_pbkit_active) {
        pb_print("%s", footer_buf);
    }
#endif

    if ((s_frame_count % 30) == 0) {
#if defined(HAS_PBKIT)
        debugPrint("%s", footer_buf);
#else
        printf("%s", footer_buf);
#endif
    }
}

void showcase_ui_end_frame(void) {
#if defined(HAS_PBKIT)
    if (s_pbkit_active) {
        pb_draw_text_screen();
        while (pb_busy());
        while (pb_finished());
    }
#endif
}

void showcase_ui_shutdown(void) {
#if defined(HAS_PBKIT)
    if (s_pbkit_active) {
        pb_kill();
        s_pbkit_active = false;
    }
#endif
}
