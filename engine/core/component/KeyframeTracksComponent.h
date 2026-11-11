// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#ifndef KEYFRAMETRACKS_COMPONENT_H
#define KEYFRAMETRACKS_COMPONENT_H

#include "Engine.h"
#include "action/Ease.h"

namespace quantum{

    struct QUANTUM_API KeyframeTracksComponent{
        std::vector<float> times;
        // Per-segment easing: easings[i] shapes the interpolation from key i to
        // key i+1. Empty or shorter than the segment count means linear there.
        // GLTF LINEAR clips leave this empty; GLTF STEP clips fill it with
        // EaseType::STEP. On tracks with Hermite tangents (GLTF CUBICSPLINE)
        // the eased factor feeds the cubic curve, so leave this empty for
        // spec-faithful playback.
        std::vector<EaseType> easings;
        bool loop = false; // starts over after the last key
        // values add to the target's pose when the track starts: position plus value,
        // rotation times value and scale times value
        bool relative = false;
        int index = 0;
        float interpolation = 0;

        bool hasBase = false; // the base was taken when the track started
        Vector3 basePosition;
        Quaternion baseRotation;
        Vector3 baseScale = Vector3::UNIT_SCALE;
    };

}

#endif //KEYFRAMETRACKS_COMPONENT_H