#include "webcam.h"

#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>

#include <stdlib.h>
#include <string.h>

#define MAX_DEVICES 8
#define MAX_DEVICE_NAME 128

static bool systemReady = false;
static char deviceNames[MAX_DEVICES][MAX_DEVICE_NAME];
static int deviceCount = 0;

static IMFSourceReader *reader = NULL;
static int frameWidth = 0;
static int frameHeight = 0;
static bool bottomUp = false;
static const char *status = "not initialised";

static HANDLE captureThread = NULL;
static CRITICAL_SECTION frameLock;
static bool lockCreated = false;
static unsigned char *sharedFrame = NULL;
static volatile LONG hasNewFrame = 0;
static volatile LONG stopRequested = 0;

int GetWebcamWidth(void) { return frameWidth; }
int GetWebcamHeight(void) { return frameHeight; }
const char *GetWebcamStatus(void) { return status; }
int GetWebcamDeviceCount(void) { return deviceCount; }

const char *GetWebcamDeviceName(int index)
{
    if ((index < 0) || (index >= deviceCount)) return "";
    return deviceNames[index];
}

// MF_MT_FRAME_SIZE is stored as a packed UINT64 with the width in the high 32
// bits. MFGetAttributeSize would unpack it, but that helper is not exported by
// any library in the current Windows SDK, so unpack it here instead.
static bool GetFrameSize(IMFMediaType *type, UINT32 *width, UINT32 *height)
{
    UINT64 packed = 0;
    if (FAILED(IMFMediaType_GetUINT64(type, &MF_MT_FRAME_SIZE, &packed))) return false;
    *width = (UINT32)(packed >> 32);
    *height = (UINT32)(packed & 0xFFFFFFFFu);
    return true;
}

// Media Foundation hands us BGRX rows; the renderer wants RGBA. Rows may also
// be stored bottom-up, which the sign of the stride tells us.
static void CopyFrameConverted(const BYTE *source, unsigned char *destination)
{
    int rowBytes = frameWidth*4;
    for (int y = 0; y < frameHeight; y++)
    {
        const BYTE *src = source + (size_t)(bottomUp ? (frameHeight - 1 - y) : y)*rowBytes;
        unsigned char *dst = destination + (size_t)y*rowBytes;
        for (int x = 0; x < frameWidth; x++)
        {
            dst[x*4 + 0] = src[x*4 + 2];
            dst[x*4 + 1] = src[x*4 + 1];
            dst[x*4 + 2] = src[x*4 + 0];
            dst[x*4 + 3] = 255;
        }
    }
}

static DWORD WINAPI CaptureThreadProc(LPVOID parameter)
{
    (void)parameter;
    unsigned char *scratch = (unsigned char *)malloc((size_t)frameWidth*frameHeight*4);
    if (scratch == NULL) return 1;

    while (InterlockedCompareExchange(&stopRequested, 0, 0) == 0)
    {
        DWORD streamIndex = 0;
        DWORD flags = 0;
        LONGLONG timestamp = 0;
        IMFSample *sample = NULL;

        HRESULT hr = IMFSourceReader_ReadSample(reader, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                                0, &streamIndex, &flags, &timestamp, &sample);
        if (FAILED(hr)) break;
        if (sample == NULL) continue;   // a stream tick carrying no data

        IMFMediaBuffer *buffer = NULL;
        if (SUCCEEDED(IMFSample_ConvertToContiguousBuffer(sample, &buffer)))
        {
            BYTE *data = NULL;
            DWORD maxLength = 0;
            DWORD currentLength = 0;
            if (SUCCEEDED(IMFMediaBuffer_Lock(buffer, &data, &maxLength, &currentLength)))
            {
                if (currentLength >= (DWORD)(frameWidth*frameHeight*4))
                {
                    CopyFrameConverted(data, scratch);

                    EnterCriticalSection(&frameLock);
                    memcpy(sharedFrame, scratch, (size_t)frameWidth*frameHeight*4);
                    LeaveCriticalSection(&frameLock);
                    InterlockedExchange(&hasNewFrame, 1);
                }
                IMFMediaBuffer_Unlock(buffer);
            }
            IMFMediaBuffer_Release(buffer);
        }
        IMFSample_Release(sample);
    }

    free(scratch);
    return 0;
}

// Returns the activation objects for all video capture devices. Caller must
// release each entry and CoTaskMemFree the array.
static bool EnumerateDevices(IMFActivate ***devices, UINT32 *count)
{
    IMFAttributes *config = NULL;
    if (FAILED(MFCreateAttributes(&config, 1))) return false;
    IMFAttributes_SetGUID(config, &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                          &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);

    HRESULT hr = MFEnumDeviceSources(config, devices, count);
    IMFAttributes_Release(config);
    return SUCCEEDED(hr);
}

static void StoreDeviceName(int slot, IMFActivate *device)
{
    WCHAR *wide = NULL;
    UINT32 wideLength = 0;

    if (SUCCEEDED(IMFActivate_GetAllocatedString(device, &MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME,
                                                 &wide, &wideLength)))
    {
        WideCharToMultiByte(CP_UTF8, 0, wide, -1, deviceNames[slot], MAX_DEVICE_NAME, NULL, NULL);
        deviceNames[slot][MAX_DEVICE_NAME - 1] = '\0';
        CoTaskMemFree(wide);
    }
    else
    {
        deviceNames[slot][0] = '\0';
    }

    if (deviceNames[slot][0] == '\0')
    {
        deviceNames[slot][0] = 'C';
        deviceNames[slot][1] = 'a';
        deviceNames[slot][2] = 'm';
        deviceNames[slot][3] = '\0';
    }

    // raygui separates dropdown entries with semicolons, so a name containing
    // one would split into two bogus entries.
    for (int i = 0; deviceNames[slot][i] != '\0'; i++)
    {
        if (deviceNames[slot][i] == ';') deviceNames[slot][i] = ',';
    }
}

bool InitWebcamSystem(void)
{
    if (FAILED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)))
    {
        status = "COM init failed";
        return false;
    }
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE)))
    {
        status = "Media Foundation init failed";
        return false;
    }
    systemReady = true;

    IMFActivate **devices = NULL;
    UINT32 found = 0;
    if (!EnumerateDevices(&devices, &found))
    {
        status = "could not enumerate devices";
        return false;
    }

    deviceCount = 0;
    for (UINT32 i = 0; i < found; i++)
    {
        if (deviceCount < MAX_DEVICES)
        {
            StoreDeviceName(deviceCount, devices[i]);
            deviceCount++;
        }
        IMFActivate_Release(devices[i]);
    }
    if (devices != NULL) CoTaskMemFree(devices);

    if (deviceCount == 0)
    {
        status = "no capture device found";
        return false;
    }

    status = "ready";
    return true;
}

// Picks the camera mode whose pixel count is closest to the requested size.
// Running the camera itself at a small resolution is far cheaper than
// capturing 4K frames and scaling them down afterwards.
static bool SelectClosestNativeMode(int preferredWidth, int preferredHeight)
{
    long long preferredArea = (long long)preferredWidth*preferredHeight;
    IMFMediaType *bestType = NULL;
    long long bestDistance = 0;

    for (DWORD index = 0;; index++)
    {
        IMFMediaType *type = NULL;
        HRESULT hr = IMFSourceReader_GetNativeMediaType(reader, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                                        index, &type);
        if (FAILED(hr)) break;

        UINT32 width = 0;
        UINT32 height = 0;
        bool kept = false;
        if (GetFrameSize(type, &width, &height))
        {
            long long area = (long long)width*height;
            long long distance = (area > preferredArea) ? (area - preferredArea) : (preferredArea - area);
            if ((bestType == NULL) || (distance < bestDistance))
            {
                if (bestType != NULL) IMFMediaType_Release(bestType);
                bestType = type;
                bestDistance = distance;
                kept = true;
            }
        }
        if (!kept) IMFMediaType_Release(type);
    }

    if (bestType == NULL) return false;

    HRESULT hr = IMFSourceReader_SetCurrentMediaType(reader, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                                     NULL, bestType);
    IMFMediaType_Release(bestType);
    return SUCCEEDED(hr);
}

static bool RequestRgb32Output(void)
{
    IMFMediaType *outputType = NULL;
    if (FAILED(MFCreateMediaType(&outputType))) return false;

    IMFMediaType_SetGUID(outputType, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    IMFMediaType_SetGUID(outputType, &MF_MT_SUBTYPE, &MFVideoFormat_RGB32);
    HRESULT hr = IMFSourceReader_SetCurrentMediaType(reader, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                                     NULL, outputType);
    IMFMediaType_Release(outputType);
    if (FAILED(hr)) return false;

    IMFMediaType *currentType = NULL;
    if (FAILED(IMFSourceReader_GetCurrentMediaType(reader, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                                   &currentType))) return false;

    UINT32 width = 0;
    UINT32 height = 0;
    bool haveSize = GetFrameSize(currentType, &width, &height);
    if (haveSize)
    {
        frameWidth = (int)width;
        frameHeight = (int)height;
    }

    UINT32 stride = 0;
    if (SUCCEEDED(IMFMediaType_GetUINT32(currentType, &MF_MT_DEFAULT_STRIDE, &stride)))
    {
        bottomUp = ((INT32)stride < 0);
    }

    IMFMediaType_Release(currentType);
    return haveSize && (frameWidth > 0) && (frameHeight > 0);
}

bool OpenWebcam(int deviceIndex, int preferredWidth, int preferredHeight)
{
    if (!systemReady)
    {
        status = "webcam system not started";
        return false;
    }
    if ((deviceIndex < 0) || (deviceIndex >= deviceCount))
    {
        status = "no such device";
        return false;
    }

    CloseWebcam();

    IMFActivate **devices = NULL;
    UINT32 found = 0;
    if (!EnumerateDevices(&devices, &found) || ((UINT32)deviceIndex >= found))
    {
        status = "device list changed";
        if (devices != NULL)
        {
            for (UINT32 i = 0; i < found; i++) IMFActivate_Release(devices[i]);
            CoTaskMemFree(devices);
        }
        return false;
    }

    IMFMediaSource *source = NULL;
    HRESULT hr = IMFActivate_ActivateObject(devices[deviceIndex], &IID_IMFMediaSource, (void **)&source);
    for (UINT32 i = 0; i < found; i++) IMFActivate_Release(devices[i]);
    CoTaskMemFree(devices);
    if (FAILED(hr))
    {
        status = "camera busy or blocked";
        return false;
    }

    IMFAttributes *readerAttributes = NULL;
    if (FAILED(MFCreateAttributes(&readerAttributes, 1)))
    {
        IMFMediaSource_Release(source);
        status = "out of memory";
        return false;
    }
    // Lets the reader convert and scale for us, so we can simply ask for RGB32
    // rather than decoding the camera's MJPG or YUY2 ourselves.
    IMFAttributes_SetUINT32(readerAttributes, &MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);

    hr = MFCreateSourceReaderFromMediaSource(source, readerAttributes, &reader);
    IMFAttributes_Release(readerAttributes);
    IMFMediaSource_Release(source);
    if (FAILED(hr))
    {
        reader = NULL;
        status = "could not create source reader";
        return false;
    }

    if (!SelectClosestNativeMode(preferredWidth, preferredHeight))
    {
        status = "no usable capture mode";
        CloseWebcam();
        return false;
    }
    if (!RequestRgb32Output())
    {
        status = "camera cannot deliver RGB32";
        CloseWebcam();
        return false;
    }

    sharedFrame = (unsigned char *)malloc((size_t)frameWidth*frameHeight*4);
    if (sharedFrame == NULL)
    {
        status = "out of memory";
        CloseWebcam();
        return false;
    }
    memset(sharedFrame, 0, (size_t)frameWidth*frameHeight*4);

    InitializeCriticalSection(&frameLock);
    lockCreated = true;

    InterlockedExchange(&stopRequested, 0);
    InterlockedExchange(&hasNewFrame, 0);

    captureThread = CreateThread(NULL, 0, CaptureThreadProc, NULL, 0, NULL);
    if (captureThread == NULL)
    {
        status = "could not start capture thread";
        CloseWebcam();
        return false;
    }

    status = "capturing";
    return true;
}

bool UpdateWebcamFrame(unsigned char *destination)
{
    if ((sharedFrame == NULL) || (destination == NULL)) return false;
    if (InterlockedExchange(&hasNewFrame, 0) == 0) return false;

    EnterCriticalSection(&frameLock);
    memcpy(destination, sharedFrame, (size_t)frameWidth*frameHeight*4);
    LeaveCriticalSection(&frameLock);
    return true;
}

void CloseWebcam(void)
{
    if (captureThread != NULL)
    {
        InterlockedExchange(&stopRequested, 1);
        // Unblocks a ReadSample that is waiting on the next camera frame.
        if (reader != NULL) IMFSourceReader_Flush(reader, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM);
        WaitForSingleObject(captureThread, 2000);
        CloseHandle(captureThread);
        captureThread = NULL;
    }
    if (lockCreated)
    {
        DeleteCriticalSection(&frameLock);
        lockCreated = false;
    }
    if (sharedFrame != NULL)
    {
        free(sharedFrame);
        sharedFrame = NULL;
    }
    if (reader != NULL)
    {
        IMFSourceReader_Release(reader);
        reader = NULL;
    }
    frameWidth = 0;
    frameHeight = 0;
}

void ShutdownWebcamSystem(void)
{
    CloseWebcam();
    if (systemReady)
    {
        MFShutdown();
        CoUninitialize();
        systemReady = false;
    }
}
