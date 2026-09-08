#ifndef DEPTH_H
#define DEPTH_H

#include <stdbool.h>

// Which geometry model the renderer uses. The luminance model is what ships
// on main: the image is the z = 0 plane, brightness stands in for height and
// the viewer is orthographic. It stays a first-class choice, because for a
// still photo there is no depth to be had and the cheat is the right answer.
typedef enum GeometryMode {
    GEOMETRY_LUMINANCE = 0,
    GEOMETRY_DEPTH = 1
} GeometryMode;

typedef enum DepthScene {
    DEPTH_SCENE_SPHERE = 0,   // one sphere on a back plane: the maths check
    DEPTH_SCENE_HEAD = 1      // head plus shoulders on a wall: the look check
} DepthScene;

// A depth map in metres along the view axis, plus the one camera parameter
// needed to unproject it back to 3D positions. A depth of 0 means "no data
// for this pixel", which every real source will produce and the renderer has
// to survive.
//
// The camera looks down +z from the origin, so depth grows away from the
// viewer. Image space has y = 0 at the top, so +y points down in 3D too;
// that is worth stating because it is the sign everything downstream trips
// over.
typedef struct DepthFrame {
    int width;
    int height;
    float tanHalfFovX;
    float *depth;
} DepthFrame;

// Ground truth for a synthetic scene: the depth a real sensor would report,
// the normals computed analytically from the geometry, and a matching albedo.
// The normals exist so the renderer's depth-to-normal maths can be checked
// against an exact answer rather than judged by eye.
typedef struct SyntheticScene {
    DepthFrame depth;
    float *normals;         // width*height*3, unit vectors
    unsigned char *albedo;  // width*height*4, RGBA
} SyntheticScene;

bool GenerateSyntheticScene(SyntheticScene *out, DepthScene scene,
                            int width, int height, float tanHalfFovX);
void FreeSyntheticScene(SyntheticScene *scene);

#endif
