// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#ifndef Wind_h
#define Wind_h

#include "Export.h"
#include "math/Vector2.h"

#include <cstdint>

namespace quantum {

    class Scene;

    // The wind a scene authored, evaluated at one place at one instant. It is a pure function of
    // (settings, point, seconds): no generator state, no rand(), nothing to seed, so the same scene
    // at the same second gives the same answer in the editor, in a headless probe and in a shipped
    // build. The gust is the part that is not just a number: it travels downwind over the water, so
    // a squall crosses a lake instead of every cell blinking at once.
    class QUANTUM_API Wind {
    public:
        // Metres per second of air moving over a world point, along X and Z. Air moves sideways, so
        // there is no vertical component here; a script that wants a downdraft writes its own. Zero
        // when the scene has wind off or is calm.
        static Vector2 sample(const Scene& scene, const Vector2& point, float seconds);

        // The same wind as a magnitude. It costs the same field evaluation as sample, so ask for one
        // or the other per call site, never both in a loop.
        static float speed(const Scene& scene, const Vector2& point, float seconds);

        // The heading the scene's wind blows toward, in radians, with no gust on it. A gust turns the
        // air a little: see the deviation of the two from each other in sample's field.
        static float direction(const Scene& scene);
    };

}

#endif
