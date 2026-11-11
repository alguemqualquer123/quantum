// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#ifndef DECAL_COMPONENT_H
#define DECAL_COMPONENT_H

#include "math/Vector3.h"
#include "texture/Texture.h"
#include "util/Color.h"

namespace quantum{

    // Projects an optional albedo texture through the entity's local box onto
    // visible geometry. RenderSystem owns the transient screen-space pass.
    struct QUANTUM_API DecalComponent{
        bool enabled = true;
        Vector3 color = Color::sRGBToLinear(1.0f, 1.0f, 1.0f);
        Vector3 size = Vector3(1.0f, 1.0f, 1.0f);
        float opacity = 1.0f;
        float depthFade = 0.08f;
        Texture texture;
    };

}

#endif // DECAL_COMPONENT_H
