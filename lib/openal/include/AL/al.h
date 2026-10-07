#ifndef AL_H
#define AL_H

#ifndef OPENAL_H
#define OPENAL_H

#if defined(__cplusplus)
extern "C" {
#endif

#if defined(_WIN32)
 #define AL_APIENTRY __cdecl
#else
 #define AL_APIENTRY
#endif

#ifndef AL_API
 #if defined(AL_BUILD_DLL)
  #define AL_API __declspec(dllexport)
 #elif defined(AL_USE_DLL)
  #define AL_API __declspec(dllimport)
 #else
  #define AL_API extern
 #endif
#endif

/* Standard OpenAL 1.1 Types */
typedef char ALboolean;
typedef char ALchar;
typedef signed char ALbyte;
typedef unsigned char ALubyte;
typedef short ALshort;
typedef unsigned short ALushort;
typedef int ALint;
typedef unsigned int ALuint;
typedef int ALsizei;
typedef int ALenum;
typedef float ALfloat;
typedef double ALdouble;
typedef void ALvoid;

/* Boolean Values */
#define AL_FALSE                                 0
#define AL_TRUE                                  1

/* Relative / Absolute Mode */
#define AL_SOURCE_RELATIVE                       0x0202

/* Cone & Directional Properties */
#define AL_CONE_INNER_ANGLE                      0x1001
#define AL_CONE_OUTER_ANGLE                      0x1002

/* Source Properties */
#define AL_PITCH                                 0x1003
#define AL_POSITION                              0x1004
#define AL_DIRECTION                             0x1005
#define AL_VELOCITY                              0x1006
#define AL_LOOPING                               0x1007
#define AL_BUFFER                                0x1009
#define AL_GAIN                                  0x100A
#define AL_MIN_GAIN                              0x100D
#define AL_MAX_GAIN                              0x100E
#define AL_ORIENTATION                           0x100F

/* Source State Information */
#define AL_SOURCE_STATE                          0x1010
#define AL_INITIAL                               0x1011
#define AL_PLAYING                               0x1012
#define AL_PAUSED                                0x1013
#define AL_STOPPED                               0x1014

/* Source Buffer Queue Queries */
#define AL_BUFFERS_QUEUED                        0x1015
#define AL_BUFFERS_PROCESSED                     0x1016

/* Distance & Attenuation Properties */
#define AL_REFERENCE_DISTANCE                    0x1020
#define AL_ROLLOFF_FACTOR                        0x1021
#define AL_CONE_OUTER_GAIN                       0x1022
#define AL_MAX_DISTANCE                          0x1023

/* Sound Position / Offset Properties */
#define AL_SEC_OFFSET                            0x1024
#define AL_SAMPLE_OFFSET                         0x1025
#define AL_BYTE_OFFSET                           0x1026

/* Source Type Information */
#define AL_SOURCE_TYPE                           0x1027
#define AL_STATIC                                0x1028
#define AL_STREAMING                             0x1029
#define AL_UNDETERMINED                          0x1030

/* Sound Buffer Formats */
#define AL_FORMAT_MONO8                          0x1100
#define AL_FORMAT_MONO16                         0x1101
#define AL_FORMAT_STEREO8                        0x1102
#define AL_FORMAT_STEREO16                       0x1103

/* Sound Buffer Attributes */
#define AL_FREQUENCY                             0x2001
#define AL_BITS                                  0x2002
#define AL_CHANNELS                              0x2003
#define AL_SIZE                                  0x2004

/* Buffer Queue State */
#define AL_UNUSED                                0x2010
#define AL_PENDING                               0x2011
#define AL_PROCESSED                             0x2012

/* OpenAL 1.1 Error Codes */
#define AL_NO_ERROR                              0
#define AL_INVALID_NAME                          0xA001
#define AL_INVALID_ENUM                          0xA002
#define AL_INVALID_VALUE                         0xA003
#define AL_INVALID_OPERATION                     0xA004
#define AL_OUT_OF_MEMORY                         0xA005

/* Context String Queries (AL_RENDERER also names the active backend, see AL_XBOX_BACKEND in alext.h) */
#define AL_VENDOR                                0xB001
#define AL_VERSION                               0xB002
#define AL_RENDERER                              0xB003
#define AL_EXTENSIONS                            0xB004

/* Global Doppler and Distance Model Properties */
#define AL_DOPPLER_FACTOR                        0xC000
#define AL_DOPPLER_VELOCITY                      0xC001
#define AL_SPEED_OF_SOUND                        0xC003

#define AL_DISTANCE_MODEL                        0xD000
#define AL_INVERSE_DISTANCE                      0xD001
#define AL_INVERSE_DISTANCE_CLAMPED              0xD002
#define AL_LINEAR_DISTANCE                       0xD003
#define AL_LINEAR_DISTANCE_CLAMPED               0xD004
#define AL_EXPONENT_DISTANCE                     0xD005
#define AL_EXPONENT_DISTANCE_CLAMPED             0xD006

#define AL_NONE                                  0

/*
 * ============================================================================
 * OpenAL 1.1 Function Prototypes
 * ============================================================================
 */

/* Renderer State Management */
AL_API void AL_APIENTRY alEnable(ALenum capability);
AL_API void AL_APIENTRY alDisable(ALenum capability);
AL_API ALboolean AL_APIENTRY alIsEnabled(ALenum capability);

/* State Queries */
AL_API const ALchar * AL_APIENTRY alGetString(ALenum param);
AL_API void AL_APIENTRY alGetBooleanv(ALenum param, ALboolean *values);
AL_API void AL_APIENTRY alGetIntegerv(ALenum param, ALint *values);
AL_API void AL_APIENTRY alGetFloatv(ALenum param, ALfloat *values);
AL_API void AL_APIENTRY alGetDoublev(ALenum param, ALdouble *values);
AL_API ALboolean AL_APIENTRY alGetBoolean(ALenum param);
AL_API ALint AL_APIENTRY alGetInteger(ALenum param);
AL_API ALfloat AL_APIENTRY alGetFloat(ALenum param);
AL_API ALdouble AL_APIENTRY alGetDouble(ALenum param);
AL_API ALenum AL_APIENTRY alGetError(void);
AL_API ALboolean AL_APIENTRY alIsExtensionPresent(const ALchar *extname);
AL_API void * AL_APIENTRY alGetProcAddress(const ALchar *fname);
AL_API ALenum AL_APIENTRY alGetEnumValue(const ALchar *ename);

/* Listener Configuration */
AL_API void AL_APIENTRY alListenerf(ALenum param, ALfloat value);
AL_API void AL_APIENTRY alListener3f(ALenum param, ALfloat v1, ALfloat v2, ALfloat v3);
AL_API void AL_APIENTRY alListenerfv(ALenum param, const ALfloat *values);
AL_API void AL_APIENTRY alListeneri(ALenum param, ALint value);
AL_API void AL_APIENTRY alListener3i(ALenum param, ALint v1, ALint v2, ALint v3);
AL_API void AL_APIENTRY alListeneriv(ALenum param, const ALint *values);
AL_API void AL_APIENTRY alGetListenerf(ALenum param, ALfloat *value);
AL_API void AL_APIENTRY alGetListener3f(ALenum param, ALfloat *v1, ALfloat *v2, ALfloat *v3);
AL_API void AL_APIENTRY alGetListenerfv(ALenum param, ALfloat *values);
AL_API void AL_APIENTRY alGetListeneri(ALenum param, ALint *value);
AL_API void AL_APIENTRY alGetListener3i(ALenum param, ALint *v1, ALint *v2, ALint *v3);
AL_API void AL_APIENTRY alGetListeneriv(ALenum param, ALint *values);

/* Source Management */
AL_API void AL_APIENTRY alGenSources(ALsizei n, ALuint *sources);
AL_API void AL_APIENTRY alDeleteSources(ALsizei n, const ALuint *sources);
AL_API ALboolean AL_APIENTRY alIsSource(ALuint source);
AL_API void AL_APIENTRY alSourcef(ALuint source, ALenum param, ALfloat value);
AL_API void AL_APIENTRY alSource3f(ALuint source, ALenum param, ALfloat v1, ALfloat v2, ALfloat v3);
AL_API void AL_APIENTRY alSourcefv(ALuint source, ALenum param, const ALfloat *values);
AL_API void AL_APIENTRY alSourcei(ALuint source, ALenum param, ALint value);
AL_API void AL_APIENTRY alSource3i(ALuint source, ALenum param, ALint v1, ALint v2, ALint v3);
AL_API void AL_APIENTRY alSourceiv(ALuint source, ALenum param, const ALint *values);
AL_API void AL_APIENTRY alGetSourcef(ALuint source, ALenum param, ALfloat *value);
AL_API void AL_APIENTRY alGetSource3f(ALuint source, ALenum param, ALfloat *v1, ALfloat *v2, ALfloat *v3);
AL_API void AL_APIENTRY alGetSourcefv(ALuint source, ALenum param, ALfloat *values);
AL_API void AL_APIENTRY alGetSourcei(ALuint source, ALenum param, ALint *value);
AL_API void AL_APIENTRY alGetSource3i(ALuint source, ALenum param, ALint *v1, ALint *v2, ALint *v3);
AL_API void AL_APIENTRY alGetSourceiv(ALuint source, ALenum param, ALint *values);
AL_API void AL_APIENTRY alSourcePlay(ALuint source);
AL_API void AL_APIENTRY alSourcePause(ALuint source);
AL_API void AL_APIENTRY alSourceStop(ALuint source);
AL_API void AL_APIENTRY alSourceRewind(ALuint source);
AL_API void AL_APIENTRY alSourcePlayv(ALsizei n, const ALuint *sources);
AL_API void AL_APIENTRY alSourcePausev(ALsizei n, const ALuint *sources);
AL_API void AL_APIENTRY alSourceStopv(ALsizei n, const ALuint *sources);
AL_API void AL_APIENTRY alSourceRewindv(ALsizei n, const ALuint *sources);
AL_API void AL_APIENTRY alSourceQueueBuffers(ALuint source, ALsizei nb, const ALuint *buffers);
AL_API void AL_APIENTRY alSourceUnqueueBuffers(ALuint source, ALsizei nb, ALuint *buffers);

/* Buffer Management */
AL_API void AL_APIENTRY alGenBuffers(ALsizei n, ALuint *buffers);
AL_API void AL_APIENTRY alDeleteBuffers(ALsizei n, const ALuint *buffers);
AL_API ALboolean AL_APIENTRY alIsBuffer(ALuint buffer);
AL_API void AL_APIENTRY alBufferData(ALuint buffer, ALenum format, const ALvoid *data, ALsizei size, ALsizei freq);
AL_API void AL_APIENTRY alBufferf(ALuint buffer, ALenum param, ALfloat value);
AL_API void AL_APIENTRY alBuffer3f(ALuint buffer, ALenum param, ALfloat v1, ALfloat v2, ALfloat v3);
AL_API void AL_APIENTRY alBufferfv(ALuint buffer, ALenum param, const ALfloat *values);
AL_API void AL_APIENTRY alBufferi(ALuint buffer, ALenum param, ALint value);
AL_API void AL_APIENTRY alBuffer3i(ALuint buffer, ALenum param, ALint v1, ALint v2, ALint v3);
AL_API void AL_APIENTRY alBufferiv(ALuint buffer, ALenum param, const ALint *values);
AL_API void AL_APIENTRY alGetBufferf(ALuint buffer, ALenum param, ALfloat *value);
AL_API void AL_APIENTRY alGetBuffer3f(ALuint buffer, ALenum param, ALfloat *v1, ALfloat *v2, ALfloat *v3);
AL_API void AL_APIENTRY alGetBufferfv(ALuint buffer, ALenum param, ALfloat *values);
AL_API void AL_APIENTRY alGetBufferi(ALuint buffer, ALenum param, ALint *value);
AL_API void AL_APIENTRY alGetBuffer3i(ALuint buffer, ALenum param, ALint *v1, ALint *v2, ALint *v3);
AL_API void AL_APIENTRY alGetBufferiv(ALuint buffer, ALenum param, ALint *values);

/* Global Distance and Doppler Configuration */
AL_API void AL_APIENTRY alDopplerFactor(ALfloat value);
AL_API void AL_APIENTRY alDopplerVelocity(ALfloat value);
AL_API void AL_APIENTRY alSpeedOfSound(ALfloat value);
AL_API void AL_APIENTRY alDistanceModel(ALenum distanceModel);

/* Function Pointer Type Definitions */
typedef void (AL_APIENTRY *LPALENABLE)(ALenum capability);
typedef void (AL_APIENTRY *LPALDISABLE)(ALenum capability);
typedef ALboolean (AL_APIENTRY *LPALISENABLED)(ALenum capability);
typedef const ALchar * (AL_APIENTRY *LPALGETSTRING)(ALenum param);
typedef void (AL_APIENTRY *LPALGETBOOLEANV)(ALenum param, ALboolean *values);
typedef void (AL_APIENTRY *LPALGETINTEGERV)(ALenum param, ALint *values);
typedef void (AL_APIENTRY *LPALGETFLOATV)(ALenum param, ALfloat *values);
typedef void (AL_APIENTRY *LPALGETDOUBLEV)(ALenum param, ALdouble *values);
typedef ALboolean (AL_APIENTRY *LPALGETBOOLEAN)(ALenum param);
typedef ALint (AL_APIENTRY *LPALGETINTEGER)(ALenum param);
typedef ALfloat (AL_APIENTRY *LPALGETFLOAT)(ALenum param);
typedef ALdouble (AL_APIENTRY *LPALGETDOUBLE)(ALenum param);
typedef ALenum (AL_APIENTRY *LPALGETERROR)(void);
typedef ALboolean (AL_APIENTRY *LPALISEXTENSIONPRESENT)(const ALchar *extname);
typedef void * (AL_APIENTRY *LPALGETPROCADDRESS)(const ALchar *fname);
typedef ALenum (AL_APIENTRY *LPALGETENUMVALUE)(const ALchar *ename);
typedef void (AL_APIENTRY *LPALLISTENERF)(ALenum param, ALfloat value);
typedef void (AL_APIENTRY *LPALLISTENER3F)(ALenum param, ALfloat v1, ALfloat v2, ALfloat v3);
typedef void (AL_APIENTRY *LPALLISTENERFV)(ALenum param, const ALfloat *values);
typedef void (AL_APIENTRY *LPALLISTENERI)(ALenum param, ALint value);
typedef void (AL_APIENTRY *LPALLISTENER3I)(ALenum param, ALint v1, ALint v2, ALint v3);
typedef void (AL_APIENTRY *LPALLISTENERIV)(ALenum param, const ALint *values);
typedef void (AL_APIENTRY *LPALGETLISTENERF)(ALenum param, ALfloat *value);
typedef void (AL_APIENTRY *LPALGETLISTENER3F)(ALenum param, ALfloat *v1, ALfloat *v2, ALfloat *v3);
typedef void (AL_APIENTRY *LPALGETLISTENERFV)(ALenum param, ALfloat *values);
typedef void (AL_APIENTRY *LPALGETLISTENERI)(ALenum param, ALint *value);
typedef void (AL_APIENTRY *LPALGETLISTENER3I)(ALenum param, ALint *v1, ALint *v2, ALint *v3);
typedef void (AL_APIENTRY *LPALGETLISTENERIV)(ALenum param, ALint *values);
typedef void (AL_APIENTRY *LPALGENSOURCES)(ALsizei n, ALuint *sources);
typedef void (AL_APIENTRY *LPALDELETESOURCES)(ALsizei n, const ALuint *sources);
typedef ALboolean (AL_APIENTRY *LPALISSOURCE)(ALuint source);
typedef void (AL_APIENTRY *LPALSOURCEF)(ALuint source, ALenum param, ALfloat value);
typedef void (AL_APIENTRY *LPALSOURCE3F)(ALuint source, ALenum param, ALfloat v1, ALfloat v2, ALfloat v3);
typedef void (AL_APIENTRY *LPALSOURCEFV)(ALuint source, ALenum param, const ALfloat *values);
typedef void (AL_APIENTRY *LPALSOURCEI)(ALuint source, ALenum param, ALint value);
typedef void (AL_APIENTRY *LPALSOURCE3I)(ALuint source, ALenum param, ALint v1, ALint v2, ALint v3);
typedef void (AL_APIENTRY *LPALSOURCEIV)(ALuint source, ALenum param, const ALint *values);
typedef void (AL_APIENTRY *LPALGETSOURCEF)(ALuint source, ALenum param, ALfloat *value);
typedef void (AL_APIENTRY *LPALGETSOURCE3F)(ALuint source, ALenum param, ALfloat *v1, ALfloat *v2, ALfloat *v3);
typedef void (AL_APIENTRY *LPALGETSOURCEFV)(ALuint source, ALenum param, ALfloat *values);
typedef void (AL_APIENTRY *LPALGETSOURCEI)(ALuint source, ALenum param, ALint *value);
typedef void (AL_APIENTRY *LPALGETSOURCE3I)(ALuint source, ALenum param, ALint *v1, ALint *v2, ALint *v3);
typedef void (AL_APIENTRY *LPALGETSOURCEIV)(ALuint source, ALenum param, ALint *values);
typedef void (AL_APIENTRY *LPALSOURCEPLAY)(ALuint source);
typedef void (AL_APIENTRY *LPALSOURCEPAUSE)(ALuint source);
typedef void (AL_APIENTRY *LPALSOURCESTOP)(ALuint source);
typedef void (AL_APIENTRY *LPALSOURCEREWIND)(ALuint source);
typedef void (AL_APIENTRY *LPALSOURCEPLAYV)(ALsizei n, const ALuint *sources);
typedef void (AL_APIENTRY *LPALSOURCEPAUSEV)(ALsizei n, const ALuint *sources);
typedef void (AL_APIENTRY *LPALSOURCESTOPV)(ALsizei n, const ALuint *sources);
typedef void (AL_APIENTRY *LPALSOURCEREWINDV)(ALsizei n, const ALuint *sources);
typedef void (AL_APIENTRY *LPALSOURCEQUEUEBUFFERS)(ALuint source, ALsizei nb, const ALuint *buffers);
typedef void (AL_APIENTRY *LPALSOURCEUNQUEUEBUFFERS)(ALuint source, ALsizei nb, ALuint *buffers);
typedef void (AL_APIENTRY *LPALGENBUFFERS)(ALsizei n, ALuint *buffers);
typedef void (AL_APIENTRY *LPALDELETEBUFFERS)(ALsizei n, const ALuint *buffers);
typedef ALboolean (AL_APIENTRY *LPALISBUFFER)(ALuint buffer);
typedef void (AL_APIENTRY *LPALBUFFERDATA)(ALuint buffer, ALenum format, const ALvoid *data, ALsizei size, ALsizei freq);
typedef void (AL_APIENTRY *LPALBUFFERF)(ALuint buffer, ALenum param, ALfloat value);
typedef void (AL_APIENTRY *LPALBUFFER3F)(ALuint buffer, ALenum param, ALfloat v1, ALfloat v2, ALfloat v3);
typedef void (AL_APIENTRY *LPALBUFFERFV)(ALuint buffer, ALenum param, const ALfloat *values);
typedef void (AL_APIENTRY *LPALBUFFERI)(ALuint buffer, ALenum param, ALint value);
typedef void (AL_APIENTRY *LPALBUFFER3I)(ALuint buffer, ALenum param, ALint v1, ALint v2, ALint v3);
typedef void (AL_APIENTRY *LPALBUFFERIV)(ALuint buffer, ALenum param, const ALint *values);
typedef void (AL_APIENTRY *LPALGETBUFFERF)(ALuint buffer, ALenum param, ALfloat *value);
typedef void (AL_APIENTRY *LPALGETBUFFER3F)(ALuint buffer, ALenum param, ALfloat *v1, ALfloat *v2, ALfloat *v3);
typedef void (AL_APIENTRY *LPALGETBUFFERFV)(ALuint buffer, ALenum param, ALfloat *values);
typedef void (AL_APIENTRY *LPALGETBUFFERI)(ALuint buffer, ALenum param, ALint *value);
typedef void (AL_APIENTRY *LPALGETBUFFER3I)(ALuint buffer, ALenum param, ALint *v1, ALint *v2, ALint *v3);
typedef void (AL_APIENTRY *LPALGETBUFFERIV)(ALuint buffer, ALenum param, ALint *values);
typedef void (AL_APIENTRY *LPALDOPPLERFACTOR)(ALfloat value);
typedef void (AL_APIENTRY *LPALDOPPLERVELOCITY)(ALfloat value);
typedef void (AL_APIENTRY *LPALSPEEDOFSOUND)(ALfloat value);
typedef void (AL_APIENTRY *LPALDISTANCEMODEL)(ALenum distanceModel);

#if defined(__cplusplus)
}
#endif

#endif /* OPENAL_H */
#endif /* AL_H */
