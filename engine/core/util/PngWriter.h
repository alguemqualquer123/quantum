// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#ifndef PngWriter_h
#define PngWriter_h

#include "Export.h"

#include <cstddef>
#include <filesystem>

namespace quantum {

    // Encodes and writes a 16-bit single-channel (grayscale) PNG. `pixels` holds
    // width*height 16-bit samples in native little-endian order (low byte first);
    // the encoder swaps to PNG's big-endian, applies adaptive per-row filtering and
    // DEFLATEs via stb_image_write's zlib compressor (stb cannot emit 16-bit PNGs).
    // Returns false on invalid arguments or a write/compress failure.
    //
    // This is the only writer for the format a terrain heightmap is stored in, so the terrain
    // editor, the AI generators and the procedural generator all produce maps the engine can read
    // back as RED16 (see TextureData::loadTexture).
    QUANTUM_API bool writeGray16Png(const std::filesystem::path& path, int width, int height,
                                   const unsigned char* pixels, std::size_t pixelByteCount);

    // The 8-bit colour paths of the same encoder, for the maps a PBR material is made of. `pixels`
    // holds width*height*channels bytes, row-major, in the order the function name spells. These are
    // plain 8-bit samples, so no byte swap happens; stb_image_write could do this too, but going
    // through one encoder keeps every map this repo writes filtered and compressed alike.
    QUANTUM_API bool writeRgb8Png(const std::filesystem::path& path, int width, int height,
                                 const unsigned char* pixels, std::size_t pixelByteCount);
    QUANTUM_API bool writeRgba8Png(const std::filesystem::path& path, int width, int height,
                                  const unsigned char* pixels, std::size_t pixelByteCount);
    // A single 8-bit channel: what an occlusion or a blending-height map is stored as, so a loader
    // reading one channel gets it without repacking.
    QUANTUM_API bool writeGray8Png(const std::filesystem::path& path, int width, int height,
                                  const unsigned char* pixels, std::size_t pixelByteCount);
}

#endif /* PngWriter_h */
