// Texture.h - small RAII OpenGL texture wrapper. All samples used by the lab are
// generated procedurally so the project has no binary asset dependencies.
#pragma once

#include <string>
#include <vector>

#include <imgui.h>

namespace uilab {

class Texture2D {
public:
    Texture2D() = default;
    ~Texture2D();
    Texture2D(const Texture2D&) = delete;
    Texture2D& operator=(const Texture2D&) = delete;
    Texture2D(Texture2D&& other) noexcept;
    Texture2D& operator=(Texture2D&& other) noexcept;

    bool CreateChecker(int width, int height, int cells, ImVec4 colorA, ImVec4 colorB);
    bool CreateGradient(int width, int height, ImVec4 top, ImVec4 bottom, bool vertical);
    bool CreateUvGrid(int width, int height, int cells);
    bool CreateRadialNoise(int width, int height, unsigned seed);
    bool CreateFromPixels(int width, int height, const std::vector<unsigned char>& rgba);

    void Destroy();
    bool Valid() const { return glId_ != 0; }
    ImTextureRef Ref() const { return ImTextureRef(static_cast<ImTextureID>(glId_)); }
    ImTextureID Id() const { return static_cast<ImTextureID>(glId_); }
    int Width() const { return width_; }
    int Height() const { return height_; }
    const std::string& Name() const { return name_; }
    void SetName(const std::string& n) { name_ = n; }
    std::string DebugLine() const;

private:
    unsigned glId_ = 0;
    int width_ = 0;
    int height_ = 0;
    std::string name_;
};

// Owns the sample textures shown in the Image View / content browser.
class TextureLibrary {
public:
    // Must be called with a current GL context (after the window was created).
    bool CreateAll(std::string& errorOut);
    void DestroyAll();

    Texture2D& Checker() { return checker_; }
    Texture2D& Gradient() { return gradient_; }
    Texture2D& UvGrid() { return uvGrid_; }
    Texture2D& Noise() { return noise_; }
    Texture2D& Preview() { return preview_; }

    int Count() const;

private:
    Texture2D checker_;
    Texture2D gradient_;
    Texture2D uvGrid_;
    Texture2D noise_;
    Texture2D preview_;
};

} // namespace uilab
