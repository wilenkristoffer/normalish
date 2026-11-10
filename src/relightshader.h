#ifndef RELIGHTSHADER_H
#define RELIGHTSHADER_H

#include "raylib.h"

// The relight shader and the locations of its uniforms. Shared so the clip
// analysis can drive the same shader the app does, rather than a copy of it.
typedef struct RelightShader {
    Shader shader;
    int lightPosLoc;
    int lightColorLoc;
    int lightIntensityLoc;
    int falloffLoc;
    int ambientLoc;
    int specularStrengthLoc;
    int shininessLoc;
    int aspectLoc;
    int viewModeLoc;
    int texelSizeLoc;
    int reliefLoc;
    int sampleSpacingLoc;
    int geometryModeLoc;
    int depthMapLoc;
    int tanHalfFovXLoc;
    int depthSpacingLoc;
    int depthRangeLoc;
    int distanceScaleLoc;
    int depthTexelSizeLoc;
    int depthReliefLoc;
    int subjectMaskLoc;
    int maskEnabledLoc;
    int roomLightLoc;
    int shadowStepsLoc;
    int shadowBiasLoc;
    int shadowThicknessLoc;
    int shadowSourceLoc;
    int shadowTexLoc;
    int terminatorWrapLoc;
    int sourceLookLoc;
} RelightShader;

// How the light itself is drawn; the four values of sourceLook in lighting.fs.
typedef struct SourceLook {
    float size;     // radius, as a fraction of the scene's near distance
    float glow;     // brightness of the source and its glow, 0 = not drawn
    float spill;    // how much glow spills over the person's outline, 0..1
    float rim;      // strength of the rim of light on the person's edge
} SourceLook;

// Spill and rim were 0.6 and 1.0 at first; live, with the light behind the
// head, the top of the head glowed too much.
#define DEFAULT_SOURCE_LOOK ((SourceLook){ 0.06f, 1.0f, 0.4f, 0.6f })

// terminatorWrap for the live feed: how far past 90 degrees the light still
// reaches the subject. See the diffuse term in lighting.fs.
#define LIVE_TERMINATOR_WRAP 0.35f

#endif
