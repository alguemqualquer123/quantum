// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "engine/core/Scene.h"
#include "engine/core/io/SaveGame.h"
#include "engine/core/subsystem/NetworkSystem.h"
#include "engine/core/component/ParticlesComponent.h"
#include "engine/core/component/LightComponent.h"
#include "engine/core/component/Body3DComponent.h"
#include "engine/core/component/AutoColliderComponent.h"
#include "engine/core/component/ScriptComponent.h"
#include "engine/core/component/ModelComponent.h"
#include "engine/core/component/AnimationComponent.h"
#include "engine/core/component/ActionComponent.h"
#include "engine/core/component/PositionActionComponent.h"
#include "engine/core/component/RotationActionComponent.h"
#include "engine/core/component/ScaleActionComponent.h"
#include "engine/core/component/ColorActionComponent.h"
#include "engine/core/component/AlphaActionComponent.h"
#include "engine/core/component/TimedActionComponent.h"
#include "engine/core/component/BundleComponent.h"
#include "engine/core/component/InstancedMeshComponent.h"
#include "engine/core/component/TerrainComponent.h"
#include "engine/core/component/SpriteComponent.h"
#include "engine/core/component/SpriteAnimationComponent.h"
#include "engine/core/component/MeshPolygonComponent.h"
#include "engine/core/component/PolygonComponent.h"
#include "engine/core/component/PointsComponent.h"
#include "engine/core/component/LinesComponent.h"
#include "engine/core/component/TilemapComponent.h"
#include "engine/core/component/AnimatorControllerComponent.h"
#include "engine/core/component/MeshComponent.h"
#include "engine/core/component/Joint2DComponent.h"
#include "engine/core/component/Joint3DComponent.h"
#include "engine/core/component/BoneComponent.h"
#include "engine/core/component/MorphTracksComponent.h"
#include "engine/core/component/KeyframeTracksComponent.h"
#include "engine/core/component/ScaleTracksComponent.h"
#include "engine/core/component/TranslateTracksComponent.h"
#include "engine/core/component/RotateTracksComponent.h"
#include "engine/core/component/Body2DComponent.h"
#include "engine/core/component/CameraComponent.h"
#include "engine/core/component/FogComponent.h"
#include "engine/core/component/ReflectionProbeComponent.h"
#include "engine/core/component/WaterComponent.h"
#include "engine/core/component/DecalComponent.h"
#include "engine/core/component/Light2DComponent.h"
#include "engine/core/component/SkyComponent.h"
#include "engine/core/component/MirrorComponent.h"
#include "engine/core/component/Occluder2DComponent.h"
#include "engine/core/component/WaterExclusionComponent.h"
#include "engine/core/component/SoundComponent.h"
#include "engine/core/ecs/components/NetworkComponent.h"
#include "engine/core/ecs/components/TagComponent.h"

#include <cmath>
#include <iostream>
#include <set>

int main() {
    const std::string slot = "engine-completion-serialization-smoke";
    quantum::Scene scene;
    const quantum::Entity entity = scene.createEntity();
    scene.addComponent<quantum::Transform>(entity);
    scene.addComponent<quantum::NetworkComponent>(entity);
    scene.addComponent<quantum::ScriptComponent>(entity);
    scene.addComponent<quantum::ModelComponent>(entity);
    scene.addComponent<quantum::AnimationComponent>(entity);
    scene.addComponent<quantum::ActionComponent>(entity);
    scene.addComponent<quantum::PositionActionComponent>(entity);
    scene.addComponent<quantum::RotationActionComponent>(entity);
    scene.addComponent<quantum::ScaleActionComponent>(entity);
    scene.addComponent<quantum::ColorActionComponent>(entity);
    scene.addComponent<quantum::AlphaActionComponent>(entity);
    scene.addComponent<quantum::TimedActionComponent>(entity);
    scene.addComponent<quantum::BundleComponent>(entity);
    scene.addComponent<quantum::InstancedMeshComponent>(entity);
    scene.addComponent<quantum::TerrainComponent>(entity);
    scene.addComponent<quantum::SpriteComponent>(entity);
    scene.addComponent<quantum::SpriteAnimationComponent>(entity);
    scene.addComponent<quantum::MeshPolygonComponent>(entity);
    scene.addComponent<quantum::PolygonComponent>(entity);
    scene.addComponent<quantum::PointsComponent>(entity);
    scene.addComponent<quantum::LinesComponent>(entity);
    scene.addComponent<quantum::TilemapComponent>(entity);
    scene.addComponent<quantum::AnimatorControllerComponent>(entity);
    scene.addComponent<quantum::MeshComponent>(entity);
    scene.addComponent<quantum::Joint2DComponent>(entity);
    scene.addComponent<quantum::Joint3DComponent>(entity);
    scene.addComponent<quantum::BoneComponent>(entity);
    scene.addComponent<quantum::MorphTracksComponent>(entity);
    scene.addComponent<quantum::KeyframeTracksComponent>(entity);
    scene.addComponent<quantum::ScaleTracksComponent>(entity);
    scene.addComponent<quantum::TranslateTracksComponent>(entity);
    scene.addComponent<quantum::RotateTracksComponent>(entity);
    scene.addComponent<quantum::TagComponent>(entity);
    scene.addComponent<quantum::SoundComponent>(entity);
    scene.addComponent<quantum::ParticlesComponent>(entity);
    scene.addComponent<quantum::LightComponent>(entity);
    scene.addComponent<quantum::Body3DComponent>(entity);
    scene.addComponent<quantum::AutoColliderComponent>(entity);
    scene.addComponent<quantum::Body2DComponent>(entity);
    scene.addComponent<quantum::CameraComponent>(entity);
    scene.addComponent<quantum::FogComponent>(entity);
    scene.addComponent<quantum::ReflectionProbeComponent>(entity);
    scene.addComponent<quantum::WaterComponent>(entity);
    scene.addComponent<quantum::DecalComponent>(entity);
    scene.addComponent<quantum::Light2DComponent>(entity);
    scene.addComponent<quantum::SkyComponent>(entity);
    scene.addComponent<quantum::MirrorComponent>(entity);
    scene.addComponent<quantum::Occluder2DComponent>(entity);
    scene.addComponent<quantum::WaterExclusionComponent>(entity);
    scene.setEntityName(entity, "serialization-probe");

    auto& transform = scene.getComponent<quantum::Transform>(entity);
    transform.position = quantum::Vector3(7.0f, 8.0f, 9.0f);
    auto& networkComponent = scene.getComponent<quantum::NetworkComponent>(entity);
    networkComponent.replicateTransform = true;
    networkComponent.netId = 4242;
    networkComponent.ownerId = 17;
    networkComponent.interestGroup = 9;
    networkComponent.interestRadius = 125.5f;
    networkComponent.serverAuthority = false;
    auto& scripts = scene.getComponent<quantum::ScriptComponent>(entity);
    quantum::ScriptEntry luaScript;
    luaScript.type = quantum::ScriptType::LUA;
    luaScript.path = "scripts/player.lua";
    luaScript.className = "PlayerController";
    luaScript.enabled = true;
    quantum::ScriptProperty speedProperty;
    speedProperty.name = "speed";
    speedProperty.displayName = "Move Speed";
    speedProperty.type = quantum::ScriptPropertyType::Float;
    speedProperty.value = 3.5f;
    speedProperty.defaultValue = 1.0f;
    speedProperty.ptrTypeName = "float";
    luaScript.properties.push_back(speedProperty);
    quantum::ScriptProperty spawnProperty;
    spawnProperty.name = "spawn";
    spawnProperty.displayName = "Spawn Point";
    spawnProperty.type = quantum::ScriptPropertyType::Vector3;
    spawnProperty.value = quantum::Vector3(4.0f, 5.0f, 6.0f);
    spawnProperty.defaultValue = quantum::Vector3::ZERO;
    luaScript.properties.push_back(spawnProperty);
    scripts.scripts.push_back(luaScript);
    auto& model = scene.getComponent<quantum::ModelComponent>(entity);
    model.filename = "models/hero.glb";
    model.mergeStaticMeshes = true;
    auto& animation = scene.getComponent<quantum::AnimationComponent>(entity);
    animation.actions.push_back({1.25f, 2.5f, 77, 3, true});
    animation.ownedActions = true;
    animation.loop = true;
    animation.duration = 8.0f;
    animation.defaultFadeTime = 0.35f;
    auto& action = scene.getComponent<quantum::ActionComponent>(entity);
    action.state = quantum::ActionState::Running;
    action.timecount = 1.5f;
    action.startTrigger = true;
    action.ownedTarget = true;
    action.target = 77;
    action.speed = 1.25f;
    action.startOffset = 0.2f;
    action.randomStart = true;
    action.phase = 0.4f;
    action.blendLayer = 2;
    action.blendLayerWeight = 0.65f;
    action.blendLayerAdditive = true;
    auto& positionAction = scene.getComponent<quantum::PositionActionComponent>(entity);
    positionAction.startPosition = quantum::Vector3(-1.0f, 0.0f, 2.0f);
    positionAction.endPosition = quantum::Vector3(4.0f, 5.0f, 6.0f);
    auto& rotationAction = scene.getComponent<quantum::RotationActionComponent>(entity);
    rotationAction.startRotation = quantum::Quaternion(1.0f, 0.0f, 0.0f, 0.0f);
    rotationAction.endRotation = quantum::Quaternion(0.9239f, 0.0f, 0.3827f, 0.0f);
    rotationAction.shortestPath = true;
    rotationAction.spin = true;
    rotationAction.axis = quantum::Vector3(0.0f, 1.0f, 0.0f);
    rotationAction.angle = 720.0f;
    auto& scaleAction = scene.getComponent<quantum::ScaleActionComponent>(entity);
    scaleAction.startScale = quantum::Vector3(0.5f, 0.75f, 1.0f);
    scaleAction.endScale = quantum::Vector3(2.0f, 3.0f, 4.0f);
    auto& colorAction = scene.getComponent<quantum::ColorActionComponent>(entity);
    colorAction.startColor = quantum::Vector3(0.1f, 0.2f, 0.3f);
    colorAction.endColor = quantum::Vector3(0.8f, 0.7f, 0.6f);
    colorAction.useSRGB = false;
    auto& alphaAction = scene.getComponent<quantum::AlphaActionComponent>(entity);
    alphaAction.startAlpha = 0.15f;
    alphaAction.endAlpha = 0.85f;
    auto& timedAction = scene.getComponent<quantum::TimedActionComponent>(entity);
    timedAction.time = 0.75f;
    timedAction.value = 0.25f;
    timedAction.duration = 3.5f;
    timedAction.loop = true;
    timedAction.pingPong = true;
    timedAction.function.setType(quantum::EaseType::CUBIC_IN_OUT);
    auto& bundle = scene.getComponent<quantum::BundleComponent>(entity);
    bundle.name = "hero_prefab";
    bundle.path = "bundles/characters/hero.yaml";
    auto& instanced = scene.getComponent<quantum::InstancedMeshComponent>(entity);
    quantum::InstanceData instance;
    instance.position = quantum::Vector3(2.0f, 3.0f, 4.0f);
    instance.rotation = quantum::Quaternion(1.0f, 0.0f, 0.0f, 0.0f);
    instance.scale = quantum::Vector3(1.5f, 2.0f, 0.75f);
    instance.color = quantum::Vector4(0.7f, 0.8f, 0.9f, 1.0f);
    instance.textureRect.setRect(0.1f, 0.2f, 0.3f, 0.4f);
    instance.visible = false;
    instanced.instances.push_back(instance);
    instanced.maxInstances = 512;
    instanced.cullInstances = false;
    instanced.cullDistance = 120.0f;
    instanced.distanceFade = true;
    instanced.fadeStart = 80.0f;
    instanced.fadeEnd = 110.0f;
    instanced.cullNearDistance = 2.0f;
    instanced.instancedBillboard = true;
    instanced.instancedCylindricalBillboard = true;
    instanced.viewSettings.cull = false;
    instanced.viewSettings.castShadows = false;
    instanced.viewSettings.lodEnabled = false;
    instanced.viewSettings.lodBias = 1.5f;
    auto& terrain = scene.getComponent<quantum::TerrainComponent>(entity);
    terrain.heightMap.setPath("terrain/island_height.png");
    terrain.blendMaps.emplace_back();
    terrain.blendMaps[0].setPath("terrain/island_blend.png");
    quantum::TerrainSurfaceLayer terrainLayer;
    terrainLayer.pbr = true;
    terrainLayer.colorTexture.setPath("terrain/grass_albedo.png");
    terrainLayer.normalTexture.setPath("terrain/grass_normal.png");
    terrainLayer.roughnessTexture.setPath("terrain/grass_roughness.png");
    terrainLayer.metallicTexture.setPath("terrain/grass_metallic.png");
    terrainLayer.occlusionTexture.setPath("terrain/grass_occlusion.png");
    terrainLayer.heightTexture.setPath("terrain/grass_height.png");
    terrainLayer.colorFactor = quantum::Vector4(0.8f, 0.9f, 0.7f, 1.0f);
    terrainLayer.normalStrength = 0.75f;
    terrainLayer.roughnessFactor = 0.65f;
    terrainLayer.metallicFactor = 0.1f;
    terrainLayer.occlusionStrength = 0.9f;
    terrainLayer.uvScale = quantum::Vector2(2.0f, 3.0f);
    terrainLayer.uvOffset = quantum::Vector2(0.25f, -0.5f);
    terrain.surfaceLayers.push_back(terrainLayer);
    quantum::TerrainFoliageLayer foliage;
    foliage.meshPath = "models/grass.glb";
    foliage.densityMap.setPath("terrain/grass_density.png");
    foliage.density = 0.35f;
    foliage.minScale = 0.7f;
    foliage.maxScale = 1.4f;
    foliage.rotationJitter = 0.8f;
    foliage.alignToNormal = 0.2f;
    foliage.minSlope = 4.0f;
    foliage.maxSlope = 28.0f;
    foliage.minHeight = 0.1f;
    foliage.maxHeight = 0.85f;
    foliage.drawDistance = 75.0f;
    foliage.minDrawDistance = 3.0f;
    foliage.seed = 1234;
    foliage.castShadows = false;
    terrain.foliageLayers.push_back(foliage);
    terrain.autoSetRanges = false;
    terrain.offset = quantum::Vector2(5.0f, -2.0f);
    terrain.terrainSize = 640.0f;
    terrain.maxHeight = 48.0f;
    terrain.resolution = 128.0f;
    terrain.textureBaseTiles = 4.0f;
    terrain.textureDetailTiles = 32.0f;
    terrain.rootGridSize = 4;
    terrain.levels = 8;
    auto& sprite = scene.getComponent<quantum::SpriteComponent>(entity);
    sprite.width = 256;
    sprite.height = 128;
    sprite.automaticFlipY = false;
    sprite.flipY = true;
    sprite.textureScaleFactor = 0.5f;
    sprite.numFramesRect = 2;
    sprite.framesRect[0] = {"idle", quantum::Rect(0.0f, 0.0f, 0.25f, 0.5f)};
    sprite.framesRect[1] = {"run", quantum::Rect(0.25f, 0.0f, 0.25f, 0.5f)};
    sprite.pivotPreset = quantum::PivotPreset::TOP_CENTER;
    sprite.pendingFrameRect = quantum::Rect(0.1f, 0.2f, 0.3f, 0.4f);
    sprite.needUpdateFrameRect = true;
    auto& spriteAnimation = scene.getComponent<quantum::SpriteAnimationComponent>(entity);
    spriteAnimation.loop = false;
    spriteAnimation.framesTimeSize = 2;
    spriteAnimation.framesTime[0] = 120;
    spriteAnimation.framesTime[1] = 240;
    spriteAnimation.framesSize = 2;
    spriteAnimation.frames[0] = 0;
    spriteAnimation.frames[1] = 1;
    spriteAnimation.frameIndex = 1;
    spriteAnimation.frameTimeIndex = 1;
    auto& meshPolygon = scene.getComponent<quantum::MeshPolygonComponent>(entity);
    meshPolygon.width = 320;
    meshPolygon.height = 180;
    meshPolygon.automaticFlipY = false;
    meshPolygon.flipY = true;
    meshPolygon.points = {
        {{-1.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f, 1.0f}},
        {{0.0f, 2.0f, 0.0f}, {0.0f, 1.0f, 0.0f, 1.0f}},
        {{1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f, 1.0f}}
    };
    auto& polygon = scene.getComponent<quantum::PolygonComponent>(entity);
    polygon.points = {
        {{-2.0f, 0.0f, 0.0f}, {1.0f, 0.25f, 0.1f, 1.0f}},
        {{0.0f, 3.0f, 0.0f}, {0.2f, 0.8f, 0.35f, 0.9f}},
        {{2.0f, 0.0f, 0.0f}, {0.15f, 0.3f, 1.0f, 0.75f}}
    };
    polygon.needUpdatePolygon = false;
    auto& points = scene.getComponent<quantum::PointsComponent>(entity);
    points.points = {
        {{-1.0f, 0.5f, 0.0f}, {1.0f, 0.5f, 0.25f, 1.0f}, 0.75f, 0.2f,
            quantum::Rect(0.0f, 0.0f, 0.5f, 0.5f), true},
        {{1.0f, -0.5f, 0.0f}, {0.25f, 0.75f, 1.0f, 0.8f}, 1.5f, -0.4f,
            quantum::Rect(0.5f, 0.5f, 0.5f, 0.5f), false}
    };
    points.numFramesRect = 1;
    points.framesRect[0] = {"spark", quantum::Rect(0.0f, 0.0f, 0.5f, 0.5f)};
    points.maxPoints = 512;
    points.customShader = "shaders/points_custom";
    points.texture.setPath("textures/particles/point.png");
    points.transparent = true;
    points.autoTransparency = false;
    auto& lines = scene.getComponent<quantum::LinesComponent>(entity);
    lines.lines = {
        {{-2.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f, 1.0f},
            {2.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 0.0f, 0.75f}},
        {{0.0f, -1.0f, 0.0f}, {0.0f, 0.0f, 1.0f, 1.0f},
            {0.0f, 2.0f, 0.0f}, {1.0f, 1.0f, 0.0f, 0.5f}}
    };
    lines.maxLines = 256;
    lines.customShader = "shaders/lines_custom";
    auto& tilemap = scene.getComponent<quantum::TilemapComponent>(entity);
    tilemap.width = 32;
    tilemap.height = 16;
    tilemap.automaticFlipY = false;
    tilemap.flipY = true;
    tilemap.textureScaleFactor = 0.75f;
    tilemap.reserveTiles = 8;
    tilemap.numTilesRect = 2;
    tilemap.tilesRect[0] = {"grass", 0, quantum::Rect(0.0f, 0.0f, 0.25f, 0.25f)};
    tilemap.tilesRect[1] = {"water", -1, quantum::Rect(0.25f, 0.0f, 0.25f, 0.25f)};
    tilemap.numTiles = 2;
    tilemap.tiles[0] = {"grass_tile", 0, quantum::Vector2(2.0f, 3.0f), 1.0f, 1.0f};
    tilemap.tiles[1] = {"water_tile", 1, quantum::Vector2(4.0f, 5.0f), 2.0f, 1.5f};
    auto& animator = scene.getComponent<quantum::AnimatorControllerComponent>(entity);
    animator.controllerAsset = "animations/player.animcontroller";
    animator.parameters.push_back({"speed", quantum::AnimatorParameter::Type::Float, 1.0f, false});
    animator.layers.push_back({"Base", 0.85f, false, true});
    animator.ikChains.push_back({entity, entity, entity, entity, NULL_ENTITY, 0.7f, true, true});
    animator.rootMotionEnabled = true;
    animator.rootMotionTarget = entity;
    animator.rootMotionScale = 1.25f;
    quantum::AnimatorState idle;
    idle.id = 10;
    idle.name = "Idle";
    idle.isDefault = true;
    idle.x = 120.0f;
    idle.y = 80.0f;
    quantum::AnimatorClip idleClip;
    idleClip.name = "idle_clip";
    idleClip.animation = entity;
    idleClip.audio = entity;
    idleClip.speed = 0.9f;
    idleClip.offset = 0.1f;
    idleClip.loop = true;
    idleClip.layer = 0;
    idleClip.events.push_back({0.5f, "OnFootstep", "left"});
    idle.clips.push_back(idleClip);
    animator.states.push_back(idle);
    quantum::AnimatorTransition transition;
    transition.id = 20;
    transition.name = "IdleToRun";
    transition.fromState = 10;
    transition.toState = 11;
    transition.hasExitTime = true;
    transition.exitTime = 0.8f;
    transition.transitionDuration = 0.2f;
    transition.offset = 0.0f;
    transition.conditions.push_back({"speed", quantum::AnimatorTransition::Condition::Mode::Greater, 0.5f, false});
    animator.transitions.push_back(transition);
    auto& mesh = scene.getComponent<quantum::MeshComponent>(entity);
    mesh.customShader = "shaders/mesh_custom";
    mesh.customDepthShader = "shaders/mesh_depth_custom";
    mesh.receiveLights = false;
    mesh.receiveIBL = true;
    mesh.castShadows = false;
    mesh.transparent = true;
    mesh.lodEnabled = false;
    mesh.lodBias = 1.4f;
    mesh.cullingMode = quantum::CullingMode::FRONT;
    mesh.windingOrder = quantum::WindingOrder::CW;
    mesh.numSubmeshes = 1;
    mesh.submeshes[0].overrideFields = quantum::SubmeshOverride_BaseColorFactor
        | quantum::SubmeshOverride_BaseColorTexture | quantum::SubmeshOverride_AlphaMode;
    mesh.submeshes[0].material.baseColorFactor = quantum::Vector4(0.7f, 0.2f, 0.1f, 1.0f);
    mesh.submeshes[0].material.baseColorTexture.setPath("textures/materials/hero_albedo.png");
    mesh.submeshes[0].material.alphaMode = quantum::MaterialAlphaMode::BLEND;
    mesh.submeshes[0].faceCulling = false;
    mesh.submeshes[0].textureShadow = true;
    mesh.submeshes[0].primitiveType = quantum::PrimitiveType::TRIANGLE_STRIP;
    auto& joint2D = scene.getComponent<quantum::Joint2DComponent>(entity);
    joint2D.type = quantum::Joint2DType::WHEEL;
    joint2D.bodyA = entity;
    joint2D.bodyB = entity;
    joint2D.anchorA = quantum::Vector2(1.0f, 2.0f);
    joint2D.anchorB = quantum::Vector2(-1.0f, 0.5f);
    joint2D.axis = quantum::Vector2(0.0f, 1.0f);
    joint2D.target = quantum::Vector2(3.0f, 4.0f);
    joint2D.autoAnchors = false;
    joint2D.rope = true;
    auto& joint3D = scene.getComponent<quantum::Joint3DComponent>(entity);
    joint3D.type = quantum::Joint3DType::SIXDOF;
    joint3D.bodyA = entity;
    joint3D.bodyB = entity;
    joint3D.anchorA = quantum::Vector3(1.0f, 2.0f, 3.0f);
    joint3D.anchorB = quantum::Vector3(-1.0f, 0.5f, 2.0f);
    joint3D.axis = quantum::Vector3(0.0f, 1.0f, 0.0f);
    joint3D.limitsMin = -2.0f;
    joint3D.limitsMax = 5.0f;
    joint3D.numTeethGearA = 32;
    joint3D.pathPoints = {quantum::Vector3(0.0f, 0.0f, 0.0f), quantum::Vector3(2.0f, 1.0f, 0.0f)};
    joint3D.pathPosition = quantum::Vector3(0.25f, 0.5f, 0.75f);
    joint3D.isLooping = true;
    joint3D.autoAnchors = false;
    auto& bone = scene.getComponent<quantum::BoneComponent>(entity);
    bone.model = entity;
    bone.index = 3;
    bone.bindPosition = quantum::Vector3(1.0f, 2.0f, 3.0f);
    bone.bindRotation = quantum::Quaternion(0.9f, 0.1f, 0.2f, 0.3f);
    bone.bindScale = quantum::Vector3(1.2f, 0.8f, 1.1f);
    bone.offsetMatrix = quantum::Matrix4(1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f,
        4.0f, 5.0f, 6.0f, 1.0f);
    auto& morphTracks = scene.getComponent<quantum::MorphTracksComponent>(entity);
    morphTracks.values = {{0.0f, 0.25f, 1.0f}, {1.0f, 0.5f}};
    morphTracks.inTangents = {{0.0f, 0.1f, 0.2f}, {0.3f, 0.4f}};
    morphTracks.outTangents = {{0.05f, 0.15f, 0.25f}, {0.35f, 0.45f}};
    auto& keyframes = scene.getComponent<quantum::KeyframeTracksComponent>(entity);
    keyframes.times = {0.0f, 0.5f, 1.0f};
    keyframes.easings = {quantum::EaseType::LINEAR, quantum::EaseType::CUBIC_IN_OUT};
    keyframes.loop = true;
    keyframes.relative = true;
    keyframes.index = 2;
    keyframes.interpolation = 0.75f;
    keyframes.hasBase = true;
    keyframes.basePosition = quantum::Vector3(2.0f, 3.0f, 4.0f);
    keyframes.baseRotation = quantum::Quaternion(0.8f, 0.1f, 0.2f, 0.3f);
    keyframes.baseScale = quantum::Vector3(1.1f, 1.2f, 1.3f);
    auto& scaleTracks = scene.getComponent<quantum::ScaleTracksComponent>(entity);
    scaleTracks.values = {quantum::Vector3(1.0f, 1.0f, 1.0f), quantum::Vector3(1.2f, 1.1f, 0.9f)};
    scaleTracks.inTangents = {quantum::Vector3(0.0f, 0.1f, 0.0f)};
    scaleTracks.outTangents = {quantum::Vector3(0.1f, 0.0f, 0.1f)};
    auto& translateTracks = scene.getComponent<quantum::TranslateTracksComponent>(entity);
    translateTracks.values = {quantum::Vector3(0.0f, 0.0f, 0.0f), quantum::Vector3(2.0f, 3.0f, 4.0f)};
    translateTracks.inTangents = {quantum::Vector3(0.1f, 0.2f, 0.3f)};
    translateTracks.outTangents = {quantum::Vector3(0.3f, 0.2f, 0.1f)};
    auto& rotateTracks = scene.getComponent<quantum::RotateTracksComponent>(entity);
    rotateTracks.values = {quantum::Quaternion(1.0f, 0.0f, 0.0f, 0.0f), quantum::Quaternion(0.8f, 0.1f, 0.2f, 0.3f)};
    rotateTracks.inTangents = {quantum::Quaternion(0.9f, 0.1f, 0.0f, 0.0f)};
    rotateTracks.outTangents = {quantum::Quaternion(0.85f, 0.0f, 0.1f, 0.0f)};
    auto& tag = scene.getComponent<quantum::TagComponent>(entity);
    tag.categories = quantum::TagComponent::PLAYER | quantum::TagComponent::DAMAGABLE;
    tag.tag = "serializer-probe";
    tag.layer = 4;
    auto& sound = scene.getComponent<quantum::SoundComponent>(entity);
    sound.filename = "audio/probe.wav";
    sound.bus = "effects";
    sound.volume = 0.55;
    sound.looping = true;
    sound.occlusionEnabled = true;
    sound.occlusionVolume = 0.2f;
    auto& particles = scene.getComponent<quantum::ParticlesComponent>(entity);
    particles.maxParticles = 320;
    particles.emitter = true;
    particles.loop = false;
    particles.localSpace = true;
    particles.collisionEnabled = true;
    particles.collisionBounce = 0.7f;
    particles.collisionFriction = 0.15f;
    particles.trailsEnabled = true;
    particles.trailLength = 10;
    particles.rate = 42;
    particles.maxPerUpdate = 21;
    auto& light = scene.getComponent<quantum::LightComponent>(entity);
    light.type = quantum::LightType::SPOT;
    light.direction = quantum::Vector3(0.0f, -1.0f, 0.25f);
    light.color = quantum::Vector3(0.8f, 0.7f, 0.5f);
    light.range = 24.0f;
    light.intensity = 3.5f;
    light.shadows = true;
    light.mapResolution = 1024;
    light.numShadowCascades = 4;
    auto& body = scene.getComponent<quantum::Body3DComponent>(entity);
    body.type = quantum::BodyType::DYNAMIC;
    body.motionQuality = quantum::Body3DMotionQuality::LINEAR_CAST;
    body.gravityFactor = 0.75f;
    body.linearDamping = 0.37f;
    body.angularDamping = 0.21f;
    body.buoyancy = 1.25f;
    body.waterDrag = 0.8f;
    body.waterAngularDrag = 0.04f;
    body.categoryBits = 4;
    body.maskBits = 0x00F0;
    body.sensor = true;
    body.followingTransform = true;
    body.numShapes = 1;
    auto& shape = body.shapes[0];
    shape.type = quantum::Shape3DType::BOX;
    shape.source = quantum::Shape3DSource::NONE;
    shape.position = quantum::Vector3(0.0f, 0.5f, 0.0f);
    shape.rotation = quantum::Quaternion(1.0f, 0.0f, 0.0f, 0.0f);
    shape.width = 2.0f;
    shape.height = 3.0f;
    shape.depth = 4.0f;
    shape.density = 700.0f;
    shape.friction = 0.45f;
    shape.restitution = 0.2f;
    auto& autoCollider = scene.getComponent<quantum::AutoColliderComponent>(entity);
    autoCollider.type = quantum::AutoColliderType::CONVEX_HULL;
    autoCollider.radius = 0.8f;
    autoCollider.width = 2.0f;
    autoCollider.height = 3.0f;
    autoCollider.depth = 4.0f;
    autoCollider.halfHeight = 1.5f;
    autoCollider.center = quantum::Vector3(0.25f, 0.5f, -0.75f);
    autoCollider.sensor = true;
    autoCollider.density = 650.0f;
    autoCollider.categoryBits = 16;
    autoCollider.maskBits = 0x00AA;
    autoCollider.sourceEntity = 42;
    autoCollider.needUpdate = false;
    auto& body2D = scene.getComponent<quantum::Body2DComponent>(entity);
    body2D.type = quantum::BodyType::KINEMATIC;
    body2D.followingTransform = true;
    body2D.loadedScale = quantum::Vector2(1.5f, 0.75f);
    body2D.numShapes = 1;
    auto& shape2D = body2D.shapes[0];
    shape2D.type = quantum::Shape2DType::POLYGON;
    shape2D.numVertices = 4;
    shape2D.vertices[0] = quantum::Vector2(-1.0f, -1.0f);
    shape2D.vertices[1] = quantum::Vector2(1.0f, -1.0f);
    shape2D.vertices[2] = quantum::Vector2(1.0f, 1.0f);
    shape2D.vertices[3] = quantum::Vector2(-1.0f, 1.0f);
    shape2D.density = 2.0f;
    shape2D.friction = 0.35f;
    shape2D.restitution = 0.15f;
    shape2D.sensor = true;
    shape2D.categoryBits = 8;
    shape2D.maskBits = 0x00CC;
    shape2D.groupIndex = -3;
    auto& camera = scene.getComponent<quantum::CameraComponent>(entity);
    camera.type = quantum::CameraType::CAMERA_PERSPECTIVE;
    camera.target = quantum::Vector3(2.0f, 3.0f, 4.0f);
    camera.up = quantum::Vector3(0.0f, 1.0f, 0.0f);
    camera.yfov = 1.1f;
    camera.aspect = 1.777f;
    camera.nearClip = 0.15f;
    camera.farClip = 1500.0f;
    camera.renderToTexture = true;
    camera.framebufferWidth = 1024;
    camera.framebufferHeight = 512;
    camera.framebufferFilter = quantum::TextureFilter::LINEAR_MIPMAP_LINEAR;
    camera.transparentSort = false;
    camera.depthTest = false;
    camera.useTarget = false;
    camera.autoResize = false;
    auto& fog = scene.getComponent<quantum::FogComponent>(entity);
    fog.type = quantum::FogType::EXPONENTIALSQUARED;
    fog.color = quantum::Vector3(0.2f, 0.3f, 0.4f);
    fog.density = 0.03f;
    fog.linearStart = 7.0f;
    fog.linearEnd = 240.0f;
    fog.heightFog = true;
    fog.fogHeightFalloff = 14.0f;
    fog.fogGroundHeight = -2.0f;
    fog.fogSunAmount = 0.9f;
    fog.fogNoiseEnabled = true;
    fog.fogNoiseScale = 0.012f;
    fog.fogNoiseWind = quantum::Vector2(2.0f, -0.5f);
    fog.godRays = true;
    fog.rayIntensity = 1.4f;
    fog.cloudsEnabled = true;
    fog.cloudBaseHeight = 180.0f;
    fog.cloudCoverage = 0.62f;
    fog.cloudWind = quantum::Vector2(-1.0f, 0.4f);
    fog.cloudColor = quantum::Vector3(0.7f, 0.8f, 0.95f);
    auto& probe = scene.getComponent<quantum::ReflectionProbeComponent>(entity);
    probe.mode = quantum::ReflectionProbeMode::DYNAMIC;
    probe.updateMode = quantum::ReflectionProbeUpdateMode::INTERVAL;
    probe.boxOffset = quantum::Vector3(1.0f, 2.0f, 3.0f);
    probe.boxSize = quantum::Vector3(20.0f, 12.0f, 8.0f);
    probe.blendDistance = 2.5f;
    probe.intensity = 1.4f;
    probe.priority = 7;
    probe.resolution = 256;
    probe.nearClip = 0.2f;
    probe.farClip = 600.0f;
    probe.updateInterval = 0.25f;
    probe.includeSky = false;
    auto& water = scene.getComponent<quantum::WaterComponent>(entity);
    water.customShader = "shaders/ocean_custom";
    water.customUnderwaterShader = "shaders/underwater_custom";
    water.size = quantum::Vector2(80.0f, 40.0f);
    water.subdivisions = 96;
    water.waveHeight = 0.55f;
    water.waveLength = 12.0f;
    water.waveSpeed = 2.5f;
    water.windInfluence = 0.8f;
    water.shallowColor = quantum::Vector3(0.1f, 0.5f, 0.6f);
    water.deepColor = quantum::Vector3(0.01f, 0.08f, 0.2f);
    water.planarReflection = true;
    water.refraction = true;
    water.simulate = true;
    water.simResolution = 64;
    water.simDepth = 0.45f;
    water.simFlow = 0.8f;
    water.simRain = 0.03f;
    water.simEvaporation = 0.01f;
    water.simEdgeDrain = true;
    water.simRipples = true;
    auto& decal = scene.getComponent<quantum::DecalComponent>(entity);
    decal.enabled = true;
    decal.color = quantum::Vector3(0.8f, 0.2f, 0.1f);
    decal.size = quantum::Vector3(2.0f, 3.0f, 0.25f);
    decal.opacity = 0.75f;
    decal.depthFade = 0.12f;
    decal.texture.setPath("textures/decals/scorch.png");
    auto& light2D = scene.getComponent<quantum::Light2DComponent>(entity);
    light2D.color = quantum::Vector3(1.0f, 0.35f, 0.1f);
    light2D.intensity = 2.5f;
    light2D.range = 320.0f;
    light2D.falloff = 1.8f;
    light2D.height = 4.0f;
    light2D.shadows = true;
    light2D.shadowBias = 0.02f;
    light2D.shadowSoftness = 3.0f;
    light2D.mapResolution = 1024;
    auto& sky = scene.getComponent<quantum::SkyComponent>(entity);
    sky.customShader = "shaders/sky_custom";
    sky.color = quantum::Vector4(0.4f, 0.6f, 0.95f, 1.0f);
    sky.rotation = 1.25f;
    sky.visible = false;
    sky.texture.setPath("textures/sky/stars.hdr");
    auto& mirror = scene.getComponent<quantum::MirrorComponent>(entity);
    mirror.normal = quantum::Vector3(0.0f, 0.7071f, 0.7071f);
    auto& occluder = scene.getComponent<quantum::Occluder2DComponent>(entity);
    occluder.shape = quantum::Occluder2DShape::POLYGON;
    occluder.closed = false;
    occluder.enabled = true;
    occluder.points = {quantum::Vector2(-2.0f, -1.0f), quantum::Vector2(2.0f, -1.0f),
        quantum::Vector2(1.0f, 2.0f)};
    auto& exclusion = scene.getComponent<quantum::WaterExclusionComponent>(entity);
    exclusion.shape = quantum::WaterExclusionShape::BOX;
    exclusion.center = quantum::Vector3(0.0f, 1.5f, -2.0f);
    exclusion.size = quantum::Vector3(4.0f, 3.0f, 2.0f);

    const auto network = scene.getSystem<quantum::NetworkSystem>();
    network->registerEntity(entity);
    quantum::SaveGame writer(slot);
    writer.deleteSlot();
    const bool saved = network->saveReplicatedState(writer, "ecs") && writer.save();
    const auto serializerCatalog = quantum::NetworkSystem::getReplicatedComponentCatalog();
    std::set<std::string> serializerNames;
    bool catalogValid = serializerCatalog.size() >= 48;
    for (const auto& descriptor : serializerCatalog) {
        catalogValid = catalogValid && !descriptor.name.empty()
            && descriptor.schemaVersion != 0
            && serializerNames.insert(descriptor.name).second;
    }
    const auto componentCatalog = scene.getRegisteredComponentCatalog();
    std::set<std::string> componentNames;
    std::set<quantum::ComponentId> componentIds;
    bool componentCatalogValid = componentCatalog.size() == scene.getRegisteredComponentCount();
    for (const auto& descriptor : componentCatalog) {
        componentCatalogValid = componentCatalogValid && !descriptor.name.empty()
            && componentNames.insert(descriptor.name).second
            && componentIds.insert(descriptor.id).second;
    }

    transform.position = quantum::Vector3::ZERO;
    networkComponent.replicateTransform = false;
    networkComponent.ownerId = 0;
    networkComponent.interestGroup = 0;
    networkComponent.interestRadius = 0.0f;
    networkComponent.serverAuthority = true;
    scripts.scripts.clear();
    model.filename.clear();
    model.mergeStaticMeshes = false;
    model.needUpdateModel = false;
    animation.actions.clear();
    animation.ownedActions = false;
    animation.loop = false;
    animation.duration = -1.0f;
    animation.defaultFadeTime = 0.15f;
    bundle.name.clear();
    bundle.path.clear();
    instanced.instances.clear();
    instanced.maxInstances = 100;
    instanced.cullInstances = true;
    instanced.cullDistance = 0.0f;
    instanced.distanceFade = false;
    instanced.fadeStart = 0.0f;
    instanced.fadeEnd = 0.0f;
    instanced.cullNearDistance = 0.0f;
    instanced.instancedBillboard = false;
    instanced.instancedCylindricalBillboard = false;
    instanced.viewSettings.cull = true;
    instanced.viewSettings.castShadows = true;
    instanced.viewSettings.lodEnabled = true;
    instanced.viewSettings.lodBias = 1.0f;
    terrain.heightMap.setPath("");
    terrain.blendMaps.clear();
    terrain.surfaceLayers.clear();
    terrain.foliageLayers.clear();
    terrain.autoSetRanges = true;
    terrain.offset = quantum::Vector2::ZERO;
    terrain.terrainSize = 200.0f;
    terrain.maxHeight = 5.0f;
    terrain.resolution = 32.0f;
    terrain.textureBaseTiles = 1.0f;
    terrain.textureDetailTiles = 20.0f;
    terrain.rootGridSize = 2;
    terrain.levels = 6;
    terrain.needUpdateTerrain = false;
    sprite.width = 0;
    sprite.height = 0;
    sprite.automaticFlipY = true;
    sprite.flipY = false;
    sprite.textureScaleFactor = 0.0f;
    sprite.numFramesRect = 0;
    sprite.pivotPreset = quantum::PivotPreset::BOTTOM_LEFT;
    sprite.pendingFrameRect = quantum::Rect();
    sprite.needUpdateFrameRect = false;
    sprite.needUpdateSprite = false;
    spriteAnimation.loop = true;
    spriteAnimation.framesTimeSize = 0;
    spriteAnimation.framesSize = 0;
    spriteAnimation.frameIndex = 0;
    spriteAnimation.frameTimeIndex = 0;
    spriteAnimation.spriteFrameCount = 0;
    meshPolygon.width = 0;
    meshPolygon.height = 0;
    meshPolygon.automaticFlipY = true;
    meshPolygon.flipY = false;
    meshPolygon.points.clear();
    meshPolygon.needUpdatePolygon = false;
    polygon.points.clear();
    polygon.needUpdatePolygon = false;
    points.points.clear();
    points.numFramesRect = 0;
    points.maxPoints = 100;
    points.customShader.clear();
    points.texture.setPath("");
    points.transparent = false;
    points.autoTransparency = true;
    points.needUpdate = false;
    points.needUpdateBuffer = false;
    points.needUpdateTexture = false;
    points.needReload = false;
    lines.lines.clear();
    lines.maxLines = 100;
    lines.customShader.clear();
    lines.needUpdateBuffer = false;
    lines.needReload = false;
    tilemap.width = 0;
    tilemap.height = 0;
    tilemap.automaticFlipY = true;
    tilemap.flipY = false;
    tilemap.textureScaleFactor = 0.0f;
    tilemap.reserveTiles = 10;
    tilemap.numTilesRect = 0;
    tilemap.numTiles = 0;
    tilemap.needUpdateTilemap = false;
    animator.controllerAsset.clear();
    animator.parameters.clear();
    animator.layers.clear();
    animator.ikChains.clear();
    animator.rootMotionEnabled = false;
    animator.rootMotionTarget = NULL_ENTITY;
    animator.rootMotionScale = 1.0f;
    animator.states.clear();
    animator.transitions.clear();
    animator.enabled = false;
    mesh.customShader.clear();
    mesh.customDepthShader.clear();
    mesh.receiveLights = true;
    mesh.receiveIBL = false;
    mesh.castShadows = true;
    mesh.transparent = false;
    mesh.lodEnabled = true;
    mesh.lodBias = 1.0f;
    mesh.cullingMode = quantum::CullingMode::BACK;
    mesh.windingOrder = quantum::WindingOrder::CCW;
    mesh.numSubmeshes = 0;
    mesh.needReload = false;
    joint2D.type = quantum::Joint2DType::DISTANCE;
    joint2D.bodyA = NULL_ENTITY;
    joint2D.bodyB = NULL_ENTITY;
    joint2D.anchorA = quantum::Vector2::ZERO;
    joint2D.anchorB = quantum::Vector2::ZERO;
    joint2D.axis = quantum::Vector2::ZERO;
    joint2D.target = quantum::Vector2::ZERO;
    joint2D.autoAnchors = true;
    joint2D.rope = false;
    joint2D.needUpdateJoint = false;
    joint3D.type = quantum::Joint3DType::FIXED;
    joint3D.bodyA = NULL_ENTITY;
    joint3D.bodyB = NULL_ENTITY;
    joint3D.pathPoints.clear();
    joint3D.pathPosition = quantum::Vector3::ZERO;
    joint3D.isLooping = false;
    joint3D.autoAnchors = true;
    joint3D.needUpdateJoint = false;
    bone.model = NULL_ENTITY;
    bone.index = 0;
    bone.bindPosition = quantum::Vector3::ZERO;
    bone.bindRotation = quantum::Quaternion::IDENTITY;
    bone.bindScale = quantum::Vector3::UNIT_SCALE;
    bone.offsetMatrix = quantum::Matrix4();
    morphTracks.values.clear();
    morphTracks.inTangents.clear();
    morphTracks.outTangents.clear();
    keyframes.times.clear();
    keyframes.easings.clear();
    keyframes.loop = false;
    keyframes.relative = false;
    keyframes.index = 0;
    keyframes.interpolation = 0.0f;
    keyframes.hasBase = false;
    keyframes.basePosition = quantum::Vector3::ZERO;
    keyframes.baseRotation = quantum::Quaternion::IDENTITY;
    keyframes.baseScale = quantum::Vector3::UNIT_SCALE;
    scaleTracks.values.clear();
    scaleTracks.inTangents.clear();
    scaleTracks.outTangents.clear();
    translateTracks.values.clear();
    translateTracks.inTangents.clear();
    translateTracks.outTangents.clear();
    rotateTracks.values.clear();
    rotateTracks.inTangents.clear();
    rotateTracks.outTangents.clear();
    scene.setEntityName(entity, "cleared");
    tag.categories = quantum::TagComponent::NONE;
    tag.tag.clear();
    tag.layer = 0;
    sound.filename.clear();
    sound.bus = "master";
    sound.volume = 1.0;
    sound.looping = false;
    sound.occlusionEnabled = false;
    particles.maxParticles = 100;
    particles.emitter = false;
    particles.loop = true;
    particles.localSpace = false;
    particles.collisionEnabled = false;
    particles.trailsEnabled = false;
    particles.rate = 5;
    particles.maxPerUpdate = 100;
    light.type = quantum::LightType::DIRECTIONAL;
    light.direction = quantum::Vector3::ZERO;
    light.color = quantum::Vector3::UNIT_SCALE;
    light.range = 0.0f;
    light.intensity = 1.0f;
    light.shadows = false;
    light.mapResolution = 2048;
    light.numShadowCascades = 3;
    body.type = quantum::BodyType::STATIC;
    body.motionQuality = quantum::Body3DMotionQuality::DISCRETE;
    body.gravityFactor = 1.0f;
    body.linearDamping = 0.05f;
    body.angularDamping = 0.05f;
    body.buoyancy = 0.0f;
    body.waterDrag = 0.5f;
    body.waterAngularDrag = 0.01f;
    body.categoryBits = 1;
    body.maskBits = 0xFFFF;
    body.sensor = false;
    body.followingTransform = false;
    body.numShapes = 0;
    autoCollider.type = quantum::AutoColliderType::BOX;
    autoCollider.radius = 0.5f;
    autoCollider.width = 1.0f;
    autoCollider.height = 1.0f;
    autoCollider.depth = 1.0f;
    autoCollider.halfHeight = 0.5f;
    autoCollider.center = quantum::Vector3::ZERO;
    autoCollider.sensor = false;
    autoCollider.density = 1000.0f;
    autoCollider.categoryBits = 1;
    autoCollider.maskBits = 0xFFFF;
    autoCollider.sourceEntity = 0;
    autoCollider.needUpdate = true;
    body2D.type = quantum::BodyType::STATIC;
    body2D.followingTransform = false;
    body2D.loadedScale = quantum::Vector2::UNIT_SCALE;
    body2D.numShapes = 0;
    camera.type = quantum::CameraType::CAMERA_UI;
    camera.target = quantum::Vector3::ZERO;
    camera.yfov = 0.75f;
    camera.aspect = 1.0f;
    camera.nearClip = -10.0f;
    camera.farClip = 10.0f;
    camera.renderToTexture = false;
    camera.framebufferWidth = 512;
    camera.framebufferHeight = 512;
    camera.framebufferFilter = quantum::TextureFilter::LINEAR;
    camera.transparentSort = true;
    camera.depthTest = true;
    camera.useTarget = true;
    camera.autoResize = true;
    fog.type = quantum::FogType::LINEAR;
    fog.color = quantum::Vector3(0.8f, 0.8f, 0.8f);
    fog.density = 0.01f;
    fog.linearStart = 10.0f;
    fog.linearEnd = 60.0f;
    fog.heightFog = false;
    fog.fogNoiseEnabled = false;
    fog.godRays = false;
    fog.cloudsEnabled = false;
    probe.mode = quantum::ReflectionProbeMode::STATIC;
    probe.updateMode = quantum::ReflectionProbeUpdateMode::ON_LOAD;
    probe.boxOffset = quantum::Vector3::ZERO;
    probe.boxSize = quantum::Vector3(10.0f, 10.0f, 10.0f);
    probe.blendDistance = 1.0f;
    probe.intensity = 1.0f;
    probe.priority = 0;
    probe.resolution = 128;
    probe.nearClip = 0.1f;
    probe.farClip = 100.0f;
    probe.updateInterval = 1.0f;
    probe.includeSky = true;
    water.customShader.clear();
    water.customUnderwaterShader.clear();
    water.size = quantum::Vector2(50.0f, 50.0f);
    water.subdivisions = 128;
    water.waveHeight = 0.2f;
    water.waveLength = 6.0f;
    water.waveSpeed = 1.0f;
    water.windInfluence = 1.0f;
    water.shallowColor = quantum::Vector3(0.2f, 0.62f, 0.62f);
    water.deepColor = quantum::Vector3(0.02f, 0.13f, 0.22f);
    water.planarReflection = false;
    water.refraction = true;
    water.simulate = false;
    water.simResolution = 128;
    water.simDepth = 0.3f;
    water.simFlow = 0.45f;
    water.simRain = 0.0f;
    water.simEvaporation = 0.0f;
    water.simEdgeDrain = false;
    water.simRipples = true;
    decal.enabled = false;
    decal.color = quantum::Vector3::UNIT_SCALE;
    decal.size = quantum::Vector3::UNIT_SCALE;
    decal.opacity = 1.0f;
    decal.depthFade = 0.08f;
    decal.texture.setPath("");
    light2D.color = quantum::Vector3::UNIT_SCALE;
    light2D.intensity = 1.0f;
    light2D.range = 200.0f;
    light2D.falloff = 1.0f;
    light2D.height = 0.0f;
    light2D.shadows = false;
    light2D.shadowBias = 0.01f;
    light2D.shadowSoftness = 2.0f;
    light2D.mapResolution = 512;
    sky.customShader.clear();
    sky.color = quantum::Vector4::UNIT_SCALE;
    sky.rotation = 0.0f;
    sky.visible = true;
    sky.texture.setPath("");
    mirror.normal = quantum::Vector3(0.0f, 0.0f, 1.0f);
    occluder.shape = quantum::Occluder2DShape::AUTO_QUAD;
    occluder.closed = true;
    occluder.enabled = false;
    occluder.points.clear();
    exclusion.shape = quantum::WaterExclusionShape::HULL;
    exclusion.center = quantum::Vector3::ZERO;
    exclusion.size = quantum::Vector3::UNIT_SCALE;

    quantum::SaveGame reader(slot);
    const bool restored = saved && reader.load() && network->loadReplicatedState(reader, "ecs")
        && transform.position == quantum::Vector3(7.0f, 8.0f, 9.0f)
        && networkComponent.replicateTransform
        && networkComponent.netId == 4242
        && networkComponent.ownerId == 17
        && networkComponent.interestGroup == 9
        && std::abs(networkComponent.interestRadius - 125.5f) < 0.0001f
        && !networkComponent.serverAuthority
        && scripts.scripts.size() == 1
        && scripts.scripts[0].type == quantum::ScriptType::LUA
        && scripts.scripts[0].path == "scripts/player.lua"
        && scripts.scripts[0].className == "PlayerController"
        && scripts.scripts[0].enabled
        && scripts.scripts[0].properties.size() == 2
        && scripts.scripts[0].properties[0].name == "speed"
        && scripts.scripts[0].properties[0].type == quantum::ScriptPropertyType::Float
        && std::abs(scripts.scripts[0].properties[0].getValue<float>() - 3.5f) < 0.0001f
        && std::abs(scripts.scripts[0].properties[0].getDefaultValue<float>() - 1.0f) < 0.0001f
        && scripts.scripts[0].properties[1].name == "spawn"
        && scripts.scripts[0].properties[1].getValue<quantum::Vector3>() == quantum::Vector3(4.0f, 5.0f, 6.0f)
        && model.filename == "models/hero.glb"
        && model.mergeStaticMeshes && model.needUpdateModel
        && animation.actions.size() == 1
        && std::abs(animation.actions[0].startTime - 1.25f) < 0.0001f
        && std::abs(animation.actions[0].duration - 2.5f) < 0.0001f
        && animation.actions[0].action == 77 && animation.actions[0].track == 3
        && animation.actions[0].started
        && animation.ownedActions && animation.loop
        && std::abs(animation.duration - 8.0f) < 0.0001f
        && std::abs(animation.defaultFadeTime - 0.35f) < 0.0001f
        && bundle.name == "hero_prefab"
        && bundle.path == "bundles/characters/hero.yaml"
        && instanced.instances.size() == 1
        && instanced.instances[0].position == quantum::Vector3(2.0f, 3.0f, 4.0f)
        && instanced.instances[0].scale == quantum::Vector3(1.5f, 2.0f, 0.75f)
        && instanced.instances[0].color == quantum::Vector4(0.7f, 0.8f, 0.9f, 1.0f)
        && instanced.instances[0].textureRect == quantum::Rect(0.1f, 0.2f, 0.3f, 0.4f)
        && !instanced.instances[0].visible && instanced.maxInstances == 512
        && !instanced.cullInstances && instanced.distanceFade
        && std::abs(instanced.cullDistance - 120.0f) < 0.0001f
        && std::abs(instanced.fadeStart - 80.0f) < 0.0001f
        && std::abs(instanced.fadeEnd - 110.0f) < 0.0001f
        && std::abs(instanced.cullNearDistance - 2.0f) < 0.0001f
        && instanced.instancedBillboard && instanced.instancedCylindricalBillboard
        && !instanced.viewSettings.cull && !instanced.viewSettings.castShadows
        && !instanced.viewSettings.lodEnabled
        && std::abs(instanced.viewSettings.lodBias - 1.5f) < 0.0001f
        && terrain.heightMap.getPath() == "terrain/island_height.png"
        && terrain.blendMaps.size() == 1
        && terrain.blendMaps[0].getPath() == "terrain/island_blend.png"
        && terrain.surfaceLayers.size() == 1 && terrain.surfaceLayers[0].pbr
        && terrain.surfaceLayers[0].colorTexture.getPath() == "terrain/grass_albedo.png"
        && terrain.surfaceLayers[0].normalTexture.getPath() == "terrain/grass_normal.png"
        && terrain.surfaceLayers[0].colorFactor == quantum::Vector4(0.8f, 0.9f, 0.7f, 1.0f)
        && std::abs(terrain.surfaceLayers[0].normalStrength - 0.75f) < 0.0001f
        && terrain.surfaceLayers[0].uvScale == quantum::Vector2(2.0f, 3.0f)
        && terrain.foliageLayers.size() == 1
        && terrain.foliageLayers[0].meshPath == "models/grass.glb"
        && terrain.foliageLayers[0].densityMap.getPath() == "terrain/grass_density.png"
        && std::abs(terrain.foliageLayers[0].density - 0.35f) < 0.0001f
        && terrain.foliageLayers[0].seed == 1234 && !terrain.foliageLayers[0].castShadows
        && !terrain.autoSetRanges && terrain.offset == quantum::Vector2(5.0f, -2.0f)
        && std::abs(terrain.terrainSize - 640.0f) < 0.0001f
        && std::abs(terrain.maxHeight - 48.0f) < 0.0001f
        && terrain.rootGridSize == 4 && terrain.levels == 8
        && terrain.needUpdateTerrain
        && sprite.width == 256 && sprite.height == 128
        && !sprite.automaticFlipY && sprite.flipY
        && std::abs(static_cast<float>(sprite.textureScaleFactor) - 0.5f) < 0.0001f
        && sprite.numFramesRect == 2
        && sprite.framesRect[0].name == "idle"
        && sprite.framesRect[0].rect == quantum::Rect(0.0f, 0.0f, 0.25f, 0.5f)
        && sprite.framesRect[1].name == "run"
        && sprite.pivotPreset == quantum::PivotPreset::TOP_CENTER
        && sprite.pendingFrameRect == quantum::Rect(0.1f, 0.2f, 0.3f, 0.4f)
        && sprite.needUpdateFrameRect && sprite.needUpdateSprite
        && !spriteAnimation.loop && spriteAnimation.framesTimeSize == 2
        && spriteAnimation.framesTime[0] == 120 && spriteAnimation.framesTime[1] == 240
        && spriteAnimation.framesSize == 2 && spriteAnimation.frames[0] == 0
        && spriteAnimation.frames[1] == 1 && spriteAnimation.frameIndex == 1
        && spriteAnimation.frameTimeIndex == 1 && spriteAnimation.spriteFrameCount == 0
        && meshPolygon.width == 320 && meshPolygon.height == 180
        && !meshPolygon.automaticFlipY && meshPolygon.flipY
        && meshPolygon.points.size() == 3
        && meshPolygon.points[1].position == quantum::Vector3(0.0f, 2.0f, 0.0f)
        && meshPolygon.points[2].color == quantum::Vector4(0.0f, 0.0f, 1.0f, 1.0f)
        && meshPolygon.needUpdatePolygon
        && polygon.points.size() == 3
        && polygon.points[0].position == quantum::Vector3(-2.0f, 0.0f, 0.0f)
        && polygon.points[1].color == quantum::Vector4(0.2f, 0.8f, 0.35f, 0.9f)
        && polygon.needUpdatePolygon
        && points.points.size() == 2
        && points.points[0].position == quantum::Vector3(-1.0f, 0.5f, 0.0f)
        && points.points[0].color == quantum::Vector4(1.0f, 0.5f, 0.25f, 1.0f)
        && std::abs(points.points[0].size - 0.75f) < 0.0001f
        && points.points[0].textureRect == quantum::Rect(0.0f, 0.0f, 0.5f, 0.5f)
        && !points.points[1].visible
        && points.numFramesRect == 1 && points.framesRect[0].name == "spark"
        && points.maxPoints == 512
        && points.customShader == "shaders/points_custom"
        && points.texture.getPath() == "textures/particles/point.png"
        && points.transparent && !points.autoTransparency && points.needUpdate
        && lines.lines.size() == 2
        && lines.lines[0].pointA == quantum::Vector3(-2.0f, 0.0f, 0.0f)
        && lines.lines[0].colorB == quantum::Vector4(0.0f, 1.0f, 0.0f, 0.75f)
        && lines.lines[1].pointB == quantum::Vector3(0.0f, 2.0f, 0.0f)
        && lines.maxLines == 256
        && lines.customShader == "shaders/lines_custom"
        && lines.needUpdateBuffer && lines.needReload
        && tilemap.width == 32 && tilemap.height == 16
        && !tilemap.automaticFlipY && tilemap.flipY
        && std::abs(tilemap.textureScaleFactor - 0.75f) < 0.0001f
        && tilemap.reserveTiles == 8 && tilemap.numTilesRect == 2
        && tilemap.tilesRect[0].name == "grass" && tilemap.tilesRect[0].submeshId == 0
        && tilemap.tilesRect[1].submeshId == -1
        && tilemap.numTiles == 2 && tilemap.tiles[1].rectId == 1
        && tilemap.tiles[1].position == quantum::Vector2(4.0f, 5.0f)
        && std::abs(tilemap.tiles[1].height - 1.5f) < 0.0001f
        && tilemap.needUpdateTilemap
        && animator.controllerAsset == "animations/player.animcontroller"
        && animator.parameters.size() == 1
        && animator.parameters[0].name == "speed"
        && animator.parameters[0].type == quantum::AnimatorParameter::Type::Float
        && std::abs(animator.parameters[0].defaultValue - 1.0f) < 0.0001f
        && animator.layers.size() == 1 && animator.layers[0].name == "Base"
        && std::abs(animator.layers[0].weight - 0.85f) < 0.0001f
        && animator.ikChains.size() == 1 && std::abs(animator.ikChains[0].weight - 0.7f) < 0.0001f
        && animator.rootMotionEnabled && animator.rootMotionTarget == entity
        && std::abs(animator.rootMotionScale - 1.25f) < 0.0001f
        && animator.states.size() == 1 && animator.states[0].name == "Idle"
        && animator.states[0].isDefault && animator.states[0].clips.size() == 1
        && animator.states[0].clips[0].name == "idle_clip"
        && animator.states[0].clips[0].events.size() == 1
        && animator.states[0].clips[0].events[0].functionName == "OnFootstep"
        && animator.transitions.size() == 1 && animator.transitions[0].name == "IdleToRun"
        && animator.transitions[0].conditions.size() == 1
        && animator.transitions[0].conditions[0].parameter == "speed"
        && animator.enabled
        && mesh.customShader == "shaders/mesh_custom"
        && mesh.customDepthShader == "shaders/mesh_depth_custom"
        && !mesh.receiveLights && mesh.receiveIBL && !mesh.castShadows && mesh.transparent
        && !mesh.lodEnabled && std::abs(mesh.lodBias - 1.4f) < 0.0001f
        && mesh.cullingMode == quantum::CullingMode::FRONT
        && mesh.windingOrder == quantum::WindingOrder::CW
        && mesh.numSubmeshes == 1
        && mesh.submeshes[0].material.baseColorFactor == quantum::Vector4(0.7f, 0.2f, 0.1f, 1.0f)
        && mesh.submeshes[0].material.baseColorTexture.getPath() == "textures/materials/hero_albedo.png"
        && mesh.submeshes[0].material.alphaMode == quantum::MaterialAlphaMode::BLEND
        && !mesh.submeshes[0].faceCulling && mesh.submeshes[0].textureShadow
        && mesh.submeshes[0].primitiveType == quantum::PrimitiveType::TRIANGLE_STRIP
        && mesh.needReload
        && joint2D.type == quantum::Joint2DType::WHEEL
        && joint2D.bodyA == entity && joint2D.anchorA == quantum::Vector2(1.0f, 2.0f)
        && joint2D.axis == quantum::Vector2(0.0f, 1.0f) && joint2D.target == quantum::Vector2(3.0f, 4.0f)
        && !joint2D.autoAnchors && joint2D.rope && joint2D.needUpdateJoint
        && joint3D.type == quantum::Joint3DType::SIXDOF
        && joint3D.bodyA == entity && joint3D.anchorA == quantum::Vector3(1.0f, 2.0f, 3.0f)
        && std::abs(joint3D.limitsMin + 2.0f) < 0.0001f && std::abs(joint3D.limitsMax - 5.0f) < 0.0001f
        && joint3D.numTeethGearA == 32 && joint3D.pathPoints.size() == 2
        && joint3D.pathPosition == quantum::Vector3(0.25f, 0.5f, 0.75f)
        && joint3D.isLooping && !joint3D.autoAnchors && joint3D.needUpdateJoint
        && bone.model == entity && bone.index == 3
        && bone.bindPosition == quantum::Vector3(1.0f, 2.0f, 3.0f)
        && bone.bindRotation == quantum::Quaternion(0.9f, 0.1f, 0.2f, 0.3f)
        && bone.bindScale == quantum::Vector3(1.2f, 0.8f, 1.1f)
        && std::abs(bone.offsetMatrix[0][3] - 4.0f) < 0.0001f
        && std::abs(bone.offsetMatrix[1][3] - 5.0f) < 0.0001f
        && std::abs(bone.offsetMatrix[2][3] - 6.0f) < 0.0001f
        && morphTracks.values.size() == 2 && morphTracks.values[0].size() == 3
        && std::abs(morphTracks.values[0][1] - 0.25f) < 0.0001f
        && morphTracks.inTangents.size() == 2 && morphTracks.inTangents[1][0] == 0.3f
        && morphTracks.outTangents.size() == 2 && morphTracks.outTangents[1][1] == 0.45f
        && keyframes.times.size() == 3 && std::abs(keyframes.times[1] - 0.5f) < 0.0001f
        && keyframes.easings.size() == 2 && keyframes.easings[1] == quantum::EaseType::CUBIC_IN_OUT
        && keyframes.loop && keyframes.relative && keyframes.index == 2
        && std::abs(keyframes.interpolation - 0.75f) < 0.0001f && keyframes.hasBase
        && keyframes.basePosition == quantum::Vector3(2.0f, 3.0f, 4.0f)
        && keyframes.baseRotation == quantum::Quaternion(0.8f, 0.1f, 0.2f, 0.3f)
        && keyframes.baseScale == quantum::Vector3(1.1f, 1.2f, 1.3f)
        && scaleTracks.values.size() == 2
        && scaleTracks.values[1] == quantum::Vector3(1.2f, 1.1f, 0.9f)
        && scaleTracks.inTangents.size() == 1
        && scaleTracks.inTangents[0] == quantum::Vector3(0.0f, 0.1f, 0.0f)
        && scaleTracks.outTangents.size() == 1
        && scaleTracks.outTangents[0] == quantum::Vector3(0.1f, 0.0f, 0.1f)
        && translateTracks.values.size() == 2
        && translateTracks.values[1] == quantum::Vector3(2.0f, 3.0f, 4.0f)
        && translateTracks.inTangents.size() == 1
        && translateTracks.inTangents[0] == quantum::Vector3(0.1f, 0.2f, 0.3f)
        && translateTracks.outTangents.size() == 1
        && translateTracks.outTangents[0] == quantum::Vector3(0.3f, 0.2f, 0.1f)
        && rotateTracks.values.size() == 2
        && rotateTracks.values[1] == quantum::Quaternion(0.8f, 0.1f, 0.2f, 0.3f)
        && rotateTracks.inTangents.size() == 1
        && rotateTracks.inTangents[0] == quantum::Quaternion(0.9f, 0.1f, 0.0f, 0.0f)
        && rotateTracks.outTangents.size() == 1
        && rotateTracks.outTangents[0] == quantum::Quaternion(0.85f, 0.0f, 0.1f, 0.0f)
        && scene.getEntityName(entity) == "serialization-probe"
        && tag.categories == (quantum::TagComponent::PLAYER | quantum::TagComponent::DAMAGABLE)
        && tag.tag == "serializer-probe" && tag.layer == 4
        && sound.filename == "audio/probe.wav" && sound.bus == "effects"
        && std::abs(sound.volume - 0.55) < 0.0001 && sound.looping
        && sound.occlusionEnabled && std::abs(sound.occlusionVolume - 0.2f) < 0.0001f
        && particles.maxParticles == 320 && particles.emitter && !particles.loop
        && particles.localSpace && particles.collisionEnabled
        && std::abs(particles.collisionBounce - 0.7f) < 0.0001f
        && std::abs(particles.collisionFriction - 0.15f) < 0.0001f
        && particles.trailsEnabled && particles.trailLength == 10
        && particles.rate == 42 && particles.maxPerUpdate == 21
        && light.type == quantum::LightType::SPOT
        && light.direction == quantum::Vector3(0.0f, -1.0f, 0.25f)
        && light.color == quantum::Vector3(0.8f, 0.7f, 0.5f)
        && std::abs(light.range - 24.0f) < 0.0001f
        && std::abs(light.intensity - 3.5f) < 0.0001f
        && light.shadows && light.mapResolution == 1024
        && light.numShadowCascades == 4
        && body.type == quantum::BodyType::DYNAMIC
        && body.motionQuality == quantum::Body3DMotionQuality::LINEAR_CAST
        && std::abs(body.gravityFactor - 0.75f) < 0.0001f
        && std::abs(body.linearDamping - 0.37f) < 0.0001f
        && std::abs(body.angularDamping - 0.21f) < 0.0001f
        && std::abs(body.buoyancy - 1.25f) < 0.0001f
        && std::abs(body.waterDrag - 0.8f) < 0.0001f
        && std::abs(body.waterAngularDrag - 0.04f) < 0.0001f
        && body.categoryBits == 4 && body.maskBits == 0x00F0
        && body.sensor && body.followingTransform
        && body.numShapes == 1
        && body.shapes[0].type == quantum::Shape3DType::BOX
        && body.shapes[0].position == quantum::Vector3(0.0f, 0.5f, 0.0f)
        && std::abs(body.shapes[0].width - 2.0f) < 0.0001f
        && std::abs(body.shapes[0].height - 3.0f) < 0.0001f
        && std::abs(body.shapes[0].depth - 4.0f) < 0.0001f
        && std::abs(body.shapes[0].density - 700.0f) < 0.0001f
        && std::abs(body.shapes[0].friction - 0.45f) < 0.0001f
        && std::abs(body.shapes[0].restitution - 0.2f) < 0.0001f
        && autoCollider.type == quantum::AutoColliderType::CONVEX_HULL
        && std::abs(autoCollider.radius - 0.8f) < 0.0001f
        && std::abs(autoCollider.width - 2.0f) < 0.0001f
        && std::abs(autoCollider.height - 3.0f) < 0.0001f
        && std::abs(autoCollider.depth - 4.0f) < 0.0001f
        && std::abs(autoCollider.halfHeight - 1.5f) < 0.0001f
        && autoCollider.center == quantum::Vector3(0.25f, 0.5f, -0.75f)
        && autoCollider.sensor && std::abs(autoCollider.density - 650.0f) < 0.0001f
        && autoCollider.categoryBits == 16 && autoCollider.maskBits == 0x00AA
        && autoCollider.sourceEntity == 42 && !autoCollider.needUpdate
        && body2D.type == quantum::BodyType::KINEMATIC
        && body2D.followingTransform
        && body2D.loadedScale == quantum::Vector2(1.5f, 0.75f)
        && body2D.numShapes == 1
        && body2D.shapes[0].type == quantum::Shape2DType::POLYGON
        && body2D.shapes[0].numVertices == 4
        && body2D.shapes[0].vertices[0] == quantum::Vector2(-1.0f, -1.0f)
        && body2D.shapes[0].vertices[2] == quantum::Vector2(1.0f, 1.0f)
        && std::abs(body2D.shapes[0].density - 2.0f) < 0.0001f
        && std::abs(body2D.shapes[0].friction - 0.35f) < 0.0001f
        && std::abs(body2D.shapes[0].restitution - 0.15f) < 0.0001f
        && body2D.shapes[0].sensor
        && body2D.shapes[0].categoryBits == 8
        && body2D.shapes[0].maskBits == 0x00CC
        && body2D.shapes[0].groupIndex == -3
        && camera.type == quantum::CameraType::CAMERA_PERSPECTIVE
        && camera.target == quantum::Vector3(2.0f, 3.0f, 4.0f)
        && std::abs(camera.yfov - 1.1f) < 0.0001f
        && std::abs(camera.aspect - 1.777f) < 0.0001f
        && std::abs(camera.nearClip - 0.15f) < 0.0001f
        && std::abs(camera.farClip - 1500.0f) < 0.0001f
        && camera.renderToTexture
        && camera.framebufferWidth == 1024 && camera.framebufferHeight == 512
        && camera.framebufferFilter == quantum::TextureFilter::LINEAR_MIPMAP_LINEAR
        && !camera.transparentSort && !camera.depthTest && !camera.useTarget && !camera.autoResize
        && fog.type == quantum::FogType::EXPONENTIALSQUARED
        && fog.color == quantum::Vector3(0.2f, 0.3f, 0.4f)
        && std::abs(fog.density - 0.03f) < 0.0001f
        && std::abs(fog.linearStart - 7.0f) < 0.0001f
        && std::abs(fog.linearEnd - 240.0f) < 0.0001f
        && fog.heightFog && std::abs(fog.fogHeightFalloff - 14.0f) < 0.0001f
        && std::abs(fog.fogGroundHeight + 2.0f) < 0.0001f
        && std::abs(fog.fogSunAmount - 0.9f) < 0.0001f
        && fog.fogNoiseEnabled && std::abs(fog.fogNoiseScale - 0.012f) < 0.0001f
        && fog.fogNoiseWind == quantum::Vector2(2.0f, -0.5f)
        && fog.godRays && std::abs(fog.rayIntensity - 1.4f) < 0.0001f
        && fog.cloudsEnabled && std::abs(fog.cloudBaseHeight - 180.0f) < 0.0001f
        && std::abs(fog.cloudCoverage - 0.62f) < 0.0001f
        && fog.cloudWind == quantum::Vector2(-1.0f, 0.4f)
        && fog.cloudColor == quantum::Vector3(0.7f, 0.8f, 0.95f)
        && probe.mode == quantum::ReflectionProbeMode::DYNAMIC
        && probe.updateMode == quantum::ReflectionProbeUpdateMode::INTERVAL
        && probe.boxOffset == quantum::Vector3(1.0f, 2.0f, 3.0f)
        && probe.boxSize == quantum::Vector3(20.0f, 12.0f, 8.0f)
        && std::abs(probe.blendDistance - 2.5f) < 0.0001f
        && std::abs(probe.intensity - 1.4f) < 0.0001f
        && probe.priority == 7 && probe.resolution == 256
        && std::abs(probe.nearClip - 0.2f) < 0.0001f
        && std::abs(probe.farClip - 600.0f) < 0.0001f
        && std::abs(probe.updateInterval - 0.25f) < 0.0001f
        && !probe.includeSky
        && water.customShader == "shaders/ocean_custom"
        && water.customUnderwaterShader == "shaders/underwater_custom"
        && water.size == quantum::Vector2(80.0f, 40.0f)
        && water.subdivisions == 96
        && std::abs(water.waveHeight - 0.55f) < 0.0001f
        && std::abs(water.waveLength - 12.0f) < 0.0001f
        && std::abs(water.waveSpeed - 2.5f) < 0.0001f
        && std::abs(water.windInfluence - 0.8f) < 0.0001f
        && water.shallowColor == quantum::Vector3(0.1f, 0.5f, 0.6f)
        && water.deepColor == quantum::Vector3(0.01f, 0.08f, 0.2f)
        && water.planarReflection && water.refraction && water.simulate
        && water.simResolution == 64
        && std::abs(water.simDepth - 0.45f) < 0.0001f
        && std::abs(water.simFlow - 0.8f) < 0.0001f
        && std::abs(water.simRain - 0.03f) < 0.0001f
        && std::abs(water.simEvaporation - 0.01f) < 0.0001f
        && water.simEdgeDrain && water.simRipples
        && action.state == quantum::ActionState::Running
        && std::abs(action.timecount - 1.5f) < 0.0001f
        && action.startTrigger && action.ownedTarget && action.target == 77
        && std::abs(action.speed - 1.25f) < 0.0001f && action.randomStart
        && action.blendLayer == 2 && std::abs(action.blendLayerWeight - 0.65f) < 0.0001f
        && action.blendLayerAdditive
        && positionAction.startPosition == quantum::Vector3(-1.0f, 0.0f, 2.0f)
        && positionAction.endPosition == quantum::Vector3(4.0f, 5.0f, 6.0f)
        && rotationAction.shortestPath && rotationAction.spin
        && rotationAction.axis == quantum::Vector3(0.0f, 1.0f, 0.0f)
        && std::abs(rotationAction.angle - 720.0f) < 0.0001f
        && scaleAction.startScale == quantum::Vector3(0.5f, 0.75f, 1.0f)
        && scaleAction.endScale == quantum::Vector3(2.0f, 3.0f, 4.0f)
        && colorAction.startColor == quantum::Vector3(0.1f, 0.2f, 0.3f)
        && colorAction.endColor == quantum::Vector3(0.8f, 0.7f, 0.6f) && !colorAction.useSRGB
        && std::abs(alphaAction.startAlpha - 0.15f) < 0.0001f
        && std::abs(alphaAction.endAlpha - 0.85f) < 0.0001f
        && std::abs(timedAction.time - 0.75f) < 0.0001f
        && std::abs(timedAction.value - 0.25f) < 0.0001f
        && std::abs(timedAction.duration - 3.5f) < 0.0001f
        && timedAction.loop && timedAction.pingPong
        && timedAction.function.getType() == quantum::EaseType::CUBIC_IN_OUT
        && decal.enabled
        && decal.color == quantum::Vector3(0.8f, 0.2f, 0.1f)
        && decal.size == quantum::Vector3(2.0f, 3.0f, 0.25f)
        && std::abs(decal.opacity - 0.75f) < 0.0001f
        && std::abs(decal.depthFade - 0.12f) < 0.0001f
        && decal.texture.getPath() == "textures/decals/scorch.png"
        && light2D.color == quantum::Vector3(1.0f, 0.35f, 0.1f)
        && std::abs(light2D.intensity - 2.5f) < 0.0001f
        && std::abs(light2D.range - 320.0f) < 0.0001f
        && std::abs(light2D.falloff - 1.8f) < 0.0001f
        && std::abs(light2D.height - 4.0f) < 0.0001f
        && light2D.shadows
        && std::abs(light2D.shadowBias - 0.02f) < 0.0001f
        && std::abs(light2D.shadowSoftness - 3.0f) < 0.0001f
        && light2D.mapResolution == 1024
        && light2D.shadowMapIndex == -1
        && sky.customShader == "shaders/sky_custom"
        && sky.color == quantum::Vector4(0.4f, 0.6f, 0.95f, 1.0f)
        && std::abs(sky.rotation - 1.25f) < 0.0001f
        && !sky.visible
        && sky.texture.getPath() == "textures/sky/stars.hdr"
        && mirror.normal == quantum::Vector3(0.0f, 0.7071f, 0.7071f)
        && occluder.shape == quantum::Occluder2DShape::POLYGON
        && !occluder.closed && occluder.enabled
        && occluder.points.size() == 3
        && occluder.points[0] == quantum::Vector2(-2.0f, -1.0f)
        && occluder.points[2] == quantum::Vector2(1.0f, 2.0f)
        && exclusion.shape == quantum::WaterExclusionShape::BOX
        && exclusion.center == quantum::Vector3(0.0f, 1.5f, -2.0f)
        && exclusion.size == quantum::Vector3(4.0f, 3.0f, 2.0f)
        && exclusion.hullPlanes.empty() && exclusion.hullPoints.empty()
        && exclusion.hullMap.empty() && exclusion.hullLayer == -1;
    writer.deleteSlot();

    if (!restored || !catalogValid || !componentCatalogValid) {
        std::cerr << "serialization-smoke: QNETECS3 round-trip failed; network="
            << networkComponent.netId << "," << networkComponent.ownerId << ","
            << networkComponent.interestGroup << "," << networkComponent.interestRadius
            << "," << (networkComponent.serverAuthority ? 1 : 0) << ","
            << (networkComponent.replicateTransform ? 1 : 0)
            << ", serializers=" << serializerCatalog.size()
            << ", components=" << componentCatalog.size() << "\n";
        return 1;
    }
    std::cout << "serialization-smoke: QNETECS3 round-trip OK; serializer-catalog="
              << serializerCatalog.size() << " unique schemas; component-catalog="
              << componentCatalog.size() << " unique IDs\n";
    return 0;
}
