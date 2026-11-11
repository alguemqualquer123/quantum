// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "LuaBinding.h"

#include "lua.hpp"

#include "LuaBridge.h"
#include "LuaBridgeAddon.h"

#include "Scene.h"

#include "util/Angle.h"
#include "util/Base64.h"
#include "util/Color.h"
#include "util/Noise.h"
#include "util/TerrainGenerator.h"
#include "util/TerrainMaterialLibrary.h"
#include "util/TerrainSplat.h"
#include "util/Wind.h"
#include "../../../voxel/World.h"

#include <string>
#include <vector>

using namespace quantum;

namespace luabridge
{
    // NoiseType travels as a plain Lua number, so a script can hold the base of a fractal sum in a field.
    template<> struct Stack<NoiseType> : EnumWrapper<NoiseType>{};
}

namespace {
    // Raw C functions, for the reason the Plugins namespace documents
    // (binding/PluginClassesLua.cpp): LuaBridge satisfies a declared `lua_State*` from stack slot 1,
    // so a function that wants the VM and its own arguments has to read the stack itself. Each of
    // these takes one options table.

    // One field of the options table at `tableIndex`. A missing or wrongly typed field leaves the
    // destination alone, so a script only names what it wants to override.
    bool getField(lua_State* L, int tableIndex, const char* key, std::string& value) {
        lua_getfield(L, tableIndex, key);
        const bool ok = lua_isstring(L, -1) != 0;
        if (ok) value = lua_tostring(L, -1);
        lua_pop(L, 1);
        return ok;
    }

    bool getField(lua_State* L, int tableIndex, const char* key, double& value) {
        lua_getfield(L, tableIndex, key);
        const bool ok = lua_isnumber(L, -1) != 0;
        if (ok) value = lua_tonumber(L, -1);
        lua_pop(L, 1);
        return ok;
    }

    void setNumberField(lua_State* L, const char* key, double value) {
        lua_pushnumber(L, value);
        lua_setfield(L, -2, key);
    }

    void setStringField(lua_State* L, const char* key, const char* value) {
        lua_pushstring(L, value);
        lua_setfield(L, -2, key);
    }

    const char* noiseName(NoiseType type) {
        switch (type) {
            case NoiseType::PERLIN: return "perlin";
            case NoiseType::VALUE: return "value";
            case NoiseType::SIMPLEX: return "simplex";
        }
        return "simplex";
    }

    NoiseType noiseFromName(const std::string& name) {
        if (name == "perlin") return NoiseType::PERLIN;
        if (name == "value") return NoiseType::VALUE;
        return NoiseType::SIMPLEX;
    }

    // The whole parameter struct as a Lua table, so a script reads a preset, changes one field and
    // hands it straight back.
    void pushParams(lua_State* L, const TerrainGenParams& params) {
        lua_createtable(L, 0, 20);
        setNumberField(L, "size", params.size);
        setNumberField(L, "seed", params.seed);
        setStringField(L, "noise", noiseName(params.type));
        setNumberField(L, "octaves", params.octaves);
        setNumberField(L, "lacunarity", params.lacunarity);
        setNumberField(L, "gain", params.gain);
        setNumberField(L, "frequency", params.frequency);
        setNumberField(L, "base", params.baseHeight);
        setNumberField(L, "amplitude", params.amplitude);
        setNumberField(L, "curve", params.reliefCurve);
        setNumberField(L, "ridged", params.ridgedAmount);
        setNumberField(L, "warp", params.warpAmplitude);
        setNumberField(L, "warpFrequency", params.warpFrequency);
        setNumberField(L, "seaLevel", params.seaLevel);
        setNumberField(L, "island", params.islandFalloff);
        setNumberField(L, "droplets", params.erosionDroplets);
        setNumberField(L, "erosionScale", params.erosionHeightScale);
        setNumberField(L, "erosionSeed", params.erosionSeed);
        setNumberField(L, "talus", params.thermalTalus);
        setNumberField(L, "thermalRate", params.thermalRate);
    }

    // An options table over a preset: the preset fills every field, then the table overrides what it
    // names. With no preset the engine defaults are the base.
    TerrainGenParams readParams(lua_State* L, int tableIndex) {
        std::string text;
        double number = 0.0;

        TerrainGenParams params;
        if (getField(L, tableIndex, "preset", text)) {
            params = TerrainGenerator::preset(TerrainGenerator::presetFromName(text));
        }

        if (getField(L, tableIndex, "size", number)) params.size = static_cast<int>(number);
        if (getField(L, tableIndex, "seed", number)) params.seed = static_cast<uint32_t>(number);
        if (getField(L, tableIndex, "noise", text)) params.type = noiseFromName(text);
        if (getField(L, tableIndex, "octaves", number)) params.octaves = static_cast<int>(number);
        if (getField(L, tableIndex, "lacunarity", number)) params.lacunarity = static_cast<float>(number);
        if (getField(L, tableIndex, "gain", number)) params.gain = static_cast<float>(number);
        if (getField(L, tableIndex, "frequency", number)) params.frequency = static_cast<float>(number);
        if (getField(L, tableIndex, "base", number)) params.baseHeight = static_cast<float>(number);
        if (getField(L, tableIndex, "amplitude", number)) params.amplitude = static_cast<float>(number);
        if (getField(L, tableIndex, "curve", number)) params.reliefCurve = static_cast<float>(number);
        if (getField(L, tableIndex, "ridged", number)) params.ridgedAmount = static_cast<float>(number);
        if (getField(L, tableIndex, "warp", number)) params.warpAmplitude = static_cast<float>(number);
        if (getField(L, tableIndex, "warpFrequency", number)) params.warpFrequency = static_cast<float>(number);
        if (getField(L, tableIndex, "seaLevel", number)) params.seaLevel = static_cast<float>(number);
        if (getField(L, tableIndex, "island", number)) params.islandFalloff = static_cast<float>(number);
        if (getField(L, tableIndex, "droplets", number)) params.erosionDroplets = static_cast<int>(number);
        if (getField(L, tableIndex, "erosionScale", number)) params.erosionHeightScale = static_cast<float>(number);
        if (getField(L, tableIndex, "erosionSeed", number)) params.erosionSeed = static_cast<uint32_t>(number);
        if (getField(L, tableIndex, "talus", number)) params.thermalTalus = static_cast<float>(number);
        if (getField(L, tableIndex, "thermalRate", number)) params.thermalRate = static_cast<float>(number);

        return params;
    }

    // generate(options) -> ok, error, size. `options.out` names the heightmap file to write,
    // relative to the running project.
    int worldGenGenerate(lua_State* L) {
        if (!lua_istable(L, 1)) {
            lua_pushboolean(L, 0);
            lua_pushstring(L, "WorldGen.generate takes an options table");
            return 2;
        }

        std::string out;
        if (!getField(L, 1, "out", out) || out.empty()) {
            lua_pushboolean(L, 0);
            lua_pushstring(L, "WorldGen.generate needs options.out: the heightmap file to write");
            return 2;
        }

        const TerrainGenParams params = readParams(L, 1);
        const bool written = TerrainGenerator::generatePng(params, out);

        lua_pushboolean(L, written ? 1 : 0);
        lua_pushstring(L, written ? "" : ("cannot write " + out).c_str());
        lua_pushinteger(L, TerrainGenerator::clampedSize(params.size));
        return 3;
    }

    int worldGenPresets(lua_State* L) {
        static const TerrainPreset all[] = {
            TerrainPreset::CONTINENTS, TerrainPreset::ISLAND, TerrainPreset::MOUNTAINS,
            TerrainPreset::PLAINS, TerrainPreset::CANYONS
        };

        lua_createtable(L, 5, 0);
        int index = 1;
        for (const TerrainPreset preset : all) {
            lua_pushstring(L, TerrainGenerator::presetName(preset));
            lua_rawseti(L, -2, index++);
        }
        return 1;
    }

    int worldGenParams(lua_State* L) {
        std::string name;
        if (lua_isstring(L, 1)) {
            name = lua_tostring(L, 1);
        }
        pushParams(L, TerrainGenerator::preset(TerrainGenerator::presetFromName(name)));
        return 1;
    }

    // waterHeight(optionsOrPreset, maxHeight) -> the world y a water plane needs for this map.
    int worldGenWaterHeight(lua_State* L) {
        double maxHeight = 1.0;
        if (lua_isnumber(L, 2)) {
            maxHeight = lua_tonumber(L, 2);
        }

        TerrainGenParams params;
        if (lua_istable(L, 1)) {
            params = readParams(L, 1);
        } else if (lua_isstring(L, 1)) {
            params = TerrainGenerator::preset(TerrainGenerator::presetFromName(lua_tostring(L, 1)));
        }

        lua_pushnumber(L, TerrainGenerator::waterHeight(params, static_cast<float>(maxHeight)));
        return 1;
    }

    // A Lua array of strings, read straight into the destination. Used by the material kinds list,
    // where "which materials" is a script's decision rather than the library's.
    bool readStringArray(lua_State* L, int tableIndex, const char* key, std::vector<std::string>& out) {
        lua_getfield(L, tableIndex, key);
        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            return false;
        }
        const int length = static_cast<int>(lua_rawlen(L, -1));
        for (int i = 1; i <= length; ++i) {
            lua_rawgeti(L, -1, i);
            if (lua_isstring(L, -1)) out.push_back(lua_tostring(L, -1));
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
        return !out.empty();
    }

    void pushNames(lua_State* L, const std::vector<std::string>& names) {
        lua_createtable(L, static_cast<int>(names.size()), 0);
        for (size_t i = 0; i < names.size(); ++i) {
            lua_pushstring(L, names[i].c_str());
            lua_rawseti(L, -2, static_cast<int>(i) + 1);
        }
    }

    // materials() -> every material the library can build, by name.
    int worldGenMaterials(lua_State* L) {
        pushNames(L, TerrainMaterialLibrary::materialNames());
        return 1;
    }

    // splatPlan() -> the nine layer names the automatic blend maps put weights into, in the order the
    // terrain reads them: blendMaps[m] channel c weights surfaceLayers[m * 3 + c].
    int worldGenSplatPlan(lua_State* L) {
        std::vector<std::string> names;
        for (const TerrainMaterialKind kind : TerrainSplat::layerPlan()) {
            names.push_back(TerrainMaterialLibrary::materialName(kind));
        }
        pushNames(L, names);
        return 1;
    }

    // writeMaterials(options) -> ok, error, count, entries.
    // options.out is the directory the maps land in (created if missing), options.kinds a name array
    // (default: all of them), options.size a texture side (default 512), options.seed the field seed
    // and options.pathBase the prefix every written path gets - the project's asset directory name, so
    // the material the call returns already points where a scene will look. `entries` is one table per
    // material, in the order requested, with the five paths in it: a script that builds terrain surface
    // layers from these does not have to know how the files got their names.
    int worldGenWriteMaterials(lua_State* L) {
        if (!lua_istable(L, 1)) {
            lua_pushboolean(L, 0);
            lua_pushstring(L, "WorldGen.writeMaterials takes an options table");
            return 2;
        }

        std::string out;
        if (!getField(L, 1, "out", out) || out.empty()) {
            lua_pushboolean(L, 0);
            lua_pushstring(L, "WorldGen.writeMaterials needs options.out: the directory to write into");
            return 2;
        }

        std::string pathBase;
        getField(L, 1, "pathBase", pathBase);

        double number = 0.0;
        int size = TerrainMaterialLibrary::DEFAULT_SIZE;
        if (getField(L, 1, "size", number)) size = static_cast<int>(number);
        uint32_t seed = 1;
        if (getField(L, 1, "seed", number)) seed = static_cast<uint32_t>(number);

        std::vector<TerrainMaterialKind> kinds;
        std::vector<std::string> names;
        if (readStringArray(L, 1, "kinds", names)) {
            for (const std::string& name : names) {
                TerrainMaterialKind kind;
                if (!TerrainMaterialLibrary::tryMaterialFromName(name, kind)) {
                    lua_pushboolean(L, 0);
                    lua_pushstring(L, ("unknown material: " + name).c_str());
                    return 2;
                }
                kinds.push_back(kind);
            }
        } else {
            kinds = TerrainMaterialLibrary::allKinds();
        }

        std::string error;
        std::vector<TerrainMaterialLibrary::Entry> entries;
        const bool ok = TerrainMaterialLibrary::writeLibrary(kinds, seed, size, out, pathBase, entries, error);

        lua_pushboolean(L, ok ? 1 : 0);
        lua_pushstring(L, error.c_str());
        lua_pushinteger(L, ok ? static_cast<int>(entries.size()) : 0);

        lua_createtable(L, static_cast<int>(entries.size()), 0);
        for (size_t i = 0; i < entries.size(); ++i) {
            lua_createtable(L, 0, 6);
            setStringField(L, "kind", TerrainMaterialLibrary::materialName(entries[i].kind));
            setStringField(L, "baseColor", entries[i].files.baseColor.c_str());
            setStringField(L, "normal", entries[i].files.normal.c_str());
            setStringField(L, "metallicRoughness", entries[i].files.metallicRoughness.c_str());
            setStringField(L, "occlusion", entries[i].files.occlusion.c_str());
            setStringField(L, "height", entries[i].files.height.c_str());
            lua_rawseti(L, -2, static_cast<int>(i) + 1);
        }
        return 4;
    }

    void pushRules(lua_State* L, const TerrainSplatRules& rules) {
        lua_createtable(L, 0, 16);
        setNumberField(L, "seed", rules.seed);
        setNumberField(L, "resolution", rules.resolution);
        setNumberField(L, "seaLevel", rules.seaLevel);
        setNumberField(L, "beachWidth", rules.beachWidth);
        setNumberField(L, "shoreDepth", rules.shoreDepth);
        setNumberField(L, "gravelSlope", rules.gravelSlope);
        setNumberField(L, "rockSlope", rules.rockSlope);
        setNumberField(L, "cliffSlope", rules.cliffSlope);
        setNumberField(L, "dirtAltitude", rules.dirtAltitude);
        setNumberField(L, "snowAltitude", rules.snowAltitude);
        setNumberField(L, "iceAltitude", rules.iceAltitude);
        setNumberField(L, "bareness", rules.bareness);
        setNumberField(L, "volcanic", rules.volcanic);
        setNumberField(L, "variation", rules.variation);
        setNumberField(L, "maxWeight", rules.maxWeight);
    }

    TerrainSplatRules readRules(lua_State* L, int tableIndex) {
        TerrainSplatRules rules;
        double number = 0.0;
        if (getField(L, tableIndex, "seed", number)) rules.seed = static_cast<uint32_t>(number);
        if (getField(L, tableIndex, "resolution", number)) rules.resolution = static_cast<int>(number);
        if (getField(L, tableIndex, "seaLevel", number)) rules.seaLevel = static_cast<float>(number);
        if (getField(L, tableIndex, "beachWidth", number)) rules.beachWidth = static_cast<float>(number);
        if (getField(L, tableIndex, "shoreDepth", number)) rules.shoreDepth = static_cast<float>(number);
        if (getField(L, tableIndex, "gravelSlope", number)) rules.gravelSlope = static_cast<float>(number);
        if (getField(L, tableIndex, "rockSlope", number)) rules.rockSlope = static_cast<float>(number);
        if (getField(L, tableIndex, "cliffSlope", number)) rules.cliffSlope = static_cast<float>(number);
        if (getField(L, tableIndex, "dirtAltitude", number)) rules.dirtAltitude = static_cast<float>(number);
        if (getField(L, tableIndex, "snowAltitude", number)) rules.snowAltitude = static_cast<float>(number);
        if (getField(L, tableIndex, "iceAltitude", number)) rules.iceAltitude = static_cast<float>(number);
        if (getField(L, tableIndex, "bareness", number)) rules.bareness = static_cast<float>(number);
        if (getField(L, tableIndex, "volcanic", number)) rules.volcanic = static_cast<float>(number);
        if (getField(L, tableIndex, "variation", number)) rules.variation = static_cast<float>(number);
        if (getField(L, tableIndex, "maxWeight", number)) rules.maxWeight = static_cast<float>(number);
        return rules;
    }

    // splatRules() -> the default rule set, so a script reads it, changes one threshold and hands it
    // straight back to bakeSplat.
    int worldGenSplatRules(lua_State* L) {
        pushRules(L, TerrainSplatRules());
        return 1;
    }

    // bakeSplat(options) -> ok, error, paths.
    // options.heightmap is the 16-bit PNG to read and options.out the directory the three blend maps
    // land in. options.maxHeight and options.terrainSize are the component's, because they set the
    // world scale a slope is measured against; every other field is a rule from splatRules().
    int worldGenBakeSplat(lua_State* L) {
        if (!lua_istable(L, 1)) {
            lua_pushboolean(L, 0);
            lua_pushstring(L, "WorldGen.bakeSplat takes an options table");
            return 2;
        }

        std::string heightmap;
        std::string out;
        if (!getField(L, 1, "heightmap", heightmap) || heightmap.empty()) {
            lua_pushboolean(L, 0);
            lua_pushstring(L, "WorldGen.bakeSplat needs options.heightmap: the map to read");
            return 2;
        }
        if (!getField(L, 1, "out", out) || out.empty()) {
            lua_pushboolean(L, 0);
            lua_pushstring(L, "WorldGen.bakeSplat needs options.out: the directory to write into");
            return 2;
        }

        TerrainSplatRules rules = readRules(L, 1);
        std::string pathBase;
        getField(L, 1, "pathBase", pathBase);

        double number = 0.0;
        float maxHeight = 100.0f;
        if (getField(L, 1, "maxHeight", number)) maxHeight = static_cast<float>(number);
        float terrainSize = 200.0f;
        if (getField(L, 1, "terrainSize", number)) terrainSize = static_cast<float>(number);

        TerrainSplat::Maps maps;
        std::string error;
        if (!TerrainSplat::bakeFromFile(heightmap, maxHeight, terrainSize, rules, maps, error)) {
            lua_pushboolean(L, 0);
            lua_pushstring(L, error.c_str());
            return 2;
        }

        TerrainSplat::Files files;
        if (!TerrainSplat::write(maps, out, pathBase, files, error)) {
            lua_pushboolean(L, 0);
            lua_pushstring(L, error.c_str());
            return 2;
        }

        lua_pushboolean(L, 1);
        lua_pushstring(L, "");
        lua_createtable(L, 3, 0);
        for (int m = 0; m < 3; ++m) {
            lua_pushstring(L, files.blend[m].c_str());
            lua_rawseti(L, -2, m + 1);
        }
        return 3;
    }
}

void LuaBinding::registerUtilClasses(lua_State *L){
#ifndef DISABLE_LUA_BINDINGS

    luabridge::getGlobalNamespace(L)
        .beginClass<Angle>("Angle")
        .addStaticFunction("radToDefault", &Angle::radToDefault)
        .addStaticFunction("degToDefault", &Angle::degToDefault)
        .addStaticFunction("defaultToRad", &Angle::defaultToRad)
        .addStaticFunction("defaultToDeg", &Angle::defaultToDeg)
        .addStaticFunction("radToDeg", &Angle::radToDeg)
        .addStaticFunction("degToRad", &Angle::degToRad)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<Base64>("Base64")
        //.addStaticFunction("encode", &Base64::encode)  //TODO: read and write data in Lua
        //.addStaticFunction("decode", &Base64::decode)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<Color>("Color")
        .addStaticFunction("linearTosRGB", 
            luabridge::overload<Vector3>(&Color::linearTosRGB),
            luabridge::overload<Vector4>(&Color::linearTosRGB),
            luabridge::overload<const float, const float, const float>(&Color::linearTosRGB),
            luabridge::overload<const float, const float, const float, const float>(&Color::linearTosRGB))
        .addStaticFunction("sRGBToLinear", 
            luabridge::overload<Vector3>(&Color::sRGBToLinear),
            luabridge::overload<Vector4>(&Color::sRGBToLinear),
            luabridge::overload<const float, const float, const float>(&Color::sRGBToLinear),
            luabridge::overload<const float, const float, const float, const float>(&Color::sRGBToLinear))
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("NoiseType")
        .addVariable("PERLIN", NoiseType::PERLIN)
        .addVariable("SIMPLEX", NoiseType::SIMPLEX)
        .addVariable("VALUE", NoiseType::VALUE)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginClass<Noise>("Noise")
        .addStaticProperty("MAX_OCTAVES", [] () -> int { return Noise::MAX_OCTAVES; })
        .addStaticFunction("perlin2", &Noise::perlin2)
        .addStaticFunction("perlin3", &Noise::perlin3)
        .addStaticFunction("simplex2", &Noise::simplex2)
        .addStaticFunction("simplex3", &Noise::simplex3)
        .addStaticFunction("value2", &Noise::value2)
        .addStaticFunction("value3", &Noise::value3)
        .addStaticFunction("hash2", &Noise::hash2)
        .addStaticFunction("hash3", &Noise::hash3)
        .addStaticFunction("sample", &Noise::sample)
        .addStaticFunction("sample3", &Noise::sample3)
        .addStaticFunction("fbm", &Noise::fbm)
        .addStaticFunction("fbm3", &Noise::fbm3)
        .addStaticFunction("turbulence", &Noise::turbulence)
        .addStaticFunction("turbulence3", &Noise::turbulence3)
        .addStaticFunction("ridged", &Noise::ridged)
        .addStaticFunction("ridged3", &Noise::ridged3)
        .addStaticFunction("warpedFbm", &Noise::warpedFbm)
        .addStaticFunction("warpedFbm3", &Noise::warpedFbm3)
        .endClass();

    // The wind a scene authored, read at one point and instant. Nothing here stores anything: a
    // script that wants the gust pattern to keep still in world space passes a world position, and
    // one that wants to know what hits its own boat passes the position it cares about.
    luabridge::getGlobalNamespace(L)
        .beginClass<Wind>("Wind")
        .addStaticFunction("sample", &Wind::sample)
        .addStaticFunction("speed", &Wind::speed)
        .addStaticFunction("direction", &Wind::direction)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<VoxelRayHit>("VoxelRayHit")
        .addProperty("hit", &VoxelRayHit::hit)
        .addProperty("distance", &VoxelRayHit::distance)
        .addProperty("blockX", &VoxelRayHit::blockX)
        .addProperty("blockY", &VoxelRayHit::blockY)
        .addProperty("blockZ", &VoxelRayHit::blockZ)
        .addProperty("normalX", &VoxelRayHit::normalX)
        .addProperty("normalY", &VoxelRayHit::normalY)
        .addProperty("normalZ", &VoxelRayHit::normalZ)
        .addProperty("block", &VoxelRayHit::block)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<World>("VoxelWorld")
        .addConstructor<void(), void(uint32_t)>()
        .addFunction("setBlock", &World::setBlock)
        .addFunction("getBlock", &World::getBlock)
        .addFunction("markChunkDirty", &World::markChunkDirty)
        .addFunction("update", &World::update)
        .addFunction("updateAsync", &World::updateAsync)
        .addFunction("rebuildLighting", &World::rebuildLighting)
        .addFunction("raycast", &World::raycast)
        .addFunction("saveRegion", &World::saveRegion)
        .addFunction("saveRegionIncremental", &World::saveRegionIncremental)
        .addFunction("loadRegion", &World::loadRegion)
        .addProperty("seed", &World::getSeed)
        .addProperty("chunkCount", &World::chunkCount)
        .addProperty("pendingChunkCount", &World::pendingChunkCount)
        .addProperty("dirtyChunkCount", &World::dirtyChunkCount)
        .addProperty("meshDirtyChunkCount", &World::meshDirtyChunkCount)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("WorldGen")
        .addFunction("generate", &worldGenGenerate)
        .addFunction("presets", &worldGenPresets)
        .addFunction("params", &worldGenParams)
        .addFunction("waterHeight", &worldGenWaterHeight)
        .addFunction("materials", &worldGenMaterials)
        .addFunction("splatPlan", &worldGenSplatPlan)
        .addFunction("writeMaterials", &worldGenWriteMaterials)
        .addFunction("splatRules", &worldGenSplatRules)
        .addFunction("bakeSplat", &worldGenBakeSplat)
        .endNamespace();

#endif //DISABLE_LUA_BINDINGS
}
