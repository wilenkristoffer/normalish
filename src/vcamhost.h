#ifndef VCAMHOST_H
#define VCAMHOST_H

#include <stdbool.h>

// Owns the virtual camera from normalish's side: creates the device, and
// publishes relit frames for the media source DLL to pick up.
//
// Like webcam.h, this deliberately keeps raylib out, because windows.h and
// raylib.h declare conflicting names. Frames are handed over as a plain byte
// buffer.

// True when the machine supports software virtual cameras at all.
bool IsVirtualCameraSupported(void);

// True when the media source DLL is registered. Registration needs elevation,
// so the app can offer to do it rather than failing opaquely.
bool IsVirtualCameraRegistered(void);

// Creates the camera and makes it visible to other applications. It exists
// only while this process holds it, so nothing lingers after exit.
bool StartVirtualCamera(void);
void StopVirtualCamera(void);
bool IsVirtualCameraRunning(void);

const char *GetVirtualCameraStatus(void);

// Publishes one frame. Pixels must be BGRA, top row first.
bool PublishVirtualCameraFrame(const unsigned char *bgra, int width, int height);

// The resolution the media source advertises; frames must match it.
int GetVirtualCameraWidth(void);
int GetVirtualCameraHeight(void);

#endif
