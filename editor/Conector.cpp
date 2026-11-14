// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "Conector.h"

#include "editor/Out.h"
#include "Project.h"
#include "engine/core/Engine.h"
#include "engine/core/script/ProjectABI.h"

#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

using namespace quantum;

namespace {

bool validateProjectAbi(void* libHandle) {
    using AbiFunction = std::uint64_t (*)();
    AbiFunction function = nullptr;
#ifdef _WIN32
    function = reinterpret_cast<AbiFunction>(GetProcAddress(static_cast<HMODULE>(libHandle),
        "quantumProjectAbi"));
#else
    dlerror();
    function = reinterpret_cast<AbiFunction>(dlsym(libHandle, "quantumProjectAbi"));
    if (dlerror() != nullptr) function = nullptr;
#endif
    if (!function) {
        editor::Out::error("C++ script library does not export quantumProjectAbi; rebuild it with this Quantum editor");
        return false;
    }

    const std::uint64_t actual = function();
    const std::uint64_t expected = quantum::projectAbiFingerprint();
    if (actual != expected) {
        editor::Out::error("C++ script library ABI mismatch (expected 0x%llx, got 0x%llx); refusing to load",
            static_cast<unsigned long long>(expected), static_cast<unsigned long long>(actual));
        return false;
    }
    return true;
}

// false when the library does not export it
template<typename... Args>
bool callLibraryFunction(void* libHandle, const char* name, Args... args) {
    using LibraryFunc = void (*)(Args...);
    #ifdef _WIN32
        LibraryFunc fn = reinterpret_cast<LibraryFunc>(GetProcAddress(static_cast<HMODULE>(libHandle), name));
        if (!fn) {
            editor::Out::error("Failed to find function '%s' in the library (Error code: %lu)", name, GetLastError());
            return false;
        }
    #else
        dlerror(); // clear any existing error
        LibraryFunc fn = reinterpret_cast<LibraryFunc>(dlsym(libHandle, name));
        const char* err = dlerror();
        if (err) {
            editor::Out::error("Failed to find function '%s' in the library (Error: %s)", name, err);
            return false;
        }
    #endif

    try {
        fn(args...);
    } catch (const std::exception& e) {
        editor::Out::error("Exception in %s(): %s", name, e.what());
    } catch (...) {
        editor::Out::error("Unknown exception in %s()", name);
    }
    return true;
}

}

editor::Conector::Conector(){
    libHandle = nullptr;
}

editor::Conector::~Conector(){
    disconnect();
}

bool editor::Conector::fileExists(const fs::path& path) {
    return std::filesystem::exists(path);
}

void* editor::Conector::loadSharedLibrary(const std::string& libPath) {
    #ifdef _WIN32
        HMODULE libHandle = LoadLibrary(libPath.c_str());
        if (!libHandle) {
            Out::error("Failed to load library: %s (Error code: %lu)", libPath.c_str(), GetLastError());
            return nullptr;
        }
        return libHandle;
    #else
        void* libHandle = dlopen(libPath.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!libHandle) {
            Out::error("Failed to load library: %s (Error: %s)", libPath.c_str(), dlerror());
            return nullptr;
        }
        return libHandle;
    #endif
}

void editor::Conector::unloadSharedLibrary(void* libHandle) {
    #ifdef _WIN32
        if (libHandle) {
            FreeLibrary(static_cast<HMODULE>(libHandle));
            std::cout << "Library unloaded successfully.\n";
        }
    #else
        if (libHandle) {
            dlclose(libHandle);
            std::cout << "Library unloaded successfully.\n";
        }
    #endif
}

bool editor::Conector::stageLibrary(const fs::path& buildPath, const std::string& libName,
                                    void*& outHandle, fs::path& outCopy) {
    outHandle = nullptr;
    outCopy.clear();
    std::string libPath = libName;
    #ifdef _WIN32
        libPath = libPath + ".dll";
    #elif defined(__APPLE__)
        libPath = libPath + ".dylib";
    #else
        libPath = libPath + ".so";
    #endif

    fs::path fullLibPath = buildPath / fs::path(libPath);
    if (!fileExists(fullLibPath)) {
        Out::error("Library file not found: %s", libPath.c_str());
        return false;
    }

    // Load a freshly-named COPY of the built library rather than the build
    // output directly. The dynamic loader identifies a library by its file
    // identity and dlclose()/FreeLibrary() are not guaranteed to unmap it, so
    // re-loading the same path after a rebuild can hand back the OLD code that
    // is still resident in the process (the reason a fresh rebuild only takes
    // effect after restarting the editor). On Windows LoadLibrary also keeps the
    // .dll locked, blocking the next build from overwriting it. A unique filename
    // per load forces the loader to map the new file every time.
    static std::atomic<unsigned long long> loadCounter{0};
    #ifdef _WIN32
        const std::string ext = ".dll";
    #elif defined(__APPLE__)
        const std::string ext = ".dylib";
    #else
        const std::string ext = ".so";
    #endif
    const std::string uniqueName = libName + "_" + std::to_string(loadCounter++) + "_" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ext;

    // Stage the copy in the build dir, falling back to the system temp dir if
    // that is not writable (disk full, permissions, read-only mount, ...). We
    // deliberately never fall back to loading fullLibPath directly: that path is
    // identity-cached by the loader and would silently re-run stale code, the
    // exact "build succeeded, Play runs old code" bug this staging prevents.
    std::error_code ec;
    std::vector<fs::path> stagingDirs;
    stagingDirs.push_back(buildPath / "hotreload");
    fs::path tempBase = fs::temp_directory_path(ec);
    if (!ec && !tempBase.empty()) {
        stagingDirs.push_back(tempBase / "quantum_hotreload");
    }

    fs::path copyPath;
    for (const fs::path& dir : stagingDirs) {
        std::error_code dirEc;
        fs::create_directories(dir, dirEc);

        // Best-effort sweep of copies left behind by previous sessions/crashes.
        // Still-mapped files fail to remove and are simply skipped.
        for (const auto& entry : fs::directory_iterator(dir, dirEc)) {
            std::error_code rmEc;
            fs::remove(entry.path(), rmEc);
        }

        fs::path candidate = dir / uniqueName;
        std::error_code cpEc;
        fs::copy_file(fullLibPath, candidate, fs::copy_options::overwrite_existing, cpEc);
        if (!cpEc) {
            copyPath = candidate;
            break;
        }
        Out::warning("Could not stage hot-reload copy in %s (%s)", dir.string().c_str(), cpEc.message().c_str());
    }

    if (copyPath.empty()) {
        Out::error("Failed to stage a loadable copy of the script library; aborting to avoid running stale code");
        return false;
    }

    outHandle = loadSharedLibrary(copyPath.string());
    if (!outHandle) {
        std::error_code rmEc;
        fs::remove(copyPath, rmEc);
        return false;
    }

    outCopy = copyPath;
    return true;
}

bool editor::Conector::connect(const fs::path& buildPath, std::string libName){
    void* candidate = nullptr;
    fs::path candidateCopy;
    if (!stageLibrary(buildPath, libName, candidate, candidateCopy)) return false;

    if (!validateProjectAbi(candidate)) {
        unloadSharedLibrary(candidate);
        std::error_code ec;
        fs::remove(candidateCopy, ec);
        return false;
    }

    // Disconnect only after the replacement is mapped and ABI-checked. This keeps a working
    // library alive if the new build is missing, malformed or compiled against another editor.
    disconnect();
    libHandle = candidate;
    loadedLibCopy = candidateCopy;
    Out::info("Successfully connected to library");
    return true;
}

bool editor::Conector::reload(const fs::path& buildPath, const std::string& libName,
                              const std::vector<Scene*>& scenes) {
    void* candidate = nullptr;
    fs::path candidateCopy;
    if (!stageLibrary(buildPath, libName, candidate, candidateCopy)) return false;

    if (!validateProjectAbi(candidate)) {
        unloadSharedLibrary(candidate);
        std::error_code ec;
        fs::remove(candidateCopy, ec);
        return false;
    }

    // The old ABI is still mapped while destructors run. No callback can point at the old
    // code after cleanup returns, so the actual unload/swap is a short owner-thread operation.
    for (Scene* scene : scenes) {
        if (!scene) continue;
        cleanup(scene);
        Engine::clearComponentSubscriptions(scene);
    }

    disconnect();
    libHandle = candidate;
    loadedLibCopy = candidateCopy;
    for (Scene* scene : scenes) {
        if (scene) init(scene);
    }
    Out::info("Safely reloaded C++ script library");
    return true;
}

void editor::Conector::disconnect(){
    if (libHandle) {
        unloadSharedLibrary(libHandle);
        libHandle = nullptr;

        Out::info("Disconnected from library successfully");
    }

    if (!loadedLibCopy.empty()) {
        // Safe to unlink even if the OS kept the library mapped: on POSIX the
        // inode outlives the directory entry, and on Windows this is best-effort.
        std::error_code ec;
        fs::remove(loadedLibCopy, ec);
        loadedLibCopy.clear();
    }
}

void editor::Conector::init(Scene* scene){
    if (!libHandle) {
        Out::error("Cannot execute: Not connected to library");
        return;
    }

    callLibraryFunction(libHandle, "initScripts", scene);
}

void editor::Conector::cleanup(Scene* scene){
    if (!libHandle) {
        Out::error("Cannot cleanup: Not connected to library");
        return;
    }

    if (!callLibraryFunction(libHandle, "cleanupScripts", scene)) {
        Out::warning("Cleanup function not found in library");
    }
}

void editor::Conector::cleanupEntity(Scene* scene, Entity entity){
    if (libHandle) {
        callLibraryFunction(libHandle, "cleanupEntityScripts", scene, entity);
    }
}

bool editor::Conector::isLibraryConnected() const {
    return libHandle != nullptr;
}
