# Findings

Things that cost real time to learn, kept because they will bite again.
Organised by area rather than in the order they were discovered.
[PLAN.md](PLAN.md) has the phase-by-phase account; this file has the durable
facts.

## Windows virtual cameras (Media Foundation)

**The Frame Server caches a device's configuration per CLSID.** This is the
single most expensive thing in the project. The first `Start` registered an
early, incomplete media source, and the server then served that stale
configuration to every subsequent test - including an entirely different
implementation. Code changes appeared to have no effect whatsoever for four
rounds of debugging. After changing the DLL:

```powershell
Restart-Service FrameServer -Force    # elevated
```

If a change seems to do nothing at all, suspect that the thing under test is
not the thing you changed.

**The media source must be registered in HKLM, so registration needs
elevation.** HKCU is tempting and *appears* to work: it satisfies the
in-process activation that `IMFVirtualCamera::Start` performs first, so the
error moves on and looks like progress. It then fails later, when the camera
is published for other processes, as `ERROR_PATH_NOT_FOUND` - a Win32 path
error with nothing to do with paths. An error code changing means the first
obstacle moved, not that the goal is reachable.

**The registered CLSID must be an activation object, not the media source.**
Media Foundation asks the class factory for `IMFActivate` and then calls
`ActivateObject` to get the source, the same indirection
`MFEnumDeviceSources` returns for real cameras. Getting this wrong surfaces
only as `E_NOINTERFACE` from `CoCreateInstance`.

**The source must be constructed before Frame Server reads the activation
object's attributes.** Building it lazily inside `ActivateObject` is too
late: the attributes are inspected first, and anything the source adds to
them - notably the sensor profile collection - is not there yet.

**`MF_DEVICEMFT_SENSORPROFILE_COLLECTION` is required**, along with the
standard device-source attributes (`SOURCE_TYPE`, `FRIENDLY_NAME`) and
`MF_DEVICESTREAM_FRAMESERVER_SHARED` on the stream. Missing attributes
surface as `MF_E_ATTRIBUTENOTFOUND` from `Start` with no indication of which
one, so logging every attribute lookup that misses is the fastest way to
find out.

**`GetAllocatorUsage` should report `UsesProvidedAllocator`** and samples
should come from the allocator Frame Server hands over, rather than from
`MFCreateSample`.

**Most consumers want NV12**, though offering RGB32 as well is what real
cameras do; the working reference advertises both.

**Instrument the DLL with a log file.** It is loaded into the Frame Server
service, where there is no console and nothing to attach a debugger to. Both
hard blockers here were found by logging what was asked of the DLL, not by
reasoning about error codes. Note that an attribute store handed out by
`GetSourceAttributes` is read *directly* by the caller, so its reads bypass
any logging on the object that returned it - silence there proves nothing.

## Media Foundation, generally

- **`MFGetAttributeSize` and `MFSetAttributeSize` are not exported by any
  library in the current SDK.** `MF_MT_FRAME_SIZE` and friends are packed
  UINT64s with the width in the high 32 bits; pack and unpack them by hand.
- **`MFEnumDeviceSources` lives in `mf.lib`**, not `mfplat.lib`.
  `MFCreateSensorProfile*` needs `mfsensorgroup.lib`.
- **`IKsControl` comes from `ksproxy.h`**, not `ks.h`, which declares it
  behind a guard a normal user-mode build does not satisfy.
- **Synchronous `ReadSample` blocks** until the camera produces a frame, so
  calling it from a render loop pins the whole application to the camera's
  frame rate. Capture belongs on its own thread.
- **Pick a capture mode by aspect ratio first, size second.** Scoring purely
  on pixel count picked a 4:3 mode on a camera whose other modes were all
  16:9. A wrong shape cannot be undone later; a wrong size is only scaling.
  Also set the output type's frame size explicitly, or MF picks its own
  default and quietly reshapes the capture.
- **`CoInitializeEx` returns `RPC_E_CHANGED_MODE`** when the thread is
  already in a different apartment - which it is, because GLFW has made the
  process STA. That is not a failure; treating it as one made an entire
  feature report itself unsupported.

## Cross-process frame transport

The media source runs in the Frame Server service, in session 0 under
another account. That rules out the obvious options: a `Local\` shared
memory name is invisible to it, and a `Global\` name needs
`SeCreateGlobalPrivilege`, which a normal user does not have.

**Two processes mapping the same file share pages with no namespace and no
privilege** - only file permissions, and ProgramData is writable by the user
and readable by services. A seqlock (sequence number odd while writing)
publishes frames without needing a cross-session event either.

**Never fall back to a placeholder on a single missed frame.** Reusing the
previous frame is invisible; a test pattern flickering in is not. Fall back
only after about a second of failures, when the producer has most likely
gone away.

## raylib

- **`SetShaderValueTexture` must be called inside `BeginShaderMode`.**
  `BeginShaderMode` flushes the render batch, and `rlDrawRenderBatch` clears
  the registered auxiliary texture units, so a binding made before it is
  silently discarded and the sampler reads an unrelated texture. No error,
  just wrong pixels.
- **JPEG support is off in the default build** (`SUPPORT_FILEFORMAT_JPG` is
  commented out in `config.h`), so source images need to be PNG.
- **raylib sizes windows in physical pixels.** On a display with OS scaling
  everything is drawn smaller than intended; read `GetWindowScaleDPI()` and
  scale the window and every UI measurement by it.
- **Render textures are bottom-up**, so a GPU readback needs a vertical flip.
- `windows.h` and `raylib.h` declare conflicting names (`Rectangle`,
  `LoadImage`, `CloseWindow`). Keep Win32 code in its own translation unit
  and hand data across as plain buffers.

## raygui

- **It only draws, in the order you call it.** There is no z-order: an open
  dropdown does not paint above controls declared after it.
  Reserve the dropdown's rectangle during layout, draw it last, and
  `GuiLock()` everything else while it is open so clicks cannot fall through.
- **A single-item dropdown is deliberately inert** (`itemCount > 1` in
  `GuiDropdownBox`). On a machine with one camera it will not open, which
  looks like a bug until you know.
- Controls drawn directly rather than through raygui do not advance the
  layout cursor, so later controls will overlap them.

## WRL

**`ChainInterfaces` is required, not cosmetic.** WRL only answers
`QueryInterface` for IIDs listed explicitly, so declaring only the
most-derived interface makes QI for its bases fail with `E_NOINTERFACE`. Use
`ChainInterfaces<IMFMediaSourceEx, IMFMediaSource, IMFMediaEventGenerator>`
and likewise for streams.

## ONNX Runtime

- **The DirectML provider needs `DirectML.dll` shipped alongside
  `onnxruntime.dll`.** The `Microsoft.ML.OnnxRuntime.DirectML` NuGet package
  does not contain it; it declares a dependency on `Microsoft.AI.DirectML`
  instead. Windows does ship a `DirectML.dll` in System32, but not
  necessarily the version ONNX Runtime was built against.
- **NuGet packages are plain zips**, fetchable by URL from
  `nuget.org/api/v2/package/<id>/<version>`, so CMake FetchContent can pull
  them without nuget.exe - which is not installed here.
- The C API needs only `onnxruntime.lib` to link; the DirectML provider is
  selected with `OrtSessionOptionsAppendExecutionProvider_DML` from
  `dml_provider_factory.h`.
- **The DirectML provider requires `DisableMemPattern` and
  `ORT_SEQUENTIAL` execution mode**, both set on the session options before
  the session is created.
- `CreateSession` takes `ORTCHAR_T*`, which is `wchar_t*` on Windows.
- Read input and output names off the model with `SessionGetInputName` and
  `SessionGetOutputName` rather than hardcoding them.
- **The first inference costs roughly double**, paying for shader
  compilation and allocation. Time a warm-up run separately or the numbers
  are wrong.

## Depth maps

- **Normals from a monocular depth model are dominated by the model's own
  error.** At 2.5 m a 518-wide depth map spans roughly 7 mm of scene per
  texel, so a 5 mm depth error between neighbours tilts a normal by 36
  degrees. These models are trained for depth accuracy, not gradient
  accuracy, and their surfaces undulate by centimetres. Attenuating the tilt
  (scale the normal's lateral components, renormalise) is what works.
- **Widening the gradient taps makes that worse, not better**, because the
  error is low-frequency: it converts fine noise into large blobs.
- **Take the gradient at the depth map's own texel size**, not the colour
  image's. Sampling inside a single depth texel reads the bilinear
  interpolation, and the normals then follow the interpolation grid.
- **Nothing may assume a scene's scale.** A synthetic subject at 0.6 m and a
  real room running to 8 m break every hardcoded range - light distance,
  shadow thickness, shadow bias, march span, view mapping, slider bounds.
  Derive them from the depth source's own reported range, and measure falloff
  relative to the nearest surface so one setting travels between scenes.
- **Segmenting a person out of a depth map needs continuity plus an absolute
  band, and the band must be per region.** Continuity alone connects a person
  through their chair and the floor to the far wall. One global band anchored
  on the frame's nearest pixel fails whenever something else is nearer than
  the subject.
- **A single seed is not a person detector.** Label candidate regions and
  score them; the test that does the most work is penalising a region that
  covers most of the frame height at one side, which is what a curtain, wall
  or door frame looks like.
- **Nor is region scoring a person detector.** With nobody in frame, the
  office chair in the subject position is what gets selected: its headrest and
  back read as a head and shoulders in depth, and it is near, central, large
  and running out of the bottom of frame - every feature the scoring rewards.
  Separating a chair from a person needs an actual person or face model.
- **A "metric" monocular model's absolute scale can simply be wrong, by a
  lot.** Measured here: a subject sitting under 1 m from the webcam was
  reported at 3.10 m, with the scene's near plane at 2.46 m - out by roughly
  3x. The likely reason is the training distribution:
  Depth-Anything-V2-Metric-Hypersim is trained on synthetic *room-scale
  architecture*, and a head filling the frame at 70 cm is nothing like that,
  so it reads the scene as a larger room viewed from further back. Never
  present these numbers to a user as real distances.
- **A "metric" monocular model's absolute depth is not trustworthy.**
  Measured on two frames of the same room: five static points (wall, curtain,
  doorway) read 0.19 to 0.32 m nearer in one frame than the other, so the
  global scale drifts between inferences. Worse, at one pixel a chair back
  read 0.68 m *nearer* than the chest of the person who had been sitting in
  front of it - the sign is wrong, not just the magnitude. Use these depths
  for relative structure, and derive every parameter from the scene's own
  reported range; do not compare absolute values across frames, and do not
  build a decision on one.
- **A live camera is not a test fixture when the scene can change.** Three
  conclusions here were drawn from before-and-after screenshots of a room
  with other people moving in it, and each compared two different scenes: an
  empty chair that had supposedly moved, a speckle artefact that appeared to
  survive a fix, and a mask that seemed to cover the wrong thing. Grab one
  frame and render *that* repeatedly instead.
- **With a flat normal, `dot(N, L)` is proportional to `P.z - lightPos.z`**,
  so it crosses zero exactly at the light's depth plane and `max(dot, 0)`
  clamps hard there. Any surface lying near that plane flickers on and off
  with depth noise. A wrapped term that never reaches zero removes the
  terminator; the falloff still places the light.
- **Clamp the minimum distance in a point light's falloff.** The derivative
  of `1/(1 + f d^2)` is largest as d approaches zero, which is exactly where
  depth noise becomes speckle and where putting the light on a surface blows
  it out.
- **Do not explain a surprising number until you have measured why.** Two
  confident explanations were offered for a chair outscoring a person - that
  empty chairs are nearer than their occupants, and that the chair had been
  moved - and both were wrong, the first physically backwards and the second
  contradicted by the user. Probing identical pixels in both frames took a
  few minutes and gave the actual answer, which was about the model rather
  than the scene.
- **A monocular model's input size caps the depth resolution, so a higher
  capture resolution does not improve depth.** Measured from 1080p to 4K: the
  depth map stays 518 wide, the mask changes by 0.1% of frame, and the depth
  gradient is 4% smoother. Worth doing only for output sharpness.
- **Depth alone cannot say which thing is the person.** Scoring depth-band
  regions picked the curtain or a box for 1 to 4 frames at a time on a person
  sitting still (17 of 194 frames), and picked the chair in an empty room.
  Every flip flashed the face by about 46 levels. Earlier notes here treated
  this as tunable; it is not. Depth answers "how far", a segmentation model
  answers "who".
- **Depth trailing the picture by a frame is not what makes shadows jump.**
  Replaying a waving clip with the depth one frame late measured the same as
  with none. The causes were the mask flipping and the march itself.
- **Measure the live rate live.** A recorder writing 1080p frames to disk
  logged 21-24 fps and that was reported as the camera's rate. The camera
  delivers 39 new frames a second; the disk was the limit. Likewise the 44 ms
  inference recorded earlier was with the GPU shared; alone it is 23 ms.
- **The flat-normal terminator applies to the person too, not only the
  room.** An earlier entry fixed it for the room and left the person on a
  hard clamp because "its normals are real". On the live feed the relief is
  scaled to 0.02, so they are nearly flat, and the clamp drew moving dark
  blotches on the face wherever its depth crossed the light's. Most of what
  looked like bad shadows was this. A terminator with a soft edge
  (`(ndl + w)/(1 + w)`, w = 0.35) turns the switch into a small change.
- **Test with the light where a person would put it.** The analysis default
  put the light inside the head, the worst case for the entry above, and two
  stages of numbers were taken there.
- **A depth map has no back faces, so flip any normal that points away.** On
  a surface seen nearly edge-on the neighbouring point lands on the wrong
  side and the cross product changes sign. Those texels were lit from
  behind: black lines and speckle.
- **A depth step is a ramp, and a ramp is not a surface.** The 518-wide map
  stretched over 1080p smears every step over a few texels. Picking the
  better of the two one-sided differences does not help a texel in the
  middle of the ramp; both sides are steep. Believe a difference less as its
  slope passes 4 and not at all past 12, and give those texels no shadow.
- **Thresholds on noisy depth flip; ramps do not.** The march's "behind the
  surface" and "inside the slab" tests are smoothsteps now.
- **March shadows once, small, then blur and average over time.** At 640
  wide the march is already finer than the depth map, and a 0.10 s average
  on the shadow term alone does not smear the picture.
- **Two render-texture passes cancel raylib's vertical flip.** The raw march
  is drawn flipped, the filter pass flips it back, so the history is upright
  in the picture's own coordinates and is read with `gl_FragCoord`.
- **`cast` is a reserved word in GLSL.** A variable of that name fails to
  compile, and since shaders are loaded at run time from the source folder
  the build still succeeds. Check the log for `SHADER` errors after a shader
  edit.

## Person segmentation

- **MediaPipe's selfie models run fine under ONNX Runtime** from community
  conversions, all Apache 2.0: landscape (256x144, 462 kB, 2 ms on two CPU
  threads, no faster on the GPU), square (256x256, 462 kB, 4 ms CPU) and
  multiclass (256x256, 16.5 MB, 3.5 ms on DirectML, 67 ms on the CPU). Input
  is RGB scaled to 0..1 with no mean or deviation.
- **Read the layout off the model.** The same network is published NCHW by
  one converter and NHWC by another, and the multiclass output is six class
  scores (background, hair, body skin, face skin, clothes, others), not one
  person channel. Person = 1 - softmax(background).
- **Each model fails differently, and none is enough alone.** Landscape is
  solid on a torso in any clothing but drops a hand raised away from the
  body and marks bright windows and half of an empty chair. Multiclass is
  sure about heads and hands and never took furniture for a person, but
  barely marks a dark top against a dark chair. Union of both, kept only
  where connected to pixels multiclass is sure of and not far behind them in
  depth, held up on five of six clips.
- **Squash, do not letterbox, a 16:9 frame into the square multiclass
  model.** Letterboxing recovered a little of the shoulders but produced a
  large false region on a bed behind a waving hand.
- **A person with no face in view is not found.** Back turned in dark
  clothes at the frame edge, or bent close to the lens: both models give
  little or nothing. The mask going empty is the graceful outcome.
- **An empty-room difference image is a poor reference for "where the person
  is"** when clothes and furniture are equally dark, and wrong once the
  person has moved the furniture. Trust its trends, not its absolutes, and
  look at the frames.
- **Run the segmentation models before taking the lock, and feed them the
  picture the depth network already has.** The worker undoes the ImageNet
  normalisation on the 518x291 crop instead of keeping a second copy of the
  camera frame, so mask and depth come from exactly the same frame. Only the
  cheap combining step runs under the lock the render thread waits on.
- **Gate the mask with this frame's raw depth, not the smoothed depth.** The
  smoothed map trails a moving arm, and the "not far behind the person" test
  then cuts the arm off.
- **A fixed temporal average is wrong for a mask.** It trails a moving arm
  with a ghost. No average lets a one-pixel wobble at the silhouette crawl.
  Scale the blend by the size of the change: small changes are averaged
  (0.3), a pixel that flips completely follows at once. Remaining flaw: the
  soft rim of a region that vanishes fades over about five results, a faint
  outline in the mask view that was not visible in the lit picture.
- **Hold the last mask for two results when nobody is found.** A single
  missed frame otherwise blinks the whole relight off.

## Build

- **`UNICODE`/`_UNICODE` must be defined** for code written against a Visual
  Studio project, or every Win32 call resolves to its `...A` variant and
  every wide string argument fails to compile.
- C++/WinRT's `winrt/base.h` **ships in the Windows SDK** and is already on
  the include path, so no NuGet is needed unless generated projection
  headers are used. WIL is header-only and comes from FetchContent.
- PowerShell turns a native command's stderr into a terminating error when
  piped, which made a successful CMake configure look like a failure. Pass
  `-Wno-deprecated` to silence warnings that are not ours to fix.
