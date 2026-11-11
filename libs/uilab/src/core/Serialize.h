// Serialize.h - tiny key=value document used for themes, property copy/paste
// and persisted settings. No external dependency on purpose.
#pragma once

#include <string>
#include <utility>
#include <vector>

#include <imgui.h>

namespace uilab {

class KeyValueDoc {
public:
    void Clear() { items_.clear(); }
    bool Empty() const { return items_.empty(); }
    size_t Size() const { return items_.size(); }

    void Set(const std::string& key, const std::string& value);
    void SetFloat(const std::string& key, float value);
    void SetInt(const std::string& key, int value);
    void SetBool(const std::string& key, bool value);
    void SetVec2(const std::string& key, const ImVec2& v);
    void SetVec4(const std::string& key, const ImVec4& v);

    bool Has(const std::string& key) const;
    std::string Get(const std::string& key, const std::string& def = std::string()) const;
    float GetFloat(const std::string& key, float def = 0.0f) const;
    int GetInt(const std::string& key, int def = 0) const;
    bool GetBool(const std::string& key, bool def = false) const;
    ImVec2 GetVec2(const std::string& key, const ImVec2& def = ImVec2(0, 0)) const;
    ImVec4 GetVec4(const std::string& key, const ImVec4& def = ImVec4(0, 0, 0, 1)) const;

    std::string ToText(const std::string& header) const;
    bool Parse(const std::string& text, std::string& errorOut);
    bool SaveToFile(const std::string& path, const std::string& header, std::string& errorOut) const;
    bool LoadFromFile(const std::string& path, std::string& errorOut);

    const std::vector<std::pair<std::string, std::string>>& Items() const { return items_; }

private:
    std::vector<std::pair<std::string, std::string>> items_;
};

// Reads a whole file into a string. Returns false when the file cannot be read.
bool ReadTextFile(const std::string& path, std::string& out, std::string& errorOut);
bool WriteTextFile(const std::string& path, const std::string& text, std::string& errorOut);
// Creates the parent directories of a file path when missing.
bool EnsureParentDirectory(const std::string& path, std::string& errorOut);

} // namespace uilab
