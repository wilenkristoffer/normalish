#ifndef ANALYSE_H
#define ANALYSE_H

#include <stdbool.h>

#include "relightshader.h"

typedef struct AnalysisOptions {
    float lightU, lightV;   // light position in the image, 0..1
    float zFactor;          // light distance as a multiple of the scene's near
    int lag;                // frames the depth trails the picture by; 0 = none
    int extraDump[2];       // further frames to write pictures for, -1 = none
} AnalysisOptions;

// Replays a recorded clip through the depth model and the relight shader and
// measures what comes out: where shadows fall, how much the output flickers
// beyond what the camera itself does, and how stable the subject mask is.
//
// emptyPath is a clip of the same room with nobody in it, or "-". It gives a
// reference for where the person is that does not depend on the mask being
// measured. Writes <outPrefix>.csv plus a handful of PNGs. Returns 0 on success.
int RunClipAnalysis(const RelightShader *rs, const char *clipPath, const char *emptyPath,
                    const char *outPrefix, AnalysisOptions options);

#endif
