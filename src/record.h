#ifndef RECORD_H
#define RECORD_H

// Runs the guided recording session: a preview window that walks through a
// fixed list of poses and writes one clip per pose into 'directory'. Expects
// the raylib window to exist already. Returns 0 on success.
int RunRecordSession(const char *directory, int captureWidth, int captureHeight);

#endif
