# Implementation Plan

Phased so each phase produces something runnable/visible before moving on.
Check off phases as they're completed; adjust scope as we learn.

## Phase 0 - Environment setup (DONE)
Goal: prove the toolchain works before writing any real code.

Toolchain decision changed during this phase: MSVC + CMake + Ninja were
already installed (Visual Studio Build Tools 2026), so w64devkit and a plain
Makefile were dropped. Nothing had to be downloaded or installed.

- [x] Confirm a working C compiler: MSVC cl.exe 19.50, plus CMake 4.1.1 and
      Ninja 1.12.1 bundled with Build Tools.
- [x] Get raylib: CMake FetchContent, pinned to tag 5.5, built from source
      as a static lib.
- [x] `build.ps1` wrapper that loads the MSVC environment itself, so an
      ordinary PowerShell prompt is enough.
- [x] Build and run a minimal raylib window (`src/main.c`).
- [x] Confirm iteration speed: incremental builds are a couple of seconds
      (only the first build pays the raylib compile cost).

Exit criteria met: window opens, renders at 60 FPS, closes cleanly.
Confirmed at runtime: OpenGL 3.3 Core / GLSL 4.60 on an RX 7800 XT, so
shaders will target `#version 330`.

## Phase 1 - Shader pipeline basics (DONE)
Goal: learn raylib's shader API and prove a movable light works, before
touching any photo.

- [x] Load a custom fragment shader (`shaders/lighting.fs`) via `LoadShader`
      with raylib's default vertex shader.
- [x] Apply it to a textured quad (procedural checker texture as albedo,
      drawn with `BeginShaderMode` + `DrawTexturePro`).
- [x] Pass light position as a `vec3` uniform, updated from the mouse each
      frame; mouse wheel controls light height above the surface.
- [x] Blinn-Phong (ambient + diffuse + specular + distance falloff) against
      a procedural egg-carton normal, differentiated analytically in the
      shader.
- [x] Debug view modes (keys 1/2/3: lit / normals / albedo) - these carry
      over to Phase 2/3 for checking the generated normal map.
- [x] Shader hot reload (key R), so tuning does not need a rebuild.

Exit criteria met: verified on screen that moving the mouse moves the lit
hotspot with correct per-bump directional shading, and the normals debug
view renders the expected RGB encoding.

Conventions established here (worth keeping in Phase 3):
- Lighting math works in "surface units": x spans 0..aspect, y spans 0..1,
  z is height above the surface plane. This keeps the falloff circular on a
  non-square image.
- The viewer is treated as orthographic and straight-on, `V = (0, 0, 1)`.
- Look tunables (light color, intensity, falloff, ambient, shininess, bump
  strength) are `const` in the shader rather than uniforms, so they can be
  tuned with hot reload. Promote them to uniforms in Phase 4 when there is
  UI for them.

## Phase 2 - Normal map generation from a photo (DONE)
Goal: turn one of your photos into a normal map.

- [x] Convert photo to luminance (Rec. 709 weights), treat as a heightmap.
- [x] Gaussian blur before differentiating, otherwise pore/noise/JPEG detail
      buries the actual shape.
- [x] Sobel filter for X/Y gradients.
- [x] Encode into an RGB normal map.
- [x] Compared strength/blur settings on the real photo; strength 4 with a
      blur of 2 gave the best structure on a 500x500 image.

Exit criteria met: the generated map reads as a plausible relief map - nose,
brows, cheeks and lips all legible, mostly z-dominant with the expected rim
at the silhouette.

Prototyped as a Python script (numpy + Pillow) to iterate on the algorithm
quickly, then **ported to C** (`src/normalmap.c`) once it was settled, and
the Python version deleted. Reasons the C version is better here, not just
more consistent:
- No Python/numpy/Pillow dependency for ~70 lines of pixel arithmetic.
- raylib already provides the primitives (`ImageBlurGaussian`,
  `LoadImageColors`, `ImageCopy`), so nothing had to be written from scratch.
- Because it now runs in-process, strength and blur are tunable live with
  Q/W and A/S, with the relighting updating instantly. That replaced an
  edit-script/rerun/restart loop, the same win shader hot reload gave us.
- There is no longer a separate normal map file to keep in sync with the
  photo; the photo is the only input.

Python would still be the better tool for trying fundamentally different
algorithms (bilateral filtering, frequency separation, an ML model), where
numpy saves real time. It was the right prototyping choice and the wrong
final home.

Convention: the green channel encodes `-dh/dy` with y increasing DOWNWARD,
matching raylib's `fragTexCoord`. Both ends of this are ours, so the usual
OpenGL-vs-DirectX green-channel argument does not apply - just keep the
generator and the shader agreed.

The sample asset was later swapped for a stock portrait (see README credits)
so the repository could be published without a personal photo in it. The
artifacts below were observed on the original headshot; the replacement has
no glasses and a soft background gradient rather than a hard white edge, so
it shows the first two far less.

Known artifacts of the luminance cheat: 
- Glasses frames are dark, so they read as grooves.
- The hard white-background edge against hair and suit becomes a rim ridge.
- The dark suit reads as a pit beside the light shirt.
- The photo's original studio lighting is baked into the "height", so the
  relief responds a little better to light from the original direction.

## Phase 3 - Photo relighting integration (DONE)
Goal: combine everything - this is the actual feature from the original
idea.

- [x] Load the photo as `texture0` and the generated normal map as a second
      sampler (`normalMap`), the latter built in-process at startup.
- [x] Shader samples the normal map instead of computing a procedural
      normal; the rest of the Blinn-Phong math from Phase 1 is unchanged.
- [x] Point light with adjustable height, moved with the mouse.
- [x] Quad sized to the photo's aspect ratio and centred.
- [x] Optional argv override for photo and normal map paths, which made the
      isolation test below possible.

Exit criteria met: verified that moving the light left, right and below
shifts the shading across the face convincingly, including a believable
uplight look from below.

**Bug found and fixed here, worth remembering:** `SetShaderValueTexture`
must be called INSIDE `BeginShaderMode`. `BeginShaderMode` flushes raylib's
render batch, and `rlDrawRenderBatch` clears the registered auxiliary
texture units (rlgl.h:3116). Calling it before shader mode silently left
the `normalMap` sampler pointing at an unrelated black texture, which
rendered as a near-black surface with a diagonal terminator line.

Diagnosing that was much easier with a white albedo plus a flat
(128,128,255) normal map fed in via argv: the lit result then has to be a
radial hotspot centred exactly on the mouse, so any deviation is a bug
rather than a quirk of the photo. Worth reusing whenever the lighting looks
suspicious.

## Phase 3 - Photo relighting integration
Goal: combine everything - this is the actual feature from the original
idea.

- [ ] Load the photo as the diffuse texture and the Phase 2 normal map as a
      second texture in raylib.
- [ ] Extend the Phase 1 shader to sample both textures.
- [ ] Point light with adjustable height/depth (since the "surface" is a
      flat plane pretending to have relief).
- [ ] Move light with mouse in screen space, mapped to the photo's plane.

Exit criteria: moving the mouse over your own photo visibly shifts
highlights/shadows across your face in a convincing way.

## Phase 4 - Polish and controls (DONE)
Goal: make it pleasant to play with, not just technically working.

- [x] On-screen controls via raygui: sliders for intensity, falloff,
      ambient, specular, shininess and light height, plus a colour picker
      for the light, and sliders for the normal map's relief and blur.
- [x] Promoted the shader's look tunables from `const` to uniforms, as
      Phase 1 anticipated.
- [x] Support swapping in different source photos without recompiling
      (pass the photo path as the first argument).
- [ ] Skipped as unnecessary for now: multiple lights, rim/fill light.

Exit criteria met: the look can be tuned comfortably while the light moves.

raygui was chosen over hand-rolled keyboard controls because it is by
raylib's own author, is pure C in a single header, and keeps the project
within the C-and-raylib constraint. It is fetched by CMake with
`SOURCE_SUBDIR src` so FetchContent only downloads it instead of trying to
build raygui's own targets.

Notes from building this:
- The light follows the mouse only while the cursor is over the photo, so
  it stays put while you reach for a slider. Without that, every trip to
  the panel dragged the light across the face.
- Dragging the relief slider regenerates the whole normal map every frame
  and still holds 60 FPS at 500x500, so the height-field caching I had
  planned turned out to be unnecessary. Worth re-checking on a much larger
  photo.
- Relief is now a single control living in the generator. The shader's old
  `NORMAL_STRENGTH` was removed because scaling normals in the shader is
  mathematically identical to scaling the gradients before normalising, and
  two knobs for one effect is just confusing.

## Phase 5 - Relight the live webcam feed (DONE)

**This phase was originally written down wrong.** Every earlier version of
this plan described Phase 5 as "use the webcam to move the light", i.e. the
camera as an input device pointing a light at the still photo. The actual
goal, clarified on 2026-09-08, is the opposite and much more interesting:
**the webcam feed becomes the thing being lit**, so the artificial light
shines on you in live video. The still photo was only ever a stepping
stone for getting the lighting maths right.

Goal: relight the live camera feed in real time.

- [x] Source toggle in the panel: relight the still photo, or the live
      webcam feed.
- [x] Normals derived per pixel in the fragment shader, so they keep up
      with live video (see below).
- [x] Light positioned with the mouse over the feed, exactly as with the
      photo.
- [x] Feed mirrored by default, so it behaves like a mirror when you are
      the subject.

Exit criteria met: with ambient at 0 the light was placed at two known
points over the live feed and landed exactly there, lighting the room's
surfaces with visible relief, holding 60 FPS.

**Normal generation moved from the CPU to the GPU.** The old
`src/normalmap.c` rebuilt a normal map texture on the CPU, which was fine
for a still photo but would have meant blurring, Sobel-filtering, encoding
and re-uploading a texture 30 times a second for live video. The fragment
shader now samples the albedo texture's neighbours and computes the Sobel
gradient per pixel instead. Consequences:
- The CPU generator became dead code and was deleted. Verified first that
  the still photo looks the same or slightly crisper than before, so this
  was not a quality regression.
- One code path now serves both the photo and the live feed.
- Relief and smoothing are free to change, with no texture rebuild at all.
- raylib's `ImageBlurGaussian` is replaced by simply widening the Sobel tap
  spacing ("Smoothing"), which relies on bilinear filtering to average as
  the taps spread out. Not a true Gaussian, but visually equivalent here.

**Bug found during this phase:** the light landed mirrored on the live
feed. The feed is drawn with a negative source width to mirror it, which
reverses the texture coordinates the shader lights in, while the light
position was still being computed in screen space. The two spaces
disagreed, so the fix is to flip x when converting the light position into
texture space.

### Device selection

Added after noticing that picking `devices[0]` is a coin flip on any machine
with both a real camera and a virtual one such as OBS. The webcam module now
splits into a system lifecycle (`InitWebcamSystem` / `ShutdownWebcamSystem`)
and a per-device one (`OpenWebcam` / `CloseWebcam`), so switching source is
just closing and reopening. Device friendly names come from
`MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME` as UTF-16 and are converted with
`WideCharToMultiByte`; any semicolon in a name is replaced, since that is
raygui's dropdown separator.

A second dropdown selects capture resolution (480p / 720p / 1080p,
defaulting to 480p). Changing it needs no restart: it reuses the same
close-and-reopen path as switching device, which is now factored into a
single `ReopenWebcam` helper so startup and both dropdowns share one code
path instead of duplicating the buffer and texture rebuild.

Measured, rather than assumed: **1080p holds 60 FPS** on this machine and
the camera really does deliver 1920x1080. Camera resolution costs nothing in
the shader - it is CPU work per frame (MF format conversion, the BGRA to
RGBA swizzle, two memcpys, the texture upload), about 8 MB per frame at
1080p against 1.2 MB at 480p, and mostly on the capture thread. 4K is
deliberately not offered: four times that cost, and 4K webcams usually only
manage it at low frame rates.

If 1080p ever does prove heavy, the first thing to cut is the swizzle loop -
OpenGL can take BGRA directly, so the CPU need not touch the pixels at all.
There is also a redundant memcpy that double-buffering with a pointer swap
would remove.

Three things learned building the dropdowns:
- With two dropdowns, order matters. The resolution box sits below the
  device box, so an open device list expands over it; drawing resolution
  first and device last is correct for both cases, since an open resolution
  list only expands downward over the checkboxes. Opening one closes the
  other.
- An open raygui dropdown does not paint above controls declared before it,
  because raygui has no z-order. The fix is to reserve its rectangle during
  layout, draw it last, and `GuiLock()` everything else while it is open so
  clicks cannot fall through to the controls hidden underneath.
- raygui makes a dropdown with only one item deliberately inert
  (`itemCount > 1` in GuiDropdownBox). On a machine with a single camera it
  will not open, which is reasonable but looks like a bug until you know.
  Verified the open state by temporarily adding a fake second entry.

### DPI scaling and a resizable window

Added because the UI text was hard to read on a 4K display. raylib sizes
windows in physical pixels, so at 150% OS scaling everything was drawn at
two thirds of its intended size. `GetWindowScaleDPI()` reports 1.50 on this
machine, and that factor now multiplies the window size, raygui's
`TEXT_SIZE` and every layout measurement.

The window is also resizable now (`FLAG_WINDOW_RESIZABLE`), with the layout
recomputed each frame from `GetScreenWidth`/`GetScreenHeight` rather than
from constants captured at startup.

Two things this forced, both improvements:
- The light is now stored in normalised image coordinates instead of screen
  pixels. Resizing used to be able to leave it stranded off the subject, and
  the mirrored-feed conversion also reads more clearly this way.
- `DrawFPS` was replaced with a plain `DrawText`, because its font size is
  fixed and could not scale.

Worth recording, since it was the actual question asked: resizing does not
touch the webcam resolution. Capture stays at the opened mode and only the
fragment count changes. Maximised to 3840x2054 it still holds 60 FPS, so
the per-pixel Sobel is not a bottleneck at 4K.

### Earlier notes: webcam as an input device

Built before the goal was clarified, and kept because it is a genuinely
nice extra: "Light follows brightest" tracks the brightest point in the
camera image and drives the light with it, so a phone torch in the room
moves the virtual light.

- [x] Capture via Windows Media Foundation (`src/webcam.c`). Chosen over
      OpenCV because it ships with Windows, is callable from plain C, and
      adds nothing to the build. No companion process was needed. This part
      was needed either way and carried over unchanged.
- [x] Track the brightest point, as a weighted centroid of the brightest
      pixels rather than a single argmax, which hopped between equally
      bright pixels and made the light twitch.
- [x] Map the tracked position onto the light, with exponential smoothing
      and an optional mirror so it feels like a mirror rather than a
      reversed image.
- [x] Live preview in the panel with a crosshair on the lock, so it is
      obvious what the tracker has latched onto.
- [ ] Real hand/face tracking - still not obviously worth the complexity,
      see the honest assessment below.

Exit criteria met: the light on the photo follows the brightest thing in
the camera's view, at a steady 60 FPS.

Implementation notes worth keeping:
- Capture runs on its own thread. Media Foundation's synchronous
  `ReadSample` blocks until the camera produces a frame, which would
  otherwise have pinned the render loop to the camera's 30 FPS. The thread
  writes into a shared buffer behind a critical section and the render loop
  copies out whatever is newest.
- `src/webcam.c` deliberately does not include raylib.h. windows.h and
  raylib.h declare conflicting names (`Rectangle`, `LoadImage`,
  `CloseWindow`), so the module keeps to plain Win32 plus the C standard
  library and hands frames over as a byte buffer.
- `MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING` lets the reader
  convert to RGB32 for us, so there is no need to decode the camera's MJPG
  or YUY2 by hand.
- The camera's smallest sensible mode is selected rather than its native
  4K, because capturing 4K and scaling down would be far more expensive.
- Two linker surprises: `MFEnumDeviceSources` lives in `mf.lib`, not
  `mfplat.lib`; and `MFGetAttributeSize` is not exported by any library in
  the current Windows SDK, so the packed `MF_MT_FRAME_SIZE` UINT64 is
  unpacked directly instead.

Honest assessment of the tracking: brightest-point works and is very cheap,
but it locks onto whatever is brightest in the room - daylight on a wall,
a window, a pale surface - rather than onto you. Because it is a centroid
of all the brightest pixels, it can also sit between two bright areas and
correspond to no object at all. A phone torch gives genuinely good control.
If controlling it by body position matters, the cheap next step is frame
differencing to track motion instead of brightness. Real face tracking is a
much larger jump and should wait until there is a reason for it.

## Demo mode

Built on the `feature/demo-mode` branch, after the five planned phases. A
self-driving light so the effect can be demonstrated without holding the
mouse. Basic moves position only and respects every setting made by hand;
Full also drifts height and hue.

Motion approach: two sine waves per axis at unrelated frequencies, summed,
with phases randomised at startup. Considered and rejected: interpolating
between random waypoints, which stalls momentarily at every target unless
you go to Catmull-Rom for continuous velocity; and Perlin noise, which
raylib has no scalar version of and which would have to be written from
scratch for no visible gain here. Summed sines give continuous velocity for
about four lines of code.

Design decisions worth keeping:
- The demo never writes into the user's settings. It computes an
  `activeHeight` and `activeColor` used only for the shader upload, so
  switching the demo off restores whatever was set by hand. Writing into
  `light` directly would have destroyed the user's values and made the
  sliders jitter and fight the demo.
- While Full runs, the Height slider and colour picker are switched to
  `STATE_DISABLED` and fed a temporary copy of the live demo value, so they
  read out what is happening instead of lying about it.
- raygui's disabled styling greys the colour picker out, which hides the
  colour it is meant to be showing. A small live swatch next to the "Light
  colour" label fixes that.
- Hue is allowed to wrap with `fmodf` rather than being clamped, because the
  hue wheel joins up at 360 and wrapping is visually continuous.
- Base window height went from 800 to 860 to fit the extra row.

## Open questions / decisions deferred to when we hit them

- Webcam library choice (Phase 5) - deliberately not decided yet. Note that
  the project is now C-only with raylib, so a pure-C option is preferred
  over pulling in a C++ dependency.
### Deferred: masking the background out

Not being done for now - as of 2026-09-08 the halo is not bothering us at
the relief values we actually use, so this is only worth building if it
starts to look wrong. Noted here so the plan does not have to be
rediscovered.

The problem: the photo's white backdrop meets dark hair and suit at a hard
brightness cliff. The generator reads that cliff as a steep slope, so the
silhouette gets a bright chrome-like rim that catches the light. It is
obvious at high relief values and subtle at low ones.

Sketch of the fix (~60 lines):
- Flood-fill inward from the four image corners with a colour tolerance to
  identify backdrop pixels. Flood fill rather than a plain brightness
  threshold, so the white shirt and teeth are not also treated as backdrop.
- Store the result in the normal map's alpha channel, which is currently
  unused and always 255. No extra texture needed.
- In the shader, either flatten those pixels to a fixed normal so they stop
  catching the light, or replace them with a plain dark backdrop.
- A slider for the tolerance, since it is photo-dependent.

## Non-goals (for now)

- Physically accurate single-image normal estimation (ML-based). The
  heightmap-from-luminance cheat is the intended approach unless it proves
  visually insufficient.
- Cross-platform support beyond Windows (can revisit later if desired).
