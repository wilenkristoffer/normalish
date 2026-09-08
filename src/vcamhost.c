#include "vcamhost.h"
#include "frameshare.h"

#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfvirtualcamera.h>

#include <stdio.h>
#include <string.h>

// Must match dllmain.cpp in the media source, and the HKLM registration.
static const wchar_t *kClsid = L"{B8B0C2A4-9D3E-4F71-9A2C-7E1F5D6A3C10}";
static const wchar_t *kFriendlyName = L"normalish camera";

// Must match NUM_IMAGE_COLS/ROWS in the media source's MediaStream.cpp.
static const int kWidth = 1280;
static const int kHeight = 720;

static bool mfStarted = false;
static IMFVirtualCamera *camera = NULL;
static const char *status = "off";

static HANDLE frameFile = INVALID_HANDLE_VALUE;
static HANDLE frameMapping = NULL;
static unsigned char *frameView = NULL;

int GetVirtualCameraWidth(void) { return kWidth; }
int GetVirtualCameraHeight(void) { return kHeight; }
const char *GetVirtualCameraStatus(void) { return status; }
bool IsVirtualCameraRunning(void) { return camera != NULL; }

static bool EnsureMediaFoundation(void)
{
    if (mfStarted) return true;

    // raylib's GLFW has already put this thread in a single-threaded
    // apartment, so asking for multi-threaded returns RPC_E_CHANGED_MODE.
    // That is not a failure - COM is initialised, just not the way we asked -
    // and treating it as one made the whole feature look unsupported.
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(hr) && (hr != RPC_E_CHANGED_MODE)) return false;

    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) return false;
    mfStarted = true;
    return true;
}

bool IsVirtualCameraSupported(void)
{
    if (!EnsureMediaFoundation()) return false;
    BOOL supported = FALSE;
    if (FAILED(MFIsVirtualCameraTypeSupported(MFVirtualCameraType_SoftwareCameraSource, &supported))) return false;
    return supported ? true : false;
}

bool IsVirtualCameraRegistered(void)
{
    // The media source is registered machine-wide; its absence is the usual
    // reason the camera cannot start, and it is worth reporting distinctly.
    wchar_t key[256];
    swprintf_s(key, 256, L"Software\\Classes\\CLSID\\%s\\InprocServer32", kClsid);

    HKEY handle = NULL;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key, 0, KEY_READ, &handle) != ERROR_SUCCESS) return false;
    RegCloseKey(handle);
    return true;
}

// The shared file is created here rather than in the DLL, because the DLL runs
// as a service account and normalish is the one that knows where its own data
// lives. ProgramData is writable by the user and readable by services.
static bool EnsureFrameFile(void)
{
    if (frameView != NULL) return true;

    CreateDirectoryA("C:\\ProgramData\\normalish", NULL);

    frameFile = CreateFileA(NORMALISH_FRAME_PATH, GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (frameFile == INVALID_HANDLE_VALUE) return false;

    frameMapping = CreateFileMappingA(frameFile, NULL, PAGE_READWRITE,
                                      0, (DWORD)NORMALISH_FRAME_FILE_BYTES, NULL);
    if (frameMapping == NULL) return false;

    frameView = (unsigned char *)MapViewOfFile(frameMapping, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (frameView == NULL) return false;

    NormalishFrameHeader *header = (NormalishFrameHeader *)frameView;
    header->magic = NORMALISH_FRAME_MAGIC;
    header->version = NORMALISH_FRAME_VERSION;
    header->width = (unsigned int)kWidth;
    header->height = (unsigned int)kHeight;
    header->sequence = 0;
    header->reserved = 0;
    return true;
}

static void ReleaseFrameFile(void)
{
    if (frameView != NULL) { UnmapViewOfFile(frameView); frameView = NULL; }
    if (frameMapping != NULL) { CloseHandle(frameMapping); frameMapping = NULL; }
    if (frameFile != INVALID_HANDLE_VALUE) { CloseHandle(frameFile); frameFile = INVALID_HANDLE_VALUE; }
}

bool PublishVirtualCameraFrame(const unsigned char *bgra, int width, int height)
{
    if ((bgra == NULL) || (width != kWidth) || (height != kHeight)) return false;
    if (!EnsureFrameFile()) return false;

    NormalishFrameHeader *header = (NormalishFrameHeader *)frameView;
    unsigned char *pixels = frameView + sizeof(NormalishFrameHeader);
    size_t bytes = (size_t)width*height*4;

    // Seqlock: odd while writing tells the reader to skip this frame rather
    // than copy a half-written one. No cross-session event needed.
    header->sequence++;
    MemoryBarrier();
    memcpy(pixels, bgra, bytes);
    MemoryBarrier();
    header->sequence++;
    return true;
}

bool StartVirtualCamera(void)
{
    if (camera != NULL) return true;

    if (!EnsureMediaFoundation())
    {
        status = "Media Foundation unavailable";
        return false;
    }
    if (!IsVirtualCameraSupported())
    {
        status = "not supported on this Windows version";
        return false;
    }
    if (!IsVirtualCameraRegistered())
    {
        status = "camera support not installed";
        return false;
    }

    // Publish a frame before the camera exists, so the first consumer to
    // connect sees our output rather than the fallback pattern.
    EnsureFrameFile();

    HRESULT hr = MFCreateVirtualCamera(MFVirtualCameraType_SoftwareCameraSource,
                                       MFVirtualCameraLifetime_Session,
                                       MFVirtualCameraAccess_CurrentUser,
                                       kFriendlyName, kClsid, NULL, 0, &camera);
    if (FAILED(hr))
    {
        camera = NULL;
        status = "could not create the camera";
        return false;
    }

    hr = IMFVirtualCamera_Start(camera, NULL);
    if (FAILED(hr))
    {
        IMFVirtualCamera_Release(camera);
        camera = NULL;
        status = "camera created but would not start";
        return false;
    }

    status = "live";
    return true;
}

void StopVirtualCamera(void)
{
    if (camera != NULL)
    {
        IMFVirtualCamera_Stop(camera);
        IMFVirtualCamera_Remove(camera);
        IMFVirtualCamera_Release(camera);
        camera = NULL;
    }
    ReleaseFrameFile();
    status = "off";
}
