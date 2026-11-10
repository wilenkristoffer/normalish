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
} RelightShader;

#endif
