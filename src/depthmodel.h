#ifndef DEPTHMODEL_H
#define DEPTHMODEL_H

#include <stdbool.h>

// Monocular depth estimation, so the live webcam feed has real geometry.
//
// Runs Depth-Anything-V2-Metric-Hypersim-Small through ONNX Runtime with the
// DirectML provider. The metric variant matters: it outputs depth in metres,
// which is what the renderer's camera model already works in, rather than the
// unitless inverse depth the base models produce.
//
// Inference is about 22 ms on an RX 7800 XT, so it runs on its own thread and
// the renderer keeps its frame rate regardless. Nothing here includes raylib:
// frames go in as plain RGBA and depth comes out as plain floats.

// The model is tens of MB and is not in the repository. Depth stays
// unavailable, with a reason, until it has been fetched to this path.
#define DEPTH_MODEL_PATH "C:\\ProgramData\\normalish\\depth-anything-v2-metric-small.onnx"

// The square input the network expects. Frames are letterboxed into it rather
// than squashed, so the geometry stays consistent with the real camera.
#define DEPTH_MODEL_SIDE 518

bool InitDepthModel(void);
void ShutdownDepthModel(void);

// False when the model file is missing or the session could not be created;
// GetDepthModelStatus explains which, for the UI.
bool IsDepthModelReady(void);
const char *GetDepthModelStatus(void);

// Hands a frame to the worker if it is idle, and returns immediately. Dropping
// frames is the correct behaviour: depth of a head does not need every frame,
// and queueing would only add latency.
void SubmitDepthFrame(const unsigned char *rgba, int width, int height);

// The most recent depth map, cropped to the region the frame occupied so its
// texture coordinates line up with the albedo, together with a mask of the
// subject. Returns false until the first inference completes, and only
// reports true once per new result.
//
// The buffers handed back are copies taken while the worker is locked out, so
// they stay valid until the next call.
bool TakeLatestDepth(const float **depth, const unsigned char **subjectMask,
                     int *width, int *height);

// Milliseconds for the last inference, for the panel read-out.
float GetDepthInferenceMs(void);

// The range the current depth map occupies, as robust percentiles rather than
// raw min and max so one hot pixel cannot set the scale. False before the
// first result.
bool GetDepthRange(float *nearMetres, float *farMetres);

// How deep a slice of the scene counts as the subject, in metres from the
// nearest surface. Continuity alone cannot separate a person from the room
// behind them, because the two are connected through the floor.
void SetSubjectBand(float metres);

// Prints per-region scores while choosing the subject. For working out why a
// frame picked the region it did; off in normal use.
void SetDepthModelVerbose(bool on);

// The winning region's score. A frame with no subject in it still produces a
// winner, so this is how "found nothing convincing" can be told apart from
// "found someone".
double GetSubjectScore(void);

#endif
