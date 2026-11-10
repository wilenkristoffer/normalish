#ifndef SEGMODEL_H
#define SEGMODEL_H

#include <stdbool.h>

// Person segmentation: which pixels of a frame are a person, as opposed to
// how far away they are. Small MediaPipe networks run through ONNX Runtime,
// the same way the depth model is.
//
// More than one can be loaded at once, so each is its own object.

// The 16:9 selfie model: 256x144, about 2 ms on the CPU. Solid on head, torso
// and shoulders whatever the clothing; misses a hand held away from the body.
#define SEG_BODY_MODEL_PATH "C:/ProgramData/normalish/selfie-segmentation-landscape.onnx"
// The multiclass selfie model: 256x256, about 3.5 ms on the GPU (far too slow
// on the CPU). Sure about heads, hair, skin and hands and does not mistake
// furniture for a person, but weak on dark clothing.
#define SEG_PARTS_MODEL_PATH "C:/ProgramData/normalish/selfie-multiclass.onnx"

typedef struct SegModel SegModel;

// Always returns an object unless memory ran out; ask SegModelReady whether
// the model actually loaded and GetSegModelStatus why not.
SegModel *LoadSegModel(const char *path, bool onGpu);
bool SegModelReady(const SegModel *model);
const char *GetSegModelStatus(const SegModel *model);
void UnloadSegModel(SegModel *model);

// Runs the model on one RGBA frame, blocking. On success *mask points at one
// byte per pixel (0 = background, 255 = person) at the model's own
// resolution, valid until the next call on the same model.
bool RunSegModel(SegModel *model, const unsigned char *rgba, int width, int height,
                 const unsigned char **mask, int *maskWidth, int *maskHeight);
float GetSegInferenceMs(const SegModel *model);

#endif
