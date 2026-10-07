#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "alc_context.h"
#include <AL/al.h>
#include <string.h>
#include <stdlib.h>
#include "apu_hardware.h"
#include "apu_mem.h"
#include "apu_voice.h"
#include "apu_gp_ucode.h"
#include "al_buffer.h"
#include "al_source.h"
#include "al_listener.h"
#include "apu_spatial.h"

/*
 * ============================================================================
 * Internal State & Global Hardware Tracking
 * ============================================================================
 */
static const ALCchar s_default_device_name[] = "MCPX APU 5.1 Surround";

static uintptr_t s_alc_apu_base = NV_PAPU_BASE;
static ALCcontext *s_current_context = NULL;
static ALCenum s_alc_global_error = ALC_NO_ERROR;

static uint32_t s_active_device_count = 0;
static void *s_hw_voice_table_virt = NULL;
static uint32_t s_hw_voice_table_phys = 0;

/*
 * ============================================================================
 * Error Reporting Helper
 * ============================================================================
 */
static void alc_set_error(ALCdevice *device, ALCenum error) {
    if (device != NULL) {
        if (device->last_error == ALC_NO_ERROR && error != ALC_NO_ERROR) {
            device->last_error = error;
        }
    } else {
        if (s_alc_global_error == ALC_NO_ERROR && error != ALC_NO_ERROR) {
            s_alc_global_error = error;
        }
    }
}

/*
 * ============================================================================
 * Testing & Hardware Mock Configuration API
 * ============================================================================
 */
void alc_set_apu_base(uintptr_t base) {
    s_alc_apu_base = (base != 0) ? base : NV_PAPU_BASE;
}

uintptr_t alc_get_apu_base(void) {
    return s_alc_apu_base;
}

/*
 * ============================================================================
 * OpenAL 1.1 Device Management APIs
 * ============================================================================
 */

ALC_API ALCdevice * ALC_APIENTRY alcOpenDevice(const ALCchar *devicename) {
    /* Validate device specifier if provided */
    if (devicename != NULL) {
        if (strcmp(devicename, s_default_device_name) != 0 &&
            strcmp(devicename, "MCPX APU") != 0 &&
            strcmp(devicename, "DirectSound3D") != 0 &&
            strcmp(devicename, "DirectSound") != 0) {
            alc_set_error(NULL, ALC_INVALID_VALUE);
            return NULL;
        }
    }

    uintptr_t apu_base = s_alc_apu_base;
    if (apu_base == 0) {
        apu_base = NV_PAPU_BASE;
    }

    /* Initialize hardware resources on first open device */
    if (s_active_device_count == 0) {
        /* 1. Contiguous physical memory pool */
        if (apu_mem_init(0) != 0) {
            alc_set_error(NULL, ALC_OUT_OF_MEMORY);
            return NULL;
        }

        /* 2. 8KB 4KB-aligned Voice Context Array */
        s_hw_voice_table_virt = apu_mem_alloc_phys(
            NV_PAPU_VOICE_ARRAY_SIZE_3D,
            NV_PAPU_VOICE_ARRAY_ALIGN,
            &s_hw_voice_table_phys
        );
        if (!s_hw_voice_table_virt) {
            apu_mem_shutdown();
            alc_set_error(NULL, ALC_OUT_OF_MEMORY);
            return NULL;
        }
        memset(s_hw_voice_table_virt, 0, NV_PAPU_VOICE_ARRAY_SIZE_3D);

        /* 2b. Hardware ITD Circular Buffer Pool (16KB) */
        if (apu_itd_subsystem_init() != 0) {
            apu_mem_free_phys(s_hw_voice_table_virt);
            s_hw_voice_table_virt = NULL;
            s_hw_voice_table_phys = 0;
            apu_mem_shutdown();
            alc_set_error(NULL, ALC_OUT_OF_MEMORY);
            return NULL;
        }

        /* 3. Output Processor (EP) stereo FIFO routing */
        apu_write32(apu_base, NV_PAPU_EP_FIFO_CONFIG, NV_PAPU_EP_FIFO_CONFIG_STEREO);
        apu_write32(apu_base, NV_PAPU_EP_FIFO_ROUTE, NV_PAPU_EP_ROUTE_DEFAULT);
        apu_write32(apu_base, NV_PAPU_EP_CONTROL, NV_PAPU_EP_CONTROL_ENABLE);

        /* 4. Global Processor (GP) DSP 5.1 passthrough microcode */
        int ucode_res = apu_gp_load_microcode(apu_base, gp_passthrough_51_bin, GP_PASSTHROUGH_51_SIZE);
        if (ucode_res != 0) {
            apu_itd_subsystem_deinit();
            apu_mem_free_phys(s_hw_voice_table_virt);
            s_hw_voice_table_virt = NULL;
            s_hw_voice_table_phys = 0;
            apu_mem_shutdown();
            alc_set_error(NULL, ALC_INVALID_VALUE);
            return NULL;
        }
        apu_gp_start(apu_base);

        /* 5. Voice Processor (VP) subsystem */
        int vp_res = apu_voice_subsystem_init(apu_base, s_hw_voice_table_virt, s_hw_voice_table_phys);
        if (vp_res != 0) {
            apu_gp_stop(apu_base);
            apu_itd_subsystem_deinit();
            apu_mem_free_phys(s_hw_voice_table_virt);
            s_hw_voice_table_virt = NULL;
            s_hw_voice_table_phys = 0;
            apu_mem_shutdown();
            alc_set_error(NULL, ALC_INVALID_VALUE);
            return NULL;
        }

        /* 6. OpenAL object subsystems */
        al_buffer_init_subsystem();
        al_source_init_subsystem();
        al_listener_init();
        al_source_set_apu_base(apu_base);
    }

    ALCdevice *device = (ALCdevice *)calloc(1, sizeof(ALCdevice));
    if (!device) {
        if (s_active_device_count == 0) {
            apu_voice_subsystem_deinit(apu_base);
            apu_gp_stop(apu_base);
            apu_write32(apu_base, NV_PAPU_EP_CONTROL, 0u);
            apu_itd_subsystem_deinit();
            apu_mem_free_phys(s_hw_voice_table_virt);
            s_hw_voice_table_virt = NULL;
            s_hw_voice_table_phys = 0;
            apu_mem_shutdown();
        }
        alc_set_error(NULL, ALC_OUT_OF_MEMORY);
        return NULL;
    }

    device->is_open = true;
    device->last_error = ALC_NO_ERROR;
    {
        size_t name_len = strlen(s_default_device_name);
        if (name_len >= sizeof(device->name)) {
            name_len = sizeof(device->name) - 1;
        }
        memcpy(device->name, s_default_device_name, name_len);
        device->name[name_len] = '\0';
    }
    device->voice_table_virt = s_hw_voice_table_virt;
    device->voice_table_phys = s_hw_voice_table_phys;
    device->context_count = 0;
    device->active_context = NULL;
    device->apu_base = apu_base;

    s_active_device_count++;
    return device;
}

ALC_API ALCboolean ALC_APIENTRY alcCloseDevice(ALCdevice *device) {
    if (!device || !device->is_open) {
        alc_set_error(device, ALC_INVALID_DEVICE);
        return ALC_FALSE;
    }

    if (device->context_count > 0) {
        /* OpenAL spec: Cannot close a device that still owns active contexts */
        alc_set_error(device, ALC_INVALID_VALUE);
        return ALC_FALSE;
    }

    if (s_active_device_count > 0) {
        s_active_device_count--;
        if (s_active_device_count == 0) {
            /* Deinitialize OpenAL subsystems */
            al_source_cleanup_subsystem();
            al_buffer_cleanup_subsystem();
            al_listener_reset();

            /* Deinitialize ITD circular buffer subsystem */
            apu_itd_subsystem_deinit();

            /* Deinitialize hardware subsystems */
            apu_voice_subsystem_deinit(device->apu_base);
            apu_gp_stop(device->apu_base);
            apu_write32(device->apu_base, NV_PAPU_EP_CONTROL, 0u);

            if (s_hw_voice_table_virt != NULL) {
                apu_mem_free_phys(s_hw_voice_table_virt);
                s_hw_voice_table_virt = NULL;
                s_hw_voice_table_phys = 0;
            }

            apu_mem_shutdown();
        }
    }

    device->is_open = false;
    free(device);
    return ALC_TRUE;
}

/*
 * ============================================================================
 * OpenAL 1.1 Context Management APIs
 * ============================================================================
 */

ALC_API ALCcontext * ALC_APIENTRY alcCreateContext(ALCdevice *device, const ALCint *attrlist) {
    if (!device || !device->is_open) {
        alc_set_error(device, ALC_INVALID_DEVICE);
        return NULL;
    }

    ALCcontext *ctx = (ALCcontext *)calloc(1, sizeof(ALCcontext));
    if (!ctx) {
        alc_set_error(device, ALC_OUT_OF_MEMORY);
        return NULL;
    }

    ctx->is_valid = true;
    ctx->device = device;
    ctx->frequency = 48000;
    ctx->refresh = 60;
    ctx->sync = ALC_FALSE;
    ctx->mono_sources = 64;
    ctx->stereo_sources = 32;

    if (attrlist != NULL) {
        for (const ALCint *attr = attrlist; *attr != 0; attr += 2) {
            ALCenum param = attr[0];
            ALCint val = attr[1];
            switch (param) {
                case ALC_FREQUENCY:
                    ctx->frequency = val;
                    break;
                case ALC_REFRESH:
                    ctx->refresh = val;
                    break;
                case ALC_SYNC:
                    ctx->sync = val;
                    break;
                case ALC_MONO_SOURCES:
                    ctx->mono_sources = val;
                    break;
                case ALC_STEREO_SOURCES:
                    ctx->stereo_sources = val;
                    break;
                default:
                    break;
            }
        }
    }

    device->context_count++;
    return ctx;
}

ALC_API ALCboolean ALC_APIENTRY alcMakeContextCurrent(ALCcontext *context) {
    if (context == NULL) {
        if (s_current_context != NULL) {
            if (s_current_context->device != NULL) {
                s_current_context->device->active_context = NULL;
            }
            s_current_context = NULL;
        }
        return ALC_TRUE;
    }

    if (!context->is_valid || !context->device || !context->device->is_open) {
        alc_set_error(context ? context->device : NULL, ALC_INVALID_CONTEXT);
        return ALC_FALSE;
    }

    if (s_current_context != NULL && s_current_context->device != NULL) {
        s_current_context->device->active_context = NULL;
    }

    s_current_context = context;
    context->device->active_context = context;
    return ALC_TRUE;
}

ALC_API void ALC_APIENTRY alcProcessContext(ALCcontext *context) {
    if (!context || !context->is_valid) {
        alc_set_error(NULL, ALC_INVALID_CONTEXT);
        return;
    }
}

ALC_API void ALC_APIENTRY alcSuspendContext(ALCcontext *context) {
    if (!context || !context->is_valid) {
        alc_set_error(NULL, ALC_INVALID_CONTEXT);
        return;
    }
}

ALC_API void ALC_APIENTRY alcDestroyContext(ALCcontext *context) {
    if (!context || !context->is_valid) {
        alc_set_error(NULL, ALC_INVALID_CONTEXT);
        return;
    }

    if (context == s_current_context) {
        /* OpenAL spec: Cannot destroy the current context */
        alc_set_error(context->device, ALC_INVALID_CONTEXT);
        return;
    }

    if (context->device != NULL) {
        if (context->device->context_count > 0) {
            context->device->context_count--;
        }
        if (context->device->active_context == context) {
            context->device->active_context = NULL;
        }
    }

    context->is_valid = false;
    free(context);
}

ALC_API ALCcontext * ALC_APIENTRY alcGetCurrentContext(void) {
    return s_current_context;
}

ALC_API ALCdevice * ALC_APIENTRY alcGetContextsDevice(ALCcontext *context) {
    if (!context || !context->is_valid) {
        alc_set_error(NULL, ALC_INVALID_CONTEXT);
        return NULL;
    }
    return context->device;
}

/*
 * ============================================================================
 * OpenAL 1.1 Error Support API
 * ============================================================================
 */

ALC_API ALCenum ALC_APIENTRY alcGetError(ALCdevice *device) {
    if (device == NULL) {
        ALCenum err = s_alc_global_error;
        s_alc_global_error = ALC_NO_ERROR;
        return err;
    }
    ALCenum err = device->last_error;
    device->last_error = ALC_NO_ERROR;
    return err;
}

/*
 * ============================================================================
 * OpenAL 1.1 Query & Information APIs
 * ============================================================================
 */

ALC_API const ALCchar * ALC_APIENTRY alcGetString(ALCdevice *device, ALCenum param) {
    switch (param) {
        case ALC_DEFAULT_DEVICE_SPECIFIER:
        case ALC_DEFAULT_ALL_DEVICES_SPECIFIER:
            return s_default_device_name;

        case ALC_DEVICE_SPECIFIER:
            if (device != NULL && device->is_open) {
                return device->name;
            }
            return s_default_device_name;

        case ALC_ALL_DEVICES_SPECIFIER:
            /* Double-null terminated list of specifiers */
            return "MCPX APU 5.1 Surround\0";

        case ALC_EXTENSIONS:
            return "";

        case ALC_CAPTURE_DEVICE_SPECIFIER:
        case ALC_CAPTURE_DEFAULT_DEVICE_SPECIFIER:
            return NULL;

        default:
            alc_set_error(device, ALC_INVALID_ENUM);
            return NULL;
    }
}

ALC_API void ALC_APIENTRY alcGetIntegerv(ALCdevice *device, ALCenum param, ALCsizei size, ALCint *values) {
    if (values == NULL || size <= 0) {
        alc_set_error(device, ALC_INVALID_VALUE);
        return;
    }

    switch (param) {
        case ALC_MAJOR_VERSION:
            *values = 1;
            break;

        case ALC_MINOR_VERSION:
            *values = 1;
            break;

        case ALC_ATTRIBUTES_SIZE:
            *values = 11;
            break;

        case ALC_ALL_ATTRIBUTES:
            if (size < 11) {
                alc_set_error(device, ALC_INVALID_VALUE);
                return;
            }
            values[0] = ALC_FREQUENCY;
            values[1] = 48000;
            values[2] = ALC_REFRESH;
            values[3] = 60;
            values[4] = ALC_SYNC;
            values[5] = ALC_FALSE;
            values[6] = ALC_MONO_SOURCES;
            values[7] = 64;
            values[8] = ALC_STEREO_SOURCES;
            values[9] = 32;
            values[10] = 0;
            break;

        case ALC_FREQUENCY:
            *values = 48000;
            break;

        case ALC_REFRESH:
            *values = 60;
            break;

        case ALC_SYNC:
            *values = ALC_FALSE;
            break;

        case ALC_MONO_SOURCES:
            *values = 64;
            break;

        case ALC_STEREO_SOURCES:
            *values = 32;
            break;

        case ALC_CAPTURE_SAMPLES:
            *values = 0;
            break;

        default:
            alc_set_error(device, ALC_INVALID_ENUM);
            break;
    }
}

/*
 * ============================================================================
 * OpenAL 1.1 Extension Support APIs
 * ============================================================================
 */

ALC_API ALCboolean ALC_APIENTRY alcIsExtensionPresent(ALCdevice *device, const ALCchar *extname) {
    (void)device;
    (void)extname;
    return ALC_FALSE;
}

ALC_API void * ALC_APIENTRY alcGetProcAddress(ALCdevice *device, const ALCchar *funcname) {
    (void)device;
    (void)funcname;
    return NULL;
}

ALC_API ALCenum ALC_APIENTRY alcGetEnumValue(ALCdevice *device, const ALCchar *enumname) {
    (void)device;
    (void)enumname;
    return 0;
}

/*
 * ============================================================================
 * OpenAL 1.1 Capture Device APIs (Stubs)
 * ============================================================================
 */

ALC_API ALCdevice * ALC_APIENTRY alcCaptureOpenDevice(const ALCchar *devicename, ALCuint frequency, ALCenum format, ALCsizei buffersize) {
    (void)devicename;
    (void)frequency;
    (void)format;
    (void)buffersize;
    alc_set_error(NULL, ALC_INVALID_VALUE);
    return NULL;
}

ALC_API ALCboolean ALC_APIENTRY alcCaptureCloseDevice(ALCdevice *device) {
    (void)device;
    return ALC_FALSE;
}

ALC_API void ALC_APIENTRY alcCaptureStart(ALCdevice *device) {
    (void)device;
}

ALC_API void ALC_APIENTRY alcCaptureStop(ALCdevice *device) {
    (void)device;
}

ALC_API void ALC_APIENTRY alcCaptureSamples(ALCdevice *device, ALCvoid *buffer, ALCsizei samples) {
    (void)device;
    (void)buffer;
    (void)samples;
}
