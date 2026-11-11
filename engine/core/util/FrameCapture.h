// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#ifndef FrameCapture_h
#define FrameCapture_h

#include "Export.h"

#include <string>

namespace quantum {

    // One frame of the window, written out as a PNG.
    //
    // The read has to happen after the last pass is submitted and before the platform swaps:
    // sokol's GL backend is immediate, so that is the only moment the backbuffer holds the image
    // the user is about to see. Engine::runFrame calls runPending() at exactly that point, which is
    // why a request is served on a later frame - by design, so a script can set up a state and have
    // the picture show it rather than the frame it was requested in.
    //
    // Without this there is no way to see anything a headless run produces: the engine has no
    // framebuffer read path at all, and the rule against launching the editor from a script or an
    // agent means a PNG is the only image a graphics change can be argued from.
    class QUANTUM_API FrameCapture {
    public:
        // Capture the frame `delay` frames from now (0 = the next frame that finishes).
        // A second request replaces the first; the pending one is cleared once written.
        static void request(const std::string& path, int delay = 0);

        static bool isPending();

        // Serves the queued request, if any. Returns false when nothing was pending or the capture
        // failed; the reason is logged either way.
        static bool runPending();

        // Read the window's colour buffer and encode it now - for a caller already at the right
        // point in the frame.
        static bool captureTo(const std::string& path);
    };
}

#endif
