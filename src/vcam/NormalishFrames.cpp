#include "pch.h"
#include <vector>
#include "NormalishFrames.h"
#include "../frameshare.h"

NormalishFrames::~NormalishFrames()
{
    Unmap();
}

void NormalishFrames::Unmap()
{
    if (_view != nullptr) { UnmapViewOfFile(_view); _view = nullptr; }
    if (_mapping != nullptr) { CloseHandle(_mapping); _mapping = nullptr; }
    if (_file != INVALID_HANDLE_VALUE) { CloseHandle(_file); _file = INVALID_HANDLE_VALUE; }
}

bool NormalishFrames::EnsureMapped()
{
    if (_view != nullptr) return true;

    // normalish may not be running, and this is called per frame. Retrying the
    // open every 30 ms is plenty and keeps the failure path cheap.
    ULONGLONG now = GetTickCount64();
    if ((_lastAttempt != 0) && ((now - _lastAttempt) < 30)) return false;
    _lastAttempt = now;

    _file = CreateFileW(NORMALISH_FRAME_PATH_W, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (_file == INVALID_HANDLE_VALUE) return false;

    _mapping = CreateFileMappingW(_file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (_mapping == nullptr) { Unmap(); return false; }

    _view = (BYTE*)MapViewOfFile(_mapping, FILE_MAP_READ, 0, 0, 0);
    if (_view == nullptr) { Unmap(); return false; }
    return true;
}

// BT.601, matching what capture pipelines expect from a camera.
static inline void BgraToYuv(BYTE b, BYTE g, BYTE r, BYTE* y, BYTE* u, BYTE* v)
{
    *y = (BYTE)(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
    *u = (BYTE)(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
    *v = (BYTE)(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
}

bool NormalishFrames::TryFill(BYTE* destination, UINT32 width, UINT32 height, const GUID& format)
{
    if (destination == nullptr) return false;
    if (!EnsureMapped()) return false;

    auto header = (NormalishFrameHeader*)_view;
    if (header->magic != NORMALISH_FRAME_MAGIC) return false;
    if (header->version != NORMALISH_FRAME_VERSION) return false;
    if ((header->width != width) || (header->height != height)) return false;

    const size_t frameBytes = (size_t)width * height * 4;
    if (_staging.size() < frameBytes) _staging.resize(frameBytes);

    // Seqlock: an odd sequence means a write is in progress, and a changed
    // sequence means the frame was replaced mid-copy. Either way, skip this
    // one - the next request is 33 ms away.
    unsigned int before = header->sequence;
    if ((before & 1u) != 0u) return false;
    MemoryBarrier();
    memcpy(_staging.data(), _view + sizeof(NormalishFrameHeader), frameBytes);
    MemoryBarrier();
    if (header->sequence != before) return false;

    const BYTE* source = _staging.data();

    if (format == MFVideoFormat_RGB32)
    {
        // Already BGRA, and normalish writes rows top-down for us.
        memcpy(destination, source, frameBytes);
        return true;
    }

    if (format == MFVideoFormat_NV12)
    {
        BYTE* luma = destination;
        BYTE* chroma = destination + (size_t)width * height;

        for (UINT32 row = 0; row < height; row++)
        {
            const BYTE* in = source + (size_t)row * width * 4;
            BYTE* outY = luma + (size_t)row * width;
            for (UINT32 x = 0; x < width; x++)
            {
                BYTE y, u, v;
                BgraToYuv(in[x * 4 + 0], in[x * 4 + 1], in[x * 4 + 2], &y, &u, &v);
                outY[x] = y;
            }
        }

        // One UV pair per 2x2 block, sampled from its top-left pixel.
        for (UINT32 row = 0; row < height / 2; row++)
        {
            const BYTE* in = source + (size_t)(row * 2) * width * 4;
            BYTE* outUV = chroma + (size_t)row * width;
            for (UINT32 x = 0; x < width / 2; x++)
            {
                BYTE y, u, v;
                const BYTE* p = in + (size_t)(x * 2) * 4;
                BgraToYuv(p[0], p[1], p[2], &y, &u, &v);
                outUV[x * 2 + 0] = u;
                outUV[x * 2 + 1] = v;
            }
        }
        return true;
    }

    return false;
}
