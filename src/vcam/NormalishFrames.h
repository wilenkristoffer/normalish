#pragma once

// Reads relit frames published by normalish through the memory-mapped file
// described in ../frameshare.h. Falls back cleanly when normalish is not
// running, so the camera shows the test pattern rather than freezing.

struct NormalishFrames
{
    ~NormalishFrames();

    // Fills 'destination' for the given format. Returns false when no fresh
    // frame is available, in which case the caller should fall back.
    bool TryFill(BYTE* destination, UINT32 width, UINT32 height, const GUID& format);

private:
    bool EnsureMapped();
    void Unmap();
    bool Acquire(UINT32 width, UINT32 height);
    void Convert(BYTE* destination, UINT32 width, UINT32 height, const GUID& format) const;

    HANDLE _file = INVALID_HANDLE_VALUE;
    HANDLE _mapping = nullptr;
    BYTE* _view = nullptr;
    // The last frame read successfully. A momentary miss - the writer being
    // mid-copy - reuses this instead of falling back to the test pattern,
    // which is invisible where a colour card is not.
    std::vector<BYTE> _lastGood;
    UINT32 _consecutiveMisses = 0;
    ULONGLONG _lastAttempt = 0;
};
