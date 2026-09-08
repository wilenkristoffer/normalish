#include "depthmodel.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
static int resultWidth = 0, resultHeight = 0;
static bool resultFresh = false;
static float resultNear = 0.0f, resultFar = 0.0f;
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

    for (int y = 0; y < fitH; y++)
    {
        int sy = (int)((y + 0.5f)/scale);
        if (sy >= height) sy = height - 1;
        const unsigned char *row = rgba + (size_t)sy*width*4;

        for (int x = 0; x < fitW; x++)
        {
            int sx = (int)((x + 0.5f)/scale);
            if (sx >= width) sx = width - 1;
            const unsigned char *p = row + (size_t)sx*4;

            for (int c = 0; c < 3; c++)
            {
                size_t index = (size_t)c*SIDE*SIDE + (size_t)(y + offY)*SIDE + (x + offX);
                pendingInput[index] = (p[c]/255.0f - kMean[c])/kStd[c];
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

bool GetDepthRange(float *nearMetres, float *farMetres)
{
    if (!ready) return false;
    EnterCriticalSection(&lock);
    bool have = (resultFar > resultNear);
    if (have) { *nearMetres = resultNear; *farMetres = resultFar; }
    LeaveCriticalSection(&lock);
    return have;
}

bool TakeLatestDepth(const float **depth, int *width, int *height)
{
    if (!ready) return false;

    EnterCriticalSection(&lock);
    bool fresh = resultFresh && (smoothedDepth != NULL);
    if (fresh)
    {
        *depth = smoothedDepth;
        *width = resultWidth;
        *height = resultHeight;
        resultFresh = false;
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
    ready = false;
}
