#include "shadowpass.h"

#include <math.h>

// Width of the shadow texture. A little over the depth map's 518.
#define SHADOW_WIDTH 640

// How long the average over time reaches back, in seconds: after this long a
// change is about two thirds through. Long enough to cover two or three depth
// results, short enough that a shadow follows a moving head without trailing.
#define SHADOW_SETTLE_SECONDS 0.10f

ShadowPass LoadShadowPass(int sourceWidth, int sourceHeight)
{
    ShadowPass pass = { 0 };
    int w = SHADOW_WIDTH;
    int h = (sourceWidth > 0) ? SHADOW_WIDTH*sourceHeight/sourceWidth : SHADOW_WIDTH*9/16;
    if (h < 1) h = 1;

    pass.raw = LoadRenderTexture(w, h);
    SetTextureFilter(pass.raw.texture, TEXTURE_FILTER_BILINEAR);
    for (int i = 0; i < 2; i++)
    {
        pass.history[i] = LoadRenderTexture(w, h);
        SetTextureFilter(pass.history[i].texture, TEXTURE_FILTER_BILINEAR);
        SetTextureWrap(pass.history[i].texture, TEXTURE_WRAP_CLAMP);
    }
    SetTextureWrap(pass.raw.texture, TEXTURE_WRAP_CLAMP);

    pass.filter = LoadShader(0, SHADER_DIR "/shadowfilter.fs");
    pass.historyLoc = GetShaderLocation(pass.filter, "history");
    pass.texelLoc = GetShaderLocation(pass.filter, "texel");
    pass.blendLoc = GetShaderLocation(pass.filter, "blend");
    return pass;
}

void UnloadShadowPass(ShadowPass *pass)
{
    if (pass->raw.id != 0) UnloadRenderTexture(pass->raw);
    for (int i = 0; i < 2; i++)
        if (pass->history[i].id != 0) UnloadRenderTexture(pass->history[i]);
    if (pass->filter.id != 0) UnloadShader(pass->filter);
    *pass = (ShadowPass){ 0 };
}

Texture2D UpdateShadowPass(ShadowPass *pass, const RelightShader *rs, Texture2D albedo,
                           Texture2D depth, Texture2D mask, float seconds)
{
    int w = pass->raw.texture.width, h = pass->raw.texture.height;
    Rectangle whole = { 0.0f, 0.0f, (float)w, (float)h };

    // 1. The march, through the relight shader's own shadow-term view so
    //    there is one copy of that code.
    int shadowView = 5, marchHere = 0;
    SetShaderValue(rs->shader, rs->viewModeLoc, &shadowView, SHADER_UNIFORM_INT);
    SetShaderValue(rs->shader, rs->shadowSourceLoc, &marchHere, SHADER_UNIFORM_INT);

    BeginTextureMode(pass->raw);
    ClearBackground(WHITE);
    BeginShaderMode(rs->shader);
    SetShaderValueTexture(rs->shader, rs->depthMapLoc, depth);
    SetShaderValueTexture(rs->shader, rs->subjectMaskLoc, mask);
    DrawTexturePro(albedo, (Rectangle){ 0.0f, 0.0f, (float)albedo.width, (float)albedo.height },
                   whole, (Vector2){ 0.0f, 0.0f }, 0.0f, WHITE);
    EndShaderMode();
    EndTextureMode();

    // 2. Blur, and average with the previous result.
    float blend = 1.0f;
    if (pass->primed && (seconds > 0.0f)) blend = 1.0f - expf(-seconds/SHADOW_SETTLE_SECONDS);
    if (blend > 1.0f) blend = 1.0f;
    float texel[2] = { 1.0f/(float)w, 1.0f/(float)h };
    SetShaderValue(pass->filter, pass->texelLoc, texel, SHADER_UNIFORM_VEC2);
    SetShaderValue(pass->filter, pass->blendLoc, &blend, SHADER_UNIFORM_FLOAT);

    int next = 1 - pass->current;
    BeginTextureMode(pass->history[next]);
    ClearBackground(WHITE);
    BeginShaderMode(pass->filter);
    SetShaderValueTexture(pass->filter, pass->historyLoc, pass->history[pass->current].texture);
    DrawTexturePro(pass->raw.texture, whole, whole, (Vector2){ 0.0f, 0.0f }, 0.0f, WHITE);
    EndShaderMode();
    EndTextureMode();

    pass->current = next;
    pass->primed = true;

    int readTexture = 1;
    SetShaderValue(rs->shader, rs->shadowSourceLoc, &readTexture, SHADER_UNIFORM_INT);
    return pass->history[next].texture;
}
