// (c) Eduardo Doria
// SPDX-License-Identifier: MIT

#include "slate/core/SlateTypes.h"

#include <algorithm>

namespace doriax::slate {

    //UMG CanvasPanel slot math: the anchors pick the edges of `reference` the
    //slot is glued to, the offsets push the rect away from those edges in
    //pixels. Negative resulting sizes are clamped to 0 rather than flipped,
    //because a slot with inverted offsets means "no area", not "mirror".
    //`pivot` is not applied here: it only names the point a future rotation or
    //scale would turn around, so the resolved rect stays anchor/offset only.
    Rect resolveAnchorRect(const AnchorData& anchor, const Rect& reference) {
        float minX = reference.getX() + anchor.anchors.x * reference.getWidth();
        float maxX = reference.getX() + anchor.anchors.z * reference.getWidth();
        float minY = reference.getY() + anchor.anchors.y * reference.getHeight();
        float maxY = reference.getY() + anchor.anchors.w * reference.getHeight();

        float left = minX + anchor.offsets.x;
        float top = minY + anchor.offsets.y;
        float width = std::max(0.0f, (maxX - minX) - anchor.offsets.x - anchor.offsets.z);
        float height = std::max(0.0f, (maxY - minY) - anchor.offsets.y - anchor.offsets.w);

        return Rect(left, top, width, height);
    }
}
