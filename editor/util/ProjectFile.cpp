// (c) SR VINIX
// SPDX-License-Identifier: MIT

#include "util/ProjectFile.h"

#include <cctype>
#include <cstdint>
#include <fstream>
#include <system_error>

namespace fs = std::filesystem;

namespace quantum::editor::ProjectFile {

namespace {

    // Windows refuses the same handful of characters that POSIX merely discourages,
    // and a trailing dot or space is silently dropped there, so the marker is named
    // with a subset both tolerate.
    std::string sanitizeFileName(const std::string& name) {
        std::string out;
        out.reserve(name.size());
        for (unsigned char raw : name) {
            const char c = static_cast<char>(raw);
            const bool safe = std::isalnum(raw) != 0 || c == '_' || c == '-' || c == '.' || c == ' ';
            out.push_back(safe ? c : '_');
        }
        while (!out.empty() && (out.back() == ' ' || out.back() == '.')) {
            out.pop_back();
        }
        return out;
    }

    std::string lower(std::string value) {
        for (char& c : value) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return value;
    }

    bool hasExtension(const fs::path& candidate, const std::string& extension) {
        return lower(candidate.extension().string()) == lower(extension);
    }

    // Only small text files can be one of our markers, so the scan never reads a
    // binary asset that happens to share the extension.
    std::string markerExtension(const std::string& content) {
        static const std::string key = "extension = ";
        const size_t at = content.find(key);
        if (at == std::string::npos) {
            return {};
        }
        const size_t value = at + key.size();
        const size_t lineEnd = content.find_first_of("\r\n", value);
        std::string out = content.substr(value, lineEnd == std::string::npos ? std::string::npos
                                                                           : lineEnd - value);
        const size_t last = out.find_last_not_of(" \t");
        if (last != std::string::npos) {
            out.erase(last + 1);
        }
        return out;
    }

    // A marker records the extension it was written for. The extension is a user
    // setting, so a file that names another one does not belong to this configuration
    // and must not be adopted, however similar its name looks.
    bool isOurMarker(const fs::path& candidate, const std::string& wanted) {
        std::error_code sizeEc;
        const std::uintmax_t size = fs::file_size(candidate, sizeEc);
        if (sizeEc || size == 0 || size > 4096) {
            return false;
        }
        std::ifstream file(candidate, std::ios::in | std::ios::binary);
        if (!file) {
            return false;
        }
        std::string buffer(static_cast<size_t>(size), '\0');
        file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        if (buffer.find("project = project.yaml") == std::string::npos) {
            return false;
        }
        return markerExtension(buffer) == wanted;
    }

    // The folder's marker, if it has one under the wanted extension. The scan matches
    // the extension case-insensitively because that is how Windows compares them, and
    // leaves the exact decision to the recorded value.
    fs::path findExistingMarker(const fs::path& projectDir, const std::string& wanted) {
        std::error_code ec;
        const fs::directory_iterator end;
        for (fs::directory_iterator it(projectDir, ec); !ec && it != end; it.increment(ec)) {
            if (hasExtension(it->path(), wanted) && fs::is_regular_file(it->path(), ec)
                && isOurMarker(it->path(), wanted)) {
                return it->path();
            }
        }
        return {};
    }
}

std::string normalizeExtension(const std::string& extension) {
    // Surrounding blanks are a typing accident; a blank inside the name would make
    // the extension unusable both as a file name and as a registry key.
    const std::string::size_type first = extension.find_first_not_of(" \t");
    const std::string::size_type last = extension.find_last_not_of(" \t");
    std::string out = first == std::string::npos ? std::string()
                                                 : extension.substr(first, last - first + 1);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    while (!out.empty() && out.front() == '.') {
        out.erase(out.begin());
    }
    if (out.empty()) {
        out = DEFAULT_EXTENSION + 1;
    }
    // A single dot plus a plain ASCII name: anything else would corrupt the registry
    // key, the Explorer badge and the file name alike, so it falls back to default.
    for (char c : out) {
        const bool allowed = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (!allowed) {
            return DEFAULT_EXTENSION;
        }
    }
    if (out.size() > 15) {
        return DEFAULT_EXTENSION;
    }
    return "." + out;
}

bool isProjectDir(const fs::path& dir) {
    std::error_code ec;
    return !dir.empty() && fs::is_directory(dir, ec) && fs::exists(dir / "project.yaml", ec);
}

fs::path markerPath(const fs::path& projectDir,
                    const std::string& projectName,
                    const std::string& extension) {
    if (!isProjectDir(projectDir)) {
        return {};
    }

    const std::string wanted = normalizeExtension(extension);

    // Keep one marker per project: an existing file under the wanted extension is
    // reused even if its name no longer matches the project's, so renaming a project
    // never leaves a stale second file behind.
    const fs::path existing = findExistingMarker(projectDir, wanted);
    if (!existing.empty()) {
        return existing;
    }

    std::string stem = sanitizeFileName(projectName);
    if (stem.empty()) {
        stem = sanitizeFileName(projectDir.filename().string());
    }
    if (stem.empty()) {
        return {};
    }
    return projectDir / (stem + wanted);
}

fs::path resolve(const fs::path& path) {
    if (path.empty()) {
        return {};
    }
    std::error_code ec;
    const fs::path dir = fs::is_directory(path, ec) ? path : path.parent_path();
    return isProjectDir(dir) ? dir : fs::path();
}

bool ensureMarker(const fs::path& projectDir,
                  const std::string& projectName,
                  const std::string& extension,
                  std::string& error) {
    error.clear();
    if (!isProjectDir(projectDir)) {
        error = "no project.yaml in " + projectDir.string();
        return false;
    }

    const std::string wanted = normalizeExtension(extension);

    const fs::path existing = findExistingMarker(projectDir, wanted);
    if (!existing.empty()) {
        return true;
    }

    const fs::path marker = markerPath(projectDir, projectName, wanted);
    if (marker.empty()) {
        error = "the project name cannot be used as a file name";
        return false;
    }

    // markerPath only returns an existing path when it is one of our markers, so a
    // file already sitting here belongs to somebody else. Writing over it would
    // destroy their project, and adopting it would open this one as theirs.
    std::error_code existsEc;
    if (fs::exists(marker, existsEc)) {
        error = marker.filename().string() + " already exists and is not a Quantum marker. "
                                             "Set another project file extension in Editor "
                                             "Settings, or remove that file";
        return false;
    }

    std::ofstream file(marker, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!file) {
        error = "cannot write " + marker.string();
        return false;
    }
    file << "# Quantum project\n"
            "# Double-click to open this project in the Quantum editor.\n"
            "# The settings live in project.yaml, next to this file. This marker only gives\n"
            "# the operating system a file to bind the icon and the \"open with\" entry to.\n"
         << "extension = " << wanted << "\n"
         << "project = project.yaml\n"
         << "name = " << projectName << "\n";
    file.close();
    if (file.fail()) {
        error = "cannot write " + marker.string();
        return false;
    }
    return true;
}

}
