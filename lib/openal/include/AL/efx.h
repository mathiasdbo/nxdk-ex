#ifndef AL_EFX_H
#define AL_EFX_H

/*
 * ALC_EXT_EFX 1.0: the OpenAL Effects Extension (tokens, limits, entry points).
 * Written for nxdk-ex from the extension's public token list; the values are
 * the interface every EFX application already uses.
 *
 * On the MCPX APU (lib/openal/docs/EFX_PLAN.md):
 *   effect types      AL_EFFECT_NULL, AL_EFFECT_REVERB and AL_EFFECT_EAXREVERB are
 *                     accepted; the other types give AL_INVALID_VALUE
 *   filter types      AL_FILTER_NULL only for now (the VP filter is being measured)
 *   effect slots      4; one of them can hold an effect at a time (the GP runs one)
 *   sends per source  ALC_MAX_AUXILIARY_SENDS, at most 2
 *
 * The entry points are fetched with alGetProcAddress("alGenEffects") and so on.
 * Define AL_ALEXT_PROTOTYPES before including this header to get the
 * prototypes too (the library is linked statically, so they resolve).
 */

#include <AL/al.h>
#include <AL/alc.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ALC_EXT_EFX_NAME                         "ALC_EXT_EFX"

/* ALC */
#define ALC_EFX_MAJOR_VERSION                    0x20001
#define ALC_EFX_MINOR_VERSION                    0x20002
#define ALC_MAX_AUXILIARY_SENDS                  0x20003

/* Listener */
#define AL_METERS_PER_UNIT                       0x20004

/* Source */
#define AL_DIRECT_FILTER                         0x20005
#define AL_AUXILIARY_SEND_FILTER                 0x20006
#define AL_AIR_ABSORPTION_FACTOR                 0x20007
#define AL_ROOM_ROLLOFF_FACTOR                   0x20008
#define AL_CONE_OUTER_GAINHF                     0x20009
#define AL_DIRECT_FILTER_GAINHF_AUTO             0x2000A
#define AL_AUXILIARY_SEND_FILTER_GAIN_AUTO       0x2000B
#define AL_AUXILIARY_SEND_FILTER_GAINHF_AUTO     0x2000C

/* Reverb */
#define AL_REVERB_DENSITY                        0x0001
#define AL_REVERB_DIFFUSION                      0x0002
#define AL_REVERB_GAIN                           0x0003
#define AL_REVERB_GAINHF                         0x0004
#define AL_REVERB_DECAY_TIME                     0x0005
#define AL_REVERB_DECAY_HFRATIO                  0x0006
#define AL_REVERB_REFLECTIONS_GAIN               0x0007
#define AL_REVERB_REFLECTIONS_DELAY              0x0008
#define AL_REVERB_LATE_REVERB_GAIN               0x0009
#define AL_REVERB_LATE_REVERB_DELAY              0x000A
#define AL_REVERB_AIR_ABSORPTION_GAINHF          0x000B
#define AL_REVERB_ROOM_ROLLOFF_FACTOR            0x000C
#define AL_REVERB_DECAY_HFLIMIT                  0x000D

/* EAX reverb */
#define AL_EAXREVERB_DENSITY                     0x0001
#define AL_EAXREVERB_DIFFUSION                   0x0002
#define AL_EAXREVERB_GAIN                        0x0003
#define AL_EAXREVERB_GAINHF                      0x0004
#define AL_EAXREVERB_GAINLF                      0x0005
#define AL_EAXREVERB_DECAY_TIME                  0x0006
#define AL_EAXREVERB_DECAY_HFRATIO               0x0007
#define AL_EAXREVERB_DECAY_LFRATIO               0x0008
#define AL_EAXREVERB_REFLECTIONS_GAIN            0x0009
#define AL_EAXREVERB_REFLECTIONS_DELAY           0x000A
#define AL_EAXREVERB_REFLECTIONS_PAN             0x000B
#define AL_EAXREVERB_LATE_REVERB_GAIN            0x000C
#define AL_EAXREVERB_LATE_REVERB_DELAY           0x000D
#define AL_EAXREVERB_LATE_REVERB_PAN             0x000E
#define AL_EAXREVERB_ECHO_TIME                   0x000F
#define AL_EAXREVERB_ECHO_DEPTH                  0x0010
#define AL_EAXREVERB_MODULATION_TIME             0x0011
#define AL_EAXREVERB_MODULATION_DEPTH            0x0012
#define AL_EAXREVERB_AIR_ABSORPTION_GAINHF       0x0013
#define AL_EAXREVERB_HFREFERENCE                 0x0014
#define AL_EAXREVERB_LFREFERENCE                 0x0015
#define AL_EAXREVERB_ROOM_ROLLOFF_FACTOR         0x0016
#define AL_EAXREVERB_DECAY_HFLIMIT               0x0017

/* Chorus */
#define AL_CHORUS_WAVEFORM                       0x0001
#define AL_CHORUS_PHASE                          0x0002
#define AL_CHORUS_RATE                           0x0003
#define AL_CHORUS_DEPTH                          0x0004
#define AL_CHORUS_FEEDBACK                       0x0005
#define AL_CHORUS_DELAY                          0x0006

/* Distortion */
#define AL_DISTORTION_EDGE                       0x0001
#define AL_DISTORTION_GAIN                       0x0002
#define AL_DISTORTION_LOWPASS_CUTOFF             0x0003
#define AL_DISTORTION_EQCENTER                   0x0004
#define AL_DISTORTION_EQBANDWIDTH                0x0005

/* Echo */
#define AL_ECHO_DELAY                            0x0001
#define AL_ECHO_LRDELAY                          0x0002
#define AL_ECHO_DAMPING                          0x0003
#define AL_ECHO_FEEDBACK                         0x0004
#define AL_ECHO_SPREAD                           0x0005

#define AL_ECHO_MIN_DELAY                        (0.0f)
#define AL_ECHO_MAX_DELAY                        (0.207f)
#define AL_ECHO_DEFAULT_DELAY                    (0.1f)
#define AL_ECHO_MIN_LRDELAY                      (0.0f)
#define AL_ECHO_MAX_LRDELAY                      (0.404f)
#define AL_ECHO_DEFAULT_LRDELAY                  (0.1f)
#define AL_ECHO_MIN_DAMPING                      (0.0f)
#define AL_ECHO_MAX_DAMPING                      (0.99f)
#define AL_ECHO_DEFAULT_DAMPING                  (0.5f)
#define AL_ECHO_MIN_FEEDBACK                     (0.0f)
#define AL_ECHO_MAX_FEEDBACK                     (1.0f)
#define AL_ECHO_DEFAULT_FEEDBACK                 (0.5f)
#define AL_ECHO_MIN_SPREAD                       (-1.0f)
#define AL_ECHO_MAX_SPREAD                       (1.0f)
#define AL_ECHO_DEFAULT_SPREAD                   (-1.0f)

/* Flanger */
#define AL_FLANGER_WAVEFORM                      0x0001
#define AL_FLANGER_PHASE                         0x0002
#define AL_FLANGER_RATE                          0x0003
#define AL_FLANGER_DEPTH                         0x0004
#define AL_FLANGER_FEEDBACK                      0x0005
#define AL_FLANGER_DELAY                         0x0006

/* Frequency shifter */
#define AL_FREQUENCY_SHIFTER_FREQUENCY           0x0001
#define AL_FREQUENCY_SHIFTER_LEFT_DIRECTION      0x0002
#define AL_FREQUENCY_SHIFTER_RIGHT_DIRECTION     0x0003

/* Vocal morpher */
#define AL_VOCAL_MORPHER_PHONEMEA                0x0001
#define AL_VOCAL_MORPHER_PHONEMEA_COARSE_TUNING  0x0002
#define AL_VOCAL_MORPHER_PHONEMEB                0x0003
#define AL_VOCAL_MORPHER_PHONEMEB_COARSE_TUNING  0x0004
#define AL_VOCAL_MORPHER_WAVEFORM                0x0005
#define AL_VOCAL_MORPHER_RATE                    0x0006

/* Pitch shifter */
#define AL_PITCH_SHIFTER_COARSE_TUNE             0x0001
#define AL_PITCH_SHIFTER_FINE_TUNE               0x0002

/* Ring modulator */
#define AL_RING_MODULATOR_FREQUENCY              0x0001
#define AL_RING_MODULATOR_HIGHPASS_CUTOFF        0x0002
#define AL_RING_MODULATOR_WAVEFORM               0x0003

/* Autowah */
#define AL_AUTOWAH_ATTACK_TIME                   0x0001
#define AL_AUTOWAH_RELEASE_TIME                  0x0002
#define AL_AUTOWAH_RESONANCE                     0x0003
#define AL_AUTOWAH_PEAK_GAIN                     0x0004

/* Compressor */
#define AL_COMPRESSOR_ONOFF                      0x0001

/* Equalizer */
#define AL_EQUALIZER_LOW_GAIN                    0x0001
#define AL_EQUALIZER_LOW_CUTOFF                  0x0002
#define AL_EQUALIZER_MID1_GAIN                   0x0003
#define AL_EQUALIZER_MID1_CENTER                 0x0004
#define AL_EQUALIZER_MID1_WIDTH                  0x0005
#define AL_EQUALIZER_MID2_GAIN                   0x0006
#define AL_EQUALIZER_MID2_CENTER                 0x0007
#define AL_EQUALIZER_MID2_WIDTH                  0x0008
#define AL_EQUALIZER_HIGH_GAIN                   0x0009
#define AL_EQUALIZER_HIGH_CUTOFF                 0x000A

/* Effect type */
#define AL_EFFECT_FIRST_PARAMETER                0x0000
#define AL_EFFECT_LAST_PARAMETER                 0x8000
#define AL_EFFECT_TYPE                           0x8001

#define AL_EFFECT_NULL                           0x0000
#define AL_EFFECT_REVERB                         0x0001
#define AL_EFFECT_CHORUS                         0x0002
#define AL_EFFECT_DISTORTION                     0x0003
#define AL_EFFECT_ECHO                           0x0004
#define AL_EFFECT_FLANGER                        0x0005
#define AL_EFFECT_FREQUENCY_SHIFTER              0x0006
#define AL_EFFECT_VOCAL_MORPHER                  0x0007
#define AL_EFFECT_PITCH_SHIFTER                  0x0008
#define AL_EFFECT_RING_MODULATOR                 0x0009
#define AL_EFFECT_AUTOWAH                        0x000A
#define AL_EFFECT_COMPRESSOR                     0x000B
#define AL_EFFECT_EQUALIZER                      0x000C
#define AL_EFFECT_EAXREVERB                      0x8000

/* Auxiliary effect slot */
#define AL_EFFECTSLOT_EFFECT                     0x0001
#define AL_EFFECTSLOT_GAIN                       0x0002
#define AL_EFFECTSLOT_AUXILIARY_SEND_AUTO        0x0003
#define AL_EFFECTSLOT_NULL                       0x0000

/* Lowpass */
#define AL_LOWPASS_GAIN                          0x0001
#define AL_LOWPASS_GAINHF                        0x0002

/* Highpass */
#define AL_HIGHPASS_GAIN                         0x0001
#define AL_HIGHPASS_GAINLF                       0x0002

/* Bandpass */
#define AL_BANDPASS_GAIN                         0x0001
#define AL_BANDPASS_GAINLF                       0x0002
#define AL_BANDPASS_GAINHF                       0x0003

/* Filter type */
#define AL_FILTER_FIRST_PARAMETER                0x0000
#define AL_FILTER_LAST_PARAMETER                 0x8000
#define AL_FILTER_TYPE                           0x8001

#define AL_FILTER_NULL                           0x0000
#define AL_FILTER_LOWPASS                        0x0001
#define AL_FILTER_HIGHPASS                       0x0002
#define AL_FILTER_BANDPASS                       0x0003

/* Reverb limits and defaults */
#define AL_REVERB_MIN_DENSITY                    (0.0f)
#define AL_REVERB_MAX_DENSITY                    (1.0f)
#define AL_REVERB_DEFAULT_DENSITY                (1.0f)
#define AL_REVERB_MIN_DIFFUSION                  (0.0f)
#define AL_REVERB_MAX_DIFFUSION                  (1.0f)
#define AL_REVERB_DEFAULT_DIFFUSION              (1.0f)
#define AL_REVERB_MIN_GAIN                       (0.0f)
#define AL_REVERB_MAX_GAIN                       (1.0f)
#define AL_REVERB_DEFAULT_GAIN                   (0.32f)
#define AL_REVERB_MIN_GAINHF                     (0.0f)
#define AL_REVERB_MAX_GAINHF                     (1.0f)
#define AL_REVERB_DEFAULT_GAINHF                 (0.89f)
#define AL_REVERB_MIN_DECAY_TIME                 (0.1f)
#define AL_REVERB_MAX_DECAY_TIME                 (20.0f)
#define AL_REVERB_DEFAULT_DECAY_TIME             (1.49f)
#define AL_REVERB_MIN_DECAY_HFRATIO              (0.1f)
#define AL_REVERB_MAX_DECAY_HFRATIO              (2.0f)
#define AL_REVERB_DEFAULT_DECAY_HFRATIO          (0.83f)
#define AL_REVERB_MIN_REFLECTIONS_GAIN           (0.0f)
#define AL_REVERB_MAX_REFLECTIONS_GAIN           (3.16f)
#define AL_REVERB_DEFAULT_REFLECTIONS_GAIN       (0.05f)
#define AL_REVERB_MIN_REFLECTIONS_DELAY          (0.0f)
#define AL_REVERB_MAX_REFLECTIONS_DELAY          (0.3f)
#define AL_REVERB_DEFAULT_REFLECTIONS_DELAY      (0.007f)
#define AL_REVERB_MIN_LATE_REVERB_GAIN           (0.0f)
#define AL_REVERB_MAX_LATE_REVERB_GAIN           (10.0f)
#define AL_REVERB_DEFAULT_LATE_REVERB_GAIN       (1.26f)
#define AL_REVERB_MIN_LATE_REVERB_DELAY          (0.0f)
#define AL_REVERB_MAX_LATE_REVERB_DELAY          (0.1f)
#define AL_REVERB_DEFAULT_LATE_REVERB_DELAY      (0.011f)
#define AL_REVERB_MIN_AIR_ABSORPTION_GAINHF      (0.892f)
#define AL_REVERB_MAX_AIR_ABSORPTION_GAINHF      (1.0f)
#define AL_REVERB_DEFAULT_AIR_ABSORPTION_GAINHF  (0.994f)
#define AL_REVERB_MIN_ROOM_ROLLOFF_FACTOR        (0.0f)
#define AL_REVERB_MAX_ROOM_ROLLOFF_FACTOR        (10.0f)
#define AL_REVERB_DEFAULT_ROOM_ROLLOFF_FACTOR    (0.0f)
#define AL_REVERB_MIN_DECAY_HFLIMIT              AL_FALSE
#define AL_REVERB_MAX_DECAY_HFLIMIT              AL_TRUE
#define AL_REVERB_DEFAULT_DECAY_HFLIMIT          AL_TRUE

/* EAX reverb limits and defaults (the shared parameters match the reverb's) */
#define AL_EAXREVERB_MIN_DENSITY                 AL_REVERB_MIN_DENSITY
#define AL_EAXREVERB_MAX_DENSITY                 AL_REVERB_MAX_DENSITY
#define AL_EAXREVERB_DEFAULT_DENSITY             AL_REVERB_DEFAULT_DENSITY
#define AL_EAXREVERB_MIN_DIFFUSION               AL_REVERB_MIN_DIFFUSION
#define AL_EAXREVERB_MAX_DIFFUSION               AL_REVERB_MAX_DIFFUSION
#define AL_EAXREVERB_DEFAULT_DIFFUSION           AL_REVERB_DEFAULT_DIFFUSION
#define AL_EAXREVERB_MIN_GAIN                    AL_REVERB_MIN_GAIN
#define AL_EAXREVERB_MAX_GAIN                    AL_REVERB_MAX_GAIN
#define AL_EAXREVERB_DEFAULT_GAIN                AL_REVERB_DEFAULT_GAIN
#define AL_EAXREVERB_MIN_GAINHF                  AL_REVERB_MIN_GAINHF
#define AL_EAXREVERB_MAX_GAINHF                  AL_REVERB_MAX_GAINHF
#define AL_EAXREVERB_DEFAULT_GAINHF              AL_REVERB_DEFAULT_GAINHF
#define AL_EAXREVERB_MIN_GAINLF                  (0.0f)
#define AL_EAXREVERB_MAX_GAINLF                  (1.0f)
#define AL_EAXREVERB_DEFAULT_GAINLF              (1.0f)
#define AL_EAXREVERB_MIN_DECAY_TIME              AL_REVERB_MIN_DECAY_TIME
#define AL_EAXREVERB_MAX_DECAY_TIME              AL_REVERB_MAX_DECAY_TIME
#define AL_EAXREVERB_DEFAULT_DECAY_TIME          AL_REVERB_DEFAULT_DECAY_TIME
#define AL_EAXREVERB_MIN_DECAY_HFRATIO           AL_REVERB_MIN_DECAY_HFRATIO
#define AL_EAXREVERB_MAX_DECAY_HFRATIO           AL_REVERB_MAX_DECAY_HFRATIO
#define AL_EAXREVERB_DEFAULT_DECAY_HFRATIO       AL_REVERB_DEFAULT_DECAY_HFRATIO
#define AL_EAXREVERB_MIN_DECAY_LFRATIO           (0.1f)
#define AL_EAXREVERB_MAX_DECAY_LFRATIO           (2.0f)
#define AL_EAXREVERB_DEFAULT_DECAY_LFRATIO       (1.0f)
#define AL_EAXREVERB_MIN_REFLECTIONS_GAIN        AL_REVERB_MIN_REFLECTIONS_GAIN
#define AL_EAXREVERB_MAX_REFLECTIONS_GAIN        AL_REVERB_MAX_REFLECTIONS_GAIN
#define AL_EAXREVERB_DEFAULT_REFLECTIONS_GAIN    AL_REVERB_DEFAULT_REFLECTIONS_GAIN
#define AL_EAXREVERB_MIN_REFLECTIONS_DELAY       AL_REVERB_MIN_REFLECTIONS_DELAY
#define AL_EAXREVERB_MAX_REFLECTIONS_DELAY       AL_REVERB_MAX_REFLECTIONS_DELAY
#define AL_EAXREVERB_DEFAULT_REFLECTIONS_DELAY   AL_REVERB_DEFAULT_REFLECTIONS_DELAY
#define AL_EAXREVERB_DEFAULT_REFLECTIONS_PAN_XYZ (0.0f)
#define AL_EAXREVERB_MIN_LATE_REVERB_GAIN        AL_REVERB_MIN_LATE_REVERB_GAIN
#define AL_EAXREVERB_MAX_LATE_REVERB_GAIN        AL_REVERB_MAX_LATE_REVERB_GAIN
#define AL_EAXREVERB_DEFAULT_LATE_REVERB_GAIN    AL_REVERB_DEFAULT_LATE_REVERB_GAIN
#define AL_EAXREVERB_MIN_LATE_REVERB_DELAY       AL_REVERB_MIN_LATE_REVERB_DELAY
#define AL_EAXREVERB_MAX_LATE_REVERB_DELAY       AL_REVERB_MAX_LATE_REVERB_DELAY
#define AL_EAXREVERB_DEFAULT_LATE_REVERB_DELAY   AL_REVERB_DEFAULT_LATE_REVERB_DELAY
#define AL_EAXREVERB_DEFAULT_LATE_REVERB_PAN_XYZ (0.0f)
#define AL_EAXREVERB_MIN_ECHO_TIME               (0.075f)
#define AL_EAXREVERB_MAX_ECHO_TIME               (0.25f)
#define AL_EAXREVERB_DEFAULT_ECHO_TIME           (0.25f)
#define AL_EAXREVERB_MIN_ECHO_DEPTH              (0.0f)
#define AL_EAXREVERB_MAX_ECHO_DEPTH              (1.0f)
#define AL_EAXREVERB_DEFAULT_ECHO_DEPTH          (0.0f)
#define AL_EAXREVERB_MIN_MODULATION_TIME         (0.04f)
#define AL_EAXREVERB_MAX_MODULATION_TIME         (4.0f)
#define AL_EAXREVERB_DEFAULT_MODULATION_TIME     (0.25f)
#define AL_EAXREVERB_MIN_MODULATION_DEPTH        (0.0f)
#define AL_EAXREVERB_MAX_MODULATION_DEPTH        (1.0f)
#define AL_EAXREVERB_DEFAULT_MODULATION_DEPTH    (0.0f)
#define AL_EAXREVERB_MIN_AIR_ABSORPTION_GAINHF   AL_REVERB_MIN_AIR_ABSORPTION_GAINHF
#define AL_EAXREVERB_MAX_AIR_ABSORPTION_GAINHF   AL_REVERB_MAX_AIR_ABSORPTION_GAINHF
#define AL_EAXREVERB_DEFAULT_AIR_ABSORPTION_GAINHF AL_REVERB_DEFAULT_AIR_ABSORPTION_GAINHF
#define AL_EAXREVERB_MIN_HFREFERENCE             (1000.0f)
#define AL_EAXREVERB_MAX_HFREFERENCE             (20000.0f)
#define AL_EAXREVERB_DEFAULT_HFREFERENCE         (5000.0f)
#define AL_EAXREVERB_MIN_LFREFERENCE             (20.0f)
#define AL_EAXREVERB_MAX_LFREFERENCE             (1000.0f)
#define AL_EAXREVERB_DEFAULT_LFREFERENCE         (250.0f)
#define AL_EAXREVERB_MIN_ROOM_ROLLOFF_FACTOR     AL_REVERB_MIN_ROOM_ROLLOFF_FACTOR
#define AL_EAXREVERB_MAX_ROOM_ROLLOFF_FACTOR     AL_REVERB_MAX_ROOM_ROLLOFF_FACTOR
#define AL_EAXREVERB_DEFAULT_ROOM_ROLLOFF_FACTOR AL_REVERB_DEFAULT_ROOM_ROLLOFF_FACTOR
#define AL_EAXREVERB_MIN_DECAY_HFLIMIT           AL_FALSE
#define AL_EAXREVERB_MAX_DECAY_HFLIMIT           AL_TRUE
#define AL_EAXREVERB_DEFAULT_DECAY_HFLIMIT       AL_TRUE

/* Effect slot, source and listener limits and defaults */
#define AL_EFFECTSLOT_MIN_GAIN                   (0.0f)
#define AL_EFFECTSLOT_MAX_GAIN                   (1.0f)
#define AL_EFFECTSLOT_DEFAULT_GAIN               (1.0f)
#define AL_EFFECTSLOT_DEFAULT_AUXILIARY_SEND_AUTO AL_TRUE

#define AL_MIN_AIR_ABSORPTION_FACTOR             (0.0f)
#define AL_MAX_AIR_ABSORPTION_FACTOR             (10.0f)
#define AL_DEFAULT_AIR_ABSORPTION_FACTOR         (0.0f)
#define AL_MIN_ROOM_ROLLOFF_FACTOR               (0.0f)
#define AL_MAX_ROOM_ROLLOFF_FACTOR               (10.0f)
#define AL_DEFAULT_ROOM_ROLLOFF_FACTOR           (0.0f)
#define AL_MIN_CONE_OUTER_GAINHF                 (0.0f)
#define AL_MAX_CONE_OUTER_GAINHF                 (1.0f)
#define AL_DEFAULT_CONE_OUTER_GAINHF             (1.0f)
#define AL_DEFAULT_DIRECT_FILTER_GAINHF_AUTO     AL_TRUE
#define AL_DEFAULT_AUXILIARY_SEND_FILTER_GAIN_AUTO AL_TRUE
#define AL_DEFAULT_AUXILIARY_SEND_FILTER_GAINHF_AUTO AL_TRUE

#define AL_DEFAULT_METERS_PER_UNIT               (1.0f)

/* Entry points */
typedef void (AL_APIENTRY *LPALGENEFFECTS)(ALsizei, ALuint *);
typedef void (AL_APIENTRY *LPALDELETEEFFECTS)(ALsizei, const ALuint *);
typedef ALboolean (AL_APIENTRY *LPALISEFFECT)(ALuint);
typedef void (AL_APIENTRY *LPALEFFECTI)(ALuint, ALenum, ALint);
typedef void (AL_APIENTRY *LPALEFFECTIV)(ALuint, ALenum, const ALint *);
typedef void (AL_APIENTRY *LPALEFFECTF)(ALuint, ALenum, ALfloat);
typedef void (AL_APIENTRY *LPALEFFECTFV)(ALuint, ALenum, const ALfloat *);
typedef void (AL_APIENTRY *LPALGETEFFECTI)(ALuint, ALenum, ALint *);
typedef void (AL_APIENTRY *LPALGETEFFECTIV)(ALuint, ALenum, ALint *);
typedef void (AL_APIENTRY *LPALGETEFFECTF)(ALuint, ALenum, ALfloat *);
typedef void (AL_APIENTRY *LPALGETEFFECTFV)(ALuint, ALenum, ALfloat *);

typedef void (AL_APIENTRY *LPALGENFILTERS)(ALsizei, ALuint *);
typedef void (AL_APIENTRY *LPALDELETEFILTERS)(ALsizei, const ALuint *);
typedef ALboolean (AL_APIENTRY *LPALISFILTER)(ALuint);
typedef void (AL_APIENTRY *LPALFILTERI)(ALuint, ALenum, ALint);
typedef void (AL_APIENTRY *LPALFILTERIV)(ALuint, ALenum, const ALint *);
typedef void (AL_APIENTRY *LPALFILTERF)(ALuint, ALenum, ALfloat);
typedef void (AL_APIENTRY *LPALFILTERFV)(ALuint, ALenum, const ALfloat *);
typedef void (AL_APIENTRY *LPALGETFILTERI)(ALuint, ALenum, ALint *);
typedef void (AL_APIENTRY *LPALGETFILTERIV)(ALuint, ALenum, ALint *);
typedef void (AL_APIENTRY *LPALGETFILTERF)(ALuint, ALenum, ALfloat *);
typedef void (AL_APIENTRY *LPALGETFILTERFV)(ALuint, ALenum, ALfloat *);

typedef void (AL_APIENTRY *LPALGENAUXILIARYEFFECTSLOTS)(ALsizei, ALuint *);
typedef void (AL_APIENTRY *LPALDELETEAUXILIARYEFFECTSLOTS)(ALsizei, const ALuint *);
typedef ALboolean (AL_APIENTRY *LPALISAUXILIARYEFFECTSLOT)(ALuint);
typedef void (AL_APIENTRY *LPALAUXILIARYEFFECTSLOTI)(ALuint, ALenum, ALint);
typedef void (AL_APIENTRY *LPALAUXILIARYEFFECTSLOTIV)(ALuint, ALenum, const ALint *);
typedef void (AL_APIENTRY *LPALAUXILIARYEFFECTSLOTF)(ALuint, ALenum, ALfloat);
typedef void (AL_APIENTRY *LPALAUXILIARYEFFECTSLOTFV)(ALuint, ALenum, const ALfloat *);
typedef void (AL_APIENTRY *LPALGETAUXILIARYEFFECTSLOTI)(ALuint, ALenum, ALint *);
typedef void (AL_APIENTRY *LPALGETAUXILIARYEFFECTSLOTIV)(ALuint, ALenum, ALint *);
typedef void (AL_APIENTRY *LPALGETAUXILIARYEFFECTSLOTF)(ALuint, ALenum, ALfloat *);
typedef void (AL_APIENTRY *LPALGETAUXILIARYEFFECTSLOTFV)(ALuint, ALenum, ALfloat *);

#ifdef AL_ALEXT_PROTOTYPES
AL_API void AL_APIENTRY alGenEffects(ALsizei n, ALuint *effects);
AL_API void AL_APIENTRY alDeleteEffects(ALsizei n, const ALuint *effects);
AL_API ALboolean AL_APIENTRY alIsEffect(ALuint effect);
AL_API void AL_APIENTRY alEffecti(ALuint effect, ALenum param, ALint iValue);
AL_API void AL_APIENTRY alEffectiv(ALuint effect, ALenum param, const ALint *piValues);
AL_API void AL_APIENTRY alEffectf(ALuint effect, ALenum param, ALfloat flValue);
AL_API void AL_APIENTRY alEffectfv(ALuint effect, ALenum param, const ALfloat *pflValues);
AL_API void AL_APIENTRY alGetEffecti(ALuint effect, ALenum param, ALint *piValue);
AL_API void AL_APIENTRY alGetEffectiv(ALuint effect, ALenum param, ALint *piValues);
AL_API void AL_APIENTRY alGetEffectf(ALuint effect, ALenum param, ALfloat *pflValue);
AL_API void AL_APIENTRY alGetEffectfv(ALuint effect, ALenum param, ALfloat *pflValues);

AL_API void AL_APIENTRY alGenFilters(ALsizei n, ALuint *filters);
AL_API void AL_APIENTRY alDeleteFilters(ALsizei n, const ALuint *filters);
AL_API ALboolean AL_APIENTRY alIsFilter(ALuint filter);
AL_API void AL_APIENTRY alFilteri(ALuint filter, ALenum param, ALint iValue);
AL_API void AL_APIENTRY alFilteriv(ALuint filter, ALenum param, const ALint *piValues);
AL_API void AL_APIENTRY alFilterf(ALuint filter, ALenum param, ALfloat flValue);
AL_API void AL_APIENTRY alFilterfv(ALuint filter, ALenum param, const ALfloat *pflValues);
AL_API void AL_APIENTRY alGetFilteri(ALuint filter, ALenum param, ALint *piValue);
AL_API void AL_APIENTRY alGetFilteriv(ALuint filter, ALenum param, ALint *piValues);
AL_API void AL_APIENTRY alGetFilterf(ALuint filter, ALenum param, ALfloat *pflValue);
AL_API void AL_APIENTRY alGetFilterfv(ALuint filter, ALenum param, ALfloat *pflValues);

AL_API void AL_APIENTRY alGenAuxiliaryEffectSlots(ALsizei n, ALuint *effectslots);
AL_API void AL_APIENTRY alDeleteAuxiliaryEffectSlots(ALsizei n, const ALuint *effectslots);
AL_API ALboolean AL_APIENTRY alIsAuxiliaryEffectSlot(ALuint effectslot);
AL_API void AL_APIENTRY alAuxiliaryEffectSloti(ALuint effectslot, ALenum param, ALint iValue);
AL_API void AL_APIENTRY alAuxiliaryEffectSlotiv(ALuint effectslot, ALenum param, const ALint *piValues);
AL_API void AL_APIENTRY alAuxiliaryEffectSlotf(ALuint effectslot, ALenum param, ALfloat flValue);
AL_API void AL_APIENTRY alAuxiliaryEffectSlotfv(ALuint effectslot, ALenum param, const ALfloat *pflValues);
AL_API void AL_APIENTRY alGetAuxiliaryEffectSloti(ALuint effectslot, ALenum param, ALint *piValue);
AL_API void AL_APIENTRY alGetAuxiliaryEffectSlotiv(ALuint effectslot, ALenum param, ALint *piValues);
AL_API void AL_APIENTRY alGetAuxiliaryEffectSlotf(ALuint effectslot, ALenum param, ALfloat *pflValue);
AL_API void AL_APIENTRY alGetAuxiliaryEffectSlotfv(ALuint effectslot, ALenum param, ALfloat *pflValues);
#endif

#ifdef __cplusplus
}
#endif

#endif /* AL_EFX_H */
