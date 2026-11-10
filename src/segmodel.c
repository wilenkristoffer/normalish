#include "segmodel.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "onnxruntime_c_api.h"
#include "dml_provider_factory.h"

static const OrtApi *ort = NULL;

struct SegModel {
    OrtEnv *env;
    OrtSessionOptions *options;
    OrtSession *session;
    OrtMemoryInfo *memoryInfo;
    char *inputName;
    char *outputName;
    char statusText[256];
    bool ready;

    // Read off the model rather than assumed: the same network is published
    // both channels-first and channels-last depending on who converted it.
    int inW, inH;
    bool channelsLast;
    int64_t inputShape[4];

    float *input;
    unsigned char *maskOut;
    float lastMs;
};

static void SetStatus(SegModel *m, const char *text)
{
    strncpy(m->statusText, text, sizeof(m->statusText) - 1);
    m->statusText[sizeof(m->statusText) - 1] = 0;
}

static bool Failed(SegModel *m, OrtStatus *status, const char *what)
{
    if (status == NULL) return false;
    char buffer[256];
    snprintf(buffer, sizeof(buffer), "%s: %s", what, ort->GetErrorMessage(status));
    SetStatus(m, buffer);
    ort->ReleaseStatus(status);
    return true;
}

bool SegModelReady(const SegModel *m) { return (m != NULL) && m->ready; }
const char *GetSegModelStatus(const SegModel *m) { return (m != NULL) ? m->statusText : "out of memory"; }
float GetSegInferenceMs(const SegModel *m) { return (m != NULL) ? m->lastMs : 0.0f; }

static bool Load(SegModel *m, const char *path, bool onGpu)
{
    FILE *probe = NULL;
    if ((fopen_s(&probe, path, "rb") != 0) || (probe == NULL))
    {
        SetStatus(m, "segmentation model file not found");
        return false;
    }
    fclose(probe);

    if (ort == NULL) ort = OrtGetApiBase()->GetApi(ORT_API_VERSION);
    if (ort == NULL) { SetStatus(m, "ONNX Runtime unavailable"); return false; }

    if (Failed(m, ort->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "normalish-seg", &m->env), "env")) return false;
    if (Failed(m, ort->CreateSessionOptions(&m->options), "options")) return false;

    bool gpu = false;
    if (onGpu)
    {
        // Both are required by the DirectML provider.
        if (Failed(m, ort->DisableMemPattern(m->options), "mem pattern")) return false;
        if (Failed(m, ort->SetSessionExecutionMode(m->options, ORT_SEQUENTIAL), "exec mode")) return false;
        OrtStatus *dml = OrtSessionOptionsAppendExecutionProvider_DML(m->options, 0);
        gpu = (dml == NULL);
        if (!gpu) ort->ReleaseStatus(dml);
    }
    else
    {
        // Two threads: enough for a network this small, and it leaves the
        // render thread and the depth worker their cores.
        if (Failed(m, ort->SetIntraOpNumThreads(m->options, 2), "threads")) return false;
    }

    wchar_t wide[1024];
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, 1024);
    if (Failed(m, ort->CreateSession(m->env, wide, m->options, &m->session), "session")) return false;

    OrtAllocator *allocator = NULL;
    if (Failed(m, ort->GetAllocatorWithDefaultOptions(&allocator), "allocator")) return false;
    if (Failed(m, ort->SessionGetInputName(m->session, 0, allocator, &m->inputName), "input name")) return false;
    if (Failed(m, ort->SessionGetOutputName(m->session, 0, allocator, &m->outputName), "output name")) return false;

    // Input layout and size.
    OrtTypeInfo *typeInfo = NULL;
    if (Failed(m, ort->SessionGetInputTypeInfo(m->session, 0, &typeInfo), "input type")) return false;
    const OrtTensorTypeAndShapeInfo *tensorInfo = NULL;
    if (Failed(m, ort->CastTypeInfoToTensorInfo(typeInfo, &tensorInfo), "input info")) return false;
    size_t dimCount = 0;
    if (Failed(m, ort->GetDimensionsCount(tensorInfo, &dimCount), "input dims")) return false;
    int64_t dims[4] = { 1, 3, 144, 256 };
    if (dimCount == 4) ort->ReleaseStatus(ort->GetDimensions(tensorInfo, dims, 4));
    ort->ReleaseTypeInfo(typeInfo);

    m->channelsLast = (dims[3] == 3);
    int64_t hDim = m->channelsLast ? dims[1] : dims[2];
    int64_t wDim = m->channelsLast ? dims[2] : dims[3];
    // A dynamic dimension reads as -1; fall back to the landscape size.
    m->inH = (hDim > 0) ? (int)hDim : 144;
    m->inW = (wDim > 0) ? (int)wDim : 256;
    m->inputShape[0] = 1;
    m->inputShape[1] = m->channelsLast ? m->inH : 3;
    m->inputShape[2] = m->channelsLast ? m->inW : m->inH;
    m->inputShape[3] = m->channelsLast ? 3 : m->inW;

    if (Failed(m, ort->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &m->memoryInfo),
               "memory info")) return false;

    m->input = (float *)calloc((size_t)3*m->inW*m->inH, sizeof(float));
    m->maskOut = (unsigned char *)malloc((size_t)m->inW*m->inH);
    if ((m->input == NULL) || (m->maskOut == NULL)) { SetStatus(m, "out of memory"); return false; }

    char buffer[256];
    snprintf(buffer, sizeof(buffer), "ready (%s, %dx%d, %s)", gpu ? "DirectML" : "CPU",
             m->inW, m->inH, m->channelsLast ? "NHWC" : "NCHW");
    SetStatus(m, buffer);
    return true;
}

SegModel *LoadSegModel(const char *path, bool onGpu)
{
    SegModel *m = (SegModel *)calloc(1, sizeof(SegModel));
    if (m == NULL) return NULL;
    SetStatus(m, "not started");
    m->ready = Load(m, path, onGpu);
    return m;
}

void UnloadSegModel(SegModel *m)
{
    if (m == NULL) return;
    if (m->session != NULL) ort->ReleaseSession(m->session);
    if (m->options != NULL) ort->ReleaseSessionOptions(m->options);
    if (m->memoryInfo != NULL) ort->ReleaseMemoryInfo(m->memoryInfo);
    if (m->env != NULL) ort->ReleaseEnv(m->env);
    free(m->input);
    free(m->maskOut);
    free(m);
}

bool RunSegModel(SegModel *m, const unsigned char *rgba, int width, int height,
                 const unsigned char **mask, int *maskWidth, int *maskHeight)
{
    if ((m == NULL) || !m->ready || (rgba == NULL) || (width <= 0) || (height <= 0)) return false;
    int inW = m->inW, inH = m->inH;
    bool channelsLast = m->channelsLast;

    // The whole frame is stretched to the model's input. For the 16:9 model
    // that distorts nothing; for a square one it squashes the picture, which
    // measured better than fitting it inside with padding (see FINDINGS.md).
    // Each input pixel is the average of a bounded number of taps over its
    // footprint, as for the depth model: point sampling 1080p down to 144
    // rows would alias badly.
    const int maxTaps = 4;
    for (int y = 0; y < inH; y++)
    {
        int sy0 = y*height/inH, sy1 = (y + 1)*height/inH;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        int stepY = ((sy1 - sy0) + maxTaps - 1)/maxTaps;
        for (int x = 0; x < inW; x++)
        {
            int sx0 = x*width/inW, sx1 = (x + 1)*width/inW;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            int stepX = ((sx1 - sx0) + maxTaps - 1)/maxTaps;

            int sum[3] = { 0, 0, 0 };
            int taps = 0;
            for (int sy = sy0; sy < sy1; sy += stepY)
            {
                const unsigned char *row = rgba + (size_t)sy*width*4;
                for (int sx = sx0; sx < sx1; sx += stepX)
                {
                    const unsigned char *p = row + (size_t)sx*4;
                    sum[0] += p[0]; sum[1] += p[1]; sum[2] += p[2];
                    taps++;
                }
            }
            for (int c = 0; c < 3; c++)
            {
                size_t at = (size_t)y*inW + x;
                size_t index = channelsLast ? (at*3 + c) : ((size_t)c*inW*inH + at);
                m->input[index] = (float)sum[c]/(float)taps/255.0f;
            }
        }
    }

    OrtValue *inputTensor = NULL;
    if (Failed(m, ort->CreateTensorWithDataAsOrtValue(m->memoryInfo, m->input,
                                                      (size_t)3*inW*inH*sizeof(float),
                                                      m->inputShape, 4,
                                                      ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,
                                                      &inputTensor), "tensor")) return false;

    const char *inNames[1] = { m->inputName };
    const char *outNames[1] = { m->outputName };
    OrtValue *outputTensor = NULL;

    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    if (Failed(m, ort->Run(m->session, NULL, inNames, (const OrtValue *const *)&inputTensor, 1,
                           outNames, 1, &outputTensor), "run"))
    {
        ort->ReleaseValue(inputTensor);
        return false;
    }
    QueryPerformanceCounter(&t1);
    m->lastMs = (float)((double)(t1.QuadPart - t0.QuadPart)*1000.0/(double)freq.QuadPart);

    bool ok = false;
    OrtTensorTypeAndShapeInfo *outInfo = NULL;
    float *raw = NULL;
    if (!Failed(m, ort->GetTensorTypeAndShape(outputTensor, &outInfo), "output shape") &&
        !Failed(m, ort->GetTensorMutableData(outputTensor, (void **)&raw), "data"))
    {
        size_t total = 0;
        ort->ReleaseStatus(ort->GetTensorShapeElementCount(outInfo, &total));
        // The output is the input's size with some number of channels. Its
        // layout need not match the input's, so it is read off the shape: the
        // channel axis is whichever of the two candidates is small.
        size_t pixels = (size_t)inW*inH;
        size_t channels = (pixels > 0) ? total/pixels : 0;
        int64_t outDims[4] = { 0, 0, 0, 0 };
        size_t outDimCount = 0;
        ort->ReleaseStatus(ort->GetDimensionsCount(outInfo, &outDimCount));
        bool outLast = channelsLast;
        if (outDimCount == 4)
        {
            ort->ReleaseStatus(ort->GetDimensions(outInfo, outDims, 4));
            outLast = (outDims[3] == (int64_t)channels) && (outDims[1] != (int64_t)channels);
        }
        if ((channels >= 1) && (channels*pixels == total))
        {
            for (size_t i = 0; i < pixels; i++)
            {
                float v;
                if (channels <= 2)
                {
                    // One value per pixel, or (background, person): the
                    // person channel is the last one.
                    v = outLast ? raw[i*channels + (channels - 1)]
                                : raw[(channels - 1)*pixels + i];
                }
                else
                {
                    // Several classes (background, hair, skin, clothes and so
                    // on), as scores. Everything that is not background is
                    // the person, so it is one minus the background's share
                    // after a softmax.
                    float top = -1.0e30f;
                    for (size_t c = 0; c < channels; c++)
                    {
                        float s = outLast ? raw[i*channels + c] : raw[c*pixels + i];
                        if (s > top) top = s;
                    }
                    float sum = 0.0f, background = 0.0f, rawSum = 0.0f, low = 1.0e30f;
                    for (size_t c = 0; c < channels; c++)
                    {
                        float s = outLast ? raw[i*channels + c] : raw[c*pixels + i];
                        float e = expf(s - top);
                        sum += e;
                        rawSum += s;
                        if (s < low) low = s;
                        if (c == 0) background = e;
                    }
                    v = 1.0f - background/sum;
                    // Some conversions keep the softmax inside the model, in
                    // which case the values are already shares.
                    if ((low >= 0.0f) && (fabsf(rawSum - 1.0f) < 0.01f))
                        v = 1.0f - (outLast ? raw[i*channels] : raw[i]);
                }
                v = (v < 0.0f) ? 0.0f : ((v > 1.0f) ? 1.0f : v);
                m->maskOut[i] = (unsigned char)(v*255.0f + 0.5f);
            }
            *mask = m->maskOut;
            *maskWidth = inW;
            *maskHeight = inH;
            ok = true;
        }
        else SetStatus(m, "unexpected output shape");
    }
    if (outInfo != NULL) ort->ReleaseTensorTypeAndShapeInfo(outInfo);

    ort->ReleaseValue(outputTensor);
    ort->ReleaseValue(inputTensor);
    return ok;
}
