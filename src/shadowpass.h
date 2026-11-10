#ifndef SHADOWPASS_H
#define SHADOWPASS_H

#include "raylib.h"
#include "relightshader.h"

// Shadows as their own pass.
//
// The relight shader can march the shadow for every pixel it draws, and used
// to. That gave a shadow with ragged edges that changed every frame, at full
// resolution, once for the window and again for the virtual camera. Here the
// same march runs once into a small texture, which is then blurred and
// averaged over time, and the relight shader reads the result.
//
// Small is not a compromise: the depth map the march reads is 518 texels
// wide, so marching at 1920 only resolves its interpolation.

typedef struct ShadowPass {
    RenderTexture2D raw;          // the march, this frame
    RenderTexture2D history[2];   // filtered result, this frame and the last
    int current;                  // which of history[] holds the latest
    bool primed;                  // false until history holds something real
    Shader filter;
    int historyLoc, texelLoc, blendLoc;
} ShadowPass;

// sourceWidth/Height give the picture's shape; the pass picks its own size.
ShadowPass LoadShadowPass(int sourceWidth, int sourceHeight);
void UnloadShadowPass(ShadowPass *pass);

// Runs the pass for this frame and returns the texture to bind as shadowTex.
// Every relight uniform must already be set for the frame. seconds is the
// time since the last call, which sets how much of the old result is kept.
// Leaves the shader with shadowSource = 1; the caller restores viewMode.
Texture2D UpdateShadowPass(ShadowPass *pass, const RelightShader *rs, Texture2D albedo,
                           Texture2D depth, Texture2D mask, float seconds);

#endif
