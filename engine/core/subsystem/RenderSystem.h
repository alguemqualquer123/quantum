// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#ifndef RENDERSYSTEM_H
#define RENDERSYSTEM_H

#include "SubSystem.h"
#include "component/MeshComponent.h"
#include "component/InstancedMeshComponent.h"
#include "component/ModelComponent.h"
#include "component/SkyComponent.h"
#include "component/WaterComponent.h"
#include "component/WaterExclusionComponent.h"
#include "component/ReflectionProbeComponent.h"
#include "component/UILayoutComponent.h"
#include "component/UIComponent.h"
#include "component/ImageComponent.h"
#include "component/CameraComponent.h"
#include "component/LightComponent.h"
#include "component/PointsComponent.h"
#include "component/LinesComponent.h"
#include "component/TerrainComponent.h"
#include "component/TilemapComponent.h"
#include "component/SpriteComponent.h"
#include "component/Transform.h"
#include "render/ObjectRender.h"
#include "render/CameraRender.h"
#include "render/BufferRender.h"
#include "render/FramebufferRender.h"
#include "buffer/ExternalBuffer.h"
#include "Engine.h"
#include <map>
#include <array>
#include <memory>
#include <queue>
#include <string>
#include <cstdint>
#include <unordered_map>

namespace quantum{
	// mirrored by the u_fs_lighting block of mesh.frag and water.frag
	typedef struct fs_lighting_t {
	    Vector4 direction_range[MAX_FORWARD_LIGHTS];
	    Vector4 color_intensity[MAX_FORWARD_LIGHTS];
	    Vector4 position_type[MAX_FORWARD_LIGHTS];
	    Vector4 inCon_ouCon_shadows_cascades[MAX_FORWARD_LIGHTS];
	    Vector4 spotUp_maskAspect[MAX_FORWARD_LIGHTS]; // world up.xyz; w > 0 means a ready mask and stores its width/height
		Vector4 eyePos;
		Vector4 cameraDir; // xyz = camera backward axis, w = 3D shadow PCF tap radius
		Vector4 globalIllum; //global illumination
		Vector4 envColor; //environment color.rgb (linear) and environment rotation.w (radians)
		Vector4 viewportInfo; //1.0/viewportSize.xy in .xy (used by USE_SSAO)
		Vector4 clusterInfo; // tile size, grid width, grid height, depth slices
		Vector4 clusterDepth; // near, far, log2(far / near), enabled
		Vector4 clusterDebug; // x = editor overlay enabled
	} fs_lighting_t;

	typedef struct fs_lighting2d_t {
	    Vector4 position_range[MAX_LIGHTS_2D];   // xy = world pos, z = height, w = range
	    Vector4 color_intensity[MAX_LIGHTS_2D];  // rgb = color (linear), w = intensity
	    Vector4 falloff_shadow[MAX_LIGHTS_2D];   // x = falloff exp, y = shadow atlas row (-1 = none), z = softness (texels), w = bias
		Vector4 ambient;                         // rgb = ambient2D (linear) * intensity, w = numLights2D
		Vector4 atlasInfo;                       // x = 1/atlasWidth, y = 1/MAX_LIGHTS_2D, z = atlasWidth, w = PCF tap radius
	} fs_lighting2d_t;

	typedef struct fs_terrain_layers_t {
		Vector4 colorFactor[MAX_TERRAIN_LAYERS]; // rgb = linear tint, w = 1 on a PBR layer
		Vector4 uvTransform[MAX_TERRAIN_LAYERS]; // xy = detail tiling scale, zw = offset
		Vector4 surface[MAX_TERRAIN_LAYERS];     // roughness, metallic, normal strength, occlusion strength
		Vector4 slices[MAX_TERRAIN_LAYERS];      // color, normal and packed surface slice (-1 = none)
	} fs_terrain_layers_t;

	typedef struct fs_reflection_probe_t {
		Vector4 position_weight; // xyz = capture position, w = local probe blend weight
		Vector4 boxMin_intensity; // xyz = influence AABB min, w = intensity
		Vector4 boxMax_lod;       // xyz = influence AABB max, w = max available mip LOD
	} fs_reflection_probe_t;

	typedef struct vs_shadow2d_t {
		Vector4 lightPos_range; // xy = light world pos, w = range
		Vector4 offset;         // x = NDC x offset for the polar seam duplication (-2, 0, +2)
	} vs_shadow2d_t;

	typedef struct fs_fog_t {
		Vector4 color_type;
		Vector4 density_start_end;
	} fs_fog_t;

	// u_fs_shadow_projection, and u_vs_shadows in shader forks that predate it
	typedef struct shadow_projection_t {
	    Matrix4 lightViewProjectionMatrix[MAX_SHADOW_ATLAS_SLOTS];
	    Vector4 shadowParams[MAX_SHADOW_ATLAS_SLOTS]; // normalBias in .x
	    Matrix4 logicalLightViewProjectionMatrix[MAX_SHADOW_ATLAS_SLOTS];
	} shadow_projection_t;

	typedef struct fs_shadows_t {
        Vector4 bias_texSize_nearFar[MAX_SHADOW_ATLAS_SLOTS];
        // xy = atlas origin, zw = atlas scale (directional/spot)
        Vector4 atlasRect[MAX_SHADOW_ATLAS_SLOTS];
	} fs_shadows_t;

	typedef struct fs_point_shadows_t {
        Vector4 bias_texSize_nearFar[MAX_POINT_SHADOW_ATLAS_SLOTS];
        // xy = atlas origin, zw = atlas scale (point cube faces)
        Vector4 atlasRect[MAX_POINT_SHADOW_ATLAS_SLOTS];
	} fs_point_shadows_t;

	typedef struct vs_depth_t {
		Matrix4 modelMatrix;
		Matrix4 lightVPMatrix;
		Matrix4 mvpMatrix; // lightVPMatrix * modelMatrix
	} vs_depth_t;

	typedef struct vs_gbuffer_t {
		Matrix4 modelMatrix;
		Matrix4 viewProjectionMatrix;
		Matrix4 normalMatrix;       // view-space normal matrix: transpose(inverse(view*model))
	} vs_gbuffer_t;

	typedef struct fs_gbuffer_material_t {
		Vector4 params;             // x = roughness, y = metallic, z = IBL source, w = alpha cutout
		Vector4 baseColorFactor;
	} fs_gbuffer_material_t;

	typedef struct fs_ssao_t {
		Matrix4 projection;
		Matrix4 invProjection;
		Vector4 kernel[SSAO_KERNEL_SIZE];
		Vector4 params;     // radius.x, bias.y, intensity.z
		Vector4 noiseScale; // screenSize/noiseSize in .xy
	} fs_ssao_t;

	typedef struct fs_ssao_blur_t {
		Vector4 texelSize; // 1.0/textureSize in .xy
	} fs_ssao_blur_t;

	typedef struct fs_contact_shadows_t {
		Matrix4 viewProjection;
		Matrix4 invViewProjection;
		Vector4 cameraPosition; // xyz = camera world position
		Vector4 lightDirection; // xyz = direction from the surface toward the light
		Vector4 params; // x = ray length, y = step count, z = thickness, w = intensity
	} fs_contact_shadows_t;

	typedef struct fs_ssgi_t {
		Matrix4 projection;
		Matrix4 invProjection;
		Vector4 params0; // x = radius, y = ray count, z = intensity, w = thickness
		Vector4 params1; // xy = 1/depth size, z = frame rotation, w = unused
	} fs_ssgi_t;

	// Mirrors u_fs_materialExtensions in mesh.frag. Kept separate from the legacy
	// 12-float material prefix so old custom shaders and depth variants retain their ABI.
	typedef struct fs_material_extensions_t {
		Vector4 clearcoat;    // x = factor, y = roughness
		Vector4 sheen;        // xyz = color, w = roughness
		Vector4 transmission; // x = factor, y = thickness, z = attenuation distance, w = IOR
		Vector4 attenuation;  // xyz = attenuation color
		Vector4 anisotropy;   // x = strength, y = rotation (radians)
	} fs_material_extensions_t;

	typedef struct fs_ssr_t {
		Matrix4 projection;
		Matrix4 invProjection;
		Vector4 params; // x=maxDistance, y=thickness, z=intensity, w=maxSteps
		Vector4 misc;   // xy=1/depthTextureSize, z=flip depth<->scene-color Y, w=glossy-blur amount
	} fs_ssr_t;

	typedef struct fs_ssr_blur_t {
		Vector4 params; // xy=1/textureSize, z=max radius(pixels), w=flip G-buffer Y
	} fs_ssr_blur_t;

	typedef struct fs_composite_t {
		Matrix4 invProjection;
		Matrix4 invView;
		Vector4 params;   // x=intensity, y=flip G-buffer Y, z=debug, w=unused
		Vector4 envColor; // rgb = env color (linear), w = env rotation (radians)
	} fs_composite_t;

	typedef struct fs_blit_t {
		Vector4 params; // x = flip source Y on sample (GL swapchain destination), yzw unused
	} fs_blit_t;

	typedef struct vs_water_t {
		Matrix4 modelMatrix;
		Matrix4 viewProjectionMatrix; // render matrix, Y-flipped for offscreen GL passes
		Vector4 waves[WATER_WAVE_COUNT]; // xy = direction, z = amplitude, w = wavelength
		Vector4 phases;
		Vector4 params; // x = crest steepness
	} vs_water_t;

	typedef struct vs_water_sim_t {
		Vector4 simMap;    // xy = 1 / world metres of the field, zw = 1 / cells
		Vector4 simOrigin; // xy = world XZ of the field's lower bound
		Vector4 simCell;   // xy = world metres per cell, z = depth under which a cell is dry
		Vector4 simTune;   // x = share of the ground slope carried into the wave normal
	} vs_water_sim_t;

	typedef struct fs_water_t {
		Matrix4 viewProjection;           // logical, finds the scene depth under the fragment
		Matrix4 invViewProjection;        // logical, unprojects the scene depth
		Matrix4 reflectionViewProjection; // logical, of the planar reflection camera
		Vector4 shallowColor;  // rgb = linear color, w = depth fade
		Vector4 deepColor;     // rgb = linear color, w = 1 when the scene depth is valid
		Vector4 foamColor;     // rgb = linear color, w = shore foam depth
		Vector4 ripples;       // x = 1 / tile size, y = strength, z = wave height, w = crest foam
		Vector4 rippleOffsets; // xy = first layer, zw = second layer (tiles)
		Vector4 surface;       // x = reflectivity, y = specular, z = roughness, w = reflection distortion
		Vector4 envColor;      // rgb = sky tint (linear), w = sky rotation (radians)
		Vector4 eyePos;        // xyz = eye of this pass, w = time
		Vector4 flags;         // x = scene lights on, y = IBL ambient available, z = planar reflection, w = eye inside the water
		Vector4 refraction;    // x = distortion, y = 1 when this pass has the scene copy
		Vector4 refractionRect; // xy = view origin, zw = view size, in scene copy uv
		Vector4 caustics;      // x = strength, y = 1 / cell size
		Vector4 waveScale;     // x = longest wavelength
		Vector4 exclusionCount; // x = volumes keeping the water out
		Vector4 exclusionRows[MAX_WATER_EXCLUSIONS * 3]; // world to 0-1 across the volume, three rows each
		Vector4 exclusionShapes[MAX_WATER_EXCLUSIONS];   // x = shape, y = hull map layer
	} fs_water_t;

	typedef struct fs_underwater_t {
		Matrix4 invViewProjection; // logical, unprojects the scene depth
		Vector4 shallowColor;  // rgb = linear color, w = depth fade
		Vector4 deepColor;     // rgb = linear color, w = 1 when the scene depth is valid
		Vector4 light;         // rgb = light reaching the water besides the sky (linear)
		Vector4 envColor;      // rgb = sky tint (linear)
		Vector4 eyePos;        // xyz = eye
		Vector4 sceneRect;     // xy = view origin, zw = view size, in scene copy uv
	} fs_underwater_t;

	typedef struct fs_bloom_threshold_t {
		Vector4 params; // x = threshold, y = soft knee (smooth step), z = 1 when the source is sRGB
	} fs_bloom_threshold_t;

	typedef struct fs_bloom_downsample_t {
		Vector4 texelSize; // 1.0/textureSize of the source in .xy, zw = unused
	} fs_bloom_downsample_t;

	typedef struct fs_bloom_upsample_t {
		Vector4 texelSize; // xy = 1.0/source (smaller) size, z = weight of the same-size level
	} fs_bloom_upsample_t;

	typedef struct fs_bloom_composite_t {
		Vector4 params; // x = intensity, y = 1 when the scene source is sRGB, z = lens dirt, w = unused
	} fs_bloom_composite_t;

	typedef struct fs_tonemap_t {
		Vector4 params; // x = exposure, y = tonemap mode (0=ACES,1=Reinhard,...), z = white point, w = flip source Y
		Vector4 grade;  // x = saturation, y = contrast, z = brightness (EV), w = hue shift in degrees
		Vector4 grade2; // x = temperature, y = tint, z = 1 when grading is on, w = 1 when the exposure comes from the adaptation texel
		Vector4 lut;    // x = 1 when a valid texture2DArray LUT is bound
	} fs_tonemap_t;

	typedef struct fs_exposure_t {
		Vector4 params; // x = min exposure, y = max exposure, z = adaptation speed, w = EV compensation
		Vector4 adapt;  // x = time delta, y = exposure to fall back on with no history, zw unused
	} fs_exposure_t;

	typedef struct fs_colorgrading_t {
		Vector4 params; // x = saturation, y = contrast, z = brightness, w = hue shift
		Vector4 params2; // x = temperature, y = tint, z = has LUT, w = unused
	} fs_colorgrading_t;

	typedef struct fs_fxaa_t {
		Vector4 params; // x = 1/width, y = 1/height, z = blend [0..1], w = flip source Y
	} fs_fxaa_t;

	typedef struct fs_dof_coc_t {
		Matrix4 invProjection;
		Vector4 params0; // x = focus distance, y = aperture, z = focus range, w = max blur
		Vector4 params1; // x = flip depth<->scene-color Y (GL offscreen), zwu = unused
	} fs_dof_coc_t;

	typedef struct fs_dof_blur_t {
		Vector4 params; // x = aspect, y = direction (0 horiz, 1 vert), z = radius cap, w = blur target height in px
	} fs_dof_blur_t;

	typedef struct fs_dof_composite_t {
		Vector4 params; // x = flip source Y, y = intensity, z = far-field bias, w = unused
	} fs_dof_composite_t;

	typedef struct fs_atmosphere_t {
		Matrix4 invViewProjection;
		Vector4 cameraPos;    // xyz = world position, w = 1 whenever the pass runs
		Vector4 sunDirection; // xyz = unit vector toward the sun, w = its intensity
		Vector4 sunScreen;    // xy = sun UV in the depth target's own orientation (clamped to the frame)
		Vector4 fog0;         // x = density, y = scale height, z = ground Y, w = far range
		Vector4 fog1;         // x = sun amount, y = scattering g, z = height fog on, w = depth<->color Y flip
		Vector4 fogColor;     // rgb = linear ambient in-scatter
		Vector4 sunColor;     // rgb = linear sun in-scatter
		Vector4 fogNoise;     // x = scale, y = strength, z = speed, w = enabled
		Vector4 fogWind;      // x/z = wind vector, w = animation time
		Vector4 fogVolume;    // x = enabled, y = ray steps, z = intensity, w = jitter time
		Vector4 rays0;        // x = intensity, y = bright-pass threshold, z = softness, w = per-sample decay
		Vector4 rays1;        // x = marched fraction, y = shafts on, zw = unused
		Vector4 clouds0;      // x = base height, y = thickness, z = coverage, w = density
		Vector4 clouds1;      // x = noise scale, y = wind speed, z = sun amount, w = clouds on
		Vector4 cloudWind;    // x/z = world XZ wind direction and magnitude, y/w = unused
		Vector4 cloudColor;   // rgb = linear cloud albedo, w = animation time
	} fs_atmosphere_t;

	typedef struct fs_decal_t {
		Matrix4 invViewProjection;
		Matrix4 inverseDecalModel;
		Vector4 colorOpacity;
		Vector4 params; // x = edge fade, y = depth UV orientation
	} fs_decal_t;

	typedef struct fs_velocity_t {
		Matrix4 prevViewProjection;
		Matrix4 invCurrentViewProjection;
		Vector4 params; // x = velocity scale, y = maximum encoded UV motion
	} fs_velocity_t;

	typedef struct fs_taa_t {
		Vector4 params0; // x = history blend, y = velocity scale, zw = target size
		Vector4 params1; // x/y = input size, z/w = jitter values reserved for camera jitter integration
	} fs_taa_t;

	typedef struct fs_motion_blur_t {
		Vector4 params; // x = intensity, y = sample count, z = velocity clamp, w = depth threshold
	} fs_motion_blur_t;

	typedef struct vs_points_params_t {
		Matrix4 mvpMatrix;
		float pointScale;
		float simulationTime;
		float padding[2];
	} vs_points_params_t;

	typedef struct check_load_t {
		Scene* scene;
		Entity entity;
	} check_load_t;

	class QUANTUM_API RenderSystem : public SubSystem {
	private:
		enum class TransparentRenderType{
			MESH,
			POINTS
		};

		struct TransparentRenderData{
			TransparentRenderType type;
			// the terrain layer uniforms are looked up by entity, so the sorted draw keeps it
			Entity entity;
			MeshComponent* mesh;
			PointsComponent* points;
			InstancedMeshComponent* instmesh;
			TerrainComponent* terrain;
			TilemapComponent* tilemap;
			Transform* transform;
			float distanceToCamera;
		};

		// opaque mesh of a 3D pass, drawn front to back so early depth rejects covered fragments
		struct OpaqueRenderData{
			Entity entity;
			MeshComponent* mesh;
			InstancedMeshComponent* instmesh;
			TerrainComponent* terrain;
			TilemapComponent* tilemap;
			Transform* transform;
			float distanceToCamera;
		};

		// water a camera pass draws
		struct WaterRenderData{
			Entity entity;
			WaterComponent* water;
			Transform* transform;
			bool eyeAbove;
		};

		// One merged run of tilemap indices to draw, in elements relative to the
		// submesh index range (see selectTilemapChunks).
		struct TilemapDrawRange{
			unsigned int offset;
			unsigned int count;
		};

		struct TransparentRenderComparison{
			bool operator()(const TransparentRenderData& lhs, const TransparentRenderData& rhs) const{
				return lhs.distanceToCamera < rhs.distanceToCamera;
			}
		};

		struct SpotMaskAtlasEntry{
			std::string sourceId;
			const std::array<TextureData, 6>* sourceOwner = nullptr;
			const void* sourcePixels = nullptr;
			int width = 0;
			int height = 0;
			float aspect = 1.0f;

			bool empty() const{
				return sourceId.empty();
			}

			bool operator==(const SpotMaskAtlasEntry& other) const{
				return sourceId == other.sourceId &&
					sourceOwner == other.sourceOwner &&
					sourcePixels == other.sourcePixels &&
					width == other.width &&
					height == other.height;
			}
		};

		Scene* scene;

		// visible ranges of the tilemap submesh being drawn, refilled by
		// selectTilemapChunks and consumed by the draw right after it
		std::vector<TilemapDrawRange> tilemapDrawRanges;

		// Editor-only viewport debug override that forces all meshes to render
		// without face culling. Defaults to false and is never set at runtime, so
		// exported games are unaffected.
		bool disableFaceCulling = false;

		// editor-only, draws as if the scene had no fog
		bool disableFog = false;

		// editor-only, draws as if the camera were never inside a water
		bool disableUnderwater = false;

		// Editor-only overlay that visualizes clustered-light occupancy.
		bool showClusteredLightGrid = false;

		// Editor-only override that suppresses the scene's fixed game resolution
		// (edit-mode viewports render native; play mode re-enables it). Defaults
		// to false and is never set at runtime, so exported games are unaffected.
		bool disableFixedResolution = false;

		// editor-only, holds the waves while the scene is stopped or paused
		bool waterPaused = false;

		static uint32_t pixelsWhite[64];
		static uint32_t pixelsBlack[64];
		static uint32_t pixelsNormal[64];

		static TextureRender emptyWhite;
		static TextureRender emptyArrayWhite;
		static TextureRender emptyBlack;
		static TextureRender emptyCubeBlack;
		static TextureRender emptyCubeWhite;
		static TextureRender emptyNormal;
		static TextureRender emptyShadowDepth;

		static bool emptyTexturesCreated;
		
		bool hasLights;
		bool hasShadows;
		// every light is directional: meshes load the 'Dlo' shader variant
		bool onlyDirectionalLights;
		bool hasFog;
		// Whether the analytic atmosphere pass has taken aerial perspective away from the
		// per-material fog. It is a variant change, so the flip has to be noticed.
		bool hasHeightFog;
		bool hasIBL;
		bool hasReflectionProbes;
		// a water samples the scene depth, so meshes need their depth shaders
		bool hasWaterDepth;
		bool hasMultipleCameras;
		// the scene render distance the projections currently in use were built with; -1 until
		// the first camera update, so a change of the distance can dirty the cameras
		float appliedRenderDistance = -1.0f;
		// extra cameras the last frame drew, to catch the switch back to main only
		bool lastMultiCameraDraw;
		bool capturingReflectionProbe;
		// batch order currently baked into the shared points/instance arrays
		bool lastBatchSort;
		// scene mesh LOD switch the levels were built (or freed) for
		bool lastMeshLodEnabled = false;
		// pipelines the objects were loaded with, to catch a destination change
		uint16_t loadedPipelines;

		// Projected spotlight masks share one horizontal R8 atlas. Each punctual-light
		// array index maps directly to one fixed-size atlas tile.
		TextureRender spotMaskAtlas;
		std::array<SpotMaskAtlasEntry, MAX_LIGHTS> spotMaskAtlasEntries;
		std::vector<unsigned char> spotMaskAtlasPixels;
		bool spotMaskAtlasCreated;

		// one layer per water exclusion hull
		TextureRender waterExclusionMaps;
		int waterExclusionMapLayers;
		bool needUpdateWaterExclusionMaps;

		struct ReflectionProbeRuntime{
			FramebufferRender captureFramebuffer;
			CameraRender capturePass;
			std::shared_ptr<TextureRender> irradianceMap;
			std::shared_ptr<TextureRender> prefilteredMap;
			unsigned int resolution = 0;
			int nextFace = 0;
			float elapsed = 0.0f;
			float retryDelay = 0.0f;
			bool ready = false;
			bool captureInProgress = false;
			bool modeInitialized = false;
			ReflectionProbeMode lastMode = ReflectionProbeMode::STATIC;
			unsigned int observedCaptureRevision = 0;
			Vector3 capturePosition;
			Vector3 capturedPosition;
		};

		std::unordered_map<Entity, std::unique_ptr<ReflectionProbeRuntime>> reflectionProbeRuntimes;
		// Each mirror's reflection camera. Kept here so the authored component stays copiable.
		std::unordered_map<Entity, Entity> mirrorCameras;
		// Main camera position for the instance distance fade: the colour and depth passes must
		// fade against the same point or a shadow outlives its instance.
		Vector3 fadeEyePosition;
		// light component indices in the shader slots, sorted
		std::vector<size_t> activeLights;
		BufferRender clusteredLightGridBuffer;
		std::vector<uint32_t> clusteredLightGridData;
		unsigned int clusteredLightGridWidth;
		unsigned int clusteredLightGridHeight;
		unsigned int clusteredLightGridDepthSlices;
		bool clusteredLightGridReady;
		uint64_t clusteredLightGridKey;
		Vector4 clusteredLightGridInfo;
		Vector4 clusteredLightGridDepth;

		// culled instances of every instanced mesh for one view, uploaded once per frame
		struct InstanceView{
			std::vector<InstanceRenderData> data;
			ExternalBuffer buffer;
			size_t capacity = 0; // bytes of the GPU buffer
			bool needUpload = false;
		};
		InstanceView instanceViews[1 + MAX_SHADOW_ATLAS_SLOTS];
		bool instanceViewsDirty;
		// The two halves of the slices have different reasons to change, and rebuilding the
		// shadow cascades because the player turned around was most of the instanced cost.
		bool mainInstanceViewDirty;    // the player's camera moved or re-qualified its levels
		bool shadowInstanceViewsDirty; // a light's shadow camera moved

		// error a view hides at a distance: threshold * (perUnit * distance + constant)
		struct LodView{
			Vector3 origin;
			float perUnit = 0;   // perspective: units per pixel at distance 1
			float constant = 0;  // orthographic: units per pixel
			float threshold = 1;
			bool enabled = false;
		};
		LodView mainLodView;

		struct ShadowSlot{
			int slot;
			LightComponent* light;
			int cameraIndex;
			int tile;
		};

		// what one pass draws of a mesh
		struct MeshDraw{
			unsigned int instanceCount = 1;
			unsigned int instanceFirst = 0;
			unsigned int lod = 0;
		};
		Entity activeReflectionProbe = NULL_ENTITY;
		fs_reflection_probe_t fs_reflection_probe;

		// 2D light path (Light2DComponent). Deliberately independent of
		// scene->getLightState(): the editor forces LightState::OFF in 2D scenes
		// to keep 3D lights out, but 2D lights must still work there.
		bool hasLights2D;
		bool hasShadows2D;
		int numLights2D;

		// Directional + spot atlas keeps up to 3x3 logical slots, but its physical
		// grid is compacted to the slots used by the current light set.
		FramebufferRender shadowAtlasFramebuffer;
		CameraRender shadowAtlasPassRender;
		unsigned int shadowAtlasSlotResolution;
		int shadowAtlasCols;
		int shadowAtlasRows;
		int shadowAtlasUsedSlots;
		bool needUpdateShadowAtlas;
		bool shadowAtlasContentDirty;
		bool hasShadowAtlas;

		// separate atlas for point-light cube faces
		FramebufferRender shadowPointAtlasFramebuffer;
		CameraRender shadowPointAtlasPassRender;
		unsigned int shadowPointAtlasSlotResolution;
		bool needUpdateShadowPointAtlas;
		bool shadowPointAtlasContentDirty;
		bool hasShadowPointAtlas;

		// 1D polar shadow atlas for 2D lights: width x MAX_LIGHTS_2D, one row per
		// shadow-casting Light2D. Occluder2D segments of the whole scene are merged
		// into one world-space line-list buffer and drawn once per light row.
		FramebufferRender shadow2DAtlasFramebuffer;
		CameraRender shadow2DAtlasPassRender;
		unsigned int shadow2DAtlasWidth;
		bool hasShadow2DAtlas;

		bool occluder2DLoaded;
		ExternalBuffer occluder2DBuffer;
		std::vector<float> occluder2DSegments; // 4 floats per vertex: endpoint xy + other-endpoint xy
		ObjectRender occluder2DRender;
		std::shared_ptr<ShaderRender> shadow2DShader;
		int shadow2DSlotParams;
		unsigned int occluder2DBufferCapacity; // in vertices
		unsigned int occluder2DVertexCount;    // vertices built this frame
		// sokol allows one sg_update_buffer per buffer per frame, but the same scene
		// can be drawn twice in a frame (e.g. open as a tab AND layered as a child
		// scene), so the upload is flagged in update() and consumed by the first draw
		bool occluder2DNeedUpdateBuffer;

		bool needUpdateShadowBindings;
		TextureRender shadowPageTable;
		std::array<unsigned char, MAX_SHADOW_VIRTUAL_PAGES * 4> shadowPageTablePixels;
		bool shadowPageTableCreated;
		// Logical pages follow the light list exposed to shaders, while these arrays
		// keep their physical atlas ownership stable when lights are reordered.
		std::array<int, MAX_SHADOW_VIRTUAL_PAGES> shadowVirtualToPhysical;
		std::array<uint64_t, MAX_SHADOW_ATLAS_SLOTS> shadowPhysicalPageKeys;

		fs_lighting_t fs_lighting;
		fs_lighting2d_t fs_lighting2d;
		shadow_projection_t shadowProjection;
		fs_shadows_t fs_shadows;
		fs_point_shadows_t fs_point_shadows;
		fs_fog_t fs_fog;

		// SSAO: per-frame fullscreen passes for the main camera. The depth pre-pass
		// renders camera-space packed depth, then the ssao + blur fullscreen passes
		// produce an AO texture sampled by lit meshes (USE_SSAO).
		bool ssaoLoaded;
		unsigned int ssaoWidth;
		unsigned int ssaoHeight;
		Framebuffer ssaoDepthFramebuffer; // color = packed depth, depth = z-test
		Framebuffer ssaoFramebuffer;      // raw AO
		Framebuffer ssaoBlurFramebuffer;  // blurred AO (sampled by meshes)
		CameraRender ssaoPassRender;      // drives the offscreen SSAO passes
		ObjectRender ssaoRender;          // fullscreen ssao.frag draw
		ObjectRender ssaoBlurRender;      // fullscreen ssao_blur.frag draw
		TextureRender ssaoNoiseTexture;
		std::shared_ptr<ShaderRender> ssaoShader;
		std::shared_ptr<ShaderRender> ssaoBlurShader;
		fs_ssao_t fs_ssao; // kernel filled once in loadSSAO; matrices/params per frame
		fs_ssao_blur_t fs_ssao_blur;
		int ssaoSlotParams;
		int ssaoBlurSlotParams;
		TextureRender* currentSSAOTexture; // AO bound to meshes this camera (or empty white)

		// Contact shadows: a short screen-space march toward the strongest directional
		// light, composited back into the color target before HDR and post-processing.
		bool contactShadowsLoaded;
		bool contactShadowsHDR;
		unsigned int contactShadowsWidth;
		unsigned int contactShadowsHeight;
		Framebuffer contactShadowsFramebuffer;
		CameraRender contactShadowsPassRender;
		ObjectRender contactShadowsRender;
		std::shared_ptr<ShaderRender> contactShadowsShader;
		fs_contact_shadows_t fs_contactShadows;
		int contactShadowsSlotParams;

		// SSGI: a screen-space diffuse bounce pass over the opaque scene color. It
		// reconstructs view-space positions from the packed depth pre-pass and returns
		// the original color plus the gathered near-field radiance.
		bool ssgiLoaded;
		bool ssgiHDR;
		unsigned int ssgiWidth;
		unsigned int ssgiHeight;
		Framebuffer ssgiFramebuffer;
		CameraRender ssgiPassRender;
		ObjectRender ssgiRender;
		std::shared_ptr<ShaderRender> ssgiShader;
		fs_ssgi_t fsSSGI;
		int ssgiSlotParams;

		// Screen-space projected decals. The pass reads the shared packed depth and
		// overlays each authored projection box onto the current scene color.
		bool decalsLoaded;
		bool decalsHDR;
		ObjectRender decalRender;
		CameraRender decalPassRender;
		std::shared_ptr<ShaderRender> decalShader;
		fs_decal_t fsDecal;
		int decalSlotParams;

		// packed opaque depth of this camera pass, or null
		TextureRender* currentSceneDepthTexture;
		bool depthPrePassRendered;
		// copy of the scene color drawn before the water, for the main camera
		Framebuffer sceneCopyFramebuffer;
		CameraRender resumePassRender; // continues the camera pass after the copy
		TextureRender* currentSceneCopy;
		Vector4 currentSceneCopyRect;
		// the water holding the main camera, which fades the scene copy
		WaterComponent* currentUnderwater;

		// engine-written custom uniforms: seconds since startup, sampled once per draw(),
		// and the size of the target being drawn (shadow slot, SSAO depth or camera color)
		float frameTime;
		Vector2 passResolution;

		// SSR: per-frame fullscreen passes for the main camera. The opaque color pass
		// renders into sceneColorFramebuffer, the ssr pass marches the depth pre-pass
		// and samples that color, then the composite pass blends reflections into the
		// real render target.
		bool ssrLoaded;
		unsigned int ssrWidth;
		unsigned int ssrHeight;
		Framebuffer gbufferFramebuffer;    // MRT: color[0] packed depth, color[1] view-space normal/roughness/metallic
		Framebuffer sceneColorFramebuffer; // offscreen opaque scene color (top-left)
		bool sceneColorHDR = false;        // that target is the float one the HDR chain wants
		Framebuffer ssrFramebuffer;        // reflection color + mask (logical orientation)
		Framebuffer ssrBlurFramebuffer;    // glossy-blurred reflection
		Framebuffer ssrCompositeFramebuffer; // HDR-safe SSR composite before bloom/tone map
		bool ssrCompositeHDR = false;
		CameraRender gbufferPassRender;    // drives the MRT G-buffer geometry pass
		CameraRender ssrPassRender;        // drives the offscreen ssr + composite passes
		ObjectRender ssrRender;            // fullscreen ssr.frag draw
		ObjectRender ssrBlurRender;        // fullscreen ssr_blur.frag draw
		ObjectRender compositeRender;      // fullscreen composite.frag draw
		std::shared_ptr<ShaderRender> ssrShader;
		std::shared_ptr<ShaderRender> ssrBlurShader;
		std::shared_ptr<ShaderRender> compositeShader;
		fs_ssr_t fs_ssr;
		fs_ssr_blur_t fs_ssr_blur;
		fs_composite_t fs_composite;
		int ssrSlotParams;
		int ssrBlurSlotParams;
		int compositeSlotParams;
		bool swapchainRedirect;            // exported builds: the last pass targets the swapchain

		// Fixed game resolution: when enabled on the Engine main scene (Scene
		// fixedResolution settings), the main camera renders into
		// fixedResFramebuffer and a fullscreen blit pass upscales it to the
		// real destination (view rect).
		bool blitLoaded;
		unsigned int fixedResWidth;
		unsigned int fixedResHeight;
		Framebuffer fixedResFramebuffer;   // offscreen scene color at the fixed size
		CameraRender blitPassRender;       // drives the fullscreen blit passes
		ObjectRender blitRender;           // fullscreen blit.frag draw
		std::shared_ptr<ShaderRender> blitShader;
		fs_blit_t fs_blit;
		int blitSlotParams;

		// User post-process chain: forked fullscreen shaders run after the color (and
		// SSR) pass, ping-ponging between two buffers into the real destination.
		struct PostProcessRuntime{
			std::shared_ptr<ShaderRender> shader;
			ObjectRender render;
			int passIndex;                         // index in the scene chain (disabled passes skipped)
			uint16_t customId;                     // 0 = built-in passthrough
			CustomUniformBlock params;             // u_fs_postParams, slot -1 when the fork has none
			std::pair<int, int> slotSceneColor;
			std::pair<int, int> slotDepth;
			std::pair<int, int> slotGBuffer;
			std::pair<int, int> slotSSAO;
		};
		bool postProcessLoaded;
		bool postProcessNeedReload;
		bool postProcessNeedUpdateUniforms;    // rewrite pass values, no chain rebuild
		bool postProcessNeedsDepth;            // a pass samples the depth texture
		bool postProcessNeedsGBuffer;          // a pass samples the G-buffer (needs SSR)
		unsigned int postProcessWidth;
		unsigned int postProcessHeight;
		Framebuffer postProcessFramebuffer[2]; // ping-pong targets
		CameraRender postProcessPassRender;    // drives the offscreen post passes
		std::vector<PostProcessRuntime> postProcessPasses;

		// HDR Bloom: extracts bright pixels, downsamples via Gaussian, upsamples with
		// tent filter, and composites onto scene color before tone mapping.
		bool bloomLoaded;
		unsigned int bloomWidth;
		unsigned int bloomHeight;
		Framebuffer bloomThresholdFramebuffer;  // extracted bright pixels (half-res)
		Framebuffer bloomDownsampleFramebuffers[5]; // downsample chain (halving each)
		Framebuffer bloomUpsampleFramebuffers[5];   // upsample chain (doubling each)
		CameraRender bloomPassRender;            // drives the fullscreen bloom passes
		ObjectRender bloomThresholdRender;       // threshold extraction draw
		ObjectRender bloomDownsampleRender;      // Gaussian downsample draw
		ObjectRender bloomUpsampleRender;        // tent upsample draw
		ObjectRender bloomCompositeRender;       // composite onto scene color draw
		std::shared_ptr<ShaderRender> bloomThresholdShader;
		std::shared_ptr<ShaderRender> bloomDownsampleShader;
		std::shared_ptr<ShaderRender> bloomUpsampleShader;
		std::shared_ptr<ShaderRender> bloomCompositeShader;
		fs_bloom_threshold_t fs_bloom_threshold;
		fs_bloom_downsample_t fs_bloom_downsample;
		fs_bloom_upsample_t fs_bloom_upsample;
		fs_bloom_composite_t fs_bloom_composite;
		int bloomThresholdSlotParams;
		int bloomDownsampleSlotParams;
		int bloomUpsampleSlotParams;
		int bloomCompositeSlotParams;
		int activeBloomMipLevels; // clamped to [1,5] from scene settings
		float bloomIntensity;    // bloom composite multiplier (0..10)

		// Tone mapping: HDR → LDR conversion after bloom composite.
		bool tonemapLoaded;
		CameraRender tonemapPassRender;
		ObjectRender tonemapRender;
		std::shared_ptr<ShaderRender> tonemapShader;
		fs_tonemap_t fs_tonemap;
		int tonemapSlotParams;

		// Eye adaptation: one 1x1 texel per frame, ping-ponged so the pass can read the
		// previous frame's value without a CPU readback.
		bool exposureLoaded;
		CameraRender exposurePassRender;
		ObjectRender exposureRender;
		std::shared_ptr<ShaderRender> exposureShader;
		fs_exposure_t fs_exposure;
		int exposureSlotParams;
		Framebuffer exposureFramebuffers[2];
		int exposureWriteIndex;
		unsigned int hdrFeatureMask; // last logged bloom/tonemap/grading/adaptation set

		// Color grading: LUT-based or procedural color adjustments after tone mapping.
		bool colorGradingLoaded;
		CameraRender colorGradingPassRender;
		ObjectRender colorGradingRender;
		std::shared_ptr<ShaderRender> colorGradingShader;
		fs_colorgrading_t fs_colorgrading;
		int colorGradingSlotParams;
		TextureRender lut3DTexture;  // 3D LUT texture (64x64x64 or 32x32x32)
		bool lut3DLoaded;
		int lut3DSize;
		std::vector<unsigned char> lut3DPixels;
		std::string lut3DPath;

		// the linear target the bloom composite writes and the tone map reads; one per
		// effect resolution, so it is rebuilt with the chain on a resize
		bool hdrLoaded;
		Framebuffer hdrFramebuffer;
		unsigned int hdrWidth;
		unsigned int hdrHeight;

		// FXAA: the last stage of the frame. It reads the finished image, so whatever ran
		// before it is redirected into fxaaFramebuffer and FXAA presents that to the real
		// destination.
		bool fxaaLoaded;
		Framebuffer fxaaFramebuffer;
		unsigned int fxaaWidth;
		unsigned int fxaaHeight;
		CameraRender fxaaPassRender;
		ObjectRender fxaaRender;
		std::shared_ptr<ShaderRender> fxaaShader;
		fs_fxaa_t fs_fxaa;
		int fxaaSlotParams;

		// Depth of field: CoC from the shared depth pre-pass, a half-resolution separable
		// blur, then a mix back over the scene color. It reads the scene color offscreen
		// like SSR/HDR, so it follows the same destination-capture rule, and it writes its
		// result back into sceneColorFramebuffer so every later stage is unchanged.
		bool dofLoaded;
		unsigned int dofWidth;
		unsigned int dofHeight;
		unsigned int dofHalfWidth;
		unsigned int dofHalfHeight;
		Framebuffer dofCocFramebuffer;      // half res, R = circle of confusion
		Framebuffer dofBlurFramebuffer[2];  // half res, horizontal then vertical
		Framebuffer dofSceneFramebuffer;    // full res, the composited scene color
		CameraRender dofPassRender;
		ObjectRender dofCocRender;
		ObjectRender dofBlurRender;
		ObjectRender dofCompositeRender;
		std::shared_ptr<ShaderRender> dofCocShader;
		std::shared_ptr<ShaderRender> dofBlurShader;
		std::shared_ptr<ShaderRender> dofCompositeShader;
		fs_dof_coc_t fs_dof_coc;
		fs_dof_blur_t fs_dof_blur;
		fs_dof_composite_t fs_dof_composite;
		int dofCocSlotParams;
		int dofBlurSlotParams;
		int dofCompositeSlotParams;

		// Atmosphere: one fullscreen pass that integrates the height fog along the ray and
		// marches the light shafts toward the sun. It reads the scene color and the shared
		// depth, so it needs its own full-res target and copies the result back into the
		// scene color exactly like DOF does, leaving every later stage (post-process, HDR,
		// upscale, FXAA) reading the buffer it already expects.
		bool atmosphereLoaded;
		unsigned int atmosphereWidth;
		unsigned int atmosphereHeight;
		Framebuffer atmosphereFramebuffer;
		CameraRender atmospherePassRender;
		ObjectRender atmosphereRender;
		std::shared_ptr<ShaderRender> atmosphereShader;
		fs_atmosphere_t fs_atmosphere;
		int atmosphereSlotParams;

		// Temporal passes share the packed depth pre-pass. History is ping-ponged so the
		// previous frame is never sampled from the target currently being written.
		bool velocityLoaded;
		bool taaLoaded;
		bool motionBlurLoaded;
		unsigned int temporalWidth;
		unsigned int temporalHeight;
		unsigned int temporalInputWidth;
		unsigned int temporalInputHeight;
		Framebuffer velocityFramebuffer;
		Framebuffer taaHistoryFramebuffers[2];
		Framebuffer motionBlurFramebuffer;
		CameraRender temporalPassRender;
		ObjectRender velocityRender;
		ObjectRender taaRender;
		ObjectRender motionBlurRender;
		std::shared_ptr<ShaderRender> velocityShader;
		std::shared_ptr<ShaderRender> taaShader;
		std::shared_ptr<ShaderRender> motionBlurShader;
		fs_velocity_t fs_velocity;
		fs_taa_t fs_taa;
		fs_motion_blur_t fs_motion_blur;
		int velocitySlotParams;
		int taaSlotParams;
		int motionBlurSlotParams;
		int taaHistoryIndex;
		bool taaHistoryValid;
		Matrix4 previousViewProjection;
		bool previousViewProjectionValid;

		static void changeLoaded(void* data);
		static void changeDestroy(void* data);
		// water can share its entity with a mesh, so it has its own callbacks
		static void changeWaterLoaded(void* data);
		static void changeWaterDestroy(void* data);

		static bool samplesCameraTarget(const CameraComponent& camera, const MeshComponent& mesh);
		static bool samplesCameraTarget(const CameraComponent& camera, const Texture& texture);
		bool isRenderingFlipped(const CameraComponent& camera) const;
		bool isFixedResolutionActive() const;
		void updateSwapchainRedirect();
		void updateMVP(size_t index, Transform& transform, CameraComponent& camera, Transform& cameraTransform);

		void createEmptyTextures();
		std::vector<size_t> chooseLights(const Vector3& eye, const std::vector<size_t>& inUse);
		void selectLights(const Vector3& eye);
		void applyViewLights(const Vector3& eye);
		void setLightSlot(int slot, size_t index);
		void checkLightsAndShadow();
		bool loadLights(int numLights);
		void processLights(int numLights, CameraComponent& camera, Transform& cameraTransform);
		void updateClusteredLightGrid(CameraComponent& camera);
		void updateSpotMaskAtlas(int numLights);
		void loadSpotMaskTexture(ShaderData& shaderData, ObjectRender& render);
		bool loadLights2D();
		void processLights2D();
		bool ensureShadow2DAtlas(unsigned int width);
		void loadShadow2DTexture(ShaderData& shaderData, ObjectRender& render, bool receiveShadows2D);
		unsigned int buildOccluder2DSegments();
		bool loadOccluder2DPass(unsigned int vertexCapacity);
		void destroyOccluder2DPass();
		bool loadAndProcessFog();
		void releaseSkyEnvironment(SkyComponent& sky);
		void updateSkyEnvironment(SkyComponent& sky);
		void updateReflectionProbes(double dt);
		void renderReflectionProbeCapture();
		void releaseReflectionProbeMaps(ReflectionProbeRuntime& runtime);
		void destroyReflectionProbe(Entity entity, ReflectionProbeComponent& probe);
		bool selectReflectionProbe(const Vector3& worldPosition, fs_reflection_probe_t& params, TextureRender*& texture);
		void initShadowAtlasRects();
		void initShadowPointAtlasRects();
		unsigned int clampShadowAtlasSlotResolution(unsigned int requestedResolution, int atlasCols, int atlasRows) const;
		bool ensureShadowAtlas(unsigned int slotResolution, int usedSlots);
		bool ensureShadowPointAtlas(unsigned int slotResolution);
		Rect getShadowAtlasSlotRect(int slotIndex) const;
		Rect getShadowPointAtlasSlotRect(int slotIndex) const;
		void configureLightShadowNearFar(LightComponent& light, const CameraComponent& camera);
		Matrix4 getDirLightProjection(const Matrix4& viewMatrix, const Matrix4& sceneCameraInv, float shadowMaxDistance, const Vector3& lightDirection, const Vector3& cameraPosition);
		int getShadowReceiverPlanes(const CameraComponent& camera, const LightComponent& light, int cascade, Plane* planes);
		bool checkPBRFrabebufferUpdate(Material& material);
		bool checkPBRTextures(Material& material, bool receiveLights);
		bool loadPBRTextures(Material& material, ShaderData& shaderData, ObjectRender& render, bool receiveLights);
		void loadShadowTextures(ShaderData& shaderData, ObjectRender& render, bool receiveLights, bool receiveShadow);
		void updateShadowPageTable();
		void updateShadowBindings();
		bool loadDepthTexture(Material& material, ShaderData& shaderData, ObjectRender& render);
		bool loadGBufferTextures(Material& material, ShaderData& shaderData, ObjectRender& render);
		// Every map of every layer is a slice of one array texture, so the whole terrain
		// costs a single bind slot. Kept here, not on the component, which stays copiable.
		struct TerrainDetailArray{
			TextureRender render;
			// what each slice was built from, in array order
			std::vector<std::string> sliceKeys;
			// how many leading slices hold layer colors (rounded up to whole blend maps)
			int colorSlices = 0;
			// where a layer's normal and packed surface slices landed, -1 when it has none
			std::array<int, MAX_TERRAIN_LAYERS> normalSlice;
			std::array<int, MAX_TERRAIN_LAYERS> surfaceSlice;
			// one sampler serves every slice, so its settings are part of what the cache holds
			TextureFilter minFilter = TextureFilter::LINEAR;
			TextureFilter magFilter = TextureFilter::LINEAR;
			TextureWrap wrapU = TextureWrap::REPEAT;
			TextureWrap wrapV = TextureWrap::REPEAT;
			bool failed = false;
		};
		std::unordered_map<Entity, TerrainDetailArray> terrainDetailArrays;

		TerrainDetailArray* getTerrainDetailArray(Entity entity, TerrainComponent& terrain);
		void destroyTerrainDetailArray(Entity entity);
		void applyTerrainLayersUniform(ObjectRender& render, int slot, Entity entity, TerrainComponent& terrain);
		bool loadTerrainTextures(Entity entity, TerrainComponent& terrain, ObjectRender& render, ShaderData& shaderData);
		bool loadTerrainHeightTexture(TerrainComponent& terrain, ObjectRender& render, ShaderData& shaderData);
		bool updateTerrainRenderTextures(Entity entity, TerrainComponent& terrain, MeshComponent& mesh);
		void updateAllTerrainRenderTextures();
		void updateInstanceBuffers();
		Rect getScissorRect(UILayoutComponent& layout, ImageComponent& img, Transform& transform, const Rect& passViewport);

		// mesh LOD
		struct LodAttribute{
			Buffer* buffer = nullptr;
			Attribute* attribute = nullptr;
		};
		struct LodAttributes{
			LodAttribute position;
			LodAttribute index;
			LodAttribute normal;
			LodAttribute uv;
		};
		void updateMeshLods(Entity entity, MeshComponent& mesh, bool rehash);
		static LodAttribute findSubmeshAttribute(Submesh& submesh, std::map<std::string, Buffer*>& buffers, AttributeType type);
		static bool findLodAttributes(Submesh& submesh, std::map<std::string, Buffer*>& buffers, LodAttributes& attributes);
		static uint64_t attributeHash(uint64_t seed, const LodAttribute& attribute);
		static bool readLodSource(const LodAttributes& attributes, MeshLodSource& source);
		void pollMeshLod(MeshComponent& mesh, Submesh& submesh);
		static bool releaseSubmeshLod(Submesh& submesh);
		void releaseMeshLods(MeshComponent& mesh);
		static bool isMeshLodPending(Submesh& submesh);
		void updateMeshLodErrors(MeshComponent& mesh);
		void updateMainLodView(CameraComponent& camera, Transform& cameraTransform);
		unsigned int selectMeshLod(const MeshComponent& mesh, const LodView* view, float distance, float worldScale) const;
		LightCamera shadowTileCamera(const LightComponent& light, int cameraIndex, int tile);
		LodView shadowLodView(const LightComponent& light, int cameraIndex, int slot, int tile);
		std::vector<ShadowSlot> collectShadowSlots();
		void rebuildShadowPageMapping(int usedSlots);

		// instance views
		void buildInstanceViews(CameraComponent& mainCamera, Transform& mainCameraTransform);
		void buildInstanceView(int viewIndex, InstancedMeshComponent& instmesh, MeshComponent& mesh, Transform& transform, const float cameraFar, const Plane frustumPlanes[6], const LodView& lodView, const Vector3& eye, const Vector3& origin, const Vector3& direction, bool keepOrder);
		bool selectMeshDraw(MeshComponent& mesh, Transform& transform, InstancedMeshComponent* instmesh, bool ownLod, int instanceView, const LodView* lodView, MeshDraw& draw);
		void bindInstances(ObjectRender& render, InstancedMeshComponent& instmesh, int instanceView, unsigned int first);
		void drawSubmeshGeometry(ObjectRender& render, MeshComponent& mesh, unsigned int submeshIndex, InstancedMeshComponent* instmesh, int instanceView, unsigned int lod, unsigned int instanceCount);

		// terrain
		bool terrainNodeLODSelect(TerrainComponent& terrain, Transform& transform, CameraComponent& camera, Transform& cameraTransform, TerrainNode& terrainNode, int lodLevel, int viewIndex);
		AABB getTerrainNodeAABB(Transform& transform, TerrainNode& terrainNode);
		bool isTerrainNodeInSphere(Vector3 position, float radius, const AABB& box);

		// tilemap
		bool selectTilemapChunks(TilemapComponent& tilemap, unsigned int submeshIndex, const float cameraFar, const Plane frustumPlanes[6]);

		float lerp(float a, float b, float fraction);

	protected:

		void updateMeshBuffers(MeshComponent& mesh);
		void updateTerrainNodesBuffer(TerrainComponent& terrain, int viewIndex);
		// instanceView -1 draws every instance from the component buffer; lodView null is full detail
		bool drawMesh(Entity entity, MeshComponent& mesh, Transform& transform, CameraComponent& camera, Transform& camTransform, PipelineType pipType, InstancedMeshComponent* instmesh, TerrainComponent* terrain, TilemapComponent* tilemap, int terrainView = 0, int instanceView = -1, const LodView* lodView = nullptr);
		bool drawMeshDepth(MeshComponent& mesh, Transform& transform, const float cameraFar, const Plane frustumPlanes[6], vs_depth_t vsDepthParams, InstancedMeshComponent* instmesh, TerrainComponent* terrain, TilemapComponent* tilemap, bool forSSAO = false, PipelineType pipelineType = PIP_DEPTH, int instanceView = -1, const LodView* lodView = nullptr);
		void destroyMesh(Entity entity, MeshComponent& mesh, bool clearAssets = false);

		// SSAO
		void loadSSAO();
		void destroySSAO();
		bool ensureDepthPrePassFramebuffer(unsigned int width, unsigned int height);
		bool ensureSSAOFramebuffers(unsigned int width, unsigned int height);
		// sharedDepth != null reuses an existing packed-depth texture (the SSR G-buffer),
		// skipping the SSAO depth pre-pass; null runs the pre-pass
		void renderSSAO(CameraComponent& camera, TextureRender* sharedDepth = nullptr);
		void loadContactShadows();
		void destroyContactShadows();
		bool ensureContactShadowsFramebuffer(unsigned int width, unsigned int height, bool hdr);
		void renderContactShadows(CameraComponent& camera, Transform& cameraTransform,
				Framebuffer* sceneColorTarget, TextureRender* depthTexture);
		void loadSSGI();
		void destroySSGI();
		bool ensureSSGIFramebuffer(unsigned int width, unsigned int height, bool hdr);
		void renderSSGI(CameraComponent& camera, Framebuffer* sceneColorTarget, TextureRender* depthTexture);
		void loadDecals(bool hdr);
		void destroyDecals();
		void renderDecals(CameraComponent& camera, Framebuffer* sceneColorTarget, TextureRender* depthTexture);

		// SSR
		void loadSSR();
		void destroySSR();
		bool ensureSceneColorFramebuffer(unsigned int width, unsigned int height);
		bool ensureSSRFramebuffers(unsigned int width, unsigned int height);
		void renderDepthPrePass(CameraComponent& camera); // shared depth for SSAO/SSR
		// G-buffer geometry pass for SSR: MRT packed depth + view-space normal/roughness/metallic
		bool ensureGBufferFramebuffer(unsigned int width, unsigned int height);
		void renderGBufferPass(CameraComponent& camera);
		bool drawMeshGBuffer(Entity entity, MeshComponent& mesh, Transform& transform, const float cameraFar, const Plane frustumPlanes[6], vs_gbuffer_t vsGBufferParams, bool hasLocalProbe, InstancedMeshComponent* instmesh, TerrainComponent* terrain, TilemapComponent* tilemap, int instanceView = -1, const LodView* lodView = nullptr);
		// destination == nullptr renders the composite to the swapchain (backbuffer)
		void renderSSR(CameraComponent& camera, FramebufferRender* destination,
			bool compositeIntoSceneColor = false);

		// fixed game resolution
		void loadBlit();
		void destroyBlit();
		bool ensureFixedResFramebuffer(unsigned int width, unsigned int height, TextureFilter filter);
		// draws source over destination (the swapchain when null), in viewport or the whole target
		void renderBlit(TextureRender* source, FramebufferRender* destination, const Rect* viewport);
		// the fullscreen blit draw alone, in the pass already started
		void drawBlit(TextureRender* source, PipelineType pipeline, bool flipY);
		bool ensureSceneCopyFramebuffer(unsigned int width, unsigned int height);
		void renderSceneCopy(Framebuffer* source);
		// upscales fixedResFramebuffer to the view rect (destination == nullptr presents the
		// swapchain or the captured Engine framebuffer, as today)
		void renderFixedResolutionBlit(FramebufferRender* destination = nullptr);

		// user post-process chain
		void loadPostProcess();
		void updatePostProcessUniforms();
		void destroyPostProcess();
		bool ensurePostProcessFramebuffers(unsigned int width, unsigned int height);
		// runs the chain from ping-pong buffer 0 into destination (swapchain when null)
		void renderPostProcess(FramebufferRender* destination);

		// HDR chain: threshold, the bloom mip chain, the composite over the scene color
		// and the tone map. It reads sceneColorFramebuffer, so the color pass has to be
		// redirected there (see useHDR in draw()).
		void loadHDR();
		void destroyHDR();
		bool ensureHDRFramebuffers(unsigned int width, unsigned int height, int levels);
		// the two 1x1 adaptation targets; created once, they hold the history between frames
		bool ensureExposureFramebuffers();
		bool ensureColorGradingLUT();
		// destination == nullptr renders the tone map to the swapchain (backbuffer)
		void renderHDR(FramebufferRender* destination);

		// FXAA over the finished image. It runs last, after the tone map and after the
		// fixed-resolution upscale, so its texel size is the one the viewer sees.
		void loadFXAA();
		void destroyFXAA();
		bool ensureFXAAFramebuffer(unsigned int width, unsigned int height);
		// destination == nullptr presents the swapchain (backbuffer)
		void renderFXAA(FramebufferRender* destination);

		// Depth of field. The CoC reads the shared depth pre-pass and the blur reads the
		// offscreen scene color; the composite writes to a scratch of the same size and a
		// final blit puts it back, so the buffer the caller passed is the one the rest of
		// the frame keeps reading.
		void loadDOF();
		void destroyDOF();
		bool ensureDOFFramebuffers(unsigned int width, unsigned int height);
		void renderDOF(CameraComponent& camera, Framebuffer* sceneColorTarget, TextureRender* depthTexture);
		void loadAtmosphere();
		void destroyAtmosphere();
		bool ensureAtmosphereFramebuffer(unsigned int width, unsigned int height);
		// True when the scene's FogComponent asks for either effect. The redirect of an
		// exported build and the pass itself both gate on it, so neither can drift.
		bool atmosphereRequested() const;
		// Fills the block from the scene's FogComponent and the strongest directional light.
		// Returns false when neither switch is on, so the caller can skip the pass entirely.
		bool buildAtmosphereParams(CameraComponent& camera, Transform& cameraTransform);
		void renderAtmosphere(CameraComponent& camera, Transform& cameraTransform,
				Framebuffer* sceneColorTarget, TextureRender* depthTexture);

		// Temporal reconstruction and velocity-driven blur. Both operate before HDR so
		// history remains in the same linear scene-color space as the current frame.
		void loadTemporalEffects();
		void destroyTemporalEffects();
		bool ensureTemporalFramebuffers(unsigned int inputWidth, unsigned int inputHeight,
				unsigned int outputWidth, unsigned int outputHeight, bool hdr);
		void renderVelocity(CameraComponent& camera, TextureRender* depthTexture);
		void renderTAA(CameraComponent& camera, Framebuffer* sceneColorTarget, TextureRender* depthTexture,
				bool copyBack = true);
		void renderMotionBlur(Framebuffer* sceneColorTarget, TextureRender* depthTexture);

		bool drawUI(UIComponent& ui, Transform& transform, PipelineType pipType);
		void destroyUI(Entity entity, UIComponent& ui);

		bool drawPoints(PointsComponent& points, Transform& transform, CameraComponent& camera, Transform& camTransform, PipelineType pipType);
		float getPointsViewportHeight(const CameraComponent& camera) const;
		float computePointsScale(const CameraComponent& camera, float viewportHeight) const;
		void destroyPoints(Entity entity, PointsComponent& points);

		bool drawLines(LinesComponent& lines, Transform& transform, Transform& camTransform, PipelineType pipType);
		void destroyLines(Entity entity, LinesComponent& lines);

		bool drawSky(SkyComponent& sky, PipelineType pipType);
		void destroySky(Entity entity, SkyComponent& sky);

		std::shared_ptr<TextureRender> getDefaultWaterNormalMap();
		void updateWater(Entity entity, WaterComponent& water, Transform& transform, uint16_t pipelines, double dt);
		void advanceWater(WaterComponent& water, float seconds);
		bool drawWater(Entity entity, WaterComponent& water, Transform& transform, CameraComponent& camera, Transform& camTransform, PipelineType pipType, bool mainCamera);
		void destroyWater(Entity entity, WaterComponent& water);
		// the RGBA32F copy of the simulation field the vertex shader fetches, made, resized
		// or freed as the component switches between a level plane and a depth per cell
		bool updateWaterField(WaterComponent& water);
		static bool isBehindWater(const std::vector<WaterRenderData>& waters, const AABB& box);
		WaterComponent* findUnderwater(const Vector3& eye);
		bool loadUnderwater(WaterComponent& water);
		void updateWaterExclusions();
		void setWaterExclusionParams(const WaterComponent& water, const Vector3& eye, fs_water_t& params);
		// the fullscreen fade of the scene copy, in the pass already started
		void drawUnderwater(WaterComponent& water, CameraComponent& camera, Transform& cameraTransform);

		void destroyLight(LightComponent& light);
		void destroyCamera(CameraComponent& camera, bool entityDestroyed);
		
		void updateSkyViewProjection(SkyComponent& sky, CameraComponent& camera);
		void updateLightFromScene(LightComponent& light, Transform& transform, CameraComponent& camera, Transform& cameraTransform);
		void updatePoints(PointsComponent& points, Transform& transform, CameraComponent& camera, Transform& camTransform);
		void updateTerrain(TerrainComponent& terrain, Transform& transform, CameraComponent& camera, Transform& cameraTransform, int viewIndex);
		void updateCameraFrustumPlanes(const Matrix4 viewProjectionMatrix, Plane* frustumPlanes);
		void updateInstancedMesh(InstancedMeshComponent& instmesh, MeshComponent& mesh, Transform& transform, CameraComponent& camera, Transform& camTransform);

		void sortPoints(PointsComponent& points, Transform& transform, CameraComponent& camera, Transform& camTransform);
		void sortInstancedMesh(InstancedMeshComponent& instmesh, MeshComponent& mesh, Transform& transform, CameraComponent& camera, Transform& camTransform);

	public:

		RenderSystem(Scene* scene);
		virtual ~RenderSystem();

		// Editor-only viewport debug override (see member declaration). Toggling it
		// reloads every mesh, since cull mode is baked into the pipeline at load time.
		void setDisableFaceCulling(bool disableFaceCulling);

		void setDisableFog(bool disableFog);

		void setDisableUnderwater(bool disableUnderwater);

		void setShowClusteredLightGrid(bool showClusteredLightGrid);
		bool isClusteredLightGridReady() const { return clusteredLightGridReady; }
		unsigned int getClusteredLightGridWidth() const { return clusteredLightGridWidth; }
		unsigned int getClusteredLightGridHeight() const { return clusteredLightGridHeight; }
		unsigned int getClusteredLightGridDepthSlices() const { return clusteredLightGridDepthSlices; }
		unsigned int getClusteredLightCount() const { return static_cast<unsigned int>(activeLights.size()); }
		bool isShadowAtlasReady() const { return hasShadowAtlas; }
		int getShadowAtlasUsedSlots() const { return shadowAtlasUsedSlots; }
		unsigned int getShadowAtlasSlotResolution() const { return shadowAtlasSlotResolution; }
		bool isShadowAtlasContentDirty() const { return shadowAtlasContentDirty; }

		// Editor-only override (see member declaration). No reload needed: the
		// editor always renders through Engine::getFramebuffer(), so PIP_RTT is
		// already baked either way.
		void setDisableFixedResolution(bool disableFixedResolution);

		void setWaterPaused(bool waterPaused);
		// moves the waves by the update time since the last call; draw() does it after drawing
		void advanceWaters();

		// the depth pass alpha-tests the submesh (MASK, or the legacy AUTO + textureShadow);
		// the editor derives export keys from it, so the rule lives in one place
		static bool usesAlphaMask(const Material& material, bool textureShadow);

		// water surface offset over the world point (x, z) as drawn, for Water::getHeight and buoyancy.
		// depth, when given, is the metres of water standing there: 0 where a simulating field is
		// dry, which is what keeps a body from floating over ground the water has not reached
		static Vector3 getWaterSurfaceOffset(const WaterComponent& water, float x, float z, Vector3* normal = nullptr, float* depth = nullptr);

		static bool isInsideWaterExclusion(const WaterExclusionComponent& exclusion, const Transform& transform, const Vector3& point);

		// near/far as rendered, perspective keeps 0 < near < far
		static Vector2 getCameraNearFar(const CameraComponent& camera);

		// the same pair with the scene render distance applied to the far: what the projection
		// and the frustum planes are actually built from. A 0 distance, an authored farClip of 0
		// ("no far plane") or a 2D/UI camera all mean "no cap".
		static Vector2 getRenderedNearFar(const CameraComponent& camera, float renderDistance);

		// copies the stacked scene composite to the swapchain (Engine::endCompositeFramebuffer)
		void presentFramebufferToSwapchain(Framebuffer* source);

		// pipelines this scene's cameras need; objects reload when it changes
		uint16_t getScenePipelines() const;

		bool loadMesh(Entity entity, MeshComponent& mesh, uint16_t pipelines, InstancedMeshComponent* instmesh, TerrainComponent* terrain);
		bool loadPoints(Entity entity, PointsComponent& points, uint16_t pipelines);
		bool loadLines(Entity entity, LinesComponent& lines, uint16_t pipelines);
		bool loadUI(Entity entity, UIComponent& ui, uint16_t pipelines, bool isText);
		bool loadSky(Entity entity, SkyComponent& sky, uint16_t pipelines);
		bool loadWater(Entity entity, WaterComponent& water, uint16_t pipelines);

		void updateFramebuffer(CameraComponent& camera);
		void updateTransform(Transform& transform, Entity entity);
		void updateCamera(CameraComponent& camera, Transform& transform);
		void updateMirrors(Entity mainCameraEntity);
		Entity createMirrorCamera(Entity mirrorEntity);
		Entity getMirrorCamera(Entity mirrorEntity) const;
		void destroyMirrorCamera(Entity entity);

		// camera
		void updateCameraSize(Entity entity);
		bool isInsideCamera(const float cameraFar, const Plane frustumPlanes[6], const AABB& box);
		bool isInsideCamera(CameraComponent& camera, const AABB& box);
		bool isInsideCamera(CameraComponent& camera, const Vector3& point);
		bool isInsideCamera(CameraComponent& camera, const Vector3& center, const float& radius);
		bool isInsideCamera(const float cameraFar, const Plane frustumPlanes[6], const Vector3& center, const float& radius);

		void needReloadPoints();
		void needReloadLines();
		void needReloadMeshes();
		void needReloadPostProcess();
		void needUpdatePostProcessUniforms();
		// components inheriting the default shader of this type rewrite their custom blocks
		void needUpdateDefaultShaderUniforms(ShaderType type);
		void needReloadUIs();
		void needReloadSky();
		void needReloadWater();
		void prepareMeshForDataReload(Entity entity, MeshComponent& mesh);

		bool isAllLoaded() const;
		void getLoadCount(size_t& loaded, size_t& total) const;
		bool hasPendingMeshLods() const;
	
		void load() override;
		void draw() override;
		void destroy() override;
		void update(double dt) override;

		void onComponentAdded(Entity entity, ComponentId componentId) override;
		void onComponentRemoved(Entity entity, ComponentId componentId) override;
	};

}

#endif //RENDERSYSTEM_H
