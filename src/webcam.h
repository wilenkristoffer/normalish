#ifndef WEBCAM_H
#define WEBCAM_H

#include <stdbool.h>

// Minimal webcam capture on top of Windows Media Foundation. Media Foundation
// is used rather than a third-party library because it ships with Windows, is
// callable from plain C, and adds no dependency to the build.
//
// This module deliberately does not include raylib.h: windows.h and raylib.h
// declare conflicting names (Rectangle, LoadImage, CloseWindow), so the two
// are kept in separate translation units and frames are handed over as a
// plain byte buffer. That also keeps the interface free of anything
// Windows-specific, so a V4L2 or AVFoundation backend could sit behind it.
//
// Capture runs on its own thread, because Media Foundation's synchronous
// ReadSample blocks until the camera produces a frame and would otherwise
// drag the render loop down to the camera's frame rate.
//
// Lifecycle: InitWebcamSystem once, then OpenWebcam / CloseWebcam as often as
// needed to switch devices, then ShutdownWebcamSystem at exit.

// Starts Media Foundation and enumerates the available capture devices.
bool InitWebcamSystem(void);
void ShutdownWebcamSystem(void);

int GetWebcamDeviceCount(void);
const char *GetWebcamDeviceName(int index);

// Opens one device by index and starts capturing. Closes any device already
// open, so it doubles as the way to switch source.
bool OpenWebcam(int deviceIndex, int preferredWidth, int preferredHeight);
void CloseWebcam(void);

int GetWebcamWidth(void);
int GetWebcamHeight(void);

// Human-readable state, for display when something did not work.
const char *GetWebcamStatus(void);

// Copies the newest frame as RGBA into 'destination', which must hold at
// least GetWebcamWidth()*GetWebcamHeight()*4 bytes. Returns false when no new
// frame has arrived since the last call, leaving the buffer untouched.
bool UpdateWebcamFrame(unsigned char *destination);

#endif
