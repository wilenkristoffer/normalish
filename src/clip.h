#ifndef CLIP_H
#define CLIP_H

#include <stdbool.h>

// Recorded webcam clips: raw frames on disk, so a change to the look can be
// judged against the same footage every time instead of against whatever the
// camera happens to be pointing at.
//
// The format is deliberately trivial - a header, then for each frame a
// timestamp and tightly packed RGB. No compression: a lossy codec would hand
// the depth model block artefacts that the live camera never produces, and the
// point of a recording is to stand in for the live camera.
//
//   char   magic[8]   "NCLIP01\0"
//   int32  width, height, frameCount
//   then frameCount times:
//     double seconds since the first frame
//     width*height*3 bytes, RGB, top row first
//
// Nothing here includes raylib; frames are plain byte buffers.

bool SaveClip(const char *path, int width, int height, int frameCount,
              const double *seconds, const unsigned char *rgb);

typedef struct Clip {
    void *file;
    int width;
    int height;
    int frameCount;
} Clip;

bool OpenClip(Clip *clip, const char *path);
void CloseClip(Clip *clip);

// Reads one frame as RGBA, which is what the rest of the pipeline consumes.
// 'rgba' must hold width*height*4 bytes. 'seconds' may be NULL.
bool ReadClipFrame(Clip *clip, int index, unsigned char *rgba, double *seconds);

#endif
