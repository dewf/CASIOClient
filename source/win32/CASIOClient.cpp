// CASIOClient.cpp : Defines the exported functions for the DLL application.
//

#include "header.h"
#include "../CASIOClient.h"

#include "unicodestuff.h"

#include <objbase.h> // COM stuff
#include <stdio.h>
#include <string>
#include <vector>

#include <assert.h>

#include "../../deps/ASIOSDK2.3/common/iasiodrv.h"

//============ globals =======================================================

#define MAX_REGKEY_LENGTH 512
#define MAX_REGVALUE_LENGTH 512

#define MAX_INPUT_CHANNELS 64
#define MAX_OUTPUT_CHANNELS 64

#define MAX_OPEN_DEVICES 8

static HRESULT hr;
#define MAX_ERROR_LENGTH 1024
static char errorMessage[MAX_ERROR_LENGTH];
static CASIO_EventCallback apiClientCallback = nullptr;

struct _CASIO_DeviceID {
    CLSID clsid;
    std::string name;
};

struct _CASIO_Device {
    CASIO_DeviceID id;
    IASIO *asioDriver;
    void *userData;

    // various internal properties
    char name[512]; // from the COM interface
    long driverVersion;
    long numInputs, numOutputs;
    struct {
        long minSize, maxSize, prefSize, granularity;
        long currentSize;
    } buffer;
    ASIOSampleRate sampleRate;
    bool supportsOutputReady;
    long inputLatency, outputLatency;

    ASIOBufferInfo bufferInfos[MAX_INPUT_CHANNELS + MAX_OUTPUT_CHANNELS];
    ASIOChannelInfo channelInfos[MAX_INPUT_CHANNELS + MAX_OUTPUT_CHANNELS];

    ASIOCallbacks callbacks; // ASIO keeps a pointer to this, not just the content

    // destructured buffer pointers, easier to use in callback
    struct {
        void *inputs[MAX_INPUT_CHANNELS];
        void *outputs[MAX_OUTPUT_CHANNELS];
    } bufferPtrs[2]; // for double buffers

    bool started = false;
    int globalIndex; // since we only support a limited amount of ASIO devices at once, due to the context-less callback mechanism
};

//============ logging stuff =================================================

void logMessage(const char *message) {
    CASIO_Event event;
    event.eventType = CASIO_EventType_Log;
    // .handled doesn't matter
    event.logEvent.message = message;
    apiClientCallback(&event, nullptr, nullptr);
}

#define VSPRINTF_BUFFER_LEN 10*1024
char formatBuffer[VSPRINTF_BUFFER_LEN];
void logFormat(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vsprintf_s<VSPRINTF_BUFFER_LEN>(formatBuffer, format, args);
    va_end(args);
    //
    logMessage(formatBuffer);
}

#define DEV_FORMAT_BUFFER_LEN 1024
char devFormat[DEV_FORMAT_BUFFER_LEN];
void logFormatDev(CASIO_Device d, const char *format, ...) {
    sprintf_s<DEV_FORMAT_BUFFER_LEN>(devFormat, "[%s] %s", d->name, format);
    //
    va_list args;
    va_start(args, format);
    vsprintf_s<VSPRINTF_BUFFER_LEN>(formatBuffer, devFormat, args);
    va_end(args);
    //
    logMessage(formatBuffer);
}

//============ callbacks =====================================================

// these could be avoided if the ASIO header didn't predefine NATIVE_INT64 for us
// (don't want to modify the SDK here to make it easier for other people to compile)
inline UINT64 timestampToUint64(ASIOTimeStamp &x) {
    UINT64 ret;
    ((UINT32 *)&ret)[0] = x.lo;
    ((UINT32 *)&ret)[1] = x.hi;
    return ret;
}
inline UINT64 samplesToUint64(ASIOSamples &x) {
    UINT64 ret;
    ((UINT32 *)&ret)[0] = x.lo;
    ((UINT32 *)&ret)[1] = x.hi;
    return ret;
}

ASIOTime* onBufferSwitchTimeInfo(CASIO_Device device, ASIOTime* timeInfo, long doubleBufferIndex, ASIOBool directProcess)
{
    // new callback with time info. makes ASIOGetSamplePosition() and various
    // calls to ASIOGetSampleRate obsolete,
    // and allows for timecode sync etc. to be preferred; will be used if
    // the driver calls asioMessage with selector kAsioSupportsTimeInfo.

    // (see onBufferSwitch comments for further info)

    // send event
    CASIO_Event event = {};
    event.eventType = CASIO_EventType_BufferSwitch;
    event.handled = false;

    event.bufferSwitchEvent.time.flags = 0;
    if (timeInfo->timeInfo.flags & kSystemTimeValid) {
        event.bufferSwitchEvent.time.nanoSeconds = timestampToUint64(timeInfo->timeInfo.systemTime);
        event.bufferSwitchEvent.time.flags |= CASIO_TimeFlag_NanoSecs;
    }
    if (timeInfo->timeInfo.flags & kSamplePositionValid) {
        event.bufferSwitchEvent.time.samples = samplesToUint64(timeInfo->timeInfo.samplePosition);
        event.bufferSwitchEvent.time.flags |= CASIO_TimeFlag_Samples;
    }
    if (timeInfo->timeCode.flags & kTcValid) {
        event.bufferSwitchEvent.time.tcSamples = samplesToUint64(timeInfo->timeCode.timeCodeSamples);
        event.bufferSwitchEvent.time.flags |= CASIO_TimeFlag_TCSamples;
    }

    event.bufferSwitchEvent.inputs = device->bufferPtrs[doubleBufferIndex].inputs;
    event.bufferSwitchEvent.outputs = device->bufferPtrs[doubleBufferIndex].outputs;
    apiClientCallback(&event, device, device->userData);

    // finally if the driver supports the ASIOOutputReady() optimization, do it here, all data are in place
    if (device->supportsOutputReady) {
        device->asioDriver->outputReady();
    }

    return nullptr; // ?? what of that ASIOTime * we're supposed to return?
}

void onBufferSwitch(CASIO_Device device, long doubleBufferIndex, ASIOBool directProcess)
{
    // the actual processing callback.
    // Beware that this is normally in a seperate thread, hence be sure that you take care
    // about thread synchronization. This is omitted here for simplicity.

    // bufferSwitch indicates that both input and output are to be processed.
    // the current buffer half index (0 for A, 1 for B) determines
    // - the output buffer that the host should start to fill. the other buffer
    //   will be passed to output hardware regardless of whether it got filled
    //   in time or not.
    // - the input buffer that is now filled with incoming data. Note that
    //   because of the synchronicity of i/o, the input always has at
    //   least one buffer latency in relation to the output.
    // directProcess suggests to the host whether it should immedeately
    // start processing (directProcess == ASIOTrue), or whether its process
    // should be deferred because the call comes from a very low level
    // (for instance, a high level priority interrupt), and direct processing
    // would cause timing instabilities for the rest of the system. If in doubt,
    // directProcess should be set to ASIOFalse.
    // Note: bufferSwitch may be called at interrupt time for highest efficiency.

    // as this is a "back door" into the bufferSwitchTimeInfo a timeInfo needs to be created
    // though it will only set the timeInfo.samplePosition and timeInfo.systemTime fields and the according flags
    ASIOTime  timeInfo;
    memset(&timeInfo, 0, sizeof(timeInfo));

    // get the time stamp of the buffer, not necessary if no
    // synchronization to other media is required
    if (device->asioDriver->getSamplePosition(&timeInfo.timeInfo.samplePosition, &timeInfo.timeInfo.systemTime) == ASE_OK) {
        timeInfo.timeInfo.flags = kSystemTimeValid | kSamplePositionValid;
    }

    onBufferSwitchTimeInfo(device, &timeInfo, doubleBufferIndex, directProcess);
}

void onSampleRateDidChange(CASIO_Device device, ASIOSampleRate sRate)
{
    // gets called when the AudioStreamIO detects a sample rate change
    // If sample rate is unknown, 0 is passed (for instance, clock loss
    // when externally synchronized).
    //
    // do whatever you need to do if the sample rate changed
    // usually this only happens during external sync.
    // Audio processing is not stopped by the driver, actual sample rate
    // might not have even changed, maybe only the sample rate status of an
    // AES/EBU or S/PDIF digital input at the audio device.
    // You might have to update time/sample related conversion routines, etc.
    CASIO_Event event;
    event.eventType = CASIO_EventType_SampleRateChanged;
    event.handled = false;
    event.sampleRateChangedEvent.newSampleRate = sRate;
    apiClientCallback(&event, device, device->userData);
}

long onAsioMessage(CASIO_Device device, long selector, long value, void* message, double* opt)
{
    // generic callback for various purposes, see selectors below.
    // note this is only present if the asio version is 2 or higher
    switch (selector) {
    case kAsioSelectorSupported:
        switch (value) {
        case kAsioEngineVersion:
        case kAsioResetRequest:
        case kAsioBufferSizeChange:
        case kAsioResyncRequest:
        case kAsioLatenciesChanged:
        case kAsioSupportsTimeInfo:
        case kAsioSupportsTimeCode:
        case kAsioOverload:
            return 1;
        default:
            return 0;
        }
        break;

    case kAsioEngineVersion:
        // return the supported ASIO version of the host application
        // If a host applications does not implement this selector, ASIO 1.0 is assumed
        // by the driver
        return 2;

    case kAsioResetRequest:
        // defer the task and perform the reset of the driver during the next "safe" situation
        // You cannot reset the driver right now, as this code is called from the driver.
        // Reset the driver is done by completely destruct is. I.e. ASIOStop(), ASIODisposeBuffers(), Destruction
        // Afterwards you initialize the driver again.
        logMessage("kAsioResetRequest");
        return 0;

    case kAsioBufferSizeChange:
        logMessage("kAsioBufferSizeChange");
        return 0;

    case kAsioResyncRequest:
        // This informs the application, that the driver encountered some non fatal data loss.
        // It is used for synchronization purposes of different media.
        // Added mainly to work around the Win16Mutex problems in Windows 95/98 with the
        // Windows Multimedia system, which could loose data because the Mutex was hold too long
        // by another thread.
        // However a driver can issue it in other situations, too.
        logMessage("kAsioResyncRequest");
        return 0;

    case kAsioLatenciesChanged:
        // This will inform the host application that the drivers were latencies changed.
        // Beware, it this does not mean that the buffer sizes have changed!
        // You might need to update internal delay data.
        logMessage("kAsioLatenciesChanged");
        return 0;

    case kAsioSupportsTimeInfo:
        // informs the driver wether the asioCallbacks.bufferSwitchTimeInfo() callback
        // is supported.
        // For compatibility with ASIO 1.0 drivers the host application should always support
        // the "old" bufferSwitch method, too.
        return 1;

    case kAsioSupportsTimeCode:
        // informs the driver wether application is interested in time code info.
        // If an application does not need to know about time code, the driver has less work
        // to do.
        return 0;

    case kAsioOverload:
        logMessage("kAsioOverload!");
        return 1;

    default:
        logFormat("unhandled asioMessage selector %d\n", selector);
    }
    return 0;
}

//============ below is our attempt to support multiple ASIO devices at once, since the callbacks have no user data / context arguments

static int globalNumDevices = 0;
static CASIO_Device globalDevices[MAX_OPEN_DEVICES];

#define ASIO_CALLBACKS_DECL(x) \
ASIOTime* onBufferSwitchTimeInfo_##x(ASIOTime* timeInfo, long doubleBufferIndex, ASIOBool directProcess) { \
    return onBufferSwitchTimeInfo(globalDevices[x], timeInfo, doubleBufferIndex, directProcess); \
} \
void onBufferSwitch_##x(long doubleBufferIndex, ASIOBool directProcess) { \
    onBufferSwitch(globalDevices[x], doubleBufferIndex, directProcess); \
} \
void onSampleRateDidChange_##x(ASIOSampleRate sRate) { \
    onSampleRateDidChange(globalDevices[x], sRate); \
} \
long onAsioMessage_##x(long selector, long value, void* message, double* opt) { \
    return onAsioMessage(globalDevices[x], selector, value, message, opt); \
}

ASIO_CALLBACKS_DECL(0)
ASIO_CALLBACKS_DECL(1)
ASIO_CALLBACKS_DECL(2)
ASIO_CALLBACKS_DECL(3)
ASIO_CALLBACKS_DECL(4)
ASIO_CALLBACKS_DECL(5)
ASIO_CALLBACKS_DECL(6)
ASIO_CALLBACKS_DECL(7)

#define ASIO_CALLBACK_STRUCT(x) { onBufferSwitch_##x, onSampleRateDidChange_##x, onAsioMessage_##x, onBufferSwitchTimeInfo_##x }
static ASIOCallbacks globalCallbacks[] = {
    ASIO_CALLBACK_STRUCT(0),
    ASIO_CALLBACK_STRUCT(1),
    ASIO_CALLBACK_STRUCT(2),
    ASIO_CALLBACK_STRUCT(3),
    ASIO_CALLBACK_STRUCT(4),
    ASIO_CALLBACK_STRUCT(5),
    ASIO_CALLBACK_STRUCT(6),
    ASIO_CALLBACK_STRUCT(7),
};

//==============================================================================

CASIOCLIENT_API int CDECL CASIO_Init(CASIO_EventCallback callback)
{
    apiClientCallback = callback;
    hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED); //COINIT_MULTITHREADED
    if (SUCCEEDED(hr)) {
        logMessage("hello from CASIO_Init");
        return 0;
    }
    else {
        logMessage("CoInitializeEx failed :(");
        return -1;
    }
}

CASIOCLIENT_API int CDECL CASIO_Shutdown()
{
    CoUninitialize();
    logMessage("Goodbye from CASIO_Shutdown");
    return 0;
}


CASIOCLIENT_API int CDECL CASIO_EnumerateDevices(CASIO_DeviceInfo **outInfo, int *outCount)
{
    // list all the keys in HKEY_LOCAL_MACHINE\SOFTWARE\ASIO
    std::vector<CASIO_DeviceInfo> retInfos;
    HKEY asioKey;
    LONG result;
    result = RegOpenKeyW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO", &asioKey);
    if (result == ERROR_SUCCESS) {
        // list devices
        DWORD index = 0;
        WCHAR deviceKeyName[MAX_REGKEY_LENGTH + 1]; // no ASIO devices should have a longer name than that ... (proper way is to use RegQueryInfoKey() to get max subkey len)
        while (true) {
            result = RegEnumKeyW(asioKey, index++, deviceKeyName, MAX_REGKEY_LENGTH); // -1 probably not necessary, but why take chances?
            if (result == ERROR_SUCCESS) {
                // get that key's values
                WCHAR valueStr[MAX_REGVALUE_LENGTH + 1];

                CASIO_DeviceInfo info;
                info.id = new _CASIO_DeviceID;

                // CLSID
                DWORD valueLen = MAX_REGVALUE_LENGTH;
                RegGetValueW(asioKey, deviceKeyName, L"CLSID", RRF_RT_REG_SZ, NULL, valueStr, &valueLen);
                CLSIDFromString(valueStr, &info.id->clsid);

                // description = name
                valueLen = MAX_REGVALUE_LENGTH;
                RegGetValueW(asioKey, deviceKeyName, L"Description", RRF_RT_REG_SZ, NULL, valueStr, &valueLen);
                info.id->name = wstring_to_utf8(valueStr); // internal name is a std::string
                                                           // "external" (visible to client) name is just a const char * from that std::string
                info.name = info.id->name.c_str();

                retInfos.push_back(info);
            }
            else if (result == ERROR_NO_MORE_ITEMS) {
                break;
            }
            else {
                logMessage("unknown reg key enumeration error");
                break;
            }
        }
    }
    // duplicate + return the data in the return vector
    auto count = retInfos.size();
    *outInfo = new CASIO_DeviceInfo[count];
    *outCount = (int)count;
    memcpy(*outInfo, retInfos.data(), sizeof(CASIO_DeviceInfo) * count);
    return 0;
}

CASIOCLIENT_API int CDECL CASIO_OpenDevice(CASIO_DeviceID id, void *userData, CASIO_Device *outDevice)
{
    IASIO *driver;

    GUID iid = id->clsid; // ASIO drivers just re-use their own CLSID as the IASIO interface IID. pretty sure that's wrong, but whatever ...
    hr = CoCreateInstance(id->clsid, NULL, CLSCTX_INPROC_SERVER, iid, (LPVOID *)&driver);
    if (SUCCEEDED(hr)) {
        auto ret = new _CASIO_Device;
        ret->id = id;
        ret->asioDriver = driver;
        ret->userData = userData;
        ret->globalIndex = globalNumDevices++;

        driver->getDriverName(ret->name);
        logFormatDev(ret, "opened successfully (global index %d)", ret->globalIndex);

        ret->driverVersion = driver->getDriverVersion();
        logFormatDev(ret, "driver version: %d", ret->driverVersion);

        if (driver->init(0) != ASIOTrue) { // pass 0 for sysref, since we don't use it (for that matter, why does ASIO want it?)
            driver->getErrorMessage(errorMessage);
            logFormat("init error: %s", errorMessage);
            goto errorExit;
        }
        logFormatDev(ret, "ASIO init OK");

        // formerly init_static_data:
        // get channels
        driver->getChannels(&ret->numInputs, &ret->numOutputs);
        logFormatDev(ret, "channels in/out: %d/%d", ret->numInputs, ret->numOutputs);

        // clamp channels
        ret->numInputs = min(ret->numInputs, MAX_INPUT_CHANNELS);
        ret->numOutputs = min(ret->numOutputs, MAX_OUTPUT_CHANNELS);

        // get buffer size
        driver->getBufferSize(&ret->buffer.minSize, &ret->buffer.maxSize, &ret->buffer.prefSize, &ret->buffer.granularity);
        logFormatDev(ret, "buffer min/max/pref/gran: %d, %d, %d, %d",
            ret->buffer.minSize, ret->buffer.maxSize, ret->buffer.prefSize, ret->buffer.granularity);

        // get sample rate / set sample rate
        driver->getSampleRate(&ret->sampleRate);
        logFormatDev(ret, "current samplerate: %.2f", ret->sampleRate);
        // set it to something specific if not between 0 and 96khz

        // ASIOOutputReady optimization check
        ret->supportsOutputReady = (driver->outputReady() == ASE_OK);
        if (ret->supportsOutputReady) {
            logFormatDev(ret, "driver supports outputRead()");
        }

        // ===== create_asio_buffers ======
        for (int i = 0; i < ret->numInputs + ret->numOutputs; i++) {
            auto info = &ret->bufferInfos[i];
            if (i < ret->numInputs) {
                info->isInput = ASIOTrue;
                info->channelNum = i;
            }
            else {
                info->isInput = ASIOFalse;
                info->channelNum = i - ret->numInputs;
            }
            info->buffers[0] = info->buffers[1] = NULL;
        }

        // our ghetto method of supporting multiple ASIO devices at once
        ret->callbacks = globalCallbacks[ret->globalIndex];

        // immediately assign to globalDevices, lest any callbacks fire as soon as we create buffers (ie, where callbacks are assigned)
        globalDevices[ret->globalIndex] = ret;

        ret->buffer.currentSize = ret->buffer.prefSize;

        if (driver->createBuffers(ret->bufferInfos, ret->numInputs + ret->numOutputs, ret->buffer.currentSize, &ret->callbacks) == ASE_OK) {
            logFormatDev(ret, "successfully created buffers");

            // ASIOGetChannelInfo
            for (int i = 0; i < ret->numInputs + ret->numOutputs; i++) {
                auto info = &ret->channelInfos[i];
                info->channel = ret->bufferInfos[i].channelNum;
                info->isInput = ret->bufferInfos[i].isInput;
                if (driver->getChannelInfo(info) == ASE_OK) {
                    logFormatDev(ret, "  - channel - %s:%d [%s], grp %d, %s, sampletype: %d",
                        info->isInput ? "input" : "output",
                        info->channel,
                        info->name,
                        info->channelGroup,
                        info->isActive ? "active" : "inactive",
                        info->type);
                }
                else {
                    driver->getErrorMessage(errorMessage);
                    logFormatDev(ret, "error getting channel info (%d/%s) - err %s", info->channel, info->isInput ? "input" : "output",
                        errorMessage);
                    //
                    goto errorExit;
                }
            }

            // "destructure" the ASIO double-buffers to make them easier to pass to the client callback
            for (int i = 0; i < ret->numInputs + ret->numOutputs; i++) {
                if (i < ret->numInputs) {
                    ret->bufferPtrs[0].inputs[i] = ret->bufferInfos[i].buffers[0];
                    ret->bufferPtrs[1].inputs[i] = ret->bufferInfos[i].buffers[1];
                }
                else {
                    auto outIndex = i - ret->numInputs;
                    ret->bufferPtrs[0].outputs[outIndex] = ret->bufferInfos[i].buffers[0];
                    ret->bufferPtrs[1].outputs[outIndex] = ret->bufferInfos[i].buffers[1];
                }
            }

            if (driver->getLatencies(&ret->inputLatency, &ret->outputLatency) == ASE_OK) {
                logFormatDev(ret, "i/o latencies: %d/%d", ret->inputLatency, ret->outputLatency);
                // prepared and ready to start!
                *outDevice = ret;
                // already assigned to globalDevices, right before buffers created
                return 0;
            }
            else {
                driver->getErrorMessage(errorMessage);
                logFormatDev(ret, "error getting latencies: %s", errorMessage);
            }
        }
        else {
            driver->getErrorMessage(errorMessage);
            logFormatDev(ret, "failed to create buffers: %s", errorMessage);
        }
    errorExit:
        *outDevice = nullptr;
        driver->Release();
        delete ret;
        return -1;
    }
    else {
        logMessage("COM instantiation failed");
        *outDevice = nullptr;
        return -1;
    }
}

CASIOCLIENT_API int CDECL CASIO_CloseDevice(CASIO_Device device)
{
    if (device) {
        device->asioDriver->disposeBuffers();
        logFormatDev(device, "buffers disposed");
        device->asioDriver->Release();
        globalDevices[device->globalIndex] = nullptr;
        logFormatDev(device, "COM instance released");
        delete device;
    }
    return 0;
}

CASIOCLIENT_API int CDECL CASIO_Start(CASIO_Device device)
{
    if (!device->started) {
        if (device->asioDriver->start() == ASE_OK) {
            logFormatDev(device, "ASIO playback started");
            device->started = true;
            return 0;
        }
    }
    return -1;
}

CASIOCLIENT_API int CDECL CASIO_Stop(CASIO_Device device)
{
    if (device->started) {
        if (device->asioDriver->stop() == ASE_OK) {
            logFormatDev(device, "ASIO playback stopped");
            device->started = false;
            return 0;
        }
    }
    return -1;
}

int getSampleSize(ASIOSampleType sampleType) {
    switch (sampleType)
    {
    case ASIOSTInt16LSB:
    case ASIOSTInt16MSB:
        return 2;

    case ASIOSTInt24LSB:
    case ASIOSTInt24MSB:
        return 3;

    case ASIOSTInt32LSB:
    case ASIOSTInt32MSB:
    case ASIOSTFloat32LSB:
    case ASIOSTFloat32MSB:
    case ASIOSTInt32LSB16:
    case ASIOSTInt32LSB18:
    case ASIOSTInt32LSB20:
    case ASIOSTInt32LSB24:
    case ASIOSTInt32MSB16:
    case ASIOSTInt32MSB18:
    case ASIOSTInt32MSB20:
    case ASIOSTInt32MSB24:
        return 4;

    case ASIOSTFloat64LSB:
    case ASIOSTFloat64MSB:
        return 8;

    default:
        return -1;
    }
}


CASIOCLIENT_API int CDECL CASIO_GetProperties(CASIO_Device device, CASIO_DeviceProperties *props, double *currentSampleRate)
{
    props->name = device->name;
    props->numInputs = device->numInputs;
    props->numOutputs = device->numOutputs;
    props->bufferSampleLength = device->buffer.currentSize;

    auto sampleType = device->channelInfos[0].type;

    props->bufferByteLength = device->buffer.currentSize * getSampleSize(sampleType);

    switch (sampleType) {
    case ASIOSTInt32LSB:
        props->sampleFormat = CASIO_SampleFormat_Int32;
        break;
    case ASIOSTFloat32LSB:
        props->sampleFormat = CASIO_SampleFormat_Float32;
        break;
    case ASIOSTFloat64LSB:
        props->sampleFormat = CASIO_SampleFormat_Float64;
        break;
    default:
        props->sampleFormat = CASIO_SampleFormat_Unknown;
    }

    // sample rate is separate because it can change ...
    *currentSampleRate = device->sampleRate;
    return 0;
}

CASIOCLIENT_API int CDECL CASIO_ShowControlPanel(CASIO_Device device)
{
    if (device->asioDriver->controlPanel() != ASE_OK) {
        logFormatDev(device, "failed to show control panel");
        return -1;
    }
    return 0;
}
