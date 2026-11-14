// (c) SR VINIX
// SPDX-License-Identifier: MIT

#include "PluginLoader.h"

#include <filesystem>
#include <sstream>

#ifdef _WIN32
    #include <windows.h>
#else
    #include <dlfcn.h>
#endif

using namespace quantum;
using namespace quantum::plugin;

PluginLoader::Loaded PluginLoader::open(const std::string& path) {
    Loaded out;
    out.resolvedPath = path;

    std::error_code ec;
    if (!std::filesystem::exists(std::filesystem::path(path), ec)) {
        out.error = "library file not found: " + path;
        return out;
    }

    #ifdef _WIN32
        // LoadLibraryW rather than LoadLibraryA: plugin folders live under user profiles, which
        // routinely contain non-ASCII names that the ANSI entry point mangles. The path arrives as
        // UTF-8, so it has to be converted rather than widened byte by byte.
        const int wideLength = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
        if (wideLength <= 0) {
            out.error = "cannot convert the plugin path to UTF-16 (" + systemErrorMessage() + ")";
            return out;
        }
        std::wstring wide(static_cast<size_t>(wideLength), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &wide[0], wideLength);

        HMODULE module = LoadLibraryW(wide.c_str());
        if (!module) {
            out.error = "LoadLibrary failed for " + path + " (" + systemErrorMessage() + ")";
            return out;
        }
        out.handle = static_cast<void*>(module);
    #else
        void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!handle) {
            const char* message = dlerror();
            out.error = "dlopen failed for " + path + " (" + (message ? message : "no detail") + ")";
            return out;
        }
        out.handle = handle;
    #endif

    return out;
}

void PluginLoader::close(void* handle) {
    if (!handle) return;
    #ifdef _WIN32
        FreeLibrary(static_cast<HMODULE>(handle));
    #else
        // dlclose does not reliably unmap the image (the same caveat editor/Conector.cpp:121-128
        // documents), so a plugin that is reloaded gets a fresh instance rather than the old one.
        dlclose(handle);
    #endif
}

void* PluginLoader::symbol(void* handle, const char* name) {
    if (!handle) return nullptr;
    #ifdef _WIN32
        return reinterpret_cast<void*>(
            GetProcAddress(static_cast<HMODULE>(handle), name));
    #else
        dlerror();
        return dlsym(handle, name);
    #endif
}

std::string PluginLoader::systemErrorMessage() {
    #ifdef _WIN32
        const DWORD code = GetLastError();
        char buffer[512];
        const DWORD length = FormatMessageA(
            FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr, code, 0, buffer, static_cast<DWORD>(sizeof(buffer)), nullptr);
        std::string text = "error " + std::to_string(code);
        if (length > 0) {
            std::string message(buffer, length);
            while (!message.empty() &&
                   (message.back() == '\n' || message.back() == '\r' || message.back() == ' ')) {
                message.pop_back();
            }
            text += ": " + message;
        }
        return text;
    #else
        const char* message = dlerror();
        return message ? std::string(message) : std::string("no detail");
    #endif
}
