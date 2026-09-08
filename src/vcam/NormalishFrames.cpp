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

// One seqlock read attempt into _lastGood. An odd sequence means a write is in
// progress; a changed sequence means the frame was replaced mid-copy.
bool NormalishFrames::Acquire(UINT32 width, UINT32 height)
{
    auto header = (NormalishFrameHeader*)_view;
    if (header->magic != NORMALISH_FRAME_MAGIC) return false;
    if (header->version != NORMALISH_FRAME_VERSION) return false;
    if ((header->width != width) || (header->height != height)) return false;

    const size_t frameBytes = (size_t)width * height * 4;
    if (_lastGood.size() < frameBytes) _lastGood.resize(frameBytes);

    unsigned int before = header->sequence;
    if ((before & 1u) != 0u) return false;
    MemoryBarrier();
    memcpy(_lastGood.data(), _view + sizeof(NormalishFrameHeader), frameBytes);
    MemoryBarrier();
    return header->sequence == before;
}

bool NormalishFrames::TryFill(BYTE* destination, UINT32 width, UINT32 height, const GUID& format)
{
    if (destination == nullptr) return false;
    if ((format != MFVideoFormat_RGB32) && (format != MFVideoFormat_NV12)) return false;
    if (!EnsureMapped()) return false;

    // A few quick attempts: the writer's window is about a millisecond, so a
    // retry usually lands between writes rather than losing the frame.
    bool fresh = false;
    for (int attempt = 0; (attempt < 3) && !fresh; attempt++)
    {
        fresh = Acquire(width, height);
        if (!fresh) Sleep(1);
    }

    if (fresh) _consecutiveMisses = 0;
    else
    {
        // Reuse the previous frame rather than showing a test pattern. Give up
        // only after about a second, by which point normalish has most likely
        // been closed and the pattern is the honest thing to show.
        if (_lastGood.empty()) return false;
        if (++_consecutiveMisses > 30) return false;
    }

    Convert(destination, width, height, format);
    return true;
}

void NormalishFrames::Convert(BYTE* destination, UINT32 width, UINT32 height, const GUID& format) const
{
    const BYTE* source = _lastGood.data();
    const size_t frameBytes = (size_t)width * height * 4;

    if (format == MFVideoFormat_RGB32)
    {
        // Already BGRA, and normalish writes rows top-down for us.
        memcpy(destination, source, frameBytes);
        return;
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
    }
}
