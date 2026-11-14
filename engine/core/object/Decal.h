// (c) SR VINIX
// SPDX-License-Identifier: MIT

#ifndef DECAL_H
#define DECAL_H

#include "Object.h"
#include "component/DecalComponent.h"

namespace quantum {

    // Script-facing owner for the existing projected decal component. Keeping
    // the component behind an EntityHandle lets runtime fixtures author the
    // same data path as an editor scene without exposing ECS internals.
    class QUANTUM_API Decal : public Object {
    public:
        Decal(Scene* scene);
        Decal(Scene* scene, Entity entity);
        virtual ~Decal();

        void setEnabled(bool enabled);
        bool isEnabled() const;
        void setColor(Vector3 color);
        Vector3 getColor() const;
        void setSize(Vector3 size);
        Vector3 getSize() const;
        void setOpacity(float opacity);
        float getOpacity() const;
        void setDepthFade(float depthFade);
        float getDepthFade() const;
        void setTexture(const std::string& path);
        std::string getTexture() const;
    };

}

#endif // DECAL_H
