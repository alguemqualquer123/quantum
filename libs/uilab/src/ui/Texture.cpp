#include "ui/Texture.h"

#include <cmath>
#include <cstdio>

#if defined(_WIN32)
#include <windows.h>
#endif
#include <GL/gl.h>

namespace uilab {

namespace {
constexpr int kGLClampToEdge = 0x812F;
constexpr int kGLLinear = 0x2601;

unsigned char ToByte(float v) {
    if (v <= 0.0f)
        return 0;
    if (v >= 1.0f)
        return 255;
    return static_cast<unsigned char>(v * 255.0f + 0.5f);
}

// Very small deterministic hash based noise.
float HashNoise(int x, int y, unsigned seed) {
    unsigned h = static_cast<unsigned>(x) * 374761393u + static_cast<unsigned>(y) * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h = h ^ (h >> 16);
    return static_cast<float>(h & 0xFFFFu) / 65535.0f;
}
} // namespace

Texture2D::~Texture2D() { Destroy(); }

Texture2D::Texture2D(Texture2D&& other) noexcept
    : glId_(other.glId_), width_(other.width_), height_(other.height_), name_(std::move(other.name_)) {
    other.glId_ = 0;
    other.width_ = 0;
    other.height_ = 0;
}

Texture2D& Texture2D::operator=(Texture2D&& other) noexcept {
    if (this != &other) {
        Destroy();
        glId_ = other.glId_;
        width_ = other.width_;
        height_ = other.height_;
        name_ = std::move(other.name_);
        other.glId_ = 0;
        other.width_ = 0;
        other.height_ = 0;
    }
    return *this;
}

void Texture2D::Destroy() {
    if (glId_ != 0) {
        const GLuint id = glId_;
        glDeleteTextures(1, &id);
        glId_ = 0;
    }
    width_ = 0;
    height_ = 0;
}

bool Texture2D::CreateFromPixels(int width, int height, const std::vector<unsigned char>& rgba) {
    if (width <= 0 || height <= 0)
        return false;
    const size_t expected = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
    if (rgba.size() < expected)
        return false;

    Destroy();
    GLuint id = 0;
    glGenTextures(1, &id);
    if (id == 0)
        return false;
    glBindTexture(GL_TEXTURE_2D, id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, kGLLinear);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, kGLLinear);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, kGLClampToEdge);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, kGLClampToEdge);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    glBindTexture(GL_TEXTURE_2D, 0);

    glId_ = id;
    width_ = width;
    height_ = height;
    return true;
}

bool Texture2D::CreateChecker(int width, int height, int cells, ImVec4 colorA, ImVec4 colorB) {
    if (cells <= 0)
        cells = 8;
    std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * 4);
    const float cw = static_cast<float>(width) / static_cast<float>(cells);
    const float ch = static_cast<float>(height) / static_cast<float>(cells);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int cx = static_cast<int>(x / cw);
            const int cy = static_cast<int>(y / ch);
            const bool even = ((cx + cy) % 2) == 0;
            const ImVec4 c = even ? colorA : colorB;
            const size_t i = (static_cast<size_t>(y) * width + x) * 4;
            pixels[i + 0] = ToByte(c.x);
            pixels[i + 1] = ToByte(c.y);
            pixels[i + 2] = ToByte(c.z);
            pixels[i + 3] = ToByte(c.w);
        }
    }
    return CreateFromPixels(width, height, pixels);
}

bool Texture2D::CreateGradient(int width, int height, ImVec4 top, ImVec4 bottom, bool vertical) {
    std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * 4);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const float t = vertical ? static_cast<float>(y) / static_cast<float>(height - 1 > 0 ? height - 1 : 1)
                                     : static_cast<float>(x) / static_cast<float>(width - 1 > 0 ? width - 1 : 1);
            const ImVec4 c = ImVec4(top.x + (bottom.x - top.x) * t, top.y + (bottom.y - top.y) * t,
                                    top.z + (bottom.z - top.z) * t, top.w + (bottom.w - top.w) * t);
            const size_t i = (static_cast<size_t>(y) * width + x) * 4;
            pixels[i + 0] = ToByte(c.x);
            pixels[i + 1] = ToByte(c.y);
            pixels[i + 2] = ToByte(c.z);
            pixels[i + 3] = ToByte(c.w);
        }
    }
    return CreateFromPixels(width, height, pixels);
}

bool Texture2D::CreateUvGrid(int width, int height, int cells) {
    if (cells <= 0)
        cells = 8;
    std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * 4);
    const float cellW = static_cast<float>(width) / static_cast<float>(cells);
    const float cellH = static_cast<float>(height) / static_cast<float>(cells);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const float fx = static_cast<float>(x) / static_cast<float>(width - 1 ? width - 1 : 1);
            const float fy = static_cast<float>(y) / static_cast<float>(height - 1 ? height - 1 : 1);
            ImVec4 c = ImVec4(0.08f + fx * 0.35f, 0.09f + fy * 0.35f, 0.16f + (fx * fy) * 0.4f, 1.0f);
            const float inCellX = std::fmod(static_cast<float>(x), cellW);
            const float inCellY = std::fmod(static_cast<float>(y), cellH);
            const bool lineX = inCellX < 1.5f;
            const bool lineY = inCellY < 1.5f;
            const int cx = static_cast<int>(x / cellW);
            const int cy = static_cast<int>(y / cellH);
            if (lineX || lineY) {
                c = ImVec4(0.55f, 0.62f, 0.78f, 1.0f);
                if ((cx % 4 == 0 && lineX) || (cy % 4 == 0 && lineY))
                    c = ImVec4(0.85f, 0.72f, 0.30f, 1.0f);
            } else if ((cx + cy) % 2 == 0) {
                c.x *= 1.15f;
                c.y *= 1.15f;
                c.z *= 1.15f;
            }
            const size_t i = (static_cast<size_t>(y) * width + x) * 4;
            pixels[i + 0] = ToByte(c.x);
            pixels[i + 1] = ToByte(c.y);
            pixels[i + 2] = ToByte(c.z);
            pixels[i + 3] = ToByte(c.w);
        }
    }
    return CreateFromPixels(width, height, pixels);
}

bool Texture2D::CreateRadialNoise(int width, int height, unsigned seed) {
    std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * 4);
    const float cx = static_cast<float>(width) * 0.5f;
    const float cy = static_cast<float>(height) * 0.5f;
    const float maxR = std::sqrt(cx * cx + cy * cy);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const float dx = static_cast<float>(x) - cx;
            const float dy = static_cast<float>(y) - cy;
            const float r = std::sqrt(dx * dx + dy * dy) / maxR;
            const float n = HashNoise(x, y, seed);
            const float v = std::fmax(0.0f, 1.0f - r) * (0.55f + n * 0.45f);
            const size_t i = (static_cast<size_t>(y) * width + x) * 4;
            pixels[i + 0] = ToByte(v * 1.05f);
            pixels[i + 1] = ToByte(v * 0.85f);
            pixels[i + 2] = ToByte(v * 0.55f);
            pixels[i + 3] = 255;
        }
    }
    return CreateFromPixels(width, height, pixels);
}

std::string Texture2D::DebugLine() const {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%s: %dx%d (GL id %u)", name_.empty() ? "texture" : name_.c_str(), width_,
                  height_, glId_);
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// TextureLibrary
// ---------------------------------------------------------------------------
bool TextureLibrary::CreateAll(std::string& errorOut) {
    bool ok = true;
    ok &= checker_.CreateChecker(256, 256, 8, ImVec4(0.22f, 0.24f, 0.30f, 1.0f), ImVec4(0.16f, 0.18f, 0.22f, 1.0f));
    checker_.SetName("T_Checker_256");
    ok &= gradient_.CreateGradient(256, 256, ImVec4(0.26f, 0.59f, 0.98f, 1.0f), ImVec4(0.62f, 0.47f, 0.96f, 1.0f),
                                   true);
    gradient_.SetName("T_Gradient_256");
    ok &= uvGrid_.CreateUvGrid(256, 256, 8);
    uvGrid_.SetName("T_UVGrid_256");
    ok &= noise_.CreateRadialNoise(192, 192, 20260215u);
    noise_.SetName("T_Noise_Radial");
    ok &= preview_.CreateGradient(512, 288, ImVec4(0.10f, 0.13f, 0.20f, 1.0f), ImVec4(0.24f, 0.32f, 0.48f, 1.0f),
                                  true);
    preview_.SetName("T_Preview_512x288");

    if (!ok) {
        errorOut = "One or more procedural textures could not be created (OpenGL texture upload failed).";
        return false;
    }
    errorOut.clear();
    return true;
}

void TextureLibrary::DestroyAll() {
    checker_.Destroy();
    gradient_.Destroy();
    uvGrid_.Destroy();
    noise_.Destroy();
    preview_.Destroy();
}

int TextureLibrary::Count() const {
    int n = 0;
    n += checker_.Valid() ? 1 : 0;
    n += gradient_.Valid() ? 1 : 0;
    n += uvGrid_.Valid() ? 1 : 0;
    n += noise_.Valid() ? 1 : 0;
    n += preview_.Valid() ? 1 : 0;
    return n;
}

} // namespace uilab
