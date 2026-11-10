#include "personmask.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "segmodel.h"

static SegModel *bodyModel = NULL;
static SegModel *partsModel = NULL;
static bool ready = false;
static char statusText[256] = "not started";

// What the models last returned, at their own resolutions. The pointers
// belong to the models and stay valid until their next run.
static const unsigned char *bodyRaw = NULL, *partsRaw = NULL;
static int bodyW = 0, bodyH = 0, partsW = 0, partsH = 0;
static bool haveRaw = false;
static float lastMs = 0.0f;

// Working images at the depth map's size.
static int workW = 0, workH = 0;
static unsigned char *bodyMask = NULL;    // body model, resized
static unsigned char *partsMask = NULL;   // parts model, resized
static unsigned char *reached = NULL;     // flood fill state
static unsigned char *blurScratch = NULL;
static float *steady = NULL;              // the mask carried between frames, 0..1
static int *queue = NULL;
static bool haveSteady = false;
static int emptyRun = 0;                  // consecutive frames with nobody found

bool IsPersonMaskReady(void) { return ready; }
const char *GetPersonMaskStatus(void) { return statusText; }
float GetPersonMaskMs(void) { return lastMs; }

bool InitPersonMask(void)
{
    if (ready) return true;

    // The small one on the CPU, where it takes 2 ms and the GPU is no faster.
    // The larger one on the GPU: 3.5 ms there against 67 ms on the CPU.
    bodyModel = LoadSegModel(SEG_BODY_MODEL_PATH, false);
    partsModel = LoadSegModel(SEG_PARTS_MODEL_PATH, true);

    if (!SegModelReady(bodyModel))
        snprintf(statusText, sizeof(statusText), "body model: %s", GetSegModelStatus(bodyModel));
    else if (!SegModelReady(partsModel))
        snprintf(statusText, sizeof(statusText), "parts model: %s", GetSegModelStatus(partsModel));
    else
    {
        snprintf(statusText, sizeof(statusText), "ready");
        ready = true;
        return true;
    }

    UnloadSegModel(bodyModel); bodyModel = NULL;
    UnloadSegModel(partsModel); partsModel = NULL;
    return false;
}

void ShutdownPersonMask(void)
{
    ready = false;
    UnloadSegModel(bodyModel); bodyModel = NULL;
    UnloadSegModel(partsModel); partsModel = NULL;
    free(bodyMask); bodyMask = NULL;
    free(partsMask); partsMask = NULL;
    free(reached); reached = NULL;
    free(blurScratch); blurScratch = NULL;
    free(steady); steady = NULL;
    free(queue); queue = NULL;
    workW = 0; workH = 0;
    haveRaw = false; haveSteady = false; emptyRun = 0;
}

bool RunPersonModels(const unsigned char *rgba, int width, int height)
{
    haveRaw = false;
    if (!ready) return false;
    if (!RunSegModel(bodyModel, rgba, width, height, &bodyRaw, &bodyW, &bodyH)) return false;
    if (!RunSegModel(partsModel, rgba, width, height, &partsRaw, &partsW, &partsH)) return false;
    lastMs = GetSegInferenceMs(bodyModel) + GetSegInferenceMs(partsModel);
    haveRaw = true;
    return true;
}

// Bilinear resize of an 8-bit single-channel image.
static void Resize(const unsigned char *src, int sw, int sh, unsigned char *dst, int dw, int dh)
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

// A 3x3 box blur, as two passes. Takes the stair-steps off an edge that was
// decided at a third of the picture's resolution.
static void Soften(unsigned char *image, unsigned char *scratch, int w, int h)
{
    for (int y = 0; y < h; y++)
    {
        const unsigned char *row = image + (size_t)y*w;
        unsigned char *out = scratch + (size_t)y*w;
        for (int x = 0; x < w; x++)
        {
            int l = (x > 0) ? x - 1 : x, r = (x < w - 1) ? x + 1 : x;
            out[x] = (unsigned char)((row[l] + row[x] + row[r] + 1)/3);
        }
    }
    for (int y = 0; y < h; y++)
    {
        int u = (y > 0) ? y - 1 : y, d = (y < h - 1) ? y + 1 : y;
        const unsigned char *a = scratch + (size_t)u*w, *b = scratch + (size_t)y*w,
                            *c = scratch + (size_t)d*w;
        unsigned char *out = image + (size_t)y*w;
        for (int x = 0; x < w; x++) out[x] = (unsigned char)((a[x] + b[x] + c[x] + 1)/3);
    }
}

static bool EnsureWork(int w, int h)
{
    if ((w == workW) && (h == workH) && (bodyMask != NULL)) return true;
    size_t n = (size_t)w*h;
    free(bodyMask); free(partsMask); free(reached); free(blurScratch); free(steady); free(queue);
    bodyMask = (unsigned char *)malloc(n);
    partsMask = (unsigned char *)malloc(n);
    reached = (unsigned char *)malloc(n);
    blurScratch = (unsigned char *)malloc(n);
    steady = (float *)malloc(n*sizeof(float));
    queue = (int *)malloc(n*sizeof(int));
    workW = w; workH = h;
    haveSteady = false;
    emptyRun = 0;
    return (bodyMask != NULL) && (partsMask != NULL) && (reached != NULL) &&
           (blurScratch != NULL) && (steady != NULL) && (queue != NULL);
}

// This frame's mask on its own, before any steadying. Returns false when
// nobody was found, in which case maskOut is all zero.
static bool Combine(const float *depth, int w, int h, unsigned char *maskOut)
{
    size_t n = (size_t)w*h;
    const unsigned char sure = 160;    // parts: certainly a person
    const unsigned char maybe = 96;    // either model: could be
    const float behind = 1.4f;         // how much further than the core still counts

    // How far away the sure pixels are: their median, from a coarse histogram
    // over the range they span.
    float lo = 1.0e30f, hi = -1.0e30f;
    size_t sureCount = 0;
    for (size_t i = 0; i < n; i++)
    {
        if ((partsMask[i] < sure) || (depth[i] <= 0.0f)) continue;
        if (depth[i] < lo) lo = depth[i];
        if (depth[i] > hi) hi = depth[i];
        sureCount++;
    }
    // Too little to call a person: a few stray pixels must not light up the
    // room. 0.15% of the picture is about a hand at arm's length.
    if (sureCount < n*15/10000)
    {
        memset(maskOut, 0, n);
        return false;
    }
    int bins[64] = { 0 };
    float span = (hi > lo) ? (hi - lo) : 1.0f;
    for (size_t i = 0; i < n; i++)
    {
        if ((partsMask[i] < sure) || (depth[i] <= 0.0f)) continue;
        bins[(int)((depth[i] - lo)/span*63.0f)]++;
    }
    size_t seen = 0;
    float coreDepth = hi;
    for (int b = 0; b < 64; b++)
    {
        seen += (size_t)bins[b];
        if (seen*2 >= sureCount) { coreDepth = lo + ((float)b + 0.5f)/63.0f*span; break; }
    }
    float limit = coreDepth*behind;

    // Candidates are marked by either model and not too far back. Flood from
    // the sure pixels through the candidates; what is reached is the person.
    // reached: 0 = no, 1 = candidate, 2 = reached.
    int head = 0, tail = 0;
    for (size_t i = 0; i < n; i++)
    {
        unsigned char v = (bodyMask[i] > partsMask[i]) ? bodyMask[i] : partsMask[i];
        bool candidate = (v >= maybe) && (depth[i] <= limit);
        reached[i] = candidate ? 1 : 0;
        if (candidate && (partsMask[i] >= sure)) { reached[i] = 2; queue[tail++] = (int)i; }
    }
    while (head < tail)
    {
        int i = queue[head++];
        int x = i % w, y = i / w;
        if ((x > 0) && (reached[i - 1] == 1)) { reached[i - 1] = 2; queue[tail++] = i - 1; }
        if ((x < w - 1) && (reached[i + 1] == 1)) { reached[i + 1] = 2; queue[tail++] = i + 1; }
        if ((y > 0) && (reached[i - w] == 1)) { reached[i - w] = 2; queue[tail++] = i - w; }
        if ((y < h - 1) && (reached[i + w] == 1)) { reached[i + w] = 2; queue[tail++] = i + w; }
    }

    for (size_t i = 0; i < n; i++) maskOut[i] = (reached[i] == 2) ? 255 : 0;
    Soften(maskOut, blurScratch, w, h);
    return true;
}

void BuildPersonMask(const float *depth, int width, int height, unsigned char *maskOut)
{
    size_t n = (size_t)width*height;
    if (!ready || !haveRaw || !EnsureWork(width, height))
    {
        memset(maskOut, 0, n);
        return;
    }

    Resize(bodyRaw, bodyW, bodyH, bodyMask, width, height);
    Resize(partsRaw, partsW, partsH, partsMask, width, height);
    bool found = Combine(depth, width, height, maskOut);

    // Steadying, in two parts.
    //
    // A person does not vanish for one frame. If nobody is found but someone
    // was there a moment ago, keep the last mask for up to two results (about
    // a tenth of a second) rather than letting the whole relight blink off.
    if (!found && haveSteady && (emptyRun < 2))
    {
        emptyRun++;
        for (size_t i = 0; i < n; i++) maskOut[i] = (unsigned char)(steady[i]*255.0f + 0.5f);
        return;
    }
    emptyRun = found ? 0 : emptyRun + 1;

    // And the edge is averaged over time where it is only trembling, but
    // follows at once where it has really moved. A fixed average would trail
    // a moving arm with a lit ghost; no average lets a one-pixel wobble at the
    // silhouette crawl. The size of the change tells the two apart: a tremble
    // changes a pixel a little, a move changes it completely.
    if (!haveSteady)
    {
        for (size_t i = 0; i < n; i++) steady[i] = (float)maskOut[i]/255.0f;
        haveSteady = true;
    }
    else
    {
        for (size_t i = 0; i < n; i++)
        {
            float now = (float)maskOut[i]/255.0f;
            float change = now - steady[i];
            float size = (change < 0.0f) ? -change : change;
            float t = (size - 0.25f)/0.5f;
            t = (t < 0.0f) ? 0.0f : ((t > 1.0f) ? 1.0f : t);
            float alpha = 0.3f + 0.7f*t;
            steady[i] += change*alpha;
            maskOut[i] = (unsigned char)(steady[i]*255.0f + 0.5f);
        }
    }
    if (!found) haveSteady = false;
}
