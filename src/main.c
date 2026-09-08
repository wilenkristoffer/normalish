#include "raylib.h"
#include "webcam.h"
#include "vcamhost.h"
#include "depth.h"
#include "depthmodel.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define RAYGUI_IMPLEMENTATION
#include "raygui.h"

#define TAU 6.2831853f

// Smooth pseudo-random drift in roughly -1..1. Two sine waves at unrelated
// frequencies read as wandering rather than as an obvious oscillation, and
// unlike interpolating between random waypoints there is no momentary stop at
// each target. Random phases are drawn once at startup so each run differs.
typedef struct Drift {
    float freqA;
    float freqB;
    float phaseA;
    float phaseB;
} Drift;

static Drift MakeDrift(float hzA, float hzB)
{
    Drift d;
    d.freqA = hzA*TAU;
    d.freqB = hzB*TAU;
    d.phaseA = (float)GetRandomValue(0, 6283)/1000.0f;
    d.phaseB = (float)GetRandomValue(0, 6283)/1000.0f;
    return d;
}

static float DriftValue(Drift d, float t)
{
    return 0.62f*sinf(t*d.freqA + d.phaseA) + 0.38f*sinf(t*d.freqB + d.phaseB);
}

typedef struct RelightShader {
    Shader shader;
    int lightPosLoc;
    int lightColorLoc;
    int lightIntensityLoc;
    int falloffLoc;
    int ambientLoc;
    int specularStrengthLoc;
    int shininessLoc;
    int aspectLoc;
    int viewModeLoc;
    int texelSizeLoc;
    int reliefLoc;
    int sampleSpacingLoc;
    int geometryModeLoc;
    int depthMapLoc;
    int tanHalfFovXLoc;
    int depthSpacingLoc;
    int depthRangeLoc;
    int distanceScaleLoc;
    int depthTexelSizeLoc;
    int depthReliefLoc;
    int subjectMaskLoc;
    int maskEnabledLoc;
    int maskStrengthLoc;
    int shadowStepsLoc;
    int shadowBiasLoc;
    int shadowThicknessLoc;
} RelightShader;

typedef struct LightSettings {
    Color color;
    float intensity;
    float falloff;
    float ambient;
    float specularStrength;
    float shininess;
    float height;
} LightSettings;

static RelightShader LoadRelightShader(void)
{
    RelightShader r;
    r.shader = LoadShader(0, SHADER_DIR "/lighting.fs");
    r.lightPosLoc = GetShaderLocation(r.shader, "lightPos");
    r.lightColorLoc = GetShaderLocation(r.shader, "lightColor");
    r.lightIntensityLoc = GetShaderLocation(r.shader, "lightIntensity");
    r.falloffLoc = GetShaderLocation(r.shader, "falloff");
    r.ambientLoc = GetShaderLocation(r.shader, "ambient");
    r.specularStrengthLoc = GetShaderLocation(r.shader, "specularStrength");
    r.shininessLoc = GetShaderLocation(r.shader, "shininess");
    r.aspectLoc = GetShaderLocation(r.shader, "aspect");
    r.viewModeLoc = GetShaderLocation(r.shader, "viewMode");
    r.texelSizeLoc = GetShaderLocation(r.shader, "texelSize");
    r.reliefLoc = GetShaderLocation(r.shader, "relief");
    r.sampleSpacingLoc = GetShaderLocation(r.shader, "sampleSpacing");
    r.geometryModeLoc = GetShaderLocation(r.shader, "geometryMode");
    r.depthMapLoc = GetShaderLocation(r.shader, "depthMap");
    r.tanHalfFovXLoc = GetShaderLocation(r.shader, "tanHalfFovX");
    r.depthSpacingLoc = GetShaderLocation(r.shader, "depthSpacing");
    r.depthRangeLoc = GetShaderLocation(r.shader, "depthRange");
    r.distanceScaleLoc = GetShaderLocation(r.shader, "distanceScale");
    r.depthTexelSizeLoc = GetShaderLocation(r.shader, "depthTexelSize");
    r.depthReliefLoc = GetShaderLocation(r.shader, "depthRelief");
    r.subjectMaskLoc = GetShaderLocation(r.shader, "subjectMask");
    r.maskEnabledLoc = GetShaderLocation(r.shader, "maskEnabled");
    r.maskStrengthLoc = GetShaderLocation(r.shader, "maskStrength");
    r.shadowStepsLoc = GetShaderLocation(r.shader, "shadowSteps");
    r.shadowBiasLoc = GetShaderLocation(r.shader, "shadowBias");
    r.shadowThicknessLoc = GetShaderLocation(r.shader, "shadowThickness");
    return r;
}

// Weighted centroid of the brightest pixels. Steadier than a plain argmax,
// which hops between equally bright pixels and makes the light jitter.
static bool TrackBrightestPoint(Image frame, int step, Vector2 *point)
{
    const unsigned char *pixels = (const unsigned char *)frame.data;
    int brightest = 0;

    for (int y = 0; y < frame.height; y += step)
    {
        for (int x = 0; x < frame.width; x += step)
        {
            const unsigned char *p = pixels + ((size_t)y*frame.width + x)*4;
            int luma = (p[0]*54 + p[1]*183 + p[2]*19) >> 8;
            if (luma > brightest) brightest = luma;
        }
    }

    if (brightest < 40) return false;   // nothing bright enough to lock onto

    int threshold = brightest - 12;
    double sumX = 0.0;
    double sumY = 0.0;
    long long count = 0;

    for (int y = 0; y < frame.height; y += step)
    {
        for (int x = 0; x < frame.width; x += step)
        {
            const unsigned char *p = pixels + ((size_t)y*frame.width + x)*4;
            int luma = (p[0]*54 + p[1]*183 + p[2]*19) >> 8;
            if (luma >= threshold)
            {
                sumX += x;
                sumY += y;
                count++;
            }
        }
    }

    if (count == 0) return false;
    point->x = (float)(sumX/(double)count);
    point->y = (float)(sumY/(double)count);
    return true;
}

// Capture resolutions offered in the UI. The camera decides what it can
// actually deliver, so the panel reports the mode it settled on.
static const int captureWidths[] = { 640, 1280, 1920 };
static const int captureHeights[] = { 480, 720, 1080 };
static const char *captureModeList = "480p;720p;1080p";

// Opens (or reopens) a camera and rebuilds the frame buffer and texture for
// it, since a different device or mode can mean a different frame size. Used
// both at startup and whenever the dropdowns change.
static bool ReopenWebcam(int device, int mode, Image *frame, Texture2D *texture)
{
    if (texture->id != 0)
    {
        UnloadTexture(*texture);
        *texture = (Texture2D){ 0 };
    }
    if (frame->data != NULL)
    {
        UnloadImage(*frame);
        *frame = (Image){ 0 };
    }

    if (!OpenWebcam(device, captureWidths[mode], captureHeights[mode])) return false;

    *frame = GenImageColor(GetWebcamWidth(), GetWebcamHeight(), BLACK);
    *texture = LoadTextureFromImage(*frame);
    SetTextureFilter(*texture, TEXTURE_FILTER_BILINEAR);
    return true;
}

// Largest rectangle with the source's aspect ratio that fits in the area.
static Rectangle FitRectangle(Rectangle area, int sourceWidth, int sourceHeight)
{
    float sourceAspect = (float)sourceWidth/(float)sourceHeight;
    float width = area.height*sourceAspect;
    float height = area.height;
    if (width > area.width)
    {
        width = area.width;
        height = area.width/sourceAspect;
    }
    return (Rectangle){
        area.x + (area.width - width)*0.5f,
        area.y + (area.height - height)*0.5f,
        width, height
    };
}

// A synthetic scene carries its own albedo as well as its depth, because a
// sphere's depth map means nothing applied to a photograph of a person. The
// scene data is kept alongside the textures so the analytic normals stay
// available for the verification pass.
typedef struct SyntheticSource {
    bool ready;
    SyntheticScene scene;
    Texture2D albedo;
    Texture2D depth;
} SyntheticSource;

// A 65 degree horizontal field of view: narrower than a webcam's usual 70-90,
// and close to what a sensible stereo rig would use for a head and shoulders.
#define SYNTHETIC_FOV_TAN 0.6371f

static bool LoadSyntheticSource(SyntheticSource *out, DepthScene which, int width, int height)
{
    if (!GenerateSyntheticScene(&out->scene, which, width, height, SYNTHETIC_FOV_TAN)) return false;

    Image albedoImage = {
        .data = out->scene.albedo,
        .width = width, .height = height, .mipmaps = 1,
        .format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8
    };
    out->albedo = LoadTextureFromImage(albedoImage);
    SetTextureFilter(out->albedo, TEXTURE_FILTER_BILINEAR);

    Image depthImage = {
        .data = out->scene.depth.depth,
        .width = width, .height = height, .mipmaps = 1,
        .format = PIXELFORMAT_UNCOMPRESSED_R32
    };
    out->depth = LoadTextureFromImage(depthImage);
    // Point sampling, deliberately. Interpolating across a silhouette invents
    // a depth halfway between the face and the wall behind it, and a surface
    // that steep produces a normal pointing anywhere but out.
    SetTextureFilter(out->depth, TEXTURE_FILTER_POINT);

    out->ready = (out->albedo.id != 0) && (out->depth.id != 0);
    if (!out->ready)
    {
        TraceLog(LOG_WARNING, "DEPTH: synthetic scene %d failed to upload (albedo id %u, depth id %u)",
                 (int)which, out->albedo.id, out->depth.id);
    }
    return out->ready;
}

static void UnloadSyntheticSource(SyntheticSource *s)
{
    if (s->albedo.id != 0) UnloadTexture(s->albedo);
    if (s->depth.id != 0) UnloadTexture(s->depth);
    FreeSyntheticScene(&s->scene);
    s->ready = false;
}

// Renders the shader's depth-derived normals for a synthetic scene and
// compares them against that scene's analytic normals.
//
// This exists because an unprojection with the aspect ratio inverted, or a
// cross product taken the wrong way round, produces a normal map that looks
// entirely plausible on screen and is wrong. Those mistakes show up here as
// tens of degrees. The floor is about 0.45 degrees, since the normals come
// back through an 8-bit render target.
static int VerifyNormals(RelightShader rs, DepthScene which, int width, int height)
{
    SyntheticSource s = { 0 };
    if (!LoadSyntheticSource(&s, which, width, height))
    {
        printf("VERIFY: could not build scene %d\n", (int)which);
        return 1;
    }

    RenderTexture2D target = LoadRenderTexture(width, height);

    float aspect = (float)width/(float)height;
    int geometry = GEOMETRY_DEPTH;
    int view = 1;                       // normals
    float spacing = 1.0f;
    float range[2] = { 0.35f, 1.45f };
    float texel[2] = { 1.0f/(float)width, 1.0f/(float)height };

    SetShaderValue(rs.shader, rs.aspectLoc, &aspect, SHADER_UNIFORM_FLOAT);
    SetShaderValue(rs.shader, rs.viewModeLoc, &view, SHADER_UNIFORM_INT);
    SetShaderValue(rs.shader, rs.geometryModeLoc, &geometry, SHADER_UNIFORM_INT);
    SetShaderValue(rs.shader, rs.tanHalfFovXLoc, &s.scene.depth.tanHalfFovX, SHADER_UNIFORM_FLOAT);
    SetShaderValue(rs.shader, rs.depthSpacingLoc, &spacing, SHADER_UNIFORM_FLOAT);
    SetShaderValue(rs.shader, rs.depthRangeLoc, range, SHADER_UNIFORM_VEC2);
    SetShaderValue(rs.shader, rs.texelSizeLoc, texel, SHADER_UNIFORM_VEC2);
    // The depth map is the same size as the albedo here, but it has to be set
    // explicitly all the same: leaving it at zero collapses the gradient taps
    // onto a single texel, which is how this check caught the uniform being
    // missed in the first place.
    SetShaderValue(rs.shader, rs.depthTexelSizeLoc, texel, SHADER_UNIFORM_VEC2);
    float verifyScale = 1.0f;
    SetShaderValue(rs.shader, rs.distanceScaleLoc, &verifyScale, SHADER_UNIFORM_FLOAT);
    // Ground truth is the unattenuated surface, so keep all of the tilt.
    SetShaderValue(rs.shader, rs.depthReliefLoc, &verifyScale, SHADER_UNIFORM_FLOAT);
    int verifyMask = 0;
    SetShaderValue(rs.shader, rs.maskEnabledLoc, &verifyMask, SHADER_UNIFORM_INT);

    Rectangle whole = { 0.0f, 0.0f, (float)width, (float)height };

    BeginTextureMode(target);
    ClearBackground(BLACK);
    BeginShaderMode(rs.shader);
    SetShaderValueTexture(rs.shader, rs.depthMapLoc, s.depth);
    DrawTexturePro(s.albedo, whole, whole, (Vector2){ 0.0f, 0.0f }, 0.0f, WHITE);
    EndShaderMode();
    EndTextureMode();

    Image shot = LoadImageFromTexture(target.texture);
    if (shot.data == NULL)
    {
        printf("VERIFY: readback failed\n");
        return 1;
    }
    ImageFlipVertical(&shot);   // render textures are bottom-up

    const unsigned char *pixels = (const unsigned char *)shot.data;
    const float *truth = s.scene.normals;
    const float *depth = s.scene.depth.depth;

    double sum = 0.0;
    double worst = 0.0;
    long long counted = 0;
    double edgeSum = 0.0;
    double edgeWorst = 0.0;
    long long edgeCounted = 0;

    for (int y = 1; y < height - 1; y++)
    {
        for (int x = 1; x < width - 1; x++)
        {
            size_t i = (size_t)y*width + x;
            if (depth[i] <= 0.0f) continue;

            // At a silhouette the right answer is genuinely ambiguous: the
            // one-sided difference reports whichever surface it stayed on, and
            // either is defensible. Those pixels are counted separately rather
            // than allowed to swamp the interior.
            bool edge = false;
            for (int dy = -1; dy <= 1; dy++)
            {
                for (int dx = -1; dx <= 1; dx++)
                {
                    float z = depth[(size_t)(y + dy)*width + (x + dx)];
                    if ((z <= 0.0f) || (fabsf(z - depth[i]) > 0.02f)) edge = true;
                }
            }

            float nx = (pixels[i*4 + 0]/255.0f)*2.0f - 1.0f;
            float ny = (pixels[i*4 + 1]/255.0f)*2.0f - 1.0f;
            float nz = (pixels[i*4 + 2]/255.0f)*2.0f - 1.0f;
            float len = sqrtf(nx*nx + ny*ny + nz*nz);
            if (len <= 0.0001f) continue;
            nx /= len; ny /= len; nz /= len;

            float d = nx*truth[i*3 + 0] + ny*truth[i*3 + 1] + nz*truth[i*3 + 2];
            if (d > 1.0f) d = 1.0f;
            if (d < -1.0f) d = -1.0f;
            double degrees = acos(d)*180.0/3.14159265358979;

            if (edge)
            {
                edgeSum += degrees;
                if (degrees > edgeWorst) edgeWorst = degrees;
                edgeCounted++;
            }
            else
            {
                sum += degrees;
                if (degrees > worst) worst = degrees;
                counted++;
            }
        }
    }

    printf("VERIFY scene %d at %dx%d\n", (int)which, width, height);
    printf("  interior : mean %.3f deg, worst %.3f deg over %lld px\n",
           counted ? sum/(double)counted : 0.0, worst, counted);
    printf("  silhouette: mean %.3f deg, worst %.3f deg over %lld px\n",
           edgeCounted ? edgeSum/(double)edgeCounted : 0.0, edgeWorst, edgeCounted);

    // Interior normals are exactly computable, so hold them to a tight bound.
    // 1.5 degrees leaves room over the 8-bit floor without hiding a real bug.
    bool pass = (counted > 0) && ((sum/(double)counted) < 1.5);
    printf("  %s\n", pass ? "PASS" : "FAIL");

    UnloadImage(shot);
    UnloadRenderTexture(target);
    UnloadSyntheticSource(&s);
    return pass ? 0 : 1;
}


// Runs the depth model over one saved image and writes out what it produced.
//
// This exists because the subject mask cannot be judged against a live camera:
// three attempts at it were assessed from screenshots, and the last one was
// taken of an empty chair, which says nothing about a person detector. A fixed
// frame makes the check repeatable, and it runs the same code path the app
// does rather than a parallel copy of it.
static int RunDepthOnImage(const char *path, const char *outPrefix)
{
    if (!InitDepthModel())
    {
        printf("DEPTH-IMAGE: %s\n", GetDepthModelStatus());
        return 1;
    }

    Image frame = LoadImage(path);
    if (frame.data == NULL)
    {
        printf("DEPTH-IMAGE: could not load %s\n", path);
        return 1;
    }
    ImageFormat(&frame, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    printf("DEPTH-IMAGE: %s  %dx%d\n", path, frame.width, frame.height);

    SubmitDepthFrame((const unsigned char *)frame.data, frame.width, frame.height);

    const float *depth = NULL;
    const unsigned char *mask = NULL;
    int w = 0, h = 0;
    bool got = false;
    for (int wait = 0; (wait < 200) && !got; wait++)
    {
        got = TakeLatestDepth(&depth, &mask, &w, &h);
        if (!got) WaitTime(0.02);
    }
    if (!got)
    {
        printf("DEPTH-IMAGE: no result\n");
        return 1;
    }

    float near = 0.0f, far = 0.0f;
    GetDepthRange(&near, &far);
    printf("  depth map  : %dx%d\n", w, h);
    printf("  range      : %.3f to %.3f m (robust)\n", near, far);
    printf("  inference  : %.1f ms\n", GetDepthInferenceMs());

    size_t count = (size_t)w*h;
    long long covered = 0;
    for (size_t i = 0; i < count; i++) if (mask[i] > 127) covered++;
    printf("  mask       : %.1f%% of the frame\n", 100.0*(double)covered/(double)count);

    // Near is white, so the visualisation matches the in-app depth view.
    unsigned char *grey = (unsigned char *)RL_MALLOC(count);
    for (size_t i = 0; i < count; i++)
    {
        float t = (depth[i] - near)/((far > near) ? (far - near) : 1.0f);
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
        grey[i] = (unsigned char)((1.0f - t)*255.0f);
    }

    Image depthImage = { .data = grey, .width = w, .height = h, .mipmaps = 1,
                         .format = PIXELFORMAT_UNCOMPRESSED_GRAYSCALE };
    Image maskImage = { .data = (void *)mask, .width = w, .height = h, .mipmaps = 1,
                        .format = PIXELFORMAT_UNCOMPRESSED_GRAYSCALE };

    ExportImage(depthImage, TextFormat("%s-depth.png", outPrefix));
    ExportImage(maskImage, TextFormat("%s-mask.png", outPrefix));
    printf("  wrote      : %s-depth.png, %s-mask.png\n", outPrefix, outPrefix);

    RL_FREE(grey);
    UnloadImage(frame);
    ShutdownDepthModel();
    return 0;
}

int main(int argc, char **argv)
{
    // A leading flag is not a photo path.
    bool verifyOnly = (argc > 1) && (TextIsEqual(argv[1], "--verify-normals"));
    bool depthImageOnly = (argc > 3) && (TextIsEqual(argv[1], "--depth-image"));
    const char *photoPath = ((argc > 1) && (argv[1][0] != '-')) ? argv[1]
                                                                : ASSET_DIR "/portrait.png";

    const float baseWidth = 1240.0f;
    const float baseHeight = 860.0f;

    SetConfigFlags(FLAG_WINDOW_RESIZABLE);
    InitWindow((int)baseWidth, (int)baseHeight, "normalish");
    SetTargetFPS(60);

    // raylib sizes windows in physical pixels, so on a display with OS scaling
    // everything would be drawn at a fraction of its intended size - which is
    // why text looks tiny on a 4K screen. Scale the window and every UI
    // measurement by the monitor's DPI factor instead.
    Vector2 dpiScale = GetWindowScaleDPI();
    float uiScale = (dpiScale.x > 1.0f) ? dpiScale.x : 1.0f;
    TraceLog(LOG_INFO, "UI: monitor DPI scale %.2f, using UI scale %.2f", dpiScale.x, uiScale);

    if (uiScale > 1.0f)
    {
        int scaledWidth = (int)(baseWidth*uiScale);
        int scaledHeight = (int)(baseHeight*uiScale);
        SetWindowSize(scaledWidth, scaledHeight);

        int monitor = GetCurrentMonitor();
        SetWindowPosition((GetMonitorWidth(monitor) - scaledWidth)/2,
                          (GetMonitorHeight(monitor) - scaledHeight)/2);
    }
    SetWindowMinSize((int)(760.0f*uiScale), (int)(600.0f*uiScale));
    GuiSetStyle(DEFAULT, TEXT_SIZE, (int)(14.0f*uiScale));

    Texture2D photoTexture = LoadTexture(photoPath);
    SetTextureFilter(photoTexture, TEXTURE_FILTER_BILINEAR);

    RelightShader rs = LoadRelightShader();

    if (depthImageOnly)
    {
        int result = RunDepthOnImage(argv[2], argv[3]);
        UnloadShader(rs.shader);
        CloseWindow();
        return result;
    }

    if (verifyOnly)
    {
        int failures = 0;
        failures += VerifyNormals(rs, DEPTH_SCENE_SPHERE, 1280, 720);
        failures += VerifyNormals(rs, DEPTH_SCENE_HEAD, 1280, 720);
        UnloadShader(rs.shader);
        CloseWindow();
        return failures;
    }

    LightSettings light = {
        .color = (Color){ 255, 242, 217, 255 },
        .intensity = 2.2f,
        .falloff = 8.0f,
        .ambient = 0.18f,
        .specularStrength = 0.35f,
        .shininess = 32.0f,
        .height = 0.25f
    };
    float relief = 1.0f;
    float sampleSpacing = 3.0f;
    int viewMode = 0;

    // Off by default: ticking it publishes "normalish camera" to other
    // applications, and nobody's camera list should change without asking.
    bool exposeAsCamera = false;
    RenderTexture2D cameraTarget = LoadRenderTexture(GetVirtualCameraWidth(), GetVirtualCameraHeight());
    Image cameraFrame = GenImageColor(GetVirtualCameraWidth(), GetVirtualCameraHeight(), BLACK);

    bool webcamSystemReady = InitWebcamSystem();
    bool webcamReady = false;
    Image webcamFrame = { 0 };
    Texture2D webcamTexture = { 0 };
    Vector2 trackedPoint = { 0 };
    bool tracking = false;
    bool followBrightest = false;
    bool mirrorWebcam = true;
    float smoothedU = 0.5f;
    float smoothedV = 0.5f;

    // Machines often have more than one camera - a real one plus something
    // virtual like OBS - so the device is selectable rather than assumed.
    char deviceListText[512] = { 0 };
    int selectedDevice = 0;
    // 720p, not 480p: 640x480 is 4:3, and asking for it makes a 16:9 camera
    // hand back its one 4:3 mode, which then gets letterboxed downstream.
    int selectedMode = 1;          // index into captureWidths/captureHeights
    int openedDevice = -1;
    int openedMode = -1;
    bool deviceDropdownOpen = false;
    bool modeDropdownOpen = false;

    if (webcamSystemReady)
    {
        const char *names[8];
        int count = GetWebcamDeviceCount();
        if (count > 8) count = 8;
        for (int i = 0; i < count; i++) names[i] = GetWebcamDeviceName(i);
        TextCopy(deviceListText, TextJoin(names, count, ";"));

        webcamReady = ReopenWebcam(selectedDevice, selectedMode, &webcamFrame, &webcamTexture);
        if (webcamReady)
        {
            openedDevice = selectedDevice;
            openedMode = selectedMode;
        }
    }

    // 0 = still photo, 1 = live webcam feed, 2 = synthetic sphere,
    // 3 = synthetic head. The synthetic scenes exist to build and check the
    // depth renderer before a real depth source is chosen.
    int sourceMode = webcamReady ? 1 : 0;

    SyntheticSource synthetic[2] = { 0 };
    LoadSyntheticSource(&synthetic[0], DEPTH_SCENE_SPHERE, 1280, 720);
    LoadSyntheticSource(&synthetic[1], DEPTH_SCENE_HEAD, 1280, 720);

    // The flat model stays a first-class choice, not a fallback: it is what
    // ships on main, and for a still photo there is no depth to be had. It is
    // also the honest comparison - the same synthetic head under the cheat and
    // under real depth, one keystroke apart.
    int geometryMode = GEOMETRY_DEPTH;
    float depthSpacing = 1.0f;
    // Exact depth wants all of the measured tilt; a monocular model does not.
    // Set per source below rather than as one default, because the right value
    // differs by an order of magnitude between them.
    float depthRelief = 1.0f;
    bool reliefFollowsSource = true;

    // Shadows: 24 steps is enough for a head at this scale and costs little.
    // The thickness default is roughly the depth of a head, which is the
    // shallowest occluder the scenes contain.
    float shadowSteps = 24.0f;
    float shadowBias = 0.006f;
    float shadowThickness = 0.35f;

    // Bound whenever the active source has no depth map. A single zero texel
    // reads back as "no data", which the shader already handles; leaving the
    // sampler unbound would instead read whatever texture happened to be on
    // that unit.
    // Live depth from the ML model, for the webcam feed. The texture is
    // recreated when the model's crop size changes, which happens if the
    // capture resolution or aspect changes.
    bool depthModelReady = InitDepthModel();
    TraceLog(LOG_INFO, "DEPTH: model %s", GetDepthModelStatus());
    Texture2D liveDepth = { 0 };
    Texture2D liveMask = { 0 };
    // On by default: relighting the background is what produced the worst
    // artefacts, and it is not what the effect is for.
    bool subjectOnly = true;
    float maskStrength = 0.0f;
    float subjectBand = 0.9f;
    int liveDepthW = 0, liveDepthH = 0;
    bool haveLiveDepth = false;
    // A webcam's horizontal field of view is not something we can query, and
    // the unprojection needs it. 70 degrees is typical; the slider lets it be
    // matched by eye, since getting it wrong only makes the relief too strong
    // or too weak rather than breaking anything.
    float cameraFovDegrees = 70.0f;

    // Depth mode keeps its own falloff and light distance. The luminance model
    // measures distance in image heights and the depth model in metres, so a
    // single value cannot mean the right thing in both. Falloff here is
    // relative to the scene's nearest surface, so it survives a change of
    // scene scale.
    float depthFalloff = 2.0f;
    float depthLightDistance = 0.7f;
    float knownSceneNear = 0.0f;

    float noDepthTexel = 0.0f;
    Image noDepthImage = {
        .data = &noDepthTexel, .width = 1, .height = 1, .mipmaps = 1,
        .format = PIXELFORMAT_UNCOMPRESSED_R32
    };
    Texture2D noDepth = LoadTextureFromImage(noDepthImage);

    // The light is stored in normalised image coordinates rather than screen
    // pixels, so resizing the window does not move it off the subject.
    float lightU = 0.5f;
    float lightV = 0.5f;

    // Demo mode drives the light on its own, for showing the effect off
    // without holding the mouse. Deliberately drifts at unrelated rates per
    // axis, so the path wanders instead of tracing a diagonal.
    bool demoEnabled = false;
    int demoMode = 0;               // 0 = basic (position only), 1 = full
    float demoTime = 0.0f;
    Drift driftU = MakeDrift(0.043f, 0.101f);
    Drift driftV = MakeDrift(0.061f, 0.079f);
    Drift driftHeight = MakeDrift(0.037f, 0.071f);
    Drift driftHue = MakeDrift(0.023f, 0.053f);

    while (!WindowShouldClose())
    {
        if (IsKeyPressed(KEY_R))
        {
            UnloadShader(rs.shader);
            rs = LoadRelightShader();
        }
        if (IsKeyPressed(KEY_ONE)) viewMode = 0;
        if (IsKeyPressed(KEY_TWO)) viewMode = 1;
        if (IsKeyPressed(KEY_THREE)) viewMode = 2;
        if (IsKeyPressed(KEY_FOUR)) viewMode = 3;
        if (IsKeyPressed(KEY_FIVE)) viewMode = 4;

        if (webcamReady && UpdateWebcamFrame((unsigned char *)webcamFrame.data))
        {
            UpdateTexture(webcamTexture, webcamFrame.data);
            Vector2 found = { 0 };
            tracking = TrackBrightestPoint(webcamFrame, 4, &found);
            if (tracking) trackedPoint = found;

            // The worker drops the frame if it is still busy, so this is a
            // submission attempt rather than a queue.
            if (depthModelReady)
                SubmitDepthFrame((const unsigned char *)webcamFrame.data,
                                 webcamFrame.width, webcamFrame.height);
        }

        if (depthModelReady)
        {
            const float *fresh = NULL;
            const unsigned char *freshMask = NULL;
            int freshW = 0, freshH = 0;
            if (TakeLatestDepth(&fresh, &freshMask, &freshW, &freshH))
            {
                if ((freshW != liveDepthW) || (freshH != liveDepthH))
                {
                    if (liveDepth.id != 0) UnloadTexture(liveDepth);
    if (liveMask.id != 0) UnloadTexture(liveMask);
                    Image image = {
                        .data = (void *)fresh, .width = freshW, .height = freshH,
                        .mipmaps = 1, .format = PIXELFORMAT_UNCOMPRESSED_R32
                    };
                    liveDepth = LoadTextureFromImage(image);
                    if (liveMask.id != 0) UnloadTexture(liveMask);
                    Image maskImage = {
                        .data = (void *)freshMask, .width = freshW, .height = freshH,
                        .mipmaps = 1, .format = PIXELFORMAT_UNCOMPRESSED_GRAYSCALE
                    };
                    liveMask = LoadTextureFromImage(maskImage);
                    // Bilinear, and deliberately: the upsample from 518 wide
                    // to the frame is what turns the feathered mask into a
                    // smooth edge rather than a stair-stepped one.
                    SetTextureFilter(liveMask, TEXTURE_FILTER_BILINEAR);
                    // Bilinear here, unlike the synthetic scenes: this map is
                    // 518 wide against a 1280-wide frame, and point sampling
                    // that far upscaled is visibly blocky. It does soften
                    // silhouettes, which is the known trade.
                    SetTextureFilter(liveDepth, TEXTURE_FILTER_BILINEAR);
                    liveDepthW = freshW;
                    liveDepthH = freshH;
                }
                else
                {
                    UpdateTexture(liveDepth, fresh);
                    UpdateTexture(liveMask, freshMask);
                }
                haveLiveDepth = (liveDepth.id != 0);
            }
        }

        // Layout is recomputed every frame, so the window can be resized and
        // every measurement scales with the display.
        float screenW = (float)GetScreenWidth();
        float screenH = (float)GetScreenHeight();
        float panelWidth = 320.0f*uiScale;
        float margin = 20.0f*uiScale;
        Rectangle area = { margin, margin,
                           screenW - panelWidth - margin*2.0f,
                           screenH - margin*2.0f };

        // Only the webcam entry falls back to the photo; the synthetic scenes
        // are always available and must not be clobbered here.
        if (!webcamReady && (sourceMode == 1)) sourceMode = 0;

        int syntheticIndex = (sourceMode == 2) ? 0 : ((sourceMode == 3) ? 1 : -1);
        if ((syntheticIndex >= 0) && !synthetic[syntheticIndex].ready)
        {
            sourceMode = 0;
            syntheticIndex = -1;
        }

        Texture2D albedoTexture;
        if (syntheticIndex >= 0) albedoTexture = synthetic[syntheticIndex].albedo;
        else if (sourceMode == 1) albedoTexture = webcamTexture;
        else albedoTexture = photoTexture;

        // Depth mode is only meaningful where there is a depth map. The photo
        // and the webcam have none yet, so they stay on the luminance model
        // whatever the setting says.
        // Depth is available from a synthetic scene, or from the model on the
        // live feed once it has produced its first result.
        bool liveDepthUsable = (sourceMode == 1) && haveLiveDepth;
        bool depthAvailable = (syntheticIndex >= 0) || liveDepthUsable;
        int activeGeometry = (depthAvailable && (geometryMode == GEOMETRY_DEPTH))
                           ? GEOMETRY_DEPTH : GEOMETRY_LUMINANCE;

        float tanHalfFovX = SYNTHETIC_FOV_TAN;
        Texture2D activeDepth = noDepth;
        if (syntheticIndex >= 0)
        {
            tanHalfFovX = synthetic[syntheticIndex].scene.depth.tanHalfFovX;
            activeDepth = synthetic[syntheticIndex].depth;
        }
        else if (liveDepthUsable)
        {
            tanHalfFovX = tanf(cameraFovDegrees*0.5f*PI/180.0f);
            activeDepth = liveDepth;
        }

        // Follow the source until the slider is touched, then leave it alone.
        if (reliefFollowsSource) depthRelief = (syntheticIndex >= 0) ? 1.0f : 0.30f;

        // The scene's own range drives the light distance slider, the depth
        // view mapping and the falloff scale.
        float sceneNear = 0.6f, sceneFar = 1.5f;
        if (syntheticIndex >= 0)
        {
            sceneNear = synthetic[syntheticIndex].scene.depth.nearMetres;
            sceneFar = synthetic[syntheticIndex].scene.depth.farMetres;
        }
        else if (liveDepthUsable) GetDepthRange(&sceneNear, &sceneFar);
        if (sceneFar <= sceneNear) sceneFar = sceneNear + 1.0f;

        // When the scene's scale changes materially - switching from a
        // synthetic head at 0.6 m to a room running to 8 m - a light distance
        // set for the old scale is useless. Put it just behind the nearest
        // surface, which is where the effect this branch exists for lives.
        if ((knownSceneNear <= 0.0f) ||
            (sceneNear > knownSceneNear*1.6f) || (sceneNear < knownSceneNear*0.625f))
        {
            knownSceneNear = sceneNear;
            depthLightDistance = sceneNear*1.15f;
            // Thickness and bias are metres as well, so they scale with the
            // scene. These factors reproduce the values hand-tuned against
            // the synthetic head, whose nearest surface is about 0.5 m.
            shadowThickness = sceneNear*0.68f;
            shadowBias = sceneNear*0.012f;
        }

        Rectangle quad = FitRectangle(area, albedoTexture.width, albedoTexture.height);
        float aspect = quad.width/quad.height;

        Vector2 mouse = GetMousePosition();
        if (CheckCollisionPointRec(mouse, quad))
        {
            if (!followBrightest && !demoEnabled)
            {
                lightU = (mouse.x - quad.x)/quad.width;
                lightV = (mouse.y - quad.y)/quad.height;
            }
            light.height += GetMouseWheelMove()*0.02f;
            if (light.height < 0.02f) light.height = 0.02f;
            if (light.height > 1.5f) light.height = 1.5f;
        }

        if (followBrightest && tracking && !demoEnabled)
        {
            float u = trackedPoint.x/(float)GetWebcamWidth();
            float v = trackedPoint.y/(float)GetWebcamHeight();
            if (mirrorWebcam) u = 1.0f - u;

            // Exponential smoothing: the tracked centroid is noisy frame to
            // frame, and an unsmoothed light visibly twitches.
            smoothedU += (u - smoothedU)*0.2f;
            smoothedV += (v - smoothedV)*0.2f;

            lightU = smoothedU;
            lightV = smoothedV;
        }

        // The user's slider values are left untouched by the demo, so turning
        // it off restores whatever was set by hand.
        float activeHeight = light.height;
        Color activeColor = light.color;

        demoTime += GetFrameTime();
        if (demoEnabled)
        {
            lightU = 0.5f + 0.40f*DriftValue(driftU, demoTime);
            lightV = 0.5f + 0.40f*DriftValue(driftV, demoTime);

            if (demoMode == 1)
            {
                // Kept low and capped at 0.25: a light close to the surface
                // rakes across the relief and is far more dramatic than a
                // high one, which flattens everything out.
                activeHeight = 0.135f + 0.115f*DriftValue(driftHeight, demoTime);

                // Hue drifts steadily and wraps, which stays visually
                // continuous because the hue wheel joins up at 360.
                float hue = fmodf(demoTime*7.0f + 70.0f*DriftValue(driftHue, demoTime), 360.0f);
                if (hue < 0.0f) hue += 360.0f;
                activeColor = ColorFromHSV(hue, 0.45f, 1.0f);
            }
        }

        // The shader lights in texture space. A mirrored feed reverses left and
        // right relative to what is on screen, so flip x on the way in.
        bool mirroredSource = (sourceMode == 1) && mirrorWebcam;
        float lightImageU = mirroredSource ? (1.0f - lightU) : lightU;

        float lightPos[3];
        if (activeGeometry == GEOMETRY_DEPTH)
        {
            // Camera space, metres. The light is placed inside the view
            // frustum at the chosen distance using the same unprojection the
            // shader uses, so pushing it back keeps it visually where it was
            // put and simply moves it past the subject.
            // The full demo drives the light's height, which in this mode is
            // its distance. Map its 0.02..0.25 sweep onto half to one and a
            // half times the nearest surface, so the light travels from in
            // front of the subject to behind them.
            float z = depthLightDistance;
            if (demoEnabled && (demoMode == 1))
            {
                float t = (activeHeight - 0.02f)/0.23f;
                z = sceneNear*(0.5f + 1.0f*t);
            }
            lightPos[0] = (lightImageU*2.0f - 1.0f)*tanHalfFovX*z;
            lightPos[1] = (lightV*2.0f - 1.0f)*(tanHalfFovX/aspect)*z;
            lightPos[2] = z;
        }
        else
        {
            lightPos[0] = lightImageU*aspect;
            lightPos[1] = lightV;
            lightPos[2] = activeHeight;
        }
        Vector4 colorNormalized = ColorNormalize(activeColor);
        float lightColor[3] = { colorNormalized.x, colorNormalized.y, colorNormalized.z };
        float texelSize[2] = { 1.0f/albedoTexture.width, 1.0f/albedoTexture.height };

        SetShaderValue(rs.shader, rs.lightPosLoc, lightPos, SHADER_UNIFORM_VEC3);
        SetShaderValue(rs.shader, rs.lightColorLoc, lightColor, SHADER_UNIFORM_VEC3);
        SetShaderValue(rs.shader, rs.lightIntensityLoc, &light.intensity, SHADER_UNIFORM_FLOAT);
        float activeFalloff = (activeGeometry == GEOMETRY_DEPTH) ? depthFalloff : light.falloff;
        SetShaderValue(rs.shader, rs.falloffLoc, &activeFalloff, SHADER_UNIFORM_FLOAT);
        SetShaderValue(rs.shader, rs.ambientLoc, &light.ambient, SHADER_UNIFORM_FLOAT);
        SetShaderValue(rs.shader, rs.specularStrengthLoc, &light.specularStrength, SHADER_UNIFORM_FLOAT);
        SetShaderValue(rs.shader, rs.shininessLoc, &light.shininess, SHADER_UNIFORM_FLOAT);
        SetShaderValue(rs.shader, rs.aspectLoc, &aspect, SHADER_UNIFORM_FLOAT);
        SetShaderValue(rs.shader, rs.viewModeLoc, &viewMode, SHADER_UNIFORM_INT);
        SetShaderValue(rs.shader, rs.texelSizeLoc, texelSize, SHADER_UNIFORM_VEC2);
        SetShaderValue(rs.shader, rs.reliefLoc, &relief, SHADER_UNIFORM_FLOAT);
        SetShaderValue(rs.shader, rs.sampleSpacingLoc, &sampleSpacing, SHADER_UNIFORM_FLOAT);
        SetShaderValue(rs.shader, rs.geometryModeLoc, &activeGeometry, SHADER_UNIFORM_INT);
        SetShaderValue(rs.shader, rs.tanHalfFovXLoc, &tanHalfFovX, SHADER_UNIFORM_FLOAT);
        SetShaderValue(rs.shader, rs.depthSpacingLoc, &depthSpacing, SHADER_UNIFORM_FLOAT);
        float depthRange[2] = { sceneNear, sceneFar };
        SetShaderValue(rs.shader, rs.depthRangeLoc, depthRange, SHADER_UNIFORM_VEC2);
        float distanceScale = (activeGeometry == GEOMETRY_DEPTH) ? sceneNear : 1.0f;
        SetShaderValue(rs.shader, rs.distanceScaleLoc, &distanceScale, SHADER_UNIFORM_FLOAT);
        // The depth map's own resolution, which is not the albedo's once the
        // depth comes from the model.
        float depthTexel[2] = { 1.0f/(float)activeDepth.width, 1.0f/(float)activeDepth.height };
        SetShaderValue(rs.shader, rs.depthTexelSizeLoc, depthTexel, SHADER_UNIFORM_VEC2);
        SetShaderValue(rs.shader, rs.depthReliefLoc, &depthRelief, SHADER_UNIFORM_FLOAT);
        // Only the live source has a mask; a synthetic scene is all subject.
        int maskEnabled = (liveDepthUsable && subjectOnly) ? 1 : 0;
        SetShaderValue(rs.shader, rs.maskEnabledLoc, &maskEnabled, SHADER_UNIFORM_INT);
        SetShaderValue(rs.shader, rs.maskStrengthLoc, &maskStrength, SHADER_UNIFORM_FLOAT);
        // Zero steps is how shadows are turned off, so the slider is both the
        // switch and the quality control and costs one panel row instead of two.
        int activeShadowSteps = (int)shadowSteps;
        SetShaderValue(rs.shader, rs.shadowStepsLoc, &activeShadowSteps, SHADER_UNIFORM_INT);
        SetShaderValue(rs.shader, rs.shadowBiasLoc, &shadowBias, SHADER_UNIFORM_FLOAT);
        SetShaderValue(rs.shader, rs.shadowThicknessLoc, &shadowThickness, SHADER_UNIFORM_FLOAT);

        // Render the relit image again, on its own, at the camera's resolution
        // and without any of the UI. Letterboxed so the source keeps its
        // aspect ratio, which also keeps the shader's own aspect uniform valid.
        if (exposeAsCamera && IsVirtualCameraRunning())
        {
            Rectangle cameraArea = { 0.0f, 0.0f, (float)cameraTarget.texture.width,
                                     (float)cameraTarget.texture.height };
            Rectangle cameraQuad = FitRectangle(cameraArea, albedoTexture.width, albedoTexture.height);
            Rectangle cameraSource = { 0.0f, 0.0f, (float)albedoTexture.width, (float)albedoTexture.height };
            if (mirroredSource) cameraSource.width = -cameraSource.width;

            BeginTextureMode(cameraTarget);
            ClearBackground(BLACK);
            BeginShaderMode(rs.shader);
            // Inside shader mode, not before: BeginShaderMode flushes the
            // batch and rlDrawRenderBatch clears the registered auxiliary
            // texture units, so an earlier binding is silently discarded.
            SetShaderValueTexture(rs.shader, rs.depthMapLoc, activeDepth);
            SetShaderValueTexture(rs.shader, rs.subjectMaskLoc,
                                  (liveMask.id != 0) ? liveMask : noDepth);
            DrawTexturePro(albedoTexture, cameraSource, cameraQuad, (Vector2){ 0.0f, 0.0f }, 0.0f, WHITE);
            EndShaderMode();
            EndTextureMode();

            // GPU readback. Render textures are bottom-up, and the camera wants
            // BGRA top-down, so flip and swap channels on the way out.
            Image shot = LoadImageFromTexture(cameraTarget.texture);
            if (shot.data != NULL)
            {
                ImageFlipVertical(&shot);
                const unsigned char *from = (const unsigned char *)shot.data;
                unsigned char *to = (unsigned char *)cameraFrame.data;
                int pixels = cameraFrame.width*cameraFrame.height;
                for (int i = 0; i < pixels; i++)
                {
                    to[i*4 + 0] = from[i*4 + 2];
                    to[i*4 + 1] = from[i*4 + 1];
                    to[i*4 + 2] = from[i*4 + 0];
                    to[i*4 + 3] = 255;
                }
                PublishVirtualCameraFrame(to, cameraFrame.width, cameraFrame.height);
                UnloadImage(shot);
            }
        }

        BeginDrawing();
        ClearBackground((Color){ 18, 18, 22, 255 });

        BeginShaderMode(rs.shader);
        SetShaderValueTexture(rs.shader, rs.depthMapLoc, activeDepth);
        SetShaderValueTexture(rs.shader, rs.subjectMaskLoc,
                              (liveMask.id != 0) ? liveMask : noDepth);
        Rectangle albedoSource = { 0.0f, 0.0f, (float)albedoTexture.width, (float)albedoTexture.height };
        // Mirroring the live feed makes it behave like a mirror, which is what
        // you want when you are the subject.
        if (mirroredSource) albedoSource.width = -albedoSource.width;
        DrawTexturePro(albedoTexture, albedoSource, quad, (Vector2){ 0.0f, 0.0f }, 0.0f, WHITE);
        EndShaderMode();

        float panelX = screenW - panelWidth;
        GuiPanel((Rectangle){ panelX, 0.0f, panelWidth, screenH }, "Controls");

        // An open dropdown must paint over whatever is below it, so its rect is
        // reserved during layout and it is drawn last. Everything else is
        // locked meanwhile so clicks cannot land on controls beneath the list.
        Rectangle deviceRect = { 0 };
        Rectangle modeRect = { 0 };
        bool showDeviceDropdown = false;
        if (deviceDropdownOpen || modeDropdownOpen) GuiLock();

        const float pad = 12.0f*uiScale;
        const float labelX = panelX + pad;
        const float labelW = panelWidth - pad*2.0f;
        const float sliderX = panelX + 110.0f*uiScale;
        const float sliderW = 130.0f*uiScale;
        const float rowH = 20.0f*uiScale;
        const float rowStep = 26.0f*uiScale;
        const float labelStep = 22.0f*uiScale;
        const float groupStep = 34.0f*uiScale;
        float y = 40.0f*uiScale;

        GuiLabel((Rectangle){ labelX, y, labelW, rowH }, "Source");
        y += labelStep;
        // A 2x2 grid: raygui splits the label on ';' for columns and on a
        // newline escape for rows, so the four sources map straight onto
        // sourceMode 0..3.
        GuiToggleGroup((Rectangle){ labelX, y, 143.0f*uiScale, 22.0f*uiScale },
                       "Photo;Webcam feed\nTest: sphere;Test: head", &sourceMode);
        y += groupStep + 22.0f*uiScale;   // two rows of toggles, not one
        if (!webcamReady && (sourceMode == 1))
            GuiLabel((Rectangle){ labelX, y, labelW, rowH }, "no webcam available");

        GuiLabel((Rectangle){ labelX, y, labelW, rowH }, "Geometry");
        y += labelStep;
        // Depth needs a depth map. The photo and the webcam have none, so the
        // choice is shown but disabled rather than hidden, which makes it
        // obvious that the mode exists and why it is unavailable.
        if (!depthAvailable) GuiSetState(STATE_DISABLED);
        GuiToggleGroup((Rectangle){ labelX, y, 143.0f*uiScale, 22.0f*uiScale },
                       "Luminance;Depth", &geometryMode);
        if (!depthAvailable) GuiSetState(STATE_NORMAL);
        y += groupStep;

        if (activeGeometry == GEOMETRY_DEPTH)
        {
            if (liveDepthUsable)
            {
                GuiCheckBox((Rectangle){ labelX, y, rowH, rowH }, "Subject only", &subjectOnly);
                y += rowStep;
                if (!subjectOnly) GuiSetState(STATE_DISABLED);
                float beforeBand = subjectBand;
                GuiSliderBar((Rectangle){ sliderX, y, sliderW, rowH }, "Subject depth",
                             TextFormat("%.2f m", subjectBand), &subjectBand, 0.2f, 3.0f);
                if (subjectBand != beforeBand) SetSubjectBand(subjectBand);
                y += rowStep;
                GuiSliderBar((Rectangle){ sliderX, y, sliderW, rowH }, "Spill",
                             TextFormat("%.2f", maskStrength), &maskStrength, 0.0f, 1.0f);
                GuiSetState(STATE_NORMAL);
                y += rowStep;
                GuiSliderBar((Rectangle){ sliderX, y, sliderW, rowH }, "Camera FOV",
                             TextFormat("%.0f deg", cameraFovDegrees),
                             &cameraFovDegrees, 40.0f, 110.0f);
                y += rowStep;
            }
            float beforeRelief = depthRelief;
            GuiSliderBar((Rectangle){ sliderX, y, sliderW, rowH }, "Relief",
                         TextFormat("%.2f", depthRelief), &depthRelief, 0.02f, 1.0f);
            if (depthRelief != beforeRelief) reliefFollowsSource = false;
            y += rowStep;
            GuiSliderBar((Rectangle){ sliderX, y, sliderW, rowH }, "Depth taps",
                         TextFormat("%.0f", depthSpacing), &depthSpacing, 1.0f, 6.0f);
            y += rowStep;
            GuiSliderBar((Rectangle){ sliderX, y, sliderW, rowH }, "Shadow steps",
                         (shadowSteps < 1.0f) ? "off" : TextFormat("%.0f", shadowSteps),
                         &shadowSteps, 0.0f, 64.0f);
            y += rowStep;
            if (shadowSteps < 1.0f) GuiSetState(STATE_DISABLED);
            GuiSliderBar((Rectangle){ sliderX, y, sliderW, rowH }, "Thickness",
                         TextFormat("%.2f m", shadowThickness), &shadowThickness,
                         sceneNear*0.1f, sceneNear*2.5f);
            y += rowStep;
            GuiSliderBar((Rectangle){ sliderX, y, sliderW, rowH }, "Bias",
                         TextFormat("%.0f mm", shadowBias*1000.0f), &shadowBias,
                         sceneNear*0.002f, sceneNear*0.06f);
            y += rowStep;
            GuiSetState(STATE_NORMAL);
        }

        // Full demo drives height and colour itself, so those controls become
        // live read-outs rather than inputs while it runs.
        bool demoDrivesLook = demoEnabled && (demoMode == 1);

        GuiCheckBox((Rectangle){ labelX, y, 18.0f*uiScale, 18.0f*uiScale }, "Demo", &demoEnabled);
        if (!demoEnabled) GuiSetState(STATE_DISABLED);
        GuiToggleGroup((Rectangle){ labelX + 100.0f*uiScale, y - 2.0f*uiScale, 95.0f*uiScale, 22.0f*uiScale },
                       "Basic;Full", &demoMode);
        GuiSetState(STATE_NORMAL);
        y += 30.0f*uiScale;

        GuiSliderBar((Rectangle){ sliderX, y, sliderW, rowH }, "Intensity",
                     TextFormat("%.2f", light.intensity), &light.intensity, 0.0f, 6.0f);
        y += rowStep;
        GuiSliderBar((Rectangle){ sliderX, y, sliderW, rowH }, "Falloff",
                     TextFormat("%.1f", activeFalloff),
                     (activeGeometry == GEOMETRY_DEPTH) ? &depthFalloff : &light.falloff, 0.0f, 30.0f);
        y += rowStep;
        GuiSliderBar((Rectangle){ sliderX, y, sliderW, rowH }, "Ambient",
                     TextFormat("%.2f", light.ambient), &light.ambient, 0.0f, 1.0f);
        y += rowStep;
        GuiSliderBar((Rectangle){ sliderX, y, sliderW, rowH }, "Specular",
                     TextFormat("%.2f", light.specularStrength), &light.specularStrength, 0.0f, 2.0f);
        y += rowStep;
        GuiSliderBar((Rectangle){ sliderX, y, sliderW, rowH }, "Shininess",
                     TextFormat("%.0f", light.shininess), &light.shininess, 1.0f, 200.0f);
        y += rowStep;
        float shownHeight = activeHeight;
        if (demoDrivesLook) GuiSetState(STATE_DISABLED);
        // Same slider, different meaning: in depth mode it is the light's
        // distance from the camera in metres, so pushing it past the subject
        // is what puts the light behind them.
        if (activeGeometry == GEOMETRY_DEPTH)
        {
            // Metres, spanning the scene the depth source reports, so the
            // light can always be put in front of, level with, or behind the
            // subject whatever the scale.
            float shownDistance = depthLightDistance;
            GuiSliderBar((Rectangle){ sliderX, y, sliderW, rowH }, "Distance",
                         TextFormat("%.2f m", shownDistance),
                         demoDrivesLook ? &shownDistance : &depthLightDistance,
                         0.05f, sceneFar*1.1f);
        }
        else
        {
            GuiSliderBar((Rectangle){ sliderX, y, sliderW, rowH }, "Height",
                         TextFormat("%.2f", shownHeight),
                         demoDrivesLook ? &shownHeight : &light.height, 0.02f, 1.5f);
        }
        GuiSetState(STATE_NORMAL);
        y += groupStep;

        // Relief and smoothing shape the luminance cheat, so they mean nothing
        // under the depth model. Hiding them there also buys back the panel
        // height the shadow controls need.
        if (activeGeometry == GEOMETRY_LUMINANCE)
        {
            GuiLabel((Rectangle){ labelX, y, labelW, rowH }, "Derived relief");
            y += labelStep;
            GuiSliderBar((Rectangle){ sliderX, y, sliderW, rowH }, "Relief",
                         TextFormat("%.1f", relief), &relief, 0.0f, 20.0f);
            y += rowStep;
            GuiSliderBar((Rectangle){ sliderX, y, sliderW, rowH }, "Smoothing",
                         TextFormat("%.1f", sampleSpacing), &sampleSpacing, 1.0f, 12.0f);
            y += groupStep;
        }

        GuiLabel((Rectangle){ labelX, y, labelW, rowH }, "Light colour");
        // A live swatch, because raygui greys the picker out while the demo
        // drives it and the disabled styling hides the actual colour.
        Rectangle swatch = { labelX + labelW - 22.0f*uiScale, y, 18.0f*uiScale, 18.0f*uiScale };
        DrawRectangleRec(swatch, activeColor);
        DrawRectangleLinesEx(swatch, uiScale, (Color){ 90, 90, 100, 255 });
        y += labelStep;
        Color shownColor = activeColor;
        if (demoDrivesLook) GuiSetState(STATE_DISABLED);
        GuiColorPicker((Rectangle){ labelX, y, 120.0f*uiScale, 120.0f*uiScale }, NULL,
                       demoDrivesLook ? &shownColor : &light.color);
        GuiSetState(STATE_NORMAL);
        y += 132.0f*uiScale;

        if (webcamSystemReady && (GetWebcamDeviceCount() > 0))
        {
            GuiLabel((Rectangle){ labelX, y, labelW, rowH },
                     webcamReady ? TextFormat("Webcam  %ix%i", GetWebcamWidth(), GetWebcamHeight())
                                 : TextFormat("Webcam: %s", GetWebcamStatus()));
            y += labelStep;

            deviceRect = (Rectangle){ labelX, y, labelW, 24.0f*uiScale };
            y += 30.0f*uiScale;
            modeRect = (Rectangle){ labelX, y, labelW, 24.0f*uiScale };
            showDeviceDropdown = true;
            y += 32.0f*uiScale;
        }
        else
        {
            GuiLabel((Rectangle){ labelX, y, labelW, rowH },
                     TextFormat("Webcam: %s", GetWebcamStatus()));
            y += labelStep;
        }

        if (webcamReady)
        {
            GuiCheckBox((Rectangle){ labelX, y, 18.0f*uiScale, 18.0f*uiScale }, "Light follows brightest", &followBrightest);
            y += 24.0f*uiScale;
            GuiCheckBox((Rectangle){ labelX, y, 18.0f*uiScale, 18.0f*uiScale }, "Mirror", &mirrorWebcam);
            y += rowStep;

            // No preview thumbnail: the main view already shows the feed, so a
            // second copy of it only competed for panel space.
            if (followBrightest)
            {
                GuiLabel((Rectangle){ labelX, y, labelW, rowH },
                         tracking ? TextFormat("tracking %.0f, %.0f", trackedPoint.x, trackedPoint.y)
                                  : "no bright spot found");
                y += labelStep;
            }
        }

        // Virtual camera. Deliberately last in the panel and off by default:
        // ticking it makes normalish appear in everyone's camera list.
        y += 8.0f*uiScale;
        bool wasExposed = exposeAsCamera;
        if (IsVirtualCameraSupported() && IsVirtualCameraRegistered())
        {
            GuiCheckBox((Rectangle){ labelX, y, 18.0f*uiScale, 18.0f*uiScale },
                        "Expose as virtual webcam", &exposeAsCamera);
        }
        else
        {
            GuiSetState(STATE_DISABLED);
            GuiCheckBox((Rectangle){ labelX, y, 18.0f*uiScale, 18.0f*uiScale },
                        "Expose as virtual webcam", &exposeAsCamera);
            GuiSetState(STATE_NORMAL);
            exposeAsCamera = false;
        }
        y += 22.0f*uiScale;
        GuiLabel((Rectangle){ labelX, y, labelW, rowH },
                 TextFormat("camera: %s", GetVirtualCameraStatus()));

        if (exposeAsCamera != wasExposed)
        {
            if (exposeAsCamera) exposeAsCamera = StartVirtualCamera();
            else StopVirtualCamera();
        }

        GuiLabel((Rectangle){ labelX, screenH - 52.0f*uiScale, labelW, rowH },
                 TextFormat("1-5 view (%s)   R reload",
                            (viewMode == 0) ? "lit" : ((viewMode == 1) ? "normals"
                          : ((viewMode == 2) ? "albedo"
                          : ((viewMode == 3) ? "depth" : "subject")))));
        DrawText(TextFormat("%i FPS", GetFPS()), (int)labelX, (int)(screenH - 30.0f*uiScale),
                 (int)(14.0f*uiScale), GREEN);

        GuiUnlock();
        if (showDeviceDropdown)
        {
            // Resolution first, then device: an open device list expands down
            // over the resolution box, so the device box must be painted last.
            // Only one can be open at a time, so opening one closes the other.
            if (GuiDropdownBox(modeRect, captureModeList, &selectedMode, modeDropdownOpen))
            {
                modeDropdownOpen = !modeDropdownOpen;
                deviceDropdownOpen = false;
            }
            if (GuiDropdownBox(deviceRect, deviceListText, &selectedDevice, deviceDropdownOpen))
            {
                deviceDropdownOpen = !deviceDropdownOpen;
                modeDropdownOpen = false;
            }
        }

        EndDrawing();

        if (webcamSystemReady && ((selectedDevice != openedDevice) || (selectedMode != openedMode)))
        {
            tracking = false;
            webcamReady = ReopenWebcam(selectedDevice, selectedMode, &webcamFrame, &webcamTexture);
            openedDevice = selectedDevice;
            openedMode = selectedMode;
            if (!webcamReady) sourceMode = 0;
        }
    }

    if (webcamReady)
    {
        UnloadTexture(webcamTexture);
        UnloadImage(webcamFrame);
    }
    StopVirtualCamera();
    UnloadRenderTexture(cameraTarget);
    UnloadImage(cameraFrame);
    if (webcamSystemReady) ShutdownWebcamSystem();
    UnloadShader(rs.shader);
    UnloadTexture(photoTexture);
    ShutdownDepthModel();
    if (liveDepth.id != 0) UnloadTexture(liveDepth);
    UnloadTexture(noDepth);
    UnloadSyntheticSource(&synthetic[0]);
    UnloadSyntheticSource(&synthetic[1]);
    CloseWindow();
    return 0;
}
