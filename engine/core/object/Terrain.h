// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#ifndef TERRAIN_H
#define TERRAIN_H

#include "Mesh.h"
#include "component/TerrainComponent.h"

namespace quantum{

    class QUANTUM_API Terrain: public Mesh{

    public:
        Terrain(Scene* scene);
        Terrain(Scene* scene, Entity entity);
        virtual ~Terrain();

        bool createTerrain();

        void setHeightMap(const std::string& path);
        void setHeightMap(Framebuffer* framebuffer);

        void setBlendMap(const std::string& path);
        void setBlendMap(Framebuffer* framebuffer);

        void setBlendMap(unsigned int index, const std::string& path);

        // Only the layer color: a PBR layer keeps the rest of its surface
        void setTextureLayer(unsigned int index, const std::string& path);

        void setTextureDetailRed(const std::string& path);
        void setTextureDetailGreen(const std::string& path);
        void setTextureDetailBlue(const std::string& path);

        void setSurfaceLayer(unsigned int index, const TerrainSurfaceLayer& layer);
        TerrainSurfaceLayer getSurfaceLayer(unsigned int index) const;
        // Copies what a terrain layer can render, and turns the layer into a PBR one
        void setLayerFromMaterial(unsigned int index, const Material& material);
        void removeSurfaceLayer(unsigned int index);
        unsigned int getNumLayers() const;

        void setSize(float size);
        float getSize() const;

        void setMaxHeight(float maxHeight);
        float getMaxHeight() const;

        void setResolution(int resolution);
        int getResolution() const;

        void setTextureBaseTiles(int textureBaseTiles);
        int getTextureBaseTiles() const;

        void setTextureDetailTiles(int textureDetailTiles);
        int getTextureDetailTiles() const;

        void setRootGridSize(int rootGridSize);
        int getRootGridSize() const;

        void setLevels(int levels);
        int getLevels() const;

        // Foliage instances are derived from the density map and never stored, so a script
        // that wants scatter has to name both the mesh it repeats and the map that paints it.
        unsigned int addFoliageLayer(const std::string& meshPath, const std::string& densityMapPath);
        void removeFoliageLayer(unsigned int index);
        unsigned int getNumFoliageLayers() const;

        void setFoliageDensity(unsigned int index, float density);
        void setFoliageDrawDistance(unsigned int index, float distance);
        void setFoliageCastsShadows(unsigned int index, bool castShadows);

        // Anything that rewrites part of an already-loaded map - a runtime level editor, a
        // terrain script carving a crater - says which texels it touched instead of invalidating
        // the whole terrain. An empty rect is the whole-map case and invalidates broadly.
        void markHeightmapDirty(int minX, int minY, int maxX, int maxY);
        void markFoliageDensityDirty(int layer, int minX, int minY, int maxX, int maxY);
    };

}

#endif //TERRAIN_H