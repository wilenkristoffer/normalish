#include "depthmodel.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "onnxruntime_c_api.h"
#include "dml_provider_factory.h"

// ImageNet normalisation, which is what this model was trained with.
static const float kMean[3] = { 0.485f, 0.456f, 0.406f };
static const float kStd[3]  = { 0.229f, 0.224f, 0.225f };

#define SIDE DEPTH_MODEL_SIDE

static const OrtApi *ort = NULL;
static OrtEnv *env = NULL;
static OrtSessionOptions *options = NULL;
static OrtSession *session = NULL;
static OrtMemoryInfo *memoryInfo = NULL;
static char *inputName = NULL;
static char *outputName = NULL;

static char statusText[256] = "not started";
static volatile bool ready = false;

// The worker owns inference. The main thread only ever touches the submission
// slot and the published result, both under the lock.
static HANDLE workerThread = NULL;
static HANDLE frameReady = NULL;
static volatile bool quitting = false;
static CRITICAL_SECTION lock;

// Submission slot. One deep on purpose: a queue would only add latency, and a
// dropped frame costs nothing when the subject is a head.
static float *pendingInput = NULL;     // 3*SIDE*SIDE, already normalised
static int pendingCropX = 0, pendingCropY = 0, pendingCropW = 0, pendingCropH = 0;
static bool pendingFull = false;

// Published result, cropped to the letterbox region.
static float *resultDepth = NULL;
static float *smoothedDepth = NULL;
static unsigned char *maskBuffer = NULL;
static unsigned char *maskScratch = NULL;
static int *growStack = NULL;
static int *labelBuffer = NULL;
// Copies handed to the caller, so the worker can carry on without the render
// thread reading a buffer part way through being rewritten.
static float *publishDepth = NULL;
static unsigned char *publishMask = NULL;
static int resultWidth = 0, resultHeight = 0;
static bool resultFresh = false;
static float resultNear = 0.0f, resultFar = 0.0f;
// How deep a slice counts as the subject. A person plus the chair they are
// sitting in is well under a metre; beyond that the growth starts reaching
// the room behind them.
static float subjectBand = 0.9f;
// Off in normal use: this prints per-region scores, which is only wanted when
// working out why a particular frame chose the region it did.
static bool verboseMask = false;
static double lastWinningScore = 0.0;
static volatile LONG lastInferenceMicros = 0;

static void SetStatus(const char *text)
{
    strncpy(statusText, text, sizeof(statusText) - 1);
    statusText[sizeof(statusText) - 1] = 0;
}

bool IsDepthModelReady(void) { return ready; }
const char *GetDepthModelStatus(void) { return statusText; }
float GetDepthInferenceMs(void) { return (float)lastInferenceMicros/1000.0f; }

static bool Failed(OrtStatus *status, const char *what)
{
    if (status == NULL) return false;
    char buffer[256];
    snprintf(buffer, sizeof(buffer), "%s: %s", what, ort->GetErrorMessage(status));
    SetStatus(buffer);
    ort->ReleaseStatus(status);
    return true;
}


// Which pixels belong to the subject.
//
// A plain depth cutoff does not work: a curtain beside the camera is as near
// as the person sitting in front of it. Neither does growing from a single
// seed - the first attempt took the nearest pixel in the middle of the frame
// and picked the curtain, because a curtain edge reaches into the centre and
// is nearer than someone sitting off to one side.
//
// Depth continuity alone is not enough either. The second attempt labelled
// depth-continuous regions and the winning region swallowed the whole room:
// a person is depth-connected to their chair, which is connected to the
// floor, which runs smoothly to the far wall, so nothing ever breaks the
// chain. Adjacent pixels on a receding surface differ by a millimetre or two,
// well inside any sensible continuity tolerance.
//
// What works is continuity *plus* a depth band, measured from each region's
// own nearest point. A single global band anchored on the nearest pixel in
// the frame does not work either: the third attempt did that, and since the
// nearest thing was a curtain beside the camera, the band ran from the
// curtain and excluded the person standing further back entirely.
//
// Per region, the band stops growth crawling off to the wall, and anything
// else near the camera becomes its own region rather than joining the
// subject.
//
// Regions are then scored on being near, a reasonable share of the frame,
// running out of the bottom of frame, roughly central, and not a slab
// covering the full height at one side. That last test rejects curtains.
//
// This is a nearest-central-object selector, not a person detector. On a
// frame with nobody in it, the office chair in the subject position is what
// gets picked: its headrest and back read as a head and shoulders in depth,
// and it is near, central, large and running out of the bottom of frame -
// every feature the scoring rewards.
//
// That is accepted rather than worked around. Lighting the chair someone just
// left is benign and roughly where the light belongs anyway, and telling a
// chair from a person needs an actual person or face model, not a better
// heuristic over depth.
//
// Runs at the depth map's own resolution, about 150k pixels, so it is cheap
// enough to redo on every inference.

typedef struct Region {
    int area;
    double sumDepth;
    double sumX;
    int leftEdge;      // pixels in the first column
    int rightEdge;     // pixels in the last column
    bool touchesBottom;
} Region;

#define MAX_REGIONS 512

static void ComputeSubjectMask(int w, int h, float bandMetres)
{
    size_t count = (size_t)w*h;

    if (maskBuffer == NULL) maskBuffer = (unsigned char *)malloc(count);
    if (maskScratch == NULL) maskScratch = (unsigned char *)malloc(count);
    if (growStack == NULL) growStack = (int *)malloc(count*sizeof(int));
    if (labelBuffer == NULL) labelBuffer = (int *)malloc(count*sizeof(int));
    if ((maskBuffer == NULL) || (maskScratch == NULL) ||
        (growStack == NULL) || (labelBuffer == NULL)) return;

    for (size_t i = 0; i < count; i++) labelBuffer[i] = -1;

    Region regions[MAX_REGIONS];
    int regionCount = 0;

    const int dx[4] = { -1, 1, 0, 0 };
    const int dy[4] = { 0, 0, -1, 1 };

    for (size_t start = 0; start < count; start++)
    {
        if (labelBuffer[start] != -1) continue;
        if (smoothedDepth[start] <= 0.0f) { labelBuffer[start] = -2; continue; }
        if (regionCount >= MAX_REGIONS) break;

        int label = regionCount++;
        Region *r = &regions[label];
        memset(r, 0, sizeof(*r));

        int top = 0;
        growStack[top++] = (int)start;
        labelBuffer[start] = label;

        // The band is measured from the nearest point found so far in this
        // region. It only ever loosens as growth finds nearer pixels, and
        // because the continuity rule keeps neighbours within centimetres the
        // frontier depth moves slowly enough for that to be stable.
        float regionMin = smoothedDepth[start];

        while (top > 0)
        {
            int index = growStack[--top];
            int cx = index%w;
            int cy = index/w;
            float here = smoothedDepth[index];

            if (here < regionMin) regionMin = here;

            r->area++;
            r->sumDepth += here;
            r->sumX += cx;
            if (cx == 0) r->leftEdge++;
            if (cx == w - 1) r->rightEdge++;
            if (cy == h - 1) r->touchesBottom = true;

            // Tolerance grows with distance: the model is proportionally less
            // precise further away, so a fixed figure would either fragment
            // the far side of the subject or leak into the room on the near
            // side.
            float tolerance = 0.02f*here + 0.01f;

            for (int k = 0; k < 4; k++)
            {
                int nx = cx + dx[k];
                int ny = cy + dy[k];
                if ((nx < 0) || (ny < 0) || (nx >= w) || (ny >= h)) continue;

                size_t n = (size_t)ny*w + nx;
                if (labelBuffer[n] != -1) continue;

                float there = smoothedDepth[n];
                if (there <= 0.0f) { labelBuffer[n] = -2; continue; }
                // Left unlabelled rather than marked dead: a pixel beyond this
                // region's band belongs to something else, and has to stay
                // available to seed its own region.
                if (there > regionMin + bandMetres) continue;
                if (fabsf(there - here) > tolerance) continue;

                labelBuffer[n] = label;
                growStack[top++] = (int)n;
            }
        }
    }

    int best = -1;
    double bestScore = 0.0;

    for (int i = 0; i < regionCount; i++)
    {
        Region *r = &regions[i];
        // Ignore specks. A person can be a modest share of the frame once the
        // background has been gated away, so this is deliberately low.
        if (r->area < (int)(count/200)) continue;

        double meanDepth = r->sumDepth/(double)r->area;
        if (meanDepth <= 0.0) continue;

        // Nearer is better, and the near end of the scene sets the reference
        // so this does not depend on the room's absolute size.
        double proximity = (resultNear > 0.0f) ? ((double)resultNear/meanDepth) : 1.0;

        // Saturating, so a region being enormous is not itself a virtue -
        // that is how a wall would win.
        double areaFactor = (double)r->area/(0.03*(double)count);
        if (areaFactor > 1.0) areaFactor = 1.0;

        // A seated person runs out of the bottom of frame; a lamp does not.
        double bottom = r->touchesBottom ? 1.0 : 0.35;

        // A slab covering most of the frame height at one side is a curtain,
        // a wall or a door frame. This is the test that rejects them.
        int sideRun = (r->leftEdge > r->rightEdge) ? r->leftEdge : r->rightEdge;
        double side = (sideRun > (int)(0.8*h)) ? 0.2 : 1.0;

        // Mild preference for the middle, since the subject is roughly framed.
        double centreOffset = fabs((r->sumX/(double)r->area)/(double)w - 0.5)*2.0;
        double centre = 1.0 - 0.45*centreOffset;

        double score = proximity*areaFactor*bottom*side*centre;

        if (verboseMask)
        {
            printf("    region %2d: area %5.1f%%  depth %5.2f m  bottom %s  "
                   "side %4d  centreX %.2f  -> score %.3f\n",
                   i, 100.0*(double)r->area/(double)count, meanDepth,
                   r->touchesBottom ? "yes" : "no ", sideRun,
                   (r->sumX/(double)r->area)/(double)w, score);
        }

        if (score > bestScore) { bestScore = score; best = i; }
    }

    lastWinningScore = bestScore;
    if (verboseMask)
        printf("    winner    : region %d, score %.3f\n", best, bestScore);

    memset(maskBuffer, 0, count);
    if (best < 0) return;
    for (size_t i = 0; i < count; i++)
        if (labelBuffer[i] == best) maskBuffer[i] = 255;

    // Feather the edge. A hard boundary between relit and original pixels
    // reads as a cut-out; two separable box passes here become a soft band
    // once the mask is upsampled to the frame.
    const int radius = 3;
    for (int pass = 0; pass < 2; pass++)
    {
        for (int y = 0; y < h; y++)
        {
            for (int x = 0; x < w; x++)
            {
                int sum = 0, n = 0;
                for (int k = -radius; k <= radius; k++)
                {
                    int sx = x + k;
                    if ((sx < 0) || (sx >= w)) continue;
                    sum += maskBuffer[(size_t)y*w + sx];
                    n++;
                }
                maskScratch[(size_t)y*w + x] = (unsigned char)(sum/n);
            }
        }
        for (int x = 0; x < w; x++)
        {
            for (int y = 0; y < h; y++)
            {
                int sum = 0, n = 0;
                for (int k = -radius; k <= radius; k++)
                {
                    int sy = y + k;
                    if ((sy < 0) || (sy >= h)) continue;
                    sum += maskScratch[(size_t)sy*w + x];
                    n++;
                }
                maskBuffer[(size_t)y*w + x] = (unsigned char)(sum/n);
            }
        }
    }
}

// One inference. Runs only on the worker thread.
static void RunInference(const float *input, int cropX, int cropY, int cropW, int cropH)
{
    int64_t shape[4] = { 1, 3, SIDE, SIDE };
    OrtValue *inputTensor = NULL;
    if (Failed(ort->CreateTensorWithDataAsOrtValue(memoryInfo, (void *)input,
                                                   (size_t)3*SIDE*SIDE*sizeof(float),
                                                   shape, 4,
                                                   ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,
                                                   &inputTensor), "tensor")) return;

    const char *inNames[1] = { inputName };
    const char *outNames[1] = { outputName };
    OrtValue *outputTensor = NULL;

    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);

    if (Failed(ort->Run(session, NULL, inNames, (const OrtValue *const *)&inputTensor, 1,
                        outNames, 1, &outputTensor), "run"))
    {
        ort->ReleaseValue(inputTensor);
        return;
    }

    QueryPerformanceCounter(&t1);
    InterlockedExchange(&lastInferenceMicros,
                        (LONG)((t1.QuadPart - t0.QuadPart)*1000000/freq.QuadPart));

    float *raw = NULL;
    if (!Failed(ort->GetTensorMutableData(outputTensor, (void **)&raw), "data"))
    {
        EnterCriticalSection(&lock);

        if ((resultWidth != cropW) || (resultHeight != cropH))
        {
            free(resultDepth);
            free(smoothedDepth);
            free(maskBuffer); maskBuffer = NULL;
            free(maskScratch); maskScratch = NULL;
            free(growStack); growStack = NULL;
    free(labelBuffer); labelBuffer = NULL;
            free(labelBuffer); labelBuffer = NULL;
            free(publishDepth); publishDepth = NULL;
            free(publishMask); publishMask = NULL;
            resultDepth = (float *)malloc((size_t)cropW*cropH*sizeof(float));
            smoothedDepth = NULL;      // a resize invalidates the history
            resultWidth = cropW;
            resultHeight = cropH;
        }

        if (resultDepth != NULL)
        {
            // The output is the same square as the input, so lift out only the
            // region the frame actually occupied. Its texture coordinates then
            // match the albedo's without any further mapping.
            for (int y = 0; y < cropH; y++)
            {
                memcpy(resultDepth + (size_t)y*cropW,
                       raw + (size_t)(y + cropY)*SIDE + cropX,
                       (size_t)cropW*sizeof(float));
            }

            // Exponential smoothing. Per-frame flicker is far more obvious in
            // a relight than in a depth visualisation, because a wobbling
            // normal makes the whole surface shimmer. The cost is some
            // ghosting on fast movement, which is the better trade here.
            if (smoothedDepth == NULL)
            {
                smoothedDepth = (float *)malloc((size_t)cropW*cropH*sizeof(float));
                if (smoothedDepth != NULL)
                    memcpy(smoothedDepth, resultDepth, (size_t)cropW*cropH*sizeof(float));
            }
            else
            {
                const float alpha = 0.35f;
                size_t count = (size_t)cropW*cropH;
                for (size_t i = 0; i < count; i++)
                    smoothedDepth[i] += (resultDepth[i] - smoothedDepth[i])*alpha;
            }

            // Robust range, from a histogram rather than min and max: a
            // single speculative pixel at 20 m would otherwise set the scale
            // for the light distance slider and the depth view.
            size_t count = (size_t)cropW*cropH;
            float lo = 1.0e30f, hi = -1.0e30f;
            for (size_t i = 0; i < count; i++)
            {
                float z = smoothedDepth[i];
                if (z < lo) lo = z;
                if (z > hi) hi = z;
            }
            if (hi > lo)
            {
                enum { BINS = 256 };
                int histogram[BINS] = { 0 };
                float span = hi - lo;
                for (size_t i = 0; i < count; i++)
                {
                    int b = (int)((smoothedDepth[i] - lo)/span*(BINS - 1));
                    if (b < 0) b = 0;
                    if (b >= BINS) b = BINS - 1;
                    histogram[b]++;
                }
                size_t lowCut = count/50;            // 2nd percentile
                size_t highCut = count - count/50;   // 98th
                size_t running = 0;
                int lowBin = 0, highBin = BINS - 1;
                for (int b = 0; b < BINS; b++)
                {
                    running += (size_t)histogram[b];
                    if (running >= lowCut) { lowBin = b; break; }
                }
                running = 0;
                for (int b = 0; b < BINS; b++)
                {
                    running += (size_t)histogram[b];
                    if (running >= highCut) { highBin = b; break; }
                }
                resultNear = lo + span*(float)lowBin/(float)(BINS - 1);
                resultFar = lo + span*(float)highBin/(float)(BINS - 1);
            }

            ComputeSubjectMask(cropW, cropH, subjectBand);

            resultFresh = true;
        }

        LeaveCriticalSection(&lock);
    }

    ort->ReleaseValue(outputTensor);
    ort->ReleaseValue(inputTensor);
}

static DWORD WINAPI WorkerMain(LPVOID param)
{
    (void)param;

    float *local = (float *)malloc((size_t)3*SIDE*SIDE*sizeof(float));
    if (local == NULL) return 0;

    while (!quitting)
    {
        WaitForSingleObject(frameReady, INFINITE);
        if (quitting) break;

        int cx, cy, cw, ch;
        bool have = false;

        EnterCriticalSection(&lock);
        if (pendingFull)
        {
            memcpy(local, pendingInput, (size_t)3*SIDE*SIDE*sizeof(float));
            cx = pendingCropX; cy = pendingCropY; cw = pendingCropW; ch = pendingCropH;
            pendingFull = false;
            have = true;
        }
        LeaveCriticalSection(&lock);

        if (have) RunInference(local, cx, cy, cw, ch);
    }

    free(local);
    return 0;
}

bool InitDepthModel(void)
{
    if (ready) return true;

    InitializeCriticalSection(&lock);

    FILE *probe = NULL;
    if ((fopen_s(&probe, DEPTH_MODEL_PATH, "rb") != 0) || (probe == NULL))
    {
        SetStatus("model file not found - run tools/get-depth-model.ps1");
        return false;
    }
    fclose(probe);

    ort = OrtGetApiBase()->GetApi(ORT_API_VERSION);
    if (ort == NULL) { SetStatus("ONNX Runtime unavailable"); return false; }

    if (Failed(ort->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "normalish", &env), "env")) return false;
    if (Failed(ort->CreateSessionOptions(&options), "options")) return false;
    // Both are required by the DirectML provider.
    if (Failed(ort->DisableMemPattern(options), "mem pattern")) return false;
    if (Failed(ort->SetSessionExecutionMode(options, ORT_SEQUENTIAL), "exec mode")) return false;

    OrtStatus *dml = OrtSessionOptionsAppendExecutionProvider_DML(options, 0);
    bool onGpu = (dml == NULL);
    if (!onGpu)
    {
        // Not fatal: the CPU provider still works, just slowly. Better to run
        // than to refuse.
        ort->ReleaseStatus(dml);
    }

    wchar_t wide[1024];
    MultiByteToWideChar(CP_UTF8, 0, DEPTH_MODEL_PATH, -1, wide, 1024);
    if (Failed(ort->CreateSession(env, wide, options, &session), "session")) return false;

    OrtAllocator *allocator = NULL;
    if (Failed(ort->GetAllocatorWithDefaultOptions(&allocator), "allocator")) return false;
    // Read the names off the model rather than hardcoding them.
    if (Failed(ort->SessionGetInputName(session, 0, allocator, &inputName), "input name")) return false;
    if (Failed(ort->SessionGetOutputName(session, 0, allocator, &outputName), "output name")) return false;

    if (Failed(ort->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &memoryInfo),
               "memory info")) return false;

    pendingInput = (float *)calloc((size_t)3*SIDE*SIDE, sizeof(float));
    if (pendingInput == NULL) { SetStatus("out of memory"); return false; }

    frameReady = CreateEventW(NULL, FALSE, FALSE, NULL);
    workerThread = CreateThread(NULL, 0, WorkerMain, NULL, 0, NULL);
    if ((frameReady == NULL) || (workerThread == NULL)) { SetStatus("thread failed"); return false; }

    SetStatus(onGpu ? "ready (DirectML)" : "ready (CPU - slow)");
    ready = true;
    return true;
}

void SubmitDepthFrame(const unsigned char *rgba, int width, int height)
{
    if (!ready || (rgba == NULL) || (width <= 0) || (height <= 0)) return;

    // Skip if the worker has not taken the last one. Waiting would stall the
    // render thread, and the newer frame is the one worth having anyway.
    EnterCriticalSection(&lock);
    bool busy = pendingFull;
    LeaveCriticalSection(&lock);
    if (busy) return;

    // Letterbox, do not squash: fitting 16:9 into a square would distort the
    // geometry relative to the real camera, and the depth would no longer
    // describe the image it is paired with.
    float scale = (float)SIDE/(float)((width > height) ? width : height);
    int fitW = (int)(width*scale + 0.5f);
    int fitH = (int)(height*scale + 0.5f);
    if (fitW > SIDE) fitW = SIDE;
    if (fitH > SIDE) fitH = SIDE;
    int offX = (SIDE - fitW)/2;
    int offY = (SIDE - fitH)/2;

    EnterCriticalSection(&lock);

    // Padding stays at zero, which after normalisation is the ImageNet mean,
    // so the border reads as neutral rather than as a black wall the model
    // would try to explain.
    memset(pendingInput, 0, (size_t)3*SIDE*SIDE*sizeof(float));

    // Average over each output pixel's source footprint rather than picking
    // one sample from the middle of it. Point sampling a 1280-wide frame down
    // to 518 already aliases; from 4K it is a 7x reduction and the model would
    // be handed a frame full of aliasing artefacts that are not in the scene.
    // Averaging instead cuts sensor noise roughly with the square root of the
    // number of samples, so a higher capture resolution genuinely helps here.
    //
    // The tap count per output pixel is capped, so the cost is bounded by the
    // model's input size instead of growing with the camera's resolution -
    // this runs on the render thread.
    const int maxTaps = 4;

    for (int y = 0; y < fitH; y++)
    {
        int sy0 = (int)(y/scale);
        int sy1 = (int)((y + 1)/scale);
        if (sy1 <= sy0) sy1 = sy0 + 1;
        if (sy1 > height) sy1 = height;
        int stepY = ((sy1 - sy0) + maxTaps - 1)/maxTaps;
        if (stepY < 1) stepY = 1;

        for (int x = 0; x < fitW; x++)
        {
            int sx0 = (int)(x/scale);
            int sx1 = (int)((x + 1)/scale);
            if (sx1 <= sx0) sx1 = sx0 + 1;
            if (sx1 > width) sx1 = width;
            int stepX = ((sx1 - sx0) + maxTaps - 1)/maxTaps;
            if (stepX < 1) stepX = 1;

            int sum[3] = { 0, 0, 0 };
            int taps = 0;
            for (int sy = sy0; sy < sy1; sy += stepY)
            {
                const unsigned char *row = rgba + (size_t)sy*width*4;
                for (int sx = sx0; sx < sx1; sx += stepX)
                {
                    const unsigned char *p = row + (size_t)sx*4;
                    sum[0] += p[0];
                    sum[1] += p[1];
                    sum[2] += p[2];
                    taps++;
                }
            }
            if (taps == 0) continue;

            for (int c = 0; c < 3; c++)
            {
                size_t index = (size_t)c*SIDE*SIDE + (size_t)(y + offY)*SIDE + (x + offX);
                pendingInput[index] = ((float)sum[c]/(float)taps/255.0f - kMean[c])/kStd[c];
            }
        }
    }

    pendingCropX = offX;
    pendingCropY = offY;
    pendingCropW = fitW;
    pendingCropH = fitH;
    pendingFull = true;

    LeaveCriticalSection(&lock);
    SetEvent(frameReady);
}

void SetDepthModelVerbose(bool on) { verboseMask = on; }
double GetSubjectScore(void) { return lastWinningScore; }

void SetSubjectBand(float metres)
{
    if (metres < 0.1f) metres = 0.1f;
    if (metres > 4.0f) metres = 4.0f;
    EnterCriticalSection(&lock);
    subjectBand = metres;
    LeaveCriticalSection(&lock);
}

bool GetDepthRange(float *nearMetres, float *farMetres)
{
    if (!ready) return false;
    EnterCriticalSection(&lock);
    bool have = (resultFar > resultNear);
    if (have) { *nearMetres = resultNear; *farMetres = resultFar; }
    LeaveCriticalSection(&lock);
    return have;
}

bool TakeLatestDepth(const float **depth, const unsigned char **subjectMask,
                     int *width, int *height)
{
    if (!ready) return false;

    EnterCriticalSection(&lock);
    bool fresh = resultFresh && (smoothedDepth != NULL) && (maskBuffer != NULL);
    if (fresh)
    {
        size_t count = (size_t)resultWidth*resultHeight;
        if (publishDepth == NULL) publishDepth = (float *)malloc(count*sizeof(float));
        if (publishMask == NULL) publishMask = (unsigned char *)malloc(count);

        if ((publishDepth != NULL) && (publishMask != NULL))
        {
            // Copied under the lock rather than handing out the live buffers,
            // so the worker's next result cannot overwrite what the render
            // thread is uploading to the GPU.
            memcpy(publishDepth, smoothedDepth, count*sizeof(float));
            memcpy(publishMask, maskBuffer, count);
            *depth = publishDepth;
            *subjectMask = publishMask;
            *width = resultWidth;
            *height = resultHeight;
            resultFresh = false;
        }
        else fresh = false;
    }
    LeaveCriticalSection(&lock);
    return fresh;
}

void ShutdownDepthModel(void)
{
    if (workerThread != NULL)
    {
        quitting = true;
        SetEvent(frameReady);
        WaitForSingleObject(workerThread, 3000);
        CloseHandle(workerThread);
        workerThread = NULL;
    }
    if (frameReady != NULL) { CloseHandle(frameReady); frameReady = NULL; }

    if (ort != NULL)
    {
        if (memoryInfo != NULL) { ort->ReleaseMemoryInfo(memoryInfo); memoryInfo = NULL; }
        if (session != NULL) { ort->ReleaseSession(session); session = NULL; }
        if (options != NULL) { ort->ReleaseSessionOptions(options); options = NULL; }
        if (env != NULL) { ort->ReleaseEnv(env); env = NULL; }
    }

    free(pendingInput); pendingInput = NULL;
    free(resultDepth); resultDepth = NULL;
    free(smoothedDepth); smoothedDepth = NULL;
    free(maskBuffer); maskBuffer = NULL;
    free(maskScratch); maskScratch = NULL;
    free(growStack); growStack = NULL;
    free(publishDepth); publishDepth = NULL;
    free(publishMask); publishMask = NULL;
    ready = false;
}
