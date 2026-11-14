// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Project.h"

#include "shader/ShaderBuildTypes.h"
#include "ShaderData.h"
#include <vector>
#include <set>
#include <unordered_map>
#include <mutex>
#include <future>
#include <filesystem>

#include "shadercompiler.h"
#include "shaders.h"

namespace shadercompiler {
    struct spirvcross_t;
    struct input_t;
    struct args_t;
}

namespace quantum::editor {

    class ShaderBuilder {
    private:
        static std::unordered_map<ShaderKey, ShaderData> shaderDataCache;
        static std::unordered_map<ShaderKey, std::future<ShaderData>> pendingBuilds;
        // A failed async build cannot travel through its future: every target is
        // compiled with _HAS_EXCEPTIONS=0, so std::packaged_task has no handler to
        // store the throw and an escaping exception reaches std::terminate (the
        // editor aborted on any shader that failed to compile). The worker records
        // the key here and returns an empty ShaderData instead. Its own mutex
        // because requestShutdown() waits on futures while holding cacheMutex.
        static std::set<ShaderKey> failedBuilds;
        static std::mutex& failedBuildsMutex();
        static std::mutex& cacheMutex();

        static std::atomic<bool> shutdownRequested;

        // Mapping functions declarations with camelCase
        ShaderVertexType mapVertexType(shadercompiler::attribute_type_t type);
        ShaderUniformType mapUniformType(shadercompiler::uniform_type_t type);
        TextureType mapTextureType(shadercompiler::texture_type_t type);
        TextureSamplerType mapSamplerType(shadercompiler::texture_samplertype_t type);
        SamplerType mapSamplerFilterType(shadercompiler::sampler_type_t type);
        ShaderStorageBufferType mapStorageType(shadercompiler::storage_buffer_type_t type);
        ShaderStageType mapStageType(shadercompiler::stage_type_t type);
        ShaderLang mapLang(shadercompiler::lang_type_t lang);
        static void applyShaderBackend(shadercompiler::args_t& args, ShaderBackend backend);
        static void setBackendLang(shadercompiler::args_t& args);

        ShaderData convertToShaderData(
            const std::vector<shadercompiler::spirvcross_t>& spirvcrossvec,
            const std::vector<shadercompiler::input_t>& inputs,
            const shadercompiler::args_t& args);

        void addMeshPropertyDefinitions(std::vector<shadercompiler::define_t>& defs, const uint32_t prop);
        void addDepthMeshPropertyDefinitions(std::vector<shadercompiler::define_t>& defs, const uint32_t prop);
        void addGBufferMeshPropertyDefinitions(std::vector<shadercompiler::define_t>& defs, const uint32_t prop);
        void addUIPropertyDefinitions(std::vector<shadercompiler::define_t>& defs, const uint32_t prop);
        void addPointsPropertyDefinitions(std::vector<shadercompiler::define_t>& defs, const uint32_t prop);
        void addLinesPropertyDefinitions(std::vector<shadercompiler::define_t>& defs, const uint32_t prop);
        void addWaterPropertyDefinitions(std::vector<shadercompiler::define_t>& defs, const uint32_t prop);

        bool setupShaderArgs(shadercompiler::args_t& args, ShaderType shaderType, uint32_t properties);
        std::string getLangSuffix(shadercompiler::lang_type_t lang, int version, bool es, shadercompiler::platform_t platform);

        // Sets entry-point files + defines for the key. For custom shaders, resolves only
        // the exact transitive dependency graph from the fork's .vert/.frag entrypoints.
        // Throws on failure.
        void setupBuildArgs(shadercompiler::args_t& args, ShaderKey shaderKey, Project* project);

        ShaderData buildShaderInternal(ShaderKey shaderKey, Project* project, bool trackProgress);
        std::string getShaderDisplayName(ShaderKey key);

        static std::filesystem::path getShaderCachePath(ShaderKey shaderKey, Project* project);

        // Compares the cached source signature with the entry points and the exact
        // transitive include graph used by this fork. No directory walk is required.
        static bool isCustomCacheStale(ShaderKey shaderKey, Project* project, const std::filesystem::path& cachePath);

        // The engine-owned text a built-in variant is compiled from: the two entry files of its
        // type plus every include they reach inside the embedded shaderMap. Properties only add
        // #defines and are already part of the cache file name, so one signature covers every
        // variant of a type. Computed once per type and process.
        static const std::string& builtinSourceSignature(ShaderType shaderType);

        // Both disk-cache freshness rules behind one call: a forked variant signs its own resolved
        // graph, an engine variant signs the shaderMap graph of its type. A cache entry with no
        // signature next to it is stale, so the first read after this landed recompiles once.
        static bool isCacheStale(ShaderKey shaderKey, Project* project, const std::filesystem::path& cachePath);

        // The built-in half of that rule, kept separate so the sweep and the build path agree.
        static bool isBuiltinCacheStale(ShaderType shaderType, const std::filesystem::path& cachePath);

    public:
        ShaderBuilder();
        virtual ~ShaderBuilder();

        ShaderBuildResult buildShader(ShaderKey shaderKey, Project* project);
        ShaderData buildShaderForExport(ShaderKey shaderKey, Project* project, ShaderBackend backend);

        // Compiles the keys absent from the disk cache, synchronously (buildShader hands
        // async builds to the pool and returns before they land).
        // Compiles every key that is not on disk yet. Returns how many of them ended up missing
        // from the cache, so a caller can tell a completed sweep from one to retry.
        int buildMissingShaders(const std::set<ShaderKey>& shaderKeys, Project* project);

        // Drops cached builds and dependency snapshots for all custom variants so the
        // next get() recompiles. Called after source mutations and project switches.
        static void invalidateCustomShaders();

        static void requestShutdown();

        // Serialize ShaderData cache on demand (no disk I/O inside buildShaderInternal).
        static bool saveShaderDataCache(ShaderKey shaderKey, Project* project, const ShaderData& shaderData, std::string* err = nullptr);

        ShaderData& getShaderData(ShaderKey shaderKey);
    };

}
