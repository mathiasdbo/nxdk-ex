#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "alc_context.h"
#include <AL/al.h>
#include <AL/alext.h>
#include <string.h>
#include <stdlib.h>
#include "apu_hardware.h"
#include "apu_ep.h"
#include "apu_mem.h"
#include "apu_voice.h"
#include "apu_voice_mgr.h"
#include "apu_vp.h"
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
 * Backend guard.
 *
 * The register model in apu_hardware.h is not xemu-conformant (see
 * lib/openal/docs/XEMU_VERIFICATION.md): bring-up writes to the real BAR0 can disturb
 * or abort the emulator. Driving real MMIO is therefore opt-in
 * (-DOPENAL_APU_REAL_MMIO). By default, when the effective base is the
 * hardware default NV_PAPU_BASE, the device runs on a zeroed BAR0-sized RAM
 * region (the "null" backend). A base installed with alc_set_apu_base()
 * (host test mock) is used as given.
 */
#define ALC_NULL_BAR0_SIZE 0x80000u

static void *s_null_apu_ram = NULL;       /* null-backend BAR0 stand-in */
static uintptr_t s_session_apu_base = 0;  /* base of the open hardware session */
static bool s_hw_dolby_active = false;    /* cached: first device chose 5.1/DSE */

/* Route A on the real APU: the Voice Processor runs on its own (apu_vp.c) and the
 * legacy EP/GP setup, which targets registers that do not exist and aborts xemu
 * (XEMU_VERIFICATION.md 2.3), is skipped */
static bool s_vp_route_a = false;

static void alc_null_ram_release(void) {
    free(s_null_apu_ram);
    s_null_apu_ram = NULL;
}

/* MMIO means the open device's base is the real BAR0; no device open reports NULL */
static ALint alc_current_backend(void) {
    if (s_active_device_count > 0 && s_session_apu_base == NV_PAPU_BASE) {
        return AL_XBOX_BACKEND_MMIO;
    }
    return AL_XBOX_BACKEND_NULL;
}

/*
 * Portable function-pointer -> void * conversion (a direct cast is rejected
 * by -std=c99 -pedantic). All supported targets have equal-sized pointers.
 */
typedef void (*ALC_PROC)(void);
typedef char alc_proc_size_check[(sizeof(void *) == sizeof(ALC_PROC)) ? 1 : -1];

static void *alc_proc_to_ptr(ALC_PROC fn) {
    void *p = NULL;
    memcpy(&p, &fn, sizeof(p));
    return p;
}

static const struct {
    const char *name;
    ALC_PROC fn;
} s_proc_table[] = {
    { "alXboxGetHardwareStatus", (ALC_PROC)alXboxGetHardwareStatus },
    { "alXboxUpdateVoices",      (ALC_PROC)alXboxUpdateVoices }
};

static void *alc_lookup_proc(const char *name) {
    size_t i;
    if (name == NULL) {
        return NULL;
    }
    for (i = 0; i < sizeof(s_proc_table) / sizeof(s_proc_table[0]); i++) {
        if (strcmp(name, s_proc_table[i].name) == 0) {
            return alc_proc_to_ptr(s_proc_table[i].fn);
        }
    }
    return NULL;
}

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
    /* While a device is open, report the base actually in use (null-backend RAM or mock) */
    return (s_active_device_count > 0) ? s_session_apu_base : s_alc_apu_base;
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

    /* Allocate device handle and detect audio topology */
    ALCdevice *device = (ALCdevice *)calloc(1, sizeof(ALCdevice));
    if (!device) {
        alc_set_error(NULL, ALC_OUT_OF_MEMORY);
        return NULL;
    }

    device->topology = apu_detect_audio_topology();

    /* Additional devices share the hardware session of the first one */
    uintptr_t apu_base = s_session_apu_base;

    /* Initialize hardware resources on first open device */
    if (s_active_device_count == 0) {
        apu_base = s_alc_apu_base;
        if (apu_base == 0) {
            apu_base = NV_PAPU_BASE;
        }

#ifndef OPENAL_APU_REAL_MMIO
        /* Backend guard: never touch the real BAR0 unless explicitly enabled */
        if (apu_base == NV_PAPU_BASE) {
            s_null_apu_ram = calloc(1, ALC_NULL_BAR0_SIZE);
            if (!s_null_apu_ram) {
                free(device);
                alc_set_error(NULL, ALC_OUT_OF_MEMORY);
                return NULL;
            }
            apu_base = (uintptr_t)s_null_apu_ram;
        }
#endif

        s_vp_route_a = false;
#ifdef OPENAL_APU_REAL_MMIO
        s_vp_route_a = (apu_base == NV_PAPU_BASE);
#endif

        /* 1. Contiguous physical memory pool */
        if (apu_mem_init(0) != 0) {
            alc_null_ram_release();
            free(device);
            alc_set_error(NULL, ALC_OUT_OF_MEMORY);
            return NULL;
        }

        /* 2. 8KB 4KB-aligned Voice Context Array */
        s_hw_voice_table_virt = apu_mem_alloc_phys(
            APU_VOICE_MAX_SLOTS * NV_PAPU_VOICE_CONTEXT_SIZE,   /* 64 on the model, up to 128 on a console */
            NV_PAPU_VOICE_ARRAY_ALIGN,
            &s_hw_voice_table_phys
        );
        if (!s_hw_voice_table_virt) {
            apu_mem_shutdown();
            alc_null_ram_release();
            free(device);
            alc_set_error(NULL, ALC_OUT_OF_MEMORY);
            return NULL;
        }
        memset(s_hw_voice_table_virt, 0, APU_VOICE_MAX_SLOTS * NV_PAPU_VOICE_CONTEXT_SIZE);

        /* 2b. Hardware ITD Circular Buffer Pool (16KB) */
        if (apu_itd_subsystem_init() != 0) {
            apu_mem_free_phys(s_hw_voice_table_virt);
            s_hw_voice_table_virt = NULL;
            s_hw_voice_table_phys = 0;
            apu_mem_shutdown();
            alc_null_ram_release();
            free(device);
            alc_set_error(NULL, ALC_OUT_OF_MEMORY);
            return NULL;
        }

        /* 3. Output Processor (EP) subsystem initialization */
        int ep_res = s_vp_route_a ? 0 : apu_ep_subsystem_init(apu_base, device->topology);
        if (ep_res != 0) {
            apu_itd_subsystem_deinit();
            apu_mem_free_phys(s_hw_voice_table_virt);
            s_hw_voice_table_virt = NULL;
            s_hw_voice_table_phys = 0;
            apu_mem_shutdown();
            alc_null_ram_release();
            free(device);
            alc_set_error(NULL, ALC_INVALID_VALUE);
            return NULL;
        }

        /* 4. Global Processor (GP) DSP microcode & EP FIFO configuration */
        int ucode_res = s_vp_route_a ? 0 : apu_gp_load_topology_microcode(apu_base, device->topology);
        if (ucode_res != 0) {
            apu_ep_subsystem_deinit(apu_base);
            apu_itd_subsystem_deinit();
            apu_mem_free_phys(s_hw_voice_table_virt);
            s_hw_voice_table_virt = NULL;
            s_hw_voice_table_phys = 0;
            apu_mem_shutdown();
            alc_null_ram_release();
            free(device);
            alc_set_error(NULL, ALC_INVALID_VALUE);
            return NULL;
        }
        if (!s_vp_route_a) {
            apu_gp_start(apu_base);
        }

        /* 5. Voice Processor (VP) subsystem */
        int vp_res = apu_voice_subsystem_init(apu_base, s_hw_voice_table_virt, s_hw_voice_table_phys);
        if (vp_res != 0) {
            if (!s_vp_route_a) {
                apu_gp_stop(apu_base);
                apu_ep_subsystem_deinit(apu_base);
            }
            apu_itd_subsystem_deinit();
            apu_mem_free_phys(s_hw_voice_table_virt);
            s_hw_voice_table_virt = NULL;
            s_hw_voice_table_phys = 0;
            apu_mem_shutdown();
            alc_null_ram_release();
            free(device);
            alc_set_error(NULL, ALC_INVALID_VALUE);
            return NULL;
        }
        apu_voice_mgr_init();

        /* 6. OpenAL object subsystems */
        al_buffer_init_subsystem();
        al_source_init_subsystem();
        al_listener_init();
        al_source_set_apu_base(apu_base);

        s_session_apu_base = apu_base;
        s_hw_dolby_active = (device->topology == APU_TOPOLOGY_SURROUND_51);
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
            apu_voice_mgr_deinit();
            apu_voice_subsystem_deinit(device->apu_base);
            if (!s_vp_route_a) {
                apu_gp_stop(device->apu_base);
                apu_ep_subsystem_deinit(device->apu_base);
            }
            s_vp_route_a = false;

            if (s_hw_voice_table_virt != NULL) {
                apu_mem_free_phys(s_hw_voice_table_virt);
                s_hw_voice_table_virt = NULL;
                s_hw_voice_table_phys = 0;
            }

            apu_mem_shutdown();

            if (s_null_apu_ram != NULL) {
                /* Do not leave the source subsystem pointing at freed RAM */
                al_source_set_apu_base(0);
                alc_null_ram_release();
            }
            s_session_apu_base = 0;
            s_hw_dolby_active = false;
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
    ctx->mono_sources = (ALCint)apu_voice_slot_count();
    ctx->stereo_sources = (ALCint)(apu_voice_slot_count() / 2u);

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

    /* Context processing doubles as the public voice frame tick */
    alXboxUpdateVoices();
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
            values[7] = (ALCint)apu_voice_slot_count();
            values[8] = ALC_STEREO_SOURCES;
            values[9] = (ALCint)(apu_voice_slot_count() / 2u);
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
            *values = (ALCint)apu_voice_slot_count();
            break;

        case ALC_STEREO_SOURCES:
            *values = (ALCint)(apu_voice_slot_count() / 2u);
            break;

        case ALC_CAPTURE_SAMPLES:
            *values = 0;
            break;

        case ALC_XBOX_TOPOLOGY:
            if (device != NULL) {
                *values = (ALCint)device->topology;
            } else {
                *values = (ALCint)apu_detect_audio_topology();
            }
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
    return alc_lookup_proc(funcname);
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

/*
 * ============================================================================
 * OpenAL 1.1 Extension Query APIs
 * ============================================================================
 */

AL_API const ALchar * AL_APIENTRY alGetString(ALenum param) {
    switch (param) {
        case AL_VENDOR:
            return "NVIDIA / nxdk-ex";
        case AL_VERSION:
            return "1.1";
        case AL_RENDERER:
            return (alc_current_backend() == AL_XBOX_BACKEND_MMIO)
                       ? "MCPX APU (MMIO backend)"
                       : "MCPX APU (null backend)";
        case AL_EXTENSIONS:
            return "AL_EXT_OFFSET AL_SOFT_loop_points AL_XBOX_hardware_status AL_XBOX_source_control AL_XBOX_update";
        default:
            alSetError(AL_INVALID_ENUM);
            return NULL;
    }
}

AL_API ALboolean AL_APIENTRY alIsExtensionPresent(const ALchar *extname) {
    if (extname == NULL) {
        return AL_FALSE;
    }
    /* Keep in sync with the alGetString(AL_EXTENSIONS) list */
    if (strcmp(extname, "AL_EXT_OFFSET") == 0 ||
        strcmp(extname, "AL_SOFT_loop_points") == 0 ||
        strcmp(extname, "AL_XBOX_hardware_status") == 0 ||
        strcmp(extname, "AL_XBOX_source_control") == 0 ||
        strcmp(extname, "AL_XBOX_update") == 0) {
        return AL_TRUE;
    }
    return AL_FALSE;
}

AL_API void * AL_APIENTRY alGetProcAddress(const ALchar *fname) {
    return alc_lookup_proc(fname);
}

AL_API ALenum AL_APIENTRY alGetEnumValue(const ALchar *ename) {
    (void)ename;
    return 0;
}

/*
 * ============================================================================
 * Xbox Hardware Status Extension API (AL_XBOX_hardware_status)
 * ============================================================================
 */

AL_API void AL_APIENTRY alXboxGetHardwareStatus(ALenum param, ALint *value) {
    if (!value) {
        alSetError(AL_INVALID_VALUE);
        return;
    }

    switch (param) {
        case AL_XBOX_HW_VOICE_COUNT:
            *value = (ALint)apu_voice_slot_count();
            break;

        case AL_XBOX_AV_PACK_TYPE: {
            uint32_t pack = apu_query_smbus_av_pack();
            ALint pack_token = AL_XBOX_AV_PACK_NONE;
            switch (pack) {
                case APU_AV_PACK_SCART:
                    pack_token = AL_XBOX_AV_PACK_SCART;
                    break;
                case APU_AV_PACK_HDTV:
                    pack_token = AL_XBOX_AV_PACK_HDTV;
                    break;
                case APU_AV_PACK_VGA:
                    pack_token = AL_XBOX_AV_PACK_VGA;
                    break;
                case APU_AV_PACK_RFU:
                    pack_token = AL_XBOX_AV_PACK_RFU;
                    break;
                case APU_AV_PACK_SVIDEO:
                    pack_token = AL_XBOX_AV_PACK_SVIDEO;
                    break;
                case APU_AV_PACK_STANDARD:
                    pack_token = AL_XBOX_AV_PACK_COMPOSITE;
                    break;
                case APU_AV_PACK_NONE:
                default:
                    pack_token = AL_XBOX_AV_PACK_NONE;
                    break;
            }
            *value = pack_token;
            break;
        }

        /* Cached software state; no MMIO access (0 when no device is open) */
        case AL_XBOX_DOLBY_DIGITAL_ACTIVE:
            *value = (s_active_device_count > 0 && s_hw_dolby_active) ? 1 : 0;
            break;

        case AL_XBOX_VP_BASE_PHYS:
            *value = (s_active_device_count > 0) ? (ALint)s_hw_voice_table_phys : 0;
            break;

        case AL_XBOX_BACKEND:
            *value = alc_current_backend();
            break;

        case AL_XBOX_FREE_VOICES:
            *value = (s_active_device_count > 0) ? (ALint)(apu_voice_slot_count() - apu_voice_mgr_get_active_hw_count()) : 0;
            break;

        /* The APU's sample space: buffers are mapped into it when a voice starts
         * (hardware backend only; 0 on the software model) */
        case AL_XBOX_SAMPLE_PAGES_USED:
            *value = (s_active_device_count > 0 && apu_voice_hw_backend())
                         ? (ALint)apu_vp_sample_pages_used(al_source_get_apu_base())
                         : 0;
            break;

        case AL_XBOX_SAMPLE_PAGES_TOTAL:
            *value = (ALint)APU_VP_SGE_ENTRIES;
            break;

        default:
            alSetError(AL_INVALID_ENUM);
            break;
    }
}

/*
 * ============================================================================
 * Xbox Voice Frame Tick (AL_XBOX_update)
 * ============================================================================
 */

AL_API void AL_APIENTRY alXboxUpdateVoices(void) {
    /*
     * The tick reads APU state. With no device open the source subsystem
     * base may be unconfigured, the real BAR0 default, or stale (freed
     * null-backend RAM / a mock the caller has released): do not touch it.
     */
    if (s_active_device_count == 0) {
        return;
    }
    al_source_update_frame();
}
