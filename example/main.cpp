#include <cstdio>
#include "../library/source/CASIOClient.h"

#include <cmath>
#include <numbers>
#include <memory>

auto SELECTED_DEVICENAME = "Focusrite USB ASIO";
constexpr double TONE_FREQ = 200.0;

struct MyDeviceStruct {
    CASIO_Device handle;
    CASIO_DeviceProperties props;
    double currentSampleRate;

    // state stuff:
    double samplePos = 0; // cycle 0 to [samplePeriod]
    double samplePeriod;
};

int CDECL asioCallback(CASIO_Event *event, CASIO_Device device, void *userData)
{
    event->handled = true;
    switch (event->eventType) {
    case CASIO_EventType_Log:
        printf("ASIO>> %s\n", event->logEvent.message);
        break;

    case CASIO_EventType_SampleRateChanged:
        printf("Sample rate changed event! %.2f\n", event->sampleRateChangedEvent.newSampleRate);
        break;

    case CASIO_EventType_BufferSwitch:
    {
        const auto asioDevice = static_cast<MyDeviceStruct *>(userData);

        if (asioDevice->props.sampleFormat == CASIO_SampleFormat_Int32) {
            const auto outs = reinterpret_cast<int **>(event->bufferSwitchEvent.outputs);

            for (int j = 0; j < asioDevice->props.bufferSampleLength; j++) {
                const auto fsample = std::sin(asioDevice->samplePos * std::numbers::pi * 2.0 / asioDevice->samplePeriod);
                const auto isample = static_cast<int>(fsample * (1 << 30));
                for (int i = 0; i < asioDevice->props.numOutputs; i++) {
                    outs[i][j] = isample;
                }
                asioDevice->samplePos = fmod(asioDevice->samplePos + 1.0, asioDevice->samplePeriod);
            }
        }
        else {
            // just zero all the buffers, we don't handle that format
            for (int i = 0; i < asioDevice->props.numOutputs; i++) {
                memset(event->bufferSwitchEvent.outputs[i], 0, asioDevice->props.bufferByteLength);
            }
        }
        break;
    }

    default:
        printf("unhandled event type: %d\n", event->eventType);
        event->handled = false;
    }
    return 0;
}

int main()
{
    CASIO_Init(asioCallback);

    CASIO_DeviceInfo *infos;
    int deviceCount = 0;
    CASIO_EnumerateDevices(&infos, &deviceCount);

    // open all the devices and play some simple tones simultaneously
    const auto devs = std::make_unique<MyDeviceStruct[]>(deviceCount);

    float freq = 200.0;
    for (int i = 0; i < deviceCount; i++) {
        printf("===========================================\n");
        if (CASIO_OpenDevice(infos[i].id, &devs[i], &devs[i].handle) != 0) {
            printf("failed to open device %d\n", i);
            return -1;
        }
        CASIO_GetProperties(devs[i].handle, &devs[i].props, &devs[i].currentSampleRate);
        devs[i].samplePos = 0.0;
        devs[i].samplePeriod = devs[i].currentSampleRate / freq;
        freq *= 3.0 / 2.0;
    }

    printf("playing devices for 5 sec...\n");
    for (int i = 0; i < deviceCount; i++) {
        CASIO_Start(devs[i].handle);
    }

    Sleep(5000);

    printf("stopping\n");
    for (int i = 0; i < deviceCount; i++) {
        CASIO_Stop(devs[i].handle);
    }

    for (int i = 0; i < deviceCount; i++) {
        CASIO_CloseDevice(devs[i].handle);
    }

    CASIO_Shutdown();
    return 0;
}
