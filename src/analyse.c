// Offline measurement of the relight on a recorded clip.
//
// Every frame of the clip goes through the real depth model and the real
// shader, in order, so the temporal smoothing and the mask behave as they do
// live. What comes out is measured rather than looked at:
//
//   - how much the shadow term darkens pixels that are not the person
//   - how much the output changes frame to frame beyond what the camera did
//   - how far the subject mask moves between frames, and how much of it lies
//     outside the person
//   - how far the model's depth scale wanders on a room that is not moving
//
// "The person" has to come from somewhere other than the mask under test, or
// the mask would be marking its own homework. It comes from differencing each
// frame against a clip of the same room with nobody in it.

#include "analyse.h"

#include "raylib.h"
#include "clip.h"
#include "depthmodel.h"
#include "segmodel.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Width the measurements are made at. Half of 1080p, so each measured pixel is
// an exact 2x2 average of the frame and sensor noise is already halved.
#define ANALYSIS_WIDTH 960

enum {
    COL_NEAR, COL_FAR, COL_INFER_MS,
    COL_ROOM_DEPTH, COL_SUBJ_DEPTH, COL_ROOM_DEPTH_JIT, COL_SUBJ_DEPTH_JIT,
    COL_MASK_AREA, COL_MASK_IOU, COL_EDGE_MOVE, COL_MASK_LEAK, COL_MASK_MISS, COL_SCORE,
    COL_SHADOW_ROOM, COL_SHADOW_ROOM_FRAC, COL_SHADOW_SUBJ,
    COL_FLICK_ROOM_OUT, COL_FLICK_ROOM_IN, COL_FLICK_SUBJ_OUT, COL_FLICK_SUBJ_IN,
    COL_SFLICK_ROOM, COL_SFLICK_SUBJ, COL_LIT_DELTA_ROOM,
    COL_ROOM_FRAC, COL_SUBJ_FRAC, COL_SEG_MS,
    COL_COUNT
};

static const char *kColumnNames[COL_COUNT] = {
    "near", "far", "inferMs",
    "roomDepth", "subjDepth", "roomDepthJitPct", "subjDepthJitPct",
    "maskAreaPct", "maskIoU", "edgeMovePx", "maskLeakPct", "maskMissPct", "score",
    "shadowRoom", "shadowRoomFracPct", "shadowSubj",
    "flickRoomOut", "flickRoomIn", "flickSubjOut", "flickSubjIn",
    "sFlickRoom", "sFlickSubj", "litDeltaRoom",
    "roomFracPct", "subjFracPct", "segMs"
};

// Bilinear resize of an 8-bit single-channel image.
static void ResizeMask(const unsigned char *src, int sw, int sh, unsigned char *dst, int dw, int dh)
{
    for (int y = 0; y < dh; y++)
    {
        float fy = ((float)y + 0.5f)*(float)sh/(float)dh - 0.5f;
        if (fy < 0.0f) fy = 0.0f;
        int y0 = (int)fy, y1 = (y0 + 1 < sh) ? y0 + 1 : sh - 1;
        float ty = fy - (float)y0;
        for (int x = 0; x < dw; x++)
        {
            float fx = ((float)x + 0.5f)*(float)sw/(float)dw - 0.5f;
            if (fx < 0.0f) fx = 0.0f;
            int x0 = (int)fx, x1 = (x0 + 1 < sw) ? x0 + 1 : sw - 1;
            float tx = fx - (float)x0;
            float top = src[(size_t)y0*sw + x0]*(1.0f - tx) + src[(size_t)y0*sw + x1]*tx;
            float bottom = src[(size_t)y1*sw + x0]*(1.0f - tx) + src[(size_t)y1*sw + x1]*tx;
            dst[(size_t)y*dw + x] = (unsigned char)(top*(1.0f - ty) + bottom*ty + 0.5f);
        }
    }
}

static int CompareFloats(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

// Two segmentation models and the depth map, each covering for what the
// others get wrong:
//   parts  is sure about heads, skin and hands and does not take furniture for
//          a person, but barely marks dark clothing;
//   body   marks the whole torso, but misses a raised hand and sometimes marks
//          an empty chair or a bright window;
//   depth  knows nothing about people but does know how far away things are.
// The person is everything either model marks that is joined to something
// "parts" is sure of and is not far behind it. Written into body, in place.
static void CombinePersonMasks(unsigned char *body, const unsigned char *parts,
                               const float *depth, int w, int h, int *queue)
{
    size_t n = (size_t)w*h;
    const unsigned char sure = 160, maybe = 96;
    const float behind = 1.4f;   // how much further than the core still counts

    // How far away the sure pixels are: their median, by a coarse histogram
    // over the depth range present.
    float lo = 1.0e30f, hi = -1.0e30f;
    size_t sureCount = 0;
    for (size_t i = 0; i < n; i++)
    {
        if (parts[i] < sure) continue;
        if (depth[i] < lo) lo = depth[i];
        if (depth[i] > hi) hi = depth[i];
        sureCount++;
    }
    // Too little to call a person: a few stray pixels must not light up the
    // room. 0.15% of the picture is a hand at arm's length.
    if (sureCount < n*15/10000)
    {
        memset(body, 0, n);
        return;
    }
    int bins[64] = { 0 };
    float span = (hi > lo) ? (hi - lo) : 1.0f;
    for (size_t i = 0; i < n; i++)
    {
        if (parts[i] < sure) continue;
        int b = (int)((depth[i] - lo)/span*63.0f);
        bins[b]++;
    }
    size_t seen = 0;
    float coreDepth = hi;
    for (int b = 0; b < 64; b++)
    {
        seen += (size_t)bins[b];
        if (seen*2 >= sureCount) { coreDepth = lo + ((float)b + 0.5f)/63.0f*span; break; }
    }
    float limit = coreDepth*behind;

    // Candidates: marked by either model and not too far back. Reuse body as
    // the working image: 0 = no, 1 = candidate, then 2 = reached.
    for (size_t i = 0; i < n; i++)
    {
        unsigned char v = (body[i] > parts[i]) ? body[i] : parts[i];
        body[i] = ((v >= maybe) && (depth[i] <= limit)) ? 1 : 0;
    }

    int head = 0, tail = 0;
    for (size_t i = 0; i < n; i++)
    {
        if ((parts[i] >= sure) && (body[i] == 1)) { body[i] = 2; queue[tail++] = (int)i; }
    }
    while (head < tail)
    {
        int i = queue[head++];
        int x = i % w, y = i / w;
        if ((x > 0) && (body[i - 1] == 1)) { body[i - 1] = 2; queue[tail++] = i - 1; }
        if ((x < w - 1) && (body[i + 1] == 1)) { body[i + 1] = 2; queue[tail++] = i + 1; }
        if ((y > 0) && (body[i - w] == 1)) { body[i - w] = 2; queue[tail++] = i - w; }
        if ((y < h - 1) && (body[i + w] == 1)) { body[i + w] = 2; queue[tail++] = i + w; }
    }
    for (size_t i = 0; i < n; i++) body[i] = (body[i] == 2) ? 255 : 0;
}

// Sorts in place. fraction 0.5 is the median.
static float Percentile(float *values, int count, float fraction)
{
    if (count <= 0) return 0.0f;
    qsort(values, (size_t)count, sizeof(float), CompareFloats);
    int index = (int)(fraction*(float)(count - 1) + 0.5f);
    return values[index];
}

// Box average of an RGBA frame down to aw x ah, as float RGB.
static void Downsample(const unsigned char *rgba, int w, int h, float *out, int aw, int ah)
{
    for (int ay = 0; ay < ah; ay++)
    {
        int y0 = ay*h/ah, y1 = (ay + 1)*h/ah;
        if (y1 <= y0) y1 = y0 + 1;
        for (int ax = 0; ax < aw; ax++)
        {
            int x0 = ax*w/aw, x1 = (ax + 1)*w/aw;
            if (x1 <= x0) x1 = x0 + 1;
            int sum[3] = { 0, 0, 0 };
            for (int y = y0; y < y1; y++)
            {
                const unsigned char *p = rgba + ((size_t)y*w + x0)*4;
                for (int x = x0; x < x1; x++, p += 4)
                {
                    sum[0] += p[0]; sum[1] += p[1]; sum[2] += p[2];
                }
            }
            float n = (float)((x1 - x0)*(y1 - y0));
            float *o = out + ((size_t)ay*aw + ax)*3;
            o[0] = sum[0]/n; o[1] = sum[1]/n; o[2] = sum[2]/n;
        }
    }
}

// Separable max (dilate) or min (erode) over a square of the given radius.
static void MinMaxFilter(unsigned char *image, unsigned char *scratch, int w, int h,
                         int radius, bool takeMax)
{
    for (int y = 0; y < h; y++)
    {
        for (int x = 0; x < w; x++)
        {
            unsigned char v = image[(size_t)y*w + x];
            int x0 = (x - radius < 0) ? 0 : x - radius;
            int x1 = (x + radius >= w) ? w - 1 : x + radius;
            for (int k = x0; k <= x1; k++)
            {
                unsigned char s = image[(size_t)y*w + k];
                if (takeMax ? (s > v) : (s < v)) v = s;
            }
            scratch[(size_t)y*w + x] = v;
        }
    }
    for (int y = 0; y < h; y++)
    {
        int y0 = (y - radius < 0) ? 0 : y - radius;
        int y1 = (y + radius >= h) ? h - 1 : y + radius;
        for (int x = 0; x < w; x++)
        {
            unsigned char v = scratch[(size_t)y*w + x];
            for (int k = y0; k <= y1; k++)
            {
                unsigned char s = scratch[(size_t)k*w + x];
                if (takeMax ? (s > v) : (s < v)) v = s;
            }
            image[(size_t)y*w + x] = v;
        }
    }
}

static void RenderView(const RelightShader *rs, RenderTexture2D target, Texture2D albedo,
                       Texture2D depth, Texture2D mask, int view, int steps)
{
    SetShaderValue(rs->shader, rs->viewModeLoc, &view, SHADER_UNIFORM_INT);
    SetShaderValue(rs->shader, rs->shadowStepsLoc, &steps, SHADER_UNIFORM_INT);

    Rectangle source = { 0.0f, 0.0f, (float)albedo.width, (float)albedo.height };
    Rectangle dest = { 0.0f, 0.0f, (float)target.texture.width, (float)target.texture.height };
    BeginTextureMode(target);
    ClearBackground(BLACK);
    BeginShaderMode(rs->shader);
    SetShaderValueTexture(rs->shader, rs->depthMapLoc, depth);
    SetShaderValueTexture(rs->shader, rs->subjectMaskLoc, mask);
    DrawTexturePro(albedo, source, dest, (Vector2){ 0.0f, 0.0f }, 0.0f, WHITE);
    EndShaderMode();
    EndTextureMode();
}

// Luminance of a render target, 0..255, top row first.
static void ReadLuma(RenderTexture2D target, float *out)
{
    Image image = LoadImageFromTexture(target.texture);
    int w = image.width, h = image.height;
    const unsigned char *data = (const unsigned char *)image.data;
    for (int y = 0; y < h; y++)
    {
        // Render textures are bottom-up.
        const unsigned char *row = data + (size_t)(h - 1 - y)*w*4;
        for (int x = 0; x < w; x++)
        {
            out[(size_t)y*w + x] = 0.2126f*row[x*4] + 0.7152f*row[x*4 + 1] + 0.0722f*row[x*4 + 2];
        }
    }
    UnloadImage(image);
}

static void ExportTarget(RenderTexture2D target, const char *path)
{
    Image image = LoadImageFromTexture(target.texture);
    ImageFlipVertical(&image);
    ExportImage(image, path);
    UnloadImage(image);
}

static void ExportScaled(const float *values, int w, int h, float scale, const char *path)
{
    unsigned char *grey = (unsigned char *)malloc((size_t)w*h);
    for (size_t i = 0; i < (size_t)w*h; i++)
    {
        float v = values[i]*scale;
        grey[i] = (unsigned char)((v < 0.0f) ? 0.0f : ((v > 255.0f) ? 255.0f : v));
    }
    Image image = { .data = grey, .width = w, .height = h, .mipmaps = 1,
                    .format = PIXELFORMAT_UNCOMPRESSED_GRAYSCALE };
    ExportImage(image, path);
    free(grey);
}

static bool WaitForDepth(const float **depth, const unsigned char **mask, int *w, int *h)
{
    for (int wait = 0; wait < 2500; wait++)
    {
        if (TakeLatestDepth(depth, mask, w, h)) return true;
        WaitTime(0.002);
    }
    return false;
}

int RunClipAnalysis(const RelightShader *rs, const char *clipPath, const char *emptyPath,
                    const char *outPrefix, AnalysisOptions options)
{
    SetTraceLogLevel(LOG_WARNING);

    Clip clip = { 0 };
    if (!OpenClip(&clip, clipPath)) { printf("ANALYSE: could not open %s\n", clipPath); return 1; }
    if (!InitDepthModel()) { printf("ANALYSE: %s\n", GetDepthModelStatus()); return 1; }
    bool useSeg = (options.segPath != NULL);
    bool useParts = useSeg && (options.partsPath != NULL);
    SegModel *segModel = NULL, *partsModel = NULL;
    if (useSeg)
    {
        segModel = LoadSegModel(options.segPath, options.segOnGpu);
        printf("ANALYSE: segmentation %s\n", GetSegModelStatus(segModel));
        if (!SegModelReady(segModel)) return 1;
    }
    if (useParts)
    {
        partsModel = LoadSegModel(options.partsPath, true);
        printf("ANALYSE: parts segmentation %s\n", GetSegModelStatus(partsModel));
        if (!SegModelReady(partsModel)) return 1;
    }
    unsigned char *segMask = NULL, *partsMask = NULL;
    int *fillQueue = NULL;

    int w = clip.width, h = clip.height, frames = clip.frameCount;
    int aw = ANALYSIS_WIDTH, ah = ANALYSIS_WIDTH*h/w;
    size_t an = (size_t)aw*ah;

    const char *directory = GetDirectoryPath(outPrefix);
    if ((directory[0] != 0) && !DirectoryExists(directory)) MakeDirectory(directory);

    unsigned char *rgba = (unsigned char *)malloc((size_t)w*h*4);
    unsigned char *rgbaDepth = (unsigned char *)malloc((size_t)w*h*4);
    float *small = (float *)malloc(an*3*sizeof(float));
    float *background = NULL;

    // The empty room, averaged over a spread of frames so its own sensor
    // noise does not end up in the reference.
    if ((emptyPath != NULL) && !TextIsEqual(emptyPath, "-"))
    {
        Clip empty = { 0 };
        if (OpenClip(&empty, emptyPath) && (empty.width == w) && (empty.height == h))
        {
            background = (float *)calloc(an*3, sizeof(float));
            int used = 0;
            for (int k = 0; k < 12; k++)
            {
                int index = (int)((k + 0.5f)*empty.frameCount/12.0f);
                if (!ReadClipFrame(&empty, index, rgba, NULL)) continue;
                Downsample(rgba, w, h, small, aw, ah);
                for (size_t i = 0; i < an*3; i++) background[i] += small[i];
                used++;
            }
            for (size_t i = 0; i < an*3; i++) background[i] /= (float)((used > 0) ? used : 1);
            CloseClip(&empty);
        }
        else printf("ANALYSE: could not use %s as the empty room\n", emptyPath);
    }

    float *yIn = (float *)malloc(an*sizeof(float));
    float *yOut = (float *)malloc(an*sizeof(float));
    float *yNoShadow = (float *)malloc(an*sizeof(float));
    float *shade = (float *)malloc(an*sizeof(float));
    float *prevIn = (float *)malloc(an*sizeof(float));
    float *prevOut = (float *)malloc(an*sizeof(float));
    float *prevShade = (float *)malloc(an*sizeof(float));
    // 0 = unsure, 1 = certainly room, 2 = certainly person.
    unsigned char *region = (unsigned char *)malloc(an);
    unsigned char *work = (unsigned char *)malloc(an);
    unsigned char *work2 = (unsigned char *)malloc(an);
    unsigned char *scratch = (unsigned char *)malloc(an);
    float *sortBuffer = (float *)malloc(an*sizeof(float));

    // Running sums for the per-pixel temporal standard deviation images.
    double *sumIn = (double *)calloc(an, sizeof(double)), *sumIn2 = (double *)calloc(an, sizeof(double));
    double *sumOut = (double *)calloc(an, sizeof(double)), *sumOut2 = (double *)calloc(an, sizeof(double));
    double *sumShade = (double *)calloc(an, sizeof(double)), *sumShade2 = (double *)calloc(an, sizeof(double));

    float *table = (float *)calloc((size_t)frames*COL_COUNT, sizeof(float));
    double *times = (double *)calloc((size_t)frames, sizeof(double));

    Image blank = GenImageColor(w, h, BLACK);
    Texture2D albedoTex = LoadTextureFromImage(blank);
    UnloadImage(blank);
    SetTextureFilter(albedoTex, TEXTURE_FILTER_BILINEAR);
    Texture2D depthTex = { 0 }, maskTex = { 0 };
    RenderTexture2D target = LoadRenderTexture(aw, ah);
    RenderTexture2D fullTarget = LoadRenderTexture(w, h);

    float *prevDepth = NULL;
    unsigned char *prevMask = NULL;
    int dw = 0, dh = 0;

    // The light and the shadow distances follow the scene's near plane the way
    // the app's main loop does: set once, and only refreshed when the near
    // plane moves by more than 1.6x.
    float knownNear = 0.0f, lightZ = 0.0f, thickness = 0.0f, bias = 0.0f;

    float aspect = (float)w/(float)h;
    float tanHalfFovX = tanf(70.0f*0.5f*PI/180.0f);
    float lightColor[3] = { 1.0f, 0.949f, 0.851f };
    float texel[2] = { 1.0f/(float)w, 1.0f/(float)h };
    int geometry = 1, maskOn = 1;
    float intensity = 2.2f, falloff = 1.0f/(0.7f*0.7f), ambient = 0.18f;
    float specular = 0.35f, shininess = 32.0f, relief = 0.02f, spacing = 1.0f, roomLight = 0.5f;

    SetShaderValue(rs->shader, rs->lightColorLoc, lightColor, SHADER_UNIFORM_VEC3);
    SetShaderValue(rs->shader, rs->lightIntensityLoc, &intensity, SHADER_UNIFORM_FLOAT);
    SetShaderValue(rs->shader, rs->falloffLoc, &falloff, SHADER_UNIFORM_FLOAT);
    SetShaderValue(rs->shader, rs->ambientLoc, &ambient, SHADER_UNIFORM_FLOAT);
    SetShaderValue(rs->shader, rs->specularStrengthLoc, &specular, SHADER_UNIFORM_FLOAT);
    SetShaderValue(rs->shader, rs->shininessLoc, &shininess, SHADER_UNIFORM_FLOAT);
    SetShaderValue(rs->shader, rs->aspectLoc, &aspect, SHADER_UNIFORM_FLOAT);
    SetShaderValue(rs->shader, rs->texelSizeLoc, texel, SHADER_UNIFORM_VEC2);
    SetShaderValue(rs->shader, rs->geometryModeLoc, &geometry, SHADER_UNIFORM_INT);
    SetShaderValue(rs->shader, rs->tanHalfFovXLoc, &tanHalfFovX, SHADER_UNIFORM_FLOAT);
    SetShaderValue(rs->shader, rs->depthSpacingLoc, &spacing, SHADER_UNIFORM_FLOAT);
    SetShaderValue(rs->shader, rs->depthReliefLoc, &relief, SHADER_UNIFORM_FLOAT);
    SetShaderValue(rs->shader, rs->maskEnabledLoc, &maskOn, SHADER_UNIFORM_INT);
    SetShaderValue(rs->shader, rs->roomLightLoc, &roomLight, SHADER_UNIFORM_FLOAT);

    int dumpAt[6] = { frames/6, frames/2, frames/2 + 1, frames*5/6,
                      options.extraDump[0], options.extraDump[1] };

    printf("ANALYSE: %s  %dx%d  %d frames  light u %.2f v %.2f z %.2fx near  lag %d\n",
           clipPath, w, h, frames, options.lightU, options.lightV, options.zFactor, options.lag);

    for (int f = 0; f < frames; f++)
    {
        float *row = table + (size_t)f*COL_COUNT;

        // --- depth, from this frame or an earlier one ---
        int depthFrame = f - options.lag;
        const float *depth = NULL;
        const unsigned char *mask = NULL;
        bool fresh = false;
        if ((depthFrame >= 0) || (f == 0))
        {
            if (depthFrame < 0) depthFrame = 0;
            ReadClipFrame(&clip, depthFrame, rgbaDepth, NULL);
            SubmitDepthFrame(rgbaDepth, w, h);
            int nw = 0, nh = 0;
            if (!WaitForDepth(&depth, &mask, &nw, &nh)) { printf("ANALYSE: no depth at frame %d\n", f); return 1; }
            fresh = true;
            if (useSeg)
            {
                // The person mask replaces the depth model's, resampled to the
                // depth map's size so everything downstream is unchanged.
                const unsigned char *raw = NULL;
                int sw = 0, sh = 0;
                if (!RunSegModel(segModel, rgbaDepth, w, h, &raw, &sw, &sh))
                {
                    printf("ANALYSE: segmentation failed: %s\n", GetSegModelStatus(segModel));
                    return 1;
                }
                if ((nw != dw) || (nh != dh))
                {
                    free(segMask); free(partsMask); free(fillQueue);
                    segMask = (unsigned char *)malloc((size_t)nw*nh);
                    partsMask = (unsigned char *)malloc((size_t)nw*nh);
                    fillQueue = (int *)malloc((size_t)nw*nh*sizeof(int));
                }
                ResizeMask(raw, sw, sh, segMask, nw, nh);
                mask = segMask;
                row[COL_SEG_MS] = GetSegInferenceMs(segModel);
                if (useParts)
                {
                    if (!RunSegModel(partsModel, rgbaDepth, w, h, &raw, &sw, &sh))
                    {
                        printf("ANALYSE: parts segmentation failed: %s\n", GetSegModelStatus(partsModel));
                        return 1;
                    }
                    ResizeMask(raw, sw, sh, partsMask, nw, nh);
                    row[COL_SEG_MS] += GetSegInferenceMs(partsModel);
                    CombinePersonMasks(segMask, partsMask, depth, nw, nh, fillQueue);
                }
            }
            if ((nw != dw) || (nh != dh))
            {
                dw = nw; dh = nh;
                Image di = { .data = (void *)depth, .width = dw, .height = dh, .mipmaps = 1,
                             .format = PIXELFORMAT_UNCOMPRESSED_R32 };
                Image mi = { .data = (void *)mask, .width = dw, .height = dh, .mipmaps = 1,
                             .format = PIXELFORMAT_UNCOMPRESSED_GRAYSCALE };
                depthTex = LoadTextureFromImage(di);
                maskTex = LoadTextureFromImage(mi);
                SetTextureFilter(depthTex, TEXTURE_FILTER_BILINEAR);
                SetTextureFilter(maskTex, TEXTURE_FILTER_BILINEAR);
                float depthTexel[2] = { 1.0f/(float)dw, 1.0f/(float)dh };
                SetShaderValue(rs->shader, rs->depthTexelSizeLoc, depthTexel, SHADER_UNIFORM_VEC2);
            }
            else
            {
                UpdateTexture(depthTex, depth);
                UpdateTexture(maskTex, mask);
            }
        }
        size_t dn = (size_t)dw*dh;

        ReadClipFrame(&clip, f, rgba, &times[f]);
        UpdateTexture(albedoTex, rgba);

        float sceneNear = 0.6f, sceneFar = 1.5f;
        GetDepthRange(&sceneNear, &sceneFar);
        if ((knownNear <= 0.0f) || (sceneNear > knownNear*1.6f) || (sceneNear < knownNear*0.625f))
        {
            knownNear = sceneNear;
            lightZ = sceneNear*options.zFactor;
            thickness = sceneNear*0.68f;
            bias = sceneNear*0.012f;
        }
        float lightPos[3] = {
            (options.lightU*2.0f - 1.0f)*tanHalfFovX*lightZ,
            (options.lightV*2.0f - 1.0f)*(tanHalfFovX/aspect)*lightZ,
            lightZ
        };
        float range[2] = { sceneNear, sceneFar };
        SetShaderValue(rs->shader, rs->lightPosLoc, lightPos, SHADER_UNIFORM_VEC3);
        SetShaderValue(rs->shader, rs->depthRangeLoc, range, SHADER_UNIFORM_VEC2);
        SetShaderValue(rs->shader, rs->distanceScaleLoc, &sceneNear, SHADER_UNIFORM_FLOAT);
        SetShaderValue(rs->shader, rs->shadowBiasLoc, &bias, SHADER_UNIFORM_FLOAT);
        SetShaderValue(rs->shader, rs->shadowThicknessLoc, &thickness, SHADER_UNIFORM_FLOAT);

        // --- the three measured renders ---
        RenderView(rs, target, albedoTex, depthTex, maskTex, 0, 24);
        ReadLuma(target, yOut);
        RenderView(rs, target, albedoTex, depthTex, maskTex, 0, 0);
        ReadLuma(target, yNoShadow);
        RenderView(rs, target, albedoTex, depthTex, maskTex, 5, 24);
        ReadLuma(target, shade);

        Downsample(rgba, w, h, small, aw, ah);
        for (size_t i = 0; i < an; i++)
            yIn[i] = 0.2126f*small[i*3] + 0.7152f*small[i*3 + 1] + 0.0722f*small[i*3 + 2];

        // --- where the person is, independent of the mask ---
        if (background != NULL)
        {
            for (size_t i = 0; i < an; i++)
            {
                float d = 0.0f;
                for (int c = 0; c < 3; c++)
                {
                    float e = fabsf(small[i*3 + c] - background[i*3 + c]);
                    if (e > d) d = e;
                }
                work[i] = (d > 28.0f) ? 255 : 0;
            }
            // Open, to drop isolated noise.
            MinMaxFilter(work, scratch, aw, ah, 2, false);
            MinMaxFilter(work, scratch, aw, ah, 2, true);

            // Certainly person: well inside what differs from the empty room.
            memcpy(work2, work, an);
            MinMaxFilter(work2, scratch, aw, ah, 6, false);

            // Certainly room: clear of the person by a margin. Dark clothing
            // against a dark chair does not difference, so everything below
            // the highest differing pixel in a column is treated as possibly
            // person - a seated or standing person runs down out of frame.
            for (int x = 0; x < aw; x++)
            {
                bool below = false;
                for (int y = 0; y < ah; y++)
                {
                    if (work[(size_t)y*aw + x]) below = true;
                    if (below) work[(size_t)y*aw + x] = 255;
                }
            }
            MinMaxFilter(work, scratch, aw, ah, 16, true);

            // Shoulders in dark clothing do not difference either, and they are
            // wider than the head the column fill started from. Below the
            // lowest certainly-person row (about the chin), widen sideways by
            // a shoulder's width so they are not counted as room.
            int chin = -1;
            for (int y = ah - 1; (y >= 0) && (chin < 0); y--)
                for (int x = 0; x < aw; x++)
                    if (work2[(size_t)y*aw + x]) { chin = y; break; }
            if (chin >= 0)
            {
                int reach = aw*3/16;
                for (int y = chin; y < ah; y++)
                {
                    unsigned char *line = work + (size_t)y*aw;
                    int first = -1, last = -1;
                    for (int x = 0; x < aw; x++)
                    {
                        // Only the body under the certainly-person pixels, not
                        // unrelated objects elsewhere in the row.
                        if (work2[(size_t)chin*aw + x] || work2[(size_t)((chin > 8) ? chin - 8 : 0)*aw + x])
                        {
                            if (first < 0) first = x;
                            last = x;
                        }
                    }
                    if (first < 0) break;
                    int x0 = (first - reach < 0) ? 0 : first - reach;
                    int x1 = (last + reach >= aw) ? aw - 1 : last + reach;
                    for (int x = x0; x <= x1; x++) line[x] = 255;
                }
            }

            for (size_t i = 0; i < an; i++) region[i] = work2[i] ? 2 : (work[i] ? 0 : 1);
        }
        else memset(region, 1, an);

        // --- image-space measurements ---
        double shadowRoom = 0.0, shadowSubj = 0.0, litDeltaRoom = 0.0;
        double flickRoomOut = 0.0, flickRoomIn = 0.0, flickSubjOut = 0.0, flickSubjIn = 0.0;
        double sFlickRoom = 0.0, sFlickSubj = 0.0;
        long long roomCount = 0, subjCount = 0, shadowedRoom = 0;

        for (size_t i = 0; i < an; i++)
        {
            float darkening = yNoShadow[i] - yOut[i];
            if (region[i] == 1)
            {
                roomCount++;
                shadowRoom += darkening;
                if (darkening > 2.0f) shadowedRoom++;
                litDeltaRoom += fabsf(yOut[i] - yIn[i]);
                if (f > 0)
                {
                    flickRoomOut += fabsf(yOut[i] - prevOut[i]);
                    flickRoomIn += fabsf(yIn[i] - prevIn[i]);
                    sFlickRoom += fabsf(shade[i] - prevShade[i]);
                }
            }
            else if (region[i] == 2)
            {
                subjCount++;
                shadowSubj += darkening;
                if (f > 0)
                {
                    flickSubjOut += fabsf(yOut[i] - prevOut[i]);
                    flickSubjIn += fabsf(yIn[i] - prevIn[i]);
                    sFlickSubj += fabsf(shade[i] - prevShade[i]);
                }
            }
            sumIn[i] += yIn[i]; sumIn2[i] += (double)yIn[i]*yIn[i];
            sumOut[i] += yOut[i]; sumOut2[i] += (double)yOut[i]*yOut[i];
            sumShade[i] += shade[i]; sumShade2[i] += (double)shade[i]*shade[i];
        }
        double rc = (roomCount > 0) ? (double)roomCount : 1.0;
        double sc = (subjCount > 0) ? (double)subjCount : 1.0;

        row[COL_NEAR] = sceneNear;
        row[COL_FAR] = sceneFar;
        row[COL_INFER_MS] = GetDepthInferenceMs();
        row[COL_SHADOW_ROOM] = (float)(shadowRoom/rc);
        row[COL_SHADOW_ROOM_FRAC] = (float)(100.0*(double)shadowedRoom/rc);
        row[COL_SHADOW_SUBJ] = (float)(shadowSubj/sc);
        row[COL_LIT_DELTA_ROOM] = (float)(litDeltaRoom/rc);
        row[COL_FLICK_ROOM_OUT] = (float)(flickRoomOut/rc);
        row[COL_FLICK_ROOM_IN] = (float)(flickRoomIn/rc);
        row[COL_FLICK_SUBJ_OUT] = (float)(flickSubjOut/sc);
        row[COL_FLICK_SUBJ_IN] = (float)(flickSubjIn/sc);
        row[COL_SFLICK_ROOM] = (float)(sFlickRoom/rc);
        row[COL_SFLICK_SUBJ] = (float)(sFlickSubj/sc);
        row[COL_ROOM_FRAC] = (float)(100.0*(double)roomCount/(double)an);
        row[COL_SUBJ_FRAC] = (float)(100.0*(double)subjCount/(double)an);

        // --- depth and mask measurements, at the depth map's resolution ---
        if (fresh)
        {
            if (prevDepth == NULL)
            {
                prevDepth = (float *)malloc(dn*sizeof(float));
                prevMask = (unsigned char *)malloc(dn);
                memcpy(prevDepth, depth, dn*sizeof(float));
                memcpy(prevMask, mask, dn);
            }

            int roomN = 0, subjN = 0;
            float *roomValues = sortBuffer;
            float *subjValues = sortBuffer + an/2;
            double roomJit = 0.0, subjJit = 0.0;
            long long roomJitN = 0, subjJitN = 0;
            long long maskN = 0, both = 0, either = 0, leak = 0, missed = 0, personN = 0, edge = 0;

            for (int y = 0; y < dh; y++)
            {
                int ay = (int)((y + 0.5f)*ah/dh);
                for (int x = 0; x < dw; x++)
                {
                    size_t i = (size_t)y*dw + x;
                    int ax = (int)((x + 0.5f)*aw/dw);
                    unsigned char where = region[(size_t)ay*aw + ax];
                    bool in = mask[i] > 127;
                    bool was = prevMask[i] > 127;

                    if (in) maskN++;
                    if (in && was) both++;
                    if (in || was) either++;
                    if (in && (where == 1)) leak++;
                    if (where == 2) { personN++; if (!in) missed++; }
                    if (in && (x > 0) && (y > 0) && (x < dw - 1) && (y < dh - 1) &&
                        ((mask[i - 1] <= 127) || (mask[i + 1] <= 127) ||
                         (mask[i - dw] <= 127) || (mask[i + dw] <= 127))) edge++;

                    float z = depth[i];
                    float jitter = (z > 0.0f) ? fabsf(z - prevDepth[i])/z : 0.0f;
                    if (where == 1)
                    {
                        roomJit += jitter; roomJitN++;
                        if (((x + y*3)%4 == 0) && (roomN < (int)(an/2))) roomValues[roomN++] = z;
                    }
                    else if (where == 2)
                    {
                        subjJit += jitter; subjJitN++;
                        if (subjN < (int)(an/2)) subjValues[subjN++] = z;
                    }
                }
            }

            row[COL_ROOM_DEPTH] = Percentile(roomValues, roomN, 0.5f);
            row[COL_SUBJ_DEPTH] = Percentile(subjValues, subjN, 0.5f);
            row[COL_ROOM_DEPTH_JIT] = (float)(100.0*roomJit/(double)((roomJitN > 0) ? roomJitN : 1));
            row[COL_SUBJ_DEPTH_JIT] = (float)(100.0*subjJit/(double)((subjJitN > 0) ? subjJitN : 1));
            row[COL_MASK_AREA] = (float)(100.0*(double)maskN/(double)dn);
            row[COL_MASK_IOU] = (either > 0) ? (float)((double)both/(double)either) : 1.0f;
            // Pixels that changed sides, per pixel of outline: how far the
            // edge moved on average, in depth-map pixels.
            row[COL_EDGE_MOVE] = (edge > 0) ? (float)((double)(either - both)/(double)edge) : 0.0f;
            row[COL_MASK_LEAK] = (maskN > 0) ? (float)(100.0*(double)leak/(double)maskN) : 0.0f;
            row[COL_MASK_MISS] = (personN > 0) ? (float)(100.0*(double)missed/(double)personN) : 0.0f;
            row[COL_SCORE] = (float)GetSubjectScore();

            memcpy(prevDepth, depth, dn*sizeof(float));
            memcpy(prevMask, mask, dn);
        }
        else if (f > 0)
        {
            for (int c = COL_ROOM_DEPTH; c <= COL_SCORE; c++) row[c] = row[c - COL_COUNT];
        }

        // --- pictures, for a few frames ---
        bool dump = false;
        for (int k = 0; k < 6; k++) if (dumpAt[k] == f) dump = true;
        if (dump)
        {
            RenderView(rs, fullTarget, albedoTex, depthTex, maskTex, 0, 24);
            ExportTarget(fullTarget, TextFormat("%s-f%03d-lit.png", outPrefix, f));
            RenderView(rs, target, albedoTex, depthTex, maskTex, 4, 24);
            ExportTarget(target, TextFormat("%s-f%03d-mask.png", outPrefix, f));
            RenderView(rs, target, albedoTex, depthTex, maskTex, 5, 24);
            ExportTarget(target, TextFormat("%s-f%03d-shadow.png", outPrefix, f));
            RenderView(rs, target, albedoTex, depthTex, maskTex, 1, 24);
            ExportTarget(target, TextFormat("%s-f%03d-normal.png", outPrefix, f));
            RenderView(rs, target, albedoTex, depthTex, maskTex, 3, 24);
            ExportTarget(target, TextFormat("%s-f%03d-depth.png", outPrefix, f));

            for (size_t i = 0; i < an; i++) work[i] = (unsigned char)(region[i]*127);
            Image regionImage = { .data = work, .width = aw, .height = ah, .mipmaps = 1,
                                  .format = PIXELFORMAT_UNCOMPRESSED_GRAYSCALE };
            ExportImage(regionImage, TextFormat("%s-f%03d-region.png", outPrefix, f));
        }

        memcpy(prevIn, yIn, an*sizeof(float));
        memcpy(prevOut, yOut, an*sizeof(float));
        memcpy(prevShade, shade, an*sizeof(float));
    }

    // --- per-pixel temporal standard deviation ---
    float *stdIn = yIn, *stdOut = yOut, *stdShade = shade, *excess = yNoShadow;
    for (size_t i = 0; i < an; i++)
    {
        double n = (double)frames;
        double vIn = sumIn2[i]/n - (sumIn[i]/n)*(sumIn[i]/n);
        double vOut = sumOut2[i]/n - (sumOut[i]/n)*(sumOut[i]/n);
        double vShade = sumShade2[i]/n - (sumShade[i]/n)*(sumShade[i]/n);
        stdIn[i] = (float)sqrt((vIn > 0.0) ? vIn : 0.0);
        stdOut[i] = (float)sqrt((vOut > 0.0) ? vOut : 0.0);
        stdShade[i] = (float)sqrt((vShade > 0.0) ? vShade : 0.0);
        excess[i] = stdOut[i] - stdIn[i];
    }
    // x10: a standard deviation of 25 luminance levels shows as white.
    ExportScaled(stdIn, aw, ah, 10.0f, TextFormat("%s-std-in.png", outPrefix));
    ExportScaled(stdOut, aw, ah, 10.0f, TextFormat("%s-std-out.png", outPrefix));
    ExportScaled(excess, aw, ah, 10.0f, TextFormat("%s-std-excess.png", outPrefix));
    ExportScaled(stdShade, aw, ah, 4.0f, TextFormat("%s-std-shadow.png", outPrefix));

    // --- CSV and summary ---
    FILE *csv = NULL;
    if ((fopen_s(&csv, TextFormat("%s.csv", outPrefix), "w") == 0) && (csv != NULL))
    {
        fprintf(csv, "frame,seconds");
        for (int c = 0; c < COL_COUNT; c++) fprintf(csv, ",%s", kColumnNames[c]);
        fprintf(csv, "\n");
        for (int f = 0; f < frames; f++)
        {
            fprintf(csv, "%d,%.4f", f, times[f]);
            for (int c = 0; c < COL_COUNT; c++) fprintf(csv, ",%.4f", table[(size_t)f*COL_COUNT + c]);
            fprintf(csv, "\n");
        }
        fclose(csv);
    }

    printf("  %-18s %10s %10s %10s %10s\n", "column", "mean", "median", "p95", "max");
    float *column = (float *)malloc((size_t)frames*sizeof(float));
    for (int c = 0; c < COL_COUNT; c++)
    {
        // Frame 0 has no predecessor, so the frame-to-frame columns start at 1.
        int count = 0;
        double sum = 0.0;
        for (int f = 1; f < frames; f++)
        {
            column[count] = table[(size_t)f*COL_COUNT + c];
            sum += column[count];
            count++;
        }
        float median = Percentile(column, count, 0.5f);
        float p95 = column[(int)(0.95f*(float)(count - 1) + 0.5f)];
        printf("  %-18s %10.3f %10.3f %10.3f %10.3f\n", kColumnNames[c],
               sum/(double)((count > 0) ? count : 1), median, p95, column[count - 1]);
    }
    free(column);
    fflush(stdout);

    UnloadRenderTexture(target);
    UnloadRenderTexture(fullTarget);
    UnloadTexture(albedoTex);
    if (depthTex.id != 0) UnloadTexture(depthTex);
    if (maskTex.id != 0) UnloadTexture(maskTex);
    CloseClip(&clip);
    ShutdownDepthModel();
    UnloadSegModel(segModel);
    UnloadSegModel(partsModel);
    free(segMask); free(partsMask); free(fillQueue);

    free(rgba); free(rgbaDepth); free(small); free(background);
    free(yIn); free(yOut); free(yNoShadow); free(shade);
    free(prevIn); free(prevOut); free(prevShade);
    free(region); free(work); free(work2); free(scratch); free(sortBuffer);
    free(sumIn); free(sumIn2); free(sumOut); free(sumOut2); free(sumShade); free(sumShade2);
    free(table); free(times); free(prevDepth); free(prevMask);
    return 0;
}
