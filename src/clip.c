#include "clip.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static const char kMagic[8] = "NCLIP01";
#define HEADER_BYTES (8 + 3*sizeof(int32_t))

bool SaveClip(const char *path, int width, int height, int frameCount,
              const double *seconds, const unsigned char *rgb)
{
    FILE *f = NULL;
    if ((fopen_s(&f, path, "wb") != 0) || (f == NULL)) return false;

    int32_t header[3] = { width, height, frameCount };
    size_t frameBytes = (size_t)width*height*3;
    bool ok = (fwrite(kMagic, 1, 8, f) == 8) && (fwrite(header, sizeof(int32_t), 3, f) == 3);

    for (int i = 0; ok && (i < frameCount); i++)
    {
        ok = (fwrite(&seconds[i], sizeof(double), 1, f) == 1) &&
             (fwrite(rgb + (size_t)i*frameBytes, 1, frameBytes, f) == frameBytes);
    }

    fclose(f);
    return ok;
}

bool OpenClip(Clip *clip, const char *path)
{
    memset(clip, 0, sizeof(*clip));

    FILE *f = NULL;
    if ((fopen_s(&f, path, "rb") != 0) || (f == NULL)) return false;

    char magic[8];
    int32_t header[3];
    if ((fread(magic, 1, 8, f) != 8) || (memcmp(magic, kMagic, 8) != 0) ||
        (fread(header, sizeof(int32_t), 3, f) != 3) ||
        (header[0] <= 0) || (header[1] <= 0) || (header[2] <= 0))
    {
        fclose(f);
        return false;
    }

    clip->file = f;
    clip->width = header[0];
    clip->height = header[1];
    clip->frameCount = header[2];
    return true;
}

void CloseClip(Clip *clip)
{
    if (clip->file != NULL) fclose((FILE *)clip->file);
    memset(clip, 0, sizeof(*clip));
}

bool ReadClipFrame(Clip *clip, int index, unsigned char *rgba, double *seconds)
{
    if ((clip->file == NULL) || (index < 0) || (index >= clip->frameCount)) return false;

    FILE *f = (FILE *)clip->file;
    size_t pixels = (size_t)clip->width*clip->height;
    size_t frameBytes = pixels*3;

    // A clip runs to several GB, so the offset does not fit a 32-bit long.
    long long offset = (long long)HEADER_BYTES +
                       (long long)index*(long long)(sizeof(double) + frameBytes);
    if (_fseeki64(f, offset, SEEK_SET) != 0) return false;

    double t = 0.0;
    if (fread(&t, sizeof(double), 1, f) != 1) return false;
    if (seconds != NULL) *seconds = t;

    // Read the packed RGB into the tail of the caller's buffer and expand it
    // forwards in place. The write position never overtakes the read position,
    // so no second buffer is needed.
    unsigned char *packed = rgba + pixels;
    if (fread(packed, 1, frameBytes, f) != frameBytes) return false;

    for (size_t i = 0; i < pixels; i++)
    {
        unsigned char r = packed[i*3 + 0];
        unsigned char g = packed[i*3 + 1];
        unsigned char b = packed[i*3 + 2];
        rgba[i*4 + 0] = r;
        rgba[i*4 + 1] = g;
        rgba[i*4 + 2] = b;
        rgba[i*4 + 3] = 255;
    }
    return true;
}
