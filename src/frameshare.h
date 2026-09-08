#ifndef FRAMESHARE_H
#define FRAMESHARE_H

// The contract between normalish (producer) and the virtual camera media
// source (consumer). Included by both, so it stays plain C.
//
// Transport is a memory-mapped file rather than named shared memory. The media
// source is loaded by the Camera Frame Server, which runs as a service in
// session 0 under a different account, so a "Local\" name would be invisible
// to it and a "Global\" name needs SeCreateGlobalPrivilege that a normal user
// does not have. Two processes mapping the same file share pages with no
// namespace and no privilege - only file permissions, and ProgramData is
// writable by the user and readable by services.

#define NORMALISH_FRAME_PATH   "C:\\ProgramData\\normalish\\frame.bin"
#define NORMALISH_FRAME_PATH_W L"C:\\ProgramData\\normalish\\frame.bin"

#define NORMALISH_FRAME_MAGIC   0x48534C4Eu   /* 'NLSH' */
#define NORMALISH_FRAME_VERSION 1u

// Fixed capacity so the file size never changes; the header says how much of
// it is actually used.
#define NORMALISH_FRAME_MAX_WIDTH  1920u
#define NORMALISH_FRAME_MAX_HEIGHT 1080u
#define NORMALISH_FRAME_MAX_BYTES  (NORMALISH_FRAME_MAX_WIDTH*NORMALISH_FRAME_MAX_HEIGHT*4u)

// Pixels are BGRA, matching MFVideoFormat_RGB32's memory layout so the common
// case is a straight copy.
typedef struct NormalishFrameHeader {
    unsigned int magic;
    unsigned int version;
    unsigned int width;
    unsigned int height;
    // Seqlock: odd while a frame is being written, even when one is complete.
    // A reader samples it before and after copying and retries if it changed,
    // which avoids needing a cross-session event just to publish frames.
    volatile unsigned int sequence;
    unsigned int reserved;
} NormalishFrameHeader;

#define NORMALISH_FRAME_FILE_BYTES (sizeof(NormalishFrameHeader) + NORMALISH_FRAME_MAX_BYTES)

#endif
