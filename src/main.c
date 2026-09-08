#include "raylib.h"
#include "webcam.h"

#include <math.h>

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

int main(int argc, char **argv)
{
    const char *photoPath = (argc > 1) ? argv[1] : ASSET_DIR "/portrait.png";

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
    int selectedMode = 0;          // index into captureWidths/captureHeights
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

    // 0 = still photo, 1 = live webcam feed.
    int sourceMode = webcamReady ? 1 : 0;

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

        if (webcamReady && UpdateWebcamFrame((unsigned char *)webcamFrame.data))
        {
            UpdateTexture(webcamTexture, webcamFrame.data);
            Vector2 found = { 0 };
            tracking = TrackBrightestPoint(webcamFrame, 4, &found);
            if (tracking) trackedPoint = found;
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

        if (!webcamReady) sourceMode = 0;
        Texture2D albedoTexture = (sourceMode == 1) ? webcamTexture : photoTexture;
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
                activeHeight = 0.30f + 0.24f*DriftValue(driftHeight, demoTime);

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
        float surfaceX = lightU*aspect;
        if (mirroredSource) surfaceX = aspect - surfaceX;

        float lightPos[3] = { surfaceX, lightV, activeHeight };
        Vector4 colorNormalized = ColorNormalize(activeColor);
        float lightColor[3] = { colorNormalized.x, colorNormalized.y, colorNormalized.z };
        float texelSize[2] = { 1.0f/albedoTexture.width, 1.0f/albedoTexture.height };

        SetShaderValue(rs.shader, rs.lightPosLoc, lightPos, SHADER_UNIFORM_VEC3);
        SetShaderValue(rs.shader, rs.lightColorLoc, lightColor, SHADER_UNIFORM_VEC3);
        SetShaderValue(rs.shader, rs.lightIntensityLoc, &light.intensity, SHADER_UNIFORM_FLOAT);
        SetShaderValue(rs.shader, rs.falloffLoc, &light.falloff, SHADER_UNIFORM_FLOAT);
        SetShaderValue(rs.shader, rs.ambientLoc, &light.ambient, SHADER_UNIFORM_FLOAT);
        SetShaderValue(rs.shader, rs.specularStrengthLoc, &light.specularStrength, SHADER_UNIFORM_FLOAT);
        SetShaderValue(rs.shader, rs.shininessLoc, &light.shininess, SHADER_UNIFORM_FLOAT);
        SetShaderValue(rs.shader, rs.aspectLoc, &aspect, SHADER_UNIFORM_FLOAT);
        SetShaderValue(rs.shader, rs.viewModeLoc, &viewMode, SHADER_UNIFORM_INT);
        SetShaderValue(rs.shader, rs.texelSizeLoc, texelSize, SHADER_UNIFORM_VEC2);
        SetShaderValue(rs.shader, rs.reliefLoc, &relief, SHADER_UNIFORM_FLOAT);
        SetShaderValue(rs.shader, rs.sampleSpacingLoc, &sampleSpacing, SHADER_UNIFORM_FLOAT);

        BeginDrawing();
        ClearBackground((Color){ 18, 18, 22, 255 });

        BeginShaderMode(rs.shader);
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

        GuiLabel((Rectangle){ labelX, y, labelW, rowH }, "Relight");
        y += labelStep;
        if (webcamReady) GuiToggleGroup((Rectangle){ labelX, y, 143.0f*uiScale, 22.0f*uiScale }, "Photo;Webcam feed", &sourceMode);
        else GuiLabel((Rectangle){ labelX, y, labelW, rowH }, "photo (no webcam)");
        y += groupStep;

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
                     TextFormat("%.1f", light.falloff), &light.falloff, 0.0f, 30.0f);
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
        GuiSliderBar((Rectangle){ sliderX, y, sliderW, rowH }, "Height",
                     TextFormat("%.2f", shownHeight),
                     demoDrivesLook ? &shownHeight : &light.height, 0.02f, 1.5f);
        GuiSetState(STATE_NORMAL);
        y += groupStep;

        GuiLabel((Rectangle){ labelX, y, labelW, rowH }, "Derived relief");
        y += labelStep;
        GuiSliderBar((Rectangle){ sliderX, y, sliderW, rowH }, "Relief",
                     TextFormat("%.1f", relief), &relief, 0.0f, 20.0f);
        y += rowStep;
        GuiSliderBar((Rectangle){ sliderX, y, sliderW, rowH }, "Smoothing",
                     TextFormat("%.1f", sampleSpacing), &sampleSpacing, 1.0f, 12.0f);
        y += groupStep;

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

            float previewW = 130.0f*uiScale;
            Rectangle preview = { labelX, y, previewW, previewW*GetWebcamHeight()/GetWebcamWidth() };
            Rectangle previewSource = { 0.0f, 0.0f, (float)webcamTexture.width, (float)webcamTexture.height };
            if (mirrorWebcam) previewSource.width = -previewSource.width;
            DrawTexturePro(webcamTexture, previewSource, preview, (Vector2){ 0.0f, 0.0f }, 0.0f, WHITE);
            DrawRectangleLinesEx(preview, uiScale, (Color){ 90, 90, 100, 255 });

            if (followBrightest && tracking)
            {
                float u = trackedPoint.x/(float)GetWebcamWidth();
                if (mirrorWebcam) u = 1.0f - u;
                float px = preview.x + u*preview.width;
                float py = preview.y + (trackedPoint.y/GetWebcamHeight())*preview.height;
                DrawCircleLines((int)px, (int)py, 8.0f*uiScale, GREEN);
            }
        }

        GuiLabel((Rectangle){ labelX, screenH - 52.0f*uiScale, labelW, rowH },
                 TextFormat("1/2/3 view (%s)   R reload",
                            viewMode == 0 ? "lit" : (viewMode == 1 ? "normals" : "albedo")));
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
    if (webcamSystemReady) ShutdownWebcamSystem();
    UnloadShader(rs.shader);
    UnloadTexture(photoTexture);
    CloseWindow();
    return 0;
}
