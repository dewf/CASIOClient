#pragma once

#ifdef CASIOCLIENT_EXPORTS
#define CASIOCLIENT_API __declspec(dllexport)
#else
#define CASIOCLIENT_API __declspec(dllimport)
#endif

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#ifndef __cplusplus
#include <stdbool.h>
#endif

#define APIHANDLE(x) struct _##x; typedef struct _##x* x

#ifdef __cplusplus
extern "C" {
#endif

    APIHANDLE(CASIO_DeviceID); // device descriptor, unique handle to open a particular device
    APIHANDLE(CASIO_Device); // actual opened device

    typedef enum {
        CASIO_EventType_Log,
        CASIO_EventType_BufferSwitch,
        CASIO_EventType_SampleRateChanged
    } CASIO_EventType;

    typedef enum {
        CASIO_TimeFlag_NanoSecs = 1 << 0,
        CASIO_TimeFlag_Samples = 1 << 1,
        CASIO_TimeFlag_TCSamples = 1 << 2
    } CASIO_TimeFlags;

    typedef struct {
        CASIO_EventType eventType;
        bool handled;
        union {
            struct {
                const char *message;
            } logEvent;
            struct {
                // use CASIO_DeviceProperties to interpret these (count + sample type)
                void **inputs;
                void **outputs;
                struct {
                    unsigned int flags; // CASIO_TimeFlags
                    UINT64 nanoSeconds;
                    UINT64 samples;
                    UINT64 tcSamples;
                } time;
            } bufferSwitchEvent;
            struct {
                double newSampleRate;
            } sampleRateChangedEvent;
        };
    } CASIO_Event;

    typedef int(CDECL *CASIO_EventCallback)(CASIO_Event *event, CASIO_Device device, void *userData);

    CASIOCLIENT_API int CDECL CASIO_Init(CASIO_EventCallback callback);
    CASIOCLIENT_API int CDECL CASIO_Shutdown();

    typedef struct {
        CASIO_DeviceID id;
        const char *name;
    } CASIO_DeviceInfo;
    CASIOCLIENT_API int CDECL CASIO_EnumerateDevices(CASIO_DeviceInfo **outInfo, int *outCount);

    typedef enum {
        CASIO_SampleFormat_Unknown,
        CASIO_SampleFormat_Int32, // all little-endian
        CASIO_SampleFormat_Float32,
        CASIO_SampleFormat_Float64
    } CASIO_SampleFormat;

    typedef struct {
        const char *name;
        int numInputs, numOutputs;
        int bufferSampleLength;
        int bufferByteLength;
        CASIO_SampleFormat sampleFormat;
    } CASIO_DeviceProperties;

    CASIOCLIENT_API int CDECL CASIO_OpenDevice(CASIO_DeviceID id, void *userData, CASIO_Device *outDevice);
    CASIOCLIENT_API int CDECL CASIO_CloseDevice(CASIO_Device device);

    CASIOCLIENT_API int CDECL CASIO_GetProperties(CASIO_Device device, CASIO_DeviceProperties *props, double *currentSampleRate);

    CASIOCLIENT_API int CDECL CASIO_Start(CASIO_Device device);
    CASIOCLIENT_API int CDECL CASIO_Stop(CASIO_Device device);

    CASIOCLIENT_API int CDECL CASIO_ShowControlPanel(CASIO_Device device);

#ifdef __cplusplus
}
#endif
