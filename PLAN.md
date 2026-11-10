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

## Virtual camera: expose normalish as a camera source (DONE)

Goal: a "normalish camera" that Teams (or anything else) can select, showing
the relit webcam feed, behind a default-off "Expose as virtual webcam"
toggle. The alternative - OBS with a virtual camera pointed at our window -
works today but is a hassle, and was deliberately rejected in favour of
doing the real thing.

Approach: `MFCreateVirtualCamera` (Windows 11 build 22000+, and this machine
is 26200). Not DirectShow: the new Teams is Chromium-based and Chromium on
Windows captures through Media Foundation, so a DirectShow-only camera would
most likely never appear in the list. That is what broke many older virtual
cameras.

Language exception: implementing a COM server in plain C means hand-rolling
vtable structs, refcounting and QueryInterface. The media source DLL is
therefore C++, while normalish itself stays C. Agreed 2026-09-08 as specific
to this DLL.

### What shipped

- `src/vcam/` - the media source DLL, adopted from smourier/VCamSample (MIT,
  attribution in `src/vcam/LICENSE-VCamSample.txt`). Changed: our CLSID, the
  packaged-app identity lookup dropped, CMake instead of MSBuild.
- `src/vcam/NormalishFrames.cpp` - ours; reads published frames and converts
  to RGB32 or NV12.
- `src/frameshare.h` - the producer/consumer contract, plain C, shared by
  both sides.
- `src/vcamhost.c` - camera lifecycle and frame publishing from normalish.
- Frames travel through a memory-mapped file under `C:\ProgramData\normalish`
  with a seqlock, because the media source runs in the Frame Server service
  in session 0 under another account.
- The toggle is default-off and greys out with a reason when
  `MFIsVirtualCameraTypeSupported` says no. Registration is a one-time
  elevated `regsvr32`.

The technical gotchas this turned up are in [FINDINGS.md](FINDINGS.md),
organised by area rather than chronologically.

### How it went, and the root cause

A from-scratch WRL implementation got a long way: the camera appeared in the
Windows Camera app, could be selected, and negotiated a format - but never
produced a frame. Eight rounds of diffing against the working reference
found and fixed a genuine difference each time (the `IMFActivate`
indirection, HKLM registration, the device-source attributes, separate
stream attributes, the sensor profile collection, both media types,
`MF_DEVICESTREAM_FRAMESERVER_SHARED`, the provided-allocator contract) and
**not one of them changed the symptom.**

The reason none of them appeared to work: **the Camera Frame Server caches a
device's configuration per CLSID.** The very first `Start` succeeded against
an early, incomplete source, and every test after that - including an
entirely different implementation - was served that same stale device. It
gave itself away when the consumer reported 1280x720, our old dimensions,
while the adopted source uses 1280x960. `Restart-Service FrameServer -Force`
and it streamed immediately.

**The lesson, and it is a general one:** when a change appears to have no
effect at all, suspect that the thing under test is not the thing you
changed. That possibility should have been checked far earlier - the DLL
staying locked between builds was already a hint that something outside the
build was holding state.

**Uncomfortable consequence:** most of rounds five through eight were tested
against a cached device, so which of those changes were actually necessary
is unknown. Finding out would mean re-testing each in isolation with a
service restart between. The from-scratch attempt was not kept - the branch
was squashed before the feature work began - so it exists only as this
account.

Two mistakes of method are worth keeping, both the same shape of not
verifying a precondition:

- The first consumer test pointed normalish's own input dropdown at our
  virtual camera. That is circular, and it made the architecture look
  inverted. Replaced with a purpose-built consumer that finds the camera by
  name and prints exact HRESULTs. (Also a real fix: normalish now excludes
  its own virtual camera from its input list, or the feedback loop is one
  click away.)
- `GetSourceAttributes` returns the raw MF attribute store, so Frame
  Server's reads on it bypassed our logging. The absence of log lines
  therefore proved nothing.

### Remaining work

- An info button beside the toggle explaining what registration does: one
  machine-wide COM entry, no camera until the toggle is on, nothing runs at
  boot, reversible with `regsvr32 /u`.
- Detect missing registration and offer a one-click elevated install rather
  than failing opaquely.

## Depth-based relighting: put the light behind the subject

Goal: the light should be placeable **behind** the subject, not just above a
flat relief. That needs to know how far away each pixel is, which the
luminance cheat fundamentally cannot tell us - it reports a bright shirt as
a hill.

### What depth buys, and what it does not

A depth map is 2.5D: one depth per pixel, from the camera's viewpoint. There
is no data for the back of anyone's head, because no camera saw it. So a
light truly behind the subject cannot illuminate their back. What it can do,
and what this work is for:

- **Real normals** instead of luminance-as-height. The nose actually
  protrudes; a bright shirt stops being a hill; glasses stop being bumps.
- **Rim light when the light goes behind.** Front-facing normals turn away
  from a light beyond the surface, so the front goes dark and the silhouette
  catches it. This is the effect we are after.
- **Cast shadows**, by marching the depth map toward the light in screen
  space: nose onto cheek, subject onto the wall behind, contact shadows.
- **Background separation for free**, which retires the masking problem
  deferred in phase 4.

Not in scope: orbiting the light fully around the subject. That is novel
view synthesis, a different and far larger problem.

### Hardware investigation (done, 2026-09-08)

Enumerated every Media Foundation device and mode. One camera, one stream:
the Creative Live! Cam Sync 4K, offering 1920x1080 / 3840x2160 / 2560x1440 /
1600x1200 / 1280x720 in NV12 and MJPG. No second stream, no IR, no depth,
no wide side-by-side mode. No Windows Hello IR camera. Single 2560x1440
display.

**The VIVE Pro was investigated and rejected.** It is registered in PnP (not
currently connected) and does have a stereo front camera pair, but its
cameras are 480p each with a **96 degree** horizontal field of view. At a
normal 600mm working distance that FOV covers ~1333mm of scene width, so a
150mm face occupies **~72 pixels**. Depth precision along z would have been
adequate (~19mm per disparity pixel, ~5mm with subpixel fitting); it is the
lateral resolution that kills it - a 72x90 depth map resolves the nose as a
blob and the brow, eye sockets and lips not at all, and those broad features
are exactly what drives believable lighting. Secondary problems: the pair is
delivered vertically stacked rather than side by side, OpenVR's own
undistortion is broken specifically on Vive Pro (ValveSoftware/openvr#815),
access likely requires SteamVR and the OpenVR TrackedCamera API, and it is a
headset and link box on the desk.

### Candidate depth sources, and why the decision is deferred

| Source | Quality | Cost | Verdict |
| --- | --- | --- | --- |
| Synchronised dual-lens UVC module | Good | ~$78, one device, hardware-synced, side-by-side in one stream | Best if hardware is acceptable; drops into the existing device dropdown |
| Two matched webcams | Good | Two devices, unsynchronised, manual mounting | Fallback |
| Photometric stereo off the monitor | Real measured normals plus separated albedo, no hardware, no dependency | One-second calibration flash, still subject, dim-ish room | Rejected for now: the user needs the single monitor for other things |
| ML monocular depth (Depth Anything V2 via ONNX Runtime C API) | Best, live, per-frame, no ritual | One DLL plus a model file; needs a rule exception | **CHOSEN 2026-09-08** - see phase D |
| Depth from defocus | - | - | Physically impossible: webcam depth of field at 600mm is far too deep |
| Structure from motion | - | - | Sitting still gives no baseline, and a moving face breaks the rigid-scene assumption |

**The decision is deliberately deferred.** Every source above is just a
different producer feeding the same consumer, so the renderer is built first
against a synthetic depth map. That also answers the question worth asking
before spending anything: whether "light behind me" looks as good as it
sounds. If it disappoints on perfect synthetic depth, it would never have
looked good on noisy stereo depth.

### Camera model and conventions (pin these down first)

The current shader treats the image as the z = 0 plane in "surface units"
(x in 0..aspect, y in 0..1, z above the surface) with an orthographic viewer
at V = (0,0,1). That is fine for a flat relief and wrong for real depth:
unprojecting a depth value to a 3D position needs a perspective model.

New convention, camera space, metres, z positive going away from the camera:

    vec2 ndc = uv*2.0 - 1.0;
    vec3 P;
    P.z  = depth;                                   // metres from the camera
    P.xy = ndc*vec2(tanHalfFovX, tanHalfFovX/aspect)*P.z;

Consequences worth stating:

- `lightPos` moves into the same space, so **`lightPos.z` becomes distance
  from the camera** and "behind the subject" is simply a light z greater
  than the subject's depth. The existing interaction survives: dragging
  still picks a screen position, and the height slider becomes a
  camera-distance slider that sweeps the light from in front of the face to
  behind it. That single slider is the demo.
- The viewer is no longer orthographic: `V = normalize(-P)`.
- **The flat/orthographic model stays as a user-selectable mode, not a
  fallback.** Decided 2026-09-08: it works, it is the whole of what ships on
  main, and it is the right choice for a still photo where there is no depth
  to be had. So there is a "Geometry" setting with two modes - luminance
  relief (flat, orthographic, exactly as main behaves today) and depth
  (perspective) - and the luminance path is not to be touched beyond being
  moved behind that switch.
- This branch is experimental by agreement; main remains the fallback if the
  result disappoints.

### Phase A - depth as a first-class input (DONE)

No visual change; this phase is about correctness.

1. A depth-source abstraction in C, so the consumer never knows where depth
   came from: kind, a texture, and the camera parameters needed to unproject.
2. A synthetic producer generating analytic scenes - a sphere on a back
   plane first, then a crude head (sphere plus ellipsoid shoulders plus
   plane) - with a matching synthetic albedo.
3. Shader: `depthMap` sampler, `tanHalfFov`, and a validity test so pixels
   with no depth fall back rather than producing garbage.
4. Normals from depth by explicit taps rather than `dFdx`, which is
   per-quad and blocky. Depth discontinuities at silhouette edges produce
   nonsense normals across the jump, so take the smaller of the forward and
   backward difference per axis.
5. New view modes: depth, and normals-from-depth.

**Verification, measurable rather than by eye:** the synthetic scene has
analytic normals, so render normals-from-depth against ground truth and
report the angular error. If unprojection or differencing is wrong this
fails loudly instead of looking plausible.

**Result (DONE).** `normalish.exe --verify-normals` renders the shader's
normals for both scenes into a render target, reads them back and compares
against the analytic normals:

    VERIFY scene 0 at 1280x720
      interior : mean 0.319 deg, worst 1.989 deg over 914840 px
      silhouette: mean 1.415 deg, worst 10.044 deg over 2764 px
    VERIFY scene 1 at 1280x720
      interior : mean 0.319 deg, worst 2.134 deg over 912320 px
      silhouette: mean 1.506 deg, worst 95.643 deg over 5284 px

Interior mean of 0.32 degrees is essentially the 8-bit readback floor, so
the unprojection, the aspect handling, the cross product order and the sign
conventions are all correct.

Silhouette pixels are reported separately because the right answer there is
genuinely ambiguous - the one-sided difference reports whichever surface it
stayed on. The 95 degree worst case is a pixel where both neighbours on an
axis lie across the discontinuity, so no choice keeps the estimate on the
surface. It is 0.6% of pixels, and it matters more than that number
suggests, because the silhouette is where the rim-light effect lives.
Recorded rather than fixed: the cheap improvement is to mark such pixels
unreliable and fill them from their neighbours.

### Phase B - relight from real normals (DONE)

1. Lighting uses depth-derived normals when depth is present, luminance
   otherwise.
2. Reinterpret and relabel the light's z as camera distance in metres.
3. Perspective viewer for the specular term.

**Proof:** sweeping the light from in front of the subject to behind it
should read front-lit, then edge-lit, then dark with a bright rim. If that
does not look good here, stop and reconsider - this is the go/no-go.

**Result (DONE, go).** On the head scene with the light off to one side:

- 0.25 m - conventional front lighting, and the sphere reads as a sphere
  rather than as a lit disc.
- 0.55 m - level with the face. The head goes nearly black with only a
  specular glint, while the shoulders at 0.70 m stay brightly lit. Correct,
  and impossible under the luminance model, which has no notion of one part
  of the image being nearer than another.
- 0.95 m - behind the head. The subject silhouettes almost black with a
  bright rim down the lit edge of the head and across the top of the
  shoulder, and the wall glows behind. **This is the effect the whole branch
  was for, and it came out of the geometry with no shadow code at all.**

The same screenshots show exactly what is missing: the wall glow is
uniform around the head, because nothing casts a shadow yet. That is
phase C.

### Phase C - screen-space shadows (DONE)

This is where "light behind me" becomes convincing rather than merely dark.

March from each surface point toward the light, projecting each step back to
screen space and comparing against the depth map. Details that will need
care:

- **Bias**, or every surface shadows itself at step zero.
- **Jitter** the step offsets, or the shadow edges band visibly.
- **Occluder thickness.** A depth map has no back face, so the naive
  assumption is that everything behind the depth surface is solid forever.
  For a light behind the subject that is correct and gives exactly the
  dramatic look we want. For a light *between* subject and wall it
  over-shadows, because the silhouette becomes an infinite slab. A
  thickness parameter is the standard fix.
- **Rays that leave the screen, or enter a region with no depth data, are
  unoccluded** - not shadowed.
- **Cost.** At 1080p with 32 steps that is ~66M depth samples per frame.
  Fine on the RX 7800 XT, but the step count gets a slider and this needs
  measuring rather than assuming.

**Result (DONE).** Holds 60 FPS at 24 steps, which is the frame cap, so the
march is not the limit. The head and shoulders throw a soft shadow onto the
wall and there is a contact shadow under the chin. With the light behind the
subject the wall glow is now visibly occluded - confined to the lit side with
a clear boundary - where in phase B it spread evenly around the head.

Two things had to be fixed after looking at it:

- **A binary occlusion test plus jitter dithers every shadow edge into
  speckle.** Counting occluded samples along the ray instead, and mapping
  coverage through a smoothstep, turns that same jitter into a gradient that
  reads as a penumbra. It costs the early exit out of the loop, which the
  frame budget did not notice. A quarter of the ray inside geometry is taken
  as fully shadowed.
- **The panel overflowed.** Relief and smoothing shape the luminance cheat
  and mean nothing under the depth model, so they are hidden in depth mode,
  which paid for most of the new rows. The shadow on/off checkbox was then
  folded into the step slider - zero steps is off - to pay for the rest.

Shadow bias is exposed as a control rather than fixed, because it is the
parameter most likely to need tuning once depth is noisy rather than exact.

### Phase D - depth from the webcam, by monocular estimation

Decided 2026-09-08. Of the shortlist above, this is the only option that
produces the subject's own geometry using hardware already on the desk, with
no second camera, no calibration, no capture ritual and no dim room. The
price is one dependency.

**Second scoped exception to the C-and-raylib rule**, alongside the vcam DLL.
ONNX Runtime is reached through its **C** API, so no C++ enters normalish
itself, but it is a real third-party binary plus a model file. Recorded here
so it is a decision and not a drift.

**Model: Depth-Anything-V2-Metric-Hypersim-Small.** Apache-2.0, and the
*metric* variant matters more than it sounds: it outputs depth in **metres**
(indoor-tuned, 0-20 m) rather than the relative inverse depth the base models
produce. Our renderer already works in metres in camera space, so this drops
in without inventing a mapping from a unitless disparity to a distance.
Input is 1x3x518x518, ImageNet normalisation.

**Runtime: ONNX Runtime with the DirectML execution provider.** DirectML is
DX12, so it accelerates on the RX 7800 XT with no vendor-specific anything -
CUDA is not an option here. The DML EP is selected from the C API through
`dml_provider_factory.h`. Prebuilt DML builds ship as NuGet packages, which
are plain zips, so CMake FetchContent can pull one the same way WIL is
pulled; that route is already proven from fetching VCamSample's packages by
hand.

**The model file must not be committed.** It is tens of MB and does not
belong in a public repo whatever its licence. It gets downloaded on demand
into `C:\ProgramData\normalish\`, and depth mode stays unavailable with a
reason until it is present - the same shape as the virtual camera's
registration step.

#### D1 - spike, before any integration

The risky questions are all unknowns, so answer them in a throwaway harness
that does not touch normalish:

1. Does ONNX Runtime DirectML build and link from FetchContent?
2. Does the model actually run on this GPU?
3. **How long does one inference take?** This decides the whole architecture.
4. Is the output plausible, and genuinely in metres? Check a known distance:
   the wall behind should read about right.

Deliverable: a console tool that loads a saved webcam frame, runs the model,
prints min/max/centre depth in metres and the per-frame latency, and writes
the depth out as an image to look at. Nothing else.

**Result (DONE) - all four answers are favourable.**

1. ONNX Runtime 1.24.4 DirectML builds and links against its C API with
   nothing but `onnxruntime.lib`. Needed pieces: the DirectML EP package
   depends on `Microsoft.AI.DirectML` 1.15.4, whose `DirectML.dll` must sit
   beside `onnxruntime.dll` - the copy in System32 is not guaranteed to be
   new enough. Both are NuGet zips, so FetchContent can pull them.
2. `DirectML : enabled on adapter 0` on the RX 7800 XT, no vendor anything.
3. **22 ms per inference, best 22.0, worst 23.3, stable across runs.** About
   44 fps if it were the only cost, against a 40 ms cold first run that pays
   for shader compilation. This is far better than assumed and it collapses
   the architecture worry: a worker thread at 30 Hz is comfortable, and the
   elaborate decoupling contemplated below is not needed.
4. Output is `[1,518,518]`, names `pixel_values` / `predicted_depth`, and the
   values really are metres. On the portrait asset: 1.35 to 2.96 m, centre
   1.81. On a real frame of the room: 1.86 to 7.91 m, with the doorway down
   the middle reading 7.5 m and the subject the nearest thing in frame. Both
   plausible.

Quality is better than the risk note below feared. On the portrait the nose
protrudes, the eye sockets are recessed, the ears sit at the right depth and
the silhouette is crisp, with only a modest halo at the boundary. On the room
frame the subject separates cleanly from the bed, the curtain and the
corridor behind.

**One new finding, which is the thing to watch in D2.** In a room-scale
frame the depth range is dominated by the background - 1.9 to 7.9 m - so the
subject's own 20 cm of relief is a small fraction of it, and the face reads
as a nearly flat blob in a normalised visualisation. That may be harmless,
because the shader takes normals from the *metric* gradient rather than from
a normalised range, so face relief should survive regardless of what the
background does. But if the model's effective precision in that regime is
coarse, the facial normals will be noisy, and the fix is filtering or a
gradient boost. Test it early in D2 rather than discovering it at the end.

**The real cost, stated plainly:** about 27 MB of binaries (onnxruntime.dll
17 MB, DirectML.dll 10 MB) plus a 99 MB model file.

#### D2 - integration (DONE, with one thing outstanding)

- **Inference on a worker thread, not the render loop.** The existing webcam
  capture already establishes the pattern, and for the same reason: a
  synchronous call that takes tens of milliseconds would drag 60 FPS down to
  the model's rate.
- **Depth does not need 60 Hz**, and after D1 it does not need to be clever
  either. 22 ms means a worker thread can run at 30 Hz and the renderer stays
  at 60 regardless; no interpolation scheme is required.
- **Letterbox, do not squash.** Resizing 16:9 into a square input distorts
  the geometry against the real camera, and the depth would then be
  misaligned with the albedo. Pad to square, run, crop back.
- **The camera's field of view has to come from somewhere.** The
  unprojection needs `tanHalfFovX` for the actual webcam, not the synthetic
  scenes' 65 degrees. A slider with a sensible default is enough - it only
  scales how pronounced the 3D is - but it is a real input and should not be
  silently wrong.
- **Upsampling matters more than it looks.** A 518-wide depth map against a
  1080p albedo needs an edge-aware, colour-guided upsample in the shader.
  Plain bilinear smears silhouettes, and the silhouette is where the rim
  light lives; point sampling makes them blocky. This is the main quality
  risk in D2.
- **Temporal smoothing.** ML depth flickers frame to frame, and flicker in a
  relight is far more obvious than in a depth visualisation. An exponential
  moving average per pixel, accepting some ghosting on fast movement.

**Result.** The live webcam feed is relit from model depth at 60 FPS, with
the model on its own thread and inference at about 22 ms. The depth map of a
real room is good - head and shoulders cleanly nearest, glasses visible, bed,
curtain, lamp and microphone arm all at plausible depths, and a crisp
silhouette.

Three bugs found by looking at it, each a scale assumption:

- **Every depth-mode range was calibrated to the synthetic scene**, whose
  subject sits at 0.62 m and wall at 1.3 m. A real room runs 1.9 to 8 m, so
  the depth view clipped to black, the light distance slider could not reach
  the subject at its 1.5 m maximum, and falloff 8.0 extinguished the light.
  Fixed structurally rather than by retuning: `DepthFrame` now carries the
  scene's near and far, the model reports its own range as robust
  percentiles, and the light distance, view mapping, shadow thickness, shadow
  bias, march span and slider ranges all derive from those. Falloff is
  measured relative to the nearest surface, so one setting means the same
  thing at any scale. Depth mode keeps its own falloff and distance, because
  the luminance model measures in image heights and this one in metres.
- **`texelSize` is the albedo's, not the depth map's.** The model returns 518
  wide against a 1280-wide frame, so the gradient taps landed inside a single
  depth texel where the bilinear interpolation is piecewise linear, and the
  normals followed the interpolation grid instead of the surface - visible as
  vertical streaking. A separate `depthTexelSize` fixes it. This never showed
  on the synthetic scenes because there both textures are the same size.
- **Normals from monocular depth are dominated by the model's own error.**
  This is the D1 risk arriving. At 2.5 m the frame covers about 3.5 m across
  518 texels, so one depth texel is roughly 7 mm of scene; a 5 mm depth error
  between neighbours tilts the normal by 36 degrees, and the model undulates
  by centimetres because it is trained for depth accuracy, not gradient
  accuracy. Widening the taps was tried first and made it **worse** - it
  turns fine noise into large blobs rather than removing it, because the
  error is low-frequency. What works is attenuating the tilt: scaling the
  normal's lateral components and renormalising, which softens shape without
  distorting its direction. A `Relief` control, 0.30 for the model and 1.0
  for exact synthetic depth.

**What is outstanding, and it is the interesting part.** With relief at 0.30
the subject looks genuinely good - the face reads as a lit face, the wall
behind takes a clean cast shadow - but the background clutter is still badly
artefacted, worst on a curtain at a grazing angle where the model's depth is
least reliable. The artefacts are concentrated in exactly the part of the
frame nobody wants relit.

#### D3 - relight the subject, leave the room alone (DONE)

Blends back to the untouched frame outside a subject mask. The original
pixels already carry the room's own lighting, so they are the honest fallback.
This removes the worst artefacts by construction, is what the effect is for in
a call, and retires the background masking deferred back in phase 4.

Finding the subject took four attempts, and the first three are worth keeping
because each failed for a different reason.

1. **A depth cutoff.** Fails immediately: a curtain beside the camera is as
   near as the person in front of it.
2. **Growing a region from a seed**, the nearest pixel in the middle of the
   frame. Picked the curtain, because a curtain edge reaches into the centre
   third and is nearer than someone sitting off to one side. A single seed is
   not a person detector.
3. **Labelling depth-continuous regions and scoring them.** The winning region
   swallowed the entire room. A person is depth-connected to their chair,
   which connects to the floor, which runs smoothly to the far wall; adjacent
   pixels on a receding surface differ by a millimetre or two, well inside any
   sensible continuity tolerance, so nothing ever breaks the chain.
4. **Continuity plus a depth band measured from each region's own nearest
   point**, then scoring. This works.

The band has to be per region. Anchoring one global band on the nearest pixel
in the frame was tried and failed for a fourth reason: the nearest thing was
the curtain at about 1.3 m, so a 0.9 m band ran to 2.2 m and excluded the
person at 2.3 m altogether.

Regions are scored for looking like a person in a webcam shot: near, a
reasonable share of the frame with the reward saturating so a wall cannot win
on size alone, running out of the bottom of frame, mildly central, and
penalised heavily for covering most of the frame height at one side - which
is the test that rejects curtains, walls and door frames.

**Verified: 8.1% of the frame, a clean head-and-shoulders silhouette, with
the curtain, bed and wall excluded.**

**A method note that cost time.** Three of those attempts were judged from
screenshots of the live camera, and the last of them was taken of an empty
chair - which says nothing at all about a person detector. The fix was
`--depth-image <in> <prefix>`, which runs the real code path over a saved
frame and writes the depth and mask out. Same lesson as the virtual camera's
purpose-built consumer: when a result depends on what is in front of the
camera, stop testing against the camera.

Controls: **Subject only** on by default, **Subject depth** for the band, and
**Spill** to let some light reach beyond the subject when a hard cut looks
wrong.

#### D4 - calibration against real frames (2026-09-09)

Two captures agreed with the user in advance, which is the process from here:
announce what a capture is for and what pose is needed, then wait. Several
earlier mask evaluations were taken unannounced and one was of an empty
chair, which says nothing about a person detector.

**Does a higher capture resolution help? No.** Same pose grabbed at 1080p and
4K:

| | 1080p | 4K |
| --- | --- | --- |
| depth map | 518x291 | 518x291 |
| range | 2.455 to 7.187 m | 2.365 to 7.041 m |
| mask coverage | 9.2% | 9.1% |
| inference | 44.3 ms | 44.6 ms |
| depth gradient noise | 2.502 | 2.403 |

The model's input is a fixed 518 square, so the depth map's size cannot
change. The last row is the only thing that could have justified 4K - mean
absolute Laplacian of the depth map, a proxy for the high-frequency wobble
normals are made from - and it is 4% smoother. Real, and far too small to
matter, or to let relief rise above 0.02. **Stay on 1080p**; 33 MB a frame is
not worth 4%. The presets remain for output sharpness.

**With nobody in frame, the office chair is selected**, scoring 0.951 with a
mask very nearly the shape of a person: the headrest and back read as a head
and shoulders in depth, and the chair is near, central, large and running out
of the bottom of frame - every feature the scoring rewards. So the selector
is a nearest-central-object selector, and the code and docs now say so rather
than calling it a person detector.

**Two wrong explanations, and then the measurement.** The chair scoring
higher than the person was first explained as *an empty chair being nearer
the camera than its occupant*. The user pointed out that this is backwards -
sitting in a chair puts you in front of its back. The second explanation was
that the chair must have been rolled forward between captures. The user said
it had not moved. Both were guesses dressed as conclusions.

Probing identical pixels in the two frames settles it. On five points of
static scenery - wall, curtain, doorway, none of which can have moved - the
second frame reads **0.19 to 0.32 m nearer** than the first, consistently.
That is the model's absolute scale drifting between two inferences of an
unchanged room. And at the centre-bottom pixel, where the person was and then
was not, the first frame reads **3.096 m** (their chest) and the second
**2.148 m** (the chair back behind where they sat). Correcting for the global
offset the chair is still about **0.68 m nearer than the chest of the person
sitting in front of it**. A chair back is not 68 cm in front of its occupant.
**The sign is wrong, not just the magnitude.**

So the real finding is not about chairs at all:

And the absolute scale is not merely drifting, it is wrong outright: the
subject was sitting **under a metre** from the camera and was reported at
3.10 m, with the near plane at 2.46 m - out by about **3x**. That fits the
model's training distribution, which is synthetic room-scale architecture
rather than close-up portraits; a head filling the frame at 70 cm is nothing
like anything it was trained on, so it reads the scene as a larger room
viewed from further away.

**The model's absolute depth is not trustworthy - wrong by roughly 3x, not
stable across frames, and locally wrong by more than the geometry it is
describing.** It produces
numbers of roughly the right magnitude that are internally plausible, which
is a much weaker property than being metric. Calling it a metric model, as
this plan did, was overstating it.

What that does and does not break:

- **Fine.** Everything anchored to the scene's own reported range rather than
  to absolute values: the light distance slider, the falloff scale, shadow
  thickness and bias, the subject band, and the scoring's proximity term. All
  of these are relative, so a global offset moves them together.
- **Affected but tolerable.** The reconstruction is uniformly scaled, and
  normals, shadow shapes and the falloff ratio are all scale-invariant, so
  the picture is unaffected. The residual is that the drift is closer to an
  offset than a clean scale factor, which distorts the geometry slightly.
- **Wrong, and fixed here.** The controls were labelled in metres, which
  presented a fabricated distance to the user as though it were measured.
  They now read in relative units for model-derived depth, and in metres only
  for the synthetic scenes, where the figure is genuinely known.
- **Not viable.** Comparing scores between frames, which is exactly what a
  "no subject found" floor would have to do. That idea is now closed rather
  than open: the score depends on absolute depth through its proximity term,
  and absolute depth is the part that is unreliable.

Whether an occupied seat can be told from an empty one is therefore not a
tuning problem. It needs a person or face model, or a temporal cue such as
motion - not a better heuristic over these depths.

Also added `--grab-frame <out.png> [preset]`, so one pose can be captured at
several resolutions and every later analysis run against the same file, and
per-region score printing behind `SetDepthModelVerbose` for working out why a
frame chose what it did.

#### D5 - make it good enough to publish (started 2026-11-10)

The user's verdict on D4, which is the ground truth for this phase: the light
cannot be seen, shadows land all over the room and flicker there, and even on
the person they are blotchy and jump. Five stages, each stopping for review:
diagnosis, isolate the person, better shadows on the person, a visible light
behind the subject, write-up.

**Tooling added first, so every change is judged on the same footage.**
`--record-session <dir>` (driven by `tools/record-clips.ps1`) records raw
camera frames to `.nclip` files; six clips were recorded at 1080p: empty
room, sitting still, head turns, waving, leaning, leaving the frame. They are
footage of the user and stay in the gitignored `recordings/`.
`--analyse-clip` replays a clip through the depth model and the shader and
measures where shadows fall, flicker beyond the camera's own noise, and mask
stability, writing a CSV and pictures. `--perf-log <seconds> [mode]` measures
the live app. Shader view mode 5 shows the shadow term alone.

**Stage 1 - diagnosis (done).** Root causes, measured on the clips:

1. *The light cannot be seen* because nothing draws it. There is no emitter
   term in the shader. With the light behind the subject the near-flat
   normals (relief 0.02) face the camera, so the person receives almost
   nothing and becomes a silhouette with a 1-2 px rim; the room is lit by a
   flat normal that does not depend on where the light is, so moving the
   light changes the wall by a uniform ~25 levels. The only thing left that
   moves is the shadow.
2. *Shadows land everywhere* by design: every pixel is a shadow receiver.
   Room pixels darkened by the subject's shadow: 4-5% of the room sitting
   still, 8% turning the head, 7% waving (32% worst frame), 83% leaning,
   because the light's distance follows the scene's near plane with a 1.6x
   hysteresis and the person leans past it. The halo on the wall flickers
   from the jittered penumbra plus depth jitter of 0.2-0.4% per frame.
3. *Shadows on the person are bad* for three reasons. (a) The depth-band
   subject detection picks the wrong thing for 1-4 frames at a time - the
   curtain or the box on the desk: 17 of 194 frames sitting still, 6/191
   head, 13/173 wave, 9/172 lean, 73/310 leaving; the face flashes by ~46
   levels each time. In the empty room it picks the chair (score 0.94). (b)
   The default light at 1.15x near sits at the depth of the face
   (subject/near measured 1.17). (c) The screen-space march over near-flat
   normals is unstable in itself (shadow-term flicker on the subject, p95
   68-250 levels). Depth lagging the picture is *not* a main cause: one
   frame of lag measured the same as none.

**Corrections to earlier conclusions.**
- D3/D4 treated depth-band region scoring as good enough with tuning. It is
  not: the wrong-subject frames above are on a person sitting still. D4's
  closing remark - that this needs a person model, not a better heuristic -
  was right and is now acted on.
- Inference is 23 ms live (25 depth results a second), not the 44 ms recorded
  earlier; 44 ms was with the GPU shared with something else.
- During this stage I reported that the camera delivers 21-24 fps at 1080p.
  Wrong: that was the recorder's write rate. Live, the camera delivers 39
  new frames a second at both 720p and 1080p.

**Frame time before any change** (`--perf-log`, user at the camera): 720p
59.3 fps, 16.87 ms mean, 22.49 ms worst; 1080p 57.6 fps, 17.39 ms mean,
27.51 ms worst. Depth 25.5 and 23.9 results a second.

**Stage 2 - isolate the person (done 2026-11-10).** The decision is to take
"who is the person" from a segmentation model and keep depth for "how far".
Three MediaPipe selfie models were measured on the clips (all Apache 2.0;
details in FINDINGS.md). None is good enough alone:

| model | size | cost | good at | fails at |
|---|---|---|---|---|
| landscape 256x144 | 462 kB | 2 ms CPU | torso and shoulders in any clothing; 0 wrong-subject frames; nothing in the empty room | a hand raised away from the body; a patch of bright window; half-marks the empty chair |
| square 256x256 | 462 kB | 4 ms CPU | leaving the frame | unstable on the wave |
| multiclass 256x256 | 16.5 MB | 3.5 ms GPU (67 ms CPU) | head, hair, skin, hands; rejects the chair | dark clothing against a dark chair (mask 4.4% of frame against 12%) |

Combining landscape and multiclass with the depth map covers each one's
failure: the person is whatever either model marks, that is connected to
pixels the multiclass model is sure of, and is no more than 1.4x further
away than those. Prototype in `analyse.c` (`CombinePersonMasks`), no
temporal smoothing yet, measured on all six clips:

| clip | frame-to-frame mask IoU (mean) | person missed (mean) | notes |
|---|---|---|---|
| still | 0.996 | 0.004% | |
| head | 0.991 | 0.02% | |
| lean | 0.989 | 0.08% | |
| wave | 0.959 | 7.3% (p95 27%) | palm found, motion-blurred fingers and the dark forearm are not |
| leave | 0.918 | 60% | back turned in dark clothes at the frame edge is half found; bending close to the lens with no face is not found at all |
| empty | 1.000 | - | nothing marked in any frame |

Cost about 6 ms per depth result, on the depth worker thread, not the render
thread.

The user agreed to both models. The combination now lives in
`src/personmask.c` and runs in the depth worker: models before the lock, on
the picture the depth network was given; combining under the lock, against
that frame's raw depth. Steadying is a two-result hold when nobody is found
and a temporal average that only applies to small changes. The shader gives
shadows to the person only (`receives` in `lighting.fs`). The depth-band
mask stays as the fallback when the models are missing, and the "Subject
depth" slider is only shown then. The prototype in `analyse.c` is gone;
`--analyse-clip` measures the live path.

Same six clips, before (depth-band mask, shadows everywhere) and after:

| clip | mask IoU (mean) | room pixels shadowed (mean) | room shadow flicker (p95) | person shadow flicker (p95) |
|---|---|---|---|---|
| still | 0.883 -> 0.997 | 5.4% -> 0.16% | 10.9 -> 0.02 | 39 -> 5.8 |
| head | 0.890 -> 0.990 | 8.5% -> 0.16% | | 68 -> 42 |
| wave | 0.837 -> 0.956 | 9.8% (max 33.7) -> 0.14% (max 0.8) | | 103 -> 86 |
| lean | 0.902 -> 0.989 | 55% -> 0.21% | | 248 -> 21 |
| leave | 0.731 -> 0.877 | 1.3% -> 0.03% | | |
| empty | nothing marked | 0 | | |

Silhouette movement on the still clip: 43 px -> 0.22 px (p95). Models take
5.2 to 5.8 ms per depth result.

Looked at as a viewer: the wall, curtains, bed and chair are untouched in
every dumped frame. What is not fixed:

- The shadows on the person are still blotchy and still move (head and wave
  rows above). That is the shadow march and the light position, stage 3.
- Wave: the palm is found, the motion-blurred fingers are not (5.5% of the
  person missed, p95 20%), and the palm renders as a dark patch.
- Leave: a person standing with their back turned at the frame edge is not
  found (58% missed) and is simply left unlit.
- The remaining 0.1-0.2% of room pixels are the soft mask edge.

Frame time live, after (20 s each): 720p 59.9 fps, 16.70 ms mean, 17.94 ms
worst; 1080p 59.8 fps, 16.74 ms mean, 21.41 ms worst. The render thread is
unaffected. Depth results fell from 25.5 and 23.9 a second to 22.1 and 21.5,
which is the two models on the worker thread. One 720p run had a single
1668 ms frame that two repeats did not reproduce; cause unknown.

The user's review, live: the room looks better, the silhouette holds when
moving and leaning, going unlit when turned away is acceptable. The
half-covered waving hand "looks kinda bad" and is carried forward as an
open problem.

**Stage 3 - better shadows on the person (done 2026-11-10).** Two earlier conclusions were wrong and are corrected here:

- D3 gave the room a wrapped diffuse term and let the person keep a hard
  terminator "because its normals are real". On the live feed they are not:
  `depthRelief` is 0.02 there, so the person's normals are almost flat and
  the diffuse term is in effect `(P.z - light.z)/dist`, clamped at zero. The
  blotches on the face were mostly not cast shadows at all. They were that
  clamp, drawn wherever the face's depth crossed the light's depth, moving
  with the depth noise.
- The stage 1 and 2 measurements used the analysis's default light (centre
  of the frame, 1.15x near), which sits inside the head. That is the worst
  case for the clamp above and not where anyone puts a light. Stage 3 is
  measured at three positions: front (up and left, in front of the person),
  behind, and that default.

What changed, all in `lighting.fs` unless noted:

1. Soft terminator on the person (`terminatorWrap`, 0.35 live, 0 where
   depth is exact so the synthetic scenes are unchanged).
2. Normals: a normal facing away from the camera is flipped (a depth map has
   no back faces), and a depth difference steeper than a slope of 4 is
   believed less and less, down to nothing at 12. Depth steps inside the
   person (neck to collar, body to chair) are smeared into ramps by the
   small depth map and were giving normals lying on their side: black lines
   along every step and speckle on the desk edge.
3. The march uses ramps instead of thresholds for "behind the surface" and
   "inside the slab", and a soft caster mask.
4. Shadows are marched once per frame in their own pass at 640 wide, blurred
   and averaged over time (0.10 s), and the relight shader reads the result
   (`shadowpass.c`, `shadowfilter.fs`). Texels on a depth step receive no
   shadow.
5. Planned but not needed: fading cast shadows when the light is behind.
   The diffuse term is already zero there, so the shadow multiplies nothing.
   Stage 4 must keep the rim and spill out of the shadow term.

Person flicker in the shadow term and in the output picture (mean, with p95
in brackets), stage 2 code -> stage 3 code, same clips, same lights:

| clip, light | shadow term | output picture |
|---|---|---|
| still, front | 0.005 -> 0.001 | 1.18 -> 1.23 |
| still, behind | 0.34 -> 0.24 | 0.24 -> 0.24 |
| still, default | 2.79 (5.8) -> 0.19 (0.55) | 1.61 (3.3) -> 1.48 (2.6) |
| head, front | 0.25 (1.5) -> 0.13 (1.0) | 2.73 -> 2.86 |
| head, behind | 0.86 (3.0) -> 0.86 (2.8) | 0.47 |
| head, default | p95 42 -> 16.5 | p95 5.0 -> 6.8 |
| wave, front | 3.40 (12.0) -> 1.44 (6.1) | 8.64 -> 8.25 |
| wave, behind | 6.59 (24.1) -> 7.08 (17.7) | 3.57 |
| wave, default | p95 86 -> 55 | p95 14.8 -> 13.8 |

Lean and leave at the front light: shadow term 0.49 and 0.89, room shadowed
0.11% and 0.01%. Empty room: nothing. Room shadowed stays under 0.21% in
every run.

Read honestly: the shadow term is calmer everywhere it was bad, most at the
default light. The output picture's flicker number hardly moves at the front
and behind lights, because there the stage 2 picture was already steady and
what is left is the camera's own noise and the person moving. The visible
gain at those lights is not in that number: the black lines along the
collar, shoulders and chair are gone, and so is the speckle on the desk
edge. At the default light the face is evenly lit where it was blotched.
Not fixed: with the light inside the head the top of the head still goes
dark, and the wave is still the roughest clip (the hand is half masked).

Frame time live, after (20 s each): 720p 59.9 fps, 16.70 ms mean, 17.60 ms
worst; 1080p 59.8 fps, 16.73 ms mean, 20.82 ms worst. Unchanged from stage
2. Depth results 22.5 and 22.3 a second.

The user's review, live: the face "looks so much better", brightness changes
smoothly as the light moves, the lines are "96% good" with some smaller
artefacts left. Photo mode could not be judged against how it looked
before. Asked for in passing, and the first item of stage 4: the light lit
the room from the side but the source itself was nowhere to be seen. The
user also found the PC laggy during and after the test; nothing of ours was
running afterwards and CPU and GPU were idle when checked, cause not found.

**Stage 4 - a light you can see (done 2026-11-10).** Until now the shader
only lit surfaces, so a light with nothing near it was invisible. The source
is now drawn, all in one block at the end of `lighting.fs`, after the shadow
term so a shadow cannot eat it:

1. A bright disc and two glows (one tight, one wide and capped). The disc
   has a fixed physical size, so it is a small sun far away and a large lamp
   close up.
2. The person hides it: mask times a ramp around the light's depth. The
   room never hides it, because the room's depth cannot say whether a light
   is in front of a curtain or behind it.
3. Spill: the glow shows through along the person's outline.
4. Rim: a thin line of light on the inside of the outline, from the mask's
   edge and not from the normals, strongest where the outline is near the
   light in the picture.
5. Four sliders (Source glow, Source size, Spill, Rim light) and a taller
   window to hold them.

Two things were wrong on the way:

- The first rim was invisible. The edge measure used taps 13 px out and
  came out about 10 px wide, inside the mask's soft fringe.
- The second was an even grey line around the person and the chair, like a
  cut-out. It shared the spill's wide band, had a floor all the way round,
  and leaned on the outline facing the light, which also meant a light
  straight behind the head gave no rim at all. The rim now has its own thin
  band and follows nearness to the light; facing it only adds.

Person flicker in the output picture, without -> with the drawn light (mean,
p95 in brackets), light partly behind the head unless noted:

| clip | without -> with |
|---|---|
| still | 0.235 -> 0.239 |
| still, light at the side | 0.526 -> 0.537 |
| head | 0.47 (0.97) -> 0.57 (1.29) |
| lean | 0.43 (0.94) -> 0.82 (1.50) |
| wave | 3.59 (11.1) -> 4.72 (13.5) |

The still and head rows are at the final defaults; lean and wave were
measured before spill and rim were turned down and not rerun. Sitting still
the source costs nothing. Moving in front of it raises the number; part of
that is the head really covering and uncovering a bright disc and part is
the mask's edge against a bright background, and the number cannot tell
them apart.

Frame time live, after (20 s each): 720p 60.0 fps, 16.69 ms mean, 17.77 ms
worst; 1080p 59.8 fps, 16.73 ms mean, 22.98 ms worst, 4 of 1197 frames over
20 ms. Means unchanged from stage 3; the 1080p worst frame was 20.82 then.
Depth results 22.5 and 21.3 a second (22.3 at 1080p before). Measured with
spill 0.6 and rim 1.0, before they were turned down.

The user's review, live: the head blocks the light well enough, though some
hair sticks out past the mask's edge; steady while moving; the source is
always visible and acceptable in front too; the edge on top of the head
glowed too much with the light behind it. Spill and rim defaults went from
0.6 and 1.0 to 0.4 and 0.6 after that. Not seen by the user yet: those
defaults, and the taller window (the colour picker overlapped the hint text
and the frame counter at 860).

Not fixed: hair outside the mask is not rimmed and not hidden behind; a
backlit person is nearly black unless Ambient is raised; the light shows
through the half-masked waving hand; with nobody found there is no spill
or rim.

**Stage 5 - write-up (done 2026-11-10).** README: the depth section
described the old depth-band subject selector and is rewritten for the two
segmentation models, with a Backlight subsection, its limits and one
picture. The picture is the bundled stock portrait, not footage of the
user, who asked for that. The app only lights the photo in luminance mode,
so `--clip-from-image` turns a picture into a clip and `--analyse-clip`
replays it down the live path; the middle panel is that replay with the
drawn light switched off (`nosource`), not the code as it was before the
branch. `tools/get-person-models.ps1` is new: until now a fresh clone had no
way to fetch the two person models and would have fallen back to the
depth-band guess without saying why it looked worse.

Verdict on merging. Against the three complaints the branch started from:

- "Can't see the light": fixed. Judged live as always visible, blocked
  well enough by the head, steady.
- "Shadows land everywhere": fixed. Under 0.35% of room pixels shadowed in
  the worst frame of any run, from 5 to 55% on average.
- "Shadows on the person look bad": much better, not finished. "96% good"
  live; rough with movement and with the light inside the head.

Frame time is where it started or better (16.7 ms at both sizes, from 16.87
and 17.39); depth results fell from about 25 to about 22 a second, the cost
of the two person models. It is fit to merge as an experimental mode, which
is how the README labels it. It is not a finished effect: the waving hand,
loose hair and a person with their back turned are all visibly wrong, and
all three are the mask's limits, not the lighting's. Everything was tuned
and judged in one room, on one camera, with one person, which is the
biggest unknown.

#### Expectation to set now

Everything in phases A to C was judged against perfect synthetic depth with
exact silhouettes. ML depth is smooth and tends to bleed across depth
discontinuities, which is precisely the failure that hurts the rim-light
effect most. It will look worse than the test scenes. The question D2 answers
is whether it still looks *good*.

### Known risk, recorded up front

The effect lives at silhouette edges, and silhouette edges are the hardest
place for every candidate depth source - stereo matching fails at occlusion
boundaries, and ML depth is smooth and tends to bleed across them. Phase A's
synthetic scenes have perfect edges, so **phases A to C will look better than
any real depth source will.** Judge the *effect* on synthetic depth; judge
the *quality* only once real depth is in.

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
