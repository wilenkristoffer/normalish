# normalish

*Surface normals, approximately.*

A movable artificial light source for photos and live webcam video. There is
no 3D model and no depth sensor - the surface relief is guessed from image
brightness, which is nowhere near physically correct but looks convincing
under a moving light. Hence the name.

Written in C with raylib, with the lighting and the normal derivation both
living in a GLSL fragment shader.

## Why this project

- First time using C.
- First time using raylib.
- Goal is to learn both by building something visual and satisfying rather
  than starting with dry exercises.

## Core idea

1. Take an image - either a still photo or a live webcam frame - as the
   albedo.
2. Derive surface normals from it by treating pixel luminance as a
   heightmap and Sobel-filtering the result. This happens per pixel in the
   fragment shader, so it keeps up with live video.
3. Light it with a movable point light, Blinn-Phong style: ambient +
   diffuse + specular, with distance falloff.
4. Move the light in real time and watch the shading respond as if the
   image were a 3D relief.

The end goal, now working, is the live webcam feed: an artificial light you
move around that shines on you in real time. The still photo was the
stepping stone used to get the lighting maths right.

Note on realism: single-photo normal estimation is not physically accurate
(that's an open research problem). The luminance-as-heightmap approach is a
well-known "cheat" from game dev (used for sprite/portrait relighting) that
looks convincing without needing ML-based normal prediction. We can revisit
a learned approach later if the cheat isn't good enough.

## Tech stack (decisions, revisit if they don't fit)

- Language: C (C11)
- Graphics/windowing: raylib 5.5, built from source via CMake FetchContent
  (pinned to tag 5.5, so builds are reproducible).
- Compiler: MSVC (Visual Studio Build Tools 2026, cl.exe 19.50). This was
  already installed on the machine, along with CMake 4.1 and Ninja 1.12,
  so the originally planned w64devkit + Makefile route was dropped - no
  downloads or extra installs needed.
- Build system: CMake + Ninja, wrapped by `build.ps1` so no "Developer
  PowerShell" window is required.
- Normal map generation: a small standalone tool (Phase 2), not part of the
  real-time app. Keeps the shader/runtime code focused on rendering, and lets
  us regenerate normal maps offline while iterating on the algorithm.

## Verified GPU / GL capabilities (from the Phase 0 run)

- Renderer: AMD Radeon RX 7800 XT
- raylib context: OpenGL 3.3 Core, GLSL 4.60 available
- Shaders in this project therefore target `#version 330`

## Project structure (will grow as phases land)

```
normalish/
  README.md
  PLAN.md
  CMakeLists.txt   build definition (also fetches raylib and raygui)
  build.ps1        build/run wrapper
  src/             main.c and webcam capture
  shaders/         GLSL fragment shaders
  assets/          input photos
  build/           generated - build output and fetched raylib (not tracked)
```

## Status

All five planned phases complete. A movable point light relights either a
still photo or the live webcam feed in real time, at 60 FPS, with a control
panel for tuning the look. See [PLAN.md](PLAN.md) for the phase-by-phase
record, including where the plan turned out to be wrong.

## Webcam

[src/webcam.c](src/webcam.c) captures from the first video device using
Windows Media Foundation - no third-party library, since Media Foundation
ships with Windows and is callable from plain C.

Capture runs on its own thread, because Media Foundation's synchronous
`ReadSample` blocks until the camera has a frame ready and would otherwise
hold the render loop down to the camera's frame rate.

Use the **Photo / Webcam feed** toggle at the top of the panel to choose
what gets lit. "Mirror" makes the feed behave like a mirror, which is what
you want when you are the subject.

Two dropdowns control capture, and both take effect immediately - closing
and reopening the camera takes a moment but needs no restart:

- **Device** lists every capture device Media Foundation reports, by its
  Windows friendly name. This matters on machines with both a real camera
  and a virtual one such as OBS, where picking the first device found is a
  coin flip. Note that raygui makes a single-item dropdown deliberately
  inert, so it will not open if you only have one camera.
- **Resolution** offers 480p, 720p and 1080p, defaulting to 480p. The camera
  decides what it can actually deliver, so the label above reports the mode
  it settled on rather than what was asked for.

1080p holds 60 FPS on an RX 7800 XT. Camera resolution does not affect the
shader at all - it costs CPU work per frame (format conversion, the BGRA to
RGBA swizzle, two copies and the texture upload), roughly 8 MB per frame at
1080p against 1.2 MB at 480p, and most of it on the capture thread.

Higher resolution needs the relief retuned. Sobel measures *per-pixel*
differences, so at 1080p a given feature spans more pixels, each step is
smaller, and the relief reads weaker while sensor noise becomes relatively
stronger. Expect to raise both Relief and Smoothing.

4K is deliberately not offered: at 8.3M pixels the per-frame CPU cost is
four times 1080p, and 4K webcams typically only offer it at low frame rates
with heavy compression.

There is also an optional **"Light follows brightest"** mode, which tracks
the brightest point in the camera image and drives the light position with
it - so a phone torch in the room moves the virtual light. It locks onto
whatever is brightest, which is often daylight on a wall rather than you,
so a torch works far better than relying on ambient room light.

## The derived normals

Normals are computed in [shaders/lighting.fs](shaders/lighting.fs), per
pixel, from whatever image is currently being lit. There is no normal map
texture and no preprocessing step: the shader samples the albedo's eight
neighbours, takes their luminance as height, and Sobel-filters that into a
normal.

This started life as a CPU pass that built a normal map texture, which was
fine for a still photo but could not keep up with live video at 30 frames a
second. Moving it into the shader made one code path serve both, and made
the two controls free to change:

- **Relief** scales the slopes. Low is a subtle sheen, high is sculpted.
- **Smoothing** widens the spacing of the Sobel taps, which is what stops
  fine noise dominating the gradient. It replaces the old CPU blur: as the
  taps spread out, bilinear filtering averages for us.

Note that a good relief value depends on image resolution, since Sobel
measures per-pixel differences.

## Running against a different photo

```powershell
build\normalish.exe assets\photo.png
```

The argument is optional and defaults to `assets/portrait.png`.

Note that raylib's default build does not enable JPEG support, so source
images need to be PNG.

A good input photo is front-on, evenly and softly lit, and matte. The
approach reads brightness as height, so soft frontal light happens to be a
rough proxy for depth. Hard side light bakes a direction into the derived
"geometry" and fights the light you are trying to move; glare and glossy
highlights become permanent fake bumps; and glasses are the worst offender
because lens reflections read as raised blobs.

## Virtual camera

[src/vcam/](src/vcam/) is a Media Foundation media source DLL that exposes
"normalish camera" as a real camera device, so any application - Teams,
Zoom, a browser - can select it. It is the one part of the project that is
C++ rather than C: implementing a COM server in plain C means hand-rolling
vtables, reference counting and QueryInterface.

**It is adopted from [smourier/VCamSample](https://github.com/smourier/VCamSample)
by Simon Mourier, MIT licensed** - see
[src/vcam/LICENSE-VCamSample.txt](src/vcam/LICENSE-VCamSample.txt). A
from-scratch implementation reached the point where the camera appeared,
could be selected and negotiated a format, but never produced frames, and
eight rounds of comparing against that working source did not close the gap.
[PLAN.md](PLAN.md) records the whole attempt and why adopting was the better
call. Changed from the original: our CLSID, the packaged-app identity lookup
removed (normalish is unpackaged, and it was the only thing needing a
generated C++/WinRT projection header), and the build moved to CMake.

Registration needs one elevated step, because the DLL is loaded by the
Camera Frame Server service and by consuming applications, none of which
read a per-user registry hive:

```powershell
regsvr32 C:\ProgramData\normalish\normalish_vcam.dll      # elevated
regsvr32 /u C:\ProgramData\normalish\normalish_vcam.dll   # to undo
```

No camera appears in anyone's list until normalish is running with the
toggle on: the camera is created with `MFVirtualCameraLifetime_Session`, so
it exists only while the owning process holds it.

**If you change the DLL, restart the Frame Server service.** It caches a
device's configuration per CLSID, and will keep serving the old media types
and behaviour indefinitely - which makes code changes look like they had no
effect at all:

```powershell
Restart-Service FrameServer -Force    # elevated
```

## Assets and credits

`assets/portrait.png` is a photo by
[Ludovic Migneault](https://unsplash.com/@dargonesti) from
[Unsplash](https://unsplash.com/photos/man-in-blue-crew-neck-shirt-4uj3iZ5m084),
used under the [Unsplash License](https://unsplash.com/license), cropped
square and converted to PNG. It was chosen because it is softly lit, matte,
front-on and has no hard background edge - all of which suit this technique.

## Window and display scaling

The window is resizable, and the layout is recomputed every frame from the
current size, so nothing is pinned to a fixed resolution.

raylib sizes windows in *physical* pixels, which means on a display with OS
scaling everything is drawn smaller than intended - text on a 4K screen at
150% scaling comes out at two thirds the size it should be. The app reads
`GetWindowScaleDPI()` at startup and multiplies the window size, the raygui
font size and every layout measurement by that factor.

Resizing does **not** change the webcam resolution: capture stays at the
mode that was opened (typically 640x480), and only the number of screen
pixels the shader covers changes. Maximised to 3840x2054 on an RX 7800 XT it
still holds 60 FPS, so the per-pixel Sobel work is not a bottleneck at 4K.

The light is stored in normalised image coordinates rather than screen
pixels, so resizing the window leaves it where you put it on the subject.

## Demo mode

Tick **Demo** and the light drives itself, so the effect can be shown off
without holding the mouse. Two modes:

- **Basic** moves the light only, using whatever settings you have set by
  hand. Colour, height and everything else stay put.
- **Full** additionally drifts the light's height and hue on its own. Height
  is deliberately kept low, between 0.02 and 0.25: a light close to the
  surface rakes across the derived relief and is far more dramatic, where a
  high one flattens everything out.

The motion is two sine waves per axis at unrelated frequencies, summed.
That reads as wandering rather than as an obvious oscillation, and unlike
interpolating between random waypoints there is no momentary stop at each
target - the light never stalls. Phases are randomised at startup, so no two
runs trace the same path. Height and hue drift the same way, at slower
rates.

Your slider values are never overwritten, so switching the demo off gives
you back exactly what you had. While Full is running, the Height slider and
colour picker become live read-outs instead of inputs, greyed out to show
they are not yours to move at that moment - with a small swatch beside the
"Light colour" label showing the current colour, since raygui's disabled
styling would otherwise hide it.

## Controls

Most tuning lives in the raygui panel on the right: light intensity,
falloff, ambient, specular, shininess, height and colour, plus the normal
map's relief and blur.

| Input | Action |
| --- | --- |
| Mouse move | move the light (over the image, unless Demo is on) |
| Mouse wheel | raise/lower the light above the surface |
| 1 / 2 / 3 | view lit result / normals / albedo only |
| R | reload the shader from disk (no rebuild needed) |
| Esc | quit |

The light deliberately only tracks the mouse while the cursor is over the
photo, so it stays where you left it while you reach for a slider.

Shaders are loaded from the source tree (via a `SHADER_DIR` compile
definition), so editing `shaders/lighting.fs` and pressing R shows the
change immediately - useful when changing the lighting maths itself, now
that the numeric tunables are all uniforms driven by the panel.

## Build and run

```powershell
.\build.ps1          # build
.\build.ps1 -Run     # build, then run
.\build.ps1 -Clean   # wipe build/ for a full rebuild
```

The first build takes a few minutes because it clones and compiles raylib.
Later builds only recompile our own sources and take a second or two.

Requirements: Visual Studio Build Tools with the C++ workload, and git
(used by CMake to fetch raylib). `build.ps1` locates MSVC automatically via
vswhere and will tell you if it is missing.
