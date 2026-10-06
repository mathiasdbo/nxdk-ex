#ifndef ALC_H
#define ALC_H

#if defined(__cplusplus)
extern "C" {
#endif

#if defined(_WIN32)
 #define ALC_APIENTRY __cdecl
#else
 #define ALC_APIENTRY
#endif

#ifndef ALC_API
 #if defined(AL_BUILD_DLL)
  #define ALC_API __declspec(dllexport)
 #elif defined(AL_USE_DLL)
  #define ALC_API __declspec(dllimport)
 #else
  #define ALC_API extern
 #endif
#endif

/* Opaque Context and Device Handles */
typedef struct ALCdevice_struct ALCdevice;
typedef struct ALCcontext_struct ALCcontext;

/* Standard ALC Types */
typedef char ALCboolean;
typedef char ALCchar;
typedef signed char ALCbyte;
typedef unsigned char ALCubyte;
typedef short ALCshort;
typedef unsigned short ALCushort;
typedef int ALCint;
typedef unsigned int ALCuint;
typedef int ALCsizei;
typedef int ALCenum;
typedef float ALCfloat;
typedef double ALCdouble;
typedef void ALCvoid;

/* Boolean Values */
#define ALC_FALSE                                0
#define ALC_TRUE                                 1

/* Context / Device Attributes */
#define ALC_FREQUENCY                            0x1007
#define ALC_REFRESH                              0x1008
#define ALC_SYNC                                 0x1009
#define ALC_MONO_SOURCES                         0x1010
#define ALC_STEREO_SOURCES                       0x1011

/* ALC Error Codes */
#define ALC_NO_ERROR                             0
#define ALC_INVALID_DEVICE                       0xA001
#define ALC_INVALID_CONTEXT                      0xA002
#define ALC_INVALID_ENUM                         0xA003
#define ALC_INVALID_VALUE                        0xA004
#define ALC_OUT_OF_MEMORY                        0xA005

/* String and Version Queries */
#define ALC_MAJOR_VERSION                        0x1000
#define ALC_MINOR_VERSION                        0x1001
#define ALC_ATTRIBUTES_SIZE                      0x1002
#define ALC_ALL_ATTRIBUTES                       0x1003
#define ALC_DEFAULT_DEVICE_SPECIFIER             0x1004
#define ALC_DEVICE_SPECIFIER                     0x1005
#define ALC_EXTENSIONS                           0x1006

/* Capture Device Queries (OpenAL 1.1) */
#define ALC_CAPTURE_DEVICE_SPECIFIER             0x0310
#define ALC_CAPTURE_DEFAULT_DEVICE_SPECIFIER     0x0311
#define ALC_CAPTURE_SAMPLES                      0x0312

/* Enumerate All Devices Extension Tokens */
#define ALC_DEFAULT_ALL_DEVICES_SPECIFIER        0x1012
#define ALC_ALL_DEVICES_SPECIFIER                0x1013

/*
 * ============================================================================
 * ALC 1.1 Function Prototypes
 * ============================================================================
 */

/* Context Management */
ALC_API ALCcontext * ALC_APIENTRY alcCreateContext(ALCdevice *device, const ALCint *attrlist);
ALC_API ALCboolean   ALC_APIENTRY alcMakeContextCurrent(ALCcontext *context);
ALC_API void         ALC_APIENTRY alcProcessContext(ALCcontext *context);
ALC_API void         ALC_APIENTRY alcSuspendContext(ALCcontext *context);
ALC_API void         ALC_APIENTRY alcDestroyContext(ALCcontext *context);
ALC_API ALCcontext * ALC_APIENTRY alcGetCurrentContext(void);
ALC_API ALCdevice *  ALC_APIENTRY alcGetContextsDevice(ALCcontext *context);

/* Device Management */
ALC_API ALCdevice *  ALC_APIENTRY alcOpenDevice(const ALCchar *devicename);
ALC_API ALCboolean   ALC_APIENTRY alcCloseDevice(ALCdevice *device);

/* Error Support */
ALC_API ALCenum      ALC_APIENTRY alcGetError(ALCdevice *device);

/* Extension Support */
ALC_API ALCboolean   ALC_APIENTRY alcIsExtensionPresent(ALCdevice *device, const ALCchar *extname);
ALC_API void *       ALC_APIENTRY alcGetProcAddress(ALCdevice *device, const ALCchar *funcname);
ALC_API ALCenum      ALC_APIENTRY alcGetEnumValue(ALCdevice *device, const ALCchar *enumname);

/* Query Functions */
ALC_API const ALCchar * ALC_APIENTRY alcGetString(ALCdevice *device, ALCenum param);
ALC_API void         ALC_APIENTRY alcGetIntegerv(ALCdevice *device, ALCenum param, ALCsizei size, ALCint *values);

/* Capture Functions */
ALC_API ALCdevice *  ALC_APIENTRY alcCaptureOpenDevice(const ALCchar *devicename, ALCuint frequency, ALCenum format, ALCsizei buffersize);
ALC_API ALCboolean   ALC_APIENTRY alcCaptureCloseDevice(ALCdevice *device);
ALC_API void         ALC_APIENTRY alcCaptureStart(ALCdevice *device);
ALC_API void         ALC_APIENTRY alcCaptureStop(ALCdevice *device);
ALC_API void         ALC_APIENTRY alcCaptureSamples(ALCdevice *device, ALCvoid *buffer, ALCsizei samples);

/* Function Pointer Type Definitions */
typedef ALCcontext * (ALC_APIENTRY *LPALCCREATECONTEXT)(ALCdevice *device, const ALCint *attrlist);
typedef ALCboolean   (ALC_APIENTRY *LPALCMAKECONTEXTCURRENT)(ALCcontext *context);
typedef void         (ALC_APIENTRY *LPALCPROCESSCONTEXT)(ALCcontext *context);
typedef void         (ALC_APIENTRY *LPALCSUSPENDCONTEXT)(ALCcontext *context);
typedef void         (ALC_APIENTRY *LPALCDESTROYCONTEXT)(ALCcontext *context);
typedef ALCcontext * (ALC_APIENTRY *LPALCGETCURRENTCONTEXT)(void);
typedef ALCdevice *  (ALC_APIENTRY *LPALCGETCONTEXTSDEVICE)(ALCcontext *context);
typedef ALCdevice *  (ALC_APIENTRY *LPALCOPENDEVICE)(const ALCchar *devicename);
typedef ALCboolean   (ALC_APIENTRY *LPALCCLOSEDEVICE)(ALCdevice *device);
typedef ALCenum      (ALC_APIENTRY *LPALCGETERROR)(ALCdevice *device);
typedef ALCboolean   (ALC_APIENTRY *LPALCISEXTENSIONPRESENT)(ALCdevice *device, const ALCchar *extname);
typedef void *       (ALC_APIENTRY *LPALCGETPROCADDRESS)(ALCdevice *device, const ALCchar *funcname);
typedef ALCenum      (ALC_APIENTRY *LPALCGETENUMVALUE)(ALCdevice *device, const ALCchar *enumname);
typedef const ALCchar * (ALC_APIENTRY *LPALCGETSTRING)(ALCdevice *device, ALCenum param);
typedef void         (ALC_APIENTRY *LPALCGETINTEGERV)(ALCdevice *device, ALCenum param, ALCsizei size, ALCint *values);
typedef ALCdevice *  (ALC_APIENTRY *LPALCCAPTUREOPENDEVICE)(const ALCchar *devicename, ALCuint frequency, ALCenum format, ALCsizei buffersize);
typedef ALCboolean   (ALC_APIENTRY *LPALCCAPTURECLOSEDEVICE)(ALCdevice *device);
typedef void         (ALC_APIENTRY *LPALCCAPTURESTART)(ALCdevice *device);
typedef void         (ALC_APIENTRY *LPALCCAPTURESTOP)(ALCdevice *device);
typedef void         (ALC_APIENTRY *LPALCCAPTURESAMPLES)(ALCdevice *device, ALCvoid *buffer, ALCsizei samples);

#if defined(__cplusplus)
}
#endif

#endif /* ALC_H */
