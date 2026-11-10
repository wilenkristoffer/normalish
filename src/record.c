// Guided recording of test clips.
//
// The look of the relight cannot be judged against a live camera - the scene
// changes between runs - so every later comparison is made on footage captured
// once, here. The session walks through a fixed list of poses in one window,
// with the instruction on screen, so the person being recorded always knows
// what is being captured and when.

#include "record.h"

#include "raylib.h"
#include "webcam.h"
#include "clip.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct ClipSpec {
    const char *name;
    const char *title;
    const char *line1;
    const char *line2;
    float countdown;
    float seconds;
} ClipSpec;

static const ClipSpec kClips[] = {
    { "1-empty", "1 of 6: EMPTY ROOM",
      "Leave the frame completely. Leave your chair where it normally sits.",
      "You get 10 seconds to walk out. Come back after the double beep.",
      10.0f, 8.0f },
    { "2-still", "2 of 6: SITTING STILL",
      "Sit as you would in a call and look at the camera.",
      "Keep as still as you comfortably can.",
      5.0f, 8.0f },
    { "3-head", "3 of 6: TURNING YOUR HEAD",
      "Slowly turn your head left, then right, then back to centre.",
      "Finish with one nod.",
      5.0f, 8.0f },
    { "4-wave", "4 of 6: WAVING A HAND",
      "Wave in front of your chest, then in front of your face,",
      "then out to the side past your shoulder.",
      5.0f, 8.0f },
    { "5-lean", "5 of 6: LEANING",
      "Slowly lean in toward the camera, then lean back.",
      "Once or twice is enough.",
      5.0f, 8.0f },
    { "6-leave", "6 of 6: LEAVING AND RETURNING",
      "Stand up and walk out of frame. Count to three out of sight,",
      "then come back and sit down. Recording runs 14 seconds.",
      5.0f, 14.0f },
};
#define CLIP_COUNT ((int)(sizeof(kClips)/sizeof(kClips[0])))

// Frames are stored at most this often. A camera delivering 60 fps would
// otherwise fill the buffer in half the time.
#define STORE_FPS 30.0

typedef enum Stage { STAGE_WAIT, STAGE_COUNTDOWN, STAGE_RECORDING, STAGE_SAVING, STAGE_DONE } Stage;

// A short sine tone, so someone standing out of frame can hear when a
// recording starts and stops.
static Sound MakeBeep(float hz, float seconds)
{
    int rate = 22050;
    int count = (int)(rate*seconds);
    short *samples = (short *)MemAlloc((unsigned int)(count*sizeof(short)));
    for (int i = 0; i < count; i++)
    {
        float t = (float)i/(float)rate;
        // Fade both ends so the tone does not click.
        float envelope = fminf(1.0f, fminf(t, seconds - t)*80.0f);
        samples[i] = (short)(9000.0f*envelope*sinf(6.2831853f*hz*t));
    }
    Wave wave = { .frameCount = (unsigned int)count, .sampleRate = (unsigned int)rate,
                  .sampleSize = 16, .channels = 1, .data = samples };
    Sound sound = LoadSoundFromWave(wave);
    UnloadWave(wave);
    return sound;
}

int RunRecordSession(const char *directory, int captureWidth, int captureHeight)
{
    if (!InitWebcamSystem()) { printf("RECORD: no Media Foundation\n"); return 1; }
    if (!OpenWebcam(0, captureWidth, captureHeight))
    {
        printf("RECORD: could not open the camera: %s\n", GetWebcamStatus());
        ShutdownWebcamSystem();
        return 1;
    }

    int w = GetWebcamWidth();
    int h = GetWebcamHeight();
    size_t rgbBytes = (size_t)w*h*3;
    printf("RECORD: camera delivers %dx%d\n", w, h);

    float longest = 0.0f;
    for (int i = 0; i < CLIP_COUNT; i++) if (kClips[i].seconds > longest) longest = kClips[i].seconds;
    int capacity = (int)(longest*STORE_FPS) + 16;

    Image frame = GenImageColor(w, h, BLACK);
    Texture2D texture = LoadTextureFromImage(frame);
    SetTextureFilter(texture, TEXTURE_FILTER_BILINEAR);

    // Held in memory while recording and written afterwards: writing 190 MB a
    // second to disk from the capture loop would drop frames.
    unsigned char *store = (unsigned char *)malloc((size_t)capacity*rgbBytes);
    double *times = (double *)malloc((size_t)capacity*sizeof(double));
    if ((store == NULL) || (times == NULL))
    {
        printf("RECORD: could not allocate %.1f GB for the frame buffer\n",
               (double)capacity*(double)rgbBytes/1.0e9);
        return 1;
    }

    if (!DirectoryExists(directory)) MakeDirectory(directory);

    InitAudioDevice();
    Sound beep = MakeBeep(880.0f, 0.18f);
    Sound beepLow = MakeBeep(520.0f, 0.18f);

    // Poll faster than the camera delivers, so frame timestamps are not
    // quantised to a 60 Hz render loop.
    SetTargetFPS(120);

    Vector2 dpi = GetWindowScaleDPI();
    float ui = (dpi.x > 1.0f) ? dpi.x : 1.0f;

    int current = 0;
    Stage stage = STAGE_WAIT;
    double stageStart = GetTime();
    int stored = 0;
    double firstFrameTime = 0.0;
    double lastStoredTime = 0.0;
    int secondBeepPending = 0;
    double secondBeepAt = 0.0;
    char result[256] = { 0 };
    int recorded = 0;

    while (!WindowShouldClose() && (current < CLIP_COUNT))
    {
        const ClipSpec *spec = &kClips[current];
        double now = GetTime();
        float elapsed = (float)(now - stageStart);

        bool fresh = UpdateWebcamFrame((unsigned char *)frame.data);
        if (fresh) UpdateTexture(texture, frame.data);

        if (secondBeepPending && (now >= secondBeepAt))
        {
            PlaySound(beepLow);
            secondBeepPending = 0;
        }

        if (stage == STAGE_WAIT)
        {
            if (IsKeyPressed(KEY_SPACE)) { stage = STAGE_COUNTDOWN; stageStart = now; }
            else if (IsKeyPressed(KEY_S)) { current++; stageStart = now; }
        }
        else if (stage == STAGE_COUNTDOWN)
        {
            if (elapsed >= spec->countdown)
            {
                stage = STAGE_RECORDING;
                stageStart = now;
                stored = 0;
                PlaySound(beep);
            }
        }
        else if (stage == STAGE_RECORDING)
        {
            if (fresh && (stored < capacity) &&
                ((stored == 0) || ((now - lastStoredTime) >= (1.0/STORE_FPS)*0.6)))
            {
                if (stored == 0) firstFrameTime = now;
                const unsigned char *from = (const unsigned char *)frame.data;
                unsigned char *to = store + (size_t)stored*rgbBytes;
                size_t pixels = (size_t)w*h;
                for (size_t i = 0; i < pixels; i++)
                {
                    to[i*3 + 0] = from[i*4 + 0];
                    to[i*3 + 1] = from[i*4 + 1];
                    to[i*3 + 2] = from[i*4 + 2];
                }
                times[stored] = now - firstFrameTime;
                lastStoredTime = now;
                stored++;
            }
            if ((elapsed >= spec->seconds) || (stored >= capacity))
            {
                stage = STAGE_SAVING;
                stageStart = now;
                PlaySound(beepLow);
                secondBeepPending = 1;
                secondBeepAt = now + 0.3;
            }
        }
        else if (stage == STAGE_DONE)
        {
            if (IsKeyPressed(KEY_SPACE)) { current++; stage = STAGE_WAIT; stageStart = now; }
            else if (IsKeyPressed(KEY_R)) { stage = STAGE_WAIT; stageStart = now; }
        }

        // --- draw ---
        float sw = (float)GetScreenWidth();
        float sh = (float)GetScreenHeight();

        BeginDrawing();
        ClearBackground((Color){ 18, 18, 22, 255 });

        float scale = fminf(sw/(float)w, sh/(float)h);
        Rectangle dest = { (sw - w*scale)*0.5f, (sh - h*scale)*0.5f, w*scale, h*scale };
        // Mirrored, so moving left moves your image left.
        DrawTexturePro(texture, (Rectangle){ 0.0f, 0.0f, -(float)w, (float)h }, dest,
                       (Vector2){ 0.0f, 0.0f }, 0.0f, WHITE);

        int titleSize = (int)(30.0f*ui);
        int textSize = (int)(20.0f*ui);
        float bandH = 150.0f*ui;
        DrawRectangle(0, 0, (int)sw, (int)bandH, (Color){ 0, 0, 0, 190 });
        DrawText(spec->title, (int)(20.0f*ui), (int)(14.0f*ui), titleSize, RAYWHITE);
        DrawText(spec->line1, (int)(20.0f*ui), (int)(56.0f*ui), textSize, (Color){ 220, 220, 220, 255 });
        DrawText(spec->line2, (int)(20.0f*ui), (int)(82.0f*ui), textSize, (Color){ 220, 220, 220, 255 });

        const char *status = "";
        Color statusColor = YELLOW;
        if (stage == STAGE_WAIT)
            status = "Press SPACE when you are ready. S skips this clip, Esc quits.";
        else if (stage == STAGE_COUNTDOWN)
        {
            status = TextFormat("Recording starts in %d ...", (int)ceilf(spec->countdown - elapsed));
            statusColor = ORANGE;
        }
        else if (stage == STAGE_RECORDING)
        {
            status = TextFormat("RECORDING  %.0f s left", ceilf(spec->seconds - elapsed));
            statusColor = RED;
        }
        else if (stage == STAGE_SAVING) status = "Saving to disk, a few seconds ...";
        else if (stage == STAGE_DONE)
        {
            status = (current + 1 < CLIP_COUNT)
                   ? "Saved. SPACE for the next clip, R to redo this one."
                   : "Saved. That was the last one: SPACE to finish, R to redo.";
            statusColor = GREEN;
        }
        DrawText(status, (int)(20.0f*ui), (int)(116.0f*ui), textSize, statusColor);

        if (stage == STAGE_COUNTDOWN)
        {
            const char *digit = TextFormat("%d", (int)ceilf(spec->countdown - elapsed));
            int size = (int)(220.0f*ui);
            DrawText(digit, (int)((sw - MeasureText(digit, size))*0.5f), (int)(sh*0.5f - size*0.4f),
                     size, (Color){ 255, 161, 0, 220 });
        }
        if (stage == STAGE_RECORDING)
        {
            DrawCircle((int)(sw - 40.0f*ui), (int)(40.0f*ui), 14.0f*ui, RED);
            float t = elapsed/spec->seconds;
            DrawRectangle(0, (int)(sh - 10.0f*ui), (int)(sw*t), (int)(10.0f*ui), RED);
        }
        if ((stage == STAGE_DONE) && (result[0] != 0))
            DrawText(result, (int)(20.0f*ui), (int)(sh - 34.0f*ui), (int)(16.0f*ui), LIGHTGRAY);

        EndDrawing();

        // Saving blocks, so it happens after one frame has been drawn saying so.
        if ((stage == STAGE_SAVING) && (elapsed > 0.05f))
        {
            const char *path = TextFormat("%s/%s.nclip", directory, spec->name);
            bool ok = (stored > 0) && SaveClip(path, w, h, stored, times, store);
            double span = (stored > 1) ? times[stored - 1] : 0.0;
            double fps = (span > 0.0) ? (double)(stored - 1)/span : 0.0;

            if (ok)
            {
                // The middle frame as a PNG, so the clip can be checked at a
                // glance without a player for the raw format.
                Image mid = { .data = store + (size_t)(stored/2)*rgbBytes, .width = w, .height = h,
                              .mipmaps = 1, .format = PIXELFORMAT_UNCOMPRESSED_R8G8B8 };
                ExportImage(mid, TextFormat("%s/%s-mid.png", directory, spec->name));
                recorded++;
            }

            snprintf(result, sizeof(result), "%s: %d frames, %.1f s, %.1f fps, %.2f GB%s",
                     spec->name, stored, span, fps, (double)stored*(double)rgbBytes/1.0e9,
                     ok ? "" : "  ** SAVE FAILED **");
            printf("RECORD: %s\n", result);
            fflush(stdout);

            stage = STAGE_DONE;
            stageStart = GetTime();
        }
    }

    printf("RECORD: %d clip(s) written to %s\n", recorded, directory);

    UnloadSound(beep);
    UnloadSound(beepLow);
    CloseAudioDevice();
    free(store);
    free(times);
    UnloadTexture(texture);
    UnloadImage(frame);
    CloseWebcam();
    ShutdownWebcamSystem();
    return 0;
}
