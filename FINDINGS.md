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
