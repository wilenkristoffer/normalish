// Analytic depth scenes.
//
// These exist so the depth-consuming renderer can be built and verified
// before any real depth source is chosen. Every candidate source - a stereo
// pair, photometric stereo, an ML model - is just a different producer of the
// same DepthFrame, so the renderer is worth getting right first. A synthetic
// scene also gives something no real source can: exact normals to check
// against.
//
// Distances are metres. The camera sits at the origin looking down +z, and
// +y points down to match image space.

#include "depth.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct Vec3 {
    float x, y, z;
} Vec3;

static Vec3 V3(float x, float y, float z) { Vec3 v = { x, y, z }; return v; }
static float Dot(Vec3 a, Vec3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
static Vec3 Scale(Vec3 a, float s) { return V3(a.x*s, a.y*s, a.z*s); }
static Vec3 Sub(Vec3 a, Vec3 b) { return V3(a.x - b.x, a.y - b.y, a.z - b.z); }

static Vec3 Normalize(Vec3 a)
{
    float len = sqrtf(Dot(a, a));
    if (len <= 0.0f) return V3(0.0f, 0.0f, -1.0f);
    return Scale(a, 1.0f/len);
}

// What a ray hit: how far along the ray, and the surface normal there.
typedef struct Hit {
    bool valid;
    float t;
    Vec3 normal;
    int surface;    // 0 = subject, 1 = back wall
} Hit;

// Rays start at the origin, so the usual quadratic collapses: with a unit
// direction, b = dot(d, C) and c = dot(C, C) - r*r.
static bool IntersectSphere(Vec3 dir, Vec3 centre, float radius, float *outT, Vec3 *outNormal)
{
    float b = Dot(dir, centre);
    float c = Dot(centre, centre) - radius*radius;
    float disc = b*b - c;
    if (disc < 0.0f) return false;

    float t = b - sqrtf(disc);
    if (t <= 0.0f) return false;

    *outT = t;
    *outNormal = Scale(Sub(Scale(dir, t), centre), 1.0f/radius);
    return true;
}

// An ellipsoid is a sphere in a space scaled by its radii, so solve there and
// bring the result back. The normal is the gradient of the implicit surface,
// which picks up a second division by the radii.
static bool IntersectEllipsoid(Vec3 dir, Vec3 centre, Vec3 radii, float *outT, Vec3 *outNormal)
{
    Vec3 o = V3(-centre.x/radii.x, -centre.y/radii.y, -centre.z/radii.z);
    Vec3 d = V3(dir.x/radii.x, dir.y/radii.y, dir.z/radii.z);

    float a = Dot(d, d);
    float b = 2.0f*Dot(o, d);
    float c = Dot(o, o) - 1.0f;
    float disc = b*b - 4.0f*a*c;
    if ((disc < 0.0f) || (a <= 0.0f)) return false;

    float t = (-b - sqrtf(disc))/(2.0f*a);
    if (t <= 0.0f) return false;

    Vec3 p = Sub(Scale(dir, t), centre);
    *outT = t;
    *outNormal = Normalize(V3(p.x/(radii.x*radii.x), p.y/(radii.y*radii.y), p.z/(radii.z*radii.z)));
    return true;
}

static void KeepNearest(Hit *best, bool hit, float t, Vec3 normal, int surface)
{
    if (!hit) return;
    if (best->valid && (t >= best->t)) return;
    best->valid = true;
    best->t = t;
    best->normal = normal;
    best->surface = surface;
}

// Roughly life-sized and at a plausible webcam working distance: a 210mm head
// at 620mm, shoulders below it, and a wall 1.3m away. Those numbers matter
// because the whole point of moving to a metric camera model is that the
// light's distance means something.
#define HEAD_RADIUS   0.105f
#define WALL_DEPTH    1.30f

static Hit TraceScene(DepthScene scene, Vec3 dir)
{
    Hit best = { 0 };

    float t;
    Vec3 n;

    if (IntersectSphere(dir, V3(0.0f, -0.02f, 0.62f), HEAD_RADIUS, &t, &n))
        KeepNearest(&best, true, t, n, 0);

    if (scene == DEPTH_SCENE_HEAD)
    {
        // Wide and flat so it reads as shoulders, and set high enough to
        // overlap the head rather than leaving a gap of wall between them.
        // A torso running out of the bottom of frame is what a real webcam
        // shot looks like.
        if (IntersectEllipsoid(dir, V3(0.0f, 0.170f, 0.70f), V3(0.26f, 0.10f, 0.14f), &t, &n))
            KeepNearest(&best, true, t, n, 0);
    }

    // The wall. A ray pointing away from it never arrives, though with this
    // camera none do.
    if (dir.z > 0.0001f)
        KeepNearest(&best, true, WALL_DEPTH/dir.z, V3(0.0f, 0.0f, -1.0f), 1);

    return best;
}

bool GenerateSyntheticScene(SyntheticScene *out, DepthScene scene,
                            int width, int height, float tanHalfFovX)
{
    if ((out == NULL) || (width <= 0) || (height <= 0)) return false;

    memset(out, 0, sizeof(*out));

    size_t pixels = (size_t)width*height;
    out->depth.depth = (float *)calloc(pixels, sizeof(float));
    out->normals = (float *)calloc(pixels*3, sizeof(float));
    out->albedo = (unsigned char *)calloc(pixels*4, 1);
    if ((out->depth.depth == NULL) || (out->normals == NULL) || (out->albedo == NULL))
    {
        FreeSyntheticScene(out);
        return false;
    }

    out->depth.width = width;
    out->depth.height = height;
    out->depth.tanHalfFovX = tanHalfFovX;

    float aspect = (float)width/(float)height;
    float tanHalfFovY = tanHalfFovX/aspect;

    for (int y = 0; y < height; y++)
    {
        // Sample pixel centres, so the geometry does not shift by half a
        // pixel against the shader's own texture sampling.
        float ndcY = ((y + 0.5f)/(float)height)*2.0f - 1.0f;

        for (int x = 0; x < width; x++)
        {
            float ndcX = ((x + 0.5f)/(float)width)*2.0f - 1.0f;

            Vec3 dir = Normalize(V3(ndcX*tanHalfFovX, ndcY*tanHalfFovY, 1.0f));
            Hit hit = TraceScene(scene, dir);

            size_t i = (size_t)y*width + x;

            if (!hit.valid)
            {
                // Depth 0 is the "no data" marker the renderer has to handle.
                out->albedo[i*4 + 0] = 0;
                out->albedo[i*4 + 1] = 0;
                out->albedo[i*4 + 2] = 0;
                out->albedo[i*4 + 3] = 255;
                continue;
            }

            // Depth is along the view axis, not along the ray, which is what
            // a depth sensor reports and what the unprojection assumes.
            out->depth.depth[i] = dir.z*hit.t;

            out->normals[i*3 + 0] = hit.normal.x;
            out->normals[i*3 + 1] = hit.normal.y;
            out->normals[i*3 + 2] = hit.normal.z;

            // Deliberately flat albedo so that what you see is the lighting
            // and not the texture. The wall gets a faint checker purely so a
            // shadow falling on it has something to read against.
            unsigned char level;
            if (hit.surface == 0) level = 184;
            else
            {
                bool checker = (((x/32) + (y/32)) & 1) != 0;
                level = checker ? 96 : 86;
            }

            out->albedo[i*4 + 0] = level;
            out->albedo[i*4 + 1] = level;
            out->albedo[i*4 + 2] = level;
            out->albedo[i*4 + 3] = 255;
        }
    }

    return true;
}

void FreeSyntheticScene(SyntheticScene *scene)
{
    if (scene == NULL) return;
    free(scene->depth.depth);
    free(scene->normals);
    free(scene->albedo);
    memset(scene, 0, sizeof(*scene));
}
