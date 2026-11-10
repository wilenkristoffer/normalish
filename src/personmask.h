#ifndef PERSONMASK_H
#define PERSONMASK_H

#include <stdbool.h>

// Which pixels are the person.
//
// Depth cannot answer that: a curtain or a chair at the right distance looks
// the same as a torso. Two small segmentation models and the depth map are
// combined instead, each covering for what the others get wrong:
//
//   parts  is sure about heads, skin and hands and does not take furniture
//          for a person, but barely marks dark clothing;
//   body   marks the whole torso whatever it is wearing, but misses a raised
//          hand and sometimes marks an empty chair or a bright window;
//   depth  knows nothing about people but does know how far away things are.
//
// The person is everything either model marks that is joined to something
// "parts" is sure of and is not far behind it. The result is then steadied
// over time, because a mask edge that wobbles by a pixel makes the lighting
// on it crawl.
//
// Used from the depth worker thread only, in two steps so the slow part can
// run without holding the lock the render thread waits on.

// Loads both models. False, with a reason in GetPersonMaskStatus, when either
// is missing; the caller then falls back to the depth-only subject mask.
bool InitPersonMask(void);
bool IsPersonMaskReady(void);
const char *GetPersonMaskStatus(void);
void ShutdownPersonMask(void);

// Step 1: run both models on a frame (RGBA, any size; a few hundred pixels
// wide is plenty). About 6 ms. False if either model failed.
bool RunPersonModels(const unsigned char *rgba, int width, int height);

// Step 2: combine what step 1 found with the depth of the same frame, and
// write the mask (0 = room, 255 = person, soft at the edge) at the depth
// map's size. Cheap.
void BuildPersonMask(const float *depth, int width, int height, unsigned char *maskOut);

// Milliseconds the two models took on the last frame, together.
float GetPersonMaskMs(void);

#endif
