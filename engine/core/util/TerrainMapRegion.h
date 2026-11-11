// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#ifndef TerrainMapRegion_h
#define TerrainMapRegion_h

#include <algorithm>

namespace quantum {

    // Inclusive texel bounds of what one pass wrote. It lives in the engine because erosion works on
    // a region and erosion is what a procedural generator calls; the editor's terrain strokes and
    // their undo patches use the same rect.
    struct TerrainMapRegion {
        int minX = 0;
        int minY = 0;
        int maxX = -1;
        int maxY = -1;

        bool empty() const {
            return maxX < minX || maxY < minY;
        }

        int width() const {
            return empty() ? 0 : maxX - minX + 1;
        }

        int height() const {
            return empty() ? 0 : maxY - minY + 1;
        }

        bool fitsIn(int mapWidth, int mapHeight) const {
            return !empty() && minX >= 0 && minY >= 0 && maxX < mapWidth && maxY < mapHeight;
        }

        void merge(int x0, int y0, int x1, int y1) {
            if (empty()) {
                minX = x0;
                minY = y0;
                maxX = x1;
                maxY = y1;
                return;
            }
            minX = std::min(minX, x0);
            minY = std::min(minY, y0);
            maxX = std::max(maxX, x1);
            maxY = std::max(maxY, y1);
        }
    };
}

#endif /* TerrainMapRegion_h */
