// (c) SR VINIX
// SPDX-License-Identifier: MIT

#include "NetworkSystem.h"
#include "../Scene.h"
#include "../ecs/components/TagComponent.h"
#include "../component/Body2DComponent.h"
#include "../component/Body3DComponent.h"
#include "../component/AutoColliderComponent.h"
#include "../component/ScriptComponent.h"
#include "../component/ModelComponent.h"
#include "../component/AnimationComponent.h"
#include "../component/BundleComponent.h"
#include "../component/InstancedMeshComponent.h"
#include "../component/TerrainComponent.h"
#include "../component/SpriteComponent.h"
#include "../component/SpriteAnimationComponent.h"
#include "../component/MeshPolygonComponent.h"
#include "../component/PolygonComponent.h"
#include "../component/PointsComponent.h"
#include "../component/LinesComponent.h"
#include "../component/TilemapComponent.h"
#include "../component/AnimatorControllerComponent.h"
#include "../component/MeshComponent.h"
#include "../component/Joint2DComponent.h"
#include "../component/Joint3DComponent.h"
#include "../component/BoneComponent.h"
#include "../component/MorphTracksComponent.h"
#include "../component/KeyframeTracksComponent.h"
#include "../component/ScaleTracksComponent.h"
#include "../component/TranslateTracksComponent.h"
#include "../component/RotateTracksComponent.h"
#include "../component/ActionComponent.h"
#include "../component/PositionActionComponent.h"
#include "../component/RotationActionComponent.h"
#include "../component/ScaleActionComponent.h"
#include "../component/ColorActionComponent.h"
#include "../component/AlphaActionComponent.h"
#include "../component/TimedActionComponent.h"
#include "../component/CameraComponent.h"
#include "../component/FogComponent.h"
#include "../component/ReflectionProbeComponent.h"
#include "../component/WaterComponent.h"
#include "../component/DecalComponent.h"
#include "../component/Light2DComponent.h"
#include "../component/SkyComponent.h"
#include "../component/MirrorComponent.h"
#include "../component/Occluder2DComponent.h"
#include "../component/WaterExclusionComponent.h"
#include "../component/LightComponent.h"
#include "../component/SoundComponent.h"
#include "../io/SaveGame.h"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <iostream>
#include <random>
#include <thread>
#include <cstring>
#include <iterator>
#include <map>
#include <mutex>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using NetworkSocket = SOCKET;
constexpr NetworkSocket INVALID_NETWORK_SOCKET = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
using NetworkSocket = int;
constexpr NetworkSocket INVALID_NETWORK_SOCKET = -1;
#endif

namespace quantum {

namespace {

constexpr size_t MAX_PENDING_RELIABLE = 128;
constexpr size_t MAX_ORDERED_RELIABLE = 128;
constexpr size_t MAX_RECEIVED_RELIABLE = 256;
constexpr size_t MAX_RECEIVED_UNRELIABLE = 256;
constexpr size_t MAX_RECEIVED_SNAPSHOTS = 512;
constexpr size_t MAX_PACKET_BYTES = 1400;
constexpr uint32_t MAX_PACKETS_PER_SECOND = 600;
constexpr uint32_t MAX_BYTES_PER_SECOND = 512 * 1024;
constexpr uint32_t MAX_INTEREST_RADIUS = 100000;

bool wouldBlock() {
#if defined(_WIN32)
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

bool transientReceiveError() {
#if defined(_WIN32)
    const int error = WSAGetLastError();
    return error == WSAECONNRESET || error == WSAENOTSOCK;
#else
    return errno == ECONNREFUSED || errno == EBADF;
#endif
}

void closeSocket(NetworkSocket socket) {
#if defined(_WIN32)
    closesocket(socket);
#else
    close(socket);
#endif
}

bool makeNonBlocking(NetworkSocket socket) {
#if defined(_WIN32)
    u_long enabled = 1;
    return ioctlsocket(socket, FIONBIO, &enabled) == 0;
#else
    const int flags = fcntl(socket, F_GETFL, 0);
    return flags >= 0 && fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

bool samePeer(const sockaddr_in& left, const sockaddr_in& right) {
    return left.sin_family == right.sin_family
        && left.sin_port == right.sin_port
        && left.sin_addr.s_addr == right.sin_addr.s_addr;
}

bool finiteVector(const Vector3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool sameSnapshot(const network::TransformSnapshot& a, const network::TransformSnapshot& b) {
    if (a.netId != b.netId) return false;
    for (int i = 0; i < 3; ++i) {
        if (a.position[i] != b.position[i] || a.scale[i] != b.scale[i]) return false;
    }
    for (int i = 0; i < 4; ++i) {
        if (a.rotation[i] != b.rotation[i]) return false;
    }
    return true;
}

constexpr char ECS_SNAPSHOT_MAGIC_V1[] = "QNETECS1";
constexpr char ECS_SNAPSHOT_MAGIC_V2[] = "QNETECS2";
constexpr char ECS_SNAPSHOT_MAGIC[] = "QNETECS3";
constexpr uint32_t MAX_ENTITY_NAME_BYTES = 256;
constexpr uint32_t MAX_ENTITY_TAG_BYTES = 1024;
constexpr uint32_t MAX_COMPONENT_NAME_BYTES = 128;
constexpr uint32_t MAX_COMPONENT_PAYLOAD_BYTES = 1024 * 1024;
constexpr uint32_t MAX_COMPONENT_RECORDS = 64;

std::mutex replicatedComponentMutex;
std::map<std::string, NetworkSystem::ReplicatedComponentSerializer> replicatedComponentSerializers;

void appendU32(std::string& output, uint32_t value) {
    output.push_back(static_cast<char>(value & 0xffu));
    output.push_back(static_cast<char>((value >> 8) & 0xffu));
    output.push_back(static_cast<char>((value >> 16) & 0xffu));
    output.push_back(static_cast<char>((value >> 24) & 0xffu));
}

void appendU64(std::string& output, uint64_t value) {
    appendU32(output, static_cast<uint32_t>(value & 0xffffffffULL));
    appendU32(output, static_cast<uint32_t>(value >> 32));
}

void appendU8(std::string& output, uint8_t value) {
    output.push_back(static_cast<char>(value));
}

void appendString(std::string& output, const std::string& value) {
    appendU32(output, static_cast<uint32_t>(value.size()));
    output.append(value);
}

void appendFloat(std::string& output, float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    appendU32(output, bits);
}

void appendVector3(std::string& output, const Vector3& value) {
    appendFloat(output, value.x);
    appendFloat(output, value.y);
    appendFloat(output, value.z);
}

void appendVector2(std::string& output, const Vector2& value) {
    appendFloat(output, value.x);
    appendFloat(output, value.y);
}

void appendQuaternion(std::string& output, const Quaternion& value) {
    appendFloat(output, value.w);
    appendFloat(output, value.x);
    appendFloat(output, value.y);
    appendFloat(output, value.z);
}

bool readU32(const std::string& input, size_t& offset, uint32_t& value) {
    if (offset + 4 > input.size()) return false;
    value = static_cast<uint32_t>(static_cast<unsigned char>(input[offset]))
        | (static_cast<uint32_t>(static_cast<unsigned char>(input[offset + 1])) << 8)
        | (static_cast<uint32_t>(static_cast<unsigned char>(input[offset + 2])) << 16)
        | (static_cast<uint32_t>(static_cast<unsigned char>(input[offset + 3])) << 24);
    offset += 4;
    return true;
}

bool readU64(const std::string& input, size_t& offset, uint64_t& value) {
    uint32_t low = 0;
    uint32_t high = 0;
    if (!readU32(input, offset, low) || !readU32(input, offset, high)) return false;
    value = static_cast<uint64_t>(low) | (static_cast<uint64_t>(high) << 32);
    return true;
}

bool readU8(const std::string& input, size_t& offset, uint8_t& value) {
    if (offset >= input.size()) return false;
    value = static_cast<uint8_t>(input[offset++]);
    return true;
}

bool readBoundedString(const std::string& input, size_t& offset, uint32_t maxBytes,
    std::string& value) {
    uint32_t length = 0;
    if (!readU32(input, offset, length) || length > maxBytes
            || length > input.size() - offset) return false;
    value.assign(input.data() + offset, length);
    offset += length;
    return true;
}

void patchU32(std::string& output, size_t offset, uint32_t value) {
    if (offset + 4 > output.size()) return;
    output[offset] = static_cast<char>(value & 0xffu);
    output[offset + 1] = static_cast<char>((value >> 8) & 0xffu);
    output[offset + 2] = static_cast<char>((value >> 16) & 0xffu);
    output[offset + 3] = static_cast<char>((value >> 24) & 0xffu);
}

bool readFloat(const std::string& input, size_t& offset, float& value);
constexpr uint32_t MAX_SCRIPT_PROPERTY_BYTES = 64 * 1024;

bool readVector3(const std::string& input, size_t& offset, Vector3& value) {
    float x = 0.0f, y = 0.0f, z = 0.0f;
    if (!readFloat(input, offset, x) || !readFloat(input, offset, y) || !readFloat(input, offset, z)) return false;
    value = Vector3(x, y, z);
    return true;
}

bool readVector2(const std::string& input, size_t& offset, Vector2& value) {
    float x = 0.0f, y = 0.0f;
    if (!readFloat(input, offset, x) || !readFloat(input, offset, y)) return false;
    value = Vector2(x, y);
    return true;
}

bool readQuaternion(const std::string& input, size_t& offset, Quaternion& value) {
    float w = 0.0f, x = 0.0f, y = 0.0f, z = 0.0f;
    if (!readFloat(input, offset, w) || !readFloat(input, offset, x)
            || !readFloat(input, offset, y) || !readFloat(input, offset, z)) return false;
    value = Quaternion(w, x, y, z);
    return true;
}

bool readVector4(const std::string& input, size_t& offset, Vector4& value) {
    float x = 0.0f, y = 0.0f, z = 0.0f, w = 0.0f;
    if (!readFloat(input, offset, x) || !readFloat(input, offset, y)
            || !readFloat(input, offset, z) || !readFloat(input, offset, w)) return false;
    value = Vector4(x, y, z, w);
    return true;
}

bool appendTerrainTexture(std::string& output, const Texture& texture) {
    const std::string path = texture.getPath();
    if (path.size() > MAX_SCRIPT_PROPERTY_BYTES) return false;
    appendString(output, path);
    return true;
}

bool readTerrainTexture(const std::string& input, size_t& offset, Texture& texture) {
    std::string path;
    if (!readBoundedString(input, offset, MAX_SCRIPT_PROPERTY_BYTES, path)) return false;
    texture.setPath(path);
    return true;
}

constexpr uint32_t MAX_SCRIPT_ENTRIES = 64;
constexpr uint32_t MAX_SCRIPT_PROPERTIES = 256;

bool appendScriptPropertyValue(std::string& output, ScriptPropertyType type,
    const ScriptPropertyValue& value) {
    if (std::holds_alternative<std::monostate>(value)) {
        appendU8(output, 0);
        return true;
    }
    appendU8(output, 1);
    switch (type) {
        case ScriptPropertyType::Bool:
            if (!std::holds_alternative<bool>(value)) return false;
            appendU8(output, std::get<bool>(value) ? 1 : 0);
            return true;
        case ScriptPropertyType::Int:
            if (!std::holds_alternative<int>(value)) return false;
            appendU32(output, static_cast<uint32_t>(std::get<int>(value)));
            return true;
        case ScriptPropertyType::Float:
            if (!std::holds_alternative<float>(value) || !std::isfinite(std::get<float>(value))) return false;
            appendFloat(output, std::get<float>(value));
            return true;
        case ScriptPropertyType::String:
            if (!std::holds_alternative<std::string>(value)
                    || std::get<std::string>(value).size() > MAX_SCRIPT_PROPERTY_BYTES) return false;
            appendString(output, std::get<std::string>(value));
            return true;
        case ScriptPropertyType::Vector2:
            if (!std::holds_alternative<Vector2>(value)
                    || !std::isfinite(std::get<Vector2>(value).x)
                    || !std::isfinite(std::get<Vector2>(value).y)) return false;
            appendVector2(output, std::get<Vector2>(value));
            return true;
        case ScriptPropertyType::Vector3:
        case ScriptPropertyType::Color3:
            if (!std::holds_alternative<Vector3>(value)
                    || !std::isfinite(std::get<Vector3>(value).x)
                    || !std::isfinite(std::get<Vector3>(value).y)
                    || !std::isfinite(std::get<Vector3>(value).z)) return false;
            appendVector3(output, std::get<Vector3>(value));
            return true;
        case ScriptPropertyType::Vector4:
        case ScriptPropertyType::Color4:
            if (!std::holds_alternative<Vector4>(value)
                    || !std::isfinite(std::get<Vector4>(value).x)
                    || !std::isfinite(std::get<Vector4>(value).y)
                    || !std::isfinite(std::get<Vector4>(value).z)
                    || !std::isfinite(std::get<Vector4>(value).w)) return false;
            appendFloat(output, std::get<Vector4>(value).x);
            appendFloat(output, std::get<Vector4>(value).y);
            appendFloat(output, std::get<Vector4>(value).z);
            appendFloat(output, std::get<Vector4>(value).w);
            return true;
        case ScriptPropertyType::EntityReference:
            if (!std::holds_alternative<EntityReference>(value)) return false;
            appendU32(output, std::get<EntityReference>(value).entity);
            appendU32(output, std::get<EntityReference>(value).sceneId);
            return true;
    }
    return false;
}

bool readScriptPropertyValue(const std::string& input, size_t& offset, ScriptPropertyType type,
    ScriptPropertyValue& value) {
    uint8_t present = 0;
    if (!readU8(input, offset, present) || present > 1) return false;
    if (!present) {
        value = std::monostate{};
        return true;
    }
    switch (type) {
        case ScriptPropertyType::Bool: {
            uint8_t raw = 0;
            if (!readU8(input, offset, raw) || raw > 1) return false;
            value = raw != 0;
            return true;
        }
        case ScriptPropertyType::Int: {
            uint32_t raw = 0;
            if (!readU32(input, offset, raw)) return false;
            value = static_cast<int>(static_cast<int32_t>(raw));
            return true;
        }
        case ScriptPropertyType::Float: {
            float raw = 0.0f;
            if (!readFloat(input, offset, raw)) return false;
            value = raw;
            return true;
        }
        case ScriptPropertyType::String: {
            std::string raw;
            if (!readBoundedString(input, offset, MAX_SCRIPT_PROPERTY_BYTES, raw)) return false;
            value = std::move(raw);
            return true;
        }
        case ScriptPropertyType::Vector2: {
            float x = 0.0f, y = 0.0f;
            if (!readFloat(input, offset, x) || !readFloat(input, offset, y)) return false;
            value = Vector2(x, y);
            return true;
        }
        case ScriptPropertyType::Vector3:
        case ScriptPropertyType::Color3: {
            float x = 0.0f, y = 0.0f, z = 0.0f;
            if (!readFloat(input, offset, x) || !readFloat(input, offset, y) || !readFloat(input, offset, z)) return false;
            value = Vector3(x, y, z);
            return true;
        }
        case ScriptPropertyType::Vector4:
        case ScriptPropertyType::Color4: {
            float x = 0.0f, y = 0.0f, z = 0.0f, w = 0.0f;
            if (!readFloat(input, offset, x) || !readFloat(input, offset, y)
                    || !readFloat(input, offset, z) || !readFloat(input, offset, w)) return false;
            value = Vector4(x, y, z, w);
            return true;
        }
        case ScriptPropertyType::EntityReference: {
            uint32_t entity = 0, sceneId = 0;
            if (!readU32(input, offset, entity) || !readU32(input, offset, sceneId)) return false;
            value = EntityReference{static_cast<Entity>(entity), sceneId};
            return true;
        }
    }
    return false;
}

void ensureBuiltInReplicatedComponents() {
    static const bool networkRegistered = NetworkSystem::registerReplicatedComponent({
        "NetworkComponent", 2,
        [](Scene* scene, Entity entity, std::string& payload) {
            const NetworkComponent* network = scene->findComponent<NetworkComponent>(entity);
            if (!network || !std::isfinite(network->interestRadius)
                    || network->interestRadius < 0.0f || network->interestRadius > MAX_INTEREST_RADIUS) return false;
            appendU8(payload, network->replicateTransform ? 1 : 0);
            appendU32(payload, network->netId);
            appendU32(payload, network->ownerId);
            appendU32(payload, network->interestGroup);
            appendFloat(payload, network->interestRadius);
            appendU8(payload, network->serverAuthority ? 1 : 0);
            return true;
        },
        [](uint16_t version, Scene* scene, Entity entity, const std::string& payload) {
            if (version == 1) {
                if (payload.size() != 1 || static_cast<uint8_t>(payload[0]) > 1) return false;
            } else if (version != 2 || payload.size() != 18) {
                return false;
            }
            NetworkComponent* network = scene->findComponent<NetworkComponent>(entity);
            if (!network) {
                scene->addComponent<NetworkComponent>(entity);
                network = scene->findComponent<NetworkComponent>(entity);
            }
            if (!network) return false;
            network->replicateTransform = static_cast<uint8_t>(payload[0]) != 0;
            if (version == 2) {
                size_t offset = 1;
                uint32_t netId = 0, ownerId = 0, interestGroup = 0;
                float interestRadius = 0.0f;
                uint8_t serverAuthority = 0;
                if (!readU32(payload, offset, netId) || !readU32(payload, offset, ownerId)
                        || !readU32(payload, offset, interestGroup) || !readFloat(payload, offset, interestRadius)
                        || !readU8(payload, offset, serverAuthority) || serverAuthority > 1
                        || !std::isfinite(interestRadius) || interestRadius < 0.0f
                        || interestRadius > MAX_INTEREST_RADIUS || offset != payload.size()) return false;
                network->netId = netId;
                network->ownerId = ownerId;
                network->interestGroup = interestGroup;
                network->interestRadius = interestRadius;
                network->serverAuthority = serverAuthority != 0;
            }
            return true;
        }
    });
    (void)networkRegistered;

    static const bool lightRegistered = NetworkSystem::registerReplicatedComponent({
        "LightComponent", 1,
        [](Scene* scene, Entity entity, std::string& payload) {
            const LightComponent* light = scene->findComponent<LightComponent>(entity);
            if (!light || static_cast<uint8_t>(light->type) > static_cast<uint8_t>(LightType::SPOT)
                    || !std::isfinite(light->range) || !std::isfinite(light->intensity)
                    || !std::isfinite(light->innerConeCos) || !std::isfinite(light->outerConeCos)
                    || !std::isfinite(light->shadowBias) || !std::isfinite(light->shadowNormalBias)
                    || !std::isfinite(light->shadowCameraNearFar.x)
                    || !std::isfinite(light->shadowCameraNearFar.y)) return false;
            appendU8(payload, static_cast<uint8_t>(light->type));
            for (float value : {light->direction.x, light->direction.y, light->direction.z,
                    light->color.x, light->color.y, light->color.z, light->range,
                    light->intensity, light->innerConeCos, light->outerConeCos}) {
                appendFloat(payload, value);
            }
            appendU8(payload, light->shadows ? 1 : 0);
            appendU8(payload, light->automaticShadowCamera ? 1 : 0);
            appendFloat(payload, light->shadowBias);
            appendFloat(payload, light->shadowNormalBias);
            appendU32(payload, light->mapResolution);
            appendFloat(payload, light->shadowCameraNearFar.x);
            appendFloat(payload, light->shadowCameraNearFar.y);
            appendU32(payload, light->numShadowCascades);
            return payload.size() <= MAX_COMPONENT_PAYLOAD_BYTES;
        },
        [](uint16_t version, Scene* scene, Entity entity, const std::string& payload) {
            if (version != 1) return false;
            size_t offset = 0;
            uint8_t type = 0;
            float values[10]{};
            float shadowBias = 0.0f;
            float shadowNormalBias = 0.0f;
            float nearClip = 0.0f;
            float farClip = 0.0f;
            uint8_t shadows = 0;
            uint8_t automatic = 0;
            uint32_t mapResolution = 0;
            uint32_t cascades = 0;
            if (!readU8(payload, offset, type)
                    || type > static_cast<uint8_t>(LightType::SPOT)) return false;
            for (float& value : values) if (!readFloat(payload, offset, value)) return false;
            if (!readU8(payload, offset, shadows) || shadows > 1
                    || !readU8(payload, offset, automatic) || automatic > 1
                    || !readFloat(payload, offset, shadowBias)
                    || !readFloat(payload, offset, shadowNormalBias)
                    || !readU32(payload, offset, mapResolution)
                    || !readFloat(payload, offset, nearClip)
                    || !readFloat(payload, offset, farClip)
                    || !readU32(payload, offset, cascades) || offset != payload.size()
                    || mapResolution == 0 || mapResolution > 16384 || cascades > 8) return false;
            LightComponent* light = scene->findComponent<LightComponent>(entity);
            if (!light) {
                scene->addComponent<LightComponent>(entity);
                light = scene->findComponent<LightComponent>(entity);
            }
            if (!light) return false;
            light->type = static_cast<LightType>(type);
            light->direction = Vector3(values[0], values[1], values[2]);
            light->color = Vector3(values[3], values[4], values[5]);
            light->range = values[6];
            light->intensity = values[7];
            light->innerConeCos = values[8];
            light->outerConeCos = values[9];
            light->shadows = shadows != 0;
            light->automaticShadowCamera = automatic != 0;
            light->shadowBias = shadowBias;
            light->shadowNormalBias = shadowNormalBias;
            light->mapResolution = mapResolution;
            light->shadowCameraNearFar = Vector2(nearClip, farClip);
            light->numShadowCascades = cascades;
            light->needUpdateShadowCamera = true;
            light->needUpdateShadowMap = true;
            return true;
        }
    });
    (void)lightRegistered;

    static const bool bodyRegistered = NetworkSystem::registerReplicatedComponent({
        "Body3DComponent", 1,
        [](Scene* scene, Entity entity, std::string& payload) {
            const Body3DComponent* body = scene->findComponent<Body3DComponent>(entity);
            if (!body || static_cast<uint8_t>(body->type) > static_cast<uint8_t>(BodyType::DYNAMIC)
                    || static_cast<uint8_t>(body->motionQuality) > static_cast<uint8_t>(Body3DMotionQuality::LINEAR_CAST)
                    || !std::isfinite(body->solidBoxSize.x) || !std::isfinite(body->solidBoxSize.y)
                    || !std::isfinite(body->solidBoxSize.z) || !std::isfinite(body->solidBoxDensity)
                    || !std::isfinite(body->gravityFactor) || !std::isfinite(body->linearDamping)
                    || !std::isfinite(body->angularDamping) || !std::isfinite(body->buoyancy)
                    || !std::isfinite(body->waterDrag) || !std::isfinite(body->waterAngularDrag)) return false;
            appendU8(payload, static_cast<uint8_t>(body->type));
            appendU8(payload, static_cast<uint8_t>(body->motionQuality));
            appendU8(payload, body->allowedDOFs);
            appendU32(payload, body->categoryBits);
            appendU32(payload, body->maskBits);
            appendU8(payload, body->sensor ? 1 : 0);
            appendU8(payload, body->overrideMassProperties ? 1 : 0);
            for (float value : {body->solidBoxSize.x, body->solidBoxSize.y, body->solidBoxSize.z,
                    body->solidBoxDensity, body->gravityFactor, body->linearDamping,
                    body->angularDamping, body->buoyancy, body->waterDrag, body->waterAngularDrag}) {
                appendFloat(payload, value);
            }
            appendU8(payload, body->followingTransform ? 1 : 0);
            if (body->numShapes > MAX_SHAPES_3D) return false;
            appendU32(payload, static_cast<uint32_t>(body->numShapes));
            for (size_t shapeIndex = 0; shapeIndex < body->numShapes; ++shapeIndex) {
                const Shape3D& shape = body->shapes[shapeIndex];
                if (static_cast<uint8_t>(shape.type) > static_cast<uint8_t>(Shape3DType::HEIGHTFIELD)
                        || static_cast<uint8_t>(shape.source) > static_cast<uint8_t>(Shape3DSource::ENTITY_HEIGHTFIELD)
                        || shape.numVertices > MAX_SHAPE_VERTICES_3D || shape.numIndices > MAX_SHAPE_INDICES_3D
                        || !std::isfinite(shape.position.x) || !std::isfinite(shape.position.y)
                        || !std::isfinite(shape.position.z) || !std::isfinite(shape.width)
                        || !std::isfinite(shape.height) || !std::isfinite(shape.depth)
                        || !std::isfinite(shape.radius) || !std::isfinite(shape.halfHeight)
                        || !std::isfinite(shape.topRadius) || !std::isfinite(shape.bottomRadius)
                        || !std::isfinite(shape.density) || !std::isfinite(shape.friction)
                        || !std::isfinite(shape.restitution)) return false;
                appendU8(payload, static_cast<uint8_t>(shape.type));
                appendU8(payload, static_cast<uint8_t>(shape.source));
                appendU32(payload, shape.sourceEntity);
                appendU32(payload, shape.samplesSize);
                appendVector3(payload, shape.position);
                appendQuaternion(payload, shape.rotation);
                for (float value : {shape.width, shape.height, shape.depth, shape.radius,
                        shape.halfHeight, shape.topRadius, shape.bottomRadius, shape.density,
                        shape.friction, shape.restitution}) appendFloat(payload, value);
                appendU32(payload, shape.numVertices);
                for (uint16_t vertexIndex = 0; vertexIndex < shape.numVertices; ++vertexIndex) {
                    appendVector3(payload, shape.vertices[vertexIndex]);
                }
                appendU32(payload, shape.numIndices);
                for (uint16_t index = 0; index < shape.numIndices; ++index) {
                    appendU32(payload, shape.indices[index]);
                }
            }
            return payload.size() <= MAX_COMPONENT_PAYLOAD_BYTES;
        },
        [](uint16_t version, Scene* scene, Entity entity, const std::string& payload) {
            if (version != 1) return false;
            size_t offset = 0;
            uint8_t type = 0;
            uint8_t motionQuality = 0;
            uint8_t allowedDOFs = 0;
            uint32_t categoryBits = 0;
            uint32_t maskBits = 0;
            uint8_t sensor = 0;
            uint8_t overrideMass = 0;
            uint8_t followingTransform = 0;
            float values[10]{};
            if (!readU8(payload, offset, type)
                    || type > static_cast<uint8_t>(BodyType::DYNAMIC)
                    || !readU8(payload, offset, motionQuality)
                    || motionQuality > static_cast<uint8_t>(Body3DMotionQuality::LINEAR_CAST)
                    || !readU8(payload, offset, allowedDOFs)
                    || (allowedDOFs & ~static_cast<uint8_t>(Body3DAllowedDOF_All)) != 0
                    || !readU32(payload, offset, categoryBits) || categoryBits > UINT16_MAX
                    || !readU32(payload, offset, maskBits) || maskBits > UINT16_MAX
                    || !readU8(payload, offset, sensor) || sensor > 1
                    || !readU8(payload, offset, overrideMass) || overrideMass > 1) return false;
            for (float& value : values) if (!readFloat(payload, offset, value)) return false;
            if (!readU8(payload, offset, followingTransform) || followingTransform > 1) return false;
            uint32_t shapeCount = 0;
            if (!readU32(payload, offset, shapeCount) || shapeCount > MAX_SHAPES_3D) return false;
            std::vector<Shape3D> decodedShapes;
            decodedShapes.reserve(shapeCount);
            for (uint32_t shapeIndex = 0; shapeIndex < shapeCount; ++shapeIndex) {
                Shape3D shape;
                uint8_t shapeType = 0;
                uint8_t source = 0;
                uint32_t sourceEntity = 0;
                uint32_t samplesSize = 0;
                float position[3]{};
                float rotation[4]{};
                float shapeValues[10]{};
                uint32_t vertexCount = 0;
                uint32_t indexCount = 0;
                if (!readU8(payload, offset, shapeType)
                        || shapeType > static_cast<uint8_t>(Shape3DType::HEIGHTFIELD)
                        || !readU8(payload, offset, source)
                        || source > static_cast<uint8_t>(Shape3DSource::ENTITY_HEIGHTFIELD)
                        || !readU32(payload, offset, sourceEntity)
                        || !readU32(payload, offset, samplesSize)) return false;
                for (float& value : position) if (!readFloat(payload, offset, value)) return false;
                for (float& value : rotation) if (!readFloat(payload, offset, value)) return false;
                for (float& value : shapeValues) if (!readFloat(payload, offset, value)) return false;
                if (!readU32(payload, offset, vertexCount) || vertexCount > MAX_SHAPE_VERTICES_3D)
                    return false;
                shape.type = static_cast<Shape3DType>(shapeType);
                shape.source = static_cast<Shape3DSource>(source);
                shape.sourceEntity = sourceEntity;
                shape.samplesSize = samplesSize;
                shape.position = Vector3(position[0], position[1], position[2]);
                shape.rotation = Quaternion(rotation[0], rotation[1], rotation[2], rotation[3]);
                shape.width = shapeValues[0];
                shape.height = shapeValues[1];
                shape.depth = shapeValues[2];
                shape.radius = shapeValues[3];
                shape.halfHeight = shapeValues[4];
                shape.topRadius = shapeValues[5];
                shape.bottomRadius = shapeValues[6];
                shape.density = shapeValues[7];
                shape.friction = shapeValues[8];
                shape.restitution = shapeValues[9];
                shape.numVertices = static_cast<uint16_t>(vertexCount);
                for (uint32_t vertexIndex = 0; vertexIndex < vertexCount; ++vertexIndex) {
                    float vertex[3]{};
                    for (float& value : vertex) if (!readFloat(payload, offset, value)) return false;
                    shape.vertices[vertexIndex] = Vector3(vertex[0], vertex[1], vertex[2]);
                }
                if (!readU32(payload, offset, indexCount) || indexCount > MAX_SHAPE_INDICES_3D)
                    return false;
                shape.numIndices = static_cast<uint16_t>(indexCount);
                for (uint32_t index = 0; index < indexCount; ++index) {
                    uint32_t value = 0;
                    if (!readU32(payload, offset, value) || value > UINT16_MAX) return false;
                    shape.indices[index] = static_cast<uint16_t>(value);
                }
                decodedShapes.push_back(std::move(shape));
            }
            if (offset != payload.size()) return false;
            Body3DComponent* body = scene->findComponent<Body3DComponent>(entity);
            if (!body) {
                scene->addComponent<Body3DComponent>(entity);
                body = scene->findComponent<Body3DComponent>(entity);
            }
            if (!body) return false;
            body->type = static_cast<BodyType>(type);
            body->motionQuality = static_cast<Body3DMotionQuality>(motionQuality);
            body->allowedDOFs = allowedDOFs;
            body->categoryBits = static_cast<uint16_t>(categoryBits);
            body->maskBits = static_cast<uint16_t>(maskBits);
            body->sensor = sensor != 0;
            body->overrideMassProperties = overrideMass != 0;
            body->solidBoxSize = Vector3(values[0], values[1], values[2]);
            body->solidBoxDensity = values[3];
            body->gravityFactor = values[4];
            body->linearDamping = values[5];
            body->angularDamping = values[6];
            body->buoyancy = values[7];
            body->waterDrag = values[8];
            body->waterAngularDrag = values[9];
            body->followingTransform = followingTransform != 0;
            body->numShapes = decodedShapes.size();
            for (size_t shapeIndex = 0; shapeIndex < decodedShapes.size(); ++shapeIndex) {
                body->shapes[shapeIndex] = std::move(decodedShapes[shapeIndex]);
            }
            body->needReloadBody = true;
            body->needUpdateShapes = true;
            return true;
        }
    });
    (void)bodyRegistered;

    static const bool body2DRegistered = NetworkSystem::registerReplicatedComponent({
        "Body2DComponent", 1,
        [](Scene* scene, Entity entity, std::string& payload) {
            const Body2DComponent* body = scene->findComponent<Body2DComponent>(entity);
            if (!body || static_cast<uint8_t>(body->type) > static_cast<uint8_t>(BodyType::DYNAMIC)
                    || !std::isfinite(body->loadedScale.x) || !std::isfinite(body->loadedScale.y)
                    || body->numShapes > MAX_SHAPES_2D) return false;
            appendU8(payload, static_cast<uint8_t>(body->type));
            appendU8(payload, body->followingTransform ? 1 : 0);
            appendVector2(payload, body->loadedScale);
            appendU32(payload, static_cast<uint32_t>(body->numShapes));
            for (size_t shapeIndex = 0; shapeIndex < body->numShapes; ++shapeIndex) {
                const Shape2D& shape = body->shapes[shapeIndex];
                if (static_cast<uint8_t>(shape.type) > static_cast<uint8_t>(Shape2DType::CHAIN)
                        || shape.numVertices > MAX_SHAPE_POINTS_2D
                        || !std::isfinite(shape.pointA.x) || !std::isfinite(shape.pointA.y)
                        || !std::isfinite(shape.pointB.x) || !std::isfinite(shape.pointB.y)
                        || !std::isfinite(shape.radius) || !std::isfinite(shape.density)
                        || !std::isfinite(shape.friction) || !std::isfinite(shape.restitution)) return false;
                appendU8(payload, static_cast<uint8_t>(shape.type));
                appendVector2(payload, shape.pointA);
                appendVector2(payload, shape.pointB);
                appendFloat(payload, shape.radius);
                appendU8(payload, shape.loop ? 1 : 0);
                appendFloat(payload, shape.density);
                appendFloat(payload, shape.friction);
                appendFloat(payload, shape.restitution);
                appendU8(payload, shape.sensor ? 1 : 0);
                appendU8(payload, shape.enableHitEvents ? 1 : 0);
                appendU8(payload, shape.contactEvents ? 1 : 0);
                appendU8(payload, shape.preSolveEvents ? 1 : 0);
                appendU8(payload, shape.sensorEvents ? 1 : 0);
                appendU32(payload, shape.categoryBits);
                appendU32(payload, shape.maskBits);
                appendU32(payload, static_cast<uint32_t>(static_cast<int32_t>(shape.groupIndex)));
                appendU32(payload, shape.numVertices);
                for (uint8_t vertexIndex = 0; vertexIndex < shape.numVertices; ++vertexIndex) {
                    appendVector2(payload, shape.vertices[vertexIndex]);
                }
            }
            return payload.size() <= MAX_COMPONENT_PAYLOAD_BYTES;
        },
        [](uint16_t version, Scene* scene, Entity entity, const std::string& payload) {
            if (version != 1) return false;
            size_t offset = 0;
            uint8_t type = 0;
            uint8_t followingTransform = 0;
            float scale[2]{};
            uint32_t shapeCount = 0;
            if (!readU8(payload, offset, type) || type > static_cast<uint8_t>(BodyType::DYNAMIC)
                    || !readU8(payload, offset, followingTransform) || followingTransform > 1
                    || !readFloat(payload, offset, scale[0]) || !readFloat(payload, offset, scale[1])
                    || !readU32(payload, offset, shapeCount) || shapeCount > MAX_SHAPES_2D) return false;
            std::vector<Shape2D> decodedShapes;
            decodedShapes.reserve(shapeCount);
            for (uint32_t shapeIndex = 0; shapeIndex < shapeCount; ++shapeIndex) {
                Shape2D shape;
                uint8_t shapeType = 0;
                float pointA[2]{};
                float pointB[2]{};
                float radius = 0.0f;
                uint8_t flags[6]{};
                float materials[3]{};
                uint32_t categoryBits = 0;
                uint32_t maskBits = 0;
                uint32_t groupIndex = 0;
                uint32_t vertexCount = 0;
                if (!readU8(payload, offset, shapeType)
                        || shapeType > static_cast<uint8_t>(Shape2DType::CHAIN)) return false;
                for (float& value : pointA) if (!readFloat(payload, offset, value)) return false;
                for (float& value : pointB) if (!readFloat(payload, offset, value)) return false;
                if (!readFloat(payload, offset, radius) || !readU8(payload, offset, flags[0])
                        || flags[0] > 1) return false;
                for (float& value : materials) if (!readFloat(payload, offset, value)) return false;
                for (int flag = 1; flag < 6; ++flag) {
                    if (!readU8(payload, offset, flags[flag]) || flags[flag] > 1) return false;
                }
                if (!readU32(payload, offset, categoryBits) || categoryBits > UINT16_MAX
                        || !readU32(payload, offset, maskBits) || maskBits > UINT16_MAX
                        || !readU32(payload, offset, groupIndex)
                        || !readU32(payload, offset, vertexCount) || vertexCount > MAX_SHAPE_POINTS_2D) return false;
                const int32_t signedGroupIndex = static_cast<int32_t>(groupIndex);
                if (signedGroupIndex < INT16_MIN || signedGroupIndex > INT16_MAX) return false;
                shape.type = static_cast<Shape2DType>(shapeType);
                shape.pointA = Vector2(pointA[0], pointA[1]);
                shape.pointB = Vector2(pointB[0], pointB[1]);
                shape.radius = radius;
                shape.loop = flags[0] != 0;
                shape.density = materials[0];
                shape.friction = materials[1];
                shape.restitution = materials[2];
                shape.sensor = flags[1] != 0;
                shape.enableHitEvents = flags[2] != 0;
                shape.contactEvents = flags[3] != 0;
                shape.preSolveEvents = flags[4] != 0;
                shape.sensorEvents = flags[5] != 0;
                shape.categoryBits = static_cast<uint16_t>(categoryBits);
                shape.maskBits = static_cast<uint16_t>(maskBits);
                shape.groupIndex = static_cast<int16_t>(signedGroupIndex);
                shape.numVertices = static_cast<uint8_t>(vertexCount);
                for (uint32_t vertexIndex = 0; vertexIndex < vertexCount; ++vertexIndex) {
                    float vertex[2]{};
                    if (!readFloat(payload, offset, vertex[0]) || !readFloat(payload, offset, vertex[1])) return false;
                    shape.vertices[vertexIndex] = Vector2(vertex[0], vertex[1]);
                }
                decodedShapes.push_back(std::move(shape));
            }
            if (offset != payload.size()) return false;
            Body2DComponent* body = scene->findComponent<Body2DComponent>(entity);
            if (!body) {
                scene->addComponent<Body2DComponent>(entity);
                body = scene->findComponent<Body2DComponent>(entity);
            }
            if (!body) return false;
            body->type = static_cast<BodyType>(type);
            body->followingTransform = followingTransform != 0;
            body->loadedScale = Vector2(scale[0], scale[1]);
            body->numShapes = decodedShapes.size();
            for (size_t shapeIndex = 0; shapeIndex < decodedShapes.size(); ++shapeIndex) {
                body->shapes[shapeIndex] = std::move(decodedShapes[shapeIndex]);
            }
            body->needReloadBody = true;
            body->needUpdateShapes = true;
            return true;
        }
    });
    (void)body2DRegistered;

    static const bool cameraRegistered = NetworkSystem::registerReplicatedComponent({
        "CameraComponent", 1,
        [](Scene* scene, Entity entity, std::string& payload) {
            const CameraComponent* camera = scene->findComponent<CameraComponent>(entity);
            if (!camera || static_cast<uint8_t>(camera->type) > static_cast<uint8_t>(CameraType::CAMERA_PERSPECTIVE)
                    || static_cast<uint8_t>(camera->framebufferFilter) > static_cast<uint8_t>(TextureFilter::LINEAR_MIPMAP_LINEAR)
                    || camera->framebufferWidth == 0 || camera->framebufferWidth > 16384
                    || camera->framebufferHeight == 0 || camera->framebufferHeight > 16384
                    || !std::isfinite(camera->target.x) || !std::isfinite(camera->target.y)
                    || !std::isfinite(camera->target.z) || !std::isfinite(camera->up.x)
                    || !std::isfinite(camera->up.y) || !std::isfinite(camera->up.z)
                    || !std::isfinite(camera->leftClip) || !std::isfinite(camera->rightClip)
                    || !std::isfinite(camera->bottomClip) || !std::isfinite(camera->topClip)
                    || !std::isfinite(camera->yfov) || !std::isfinite(camera->aspect)
                    || !std::isfinite(camera->nearClip) || !std::isfinite(camera->farClip)) return false;
            appendU8(payload, static_cast<uint8_t>(camera->type));
            appendVector3(payload, camera->target);
            appendVector3(payload, camera->up);
            for (float value : {camera->leftClip, camera->rightClip, camera->bottomClip,
                    camera->topClip, camera->yfov, camera->aspect, camera->nearClip, camera->farClip}) {
                appendFloat(payload, value);
            }
            appendU8(payload, camera->renderToTexture ? 1 : 0);
            appendU32(payload, camera->framebufferWidth);
            appendU32(payload, camera->framebufferHeight);
            appendU8(payload, static_cast<uint8_t>(camera->framebufferFilter));
            appendU8(payload, camera->transparentSort ? 1 : 0);
            appendU8(payload, camera->depthTest ? 1 : 0);
            appendU8(payload, camera->useTarget ? 1 : 0);
            appendU8(payload, camera->autoResize ? 1 : 0);
            return payload.size() <= MAX_COMPONENT_PAYLOAD_BYTES;
        },
        [](uint16_t version, Scene* scene, Entity entity, const std::string& payload) {
            if (version != 1) return false;
            size_t offset = 0;
            uint8_t type = 0;
            float target[3]{};
            float up[3]{};
            float clipValues[8]{};
            uint8_t renderToTexture = 0;
            uint32_t framebufferWidth = 0;
            uint32_t framebufferHeight = 0;
            uint8_t filter = 0;
            uint8_t flags[4]{};
            if (!readU8(payload, offset, type)
                    || type > static_cast<uint8_t>(CameraType::CAMERA_PERSPECTIVE)) return false;
            for (float& value : target) if (!readFloat(payload, offset, value)) return false;
            for (float& value : up) if (!readFloat(payload, offset, value)) return false;
            for (float& value : clipValues) if (!readFloat(payload, offset, value)) return false;
            if (!readU8(payload, offset, renderToTexture) || renderToTexture > 1
                    || !readU32(payload, offset, framebufferWidth) || framebufferWidth == 0
                    || framebufferWidth > 16384 || !readU32(payload, offset, framebufferHeight)
                    || framebufferHeight == 0 || framebufferHeight > 16384
                    || !readU8(payload, offset, filter)
                    || filter > static_cast<uint8_t>(TextureFilter::LINEAR_MIPMAP_LINEAR)) return false;
            for (uint8_t& flag : flags) {
                if (!readU8(payload, offset, flag) || flag > 1) return false;
            }
            if (offset != payload.size()) return false;
            CameraComponent* camera = scene->findComponent<CameraComponent>(entity);
            if (!camera) {
                scene->addComponent<CameraComponent>(entity);
                camera = scene->findComponent<CameraComponent>(entity);
            }
            if (!camera) return false;
            camera->type = static_cast<CameraType>(type);
            camera->target = Vector3(target[0], target[1], target[2]);
            camera->up = Vector3(up[0], up[1], up[2]);
            camera->leftClip = clipValues[0];
            camera->rightClip = clipValues[1];
            camera->bottomClip = clipValues[2];
            camera->topClip = clipValues[3];
            camera->yfov = clipValues[4];
            camera->aspect = clipValues[5];
            camera->nearClip = clipValues[6];
            camera->farClip = clipValues[7];
            camera->renderToTexture = renderToTexture != 0;
            camera->framebufferWidth = framebufferWidth;
            camera->framebufferHeight = framebufferHeight;
            camera->framebufferFilter = static_cast<TextureFilter>(filter);
            camera->transparentSort = flags[0] != 0;
            camera->depthTest = flags[1] != 0;
            camera->useTarget = flags[2] != 0;
            camera->autoResize = flags[3] != 0;
            camera->needUpdate = true;
            return true;
        }
    });
    (void)cameraRegistered;

    static const bool fogRegistered = NetworkSystem::registerReplicatedComponent({
        "FogComponent", 2,
        [](Scene* scene, Entity entity, std::string& payload) {
            const FogComponent* fog = scene->findComponent<FogComponent>(entity);
            if (!fog || static_cast<uint8_t>(fog->type) > static_cast<uint8_t>(FogType::EXPONENTIALSQUARED)) return false;
            appendU8(payload, static_cast<uint8_t>(fog->type));
            appendVector3(payload, fog->color);
            for (float value : {fog->density, fog->linearStart, fog->linearEnd}) appendFloat(payload, value);
            appendU8(payload, fog->heightFog ? 1 : 0);
            for (float value : {fog->fogHeightFalloff, fog->fogGroundHeight, fog->fogSunAmount,
                    fog->fogFarRange, fog->fogScatteringG}) appendFloat(payload, value);
            appendVector3(payload, fog->fogSunColor);
            appendU8(payload, fog->fogNoiseEnabled ? 1 : 0);
            for (float value : {fog->fogNoiseScale, fog->fogNoiseStrength, fog->fogNoiseSpeed}) appendFloat(payload, value);
            appendVector2(payload, fog->fogNoiseWind);
            appendU8(payload, fog->godRays ? 1 : 0);
            for (float value : {fog->rayIntensity, fog->rayThreshold, fog->raySoftness,
                    fog->rayDecay, fog->rayDensity}) appendFloat(payload, value);
            appendU8(payload, fog->cloudsEnabled ? 1 : 0);
            for (float value : {fog->cloudBaseHeight, fog->cloudThickness, fog->cloudCoverage,
                    fog->cloudDensity, fog->cloudScale, fog->cloudSpeed, fog->cloudSunAmount}) appendFloat(payload, value);
            appendVector2(payload, fog->cloudWind);
            appendVector3(payload, fog->cloudColor);
            appendU8(payload, fog->volumetricFog ? 1 : 0);
            appendU32(payload, fog->volumetricFogSteps);
            appendFloat(payload, fog->volumetricFogIntensity);
            return payload.size() <= MAX_COMPONENT_PAYLOAD_BYTES;
        },
        [](uint16_t version, Scene* scene, Entity entity, const std::string& payload) {
            if (version != 1 && version != 2) return false;
            size_t offset = 0;
            uint8_t type = 0;
            float color[3]{};
            float basic[3]{};
            uint8_t heightFog = 0;
            float heightValues[5]{};
            float sunColor[3]{};
            uint8_t noiseEnabled = 0;
            float noiseValues[3]{};
            float noiseWind[2]{};
            uint8_t godRays = 0;
            float rayValues[5]{};
            uint8_t cloudsEnabled = 0;
            float cloudValues[7]{};
            float cloudWind[2]{};
            float cloudColor[3]{};
            uint8_t volumetricFog = 0;
            uint32_t volumetricFogSteps = 32;
            float volumetricFogIntensity = 1.0f;
            if (!readU8(payload, offset, type)
                    || type > static_cast<uint8_t>(FogType::EXPONENTIALSQUARED)) return false;
            for (float& value : color) if (!readFloat(payload, offset, value)) return false;
            for (float& value : basic) if (!readFloat(payload, offset, value)) return false;
            if (!readU8(payload, offset, heightFog) || heightFog > 1) return false;
            for (float& value : heightValues) if (!readFloat(payload, offset, value)) return false;
            for (float& value : sunColor) if (!readFloat(payload, offset, value)) return false;
            if (!readU8(payload, offset, noiseEnabled) || noiseEnabled > 1) return false;
            for (float& value : noiseValues) if (!readFloat(payload, offset, value)) return false;
            for (float& value : noiseWind) if (!readFloat(payload, offset, value)) return false;
            if (!readU8(payload, offset, godRays) || godRays > 1) return false;
            for (float& value : rayValues) if (!readFloat(payload, offset, value)) return false;
            if (!readU8(payload, offset, cloudsEnabled) || cloudsEnabled > 1) return false;
            for (float& value : cloudValues) if (!readFloat(payload, offset, value)) return false;
            for (float& value : cloudWind) if (!readFloat(payload, offset, value)) return false;
            for (float& value : cloudColor) if (!readFloat(payload, offset, value)) return false;
            if (version >= 2){
                if (!readU8(payload, offset, volumetricFog) || volumetricFog > 1
                        || !readU32(payload, offset, volumetricFogSteps)
                        || !readFloat(payload, offset, volumetricFogIntensity)) return false;
            }
            if (offset != payload.size()) return false;
            FogComponent* fog = scene->findComponent<FogComponent>(entity);
            if (!fog) {
                scene->addComponent<FogComponent>(entity);
                fog = scene->findComponent<FogComponent>(entity);
            }
            if (!fog) return false;
            fog->type = static_cast<FogType>(type);
            fog->color = Vector3(color[0], color[1], color[2]);
            fog->density = basic[0];
            fog->linearStart = basic[1];
            fog->linearEnd = basic[2];
            fog->heightFog = heightFog != 0;
            fog->fogHeightFalloff = heightValues[0];
            fog->fogGroundHeight = heightValues[1];
            fog->fogSunAmount = heightValues[2];
            fog->fogFarRange = heightValues[3];
            fog->fogScatteringG = heightValues[4];
            fog->fogSunColor = Vector3(sunColor[0], sunColor[1], sunColor[2]);
            fog->fogNoiseEnabled = noiseEnabled != 0;
            fog->fogNoiseScale = noiseValues[0];
            fog->fogNoiseStrength = noiseValues[1];
            fog->fogNoiseSpeed = noiseValues[2];
            fog->fogNoiseWind = Vector2(noiseWind[0], noiseWind[1]);
            fog->godRays = godRays != 0;
            fog->rayIntensity = rayValues[0];
            fog->rayThreshold = rayValues[1];
            fog->raySoftness = rayValues[2];
            fog->rayDecay = rayValues[3];
            fog->rayDensity = rayValues[4];
            fog->cloudsEnabled = cloudsEnabled != 0;
            fog->cloudBaseHeight = cloudValues[0];
            fog->cloudThickness = cloudValues[1];
            fog->cloudCoverage = cloudValues[2];
            fog->cloudDensity = cloudValues[3];
            fog->cloudScale = cloudValues[4];
            fog->cloudSpeed = cloudValues[5];
            fog->cloudSunAmount = cloudValues[6];
            fog->cloudWind = Vector2(cloudWind[0], cloudWind[1]);
            fog->cloudColor = Vector3(cloudColor[0], cloudColor[1], cloudColor[2]);
            fog->volumetricFog = volumetricFog != 0;
            fog->volumetricFogSteps = std::clamp(volumetricFogSteps, 4u, 64u);
            fog->volumetricFogIntensity = std::max(volumetricFogIntensity, 0.0f);
            return true;
        }
    });
    (void)fogRegistered;

    static const bool reflectionProbeRegistered = NetworkSystem::registerReplicatedComponent({
        "ReflectionProbeComponent", 1,
        [](Scene* scene, Entity entity, std::string& payload) {
            const ReflectionProbeComponent* probe = scene->findComponent<ReflectionProbeComponent>(entity);
            if (!probe || static_cast<uint8_t>(probe->mode) > static_cast<uint8_t>(ReflectionProbeMode::DYNAMIC)
                    || static_cast<uint8_t>(probe->updateMode) > static_cast<uint8_t>(ReflectionProbeUpdateMode::MANUAL)
                    || !std::isfinite(probe->boxOffset.x) || !std::isfinite(probe->boxOffset.y)
                    || !std::isfinite(probe->boxOffset.z) || !std::isfinite(probe->boxSize.x)
                    || !std::isfinite(probe->boxSize.y) || !std::isfinite(probe->boxSize.z)
                    || !std::isfinite(probe->blendDistance) || !std::isfinite(probe->intensity)
                    || !std::isfinite(probe->nearClip) || !std::isfinite(probe->farClip)
                    || !std::isfinite(probe->updateInterval) || probe->resolution == 0
                    || probe->resolution > 4096) return false;
            appendU8(payload, static_cast<uint8_t>(probe->mode));
            appendU8(payload, static_cast<uint8_t>(probe->updateMode));
            appendVector3(payload, probe->boxOffset);
            appendVector3(payload, probe->boxSize);
            for (float value : {probe->blendDistance, probe->intensity, probe->nearClip,
                    probe->farClip, probe->updateInterval}) appendFloat(payload, value);
            appendU32(payload, probe->priority);
            appendU32(payload, probe->resolution);
            appendU8(payload, probe->includeSky ? 1 : 0);
            return payload.size() <= MAX_COMPONENT_PAYLOAD_BYTES;
        },
        [](uint16_t version, Scene* scene, Entity entity, const std::string& payload) {
            if (version != 1) return false;
            size_t offset = 0;
            uint8_t mode = 0;
            uint8_t updateMode = 0;
            float boxOffset[3]{};
            float boxSize[3]{};
            float values[5]{};
            uint32_t priority = 0;
            uint32_t resolution = 0;
            uint8_t includeSky = 0;
            if (!readU8(payload, offset, mode) || mode > static_cast<uint8_t>(ReflectionProbeMode::DYNAMIC)
                    || !readU8(payload, offset, updateMode)
                    || updateMode > static_cast<uint8_t>(ReflectionProbeUpdateMode::MANUAL)) return false;
            for (float& value : boxOffset) if (!readFloat(payload, offset, value)) return false;
            for (float& value : boxSize) if (!readFloat(payload, offset, value)) return false;
            for (float& value : values) if (!readFloat(payload, offset, value)) return false;
            if (!readU32(payload, offset, priority) || !readU32(payload, offset, resolution)
                    || resolution == 0 || resolution > 4096
                    || !readU8(payload, offset, includeSky) || includeSky > 1
                    || offset != payload.size()) return false;
            ReflectionProbeComponent* probe = scene->findComponent<ReflectionProbeComponent>(entity);
            if (!probe) {
                scene->addComponent<ReflectionProbeComponent>(entity);
                probe = scene->findComponent<ReflectionProbeComponent>(entity);
            }
            if (!probe) return false;
            probe->mode = static_cast<ReflectionProbeMode>(mode);
            probe->updateMode = static_cast<ReflectionProbeUpdateMode>(updateMode);
            probe->boxOffset = Vector3(boxOffset[0], boxOffset[1], boxOffset[2]);
            probe->boxSize = Vector3(boxSize[0], boxSize[1], boxSize[2]);
            probe->blendDistance = values[0];
            probe->intensity = values[1];
            probe->nearClip = values[2];
            probe->farClip = values[3];
            probe->updateInterval = values[4];
            probe->priority = static_cast<int>(priority);
            probe->resolution = resolution;
            probe->includeSky = includeSky != 0;
            probe->needUpdate = true;
            return true;
        }
    });
    (void)reflectionProbeRegistered;

    static const bool waterRegistered = NetworkSystem::registerReplicatedComponent({
        "WaterComponent", 1,
        [](Scene* scene, Entity entity, std::string& payload) {
            const WaterComponent* water = scene->findComponent<WaterComponent>(entity);
            if (!water || water->customShader.size() > MAX_COMPONENT_NAME_BYTES * 8
                    || water->customUnderwaterShader.size() > MAX_COMPONENT_NAME_BYTES * 8
                    || water->subdivisions == 0 || water->subdivisions > MAX_WATER_SUBDIVISIONS
                    || water->simResolution < MIN_WATER_SIM || water->simResolution > MAX_WATER_SIM) return false;
            appendString(payload, water->customShader);
            appendString(payload, water->customUnderwaterShader);
            appendVector2(payload, water->size);
            appendU32(payload, water->subdivisions);
            appendVector3(payload, water->shallowColor);
            appendVector3(payload, water->deepColor);
            for (float value : {water->depthFade, water->waveHeight, water->waveLength, water->waveSpeed,
                    water->waveDirection, water->waveSteepness, water->windInfluence, water->fetch,
                    water->normalScale, water->normalStrength, water->rippleSpeed, water->reflectivity,
                    water->specularIntensity, water->roughness}) appendFloat(payload, value);
            appendVector3(payload, water->foamColor);
            for (float value : {water->shoreFoam, water->crestFoam}) appendFloat(payload, value);
            appendU8(payload, water->receiveShadows ? 1 : 0);
            appendU8(payload, water->depthEffects ? 1 : 0);
            appendU8(payload, water->planarReflection ? 1 : 0);
            appendFloat(payload, water->reflectionDistortion);
            appendU8(payload, water->refraction ? 1 : 0);
            appendFloat(payload, water->refractionDistortion);
            appendFloat(payload, water->caustics);
            appendFloat(payload, water->causticsScale);
            appendU8(payload, water->underwater ? 1 : 0);
            appendU8(payload, water->buoyancy ? 1 : 0);
            appendFloat(payload, water->buoyancyDepth);
            appendU8(payload, water->simulate ? 1 : 0);
            appendU32(payload, water->simResolution);
            for (float value : {water->simDepth, water->simFlow, water->simRain,
                    water->simEvaporation, water->simDryDepth}) appendFloat(payload, value);
            appendU8(payload, water->simEdgeDrain ? 1 : 0);
            appendU8(payload, water->simRipples ? 1 : 0);
            for (float value : {water->simRippleSpeed, water->simRippleDamping, water->simRippleStrength}) {
                appendFloat(payload, value);
            }
            return payload.size() <= MAX_COMPONENT_PAYLOAD_BYTES;
        },
        [](uint16_t version, Scene* scene, Entity entity, const std::string& payload) {
            if (version != 1) return false;
            size_t offset = 0;
            std::string customShader;
            std::string customUnderwaterShader;
            float size[2]{};
            uint32_t subdivisions = 0;
            float shallowColor[3]{};
            float deepColor[3]{};
            float waveValues[14]{};
            float foamColor[3]{};
            float foamValues[2]{};
            uint8_t surfaceFlags[3]{};
            float reflectionDistortion = 0.0f;
            uint8_t refraction = 0;
            float refractionDistortion = 0.0f;
            float caustics = 0.0f;
            float causticsScale = 0.0f;
            uint8_t underwater = 0;
            uint8_t buoyancy = 0;
            float buoyancyDepth = 0.0f;
            uint8_t simulate = 0;
            uint32_t simResolution = 0;
            float simValues[5]{};
            uint8_t simFlags[2]{};
            float rippleValues[3]{};
            if (!readBoundedString(payload, offset, MAX_COMPONENT_NAME_BYTES * 8, customShader)
                    || !readBoundedString(payload, offset, MAX_COMPONENT_NAME_BYTES * 8, customUnderwaterShader)
                    || !readFloat(payload, offset, size[0]) || !readFloat(payload, offset, size[1])
                    || !readU32(payload, offset, subdivisions) || subdivisions == 0
                    || subdivisions > MAX_WATER_SUBDIVISIONS) return false;
            for (float& value : shallowColor) if (!readFloat(payload, offset, value)) return false;
            for (float& value : deepColor) if (!readFloat(payload, offset, value)) return false;
            for (float& value : waveValues) if (!readFloat(payload, offset, value)) return false;
            for (float& value : foamColor) if (!readFloat(payload, offset, value)) return false;
            for (float& value : foamValues) if (!readFloat(payload, offset, value)) return false;
            for (uint8_t& flag : surfaceFlags) {
                if (!readU8(payload, offset, flag) || flag > 1) return false;
            }
            if (!readFloat(payload, offset, reflectionDistortion)
                    || !readU8(payload, offset, refraction) || refraction > 1
                    || !readFloat(payload, offset, refractionDistortion)
                    || !readFloat(payload, offset, caustics) || !readFloat(payload, offset, causticsScale)
                    || !readU8(payload, offset, underwater) || underwater > 1
                    || !readU8(payload, offset, buoyancy) || buoyancy > 1
                    || !readFloat(payload, offset, buoyancyDepth)
                    || !readU8(payload, offset, simulate) || simulate > 1
                    || !readU32(payload, offset, simResolution)
                    || simResolution < MIN_WATER_SIM || simResolution > MAX_WATER_SIM) return false;
            for (float& value : simValues) if (!readFloat(payload, offset, value)) return false;
            for (uint8_t& flag : simFlags) {
                if (!readU8(payload, offset, flag) || flag > 1) return false;
            }
            for (float& value : rippleValues) if (!readFloat(payload, offset, value)) return false;
            if (offset != payload.size()) return false;
            WaterComponent* water = scene->findComponent<WaterComponent>(entity);
            if (!water) {
                scene->addComponent<WaterComponent>(entity);
                water = scene->findComponent<WaterComponent>(entity);
            }
            if (!water) return false;
            water->customShader = std::move(customShader);
            water->customUnderwaterShader = std::move(customUnderwaterShader);
            water->size = Vector2(size[0], size[1]);
            water->subdivisions = subdivisions;
            water->shallowColor = Vector3(shallowColor[0], shallowColor[1], shallowColor[2]);
            water->deepColor = Vector3(deepColor[0], deepColor[1], deepColor[2]);
            water->depthFade = waveValues[0];
            water->waveHeight = waveValues[1];
            water->waveLength = waveValues[2];
            water->waveSpeed = waveValues[3];
            water->waveDirection = waveValues[4];
            water->waveSteepness = waveValues[5];
            water->windInfluence = waveValues[6];
            water->fetch = waveValues[7];
            water->normalScale = waveValues[8];
            water->normalStrength = waveValues[9];
            water->rippleSpeed = waveValues[10];
            water->reflectivity = waveValues[11];
            water->specularIntensity = waveValues[12];
            water->roughness = waveValues[13];
            water->foamColor = Vector3(foamColor[0], foamColor[1], foamColor[2]);
            water->shoreFoam = foamValues[0];
            water->crestFoam = foamValues[1];
            water->receiveShadows = surfaceFlags[0] != 0;
            water->depthEffects = surfaceFlags[1] != 0;
            water->planarReflection = surfaceFlags[2] != 0;
            water->reflectionDistortion = reflectionDistortion;
            water->refraction = refraction != 0;
            water->refractionDistortion = refractionDistortion;
            water->caustics = caustics;
            water->causticsScale = causticsScale;
            water->underwater = underwater != 0;
            water->buoyancy = buoyancy != 0;
            water->buoyancyDepth = buoyancyDepth;
            water->simulate = simulate != 0;
            water->simResolution = simResolution;
            water->simDepth = simValues[0];
            water->simFlow = simValues[1];
            water->simRain = simValues[2];
            water->simEvaporation = simValues[3];
            water->simDryDepth = simValues[4];
            water->simEdgeDrain = simFlags[0] != 0;
            water->simRipples = simFlags[1] != 0;
            water->simRippleSpeed = rippleValues[0];
            water->simRippleDamping = rippleValues[1];
            water->simRippleStrength = rippleValues[2];
            water->needReload = true;
            water->needUpdateTexture = true;
            return true;
        }
    });
    (void)waterRegistered;

    static const bool decalRegistered = NetworkSystem::registerReplicatedComponent({
        "DecalComponent", 1,
        [](Scene* scene, Entity entity, std::string& payload) {
            const DecalComponent* decal = scene->findComponent<DecalComponent>(entity);
            const std::string textureId = decal ? decal->texture.getId() : std::string();
            const std::string texturePath = decal ? decal->texture.getPath() : std::string();
            if (!decal || textureId.size() > MAX_COMPONENT_NAME_BYTES * 8
                    || texturePath.size() > MAX_COMPONENT_NAME_BYTES * 8
                    || !std::isfinite(decal->color.x) || !std::isfinite(decal->color.y)
                    || !std::isfinite(decal->color.z) || !std::isfinite(decal->size.x)
                    || !std::isfinite(decal->size.y) || !std::isfinite(decal->size.z)
                    || !std::isfinite(decal->opacity) || !std::isfinite(decal->depthFade)) return false;
            appendU8(payload, decal->enabled ? 1 : 0);
            appendVector3(payload, decal->color);
            appendVector3(payload, decal->size);
            appendFloat(payload, decal->opacity);
            appendFloat(payload, decal->depthFade);
            appendString(payload, textureId);
            appendString(payload, texturePath);
            return payload.size() <= MAX_COMPONENT_PAYLOAD_BYTES;
        },
        [](uint16_t version, Scene* scene, Entity entity, const std::string& payload) {
            if (version != 1) return false;
            size_t offset = 0;
            uint8_t enabled = 0;
            float color[3]{};
            float size[3]{};
            float opacity = 0.0f;
            float depthFade = 0.0f;
            std::string textureId;
            std::string texturePath;
            if (!readU8(payload, offset, enabled) || enabled > 1) return false;
            for (float& value : color) if (!readFloat(payload, offset, value)) return false;
            for (float& value : size) if (!readFloat(payload, offset, value)) return false;
            if (!readFloat(payload, offset, opacity) || !readFloat(payload, offset, depthFade)
                    || !readBoundedString(payload, offset, MAX_COMPONENT_NAME_BYTES * 8, textureId)
                    || !readBoundedString(payload, offset, MAX_COMPONENT_NAME_BYTES * 8, texturePath)
                    || offset != payload.size()) return false;
            DecalComponent* decal = scene->findComponent<DecalComponent>(entity);
            if (!decal) {
                scene->addComponent<DecalComponent>(entity);
                decal = scene->findComponent<DecalComponent>(entity);
            }
            if (!decal) return false;
            decal->enabled = enabled != 0;
            decal->color = Vector3(color[0], color[1], color[2]);
            decal->size = Vector3(size[0], size[1], size[2]);
            decal->opacity = opacity;
            decal->depthFade = depthFade;
            if (!texturePath.empty()) decal->texture.setPath(texturePath);
            else if (!textureId.empty()) decal->texture.setId(textureId);
            return true;
        }
    });
    (void)decalRegistered;

    static const bool light2DRegistered = NetworkSystem::registerReplicatedComponent({
        "Light2DComponent", 1,
        [](Scene* scene, Entity entity, std::string& payload) {
            const Light2DComponent* light = scene->findComponent<Light2DComponent>(entity);
            if (!light || !std::isfinite(light->color.x) || !std::isfinite(light->color.y)
                    || !std::isfinite(light->color.z) || !std::isfinite(light->intensity)
                    || !std::isfinite(light->range) || !std::isfinite(light->falloff)
                    || !std::isfinite(light->height) || !std::isfinite(light->shadowBias)
                    || !std::isfinite(light->shadowSoftness) || light->mapResolution == 0
                    || light->mapResolution > 16384) return false;
            appendVector3(payload, light->color);
            for (float value : {light->intensity, light->range, light->falloff, light->height,
                    light->shadowBias, light->shadowSoftness}) appendFloat(payload, value);
            appendU8(payload, light->shadows ? 1 : 0);
            appendU32(payload, light->mapResolution);
            return payload.size() <= MAX_COMPONENT_PAYLOAD_BYTES;
        },
        [](uint16_t version, Scene* scene, Entity entity, const std::string& payload) {
            if (version != 1) return false;
            size_t offset = 0;
            float color[3]{};
            float values[6]{};
            uint8_t shadows = 0;
            uint32_t mapResolution = 0;
            for (float& value : color) if (!readFloat(payload, offset, value)) return false;
            for (float& value : values) if (!readFloat(payload, offset, value)) return false;
            if (!readU8(payload, offset, shadows) || shadows > 1
                    || !readU32(payload, offset, mapResolution) || mapResolution == 0
                    || mapResolution > 16384 || offset != payload.size()) return false;
            Light2DComponent* light = scene->findComponent<Light2DComponent>(entity);
            if (!light) {
                scene->addComponent<Light2DComponent>(entity);
                light = scene->findComponent<Light2DComponent>(entity);
            }
            if (!light) return false;
            light->color = Vector3(color[0], color[1], color[2]);
            light->intensity = values[0];
            light->range = values[1];
            light->falloff = values[2];
            light->height = values[3];
            light->shadowBias = values[4];
            light->shadowSoftness = values[5];
            light->shadows = shadows != 0;
            light->mapResolution = mapResolution;
            light->shadowMapIndex = -1;
            return true;
        }
    });
    (void)light2DRegistered;

    static const bool skyRegistered = NetworkSystem::registerReplicatedComponent({
        "SkyComponent", 1,
        [](Scene* scene, Entity entity, std::string& payload) {
            const SkyComponent* sky = scene->findComponent<SkyComponent>(entity);
            const std::string textureId = sky ? sky->texture.getId() : std::string();
            const std::string texturePath = sky ? sky->texture.getPath() : std::string();
            if (!sky || sky->customShader.size() > MAX_COMPONENT_NAME_BYTES * 8
                    || textureId.size() > MAX_COMPONENT_NAME_BYTES * 8
                    || texturePath.size() > MAX_COMPONENT_NAME_BYTES * 8
                    || !std::isfinite(sky->color.x) || !std::isfinite(sky->color.y)
                    || !std::isfinite(sky->color.z) || !std::isfinite(sky->color.w)
                    || !std::isfinite(sky->rotation)) return false;
            appendString(payload, sky->customShader);
            appendString(payload, textureId);
            appendString(payload, texturePath);
            appendFloat(payload, sky->color.x);
            appendFloat(payload, sky->color.y);
            appendFloat(payload, sky->color.z);
            appendFloat(payload, sky->color.w);
            appendFloat(payload, sky->rotation);
            appendU8(payload, sky->visible ? 1 : 0);
            return payload.size() <= MAX_COMPONENT_PAYLOAD_BYTES;
        },
        [](uint16_t version, Scene* scene, Entity entity, const std::string& payload) {
            if (version != 1) return false;
            size_t offset = 0;
            std::string customShader;
            std::string textureId;
            std::string texturePath;
            float color[4]{};
            float rotation = 0.0f;
            uint8_t visible = 0;
            if (!readBoundedString(payload, offset, MAX_COMPONENT_NAME_BYTES * 8, customShader)
                    || !readBoundedString(payload, offset, MAX_COMPONENT_NAME_BYTES * 8, textureId)
                    || !readBoundedString(payload, offset, MAX_COMPONENT_NAME_BYTES * 8, texturePath)) return false;
            for (float& value : color) if (!readFloat(payload, offset, value)) return false;
            if (!readFloat(payload, offset, rotation) || !readU8(payload, offset, visible)
                    || visible > 1 || offset != payload.size()) return false;
            SkyComponent* sky = scene->findComponent<SkyComponent>(entity);
            if (!sky) {
                scene->addComponent<SkyComponent>(entity);
                sky = scene->findComponent<SkyComponent>(entity);
            }
            if (!sky) return false;
            sky->customShader = std::move(customShader);
            sky->color = Vector4(color[0], color[1], color[2], color[3]);
            sky->rotation = rotation;
            sky->visible = visible != 0;
            if (!texturePath.empty()) sky->texture.setPath(texturePath);
            else if (!textureId.empty()) sky->texture.setId(textureId);
            sky->needUpdateEnvironment = true;
            sky->needUpdateTexture = true;
            sky->needUpdateSky = true;
            return true;
        }
    });
    (void)skyRegistered;

    static const bool mirrorRegistered = NetworkSystem::registerReplicatedComponent({
        "MirrorComponent", 1,
        [](Scene* scene, Entity entity, std::string& payload) {
            const MirrorComponent* mirror = scene->findComponent<MirrorComponent>(entity);
            if (!mirror || !std::isfinite(mirror->normal.x) || !std::isfinite(mirror->normal.y)
                    || !std::isfinite(mirror->normal.z)) return false;
            appendVector3(payload, mirror->normal);
            return true;
        },
        [](uint16_t version, Scene* scene, Entity entity, const std::string& payload) {
            if (version != 1 || payload.size() != sizeof(float) * 3) return false;
            size_t offset = 0;
            float normal[3]{};
            for (float& value : normal) if (!readFloat(payload, offset, value)) return false;
            MirrorComponent* mirror = scene->findComponent<MirrorComponent>(entity);
            if (!mirror) {
                scene->addComponent<MirrorComponent>(entity);
                mirror = scene->findComponent<MirrorComponent>(entity);
            }
            if (!mirror) return false;
            mirror->normal = Vector3(normal[0], normal[1], normal[2]);
            return true;
        }
    });
    (void)mirrorRegistered;

    static const bool occluderRegistered = NetworkSystem::registerReplicatedComponent({
        "Occluder2DComponent", 1,
        [](Scene* scene, Entity entity, std::string& payload) {
            const Occluder2DComponent* occluder = scene->findComponent<Occluder2DComponent>(entity);
            if (!occluder || static_cast<uint8_t>(occluder->shape) > static_cast<uint8_t>(Occluder2DShape::POLYGON)
                    || occluder->points.size() > 4096) return false;
            appendU8(payload, static_cast<uint8_t>(occluder->shape));
            appendU8(payload, occluder->closed ? 1 : 0);
            appendU8(payload, occluder->enabled ? 1 : 0);
            appendU32(payload, static_cast<uint32_t>(occluder->points.size()));
            for (const Vector2& point : occluder->points) {
                if (!std::isfinite(point.x) || !std::isfinite(point.y)) return false;
                appendVector2(payload, point);
            }
            return payload.size() <= MAX_COMPONENT_PAYLOAD_BYTES;
        },
        [](uint16_t version, Scene* scene, Entity entity, const std::string& payload) {
            if (version != 1) return false;
            size_t offset = 0;
            uint8_t shape = 0;
            uint8_t closed = 0;
            uint8_t enabled = 0;
            uint32_t pointCount = 0;
            if (!readU8(payload, offset, shape)
                    || shape > static_cast<uint8_t>(Occluder2DShape::POLYGON)
                    || !readU8(payload, offset, closed) || closed > 1
                    || !readU8(payload, offset, enabled) || enabled > 1
                    || !readU32(payload, offset, pointCount) || pointCount > 4096) return false;
            std::vector<Vector2> points;
            points.reserve(pointCount);
            for (uint32_t index = 0; index < pointCount; ++index) {
                float point[2]{};
                if (!readFloat(payload, offset, point[0]) || !readFloat(payload, offset, point[1])) return false;
                points.emplace_back(point[0], point[1]);
            }
            if (offset != payload.size()) return false;
            Occluder2DComponent* occluder = scene->findComponent<Occluder2DComponent>(entity);
            if (!occluder) {
                scene->addComponent<Occluder2DComponent>(entity);
                occluder = scene->findComponent<Occluder2DComponent>(entity);
            }
            if (!occluder) return false;
            occluder->shape = static_cast<Occluder2DShape>(shape);
            occluder->closed = closed != 0;
            occluder->enabled = enabled != 0;
            occluder->points = std::move(points);
            return true;
        }
    });
    (void)occluderRegistered;

    static const bool waterExclusionRegistered = NetworkSystem::registerReplicatedComponent({
        "WaterExclusionComponent", 1,
        [](Scene* scene, Entity entity, std::string& payload) {
            const WaterExclusionComponent* exclusion = scene->findComponent<WaterExclusionComponent>(entity);
            if (!exclusion || static_cast<uint8_t>(exclusion->shape) > static_cast<uint8_t>(WaterExclusionShape::HULL)
                    || !std::isfinite(exclusion->center.x) || !std::isfinite(exclusion->center.y)
                    || !std::isfinite(exclusion->center.z) || !std::isfinite(exclusion->size.x)
                    || !std::isfinite(exclusion->size.y) || !std::isfinite(exclusion->size.z)) return false;
            appendU8(payload, static_cast<uint8_t>(exclusion->shape));
            appendVector3(payload, exclusion->center);
            appendVector3(payload, exclusion->size);
            return payload.size() <= MAX_COMPONENT_PAYLOAD_BYTES;
        },
        [](uint16_t version, Scene* scene, Entity entity, const std::string& payload) {
            if (version != 1 || payload.size() != 1 + sizeof(float) * 6) return false;
            size_t offset = 0;
            uint8_t shape = 0;
            float center[3]{};
            float size[3]{};
            if (!readU8(payload, offset, shape)
                    || shape > static_cast<uint8_t>(WaterExclusionShape::HULL)) return false;
            for (float& value : center) if (!readFloat(payload, offset, value)) return false;
            for (float& value : size) if (!readFloat(payload, offset, value)) return false;
            WaterExclusionComponent* exclusion = scene->findComponent<WaterExclusionComponent>(entity);
            if (!exclusion) {
                scene->addComponent<WaterExclusionComponent>(entity);
                exclusion = scene->findComponent<WaterExclusionComponent>(entity);
            }
            if (!exclusion) return false;
            exclusion->shape = static_cast<WaterExclusionShape>(shape);
            exclusion->center = Vector3(center[0], center[1], center[2]);
            exclusion->size = Vector3(size[0], size[1], size[2]);
            exclusion->hullPlanes.clear();
            exclusion->hullPoints.clear();
            exclusion->hullMap.clear();
            exclusion->hullSignature = 0;
            exclusion->hullLayer = -1;
            return true;
        }
    });
    (void)waterExclusionRegistered;

    static const bool autoColliderRegistered = NetworkSystem::registerReplicatedComponent({
        "AutoColliderComponent", 1,
        [](Scene* scene, Entity entity, std::string& payload) {
            const AutoColliderComponent* collider = scene->findComponent<AutoColliderComponent>(entity);
            if (!collider || static_cast<uint8_t>(collider->type) > static_cast<uint8_t>(AutoColliderType::MESH_AUTO)
                    || !std::isfinite(collider->radius) || !std::isfinite(collider->width)
                    || !std::isfinite(collider->height) || !std::isfinite(collider->depth)
                    || !std::isfinite(collider->halfHeight) || !std::isfinite(collider->center.x)
                    || !std::isfinite(collider->center.y) || !std::isfinite(collider->center.z)
                    || !std::isfinite(collider->density)) return false;
            appendU8(payload, static_cast<uint8_t>(collider->type));
            appendFloat(payload, collider->radius);
            appendFloat(payload, collider->width);
            appendFloat(payload, collider->height);
            appendFloat(payload, collider->depth);
            appendFloat(payload, collider->halfHeight);
            appendVector3(payload, collider->center);
            appendU8(payload, collider->sensor ? 1 : 0);
            appendFloat(payload, collider->density);
            appendU32(payload, collider->categoryBits);
            appendU32(payload, collider->maskBits);
            appendU32(payload, collider->sourceEntity);
            appendU8(payload, collider->needUpdate ? 1 : 0);
            return payload.size() <= MAX_COMPONENT_PAYLOAD_BYTES;
        },
        [](uint16_t version, Scene* scene, Entity entity, const std::string& payload) {
            if (version != 1 || payload.size() != 51) return false;
            size_t offset = 0;
            uint8_t type = 0;
            float values[5]{};
            float center[3]{};
            uint8_t sensor = 0;
            float density = 0.0f;
            uint32_t categoryBits = 0;
            uint32_t maskBits = 0;
            uint32_t sourceEntity = 0;
            uint8_t needUpdate = 0;
            if (!readU8(payload, offset, type) || type > static_cast<uint8_t>(AutoColliderType::MESH_AUTO)) return false;
            for (float& value : values) if (!readFloat(payload, offset, value)) return false;
            for (float& value : center) if (!readFloat(payload, offset, value)) return false;
            if (!readU8(payload, offset, sensor) || sensor > 1 || !readFloat(payload, offset, density)
                    || !readU32(payload, offset, categoryBits) || !readU32(payload, offset, maskBits)
                    || !readU32(payload, offset, sourceEntity) || !readU8(payload, offset, needUpdate)
                    || needUpdate > 1 || categoryBits > 0xFFFFu || maskBits > 0xFFFFu
                    || offset != payload.size()) return false;
            AutoColliderComponent* collider = scene->findComponent<AutoColliderComponent>(entity);
            if (!collider) {
                scene->addComponent<AutoColliderComponent>(entity);
                collider = scene->findComponent<AutoColliderComponent>(entity);
            }
            if (!collider) return false;
            collider->type = static_cast<AutoColliderType>(type);
            collider->radius = values[0];
            collider->width = values[1];
            collider->height = values[2];
            collider->depth = values[3];
            collider->halfHeight = values[4];
            collider->center = Vector3(center[0], center[1], center[2]);
            collider->sensor = sensor != 0;
            collider->density = density;
            collider->categoryBits = static_cast<uint16_t>(categoryBits);
            collider->maskBits = static_cast<uint16_t>(maskBits);
            collider->sourceEntity = static_cast<Entity>(sourceEntity);
            collider->needUpdate = needUpdate != 0;
            return true;
        }
    });
    (void)autoColliderRegistered;

    static const bool scriptRegistered = NetworkSystem::registerReplicatedComponent({
        "ScriptComponent", 1,
        [](Scene* scene, Entity entity, std::string& payload) {
            const ScriptComponent* scripts = scene->findComponent<ScriptComponent>(entity);
            if (!scripts || scripts->scripts.size() > MAX_SCRIPT_ENTRIES) return false;
            appendU32(payload, static_cast<uint32_t>(scripts->scripts.size()));
            for (const ScriptEntry& script : scripts->scripts) {
                if (static_cast<uint8_t>(script.type) > static_cast<uint8_t>(ScriptType::GRAPH)
                        || script.path.size() > MAX_SCRIPT_PROPERTY_BYTES
                        || script.headerPath.size() > MAX_SCRIPT_PROPERTY_BYTES
                        || script.className.size() > MAX_SCRIPT_PROPERTY_BYTES
                        || script.properties.size() > MAX_SCRIPT_PROPERTIES) return false;
                appendU8(payload, static_cast<uint8_t>(script.type));
                appendString(payload, script.path);
                appendString(payload, script.headerPath);
                appendString(payload, script.className);
                appendU8(payload, script.enabled ? 1 : 0);
                appendU32(payload, static_cast<uint32_t>(script.properties.size()));
                for (const ScriptProperty& property : script.properties) {
                    if (property.name.size() > MAX_SCRIPT_PROPERTY_BYTES
                            || property.displayName.size() > MAX_SCRIPT_PROPERTY_BYTES
                            || property.ptrTypeName.size() > MAX_SCRIPT_PROPERTY_BYTES
                            || static_cast<uint8_t>(property.type) > static_cast<uint8_t>(ScriptPropertyType::EntityReference)) return false;
                    appendString(payload, property.name);
                    appendString(payload, property.displayName);
                    appendString(payload, property.ptrTypeName);
                    appendU8(payload, static_cast<uint8_t>(property.type));
                    if (!appendScriptPropertyValue(payload, property.type, property.value)
                            || !appendScriptPropertyValue(payload, property.type, property.defaultValue)) return false;
                }
            }
            return payload.size() <= MAX_COMPONENT_PAYLOAD_BYTES;
        },
        [](uint16_t version, Scene* scene, Entity entity, const std::string& payload) {
            if (version != 1) return false;
            size_t offset = 0;
            uint32_t scriptCount = 0;
            if (!readU32(payload, offset, scriptCount) || scriptCount > MAX_SCRIPT_ENTRIES) return false;
            std::vector<ScriptEntry> scripts;
            scripts.reserve(scriptCount);
            for (uint32_t scriptIndex = 0; scriptIndex < scriptCount; ++scriptIndex) {
                uint8_t type = 0;
                uint8_t enabled = 0;
                uint32_t propertyCount = 0;
                ScriptEntry script;
                if (!readU8(payload, offset, type) || type > static_cast<uint8_t>(ScriptType::GRAPH)
                        || !readBoundedString(payload, offset, MAX_SCRIPT_PROPERTY_BYTES, script.path)
                        || !readBoundedString(payload, offset, MAX_SCRIPT_PROPERTY_BYTES, script.headerPath)
                        || !readBoundedString(payload, offset, MAX_SCRIPT_PROPERTY_BYTES, script.className)
                        || !readU8(payload, offset, enabled) || enabled > 1
                        || !readU32(payload, offset, propertyCount) || propertyCount > MAX_SCRIPT_PROPERTIES) return false;
                script.type = static_cast<ScriptType>(type);
                script.enabled = enabled != 0;
                script.properties.reserve(propertyCount);
                for (uint32_t propertyIndex = 0; propertyIndex < propertyCount; ++propertyIndex) {
                    uint8_t propertyType = 0;
                    ScriptProperty property;
                    if (!readBoundedString(payload, offset, MAX_SCRIPT_PROPERTY_BYTES, property.name)
                            || !readBoundedString(payload, offset, MAX_SCRIPT_PROPERTY_BYTES, property.displayName)
                            || !readBoundedString(payload, offset, MAX_SCRIPT_PROPERTY_BYTES, property.ptrTypeName)
                            || !readU8(payload, offset, propertyType)
                            || propertyType > static_cast<uint8_t>(ScriptPropertyType::EntityReference)
                            || !readScriptPropertyValue(payload, offset,
                                static_cast<ScriptPropertyType>(propertyType), property.value)
                            || !readScriptPropertyValue(payload, offset,
                                static_cast<ScriptPropertyType>(propertyType), property.defaultValue)) return false;
                    property.type = static_cast<ScriptPropertyType>(propertyType);
                    script.properties.push_back(std::move(property));
                }
                scripts.push_back(std::move(script));
            }
            if (offset != payload.size()) return false;
            ScriptComponent* component = scene->findComponent<ScriptComponent>(entity);
            if (!component) {
                scene->addComponent<ScriptComponent>(entity);
                component = scene->findComponent<ScriptComponent>(entity);
            }
            if (!component) return false;
            component->scripts = std::move(scripts);
            return true;
        }
    });
    (void)scriptRegistered;

    static const bool modelRegistered = NetworkSystem::registerReplicatedComponent({
        "ModelComponent", 1,
        [](Scene* scene, Entity entity, std::string& payload) {
            const ModelComponent* model = scene->findComponent<ModelComponent>(entity);
            if (!model || model->filename.size() > MAX_SCRIPT_PROPERTY_BYTES) return false;
            appendString(payload, model->filename);
            appendU8(payload, model->mergeStaticMeshes ? 1 : 0);
            return payload.size() <= MAX_COMPONENT_PAYLOAD_BYTES;
        },
        [](uint16_t version, Scene* scene, Entity entity, const std::string& payload) {
            if (version != 1) return false;
            size_t offset = 0;
            std::string filename;
            uint8_t mergeStaticMeshes = 0;
            if (!readBoundedString(payload, offset, MAX_SCRIPT_PROPERTY_BYTES, filename)
                    || !readU8(payload, offset, mergeStaticMeshes) || mergeStaticMeshes > 1
                    || offset != payload.size()) return false;
            ModelComponent* model = scene->findComponent<ModelComponent>(entity);
            if (!model) {
                scene->addComponent<ModelComponent>(entity);
                model = scene->findComponent<ModelComponent>(entity);
            }
            if (!model) return false;
            model->filename = std::move(filename);
            model->mergeStaticMeshes = mergeStaticMeshes != 0;
            model->gltfModel.reset();
            model->objModel.reset();
            model->skeleton = 0;
            model->bonesNameMapping.clear();
            model->bonesIdMapping.clear();
            model->meshNodesMapping.clear();
            model->nodesIdMapping.clear();
            model->skinBindings.clear();
            model->morphNameMapping.clear();
            model->animations.clear();
            model->loadedFilename.clear();
            model->loadedMergeStaticMeshes = false;
            model->needUpdateModel = true;
            return true;
        }
    });
    (void)modelRegistered;

    static const bool animationRegistered = NetworkSystem::registerReplicatedComponent({
        "AnimationComponent", 1,
        [](Scene* scene, Entity entity, std::string& payload) {
            const AnimationComponent* animation = scene->findComponent<AnimationComponent>(entity);
            if (!animation || animation->actions.size() > 256 || !std::isfinite(animation->duration)
                    || !std::isfinite(animation->defaultFadeTime)) return false;
            appendU32(payload, static_cast<uint32_t>(animation->actions.size()));
            for (const ActionFrame& action : animation->actions) {
                if (!std::isfinite(action.startTime) || !std::isfinite(action.duration)) return false;
                appendFloat(payload, action.startTime);
                appendFloat(payload, action.duration);
                appendU32(payload, action.action);
                appendU32(payload, action.track);
                appendU8(payload, action.started ? 1 : 0);
            }
            appendU8(payload, animation->ownedActions ? 1 : 0);
            appendU8(payload, animation->loop ? 1 : 0);
            appendFloat(payload, animation->duration);
            appendFloat(payload, animation->defaultFadeTime);
            return payload.size() <= MAX_COMPONENT_PAYLOAD_BYTES;
        },
        [](uint16_t version, Scene* scene, Entity entity, const std::string& payload) {
            if (version != 1) return false;
            size_t offset = 0;
            uint32_t actionCount = 0;
            if (!readU32(payload, offset, actionCount) || actionCount > 256) return false;
            std::vector<ActionFrame> actions;
            actions.reserve(actionCount);
            for (uint32_t index = 0; index < actionCount; ++index) {
                ActionFrame action;
                uint32_t actionEntity = 0;
                uint8_t started = 0;
                if (!readFloat(payload, offset, action.startTime) || !readFloat(payload, offset, action.duration)
                        || !readU32(payload, offset, actionEntity) || !readU32(payload, offset, action.track)
                        || !readU8(payload, offset, started) || started > 1
                        || !std::isfinite(action.startTime) || !std::isfinite(action.duration)) return false;
                action.action = static_cast<Entity>(actionEntity);
                action.started = started != 0;
                actions.push_back(action);
            }
            uint8_t ownedActions = 0;
            uint8_t loop = 0;
            float duration = 0.0f;
            float defaultFadeTime = 0.0f;
            if (!readU8(payload, offset, ownedActions) || ownedActions > 1
                    || !readU8(payload, offset, loop) || loop > 1
                    || !readFloat(payload, offset, duration) || !readFloat(payload, offset, defaultFadeTime)
                    || !std::isfinite(duration) || !std::isfinite(defaultFadeTime)
                    || offset != payload.size()) return false;
            AnimationComponent* animation = scene->findComponent<AnimationComponent>(entity);
            if (!animation) {
                scene->addComponent<AnimationComponent>(entity);
                animation = scene->findComponent<AnimationComponent>(entity);
            }
            if (!animation) return false;
            animation->actions = std::move(actions);
            animation->ownedActions = ownedActions != 0;
            animation->loop = loop != 0;
            animation->duration = duration;
            animation->defaultFadeTime = defaultFadeTime;
            animation->weight = 1.0f;
            animation->fadeTarget = 1.0f;
            animation->fadeSpeed = 0.0f;
            animation->stopOnFadeOut = false;
            animation->blendLayer = 0;
            animation->blendLayerWeight = 1.0f;
            animation->blendLayerAdditive = false;
            return true;
        }
    });
    (void)animationRegistered;

    static const bool bundleRegistered = NetworkSystem::registerReplicatedComponent({
        "BundleComponent", 1,
        [](Scene* scene, Entity entity, std::string& payload) {
            const BundleComponent* bundle = scene->findComponent<BundleComponent>(entity);
            if (!bundle || bundle->name.size() > MAX_COMPONENT_NAME_BYTES * 8
                    || bundle->path.size() > MAX_SCRIPT_PROPERTY_BYTES) return false;
            appendString(payload, bundle->name);
            appendString(payload, bundle->path);
            return payload.size() <= MAX_COMPONENT_PAYLOAD_BYTES;
        },
        [](uint16_t version, Scene* scene, Entity entity, const std::string& payload) {
            if (version != 1) return false;
            size_t offset = 0;
            BundleComponent* bundle = scene->findComponent<BundleComponent>(entity);
            if (!bundle) {
                scene->addComponent<BundleComponent>(entity);
                bundle = scene->findComponent<BundleComponent>(entity);
            }
            if (!bundle || !readBoundedString(payload, offset, MAX_COMPONENT_NAME_BYTES * 8, bundle->name)
                    || !readBoundedString(payload, offset, MAX_SCRIPT_PROPERTY_BYTES, bundle->path)
                    || offset != payload.size()) return false;
            return true;
        }
    });
    (void)bundleRegistered;

    static const bool instancedMeshRegistered = NetworkSystem::registerReplicatedComponent({
        "InstancedMeshComponent", 1,
        [](Scene* scene, Entity entity, std::string& payload) {
            const InstancedMeshComponent* instanced = scene->findComponent<InstancedMeshComponent>(entity);
            if (!instanced || instanced->instances.size() > 4096 || instanced->maxInstances > 4096
                    || !std::isfinite(instanced->cullDistance) || !std::isfinite(instanced->fadeStart)
                    || !std::isfinite(instanced->fadeEnd) || !std::isfinite(instanced->cullNearDistance)
                    || !std::isfinite(instanced->viewSettings.lodBias)) return false;
            appendU32(payload, static_cast<uint32_t>(instanced->instances.size()));
            for (const InstanceData& instance : instanced->instances) {
                if (!std::isfinite(instance.position.x) || !std::isfinite(instance.position.y)
                        || !std::isfinite(instance.position.z) || !std::isfinite(instance.rotation.w)
                        || !std::isfinite(instance.rotation.x) || !std::isfinite(instance.rotation.y)
                        || !std::isfinite(instance.rotation.z) || !std::isfinite(instance.scale.x)
                        || !std::isfinite(instance.scale.y) || !std::isfinite(instance.scale.z)
                        || !std::isfinite(instance.color.x) || !std::isfinite(instance.color.y)
                        || !std::isfinite(instance.color.z) || !std::isfinite(instance.color.w)
                        || !std::isfinite(instance.textureRect[0]) || !std::isfinite(instance.textureRect[1])
                        || !std::isfinite(instance.textureRect[2]) || !std::isfinite(instance.textureRect[3])) return false;
                appendVector3(payload, instance.position);
                appendQuaternion(payload, instance.rotation);
                appendVector3(payload, instance.scale);
                appendFloat(payload, instance.color.x);
                appendFloat(payload, instance.color.y);
                appendFloat(payload, instance.color.z);
                appendFloat(payload, instance.color.w);
                for (int index = 0; index < 4; ++index) appendFloat(payload, instance.textureRect[index]);
                appendU8(payload, instance.visible ? 1 : 0);
            }
            appendU32(payload, instanced->maxInstances);
            appendU8(payload, instanced->cullInstances ? 1 : 0);
            appendFloat(payload, instanced->cullDistance);
            appendU8(payload, instanced->distanceFade ? 1 : 0);
            appendFloat(payload, instanced->fadeStart);
            appendFloat(payload, instanced->fadeEnd);
            appendFloat(payload, instanced->cullNearDistance);
            appendU8(payload, instanced->instancedBillboard ? 1 : 0);
            appendU8(payload, instanced->instancedCylindricalBillboard ? 1 : 0);
            appendU8(payload, instanced->viewSettings.cull ? 1 : 0);
            appendU8(payload, instanced->viewSettings.castShadows ? 1 : 0);
            appendU8(payload, instanced->viewSettings.lodEnabled ? 1 : 0);
            appendFloat(payload, instanced->viewSettings.lodBias);
            return payload.size() <= MAX_COMPONENT_PAYLOAD_BYTES;
        },
        [](uint16_t version, Scene* scene, Entity entity, const std::string& payload) {
            if (version != 1) return false;
            size_t offset = 0;
            uint32_t instanceCount = 0;
            if (!readU32(payload, offset, instanceCount) || instanceCount > 4096) return false;
            std::vector<InstanceData> instances;
            instances.reserve(instanceCount);
            for (uint32_t index = 0; index < instanceCount; ++index) {
                InstanceData instance;
                float color[4]{};
                float rect[4]{};
                uint8_t visible = 0;
                if (!readVector3(payload, offset, instance.position)
                        || !readQuaternion(payload, offset, instance.rotation)
                        || !readVector3(payload, offset, instance.scale)
                        || !readFloat(payload, offset, color[0]) || !readFloat(payload, offset, color[1])
                        || !readFloat(payload, offset, color[2]) || !readFloat(payload, offset, color[3])) return false;
                for (float& value : rect) if (!readFloat(payload, offset, value)) return false;
                if (!readU8(payload, offset, visible) || visible > 1) return false;
                instance.color = Vector4(color[0], color[1], color[2], color[3]);
                instance.textureRect.setRect(rect[0], rect[1], rect[2], rect[3]);
                instance.visible = visible != 0;
                instances.push_back(instance);
            }
            uint32_t maxInstances = 0;
            uint8_t cullInstances = 0, distanceFade = 0, billboard = 0, cylindrical = 0;
            uint8_t cull = 0, castShadows = 0, lodEnabled = 0;
            float cullDistance = 0.0f, fadeStart = 0.0f, fadeEnd = 0.0f, cullNearDistance = 0.0f, lodBias = 0.0f;
            if (!readU32(payload, offset, maxInstances) || maxInstances > 4096
                    || !readU8(payload, offset, cullInstances) || cullInstances > 1
                    || !readFloat(payload, offset, cullDistance) || !readU8(payload, offset, distanceFade) || distanceFade > 1
                    || !readFloat(payload, offset, fadeStart) || !readFloat(payload, offset, fadeEnd)
                    || !readFloat(payload, offset, cullNearDistance)
                    || !readU8(payload, offset, billboard) || billboard > 1
                    || !readU8(payload, offset, cylindrical) || cylindrical > 1
                    || !readU8(payload, offset, cull) || cull > 1
                    || !readU8(payload, offset, castShadows) || castShadows > 1
                    || !readU8(payload, offset, lodEnabled) || lodEnabled > 1
                    || !readFloat(payload, offset, lodBias) || !std::isfinite(cullDistance)
                    || !std::isfinite(fadeStart) || !std::isfinite(fadeEnd)
                    || !std::isfinite(cullNearDistance) || !std::isfinite(lodBias)
                    || offset != payload.size()) return false;
            InstancedMeshComponent* instanced = scene->findComponent<InstancedMeshComponent>(entity);
            if (!instanced) {
                scene->addComponent<InstancedMeshComponent>(entity);
                instanced = scene->findComponent<InstancedMeshComponent>(entity);
            }
            if (!instanced) return false;
            instanced->instances = std::move(instances);
            instanced->maxInstances = maxInstances;
            instanced->cullInstances = cullInstances != 0;
            instanced->cullDistance = cullDistance;
            instanced->distanceFade = distanceFade != 0;
            instanced->fadeStart = fadeStart;
            instanced->fadeEnd = fadeEnd;
            instanced->cullNearDistance = cullNearDistance;
            instanced->instancedBillboard = billboard != 0;
            instanced->instancedCylindricalBillboard = cylindrical != 0;
            instanced->viewSettings.cull = cull != 0;
            instanced->viewSettings.castShadows = castShadows != 0;
            instanced->viewSettings.lodEnabled = lodEnabled != 0;
            instanced->viewSettings.lodBias = lodBias;
            instanced->renderInstances.clear();
            instanced->renderBounds.clear();
            instanced->numVisible = 0;
            instanced->needUpdateBuffer = true;
            instanced->needUpdateInstances = true;
            return true;
        }
    });
    (void)instancedMeshRegistered;

    static const bool terrainRegistered = NetworkSystem::registerReplicatedComponent({
        "TerrainComponent", 1,
        [](Scene* scene, Entity entity, std::string& payload) {
            const TerrainComponent* terrain = scene->findComponent<TerrainComponent>(entity);
            if (!terrain || terrain->blendMaps.size() > MAX_TERRAIN_BLENDMAPS
                    || terrain->surfaceLayers.size() > MAX_TERRAIN_LAYERS
                    || terrain->foliageLayers.size() > MAX_TERRAIN_LAYERS
                    || terrain->rootGridSize < 1 || terrain->rootGridSize > MAX_TERRAINGRID
                    || terrain->levels < 1 || terrain->levels > MAX_TERRAIN_LEVELS
                    || !std::isfinite(terrain->offset.x) || !std::isfinite(terrain->offset.y)
                    || !std::isfinite(terrain->terrainSize) || !std::isfinite(terrain->maxHeight)
                    || !std::isfinite(terrain->resolution) || !std::isfinite(terrain->textureBaseTiles)
                    || !std::isfinite(terrain->textureDetailTiles)) return false;
            if (!appendTerrainTexture(payload, terrain->heightMap)) return false;
            appendU32(payload, static_cast<uint32_t>(terrain->blendMaps.size()));
            for (const Texture& texture : terrain->blendMaps) if (!appendTerrainTexture(payload, texture)) return false;
            appendU32(payload, static_cast<uint32_t>(terrain->surfaceLayers.size()));
            for (const TerrainSurfaceLayer& layer : terrain->surfaceLayers) {
                if (!std::isfinite(layer.colorFactor.x) || !std::isfinite(layer.colorFactor.y)
                        || !std::isfinite(layer.colorFactor.z) || !std::isfinite(layer.colorFactor.w)
                        || !std::isfinite(layer.normalStrength) || !std::isfinite(layer.roughnessFactor)
                        || !std::isfinite(layer.metallicFactor) || !std::isfinite(layer.occlusionStrength)
                        || !std::isfinite(layer.uvScale.x) || !std::isfinite(layer.uvScale.y)
                        || !std::isfinite(layer.uvOffset.x) || !std::isfinite(layer.uvOffset.y)) return false;
                appendU8(payload, layer.pbr ? 1 : 0);
                if (!appendTerrainTexture(payload, layer.colorTexture)
                        || !appendTerrainTexture(payload, layer.normalTexture)
                        || !appendTerrainTexture(payload, layer.roughnessTexture)
                        || !appendTerrainTexture(payload, layer.metallicTexture)
                        || !appendTerrainTexture(payload, layer.occlusionTexture)
                        || !appendTerrainTexture(payload, layer.heightTexture)) return false;
                appendFloat(payload, layer.colorFactor.x);
                appendFloat(payload, layer.colorFactor.y);
                appendFloat(payload, layer.colorFactor.z);
                appendFloat(payload, layer.colorFactor.w);
                appendFloat(payload, layer.normalStrength);
                appendFloat(payload, layer.roughnessFactor);
                appendFloat(payload, layer.metallicFactor);
                appendFloat(payload, layer.occlusionStrength);
                appendVector2(payload, layer.uvScale);
                appendVector2(payload, layer.uvOffset);
            }
            appendU32(payload, static_cast<uint32_t>(terrain->foliageLayers.size()));
            for (const TerrainFoliageLayer& layer : terrain->foliageLayers) {
                if (layer.meshPath.size() > MAX_SCRIPT_PROPERTY_BYTES
                        || !std::isfinite(layer.density) || !std::isfinite(layer.minScale)
                        || !std::isfinite(layer.maxScale) || !std::isfinite(layer.rotationJitter)
                        || !std::isfinite(layer.alignToNormal) || !std::isfinite(layer.minSlope)
                        || !std::isfinite(layer.maxSlope) || !std::isfinite(layer.minHeight)
                        || !std::isfinite(layer.maxHeight) || !std::isfinite(layer.drawDistance)
                        || !std::isfinite(layer.minDrawDistance)) return false;
                appendString(payload, layer.meshPath);
                if (!appendTerrainTexture(payload, layer.densityMap)) return false;
                appendFloat(payload, layer.density);
                appendFloat(payload, layer.minScale);
                appendFloat(payload, layer.maxScale);
                appendFloat(payload, layer.rotationJitter);
                appendFloat(payload, layer.alignToNormal);
                appendFloat(payload, layer.minSlope);
                appendFloat(payload, layer.maxSlope);
                appendFloat(payload, layer.minHeight);
                appendFloat(payload, layer.maxHeight);
                appendFloat(payload, layer.drawDistance);
                appendFloat(payload, layer.minDrawDistance);
                appendU32(payload, layer.seed);
                appendU8(payload, layer.castShadows ? 1 : 0);
            }
            appendU8(payload, terrain->autoSetRanges ? 1 : 0);
            appendVector2(payload, terrain->offset);
            appendFloat(payload, terrain->terrainSize);
            appendFloat(payload, terrain->maxHeight);
            appendFloat(payload, terrain->resolution);
            appendFloat(payload, terrain->textureBaseTiles);
            appendFloat(payload, terrain->textureDetailTiles);
            appendU32(payload, static_cast<uint32_t>(terrain->rootGridSize));
            appendU32(payload, static_cast<uint32_t>(terrain->levels));
            return payload.size() <= MAX_COMPONENT_PAYLOAD_BYTES;
        },
        [](uint16_t version, Scene* scene, Entity entity, const std::string& payload) {
            if (version != 1) return false;
            size_t offset = 0;
            TerrainComponent* terrain = scene->findComponent<TerrainComponent>(entity);
            if (!terrain) {
                scene->addComponent<TerrainComponent>(entity);
                terrain = scene->findComponent<TerrainComponent>(entity);
            }
            if (!terrain) return false;
            TerrainComponent decoded;
            uint32_t count = 0;
            uint8_t autoSetRanges = 0;
            if (!readTerrainTexture(payload, offset, decoded.heightMap)
                    || !readU32(payload, offset, count) || count > MAX_TERRAIN_BLENDMAPS) return false;
            decoded.blendMaps.resize(count);
            for (Texture& texture : decoded.blendMaps)
                if (!readTerrainTexture(payload, offset, texture)) return false;
            if (!readU32(payload, offset, count) || count > MAX_TERRAIN_LAYERS) return false;
            decoded.surfaceLayers.resize(count);
            for (TerrainSurfaceLayer& layer : decoded.surfaceLayers) {
                uint8_t pbr = 0;
                if (!readU8(payload, offset, pbr) || pbr > 1
                        || !readTerrainTexture(payload, offset, layer.colorTexture)
                        || !readTerrainTexture(payload, offset, layer.normalTexture)
                        || !readTerrainTexture(payload, offset, layer.roughnessTexture)
                        || !readTerrainTexture(payload, offset, layer.metallicTexture)
                        || !readTerrainTexture(payload, offset, layer.occlusionTexture)
                        || !readTerrainTexture(payload, offset, layer.heightTexture)
                        || !readVector4(payload, offset, layer.colorFactor)
                        || !readFloat(payload, offset, layer.normalStrength)
                        || !readFloat(payload, offset, layer.roughnessFactor)
                        || !readFloat(payload, offset, layer.metallicFactor)
                        || !readFloat(payload, offset, layer.occlusionStrength)
                        || !readVector2(payload, offset, layer.uvScale)
                        || !readVector2(payload, offset, layer.uvOffset)) return false;
                layer.pbr = pbr != 0;
            }
            if (!readU32(payload, offset, count) || count > MAX_TERRAIN_LAYERS) return false;
            decoded.foliageLayers.resize(count);
            for (TerrainFoliageLayer& layer : decoded.foliageLayers) {
                uint8_t castShadows = 0;
                if (!readBoundedString(payload, offset, MAX_SCRIPT_PROPERTY_BYTES, layer.meshPath)
                        || !readTerrainTexture(payload, offset, layer.densityMap)
                        || !readFloat(payload, offset, layer.density) || !readFloat(payload, offset, layer.minScale)
                        || !readFloat(payload, offset, layer.maxScale) || !readFloat(payload, offset, layer.rotationJitter)
                        || !readFloat(payload, offset, layer.alignToNormal) || !readFloat(payload, offset, layer.minSlope)
                        || !readFloat(payload, offset, layer.maxSlope) || !readFloat(payload, offset, layer.minHeight)
                        || !readFloat(payload, offset, layer.maxHeight) || !readFloat(payload, offset, layer.drawDistance)
                        || !readFloat(payload, offset, layer.minDrawDistance) || !readU32(payload, offset, layer.seed)
                        || !readU8(payload, offset, castShadows) || castShadows > 1) return false;
                layer.castShadows = castShadows != 0;
            }
            uint32_t rootGridSize = 0, levels = 0;
            if (!readU8(payload, offset, autoSetRanges) || autoSetRanges > 1
                    || !readVector2(payload, offset, decoded.offset)
                    || !readFloat(payload, offset, decoded.terrainSize) || !readFloat(payload, offset, decoded.maxHeight)
                    || !readFloat(payload, offset, decoded.resolution) || !readFloat(payload, offset, decoded.textureBaseTiles)
                    || !readFloat(payload, offset, decoded.textureDetailTiles)
                    || !readU32(payload, offset, rootGridSize) || !readU32(payload, offset, levels)
                    || rootGridSize < 1 || rootGridSize > MAX_TERRAINGRID || levels < 1 || levels > MAX_TERRAIN_LEVELS
                    || !std::isfinite(decoded.offset.x) || !std::isfinite(decoded.offset.y)
                    || !std::isfinite(decoded.terrainSize) || !std::isfinite(decoded.maxHeight)
                    || !std::isfinite(decoded.resolution) || !std::isfinite(decoded.textureBaseTiles)
                    || !std::isfinite(decoded.textureDetailTiles) || offset != payload.size()) return false;
            decoded.autoSetRanges = autoSetRanges != 0;
            decoded.rootGridSize = static_cast<int>(rootGridSize);
            decoded.levels = static_cast<int>(levels);
            decoded.heightMapLoaded = false;
            decoded.ranges.clear();
            decoded.nodes.clear();
            decoded.numNodes = 0;
            for (size_t& value : decoded.grid) value = 0;
            decoded.needUpdateTerrain = true;
            decoded.needUpdateTexture = true;
            decoded.needUpdateFoliage = true;
            decoded.foliageDirtyLayer = -1;
            decoded.dirtyFoliageDensity = TerrainMapRegion();
            decoded.needUpdateRanges = false;
            decoded.dirtyHeightMap = TerrainMapRegion();
            *terrain = std::move(decoded);
            return true;
        }
    });
    (void)terrainRegistered;

    static const bool spriteRegistered = NetworkSystem::registerReplicatedComponent({
        "SpriteComponent", 1,
        [](Scene* scene, Entity entity, std::string& payload) {
            const SpriteComponent* sprite = scene->findComponent<SpriteComponent>(entity);
            if (!sprite || sprite->numFramesRect > MAX_SPRITE_FRAMES
                    || !std::isfinite(static_cast<float>(sprite->textureScaleFactor))) return false;
            appendU32(payload, sprite->width);
            appendU32(payload, sprite->height);
            appendU8(payload, sprite->automaticFlipY ? 1 : 0);
            appendU8(payload, sprite->flipY ? 1 : 0);
            appendFloat(payload, static_cast<float>(sprite->textureScaleFactor));
            appendU32(payload, sprite->numFramesRect);
            for (unsigned int index = 0; index < sprite->numFramesRect; ++index) {
                const SpriteFrameData& frame = sprite->framesRect[index];
                if (frame.name.size() > MAX_SCRIPT_PROPERTY_BYTES
                        || !std::isfinite(frame.rect[0]) || !std::isfinite(frame.rect[1])
                        || !std::isfinite(frame.rect[2]) || !std::isfinite(frame.rect[3])) return false;
                appendString(payload, frame.name);
                for (int component = 0; component < 4; ++component) appendFloat(payload, frame.rect[component]);
            }
            if (static_cast<uint8_t>(sprite->pivotPreset) > static_cast<uint8_t>(PivotPreset::BOTTOM_RIGHT)
                    || !std::isfinite(sprite->pendingFrameRect[0]) || !std::isfinite(sprite->pendingFrameRect[1])
                    || !std::isfinite(sprite->pendingFrameRect[2]) || !std::isfinite(sprite->pendingFrameRect[3])) return false;
            appendU8(payload, static_cast<uint8_t>(sprite->pivotPreset));
            for (int component = 0; component < 4; ++component) appendFloat(payload, sprite->pendingFrameRect[component]);
            appendU8(payload, sprite->needUpdateFrameRect ? 1 : 0);
            return payload.size() <= MAX_COMPONENT_PAYLOAD_BYTES;
        },
        [](uint16_t version, Scene* scene, Entity entity, const std::string& payload) {
            if (version != 1) return false;
            size_t offset = 0;
            uint32_t width = 0, height = 0, frameCount = 0;
            uint8_t automaticFlipY = 0, flipY = 0;
            float textureScaleFactor = 0.0f;
            if (!readU32(payload, offset, width) || !readU32(payload, offset, height)
                    || !readU8(payload, offset, automaticFlipY) || automaticFlipY > 1
                    || !readU8(payload, offset, flipY) || flipY > 1
                    || !readFloat(payload, offset, textureScaleFactor) || !std::isfinite(textureScaleFactor)
                    || !readU32(payload, offset, frameCount) || frameCount > MAX_SPRITE_FRAMES) return false;
            SpriteComponent decoded;
            decoded.width = width;
            decoded.height = height;
            decoded.automaticFlipY = automaticFlipY != 0;
            decoded.flipY = flipY != 0;
            decoded.textureScaleFactor = textureScaleFactor;
            decoded.numFramesRect = frameCount;
            for (uint32_t index = 0; index < frameCount; ++index) {
                SpriteFrameData& frame = decoded.framesRect[index];
                if (!readBoundedString(payload, offset, MAX_SCRIPT_PROPERTY_BYTES, frame.name)
                        || !readFloat(payload, offset, frame.rect[0]) || !readFloat(payload, offset, frame.rect[1])
                        || !readFloat(payload, offset, frame.rect[2]) || !readFloat(payload, offset, frame.rect[3])) return false;
            }
            uint8_t pivot = 0, needUpdateFrameRect = 0;
            if (!readU8(payload, offset, pivot) || pivot > static_cast<uint8_t>(PivotPreset::BOTTOM_RIGHT)
                    || !readFloat(payload, offset, decoded.pendingFrameRect[0])
                    || !readFloat(payload, offset, decoded.pendingFrameRect[1])
                    || !readFloat(payload, offset, decoded.pendingFrameRect[2])
                    || !readFloat(payload, offset, decoded.pendingFrameRect[3])
                    || !readU8(payload, offset, needUpdateFrameRect) || needUpdateFrameRect > 1
                    || offset != payload.size()) return false;
            decoded.pivotPreset = static_cast<PivotPreset>(pivot);
            decoded.needUpdateFrameRect = needUpdateFrameRect != 0;
            decoded.needUpdateSprite = true;
            SpriteComponent* sprite = scene->findComponent<SpriteComponent>(entity);
            if (!sprite) {
                scene->addComponent<SpriteComponent>(entity);
                sprite = scene->findComponent<SpriteComponent>(entity);
            }
            if (!sprite) return false;
            *sprite = std::move(decoded);
            return true;
        }
    });
    (void)spriteRegistered;

    static const bool spriteAnimationRegistered = NetworkSystem::registerReplicatedComponent({
        "SpriteAnimationComponent", 1,
        [](Scene* scene, Entity entity, std::string& payload) {
            const SpriteAnimationComponent* animation = scene->findComponent<SpriteAnimationComponent>(entity);
            if (!animation || animation->framesTimeSize > MAX_SPRITE_FRAMES || animation->framesSize > MAX_SPRITE_FRAMES
                    || animation->frameIndex < 0 || animation->frameTimeIndex < 0) return false;
            appendU8(payload, animation->loop ? 1 : 0);
            appendU32(payload, animation->framesTimeSize);
            for (unsigned int index = 0; index < animation->framesTimeSize; ++index) {
                if (animation->framesTime[index] < 0) return false;
                appendU32(payload, static_cast<uint32_t>(animation->framesTime[index]));
            }
            appendU32(payload, animation->framesSize);
            for (unsigned int index = 0; index < animation->framesSize; ++index) {
                if (animation->frames[index] < 0) return false;
                appendU32(payload, static_cast<uint32_t>(animation->frames[index]));
            }
            appendU32(payload, static_cast<uint32_t>(animation->frameIndex));
            appendU32(payload, static_cast<uint32_t>(animation->frameTimeIndex));
            return payload.size() <= MAX_COMPONENT_PAYLOAD_BYTES;
        },
        [](uint16_t version, Scene* scene, Entity entity, const std::string& payload) {
            if (version != 1) return false;
            size_t offset = 0;
            uint8_t loop = 0;
            uint32_t framesTimeSize = 0, framesSize = 0;
            if (!readU8(payload, offset, loop) || loop > 1
                    || !readU32(payload, offset, framesTimeSize) || framesTimeSize > MAX_SPRITE_FRAMES) return false;
            SpriteAnimationComponent decoded;
            decoded.loop = loop != 0;
            decoded.framesTimeSize = framesTimeSize;
            for (uint32_t index = 0; index < framesTimeSize; ++index) {
                uint32_t value = 0;
                if (!readU32(payload, offset, value) || value > static_cast<uint32_t>(INT32_MAX)) return false;
                decoded.framesTime[index] = static_cast<int>(value);
            }
            if (!readU32(payload, offset, framesSize) || framesSize > MAX_SPRITE_FRAMES) return false;
            decoded.framesSize = framesSize;
            for (uint32_t index = 0; index < framesSize; ++index) {
                uint32_t value = 0;
                if (!readU32(payload, offset, value) || value > static_cast<uint32_t>(INT32_MAX)) return false;
                decoded.frames[index] = static_cast<int>(value);
            }
            uint32_t frameIndex = 0, frameTimeIndex = 0;
            if (!readU32(payload, offset, frameIndex) || !readU32(payload, offset, frameTimeIndex)
                    || frameIndex > framesSize || frameTimeIndex > framesTimeSize
                    || offset != payload.size()) return false;
            decoded.frameIndex = static_cast<int>(frameIndex);
            decoded.frameTimeIndex = static_cast<int>(frameTimeIndex);
            decoded.spriteFrameCount = 0;
            SpriteAnimationComponent* animation = scene->findComponent<SpriteAnimationComponent>(entity);
            if (!animation) {
                scene->addComponent<SpriteAnimationComponent>(entity);
                animation = scene->findComponent<SpriteAnimationComponent>(entity);
            }
            if (!animation) return false;
            *animation = std::move(decoded);
            return true;
        }
    });
    (void)spriteAnimationRegistered;

    static const bool meshPolygonRegistered = NetworkSystem::registerReplicatedComponent({
        "MeshPolygonComponent", 1,
        [](Scene* scene, Entity entity, std::string& payload) {
            const MeshPolygonComponent* polygon = scene->findComponent<MeshPolygonComponent>(entity);
            if (!polygon || polygon->points.size() > 4096) return false;
            appendU32(payload, polygon->width);
            appendU32(payload, polygon->height);
            appendU8(payload, polygon->automaticFlipY ? 1 : 0);
            appendU8(payload, polygon->flipY ? 1 : 0);
            appendU32(payload, static_cast<uint32_t>(polygon->points.size()));
            for (const PolygonPoint& point : polygon->points) {
                if (!std::isfinite(point.position.x) || !std::isfinite(point.position.y)
                        || !std::isfinite(point.position.z) || !std::isfinite(point.color.x)
                        || !std::isfinite(point.color.y) || !std::isfinite(point.color.z)
                        || !std::isfinite(point.color.w)) return false;
                appendVector3(payload, point.position);
                appendFloat(payload, point.color.x);
                appendFloat(payload, point.color.y);
                appendFloat(payload, point.color.z);
                appendFloat(payload, point.color.w);
            }
            return payload.size() <= MAX_COMPONENT_PAYLOAD_BYTES;
        },
        [](uint16_t version, Scene* scene, Entity entity, const std::string& payload) {
            if (version != 1) return false;
            size_t offset = 0;
            uint32_t width = 0, height = 0, pointCount = 0;
            uint8_t automaticFlipY = 0, flipY = 0;
            if (!readU32(payload, offset, width) || !readU32(payload, offset, height)
                    || !readU8(payload, offset, automaticFlipY) || automaticFlipY > 1
                    || !readU8(payload, offset, flipY) || flipY > 1
                    || !readU32(payload, offset, pointCount) || pointCount > 4096) return false;
            std::vector<PolygonPoint> points;
            points.reserve(pointCount);
            for (uint32_t index = 0; index < pointCount; ++index) {
                PolygonPoint point;
                float color[4]{};
                if (!readVector3(payload, offset, point.position)
                        || !readFloat(payload, offset, color[0]) || !readFloat(payload, offset, color[1])
                        || !readFloat(payload, offset, color[2]) || !readFloat(payload, offset, color[3])) return false;
                point.color = Vector4(color[0], color[1], color[2], color[3]);
                points.push_back(point);
            }
            if (offset != payload.size()) return false;
            MeshPolygonComponent* polygon = scene->findComponent<MeshPolygonComponent>(entity);
            if (!polygon) {
                scene->addComponent<MeshPolygonComponent>(entity);
                polygon = scene->findComponent<MeshPolygonComponent>(entity);
            }
            if (!polygon) return false;
            polygon->width = width;
            polygon->height = height;
            polygon->automaticFlipY = automaticFlipY != 0;
            polygon->flipY = flipY != 0;
            polygon->points = std::move(points);
            polygon->needUpdatePolygon = true;
            return true;
        }
    });
    (void)meshPolygonRegistered;

    static const bool pointsRegistered = NetworkSystem::registerReplicatedComponent({
        "PointsComponent", 1,
        [](Scene* scene, Entity entity, std::string& payload) {
            const PointsComponent* points = scene->findComponent<PointsComponent>(entity);
            if (!points || points->points.size() > 4096 || points->numFramesRect > MAX_SPRITE_FRAMES
                    || points->maxPoints > 65535 || points->customShader.size() > MAX_SCRIPT_PROPERTY_BYTES
                    || !std::isfinite(static_cast<float>(points->maxPoints))) return false;
            appendU32(payload, static_cast<uint32_t>(points->points.size()));
            for (const PointData& point : points->points) {
                if (!std::isfinite(point.position.x) || !std::isfinite(point.position.y)
                        || !std::isfinite(point.position.z) || !std::isfinite(point.color.x)
                        || !std::isfinite(point.color.y) || !std::isfinite(point.color.z)
                        || !std::isfinite(point.color.w) || !std::isfinite(point.size)
                        || !std::isfinite(point.rotation) || !std::isfinite(point.textureRect[0])
                        || !std::isfinite(point.textureRect[1]) || !std::isfinite(point.textureRect[2])
                        || !std::isfinite(point.textureRect[3])) return false;
                appendVector3(payload, point.position);
                appendFloat(payload, point.color.x);
                appendFloat(payload, point.color.y);
                appendFloat(payload, point.color.z);
                appendFloat(payload, point.color.w);
                appendFloat(payload, point.size);
                appendFloat(payload, point.rotation);
                for (int component = 0; component < 4; ++component) appendFloat(payload, point.textureRect[component]);
                appendU8(payload, point.visible ? 1 : 0);
            }
            appendU32(payload, points->numFramesRect);
            for (unsigned int index = 0; index < points->numFramesRect; ++index) {
                const SpriteFrameData& frame = points->framesRect[index];
                if (frame.name.size() > MAX_SCRIPT_PROPERTY_BYTES
                        || !std::isfinite(frame.rect[0]) || !std::isfinite(frame.rect[1])
                        || !std::isfinite(frame.rect[2]) || !std::isfinite(frame.rect[3])) return false;
                appendString(payload, frame.name);
                for (int component = 0; component < 4; ++component) appendFloat(payload, frame.rect[component]);
            }
            appendU32(payload, points->maxPoints);
            appendString(payload, points->customShader);
            if (!appendTerrainTexture(payload, points->texture)) return false;
            appendU8(payload, points->transparent ? 1 : 0);
            appendU8(payload, points->autoTransparency ? 1 : 0);
            return payload.size() <= MAX_COMPONENT_PAYLOAD_BYTES;
        },
        [](uint16_t version, Scene* scene, Entity entity, const std::string& payload) {
            if (version != 1) return false;
            size_t offset = 0;
            uint32_t pointCount = 0, frameCount = 0, maxPoints = 0;
            if (!readU32(payload, offset, pointCount) || pointCount > 4096) return false;
            std::vector<PointData> decodedPoints;
            decodedPoints.reserve(pointCount);
            for (uint32_t index = 0; index < pointCount; ++index) {
                PointData point;
                uint8_t visible = 0;
                if (!readVector3(payload, offset, point.position) || !readVector4(payload, offset, point.color)
                        || !readFloat(payload, offset, point.size) || !readFloat(payload, offset, point.rotation)
                        || !readFloat(payload, offset, point.textureRect[0])
                        || !readFloat(payload, offset, point.textureRect[1])
                        || !readFloat(payload, offset, point.textureRect[2])
                        || !readFloat(payload, offset, point.textureRect[3])
                        || !readU8(payload, offset, visible) || visible > 1
                        || !std::isfinite(point.position.x) || !std::isfinite(point.position.y)
                        || !std::isfinite(point.position.z) || !std::isfinite(point.color.x)
                        || !std::isfinite(point.color.y) || !std::isfinite(point.color.z)
                        || !std::isfinite(point.color.w) || !std::isfinite(point.size)
                        || !std::isfinite(point.rotation)) return false;
                point.visible = visible != 0;
                decodedPoints.push_back(point);
            }
            if (!readU32(payload, offset, frameCount) || frameCount > MAX_SPRITE_FRAMES) return false;
            PointsComponent decoded;
            decoded.points = std::move(decodedPoints);
            decoded.numFramesRect = frameCount;
            for (uint32_t index = 0; index < frameCount; ++index) {
                SpriteFrameData& frame = decoded.framesRect[index];
                if (!readBoundedString(payload, offset, MAX_SCRIPT_PROPERTY_BYTES, frame.name)
                        || !readFloat(payload, offset, frame.rect[0]) || !readFloat(payload, offset, frame.rect[1])
                        || !readFloat(payload, offset, frame.rect[2]) || !readFloat(payload, offset, frame.rect[3])
                        || !std::isfinite(frame.rect[0]) || !std::isfinite(frame.rect[1])
                        || !std::isfinite(frame.rect[2]) || !std::isfinite(frame.rect[3])) return false;
            }
            uint8_t transparent = 0, autoTransparency = 0;
            if (!readU32(payload, offset, maxPoints) || maxPoints > 65535
                    || !readBoundedString(payload, offset, MAX_SCRIPT_PROPERTY_BYTES, decoded.customShader)
                    || !readTerrainTexture(payload, offset, decoded.texture)
                    || !readU8(payload, offset, transparent) || transparent > 1
                    || !readU8(payload, offset, autoTransparency) || autoTransparency > 1
                    || offset != payload.size()) return false;
            decoded.maxPoints = maxPoints;
            decoded.transparent = transparent != 0;
            decoded.autoTransparency = autoTransparency != 0;
            decoded.needUpdate = true;
            decoded.needUpdateBuffer = true;
            decoded.needUpdateTexture = true;
            decoded.needReload = true;
            PointsComponent* points = scene->findComponent<PointsComponent>(entity);
            if (!points) {
                scene->addComponent<PointsComponent>(entity);
                points = scene->findComponent<PointsComponent>(entity);
            }
            if (!points) return false;
            *points = std::move(decoded);
            return true;
        }
    });
    (void)pointsRegistered;

    static const bool linesRegistered = NetworkSystem::registerReplicatedComponent({
        "LinesComponent", 1,
        [](Scene* scene, Entity entity, std::string& payload) {
            const LinesComponent* lines = scene->findComponent<LinesComponent>(entity);
            if (!lines || lines->lines.size() > 4096 || lines->maxLines > 65535
                    || lines->customShader.size() > MAX_SCRIPT_PROPERTY_BYTES) return false;
            appendU32(payload, static_cast<uint32_t>(lines->lines.size()));
            for (const LineData& line : lines->lines) {
                if (!std::isfinite(line.pointA.x) || !std::isfinite(line.pointA.y)
                        || !std::isfinite(line.pointA.z) || !std::isfinite(line.pointB.x)
                        || !std::isfinite(line.pointB.y) || !std::isfinite(line.pointB.z)
                        || !std::isfinite(line.colorA.x) || !std::isfinite(line.colorA.y)
                        || !std::isfinite(line.colorA.z) || !std::isfinite(line.colorA.w)
                        || !std::isfinite(line.colorB.x) || !std::isfinite(line.colorB.y)
                        || !std::isfinite(line.colorB.z) || !std::isfinite(line.colorB.w)) return false;
                appendVector3(payload, line.pointA);
                appendFloat(payload, line.colorA.x);
                appendFloat(payload, line.colorA.y);
                appendFloat(payload, line.colorA.z);
                appendFloat(payload, line.colorA.w);
                appendVector3(payload, line.pointB);
                appendFloat(payload, line.colorB.x);
                appendFloat(payload, line.colorB.y);
                appendFloat(payload, line.colorB.z);
                appendFloat(payload, line.colorB.w);
            }
            appendU32(payload, lines->maxLines);
            appendString(payload, lines->customShader);
            return payload.size() <= MAX_COMPONENT_PAYLOAD_BYTES;
        },
        [](uint16_t version, Scene* scene, Entity entity, const std::string& payload) {
            if (version != 1) return false;
            size_t offset = 0;
            uint32_t lineCount = 0, maxLines = 0;
            if (!readU32(payload, offset, lineCount) || lineCount > 4096) return false;
            LinesComponent decoded;
            decoded.lines.reserve(lineCount);
            for (uint32_t index = 0; index < lineCount; ++index) {
                LineData line;
                if (!readVector3(payload, offset, line.pointA)
                        || !readFloat(payload, offset, line.colorA.x) || !readFloat(payload, offset, line.colorA.y)
                        || !readFloat(payload, offset, line.colorA.z) || !readFloat(payload, offset, line.colorA.w)
                        || !readVector3(payload, offset, line.pointB)
                        || !readFloat(payload, offset, line.colorB.x) || !readFloat(payload, offset, line.colorB.y)
                        || !readFloat(payload, offset, line.colorB.z) || !readFloat(payload, offset, line.colorB.w)
                        || !std::isfinite(line.pointA.x) || !std::isfinite(line.pointA.y)
                        || !std::isfinite(line.pointA.z) || !std::isfinite(line.pointB.x)
                        || !std::isfinite(line.pointB.y) || !std::isfinite(line.pointB.z)
                        || !std::isfinite(line.colorA.x) || !std::isfinite(line.colorA.y)
                        || !std::isfinite(line.colorA.z) || !std::isfinite(line.colorA.w)
                        || !std::isfinite(line.colorB.x) || !std::isfinite(line.colorB.y)
                        || !std::isfinite(line.colorB.z) || !std::isfinite(line.colorB.w)) return false;
                decoded.lines.push_back(line);
            }
            if (!readU32(payload, offset, maxLines) || maxLines > 65535
                    || !readBoundedString(payload, offset, MAX_SCRIPT_PROPERTY_BYTES, decoded.customShader)
                    || offset != payload.size()) return false;
            decoded.maxLines = maxLines;
            decoded.needUpdateBuffer = true;
            decoded.needReload = true;
            LinesComponent* lines = scene->findComponent<LinesComponent>(entity);
            if (!lines) {
                scene->addComponent<LinesComponent>(entity);
                lines = scene->findComponent<LinesComponent>(entity);
            }
            if (!lines) return false;
            *lines = std::move(decoded);
            return true;
        }
    });
    (void)linesRegistered;

    static const bool tilemapRegistered = NetworkSystem::registerReplicatedComponent({
        "TilemapComponent", 1,
        [](Scene* scene, Entity entity, std::string& payload) {
            const TilemapComponent* tilemap = scene->findComponent<TilemapComponent>(entity);
            if (!tilemap || tilemap->numTilesRect > MAX_TILEMAP_TILESRECT || tilemap->numTiles > MAX_TILEMAP_TILES
                    || !std::isfinite(tilemap->textureScaleFactor)) return false;
            appendU32(payload, tilemap->width);
            appendU32(payload, tilemap->height);
            appendU8(payload, tilemap->automaticFlipY ? 1 : 0);
            appendU8(payload, tilemap->flipY ? 1 : 0);
            appendFloat(payload, tilemap->textureScaleFactor);
            appendU32(payload, tilemap->reserveTiles);
            appendU32(payload, tilemap->numTilesRect);
            for (unsigned int index = 0; index < tilemap->numTilesRect; ++index) {
                const TileRectData& rect = tilemap->tilesRect[index];
                if (rect.name.size() > MAX_SCRIPT_PROPERTY_BYTES || rect.submeshId < -1
                        || rect.submeshId > 65535 || !std::isfinite(rect.rect[0])
                        || !std::isfinite(rect.rect[1]) || !std::isfinite(rect.rect[2])
                        || !std::isfinite(rect.rect[3])) return false;
                appendString(payload, rect.name);
                appendU32(payload, static_cast<uint32_t>(rect.submeshId));
                for (int component = 0; component < 4; ++component) appendFloat(payload, rect.rect[component]);
            }
            appendU32(payload, tilemap->numTiles);
            for (unsigned int index = 0; index < tilemap->numTiles; ++index) {
                const TileData& tile = tilemap->tiles[index];
                if (tile.name.size() > MAX_SCRIPT_PROPERTY_BYTES || tile.rectId < 0
                        || tile.rectId >= static_cast<int>(tilemap->numTilesRect)
                        || !std::isfinite(tile.position.x) || !std::isfinite(tile.position.y)
                        || !std::isfinite(tile.width) || !std::isfinite(tile.height)) return false;
                appendString(payload, tile.name);
                appendU32(payload, static_cast<uint32_t>(tile.rectId));
                appendFloat(payload, tile.position.x);
                appendFloat(payload, tile.position.y);
                appendFloat(payload, tile.width);
                appendFloat(payload, tile.height);
            }
            return payload.size() <= MAX_COMPONENT_PAYLOAD_BYTES;
        },
        [](uint16_t version, Scene* scene, Entity entity, const std::string& payload) {
            if (version != 1) return false;
            size_t offset = 0;
            uint32_t width = 0, height = 0, reserveTiles = 0, rectCount = 0, tileCount = 0;
            uint8_t automaticFlipY = 0, flipY = 0;
            float textureScaleFactor = 0.0f;
            if (!readU32(payload, offset, width) || !readU32(payload, offset, height)
                    || !readU8(payload, offset, automaticFlipY) || automaticFlipY > 1
                    || !readU8(payload, offset, flipY) || flipY > 1
                    || !readFloat(payload, offset, textureScaleFactor) || !std::isfinite(textureScaleFactor)
                    || !readU32(payload, offset, reserveTiles)
                    || !readU32(payload, offset, rectCount) || rectCount > MAX_TILEMAP_TILESRECT) return false;
            TilemapComponent decoded;
            decoded.width = width;
            decoded.height = height;
            decoded.automaticFlipY = automaticFlipY != 0;
            decoded.flipY = flipY != 0;
            decoded.textureScaleFactor = textureScaleFactor;
            decoded.reserveTiles = reserveTiles;
            decoded.numTilesRect = rectCount;
            for (uint32_t index = 0; index < rectCount; ++index) {
                TileRectData& rect = decoded.tilesRect[index];
                uint32_t submeshId = 0;
                if (!readBoundedString(payload, offset, MAX_SCRIPT_PROPERTY_BYTES, rect.name)
                        || !readU32(payload, offset, submeshId)
                        || !readFloat(payload, offset, rect.rect[0]) || !readFloat(payload, offset, rect.rect[1])
                        || !readFloat(payload, offset, rect.rect[2]) || !readFloat(payload, offset, rect.rect[3])
                        || (submeshId != UINT32_MAX && submeshId > 65535)
                        || !std::isfinite(rect.rect[0]) || !std::isfinite(rect.rect[1])
                        || !std::isfinite(rect.rect[2]) || !std::isfinite(rect.rect[3])) return false;
                rect.submeshId = submeshId == UINT32_MAX ? -1 : static_cast<int>(submeshId);
            }
            if (!readU32(payload, offset, tileCount) || tileCount > MAX_TILEMAP_TILES) return false;
            decoded.numTiles = tileCount;
            for (uint32_t index = 0; index < tileCount; ++index) {
                TileData& tile = decoded.tiles[index];
                uint32_t rectId = 0;
                if (!readBoundedString(payload, offset, MAX_SCRIPT_PROPERTY_BYTES, tile.name)
                        || !readU32(payload, offset, rectId) || rectId >= rectCount
                        || !readFloat(payload, offset, tile.position.x) || !readFloat(payload, offset, tile.position.y)
                        || !readFloat(payload, offset, tile.width) || !readFloat(payload, offset, tile.height)
                        || !std::isfinite(tile.position.x) || !std::isfinite(tile.position.y)
                        || !std::isfinite(tile.width) || !std::isfinite(tile.height)) return false;
                tile.rectId = static_cast<int>(rectId);
            }
            if (offset != payload.size()) return false;
            decoded.needUpdateTilemap = true;
            TilemapComponent* tilemap = scene->findComponent<TilemapComponent>(entity);
            if (!tilemap) {
                scene->addComponent<TilemapComponent>(entity);
                tilemap = scene->findComponent<TilemapComponent>(entity);
            }
            if (!tilemap) return false;
            *tilemap = std::move(decoded);
            return true;
        }
    });
    (void)tilemapRegistered;

    static const bool animatorControllerRegistered = NetworkSystem::registerReplicatedComponent({
        "AnimatorControllerComponent", 1,
        [](Scene* scene, Entity entity, std::string& payload) {
            const AnimatorControllerComponent* controller = scene->findComponent<AnimatorControllerComponent>(entity);
            if (!controller || controller->controllerAsset.size() > MAX_SCRIPT_PROPERTY_BYTES
                    || controller->parameters.size() > 128 || controller->layers.size() > MAX_ANIMATOR_LAYERS
                    || controller->ikChains.size() > 128 || controller->states.size() > 128
                    || controller->transitions.size() > 256 || !std::isfinite(controller->rootMotionScale)) return false;
            appendString(payload, controller->controllerAsset);
            appendU32(payload, static_cast<uint32_t>(controller->parameters.size()));
            for (const AnimatorParameter& parameter : controller->parameters) {
                if (parameter.name.size() > MAX_SCRIPT_PROPERTY_BYTES || static_cast<uint8_t>(parameter.type) > 3
                        || !std::isfinite(parameter.defaultValue)) return false;
                appendString(payload, parameter.name);
                appendU8(payload, static_cast<uint8_t>(parameter.type));
                appendFloat(payload, parameter.defaultValue);
                appendU8(payload, parameter.boolValue ? 1 : 0);
            }
            appendU32(payload, static_cast<uint32_t>(controller->layers.size()));
            for (const AnimatorLayer& layer : controller->layers) {
                if (layer.name.size() > MAX_SCRIPT_PROPERTY_BYTES || !std::isfinite(layer.weight)) return false;
                appendString(payload, layer.name);
                appendFloat(payload, layer.weight);
                appendU8(payload, layer.additive ? 1 : 0);
                appendU8(payload, layer.enabled ? 1 : 0);
            }
            appendU32(payload, static_cast<uint32_t>(controller->ikChains.size()));
            for (const AnimatorIKChain& chain : controller->ikChains) {
                appendU32(payload, chain.root); appendU32(payload, chain.middle); appendU32(payload, chain.tip);
                appendU32(payload, chain.target); appendU32(payload, chain.pole);
                if (!std::isfinite(chain.weight)) return false;
                appendFloat(payload, chain.weight);
                appendU8(payload, chain.enabled ? 1 : 0);
                appendU8(payload, chain.orientBones ? 1 : 0);
            }
            appendU8(payload, controller->rootMotionEnabled ? 1 : 0);
            appendU32(payload, controller->rootMotionTarget);
            appendFloat(payload, controller->rootMotionScale);
            appendU32(payload, static_cast<uint32_t>(controller->states.size()));
            for (const AnimatorState& state : controller->states) {
                if (state.name.size() > MAX_SCRIPT_PROPERTY_BYTES || state.clips.size() > 64
                        || !std::isfinite(state.x) || !std::isfinite(state.y)) return false;
                appendU32(payload, state.id); appendString(payload, state.name); appendU8(payload, state.isDefault ? 1 : 0);
                appendFloat(payload, state.x); appendFloat(payload, state.y);
                appendU32(payload, static_cast<uint32_t>(state.clips.size()));
                for (const AnimatorClip& clip : state.clips) {
                    if (clip.name.size() > MAX_SCRIPT_PROPERTY_BYTES || clip.events.size() > 64
                            || !std::isfinite(clip.speed) || !std::isfinite(clip.offset)) return false;
                    appendString(payload, clip.name); appendU32(payload, clip.animation); appendU32(payload, clip.audio);
                    appendFloat(payload, clip.speed); appendFloat(payload, clip.offset);
                    appendU8(payload, clip.loop ? 1 : 0); appendU32(payload, clip.layer);
                    appendU32(payload, static_cast<uint32_t>(clip.events.size()));
                    for (const AnimatorClip::Event& event : clip.events) {
                        if (event.functionName.size() > MAX_SCRIPT_PROPERTY_BYTES
                                || event.parameter.size() > MAX_SCRIPT_PROPERTY_BYTES || !std::isfinite(event.time)) return false;
                        appendFloat(payload, event.time); appendString(payload, event.functionName); appendString(payload, event.parameter);
                    }
                }
            }
            appendU32(payload, static_cast<uint32_t>(controller->transitions.size()));
            for (const AnimatorTransition& transition : controller->transitions) {
                if (transition.name.size() > MAX_SCRIPT_PROPERTY_BYTES || transition.conditions.size() > 64
                        || static_cast<uint8_t>(transition.interruptionSource) > 3
                        || !std::isfinite(transition.exitTime) || !std::isfinite(transition.transitionDuration)
                        || !std::isfinite(transition.offset)) return false;
                appendU32(payload, transition.id); appendString(payload, transition.name);
                appendU32(payload, transition.fromState); appendU32(payload, transition.toState);
                appendU8(payload, transition.hasExitTime ? 1 : 0); appendFloat(payload, transition.exitTime);
                appendFloat(payload, transition.transitionDuration); appendFloat(payload, transition.offset);
                appendU8(payload, transition.canTransitionToSelf ? 1 : 0);
                appendU8(payload, static_cast<uint8_t>(transition.interruptionSource));
                appendU32(payload, static_cast<uint32_t>(transition.conditions.size()));
                for (const AnimatorTransition::Condition& condition : transition.conditions) {
                    if (condition.parameter.size() > MAX_SCRIPT_PROPERTY_BYTES || static_cast<uint8_t>(condition.mode) > 3
                            || !std::isfinite(condition.value)) return false;
                    appendString(payload, condition.parameter); appendU8(payload, static_cast<uint8_t>(condition.mode));
                    appendFloat(payload, condition.value); appendU8(payload, condition.boolValue ? 1 : 0);
                }
            }
            return payload.size() <= MAX_COMPONENT_PAYLOAD_BYTES;
        },
        [](uint16_t version, Scene* scene, Entity entity, const std::string& payload) {
            if (version != 1) return false;
            size_t offset = 0;
            uint32_t count = 0;
            AnimatorControllerComponent decoded;
            if (!readBoundedString(payload, offset, MAX_SCRIPT_PROPERTY_BYTES, decoded.controllerAsset)
                    || !readU32(payload, offset, count) || count > 128) return false;
            decoded.parameters.resize(count);
            for (AnimatorParameter& parameter : decoded.parameters) {
                uint8_t type = 0, boolValue = 0;
                if (!readBoundedString(payload, offset, MAX_SCRIPT_PROPERTY_BYTES, parameter.name)
                        || !readU8(payload, offset, type) || type > 3 || !readFloat(payload, offset, parameter.defaultValue)
                        || !readU8(payload, offset, boolValue) || boolValue > 1 || !std::isfinite(parameter.defaultValue)) return false;
                parameter.type = static_cast<AnimatorParameter::Type>(type); parameter.boolValue = boolValue != 0;
            }
            if (!readU32(payload, offset, count) || count > MAX_ANIMATOR_LAYERS) return false;
            decoded.layers.resize(count);
            for (AnimatorLayer& layer : decoded.layers) {
                uint8_t additive = 0, enabled = 0;
                if (!readBoundedString(payload, offset, MAX_SCRIPT_PROPERTY_BYTES, layer.name)
                        || !readFloat(payload, offset, layer.weight) || !readU8(payload, offset, additive)
                        || !readU8(payload, offset, enabled) || additive > 1 || enabled > 1 || !std::isfinite(layer.weight)) return false;
                layer.additive = additive != 0; layer.enabled = enabled != 0;
            }
            if (!readU32(payload, offset, count) || count > 128) return false;
            decoded.ikChains.resize(count);
            for (AnimatorIKChain& chain : decoded.ikChains) {
                uint8_t enabled = 0, orientBones = 0;
                if (!readU32(payload, offset, chain.root) || !readU32(payload, offset, chain.middle)
                        || !readU32(payload, offset, chain.tip) || !readU32(payload, offset, chain.target)
                        || !readU32(payload, offset, chain.pole) || !readFloat(payload, offset, chain.weight)
                        || !readU8(payload, offset, enabled) || !readU8(payload, offset, orientBones)
                        || enabled > 1 || orientBones > 1 || !std::isfinite(chain.weight)) return false;
                chain.enabled = enabled != 0; chain.orientBones = orientBones != 0;
            }
            uint8_t rootMotionEnabled = 0;
            if (!readU8(payload, offset, rootMotionEnabled) || rootMotionEnabled > 1
                    || !readU32(payload, offset, decoded.rootMotionTarget)
                    || !readFloat(payload, offset, decoded.rootMotionScale) || !std::isfinite(decoded.rootMotionScale)
                    || !readU32(payload, offset, count) || count > 128) return false;
            decoded.rootMotionEnabled = rootMotionEnabled != 0;
            decoded.states.resize(count);
            for (AnimatorState& state : decoded.states) {
                uint8_t isDefault = 0, loop = 0;
                uint32_t clipCount = 0;
                if (!readU32(payload, offset, state.id) || !readBoundedString(payload, offset, MAX_SCRIPT_PROPERTY_BYTES, state.name)
                        || !readU8(payload, offset, isDefault) || isDefault > 1 || !readFloat(payload, offset, state.x)
                        || !readFloat(payload, offset, state.y) || !readU32(payload, offset, clipCount) || clipCount > 64
                        || !std::isfinite(state.x) || !std::isfinite(state.y)) return false;
                state.isDefault = isDefault != 0; state.clips.resize(clipCount);
                for (AnimatorClip& clip : state.clips) {
                    uint32_t eventCount = 0;
                    if (!readBoundedString(payload, offset, MAX_SCRIPT_PROPERTY_BYTES, clip.name)
                            || !readU32(payload, offset, clip.animation) || !readU32(payload, offset, clip.audio)
                            || !readFloat(payload, offset, clip.speed) || !readFloat(payload, offset, clip.offset)
                            || !readU8(payload, offset, loop) || loop > 1 || !readU32(payload, offset, clip.layer)
                            || !readU32(payload, offset, eventCount) || eventCount > 64
                            || !std::isfinite(clip.speed) || !std::isfinite(clip.offset)) return false;
                    clip.loop = loop != 0; clip.events.resize(eventCount);
                    for (AnimatorClip::Event& event : clip.events) {
                        if (!readFloat(payload, offset, event.time) || !readBoundedString(payload, offset, MAX_SCRIPT_PROPERTY_BYTES, event.functionName)
                                || !readBoundedString(payload, offset, MAX_SCRIPT_PROPERTY_BYTES, event.parameter)
                                || !std::isfinite(event.time)) return false;
                    }
                }
            }
            if (!readU32(payload, offset, count) || count > 256) return false;
            decoded.transitions.resize(count);
            for (AnimatorTransition& transition : decoded.transitions) {
                uint8_t hasExitTime = 0, canSelf = 0, interruption = 0;
                uint32_t conditionCount = 0;
                if (!readU32(payload, offset, transition.id) || !readBoundedString(payload, offset, MAX_SCRIPT_PROPERTY_BYTES, transition.name)
                        || !readU32(payload, offset, transition.fromState) || !readU32(payload, offset, transition.toState)
                        || !readU8(payload, offset, hasExitTime) || !readFloat(payload, offset, transition.exitTime)
                        || !readFloat(payload, offset, transition.transitionDuration) || !readFloat(payload, offset, transition.offset)
                        || !readU8(payload, offset, canSelf) || !readU8(payload, offset, interruption) || hasExitTime > 1
                        || canSelf > 1 || interruption > 3 || !readU32(payload, offset, conditionCount) || conditionCount > 64
                        || !std::isfinite(transition.exitTime) || !std::isfinite(transition.transitionDuration)
                        || !std::isfinite(transition.offset)) return false;
                transition.hasExitTime = hasExitTime != 0; transition.canTransitionToSelf = canSelf != 0;
                transition.interruptionSource = static_cast<AnimatorTransition::InterruptionSource>(interruption);
                transition.conditions.resize(conditionCount);
                for (AnimatorTransition::Condition& condition : transition.conditions) {
                    uint8_t mode = 0, boolValue = 0;
                    if (!readBoundedString(payload, offset, MAX_SCRIPT_PROPERTY_BYTES, condition.parameter)
                            || !readU8(payload, offset, mode) || mode > 3 || !readFloat(payload, offset, condition.value)
                            || !readU8(payload, offset, boolValue) || boolValue > 1 || !std::isfinite(condition.value)) return false;
                    condition.mode = static_cast<AnimatorTransition::Condition::Mode>(mode); condition.boolValue = boolValue != 0;
                }
            }
            if (offset != payload.size()) return false;
            decoded.enabled = true; decoded.currentState = 0; decoded.currentStateTime = 0.0f; decoded.isPlaying = false;
            AnimatorControllerComponent* controller = scene->findComponent<AnimatorControllerComponent>(entity);
            if (!controller) {
                scene->addComponent<AnimatorControllerComponent>(entity);
                controller = scene->findComponent<AnimatorControllerComponent>(entity);
            }
            if (!controller) return false;
            *controller = std::move(decoded);
            return true;
        }
    });
    (void)animatorControllerRegistered;

    static const bool meshRegistered = NetworkSystem::registerReplicatedComponent({
        "MeshComponent", 1,
        [](Scene* scene, Entity entity, std::string& payload) {
            const MeshComponent* mesh = scene->findComponent<MeshComponent>(entity);
            if (!mesh || mesh->customShader.size() > MAX_SCRIPT_PROPERTY_BYTES
                    || mesh->customDepthShader.size() > MAX_SCRIPT_PROPERTY_BYTES
                    || mesh->numSubmeshes > MAX_SUBMESHES || !std::isfinite(mesh->lodBias)) return false;
            appendString(payload, mesh->customShader); appendString(payload, mesh->customDepthShader);
            const bool flags[] = {mesh->receiveLights, mesh->receiveIBL, mesh->castShadows, mesh->receiveShadows,
                mesh->shadowsBillboard, mesh->renderInReflectionProbes, mesh->transparent, mesh->autoTransparency, mesh->lodEnabled};
            for (bool flag : flags) appendU8(payload, flag ? 1 : 0);
            appendFloat(payload, mesh->lodBias);
            appendU8(payload, static_cast<uint8_t>(mesh->cullingMode));
            appendU8(payload, static_cast<uint8_t>(mesh->windingOrder));
            appendU32(payload, mesh->numSubmeshes);
            for (unsigned int index = 0; index < mesh->numSubmeshes; ++index) {
                const Submesh& submesh = mesh->submeshes[index];
                const Material& material = submesh.material;
                if (material.name.size() > MAX_SCRIPT_PROPERTY_BYTES || static_cast<uint8_t>(material.alphaMode) > 3
                        || static_cast<uint8_t>(submesh.primitiveType) > 3
                        || !std::isfinite(material.baseColorFactor.x) || !std::isfinite(material.baseColorFactor.y)
                        || !std::isfinite(material.baseColorFactor.z) || !std::isfinite(material.baseColorFactor.w)
                        || !std::isfinite(material.metallicFactor) || !std::isfinite(material.roughnessFactor)
                        || !std::isfinite(material.alphaCutoff) || !std::isfinite(material.emissiveFactor.x)
                        || !std::isfinite(material.emissiveFactor.y) || !std::isfinite(material.emissiveFactor.z)
                        || !std::isfinite(material.foliageTransmission) || !std::isfinite(material.heightScale)
                        || !std::isfinite(material.heightMinLayers) || !std::isfinite(material.heightMaxLayers)) return false;
                appendU32(payload, submesh.overrideFields);
                appendFloat(payload, material.baseColorFactor.x); appendFloat(payload, material.baseColorFactor.y);
                appendFloat(payload, material.baseColorFactor.z); appendFloat(payload, material.baseColorFactor.w);
                appendFloat(payload, material.metallicFactor); appendFloat(payload, material.roughnessFactor);
                appendFloat(payload, material.alphaCutoff); appendVector3(payload, material.emissiveFactor);
                appendFloat(payload, material.foliageTransmission); appendFloat(payload, material.heightScale);
                appendFloat(payload, material.heightMinLayers); appendFloat(payload, material.heightMaxLayers);
                appendString(payload, material.name);
                if (!appendTerrainTexture(payload, material.baseColorTexture)
                        || !appendTerrainTexture(payload, material.emissiveTexture)
                        || !appendTerrainTexture(payload, material.metallicRoughnessTexture)
                        || !appendTerrainTexture(payload, material.occlusionTexture)
                        || !appendTerrainTexture(payload, material.normalTexture)
                        || !appendTerrainTexture(payload, material.heightTexture)) return false;
                appendU8(payload, static_cast<uint8_t>(material.alphaMode));
                appendU8(payload, submesh.faceCulling ? 1 : 0); appendU8(payload, submesh.textureShadow ? 1 : 0);
                appendU8(payload, static_cast<uint8_t>(submesh.primitiveType));
            }
            return payload.size() <= MAX_COMPONENT_PAYLOAD_BYTES;
        },
        [](uint16_t version, Scene* scene, Entity entity, const std::string& payload) {
            if (version != 1) return false;
            size_t offset = 0;
            MeshComponent decoded;
            uint8_t flags[9]{}, culling = 0, winding = 0;
            uint32_t submeshCount = 0;
            if (!readBoundedString(payload, offset, MAX_SCRIPT_PROPERTY_BYTES, decoded.customShader)
                    || !readBoundedString(payload, offset, MAX_SCRIPT_PROPERTY_BYTES, decoded.customDepthShader)) return false;
            for (uint8_t& flag : flags) if (!readU8(payload, offset, flag) || flag > 1) return false;
            if (!readFloat(payload, offset, decoded.lodBias) || !std::isfinite(decoded.lodBias)
                    || !readU8(payload, offset, culling) || culling > 1 || !readU8(payload, offset, winding) || winding > 1
                    || !readU32(payload, offset, submeshCount) || submeshCount > MAX_SUBMESHES) return false;
            decoded.receiveLights = flags[0] != 0; decoded.receiveIBL = flags[1] != 0; decoded.castShadows = flags[2] != 0;
            decoded.receiveShadows = flags[3] != 0; decoded.shadowsBillboard = flags[4] != 0;
            decoded.renderInReflectionProbes = flags[5] != 0; decoded.transparent = flags[6] != 0;
            decoded.autoTransparency = flags[7] != 0; decoded.lodEnabled = flags[8] != 0;
            decoded.cullingMode = static_cast<CullingMode>(culling); decoded.windingOrder = static_cast<WindingOrder>(winding);
            decoded.numSubmeshes = submeshCount;
            for (uint32_t index = 0; index < submeshCount; ++index) {
                Submesh& submesh = decoded.submeshes[index];
                Material& material = submesh.material;
                uint8_t alphaMode = 0, faceCulling = 0, textureShadow = 0, primitive = 0;
                if (!readU32(payload, offset, submesh.overrideFields) || !readVector4(payload, offset, material.baseColorFactor)
                        || !readFloat(payload, offset, material.metallicFactor) || !readFloat(payload, offset, material.roughnessFactor)
                        || !readFloat(payload, offset, material.alphaCutoff) || !readVector3(payload, offset, material.emissiveFactor)
                        || !readFloat(payload, offset, material.foliageTransmission) || !readFloat(payload, offset, material.heightScale)
                        || !readFloat(payload, offset, material.heightMinLayers) || !readFloat(payload, offset, material.heightMaxLayers)
                        || !readBoundedString(payload, offset, MAX_SCRIPT_PROPERTY_BYTES, material.name)
                        || !readTerrainTexture(payload, offset, material.baseColorTexture)
                        || !readTerrainTexture(payload, offset, material.emissiveTexture)
                        || !readTerrainTexture(payload, offset, material.metallicRoughnessTexture)
                        || !readTerrainTexture(payload, offset, material.occlusionTexture)
                        || !readTerrainTexture(payload, offset, material.normalTexture)
                        || !readTerrainTexture(payload, offset, material.heightTexture)
                        || !readU8(payload, offset, alphaMode) || alphaMode > 3
                        || !readU8(payload, offset, faceCulling) || faceCulling > 1
                        || !readU8(payload, offset, textureShadow) || textureShadow > 1
                        || !readU8(payload, offset, primitive) || primitive > 3) return false;
                if (!std::isfinite(material.baseColorFactor.x) || !std::isfinite(material.baseColorFactor.y)
                        || !std::isfinite(material.baseColorFactor.z) || !std::isfinite(material.baseColorFactor.w)
                        || !std::isfinite(material.metallicFactor) || !std::isfinite(material.roughnessFactor)
                        || !std::isfinite(material.alphaCutoff) || !std::isfinite(material.emissiveFactor.x)
                        || !std::isfinite(material.emissiveFactor.y) || !std::isfinite(material.emissiveFactor.z)
                        || !std::isfinite(material.foliageTransmission) || !std::isfinite(material.heightScale)
                        || !std::isfinite(material.heightMinLayers) || !std::isfinite(material.heightMaxLayers)) return false;
                material.alphaMode = static_cast<MaterialAlphaMode>(alphaMode);
                submesh.faceCulling = faceCulling != 0; submesh.textureShadow = textureShadow != 0;
                submesh.primitiveType = static_cast<PrimitiveType>(primitive); submesh.needUpdateTexture = true;
            }
            if (offset != payload.size()) return false;
            decoded.needReload = true;
            decoded.needUpdateBuffer = true;
            MeshComponent* mesh = scene->findComponent<MeshComponent>(entity);
            if (!mesh) {
                scene->addComponent<MeshComponent>(entity);
                mesh = scene->findComponent<MeshComponent>(entity);
            }
            if (!mesh) return false;
            *mesh = std::move(decoded);
            return true;
        }
    });
    (void)meshRegistered;

    static const bool joint2DRegistered = NetworkSystem::registerReplicatedComponent({
        "Joint2DComponent", 1,
        [](Scene* scene, Entity entity, std::string& payload) {
            const Joint2DComponent* joint = scene->findComponent<Joint2DComponent>(entity);
            if (!joint || static_cast<uint8_t>(joint->type) > static_cast<uint8_t>(Joint2DType::MOTOR)) return false;
            const Vector2 values[] = {joint->anchorA, joint->anchorB, joint->axis, joint->target};
            for (const Vector2& value : values) if (!std::isfinite(value.x) || !std::isfinite(value.y)) return false;
            appendU8(payload, static_cast<uint8_t>(joint->type)); appendU32(payload, joint->bodyA); appendU32(payload, joint->bodyB);
            for (const Vector2& value : values) { appendFloat(payload, value.x); appendFloat(payload, value.y); }
            appendU8(payload, joint->autoAnchors ? 1 : 0); appendU8(payload, joint->rope ? 1 : 0);
            return payload.size() <= MAX_COMPONENT_PAYLOAD_BYTES;
        },
        [](uint16_t version, Scene* scene, Entity entity, const std::string& payload) {
            if (version != 1) return false;
            size_t offset = 0; uint8_t type = 0, autoAnchors = 0, rope = 0;
            Joint2DComponent decoded;
            if (!readU8(payload, offset, type) || type > static_cast<uint8_t>(Joint2DType::MOTOR)
                    || !readU32(payload, offset, decoded.bodyA) || !readU32(payload, offset, decoded.bodyB)) return false;
            Vector2* values[] = {&decoded.anchorA, &decoded.anchorB, &decoded.axis, &decoded.target};
            for (Vector2* value : values) if (!readFloat(payload, offset, value->x) || !readFloat(payload, offset, value->y)
                    || !std::isfinite(value->x) || !std::isfinite(value->y)) return false;
            if (!readU8(payload, offset, autoAnchors) || autoAnchors > 1 || !readU8(payload, offset, rope) || rope > 1
                    || offset != payload.size()) return false;
            decoded.type = static_cast<Joint2DType>(type); decoded.autoAnchors = autoAnchors != 0; decoded.rope = rope != 0;
            decoded.needUpdateJoint = true;
            Joint2DComponent* joint = scene->findComponent<Joint2DComponent>(entity);
            if (!joint) { scene->addComponent<Joint2DComponent>(entity); joint = scene->findComponent<Joint2DComponent>(entity); }
            if (!joint) return false; *joint = std::move(decoded); return true;
        }
    });
    (void)joint2DRegistered;

    static const bool joint3DRegistered = NetworkSystem::registerReplicatedComponent({
        "Joint3DComponent", 1,
        [](Scene* scene, Entity entity, std::string& payload) {
            const Joint3DComponent* joint = scene->findComponent<Joint3DComponent>(entity);
            if (!joint || static_cast<uint8_t>(joint->type) > static_cast<uint8_t>(Joint3DType::PULLEY)
                    || joint->pathPoints.size() > 256) return false;
            const Vector3 values[] = {joint->anchorA, joint->anchorB, joint->anchor, joint->axis, joint->normal,
                joint->twistAxis, joint->planeAxis, joint->axisX, joint->axisY, joint->fixedPointA, joint->fixedPointB};
            for (const Vector3& value : values) if (!std::isfinite(value.x) || !std::isfinite(value.y) || !std::isfinite(value.z)) return false;
            const float scalars[] = {joint->limitsMin, joint->limitsMax, joint->normalHalfConeAngle,
                joint->planeHalfConeAngle, joint->twistMinAngle, joint->twistMaxAngle};
            for (float value : scalars) if (!std::isfinite(value)) return false;
            appendU8(payload, static_cast<uint8_t>(joint->type));
            const Entity entities[] = {joint->bodyA, joint->bodyB, joint->hingeA, joint->hingeB, joint->hinge, joint->slider};
            for (Entity value : entities) appendU32(payload, value);
            for (const Vector3& value : values) appendVector3(payload, value);
            for (float value : scalars) appendFloat(payload, value);
            const int teeth[] = {joint->numTeethGearA, joint->numTeethGearB, joint->numTeethRack, joint->numTeethGear, joint->rackLength};
            for (int value : teeth) appendU32(payload, static_cast<uint32_t>(value));
            appendU32(payload, static_cast<uint32_t>(joint->pathPoints.size()));
            for (const Vector3& point : joint->pathPoints) {
                if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z)) return false;
                appendVector3(payload, point);
            }
            appendVector3(payload, joint->pathPosition); appendU8(payload, joint->isLooping ? 1 : 0);
            appendU8(payload, joint->autoAnchors ? 1 : 0);
            return payload.size() <= MAX_COMPONENT_PAYLOAD_BYTES;
        },
        [](uint16_t version, Scene* scene, Entity entity, const std::string& payload) {
            if (version != 1) return false;
            size_t offset = 0; uint8_t type = 0, looping = 0, autoAnchors = 0; uint32_t pathCount = 0;
            Joint3DComponent decoded;
            if (!readU8(payload, offset, type) || type > static_cast<uint8_t>(Joint3DType::PULLEY)) return false;
            Entity* entities[] = {&decoded.bodyA, &decoded.bodyB, &decoded.hingeA, &decoded.hingeB, &decoded.hinge, &decoded.slider};
            for (Entity* value : entities) if (!readU32(payload, offset, *value)) return false;
            Vector3* vectors[] = {&decoded.anchorA, &decoded.anchorB, &decoded.anchor, &decoded.axis, &decoded.normal,
                &decoded.twistAxis, &decoded.planeAxis, &decoded.axisX, &decoded.axisY, &decoded.fixedPointA, &decoded.fixedPointB};
            for (Vector3* value : vectors) if (!readVector3(payload, offset, *value)) return false;
            float* scalars[] = {&decoded.limitsMin, &decoded.limitsMax, &decoded.normalHalfConeAngle,
                &decoded.planeHalfConeAngle, &decoded.twistMinAngle, &decoded.twistMaxAngle};
            for (float* value : scalars) if (!readFloat(payload, offset, *value) || !std::isfinite(*value)) return false;
            int* teeth[] = {&decoded.numTeethGearA, &decoded.numTeethGearB, &decoded.numTeethRack, &decoded.numTeethGear, &decoded.rackLength};
            for (int* value : teeth) { uint32_t raw = 0; if (!readU32(payload, offset, raw) || raw > static_cast<uint32_t>(INT32_MAX)) return false; *value = static_cast<int>(raw); }
            if (!readU32(payload, offset, pathCount) || pathCount > 256) return false;
            decoded.pathPoints.resize(pathCount);
            for (Vector3& point : decoded.pathPoints) if (!readVector3(payload, offset, point)
                    || !std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z)) return false;
            if (!readVector3(payload, offset, decoded.pathPosition) || !readU8(payload, offset, looping) || looping > 1
                    || !readU8(payload, offset, autoAnchors) || autoAnchors > 1 || offset != payload.size()) return false;
            decoded.type = static_cast<Joint3DType>(type); decoded.isLooping = looping != 0; decoded.autoAnchors = autoAnchors != 0;
            decoded.needUpdateJoint = true;
            Joint3DComponent* joint = scene->findComponent<Joint3DComponent>(entity);
            if (!joint) { scene->addComponent<Joint3DComponent>(entity); joint = scene->findComponent<Joint3DComponent>(entity); }
            if (!joint) return false; *joint = std::move(decoded); return true;
        }
    });
    (void)joint3DRegistered;

    static const bool boneRegistered = NetworkSystem::registerReplicatedComponent({
        "BoneComponent", 1,
        [](Scene* scene, Entity entity, std::string& payload) {
            const BoneComponent* bone = scene->findComponent<BoneComponent>(entity);
            if (!bone || bone->index < 0 || bone->index > 65535) return false;
            if (!std::isfinite(bone->bindPosition.x) || !std::isfinite(bone->bindPosition.y) || !std::isfinite(bone->bindPosition.z)
                    || !std::isfinite(bone->bindRotation.w) || !std::isfinite(bone->bindRotation.x)
                    || !std::isfinite(bone->bindRotation.y) || !std::isfinite(bone->bindRotation.z)
                    || !std::isfinite(bone->bindScale.x) || !std::isfinite(bone->bindScale.y) || !std::isfinite(bone->bindScale.z)) return false;
            appendU32(payload, bone->model); appendU32(payload, static_cast<uint32_t>(bone->index));
            appendVector3(payload, bone->bindPosition);
            appendFloat(payload, bone->bindRotation.w); appendFloat(payload, bone->bindRotation.x);
            appendFloat(payload, bone->bindRotation.y); appendFloat(payload, bone->bindRotation.z);
            appendVector3(payload, bone->bindScale);
            for (int column = 0; column < 4; ++column) for (int row = 0; row < 4; ++row) {
                if (!std::isfinite(bone->offsetMatrix[column][row])) return false;
                appendFloat(payload, bone->offsetMatrix[column][row]);
            }
            return payload.size() <= MAX_COMPONENT_PAYLOAD_BYTES;
        },
        [](uint16_t version, Scene* scene, Entity entity, const std::string& payload) {
            if (version != 1) return false;
            size_t offset = 0; uint32_t index = 0;
            BoneComponent decoded;
            if (!readU32(payload, offset, decoded.model) || !readU32(payload, offset, index) || index > 65535
                    || !readVector3(payload, offset, decoded.bindPosition) || !readFloat(payload, offset, decoded.bindRotation.w)
                    || !readFloat(payload, offset, decoded.bindRotation.x) || !readFloat(payload, offset, decoded.bindRotation.y)
                    || !readFloat(payload, offset, decoded.bindRotation.z) || !readVector3(payload, offset, decoded.bindScale)) return false;
            decoded.index = static_cast<int>(index);
            for (int column = 0; column < 4; ++column) for (int row = 0; row < 4; ++row)
                if (!readFloat(payload, offset, decoded.offsetMatrix[column][row]) || !std::isfinite(decoded.offsetMatrix[column][row])) return false;
            if (offset != payload.size() || !std::isfinite(decoded.bindPosition.x) || !std::isfinite(decoded.bindPosition.y)
                    || !std::isfinite(decoded.bindPosition.z) || !std::isfinite(decoded.bindRotation.w)
                    || !std::isfinite(decoded.bindRotation.x) || !std::isfinite(decoded.bindRotation.y)
                    || !std::isfinite(decoded.bindRotation.z) || !std::isfinite(decoded.bindScale.x)
                    || !std::isfinite(decoded.bindScale.y) || !std::isfinite(decoded.bindScale.z)) return false;
            BoneComponent* bone = scene->findComponent<BoneComponent>(entity);
            if (!bone) { scene->addComponent<BoneComponent>(entity); bone = scene->findComponent<BoneComponent>(entity); }
            if (!bone) return false; *bone = std::move(decoded); return true;
        }
    });
    (void)boneRegistered;

    static const bool morphTracksRegistered = NetworkSystem::registerReplicatedComponent({
        "MorphTracksComponent", 1,
        [](Scene* scene, Entity entity, std::string& payload) {
            const MorphTracksComponent* tracks = scene->findComponent<MorphTracksComponent>(entity);
            if (!tracks || tracks->values.size() > 256 || tracks->inTangents.size() > 256 || tracks->outTangents.size() > 256) return false;
            const std::vector<std::vector<float>>* lists[] = {&tracks->values, &tracks->inTangents, &tracks->outTangents};
            for (const auto* list : lists) {
                appendU32(payload, static_cast<uint32_t>(list->size()));
                for (const std::vector<float>& track : *list) {
                    if (track.size() > 1024) return false;
                    appendU32(payload, static_cast<uint32_t>(track.size()));
                    for (float value : track) if (!std::isfinite(value)) return false; else appendFloat(payload, value);
                }
            }
            return payload.size() <= MAX_COMPONENT_PAYLOAD_BYTES;
        },
        [](uint16_t version, Scene* scene, Entity entity, const std::string& payload) {
            if (version != 1) return false;
            size_t offset = 0;
            MorphTracksComponent decoded;
            std::vector<std::vector<float>>* lists[] = {&decoded.values, &decoded.inTangents, &decoded.outTangents};
            for (auto* list : lists) {
                uint32_t trackCount = 0;
                if (!readU32(payload, offset, trackCount) || trackCount > 256) return false;
                list->resize(trackCount);
                for (std::vector<float>& track : *list) {
                    uint32_t valueCount = 0;
                    if (!readU32(payload, offset, valueCount) || valueCount > 1024) return false;
                    track.resize(valueCount);
                    for (float& value : track) if (!readFloat(payload, offset, value) || !std::isfinite(value)) return false;
                }
            }
            if (offset != payload.size()) return false;
            MorphTracksComponent* tracks = scene->findComponent<MorphTracksComponent>(entity);
            if (!tracks) { scene->addComponent<MorphTracksComponent>(entity); tracks = scene->findComponent<MorphTracksComponent>(entity); }
            if (!tracks) return false; *tracks = std::move(decoded); return true;
        }
    });
    (void)morphTracksRegistered;

    static const bool keyframeTracksRegistered = NetworkSystem::registerReplicatedComponent({
        "KeyframeTracksComponent", 1,
        [](Scene* scene, Entity entity, std::string& payload) {
            const KeyframeTracksComponent* track = scene->findComponent<KeyframeTracksComponent>(entity);
            if (!track || track->times.size() > 1024 || track->easings.size() > 1024 || track->index < 0 || track->index > 65535
                    || !std::isfinite(track->interpolation)) return false;
            appendU32(payload, static_cast<uint32_t>(track->times.size()));
            for (float time : track->times) if (!std::isfinite(time)) return false; else appendFloat(payload, time);
            appendU32(payload, static_cast<uint32_t>(track->easings.size()));
            for (EaseType ease : track->easings) {
                if (static_cast<uint8_t>(ease) > static_cast<uint8_t>(EaseType::CUSTOM)) return false;
                appendU8(payload, static_cast<uint8_t>(ease));
            }
            appendU8(payload, track->loop ? 1 : 0); appendU8(payload, track->relative ? 1 : 0);
            appendU32(payload, static_cast<uint32_t>(track->index)); appendFloat(payload, track->interpolation);
            appendU8(payload, track->hasBase ? 1 : 0); appendVector3(payload, track->basePosition);
            appendFloat(payload, track->baseRotation.w); appendFloat(payload, track->baseRotation.x);
            appendFloat(payload, track->baseRotation.y); appendFloat(payload, track->baseRotation.z);
            appendVector3(payload, track->baseScale);
            return payload.size() <= MAX_COMPONENT_PAYLOAD_BYTES;
        },
        [](uint16_t version, Scene* scene, Entity entity, const std::string& payload) {
            if (version != 1) return false;
            size_t offset = 0; uint32_t count = 0, index = 0; uint8_t loop = 0, relative = 0, hasBase = 0;
            KeyframeTracksComponent decoded;
            if (!readU32(payload, offset, count) || count > 1024) return false;
            decoded.times.resize(count);
            for (float& time : decoded.times) if (!readFloat(payload, offset, time) || !std::isfinite(time)) return false;
            if (!readU32(payload, offset, count) || count > 1024) return false;
            decoded.easings.resize(count);
            for (EaseType& ease : decoded.easings) { uint8_t value = 0; if (!readU8(payload, offset, value) || value > static_cast<uint8_t>(EaseType::CUSTOM)) return false; ease = static_cast<EaseType>(value); }
            if (!readU8(payload, offset, loop) || loop > 1 || !readU8(payload, offset, relative) || relative > 1
                    || !readU32(payload, offset, index) || index > 65535 || !readFloat(payload, offset, decoded.interpolation)
                    || !readU8(payload, offset, hasBase) || hasBase > 1 || !readVector3(payload, offset, decoded.basePosition)
                    || !readFloat(payload, offset, decoded.baseRotation.w) || !readFloat(payload, offset, decoded.baseRotation.x)
                    || !readFloat(payload, offset, decoded.baseRotation.y) || !readFloat(payload, offset, decoded.baseRotation.z)
                    || !readVector3(payload, offset, decoded.baseScale) || offset != payload.size()
                    || !std::isfinite(decoded.interpolation) || !std::isfinite(decoded.basePosition.x)
                    || !std::isfinite(decoded.basePosition.y) || !std::isfinite(decoded.basePosition.z)
                    || !std::isfinite(decoded.baseRotation.w) || !std::isfinite(decoded.baseRotation.x)
                    || !std::isfinite(decoded.baseRotation.y) || !std::isfinite(decoded.baseRotation.z)
                    || !std::isfinite(decoded.baseScale.x) || !std::isfinite(decoded.baseScale.y) || !std::isfinite(decoded.baseScale.z)) return false;
            decoded.loop = loop != 0; decoded.relative = relative != 0; decoded.index = static_cast<int>(index); decoded.hasBase = hasBase != 0;
            KeyframeTracksComponent* track = scene->findComponent<KeyframeTracksComponent>(entity);
            if (!track) { scene->addComponent<KeyframeTracksComponent>(entity); track = scene->findComponent<KeyframeTracksComponent>(entity); }
            if (!track) return false; *track = std::move(decoded); return true;
        }
    });
    (void)keyframeTracksRegistered;

    static const bool scaleTracksRegistered = NetworkSystem::registerReplicatedComponent({
        "ScaleTracksComponent", 1,
        [](Scene* scene, Entity entity, std::string& payload) {
            const ScaleTracksComponent* tracks = scene->findComponent<ScaleTracksComponent>(entity);
            if (!tracks || tracks->values.size() > 1024 || tracks->inTangents.size() > 1024 || tracks->outTangents.size() > 1024) return false;
            const std::vector<Vector3>* lists[] = {&tracks->values, &tracks->inTangents, &tracks->outTangents};
            for (const auto* list : lists) {
                if (list->size() > 1024) return false;
                appendU32(payload, static_cast<uint32_t>(list->size()));
                for (const Vector3& value : *list) {
                    if (!std::isfinite(value.x) || !std::isfinite(value.y) || !std::isfinite(value.z)) return false;
                    appendVector3(payload, value);
                }
            }
            return payload.size() <= MAX_COMPONENT_PAYLOAD_BYTES;
        },
        [](uint16_t version, Scene* scene, Entity entity, const std::string& payload) {
            if (version != 1) return false;
            size_t offset = 0;
            ScaleTracksComponent decoded;
            std::vector<Vector3>* lists[] = {&decoded.values, &decoded.inTangents, &decoded.outTangents};
            for (auto* list : lists) {
                uint32_t valueCount = 0;
                if (!readU32(payload, offset, valueCount) || valueCount > 1024) return false;
                list->resize(valueCount);
                for (Vector3& value : *list) if (!readVector3(payload, offset, value)
                        || !std::isfinite(value.x) || !std::isfinite(value.y) || !std::isfinite(value.z)) return false;
            }
            if (offset != payload.size()) return false;
            ScaleTracksComponent* tracks = scene->findComponent<ScaleTracksComponent>(entity);
            if (!tracks) { scene->addComponent<ScaleTracksComponent>(entity); tracks = scene->findComponent<ScaleTracksComponent>(entity); }
            if (!tracks) return false; *tracks = std::move(decoded); return true;
        }
    });
    (void)scaleTracksRegistered;

    static const bool translateTracksRegistered = NetworkSystem::registerReplicatedComponent({
        "TranslateTracksComponent", 1,
        [](Scene* scene, Entity entity, std::string& payload) {
            const TranslateTracksComponent* tracks = scene->findComponent<TranslateTracksComponent>(entity);
            if (!tracks || tracks->values.size() > 1024 || tracks->inTangents.size() > 1024 || tracks->outTangents.size() > 1024) return false;
            const std::vector<Vector3>* lists[] = {&tracks->values, &tracks->inTangents, &tracks->outTangents};
            for (const auto* list : lists) {
                if (list->size() > 1024) return false;
                appendU32(payload, static_cast<uint32_t>(list->size()));
                for (const Vector3& value : *list) {
                    if (!std::isfinite(value.x) || !std::isfinite(value.y) || !std::isfinite(value.z)) return false;
                    appendVector3(payload, value);
                }
            }
            return payload.size() <= MAX_COMPONENT_PAYLOAD_BYTES;
        },
        [](uint16_t version, Scene* scene, Entity entity, const std::string& payload) {
            if (version != 1) return false;
            size_t offset = 0;
            TranslateTracksComponent decoded;
            std::vector<Vector3>* lists[] = {&decoded.values, &decoded.inTangents, &decoded.outTangents};
            for (auto* list : lists) {
                uint32_t valueCount = 0;
                if (!readU32(payload, offset, valueCount) || valueCount > 1024) return false;
                list->resize(valueCount);
                for (Vector3& value : *list) if (!readVector3(payload, offset, value)
                        || !std::isfinite(value.x) || !std::isfinite(value.y) || !std::isfinite(value.z)) return false;
            }
            if (offset != payload.size()) return false;
            TranslateTracksComponent* tracks = scene->findComponent<TranslateTracksComponent>(entity);
            if (!tracks) { scene->addComponent<TranslateTracksComponent>(entity); tracks = scene->findComponent<TranslateTracksComponent>(entity); }
            if (!tracks) return false; *tracks = std::move(decoded); return true;
        }
    });
    (void)translateTracksRegistered;

    static const bool rotateTracksRegistered = NetworkSystem::registerReplicatedComponent({
        "RotateTracksComponent", 1,
        [](Scene* scene, Entity entity, std::string& payload) {
            const RotateTracksComponent* tracks = scene->findComponent<RotateTracksComponent>(entity);
            if (!tracks || tracks->values.size() > 1024 || tracks->inTangents.size() > 1024 || tracks->outTangents.size() > 1024) return false;
            const std::vector<Quaternion>* lists[] = {&tracks->values, &tracks->inTangents, &tracks->outTangents};
            for (const auto* list : lists) {
                if (list->size() > 1024) return false;
                appendU32(payload, static_cast<uint32_t>(list->size()));
                for (const Quaternion& value : *list) {
                    if (!std::isfinite(value.w) || !std::isfinite(value.x) || !std::isfinite(value.y) || !std::isfinite(value.z)) return false;
                    appendFloat(payload, value.w); appendFloat(payload, value.x); appendFloat(payload, value.y); appendFloat(payload, value.z);
                }
            }
            return payload.size() <= MAX_COMPONENT_PAYLOAD_BYTES;
        },
        [](uint16_t version, Scene* scene, Entity entity, const std::string& payload) {
            if (version != 1) return false;
            size_t offset = 0;
            RotateTracksComponent decoded;
            std::vector<Quaternion>* lists[] = {&decoded.values, &decoded.inTangents, &decoded.outTangents};
            for (auto* list : lists) {
                uint32_t valueCount = 0;
                if (!readU32(payload, offset, valueCount) || valueCount > 1024) return false;
                list->resize(valueCount);
                for (Quaternion& value : *list) if (!readFloat(payload, offset, value.w) || !readFloat(payload, offset, value.x)
                        || !readFloat(payload, offset, value.y) || !readFloat(payload, offset, value.z)
                        || !std::isfinite(value.w) || !std::isfinite(value.x) || !std::isfinite(value.y) || !std::isfinite(value.z)) return false;
            }
            if (offset != payload.size()) return false;
            RotateTracksComponent* tracks = scene->findComponent<RotateTracksComponent>(entity);
            if (!tracks) { scene->addComponent<RotateTracksComponent>(entity); tracks = scene->findComponent<RotateTracksComponent>(entity); }
            if (!tracks) return false; *tracks = std::move(decoded); return true;
        }
    });
    (void)rotateTracksRegistered;

    static const bool actionRegistered = NetworkSystem::registerReplicatedComponent({
        "ActionComponent", 1,
        [](Scene* scene, Entity entity, std::string& payload) {
            const ActionComponent* action = scene->findComponent<ActionComponent>(entity);
            if (!action || !std::isfinite(action->timecount) || !std::isfinite(action->speed)
                    || !std::isfinite(action->startOffset) || !std::isfinite(action->phase)
                    || !std::isfinite(action->weight) || !std::isfinite(action->blendLayerWeight)) return false;
            appendU8(payload, static_cast<uint8_t>(action->state));
            appendFloat(payload, action->timecount);
            appendU8(payload, action->startTrigger ? 1 : 0);
            appendU8(payload, action->stopTrigger ? 1 : 0);
            appendU8(payload, action->pauseTrigger ? 1 : 0);
            appendU8(payload, action->ownedTarget ? 1 : 0);
            appendU32(payload, static_cast<uint32_t>(action->target));
            appendFloat(payload, action->speed);
            appendFloat(payload, action->startOffset);
            appendU8(payload, action->randomStart ? 1 : 0);
            appendFloat(payload, action->phase);
            appendFloat(payload, action->weight);
            appendU32(payload, action->blendLayer);
            appendFloat(payload, action->blendLayerWeight);
            appendU8(payload, action->blendLayerAdditive ? 1 : 0);
            return payload.size() <= MAX_COMPONENT_PAYLOAD_BYTES;
        },
        [](uint16_t version, Scene* scene, Entity entity, const std::string& payload) {
            if (version != 1) return false;
            size_t offset = 0;
            ActionComponent decoded;
            uint8_t state = 0;
            uint8_t flags[5]{};
            uint32_t target = 0;
            if (!readU8(payload, offset, state) || state > static_cast<uint8_t>(ActionState::Stopped)
                    || !readFloat(payload, offset, decoded.timecount)
                    || !readU8(payload, offset, flags[0]) || !readU8(payload, offset, flags[1])
                    || !readU8(payload, offset, flags[2]) || !readU8(payload, offset, flags[3])
                    || !readU32(payload, offset, target) || !readFloat(payload, offset, decoded.speed)
                    || !readFloat(payload, offset, decoded.startOffset) || !readU8(payload, offset, flags[4])
                    || !readFloat(payload, offset, decoded.phase) || !readFloat(payload, offset, decoded.weight)
                    || !readU32(payload, offset, decoded.blendLayer) || !readFloat(payload, offset, decoded.blendLayerWeight)) return false;
            uint8_t additive = 0;
            if (!readU8(payload, offset, additive) || offset != payload.size()) return false;
            for (uint8_t flag : flags) if (flag > 1) return false;
            if (additive > 1 || !std::isfinite(decoded.timecount) || !std::isfinite(decoded.speed)
                    || !std::isfinite(decoded.startOffset) || !std::isfinite(decoded.phase)
                    || !std::isfinite(decoded.weight) || !std::isfinite(decoded.blendLayerWeight)) return false;
            decoded.state = static_cast<ActionState>(state);
            decoded.startTrigger = flags[0] != 0;
            decoded.stopTrigger = flags[1] != 0;
            decoded.pauseTrigger = flags[2] != 0;
            decoded.ownedTarget = flags[3] != 0;
            decoded.target = static_cast<Entity>(target);
            decoded.randomStart = flags[4] != 0;
            decoded.blendLayerAdditive = additive != 0;
            ActionComponent* action = scene->findComponent<ActionComponent>(entity);
            if (!action) { scene->addComponent<ActionComponent>(entity); action = scene->findComponent<ActionComponent>(entity); }
            if (!action) return false;
            *action = std::move(decoded);
            return true;
        }
    });
    (void)actionRegistered;

    static const bool positionActionRegistered = NetworkSystem::registerReplicatedComponent({
        "PositionActionComponent", 1,
        [](Scene* scene, Entity entity, std::string& payload) {
            const PositionActionComponent* action = scene->findComponent<PositionActionComponent>(entity);
            if (!action || !std::isfinite(action->startPosition.x) || !std::isfinite(action->startPosition.y)
                    || !std::isfinite(action->startPosition.z) || !std::isfinite(action->endPosition.x)
                    || !std::isfinite(action->endPosition.y) || !std::isfinite(action->endPosition.z)) return false;
            appendVector3(payload, action->startPosition); appendVector3(payload, action->endPosition);
            return payload.size() <= MAX_COMPONENT_PAYLOAD_BYTES;
        },
        [](uint16_t version, Scene* scene, Entity entity, const std::string& payload) {
            if (version != 1) return false;
            size_t offset = 0; PositionActionComponent decoded;
            if (!readVector3(payload, offset, decoded.startPosition) || !readVector3(payload, offset, decoded.endPosition)
                    || offset != payload.size() || !std::isfinite(decoded.startPosition.x)
                    || !std::isfinite(decoded.startPosition.y) || !std::isfinite(decoded.startPosition.z)
                    || !std::isfinite(decoded.endPosition.x) || !std::isfinite(decoded.endPosition.y)
                    || !std::isfinite(decoded.endPosition.z)) return false;
            PositionActionComponent* action = scene->findComponent<PositionActionComponent>(entity);
            if (!action) { scene->addComponent<PositionActionComponent>(entity); action = scene->findComponent<PositionActionComponent>(entity); }
            if (!action) return false; *action = decoded; return true;
        }
    });
    (void)positionActionRegistered;

    static const bool rotationActionRegistered = NetworkSystem::registerReplicatedComponent({
        "RotationActionComponent", 1,
        [](Scene* scene, Entity entity, std::string& payload) {
            const RotationActionComponent* action = scene->findComponent<RotationActionComponent>(entity);
            if (!action || !std::isfinite(action->startRotation.w) || !std::isfinite(action->startRotation.x)
                    || !std::isfinite(action->startRotation.y) || !std::isfinite(action->startRotation.z)
                    || !std::isfinite(action->endRotation.w) || !std::isfinite(action->endRotation.x)
                    || !std::isfinite(action->endRotation.y) || !std::isfinite(action->endRotation.z)
                    || !std::isfinite(action->axis.x) || !std::isfinite(action->axis.y)
                    || !std::isfinite(action->axis.z) || !std::isfinite(action->angle)) return false;
            appendFloat(payload, action->startRotation.w); appendFloat(payload, action->startRotation.x);
            appendFloat(payload, action->startRotation.y); appendFloat(payload, action->startRotation.z);
            appendFloat(payload, action->endRotation.w); appendFloat(payload, action->endRotation.x);
            appendFloat(payload, action->endRotation.y); appendFloat(payload, action->endRotation.z);
            appendU8(payload, action->shortestPath ? 1 : 0); appendU8(payload, action->spin ? 1 : 0);
            appendVector3(payload, action->axis); appendFloat(payload, action->angle);
            return payload.size() <= MAX_COMPONENT_PAYLOAD_BYTES;
        },
        [](uint16_t version, Scene* scene, Entity entity, const std::string& payload) {
            if (version != 1) return false;
            size_t offset = 0; RotationActionComponent decoded; uint8_t shortest = 0; uint8_t spin = 0;
            if (!readFloat(payload, offset, decoded.startRotation.w) || !readFloat(payload, offset, decoded.startRotation.x)
                    || !readFloat(payload, offset, decoded.startRotation.y) || !readFloat(payload, offset, decoded.startRotation.z)
                    || !readFloat(payload, offset, decoded.endRotation.w) || !readFloat(payload, offset, decoded.endRotation.x)
                    || !readFloat(payload, offset, decoded.endRotation.y) || !readFloat(payload, offset, decoded.endRotation.z)
                    || !readU8(payload, offset, shortest) || !readU8(payload, offset, spin)
                    || !readVector3(payload, offset, decoded.axis) || !readFloat(payload, offset, decoded.angle)
                    || shortest > 1 || spin > 1 || offset != payload.size()) return false;
            if (!std::isfinite(decoded.startRotation.w) || !std::isfinite(decoded.startRotation.x)
                    || !std::isfinite(decoded.startRotation.y) || !std::isfinite(decoded.startRotation.z)
                    || !std::isfinite(decoded.endRotation.w) || !std::isfinite(decoded.endRotation.x)
                    || !std::isfinite(decoded.endRotation.y) || !std::isfinite(decoded.endRotation.z)
                    || !std::isfinite(decoded.axis.x) || !std::isfinite(decoded.axis.y)
                    || !std::isfinite(decoded.axis.z) || !std::isfinite(decoded.angle)) return false;
            decoded.shortestPath = shortest != 0; decoded.spin = spin != 0;
            RotationActionComponent* action = scene->findComponent<RotationActionComponent>(entity);
            if (!action) { scene->addComponent<RotationActionComponent>(entity); action = scene->findComponent<RotationActionComponent>(entity); }
            if (!action) return false; *action = decoded; return true;
        }
    });
    (void)rotationActionRegistered;

    static const bool scaleActionRegistered = NetworkSystem::registerReplicatedComponent({
        "ScaleActionComponent", 1,
        [](Scene* scene, Entity entity, std::string& payload) {
            const ScaleActionComponent* action = scene->findComponent<ScaleActionComponent>(entity);
            if (!action) return false; appendVector3(payload, action->startScale); appendVector3(payload, action->endScale);
            return payload.size() <= MAX_COMPONENT_PAYLOAD_BYTES;
        },
        [](uint16_t version, Scene* scene, Entity entity, const std::string& payload) {
            if (version != 1) return false; size_t offset = 0; ScaleActionComponent decoded;
            if (!readVector3(payload, offset, decoded.startScale) || !readVector3(payload, offset, decoded.endScale) || offset != payload.size()) return false;
            ScaleActionComponent* action = scene->findComponent<ScaleActionComponent>(entity);
            if (!action) { scene->addComponent<ScaleActionComponent>(entity); action = scene->findComponent<ScaleActionComponent>(entity); }
            if (!action) return false; *action = decoded; return true;
        }
    });
    (void)scaleActionRegistered;

    static const bool colorActionRegistered = NetworkSystem::registerReplicatedComponent({
        "ColorActionComponent", 1,
        [](Scene* scene, Entity entity, std::string& payload) {
            const ColorActionComponent* action = scene->findComponent<ColorActionComponent>(entity);
            if (!action) return false; appendVector3(payload, action->startColor); appendVector3(payload, action->endColor); appendU8(payload, action->useSRGB ? 1 : 0);
            return payload.size() <= MAX_COMPONENT_PAYLOAD_BYTES;
        },
        [](uint16_t version, Scene* scene, Entity entity, const std::string& payload) {
            if (version != 1) return false; size_t offset = 0; ColorActionComponent decoded; uint8_t srgb = 0;
            if (!readVector3(payload, offset, decoded.startColor) || !readVector3(payload, offset, decoded.endColor) || !readU8(payload, offset, srgb) || srgb > 1 || offset != payload.size()) return false;
            decoded.useSRGB = srgb != 0; ColorActionComponent* action = scene->findComponent<ColorActionComponent>(entity);
            if (!action) { scene->addComponent<ColorActionComponent>(entity); action = scene->findComponent<ColorActionComponent>(entity); }
            if (!action) return false; *action = decoded; return true;
        }
    });
    (void)colorActionRegistered;

    static const bool alphaActionRegistered = NetworkSystem::registerReplicatedComponent({
        "AlphaActionComponent", 1,
        [](Scene* scene, Entity entity, std::string& payload) {
            const AlphaActionComponent* action = scene->findComponent<AlphaActionComponent>(entity);
            if (!action || !std::isfinite(action->startAlpha) || !std::isfinite(action->endAlpha)) return false;
            appendFloat(payload, action->startAlpha); appendFloat(payload, action->endAlpha); return payload.size() <= MAX_COMPONENT_PAYLOAD_BYTES;
        },
        [](uint16_t version, Scene* scene, Entity entity, const std::string& payload) {
            if (version != 1) return false; size_t offset = 0; AlphaActionComponent decoded;
            if (!readFloat(payload, offset, decoded.startAlpha) || !readFloat(payload, offset, decoded.endAlpha) || offset != payload.size() || !std::isfinite(decoded.startAlpha) || !std::isfinite(decoded.endAlpha)) return false;
            AlphaActionComponent* action = scene->findComponent<AlphaActionComponent>(entity);
            if (!action) { scene->addComponent<AlphaActionComponent>(entity); action = scene->findComponent<AlphaActionComponent>(entity); }
            if (!action) return false; *action = decoded; return true;
        }
    });
    (void)alphaActionRegistered;

    static const bool timedActionRegistered = NetworkSystem::registerReplicatedComponent({
        "TimedActionComponent", 1,
        [](Scene* scene, Entity entity, std::string& payload) {
            const TimedActionComponent* action = scene->findComponent<TimedActionComponent>(entity);
            if (!action || action->function.getType() == EaseType::CUSTOM || !std::isfinite(action->time)
                    || !std::isfinite(action->value) || !std::isfinite(action->duration)) return false;
            appendFloat(payload, action->time); appendFloat(payload, action->value); appendFloat(payload, action->duration);
            appendU8(payload, action->loop ? 1 : 0); appendU8(payload, action->pingPong ? 1 : 0); appendU8(payload, static_cast<uint8_t>(action->function.getType()));
            return payload.size() <= MAX_COMPONENT_PAYLOAD_BYTES;
        },
        [](uint16_t version, Scene* scene, Entity entity, const std::string& payload) {
            if (version != 1) return false; size_t offset = 0; TimedActionComponent decoded; uint8_t loop = 0; uint8_t pingPong = 0; uint8_t ease = 0;
            if (!readFloat(payload, offset, decoded.time) || !readFloat(payload, offset, decoded.value) || !readFloat(payload, offset, decoded.duration)
                    || !readU8(payload, offset, loop) || !readU8(payload, offset, pingPong) || !readU8(payload, offset, ease)
                    || loop > 1 || pingPong > 1 || ease >= static_cast<uint8_t>(EaseType::CUSTOM) || offset != payload.size()
                    || !std::isfinite(decoded.time) || !std::isfinite(decoded.value) || !std::isfinite(decoded.duration)) return false;
            decoded.loop = loop != 0; decoded.pingPong = pingPong != 0; decoded.function.setType(static_cast<EaseType>(ease));
            TimedActionComponent* action = scene->findComponent<TimedActionComponent>(entity);
            if (!action) { scene->addComponent<TimedActionComponent>(entity); action = scene->findComponent<TimedActionComponent>(entity); }
            if (!action) return false; *action = decoded; return true;
        }
    });
    (void)timedActionRegistered;

    static const bool polygonRegistered = NetworkSystem::registerReplicatedComponent({
        "PolygonComponent", 1,
        [](Scene* scene, Entity entity, std::string& payload) {
            const PolygonComponent* polygon = scene->findComponent<PolygonComponent>(entity);
            if (!polygon || polygon->points.size() > 4096) return false;
            appendU32(payload, static_cast<uint32_t>(polygon->points.size()));
            for (const PolygonPoint& point : polygon->points) {
                if (!std::isfinite(point.position.x) || !std::isfinite(point.position.y)
                        || !std::isfinite(point.position.z) || !std::isfinite(point.color.x)
                        || !std::isfinite(point.color.y) || !std::isfinite(point.color.z)
                        || !std::isfinite(point.color.w)) return false;
                appendVector3(payload, point.position);
                appendFloat(payload, point.color.x); appendFloat(payload, point.color.y);
                appendFloat(payload, point.color.z); appendFloat(payload, point.color.w);
            }
            return payload.size() <= MAX_COMPONENT_PAYLOAD_BYTES;
        },
        [](uint16_t version, Scene* scene, Entity entity, const std::string& payload) {
            if (version != 1) return false;
            size_t offset = 0;
            uint32_t count = 0;
            if (!readU32(payload, offset, count) || count > 4096) return false;
            PolygonComponent decoded;
            decoded.points.resize(count);
            for (PolygonPoint& point : decoded.points) {
                if (!readVector3(payload, offset, point.position) || !readVector4(payload, offset, point.color)
                        || !std::isfinite(point.position.x) || !std::isfinite(point.position.y)
                        || !std::isfinite(point.position.z) || !std::isfinite(point.color.x)
                        || !std::isfinite(point.color.y) || !std::isfinite(point.color.z)
                        || !std::isfinite(point.color.w)) return false;
            }
            if (offset != payload.size()) return false;
            decoded.needUpdatePolygon = true;
            PolygonComponent* polygon = scene->findComponent<PolygonComponent>(entity);
            if (!polygon) { scene->addComponent<PolygonComponent>(entity); polygon = scene->findComponent<PolygonComponent>(entity); }
            if (!polygon) return false;
            *polygon = std::move(decoded);
            return true;
        }
    });
    (void)polygonRegistered;

    static const bool registered = NetworkSystem::registerReplicatedComponent({
        "TagComponent", 1,
        [](Scene* scene, Entity entity, std::string& payload) {
            const TagComponent* tag = scene->findComponent<TagComponent>(entity);
            if (!tag) return false;
            appendU64(payload, tag->categories);
            appendU8(payload, tag->layer);
            appendString(payload, tag->tag);
            return payload.size() <= MAX_COMPONENT_PAYLOAD_BYTES;
        },
        [](uint16_t version, Scene* scene, Entity entity, const std::string& payload) {
            if (version != 1) return false;
            size_t offset = 0;
            uint64_t categories = 0;
            uint8_t layer = 0;
            std::string tagValue;
            if (!readU64(payload, offset, categories) || !readU8(payload, offset, layer)
                    || !readBoundedString(payload, offset, MAX_ENTITY_TAG_BYTES, tagValue)
                    || offset != payload.size()) return false;
            TagComponent* tag = scene->findComponent<TagComponent>(entity);
            if (!tag) {
                scene->addComponent<TagComponent>(entity);
                tag = scene->findComponent<TagComponent>(entity);
            }
            if (!tag) return false;
            tag->categories = categories;
            tag->layer = layer;
            tag->tag = tagValue;
            return true;
        }
    });
    (void)registered;

    static const bool soundRegistered = NetworkSystem::registerReplicatedComponent({
        "SoundComponent", 1,
        [](Scene* scene, Entity entity, std::string& payload) {
            const SoundComponent* sound = scene->findComponent<SoundComponent>(entity);
            if (!sound || !std::isfinite(sound->volume) || !std::isfinite(sound->speed)
                    || !std::isfinite(sound->pan) || !std::isfinite(sound->loopingPoint)
                    || !std::isfinite(sound->minDistance) || !std::isfinite(sound->maxDistance)
                    || !std::isfinite(sound->attenuationRolloffFactor)
                    || !std::isfinite(sound->dopplerFactor)
                    || !std::isfinite(sound->occlusionVolume)
                    || !std::isfinite(sound->occlusionUpdateInterval)) return false;
            appendString(payload, sound->filename);
            appendString(payload, sound->bus);
            appendFloat(payload, static_cast<float>(sound->volume));
            appendFloat(payload, sound->speed);
            appendFloat(payload, sound->pan);
            appendU8(payload, sound->looping ? 1 : 0);
            appendFloat(payload, static_cast<float>(sound->loopingPoint));
            appendU8(payload, sound->protectVoice ? 1 : 0);
            appendU8(payload, sound->inaudibleBehaviorMustTick ? 1 : 0);
            appendU8(payload, sound->inaudibleBehaviorKill ? 1 : 0);
            appendFloat(payload, sound->minDistance);
            appendFloat(payload, sound->maxDistance);
            appendU8(payload, static_cast<uint8_t>(sound->attenuationModel));
            appendFloat(payload, sound->attenuationRolloffFactor);
            appendFloat(payload, sound->dopplerFactor);
            appendU8(payload, sound->occlusionEnabled ? 1 : 0);
            appendFloat(payload, sound->occlusionVolume);
            appendFloat(payload, sound->occlusionUpdateInterval);
            return payload.size() <= MAX_COMPONENT_PAYLOAD_BYTES;
        },
        [](uint16_t version, Scene* scene, Entity entity, const std::string& payload) {
            if (version != 1) return false;
            size_t offset = 0;
            std::string filename;
            std::string bus;
            float volume = 0.0f;
            float speed = 0.0f;
            float pan = 0.0f;
            uint8_t looping = 0;
            float loopingPoint = 0.0f;
            uint8_t protectVoice = 0;
            uint8_t inaudibleMustTick = 0;
            uint8_t inaudibleKill = 0;
            float minDistance = 0.0f;
            float maxDistance = 0.0f;
            uint8_t attenuation = 0;
            float rolloff = 0.0f;
            float doppler = 0.0f;
            uint8_t occlusion = 0;
            float occlusionVolume = 0.0f;
            float occlusionInterval = 0.0f;
            if (!readBoundedString(payload, offset, MAX_COMPONENT_PAYLOAD_BYTES, filename)
                    || !readBoundedString(payload, offset, MAX_COMPONENT_PAYLOAD_BYTES, bus)
                    || !readFloat(payload, offset, volume) || !readFloat(payload, offset, speed)
                    || !readFloat(payload, offset, pan) || !readU8(payload, offset, looping)
                    || looping > 1 || !readFloat(payload, offset, loopingPoint)
                    || !readU8(payload, offset, protectVoice) || protectVoice > 1
                    || !readU8(payload, offset, inaudibleMustTick) || inaudibleMustTick > 1
                    || !readU8(payload, offset, inaudibleKill) || inaudibleKill > 1
                    || !readFloat(payload, offset, minDistance) || !readFloat(payload, offset, maxDistance)
                    || !readU8(payload, offset, attenuation) || attenuation > static_cast<uint8_t>(SoundAttenuation::EXPONENTIAL_DISTANCE)
                    || !readFloat(payload, offset, rolloff) || !readFloat(payload, offset, doppler)
                    || !readU8(payload, offset, occlusion) || occlusion > 1
                    || !readFloat(payload, offset, occlusionVolume)
                    || !readFloat(payload, offset, occlusionInterval) || offset != payload.size()) return false;
            SoundComponent* sound = scene->findComponent<SoundComponent>(entity);
            if (!sound) {
                scene->addComponent<SoundComponent>(entity);
                sound = scene->findComponent<SoundComponent>(entity);
            }
            if (!sound) return false;
            sound->filename = std::move(filename);
            sound->bus = std::move(bus);
            sound->volume = volume;
            sound->speed = speed;
            sound->pan = pan;
            sound->looping = looping != 0;
            sound->loopingPoint = loopingPoint;
            sound->protectVoice = protectVoice != 0;
            sound->inaudibleBehaviorMustTick = inaudibleMustTick != 0;
            sound->inaudibleBehaviorKill = inaudibleKill != 0;
            sound->minDistance = minDistance;
            sound->maxDistance = maxDistance;
            sound->attenuationModel = static_cast<SoundAttenuation>(attenuation);
            sound->attenuationRolloffFactor = rolloff;
            sound->dopplerFactor = doppler;
            sound->occlusionEnabled = occlusion != 0;
            sound->occlusionVolume = occlusionVolume;
            sound->occlusionUpdateInterval = occlusionInterval;
            sound->needUpdate = true;
            return true;
        }
    });
    (void)soundRegistered;

    static const bool particlesRegistered = NetworkSystem::registerReplicatedComponent({
        "ParticlesComponent", 2,
        [](Scene* scene, Entity entity, std::string& payload) {
            const ParticlesComponent* particles = scene->findComponent<ParticlesComponent>(entity);
            if (!particles || !std::isfinite(particles->collisionBounce)
                    || !std::isfinite(particles->collisionFriction)) return false;
            appendU32(payload, particles->maxParticles);
            appendU8(payload, particles->emitter ? 1 : 0);
            appendU8(payload, particles->loop ? 1 : 0);
            appendU8(payload, particles->localSpace ? 1 : 0);
            appendU8(payload, particles->gpuSimulation ? 1 : 0);
            appendU8(payload, particles->collisionEnabled ? 1 : 0);
            appendFloat(payload, particles->collisionBounce);
            appendFloat(payload, particles->collisionFriction);
            appendU8(payload, particles->trailsEnabled ? 1 : 0);
            appendU32(payload, particles->trailLength);
            appendU32(payload, static_cast<uint32_t>(std::max(0, particles->rate)));
            appendU32(payload, static_cast<uint32_t>(std::max(0, particles->maxPerUpdate)));
            return payload.size() <= MAX_COMPONENT_PAYLOAD_BYTES;
        },
        [](uint16_t version, Scene* scene, Entity entity, const std::string& payload) {
            if (version != 1 && version != 2) return false;
            size_t offset = 0;
            uint32_t maxParticles = 0;
            uint8_t emitter = 0;
            uint8_t loop = 0;
            uint8_t localSpace = 0;
            uint8_t gpuSimulation = 0;
            uint8_t collisionEnabled = 0;
            float bounce = 0.0f;
            float friction = 0.0f;
            uint8_t trailsEnabled = 0;
            uint32_t trailLength = 0;
            uint32_t rate = 0;
            uint32_t maxPerUpdate = 0;
            if (!readU32(payload, offset, maxParticles) || maxParticles > 1000000
                    || !readU8(payload, offset, emitter) || emitter > 1
                    || !readU8(payload, offset, loop) || loop > 1
                    || !readU8(payload, offset, localSpace) || localSpace > 1
                    || (version >= 2 && (!readU8(payload, offset, gpuSimulation) || gpuSimulation > 1))
                    || !readU8(payload, offset, collisionEnabled) || collisionEnabled > 1
                    || !readFloat(payload, offset, bounce) || !readFloat(payload, offset, friction)
                    || !std::isfinite(bounce) || !std::isfinite(friction)
                    || !readU8(payload, offset, trailsEnabled) || trailsEnabled > 1
                    || !readU32(payload, offset, trailLength) || trailLength > 64
                    || !readU32(payload, offset, rate) || rate > 1000000
                    || !readU32(payload, offset, maxPerUpdate) || maxPerUpdate > 1000000
                    || offset != payload.size()) return false;
            ParticlesComponent* particles = scene->findComponent<ParticlesComponent>(entity);
            if (!particles) {
                scene->addComponent<ParticlesComponent>(entity);
                particles = scene->findComponent<ParticlesComponent>(entity);
            }
            if (!particles) return false;
            particles->maxParticles = maxParticles;
            particles->emitter = emitter != 0;
            particles->loop = loop != 0;
            particles->localSpace = localSpace != 0;
            particles->gpuSimulation = gpuSimulation != 0;
            particles->collisionEnabled = collisionEnabled != 0;
            particles->collisionBounce = std::clamp(bounce, 0.0f, 1.0f);
            particles->collisionFriction = std::clamp(friction, 0.0f, 1.0f);
            particles->trailsEnabled = trailsEnabled != 0;
            particles->trailLength = trailLength;
            particles->rate = static_cast<int>(rate);
            particles->maxPerUpdate = static_cast<int>(maxPerUpdate);
            particles->particles.clear();
            particles->trailHistory.clear();
            particles->trailTarget = NULL_ENTITY;
            return true;
        }
    });
    (void)particlesRegistered;
}

std::vector<NetworkSystem::ReplicatedComponentSerializer> replicatedComponentSnapshot() {
    std::lock_guard<std::mutex> lock(replicatedComponentMutex);
    std::vector<NetworkSystem::ReplicatedComponentSerializer> result;
    for (const auto& entry : replicatedComponentSerializers) result.push_back(entry.second);
    return result;
}

bool readFloat(const std::string& input, size_t& offset, float& value) {
    uint32_t bits = 0;
    if (!readU32(input, offset, bits)) return false;
    std::memcpy(&value, &bits, sizeof(value));
    return std::isfinite(value);
}

} // namespace

NetworkSystem::NetworkSystem(Scene* scene) : SubSystem(scene) {}

NetworkSystem::~NetworkSystem() {
    destroy();
}

void NetworkSystem::startServer(uint16_t port) {
    if (mode != Mode::None) return;
    connectedState = false;
    localPeerId = 0;
    connectedPeerCount = 0;
    relayEnabled = false;
    relayReady = false;
    relayPeerPort = 0;
    mode = Mode::Server;
    listenPort = port;
    running = true;
    networkThread = std::thread(&NetworkSystem::networkLoop, this);
    std::cout << "[Network] Server started on port " << port << "\n";
}

void NetworkSystem::startClient(const std::string& address, uint16_t port, uint32_t reservationId) {
    if (mode != Mode::None) return;
    connectedState = false;
    localPeerId = 0;
    connectedPeerCount = 0;
    relayEnabled = false;
    relayReady = false;
    relayPeerPort = 0;
    clientReservationId = reservationId;
    mode = Mode::Client;
    serverAddress = address;
    listenPort = port;
    std::random_device random;
    clientNonce = random();
    running = true;
    networkThread = std::thread(&NetworkSystem::networkLoop, this);
    std::cout << "[Network] Client connecting to " << address << ":" << port << "\n";
}

void NetworkSystem::startRelayServer(const std::string& address, uint16_t port,
    const std::string& room, const std::string& token, uint16_t localPort) {
    if (mode != Mode::None || address.empty() || port == 0 || room.empty()) return;
    connectedState = false;
    localPeerId = 0;
    connectedPeerCount = 0;
    relayEnabled = true;
    relayReady = false;
    relayPeerPort = 0;
    relayAddress = address;
    relayPort = port;
    relayRoom = room;
    relayToken = token;
    mode = Mode::Server;
    listenPort = localPort;
    running = true;
    networkThread = std::thread(&NetworkSystem::networkLoop, this);
    std::cout << "[Network] Relay server registering room '" << room
        << "' at " << address << ":" << port << "\n";
}

void NetworkSystem::startRelayClient(const std::string& address, uint16_t port,
    const std::string& room, const std::string& token) {
    if (mode != Mode::None || address.empty() || port == 0 || room.empty()) return;
    connectedState = false;
    localPeerId = 0;
    connectedPeerCount = 0;
    clientReservationId = 0;
    relayEnabled = true;
    relayReady = false;
    relayPeerPort = 0;
    relayAddress = address;
    relayPort = port;
    relayRoom = room;
    relayToken = token;
    mode = Mode::Client;
    listenPort = 0;
    serverAddress = address;
    std::random_device random;
    clientNonce = random();
    running = true;
    networkThread = std::thread(&NetworkSystem::networkLoop, this);
    std::cout << "[Network] Relay client joining room '" << room
        << "' at " << address << ":" << port << "\n";
}

void NetworkSystem::setAuthToken(const std::string& token) {
    std::lock_guard<std::mutex> lock(authMutex);
    authToken = token;
}

void NetworkSystem::setMaxPeers(uint32_t count) {
    maxPeers = std::clamp<uint32_t>(count, 1, 64);
}

void NetworkSystem::setLobbyInfo(const LobbyInfo& info) {
    std::lock_guard<std::mutex> lock(lobbyMutex);
    lobbyInfo = info;
}

NetworkSystem::LobbyInfo NetworkSystem::getLobbyInfo() const {
    std::lock_guard<std::mutex> lock(lobbyMutex);
    return lobbyInfo;
}

std::vector<NetworkSystem::LobbyServer> NetworkSystem::discoverLan(uint16_t discoveryPort,
    uint32_t timeoutMs, const std::string& authToken, const std::string& address) {
    std::vector<LobbyServer> result;
#if defined(_WIN32)
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return result;
#endif
    NetworkSocket socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket == INVALID_NETWORK_SOCKET || !makeNonBlocking(socket)) {
        if (socket != INVALID_NETWORK_SOCKET) closeSocket(socket);
#if defined(_WIN32)
        WSACleanup();
#endif
        return result;
    }

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = 0;
    if (::bind(socket, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0) {
        closeSocket(socket);
#if defined(_WIN32)
        WSACleanup();
#endif
        return result;
    }
    if (address == "255.255.255.255") {
        int enabled = 1;
        ::setsockopt(socket, SOL_SOCKET, SO_BROADCAST,
            reinterpret_cast<const char*>(&enabled), sizeof(enabled));
    }

    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_port = htons(discoveryPort);
    if (inet_pton(AF_INET, address.c_str(), &target.sin_addr) != 1) {
        closeSocket(socket);
#if defined(_WIN32)
        WSACleanup();
#endif
        return result;
    }
    const uint32_t nonce = static_cast<uint32_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const std::vector<uint8_t> query = network::authenticatePacket(
        network::encodeLobbyQuery(nonce, 1), authToken);
    const auto sendQuery = [&]() {
        if (query.size() <= MAX_PACKET_BYTES) {
            ::sendto(socket, reinterpret_cast<const char*>(query.data()), static_cast<int>(query.size()), 0,
                reinterpret_cast<const sockaddr*>(&target), sizeof(target));
        }
    };
    sendQuery();

    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(std::clamp<uint32_t>(timeoutMs, 10, 5000));
    auto nextQuery = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    while (std::chrono::steady_clock::now() < deadline) {
        if (std::chrono::steady_clock::now() >= nextQuery) {
            sendQuery();
            nextQuery += std::chrono::milliseconds(100);
        }
        uint8_t buffer[1500];
        sockaddr_in source{};
#if defined(_WIN32)
        int sourceLength = sizeof(source);
        const int received = ::recvfrom(socket, reinterpret_cast<char*>(buffer), sizeof(buffer), 0,
            reinterpret_cast<sockaddr*>(&source), &sourceLength);
#else
        socklen_t sourceLength = sizeof(source);
        const int received = static_cast<int>(::recvfrom(socket, buffer, sizeof(buffer), 0,
            reinterpret_cast<sockaddr*>(&source), &sourceLength));
#endif
        if (received > 0) {
            size_t authenticatedSize = static_cast<size_t>(received);
            if (network::verifyPacketAuthentication(buffer, authenticatedSize, authToken,
                    authenticatedSize)) {
                network::PacketHeader header;
                network::LobbyAdvertisement advertisement;
                if (network::decodeLobbyAdvertisement(buffer, authenticatedSize, header, advertisement)
                        && advertisement.nonce == nonce) {
                    char addressBuffer[INET_ADDRSTRLEN]{};
                    const char* resolved = inet_ntop(AF_INET, &source.sin_addr,
                        addressBuffer, sizeof(addressBuffer));
                    const std::string sourceAddress = resolved ? resolved : address;
                    const auto duplicate = std::find_if(result.begin(), result.end(),
                        [&sourceAddress, &advertisement](const LobbyServer& server) {
                            return server.address == sourceAddress
                                && server.advertisement.gamePort == advertisement.gamePort;
                        });
                    if (duplicate == result.end()) result.push_back({sourceAddress, advertisement});
                }
            }
        } else if (received < 0 && !wouldBlock() && !transientReceiveError()) {
            break;
        }
        if (received <= 0) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    closeSocket(socket);
#if defined(_WIN32)
    WSACleanup();
#endif
    return result;
}

NetworkSystem::LobbyReservation NetworkSystem::reserveLan(uint16_t discoveryPort,
    uint32_t timeoutMs, const std::string& authToken, const std::string& address,
    uint16_t requestedSeconds) {
    LobbyReservation result;
    result.address = address;
    if (authToken.empty()) return result;
#if defined(_WIN32)
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return result;
#endif
    NetworkSocket socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket == INVALID_NETWORK_SOCKET || !makeNonBlocking(socket)) {
        if (socket != INVALID_NETWORK_SOCKET) closeSocket(socket);
#if defined(_WIN32)
        WSACleanup();
#endif
        return result;
    }

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = 0;
    if (::bind(socket, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0) {
        closeSocket(socket);
#if defined(_WIN32)
        WSACleanup();
#endif
        return result;
    }
    if (address == "255.255.255.255") {
        int enabled = 1;
        ::setsockopt(socket, SOL_SOCKET, SO_BROADCAST,
            reinterpret_cast<const char*>(&enabled), sizeof(enabled));
    }

    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_port = htons(discoveryPort);
    if (inet_pton(AF_INET, address.c_str(), &target.sin_addr) != 1) {
        closeSocket(socket);
#if defined(_WIN32)
        WSACleanup();
#endif
        return result;
    }

    const uint32_t nonce = static_cast<uint32_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const uint16_t seconds = std::clamp<uint16_t>(requestedSeconds, 1, 60);
    const std::vector<uint8_t> request = network::authenticatePacket(
        network::encodeLobbyReservationRequest(nonce, seconds, 1), authToken);
    const auto sendRequest = [&]() {
        if (request.size() <= MAX_PACKET_BYTES) {
            ::sendto(socket, reinterpret_cast<const char*>(request.data()),
                static_cast<int>(request.size()), 0,
                reinterpret_cast<const sockaddr*>(&target), sizeof(target));
        }
    };
    sendRequest();

    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(std::clamp<uint32_t>(timeoutMs, 10, 5000));
    auto nextRequest = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    while (std::chrono::steady_clock::now() < deadline) {
        if (std::chrono::steady_clock::now() >= nextRequest) {
            sendRequest();
            nextRequest += std::chrono::milliseconds(100);
        }
        uint8_t buffer[1500];
        sockaddr_in source{};
#if defined(_WIN32)
        int sourceLength = sizeof(source);
        const int received = ::recvfrom(socket, reinterpret_cast<char*>(buffer), sizeof(buffer), 0,
            reinterpret_cast<sockaddr*>(&source), &sourceLength);
#else
        socklen_t sourceLength = sizeof(source);
        const int received = static_cast<int>(::recvfrom(socket, buffer, sizeof(buffer), 0,
            reinterpret_cast<sockaddr*>(&source), &sourceLength));
#endif
        if (received > 0) {
            size_t authenticatedSize = static_cast<size_t>(received);
            if (network::verifyPacketAuthentication(buffer, authenticatedSize, authToken,
                    authenticatedSize)) {
                network::PacketHeader header;
                network::LobbyReservationResponse response;
                if (network::decodeLobbyReservationResponse(buffer, authenticatedSize, header,
                        response) && response.nonce == nonce) {
                    result.reservationId = response.reservationId;
                    result.expiresSeconds = response.expiresSeconds;
                    result.accepted = response.accepted && response.reservationId != 0;
                    char addressBuffer[INET_ADDRSTRLEN]{};
                    const char* resolved = inet_ntop(AF_INET, &source.sin_addr,
                        addressBuffer, sizeof(addressBuffer));
                    result.address = resolved ? resolved : address;
                    break;
                }
            }
        } else if (received < 0 && !wouldBlock() && !transientReceiveError()) {
            break;
        }
        if (received <= 0) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    closeSocket(socket);
#if defined(_WIN32)
    WSACleanup();
#endif
    return result;
}

std::string NetworkSystem::getAuthToken() const {
    std::lock_guard<std::mutex> lock(authMutex);
    return authToken;
}

void NetworkSystem::registerEntity(Entity entity) {
    std::lock_guard<std::mutex> lock(entityMutex);
    if (std::find(replicatedEntities.begin(), replicatedEntities.end(), entity)
            != replicatedEntities.end()) return;
    if (scene) {
        NetworkComponent* networkComponent = scene->findComponent<NetworkComponent>(entity);
        if (networkComponent && networkComponent->netId == 0) {
            networkComponent->netId = nextNetworkEntityId++;
        }
    }
    replicatedEntities.push_back(entity);
}

uint32_t NetworkSystem::sendReliable(const std::vector<uint8_t>& payload) {
    const size_t authBytes = getAuthToken().empty() ? 0 : network::AUTHENTICATION_TAG_SIZE;
    if (payload.size() > MAX_PACKET_BYTES - 20 - authBytes) {
        std::lock_guard<std::mutex> metricsLock(metricsMutex);
        ++metrics.reliableDropped;
        return 0;
    }
    std::lock_guard<std::mutex> lock(reliableMutex);
    if (mode == Mode::Server) {
        if (pendingReliableBroadcast.size() >= MAX_PENDING_RELIABLE) {
            std::lock_guard<std::mutex> metricsLock(metricsMutex);
            ++metrics.reliableDropped;
            return 0;
        }
        const uint32_t sequence = nextReliableSequence++;
        pendingReliableBroadcast.push_back(payload);
        {
            std::lock_guard<std::mutex> metricsLock(metricsMutex);
            ++metrics.reliableSent;
            metrics.queueHighWatermark = std::max(metrics.queueHighWatermark,
                static_cast<uint32_t>(pendingReliableBroadcast.size()));
        }
        return sequence;
    }
    if (pendingReliable.size() >= MAX_PENDING_RELIABLE) {
        std::lock_guard<std::mutex> metricsLock(metricsMutex);
        ++metrics.reliableDropped;
        return 0;
    }
    const uint32_t sequence = nextReliableSequence++;
    pendingReliable.emplace(sequence, PendingReliable{payload});
    {
        std::lock_guard<std::mutex> metricsLock(metricsMutex);
        ++metrics.reliableSent;
        metrics.queueHighWatermark = std::max(metrics.queueHighWatermark,
            static_cast<uint32_t>(pendingReliable.size()));
    }
    return sequence;
}

uint32_t NetworkSystem::sendUnreliable(const std::vector<uint8_t>& payload) {
    const size_t authBytes = getAuthToken().empty() ? 0 : network::AUTHENTICATION_TAG_SIZE;
    if (payload.size() > MAX_PACKET_BYTES - 20 - authBytes) {
        std::lock_guard<std::mutex> metricsLock(metricsMutex);
        ++metrics.unreliableDropped;
        return 0;
    }
    std::lock_guard<std::mutex> lock(reliableMutex);
    if (pendingUnreliable.size() >= 64) {
        pendingUnreliable.pop_front();
        std::lock_guard<std::mutex> metricsLock(metricsMutex);
        ++metrics.unreliableDropped;
    }
    pendingUnreliable.push_back(payload);
    {
        std::lock_guard<std::mutex> metricsLock(metricsMutex);
        ++metrics.unreliableSent;
        metrics.queueHighWatermark = std::max(metrics.queueHighWatermark,
            static_cast<uint32_t>(pendingUnreliable.size()));
    }
    return nextReliableSequence++;
}

uint32_t NetworkSystem::sendReliableMessage(uint16_t messageType, uint16_t schemaVersion,
    const std::vector<uint8_t>& payload) {
    const std::vector<uint8_t> encoded = network::encodeMessage(messageType, schemaVersion,
        payload.data(), payload.size());
    return sendReliable(encoded);
}

bool NetworkSystem::pollReliable(std::vector<uint8_t>& payload) {
    std::lock_guard<std::mutex> lock(reliableMutex);
    if (receivedReliable.empty()) return false;
    payload = std::move(receivedReliable.front());
    receivedReliable.pop_front();
    return true;
}

bool NetworkSystem::pollUnreliable(std::vector<uint8_t>& payload) {
    std::lock_guard<std::mutex> lock(reliableMutex);
    if (receivedUnreliable.empty()) return false;
    payload = std::move(receivedUnreliable.front());
    receivedUnreliable.pop_front();
    return true;
}

bool NetworkSystem::pollReliableMessage(uint16_t& messageType, uint16_t& schemaVersion,
    std::vector<uint8_t>& payload) {
    std::vector<uint8_t> encoded;
    if (!pollReliable(encoded)) return false;
    network::MessageEnvelope message;
    if (!network::decodeMessage(encoded.data(), encoded.size(), message)
            || !network::migrateMessage(message)) return false;
    messageType = message.messageType;
    schemaVersion = message.schemaVersion;
    payload = std::move(message.payload);
    return true;
}

NetworkSystem::Metrics NetworkSystem::getMetrics() const {
    std::lock_guard<std::mutex> queueLock(reliableMutex);
    std::lock_guard<std::mutex> lock(metricsMutex);
    Metrics result = metrics;
    result.reliableQueueDepth = static_cast<uint32_t>(std::min<size_t>(UINT32_MAX,
        pendingReliable.size() + pendingReliableBroadcast.size()));
    result.unreliableQueueDepth = static_cast<uint32_t>(std::min<size_t>(UINT32_MAX, pendingUnreliable.size()));
    result.receivedQueueDepth = static_cast<uint32_t>(std::min<size_t>(UINT32_MAX,
        receivedReliable.size() + receivedUnreliable.size() + receivedSnapshots.size()
            + receivedReplicationPolicies.size()));
    const uint64_t reliableAttempts = result.reliableSent + result.reliableRetransmitted;
    result.packetLossPercent = reliableAttempts == 0 ? 0.0
        : (100.0 * static_cast<double>(result.reliableRetransmitted)
            / static_cast<double>(reliableAttempts));
    return result;
}

void NetworkSystem::setPredictionVelocity(Entity entity, Vector3 velocity) {
    if (!hasEntityAuthority(entity)) return;
    std::lock_guard<std::mutex> lock(entityMutex);
    predictions[entity].velocity = velocity;
}

void NetworkSystem::clearPrediction(Entity entity) {
    std::lock_guard<std::mutex> lock(entityMutex);
    predictions.erase(entity);
}

bool NetworkSystem::hasEntityAuthority(Entity entity) const {
    if (!scene) return false;
    if (mode == Mode::None || mode == Mode::Server) return true;
    const NetworkComponent* component = scene->findComponent<NetworkComponent>(entity);
    if (!component) return false;
    const uint32_t peerId = localPeerId.load(std::memory_order_acquire);
    return peerId != 0 && component->ownerId != 0 && component->ownerId == peerId;
}

void NetworkSystem::setPacketLossSimulationForTesting(uint32_t percent) {
    packetLossSimulationPercent = std::min<uint32_t>(100, percent);
}

void NetworkSystem::setPacketReorderingSimulationForTesting(uint32_t percent) {
    packetReorderSimulationPercent = std::min<uint32_t>(100, percent);
}

void NetworkSystem::setInterestCenter(Vector3 center, float radius, uint32_t group) {
    if (!finiteVector(center) || !std::isfinite(radius)) return;
    std::lock_guard<std::mutex> lock(interestMutex);
    localInterest.center = center;
    localInterest.radius = std::clamp(radius, 0.0f, static_cast<float>(MAX_INTEREST_RADIUS));
    localInterest.group = group;
    localInterest.valid = true;
}

void NetworkSystem::clearInterestCenter() {
    std::lock_guard<std::mutex> lock(interestMutex);
    localInterest = PeerInterest{};
}

bool NetworkSystem::setEntityReplicationPolicy(Entity entity, uint32_t ownerId,
    uint32_t interestGroup, float interestRadius, bool serverAuthority) {
    if (!scene || !std::isfinite(interestRadius) || interestRadius < 0.0f) return false;
    NetworkComponent* component = scene->findComponent<NetworkComponent>(entity);
    if (!component) return false;
    component->ownerId = ownerId;
    component->interestGroup = interestGroup;
    component->interestRadius = std::min(interestRadius, static_cast<float>(MAX_INTEREST_RADIUS));
    component->serverAuthority = serverAuthority;
    return true;
}

bool NetworkSystem::getHistoricalTransform(uint32_t netId, double secondsAgo,
    network::TransformSnapshot& snapshot) const {
    const double clampedSeconds = std::max(0.0, std::min(secondsAgo, 1.0));
    const auto wanted = std::chrono::steady_clock::now()
        - std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(clampedSeconds));
    std::lock_guard<std::mutex> lock(historyMutex);
    const auto found = snapshotHistory.find(netId);
    if (found == snapshotHistory.end() || found->second.empty()) return false;
    const HistoricalSnapshot* best = &found->second.front();
    for (const HistoricalSnapshot& candidate : found->second) {
        if (candidate.time <= wanted) best = &candidate;
        else break;
    }
    snapshot = best->snapshot;
    return true;
}

bool NetworkSystem::registerReplicatedComponent(const ReplicatedComponentSerializer& serializer) {
    if (serializer.name.empty() || serializer.name.size() > MAX_COMPONENT_NAME_BYTES
            || serializer.schemaVersion == 0 || !serializer.encode || !serializer.decode) return false;
    std::lock_guard<std::mutex> lock(replicatedComponentMutex);
    replicatedComponentSerializers[serializer.name] = serializer;
    return true;
}

std::vector<NetworkSystem::ReplicatedComponentDescriptor>
NetworkSystem::getReplicatedComponentCatalog() {
    ensureBuiltInReplicatedComponents();
    const auto serializers = replicatedComponentSnapshot();
    std::vector<ReplicatedComponentDescriptor> result;
    result.reserve(serializers.size());
    for (const auto& serializer : serializers) {
        result.push_back({serializer.name, serializer.schemaVersion});
    }
    return result;
}

bool NetworkSystem::registerServerMessageSchema(uint16_t messageType, uint16_t currentVersion,
    network::SchemaMigration migration, size_t maxPayload, network::SchemaValidation validation) {
    return network::registerMessageSchema(messageType, currentVersion, migration,
        maxPayload, validation);
}

bool NetworkSystem::saveReplicatedState(SaveGame& save, const std::string& key) const {
    if (!scene || key.empty()) return false;
    ensureBuiltInReplicatedComponents();
    const auto serializers = replicatedComponentSnapshot();
    std::vector<Entity> entities;
    {
        std::lock_guard<std::mutex> lock(entityMutex);
        entities = replicatedEntities;
    }
    std::string encoded(ECS_SNAPSHOT_MAGIC, sizeof(ECS_SNAPSHOT_MAGIC) - 1);
    appendU32(encoded, static_cast<uint32_t>(entities.size()));
    uint32_t saved = 0;
    for (Entity entity : entities) {
        const NetworkComponent* networkComponent = scene->findComponent<NetworkComponent>(entity);
        const Transform* transform = scene->findComponent<Transform>(entity);
        if (!networkComponent || !networkComponent->replicateTransform
                || !transform || networkComponent->netId == 0) continue;
        const std::string entityName = scene->getEntityName(entity);
        if (entityName.size() > MAX_ENTITY_NAME_BYTES) return false;
        appendU32(encoded, networkComponent->netId);
        appendString(encoded, entityName);
        appendFloat(encoded, transform->position.x);
        appendFloat(encoded, transform->position.y);
        appendFloat(encoded, transform->position.z);
        appendFloat(encoded, transform->rotation.w);
        appendFloat(encoded, transform->rotation.x);
        appendFloat(encoded, transform->rotation.y);
        appendFloat(encoded, transform->rotation.z);
        appendFloat(encoded, transform->scale.x);
        appendFloat(encoded, transform->scale.y);
        appendFloat(encoded, transform->scale.z);
        const size_t componentCountOffset = encoded.size();
        appendU32(encoded, 0);
        uint32_t componentCount = 0;
        for (const auto& serializer : serializers) {
            if (componentCount >= MAX_COMPONENT_RECORDS) return false;
            std::string payload;
            if (!serializer.encode(scene, entity, payload)) continue;
            if (payload.size() > MAX_COMPONENT_PAYLOAD_BYTES) return false;
            appendString(encoded, serializer.name);
            appendU32(encoded, serializer.schemaVersion);
            appendU32(encoded, static_cast<uint32_t>(payload.size()));
            encoded.append(payload);
            ++componentCount;
        }
        patchU32(encoded, componentCountOffset, componentCount);
        ++saved;
    }
    // The count is written before filtering invalid registrations. Re-encode it
    // in place so a deleted entity can never make the restore parser overrun.
    encoded[sizeof(ECS_SNAPSHOT_MAGIC) - 1] = static_cast<char>(saved & 0xffu);
    encoded[sizeof(ECS_SNAPSHOT_MAGIC)] = static_cast<char>((saved >> 8) & 0xffu);
    encoded[sizeof(ECS_SNAPSHOT_MAGIC) + 1] = static_cast<char>((saved >> 16) & 0xffu);
    encoded[sizeof(ECS_SNAPSHOT_MAGIC) + 2] = static_cast<char>((saved >> 24) & 0xffu);
    save.setString(key, encoded);
    return save.has(key);
}

bool NetworkSystem::loadReplicatedState(const SaveGame& save, const std::string& key) {
    if (!scene || key.empty()) return false;
    const std::string encoded = save.getString(key);
    const size_t magicSize = sizeof(ECS_SNAPSHOT_MAGIC) - 1;
    const bool version3 = encoded.size() >= magicSize
        && encoded.compare(0, magicSize, ECS_SNAPSHOT_MAGIC) == 0;
    const bool version2 = encoded.size() >= sizeof(ECS_SNAPSHOT_MAGIC_V2) - 1
        && encoded.compare(0, sizeof(ECS_SNAPSHOT_MAGIC_V2) - 1, ECS_SNAPSHOT_MAGIC_V2) == 0;
    const bool version1 = encoded.size() >= sizeof(ECS_SNAPSHOT_MAGIC_V1) - 1
        && encoded.compare(0, sizeof(ECS_SNAPSHOT_MAGIC_V1) - 1, ECS_SNAPSHOT_MAGIC_V1) == 0;
    if (!version1 && !version2 && !version3) return false;
    if (version3) ensureBuiltInReplicatedComponents();
    size_t offset = magicSize;
    uint32_t count = 0;
    if (!readU32(encoded, offset, count) || count > 4096) return false;
    std::lock_guard<std::mutex> lock(entityMutex);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t netId = 0;
        std::string entityName;
        uint8_t hasTag = 0;
        uint64_t tagCategories = 0;
        uint8_t tagLayer = 0;
        std::string tagValue;
        float values[10]{};
        struct PendingComponent {
            std::string name;
            uint16_t schemaVersion;
            std::string payload;
        };
        std::vector<PendingComponent> pendingComponents;
        if (!readU32(encoded, offset, netId)) return false;
        if (version2 || version3) {
            if (!readBoundedString(encoded, offset, MAX_ENTITY_NAME_BYTES, entityName)) return false;
            if (version2) {
                if (!readU8(encoded, offset, hasTag) || hasTag > 1) return false;
                if (hasTag && (!readU64(encoded, offset, tagCategories)
                        || !readU8(encoded, offset, tagLayer)
                        || !readBoundedString(encoded, offset, MAX_ENTITY_TAG_BYTES, tagValue))) return false;
            }
        }
        for (float& value : values) if (!readFloat(encoded, offset, value)) return false;
        if (version3) {
            uint32_t componentCount = 0;
            if (!readU32(encoded, offset, componentCount) || componentCount > MAX_COMPONENT_RECORDS) return false;
            pendingComponents.reserve(componentCount);
            for (uint32_t componentIndex = 0; componentIndex < componentCount; ++componentIndex) {
                PendingComponent component;
                uint32_t schemaVersion = 0;
                uint32_t payloadSize = 0;
                if (!readBoundedString(encoded, offset, MAX_COMPONENT_NAME_BYTES, component.name)
                        || !readU32(encoded, offset, schemaVersion) || schemaVersion > UINT16_MAX
                        || !readU32(encoded, offset, payloadSize) || payloadSize > MAX_COMPONENT_PAYLOAD_BYTES
                        || offset + payloadSize > encoded.size()) return false;
                component.schemaVersion = static_cast<uint16_t>(schemaVersion);
                component.payload.assign(encoded.data() + offset, payloadSize);
                offset += payloadSize;
                pendingComponents.push_back(std::move(component));
            }
        }
        for (Entity entity : replicatedEntities) {
            NetworkComponent* networkComponent = scene->findComponent<NetworkComponent>(entity);
            Transform* transform = scene->findComponent<Transform>(entity);
            if (!networkComponent || !transform || networkComponent->netId != netId) continue;
            transform->position = Vector3(values[0], values[1], values[2]);
            transform->rotation = Quaternion(values[3], values[4], values[5], values[6]);
            transform->scale = Vector3(values[7], values[8], values[9]);
            transform->needUpdate = true;
            if (version2 || version3) {
                scene->setEntityName(entity, entityName);
                if (version2 && hasTag) {
                    TagComponent* tag = scene->findComponent<TagComponent>(entity);
                    if (!tag) {
                        scene->addComponent<TagComponent>(entity);
                        tag = scene->findComponent<TagComponent>(entity);
                    }
                    if (tag) {
                        tag->categories = tagCategories;
                        tag->layer = tagLayer;
                        tag->tag = tagValue;
                    }
                }
            }
            if (version3) {
                std::lock_guard<std::mutex> serializerLock(replicatedComponentMutex);
                for (const PendingComponent& component : pendingComponents) {
                    const auto serializer = replicatedComponentSerializers.find(component.name);
                    if (serializer != replicatedComponentSerializers.end()
                            && !serializer->second.decode(component.schemaVersion, scene, entity, component.payload)) return false;
                }
            }
            break;
        }
    }
    return offset == encoded.size();
}

uint32_t NetworkSystem::sendReliableText(const std::string& payload) {
    return sendReliable(std::vector<uint8_t>(payload.begin(), payload.end()));
}

uint32_t NetworkSystem::sendUnreliableText(const std::string& payload) {
    return sendUnreliable(std::vector<uint8_t>(payload.begin(), payload.end()));
}

std::string NetworkSystem::pollReliableText() {
    std::vector<uint8_t> payload;
    if (!pollReliable(payload)) return {};
    return std::string(payload.begin(), payload.end());
}

std::string NetworkSystem::pollUnreliableText() {
    std::vector<uint8_t> payload;
    if (!pollUnreliable(payload)) return {};
    return std::string(payload.begin(), payload.end());
}

void NetworkSystem::update(double dt) {
    (void)dt;
    if (!scene) return;

    const float step = static_cast<float>(std::max(0.0, dt));
    if (step > 0.0f) {
        std::lock_guard<std::mutex> lock(entityMutex);
        for (const auto& prediction : predictions) {
            if (mode == Mode::Client && !hasEntityAuthority(prediction.first)) continue;
            Transform* transform = scene->findComponent<Transform>(prediction.first);
            if (!transform) continue;
            transform->position += prediction.second.velocity * step;
            transform->needUpdate = true;
        }
    }

    std::deque<network::TransformSnapshot> snapshots;
    std::deque<network::EntityReplicationPolicy> replicationPolicies;
    {
        std::lock_guard<std::mutex> lock(reliableMutex);
        snapshots.swap(receivedSnapshots);
        replicationPolicies.swap(receivedReplicationPolicies);
    }
    std::vector<Entity> entities;
    {
        std::lock_guard<std::mutex> lock(entityMutex);
        entities = replicatedEntities;
    }
    for (const network::TransformSnapshot& snapshot : snapshots) {
        bool knownEntity = false;
        for (Entity entity : entities) {
            NetworkComponent* networkComponent = scene->findComponent<NetworkComponent>(entity);
            if (networkComponent && networkComponent->netId == snapshot.netId) {
                knownEntity = true;
                break;
            }
        }
        if (!knownEntity && mode == Mode::Client && snapshot.netId != 0) {
            const Entity entity = scene->createEntity();
            scene->addComponent<Transform>(entity);
            scene->addComponent<NetworkComponent>(entity);
            NetworkComponent& networkComponent = scene->getComponent<NetworkComponent>(entity);
            networkComponent.netId = snapshot.netId;
            networkComponent.replicateTransform = true;
            {
                std::lock_guard<std::mutex> lock(entityMutex);
                replicatedEntities.push_back(entity);
                nextNetworkEntityId = std::max(nextNetworkEntityId, snapshot.netId + 1);
            }
            entities.push_back(entity);
        }
        SnapshotBlend& blend = snapshotBlends[snapshot.netId];
        if (blend.initialized && snapshot.sequence <= blend.lastSnapshotSequence) continue;
        if (!blend.initialized) {
            blend.current = snapshot;
            blend.target = snapshot;
            blend.initialized = true;
        } else if (!sameSnapshot(blend.target, snapshot)) {
            for (Entity entity : entities) {
                NetworkComponent* networkComponent = scene->findComponent<NetworkComponent>(entity);
                Transform* transform = scene->findComponent<Transform>(entity);
                if (networkComponent && transform && networkComponent->netId == snapshot.netId) {
                    blend.current.position[0] = transform->position.x;
                    blend.current.position[1] = transform->position.y;
                    blend.current.position[2] = transform->position.z;
                    blend.current.rotation[0] = transform->rotation.w;
                    blend.current.rotation[1] = transform->rotation.x;
                    blend.current.rotation[2] = transform->rotation.y;
                    blend.current.rotation[3] = transform->rotation.z;
                    blend.current.scale[0] = transform->scale.x;
                    blend.current.scale[1] = transform->scale.y;
                    blend.current.scale[2] = transform->scale.z;
                    break;
                }
            }
            blend.target = snapshot;
            blend.elapsed = 0.0f;
        }
        blend.lastSnapshotSequence = snapshot.sequence;
    }

    for (const network::EntityReplicationPolicy& policy : replicationPolicies) {
        pendingReplicationPolicies[policy.netId] = policy;
    }
    for (auto policy = pendingReplicationPolicies.begin(); policy != pendingReplicationPolicies.end();) {
        bool applied = false;
        for (Entity entity : entities) {
            NetworkComponent* networkComponent = scene->findComponent<NetworkComponent>(entity);
            if (!networkComponent || networkComponent->netId != policy->first) continue;
            networkComponent->ownerId = policy->second.ownerId;
            networkComponent->interestGroup = policy->second.interestGroup;
            networkComponent->interestRadius = policy->second.interestRadius;
            networkComponent->serverAuthority = policy->second.serverAuthority;
            applied = true;
            break;
        }
        if (applied) policy = pendingReplicationPolicies.erase(policy);
        else ++policy;
    }

    const float blendDuration = 0.05f;
    for (auto it = snapshotBlends.begin(); it != snapshotBlends.end();) {
        SnapshotBlend& blend = it->second;
        blend.elapsed = std::min(blend.elapsed + step, blendDuration);
        const float amount = blendDuration > 0.0f ? blend.elapsed / blendDuration : 1.0f;
        const Vector3 position(blend.current.position[0], blend.current.position[1], blend.current.position[2]);
        const Vector3 targetPosition(blend.target.position[0], blend.target.position[1], blend.target.position[2]);
        const Quaternion rotation(blend.current.rotation[0], blend.current.rotation[1],
            blend.current.rotation[2], blend.current.rotation[3]);
        const Quaternion targetRotation(blend.target.rotation[0], blend.target.rotation[1],
            blend.target.rotation[2], blend.target.rotation[3]);
        const Vector3 scale(blend.current.scale[0], blend.current.scale[1], blend.current.scale[2]);
        const Vector3 targetScale(blend.target.scale[0], blend.target.scale[1], blend.target.scale[2]);
        for (Entity entity : entities) {
            NetworkComponent* networkComponent = scene->findComponent<NetworkComponent>(entity);
            if (!networkComponent || networkComponent->netId != it->first) continue;
            Transform* transform = scene->findComponent<Transform>(entity);
            if (!transform) break;
            transform->position = position.lerp(targetPosition, amount);
            transform->rotation = Quaternion::slerp(amount, rotation, targetRotation);
            transform->scale = scale.lerp(targetScale, amount);
            transform->needUpdate = true;
            break;
        }
        if (amount >= 1.0f) blend.current = blend.target;
        ++it;
    }
}

void NetworkSystem::destroy() {
    running = false;
    connectedState = false;
    connectedPeerCount = 0;
    if (networkThread.joinable()) networkThread.join();
    mode = Mode::None;
}

void NetworkSystem::networkLoop() {
#if defined(_WIN32)
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::cerr << "[Network] WSAStartup failed\n";
        return;
    }
#endif

    NetworkSocket socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket == INVALID_NETWORK_SOCKET || !makeNonBlocking(socket)) {
        std::cerr << "[Network] failed to create non-blocking UDP socket\n";
        if (socket != INVALID_NETWORK_SOCKET) closeSocket(socket);
#if defined(_WIN32)
        WSACleanup();
#endif
        return;
    }

    std::mt19937 simulationRandom(clientNonce ^ static_cast<uint32_t>(listenPort)
        ^ static_cast<uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::uniform_int_distribution<uint32_t> simulationRoll(0, 99);

    struct SimulatedDatagram {
        std::vector<uint8_t> packet;
        sockaddr_in destination{};
        std::chrono::steady_clock::time_point sendAt{};
    };
    std::deque<SimulatedDatagram> delayedPackets;
    auto rawSend = [&](const std::vector<uint8_t>& packet, const sockaddr_in& destination) {
        if (packet.empty()) return;
        const int sent = ::sendto(socket, reinterpret_cast<const char*>(packet.data()),
            static_cast<int>(packet.size()), 0,
            reinterpret_cast<const sockaddr*>(&destination), sizeof(destination));
        if (sent >= 0) {
            std::lock_guard<std::mutex> metricsLock(metricsMutex);
            ++metrics.packetsSent;
            metrics.bytesSent += static_cast<uint64_t>(sent);
        }
    };
    auto flushDelayedPackets = [&]() {
        const auto now = std::chrono::steady_clock::now();
        while (!delayedPackets.empty() && delayedPackets.front().sendAt <= now) {
            SimulatedDatagram datagram = std::move(delayedPackets.front());
            delayedPackets.pop_front();
            rawSend(datagram.packet, datagram.destination);
        }
    };
    auto sendPacket = [&](const std::vector<uint8_t>& packet, const sockaddr_in& destination,
        bool simulate) {
        const std::vector<uint8_t> wirePacket = network::authenticatePacket(packet, getAuthToken());
        if (wirePacket.size() > MAX_PACKET_BYTES) {
            std::lock_guard<std::mutex> metricsLock(metricsMutex);
            ++metrics.reliableDropped;
            return;
        }
        if (!simulate) {
            rawSend(wirePacket, destination);
            return;
        }
        const uint32_t lossPercent = packetLossSimulationPercent.load();
        if (lossPercent > 0 && simulationRoll(simulationRandom) < lossPercent) {
            std::lock_guard<std::mutex> metricsLock(metricsMutex);
            ++metrics.simulatedPacketDrops;
            return;
        }
        const uint32_t reorderPercent = packetReorderSimulationPercent.load();
        if (reorderPercent > 0 && simulationRoll(simulationRandom) < reorderPercent) {
            if (delayedPackets.size() >= 256) {
                std::lock_guard<std::mutex> metricsLock(metricsMutex);
                ++metrics.simulatedPacketDrops;
                return;
            }
            const uint32_t delayMs = 1 + (simulationRoll(simulationRandom) % 50);
            delayedPackets.push_back(SimulatedDatagram{wirePacket, destination,
                std::chrono::steady_clock::now() + std::chrono::milliseconds(delayMs)});
            std::lock_guard<std::mutex> metricsLock(metricsMutex);
            ++metrics.simulatedPacketReorders;
            return;
        }
        rawSend(wirePacket, destination);
    };

    struct ServerPeerState {
        sockaddr_in address{};
        uint32_t peerId = 0;
        bool authenticated = false;
        std::chrono::steady_clock::time_point lastReceive{};
        uint32_t nextReliableSequence = 1;
        uint32_t nextExpectedReliableSequence = 1;
        uint32_t reliableWindow = 8;
        double reliableRttVarianceMs = 50.0;
        std::unordered_map<uint32_t, PendingReliable> pendingReliable;
        std::map<uint32_t, std::vector<uint8_t>> orderedReliable;
        PeerInterest interest;
    };
    std::vector<ServerPeerState> serverPeers;
    struct ServerReservation {
        sockaddr_in address{};
        uint32_t requestNonce = 0;
        uint32_t reservationId = 0;
        std::chrono::steady_clock::time_point expiresAt{};
    };
    std::vector<ServerReservation> reservations;
    uint32_t nextReservationId = 1;
    const auto pruneReservations = [&]() {
        const auto now = std::chrono::steady_clock::now();
        reservations.erase(std::remove_if(reservations.begin(), reservations.end(),
            [&now](const ServerReservation& reservation) {
                return reservation.expiresAt <= now;
            }), reservations.end());
    };
    auto findServerPeer = [&](const sockaddr_in& address) -> ServerPeerState* {
        for (ServerPeerState& candidate : serverPeers) {
            if (samePeer(candidate.address, address)) return &candidate;
        }
        return nullptr;
    };
    auto findReservation = [&](const sockaddr_in& address, uint32_t reservationId) {
        return std::find_if(reservations.begin(), reservations.end(),
            [&address, reservationId](const ServerReservation& reservation) {
                return reservation.reservationId == reservationId
                    && reservation.address.sin_addr.s_addr == address.sin_addr.s_addr;
            });
    };

    sockaddr_in peer{};
    peer.sin_family = AF_INET;
    peer.sin_port = htons(listenPort);
    sockaddr_in relayControlPeer{};
    relayControlPeer.sin_family = AF_INET;
    relayControlPeer.sin_port = htons(relayPort);

    if (relayEnabled) {
        if (inet_pton(AF_INET, relayAddress.c_str(), &relayControlPeer.sin_addr) != 1) {
            std::cerr << "[Network] invalid relay address: " << relayAddress << "\n";
            closeSocket(socket);
#if defined(_WIN32)
            WSACleanup();
#endif
            return;
        }
    }

    if (mode == Mode::Server) {
        peer.sin_addr.s_addr = htonl(INADDR_ANY);
        if (::bind(socket, reinterpret_cast<const sockaddr*>(&peer), sizeof(peer)) != 0) {
            std::cerr << "[Network] bind failed\n";
            closeSocket(socket);
#if defined(_WIN32)
            WSACleanup();
#endif
            return;
        }
    } else if (inet_pton(AF_INET, serverAddress.c_str(), &peer.sin_addr) != 1) {
        std::cerr << "[Network] invalid server address: " << serverAddress << "\n";
        closeSocket(socket);
#if defined(_WIN32)
        WSACleanup();
#endif
        return;
    }

    if (relayEnabled) {
        peer = relayControlPeer;
    }

    bool hasPeer = mode == Mode::Client;
    bool peerConnected = false;
    bool peerAuthenticated = false;
    uint32_t assignedPeerId = 0;
    auto lastHelloSent = std::chrono::steady_clock::now();
    auto lastRelayControlSent = std::chrono::steady_clock::now();
    auto lastRelayHeartbeatSent = std::chrono::steady_clock::now();
    auto lastSnapshotSent = std::chrono::steady_clock::now();
    auto lastReceive = std::chrono::steady_clock::now();
    auto receiveRateWindow = std::chrono::steady_clock::now();
    uint32_t receivePacketCount = 0;
    uint32_t receiveByteCount = 0;
    auto sendRelayControl = [&](network::PacketType type) {
        const std::vector<uint8_t> request = network::authenticatePacket(
            network::encodeRelaySessionRequest(type, clientNonce, nextSequence++, relayRoom),
            relayToken);
        if (request.size() <= MAX_PACKET_BYTES) rawSend(request, relayControlPeer);
    };

    if (relayEnabled) {
        sendRelayControl(mode == Mode::Server ? network::PacketType::RelayRegister
            : network::PacketType::RelayJoin);
    } else if (mode == Mode::Client) {
        const auto hello = clientReservationId != 0
            ? network::encodeHelloWithReservation(clientNonce, nextSequence++, clientReservationId)
            : network::encodeHello(network::PacketType::Hello, clientNonce, nextSequence++);
        sendPacket(hello, peer, false);
    }

    while (running) {
        flushDelayedPackets();
        if (relayEnabled && !relayReady.load()
                && std::chrono::steady_clock::now() - lastRelayControlSent
                    >= std::chrono::milliseconds(250)) {
            sendRelayControl(mode == Mode::Server ? network::PacketType::RelayRegister
                : network::PacketType::RelayJoin);
            lastRelayControlSent = std::chrono::steady_clock::now();
        }
        if (relayEnabled && relayReady.load()
                && std::chrono::steady_clock::now() - lastRelayHeartbeatSent
                    >= std::chrono::seconds(10)) {
            sendRelayControl(network::PacketType::RelayHeartbeat);
            lastRelayHeartbeatSent = std::chrono::steady_clock::now();
        }
        uint8_t buffer[1500];
        sockaddr_in source{};
#if defined(_WIN32)
        int sourceLength = sizeof(source);
        const int received = ::recvfrom(socket, reinterpret_cast<char*>(buffer), sizeof(buffer), 0,
            reinterpret_cast<sockaddr*>(&source), &sourceLength);
#else
        socklen_t sourceLength = sizeof(source);
        const int received = static_cast<int>(::recvfrom(socket, buffer, sizeof(buffer), 0,
            reinterpret_cast<sockaddr*>(&source), &sourceLength));
#endif
        bool receiveAllowed = received > 0;
        bool packetAuthenticated = true;
        const uint8_t* packetData = buffer;
        size_t packetSize = received > 0 ? static_cast<size_t>(received) : 0;
        if (received > 0) {
            const auto now = std::chrono::steady_clock::now();
            if (now - receiveRateWindow >= std::chrono::seconds(1)) {
                receiveRateWindow = now;
                receivePacketCount = 0;
                receiveByteCount = 0;
            }
            {
                std::lock_guard<std::mutex> metricsLock(metricsMutex);
                ++metrics.packetsReceived;
                metrics.bytesReceived += static_cast<uint64_t>(received);
            }
            if (static_cast<size_t>(received) > MAX_PACKET_BYTES
                    || receivePacketCount >= MAX_PACKETS_PER_SECOND
                    || receiveByteCount + static_cast<uint32_t>(received) > MAX_BYTES_PER_SECOND) {
                receiveAllowed = false;
                std::lock_guard<std::mutex> metricsLock(metricsMutex);
                ++metrics.rateLimitedPackets;
            } else {
                ++receivePacketCount;
                receiveByteCount += static_cast<uint32_t>(received);
            }
        }
        if (received > 0 && receiveAllowed) {
            size_t authenticatedSize = packetSize;
            network::PacketHeader wireHeader;
            const bool relayControl = relayEnabled
                && network::decodeHeader(buffer, packetSize, wireHeader)
                && (wireHeader.type == network::PacketType::RelayReady
                    || wireHeader.type == network::PacketType::RelayRegister
                    || wireHeader.type == network::PacketType::RelayJoin
                    || wireHeader.type == network::PacketType::RelayHeartbeat);
            const std::string token = relayControl ? relayToken : getAuthToken();
            packetAuthenticated = network::verifyPacketAuthentication(buffer, packetSize, token,
                authenticatedSize);
            if (!packetAuthenticated) {
                std::lock_guard<std::mutex> metricsLock(metricsMutex);
                ++metrics.unauthorizedPackets;
            } else {
                packetSize = authenticatedSize;
            }
        }
        if (received > 0 && receiveAllowed && packetAuthenticated) {
            network::RelaySessionResponse relayResponse;
            network::PacketHeader relayHeader;
            if (relayEnabled && network::decodeRelaySessionResponse(packetData, packetSize,
                    relayHeader, relayResponse) && samePeer(source, relayControlPeer)
                    && relayResponse.nonce == clientNonce) {
                if (relayResponse.accepted && relayResponse.peerPort != 0
                        && mode == Mode::Client) {
                    const bool wasReady = relayReady.load();
                    peer = relayControlPeer;
                    peer.sin_port = htons(relayResponse.peerPort);
                    relayPeerPort = relayResponse.peerPort;
                    relayReady = true;
                    if (!wasReady) {
                        const auto hello = network::encodeHello(network::PacketType::Hello,
                            clientNonce, nextSequence++);
                        sendPacket(hello, peer, false);
                        lastHelloSent = std::chrono::steady_clock::now();
                        std::cout << "[Network] Relay room joined; virtual peer port "
                            << relayResponse.peerPort << "\n";
                    }
                } else if (relayResponse.accepted && mode == Mode::Server) {
                    const bool wasReady = relayReady.load();
                    relayPeerPort = relayResponse.virtualPort;
                    relayReady = true;
                    if (!wasReady) std::cout << "[Network] Relay room registered\n";
                } else if (!relayResponse.accepted) {
                    relayReady = false;
                }
                continue;
            }
            network::LobbyQuery lobbyQuery;
            network::PacketHeader lobbyHeader;
            if (network::decodeLobbyQuery(packetData, packetSize, lobbyHeader, lobbyQuery)) {
                if (mode == Mode::Server) {
                    const LobbyInfo info = getLobbyInfo();
                    network::LobbyAdvertisement advertisement;
                    advertisement.nonce = lobbyQuery.nonce;
                    advertisement.gamePort = listenPort;
                    advertisement.currentPeers = static_cast<uint16_t>(
                        std::min<uint32_t>(connectedPeerCount.load(), 65535));
                    advertisement.maxPeers = static_cast<uint16_t>(
                        std::min<uint32_t>(maxPeers.load(), 65535));
                    advertisement.serverName = info.serverName;
                    advertisement.mapName = info.mapName;
                    advertisement.buildId = info.buildId;
                    sendPacket(network::encodeLobbyAdvertisement(advertisement, nextSequence++),
                        source, false);
                }
                continue;
            }
            network::LobbyReservationRequest reservationRequest;
            network::PacketHeader reservationRequestHeader;
            if (network::decodeLobbyReservationRequest(packetData, packetSize,
                    reservationRequestHeader, reservationRequest)) {
                if (mode == Mode::Server) {
                    pruneReservations();
                    network::LobbyReservationResponse response;
                    response.nonce = reservationRequest.nonce;
                    const uint16_t seconds = std::clamp<uint16_t>(
                        reservationRequest.requestedSeconds, 1, 60);
                    const bool authenticatedReservation = !getAuthToken().empty();
                    auto existing = std::find_if(reservations.begin(), reservations.end(),
                        [&source, &reservationRequest](const ServerReservation& reservation) {
                            return reservation.address.sin_addr.s_addr == source.sin_addr.s_addr
                                && reservation.requestNonce == reservationRequest.nonce;
                        });
                    const bool alreadyConnected = findServerPeer(source) != nullptr;
                    if (authenticatedReservation && !alreadyConnected
                            && (existing != reservations.end()
                                || serverPeers.size() + reservations.size() < maxPeers.load())) {
                        if (existing == reservations.end()) {
                            ServerReservation created;
                            created.address = source;
                            created.requestNonce = reservationRequest.nonce;
                            created.reservationId = nextReservationId++;
                            if (created.reservationId == 0) created.reservationId = nextReservationId++;
                            reservations.push_back(created);
                            existing = std::prev(reservations.end());
                        }
                        existing->expiresAt = std::chrono::steady_clock::now()
                            + std::chrono::seconds(seconds);
                        response.reservationId = existing->reservationId;
                        response.expiresSeconds = seconds;
                        response.accepted = true;
                    }
                    sendPacket(network::encodeLobbyReservationResponse(response, nextSequence++),
                        source, false);
                }
                continue;
            }
            network::PacketHeader header;
            network::HelloPayload helloPayload;
            if (network::decodeHello(packetData, packetSize, header, helloPayload)) {
                if (mode == Mode::Server && header.type == network::PacketType::Hello) {
                    ServerPeerState* serverPeer = findServerPeer(source);
                    const bool firstHandshake = serverPeer == nullptr;
                    if (firstHandshake) {
                        pruneReservations();
                        auto reservation = helloPayload.reservationId == 0
                            ? reservations.end()
                            : findReservation(source, helloPayload.reservationId);
                        const bool hasValidReservation = reservation != reservations.end();
                        if ((helloPayload.reservationId != 0 && !hasValidReservation)
                                || serverPeers.size() >= maxPeers.load()
                                || (!hasValidReservation
                                    && serverPeers.size() + reservations.size() >= maxPeers.load())) {
                            std::lock_guard<std::mutex> metricsLock(metricsMutex);
                            ++metrics.unauthorizedPackets;
                            continue;
                        }
                        if (hasValidReservation) reservations.erase(reservation);
                        ServerPeerState created;
                        created.address = source;
                        created.peerId = nextPeerId++;
                        if (created.peerId == 0) created.peerId = nextPeerId++;
                        serverPeers.push_back(std::move(created));
                        serverPeer = &serverPeers.back();
                    }
                    serverPeer->authenticated = true;
                    serverPeer->lastReceive = std::chrono::steady_clock::now();
                    connectedState = true;
                    connectedPeerCount = static_cast<uint32_t>(serverPeers.size());
                    const auto ack = network::encodeHello(network::PacketType::HelloAck,
                        helloPayload.nonce, nextSequence++, serverPeer->peerId);
                    sendPacket(ack, serverPeer->address, false);
                    if (firstHandshake) {
                        std::cout << "[Network] Handshake accepted for peer " << serverPeer->peerId << "\n";
                    }
                } else if (mode == Mode::Client && header.type == network::PacketType::HelloAck
                        && helloPayload.nonce == clientNonce && samePeer(source, peer)) {
                    const bool firstHandshake = !peerConnected;
                    peerAuthenticated = true;
                    peerConnected = true;
                    connectedState = true;
                    connectedPeerCount = 1;
                    if (helloPayload.peerId != 0) localPeerId = helloPayload.peerId;
                    lastReceive = std::chrono::steady_clock::now();
                    if (firstHandshake) std::cout << "[Network] Handshake completed\n";
                } else {
                    std::lock_guard<std::mutex> metricsLock(metricsMutex);
                    ++metrics.unauthorizedPackets;
                }
            } else {
                uint32_t acknowledgedSequence = 0;
                std::vector<uint8_t> reliablePayload;
                const bool decodedReliable = network::decodeReliable(packetData, packetSize, header,
                    acknowledgedSequence, reliablePayload);
                ServerPeerState* incomingServerPeer = mode == Mode::Server
                    ? findServerPeer(source) : nullptr;
                const bool peerAuthorized = mode == Mode::Server
                    ? incomingServerPeer != nullptr && incomingServerPeer->authenticated
                    : peerAuthenticated && samePeer(source, peer);
                if (decodedReliable && !peerAuthorized) {
                    std::lock_guard<std::mutex> metricsLock(metricsMutex);
                    ++metrics.unauthorizedPackets;
                } else if (decodedReliable) {
                    if (incomingServerPeer) incomingServerPeer->lastReceive = std::chrono::steady_clock::now();
                    if ((header.type == network::PacketType::Snapshot
                            || header.type == network::PacketType::SnapshotCompressed)
                            && mode == Mode::Client && scene) {
                        network::TransformSnapshot snapshot;
                        if (network::decodeTransformSnapshot(packetData, packetSize, header, snapshot)) {
                            std::lock_guard<std::mutex> lock(reliableMutex);
                            if (receivedSnapshots.size() < MAX_RECEIVED_SNAPSHOTS) {
                                receivedSnapshots.push_back(snapshot);
                            } else {
                                std::lock_guard<std::mutex> metricsLock(metricsMutex);
                                ++metrics.reliableDropped;
                            }
                            std::lock_guard<std::mutex> metricsLock(metricsMutex);
                            ++metrics.snapshotsReceived;
                        }
                    } else if ((header.type == network::PacketType::Snapshot
                            || header.type == network::PacketType::SnapshotCompressed)
                            && mode != Mode::Client) {
                        std::lock_guard<std::mutex> metricsLock(metricsMutex);
                        ++metrics.unauthorizedPackets;
                    } else if (header.type == network::PacketType::ReplicationPolicy
                            && mode == Mode::Client) {
                        network::EntityReplicationPolicy policy;
                        if (network::decodeReplicationPolicy(packetData, packetSize, header, policy)) {
                            std::lock_guard<std::mutex> lock(reliableMutex);
                            if (receivedReplicationPolicies.size() < MAX_RECEIVED_SNAPSHOTS) {
                                receivedReplicationPolicies.push_back(policy);
                            } else {
                                std::lock_guard<std::mutex> metricsLock(metricsMutex);
                                ++metrics.reliableDropped;
                            }
                        } else {
                            std::lock_guard<std::mutex> metricsLock(metricsMutex);
                            ++metrics.malformedPackets;
                        }
                        const auto ack = network::encodeReliable(network::PacketType::Ack, 0,
                            header.sequence, nullptr, 0);
                        sendPacket(ack, mode == Mode::Server && incomingServerPeer
                            ? incomingServerPeer->address : peer, true);
                    } else if (header.type == network::PacketType::ReplicationPolicy
                            && mode != Mode::Client) {
                        std::lock_guard<std::mutex> metricsLock(metricsMutex);
                        ++metrics.unauthorizedPackets;
                    } else if (header.type == network::PacketType::InterestUpdate
                            && mode == Mode::Server) {
                        network::InterestUpdate interest;
                        if (network::decodeInterestUpdate(packetData, packetSize, header, interest)
                                && interest.radius <= static_cast<float>(MAX_INTEREST_RADIUS)
                                && incomingServerPeer != nullptr) {
                            incomingServerPeer->interest.center = Vector3(interest.center[0], interest.center[1], interest.center[2]);
                            incomingServerPeer->interest.radius = interest.radius;
                            incomingServerPeer->interest.group = interest.group;
                            incomingServerPeer->interest.valid = finiteVector(incomingServerPeer->interest.center);
                        } else {
                            std::lock_guard<std::mutex> metricsLock(metricsMutex);
                            ++metrics.malformedPackets;
                        }
                        const auto ack = network::encodeReliable(network::PacketType::Ack, 0,
                            header.sequence, nullptr, 0);
                        sendPacket(ack, mode == Mode::Server && incomingServerPeer
                            ? incomingServerPeer->address : peer, true);
                    } else if (header.type == network::PacketType::Data) {
                        const auto ack = network::encodeReliable(network::PacketType::Ack, 0,
                            header.sequence, nullptr, 0);
                        sendPacket(ack, incomingServerPeer ? incomingServerPeer->address : peer, true);
                        if (mode == Mode::Server && incomingServerPeer != nullptr) {
                            network::MessageEnvelope command;
                            const bool decodedCommand = network::decodeMessage(
                                reliablePayload.data(), reliablePayload.size(), command);
                            const bool migratedCommand = decodedCommand && network::migrateMessage(command);
                            const bool validCommand = migratedCommand
                                && network::validateMessage(command, incomingServerPeer->peerId);
                            if (decodedCommand && (!migratedCommand || !validCommand)) {
                                std::lock_guard<std::mutex> metricsLock(metricsMutex);
                                ++metrics.unauthorizedPackets;
                                continue;
                            }
                        }
                        std::lock_guard<std::mutex> lock(reliableMutex);
                        uint32_t& expectedSequence = incomingServerPeer
                            ? incomingServerPeer->nextExpectedReliableSequence
                            : nextExpectedReliableSequence;
                        auto& orderedPackets = incomingServerPeer
                            ? incomingServerPeer->orderedReliable : orderedReliable;
                        if (header.sequence < expectedSequence) {
                            // A retransmission was already delivered; the ACK above is
                            // still sent so the sender can stop retrying it.
                        } else if (header.sequence == expectedSequence) {
                            if (receivedReliable.size() < MAX_RECEIVED_RELIABLE) {
                                receivedReliable.push_back(std::move(reliablePayload));
                                ++expectedSequence;
                                while (receivedReliable.size() < MAX_RECEIVED_RELIABLE) {
                                    const auto ordered = orderedPackets.find(expectedSequence);
                                    if (ordered == orderedPackets.end()) break;
                                    receivedReliable.push_back(std::move(ordered->second));
                                    orderedPackets.erase(ordered);
                                    ++expectedSequence;
                                }
                            } else {
                                std::lock_guard<std::mutex> metricsLock(metricsMutex);
                                ++metrics.reliableDropped;
                            }
                            std::lock_guard<std::mutex> metricsLock(metricsMutex);
                            ++metrics.reliableReceived;
                        } else if (orderedPackets.size() < MAX_ORDERED_RELIABLE) {
                            orderedPackets.emplace(header.sequence, std::move(reliablePayload));
                            std::lock_guard<std::mutex> metricsLock(metricsMutex);
                            ++metrics.reliableOutOfOrder;
                        } else {
                            std::lock_guard<std::mutex> metricsLock(metricsMutex);
                            ++metrics.reliableDropped;
                        }
                    } else if (header.type == network::PacketType::Unreliable) {
                        std::lock_guard<std::mutex> lock(reliableMutex);
                        if (receivedUnreliable.size() < MAX_RECEIVED_UNRELIABLE) {
                            receivedUnreliable.push_back(std::move(reliablePayload));
                        } else {
                            std::lock_guard<std::mutex> metricsLock(metricsMutex);
                            ++metrics.unreliableDropped;
                        }
                        std::lock_guard<std::mutex> metricsLock(metricsMutex);
                        ++metrics.unreliableReceived;
                    } else if (header.type == network::PacketType::Ack) {
                        std::lock_guard<std::mutex> lock(reliableMutex);
                        auto& peerPending = incomingServerPeer
                            ? incomingServerPeer->pendingReliable : pendingReliable;
                        const auto pending = peerPending.find(acknowledgedSequence);
                        if (pending != peerPending.end()) {
                            if (pending->second.lastSent != std::chrono::steady_clock::time_point{}) {
                                const double sampleMs = std::chrono::duration<double, std::milli>(
                                    std::chrono::steady_clock::now() - pending->second.lastSent).count();
                                std::lock_guard<std::mutex> metricsLock(metricsMutex);
                                if (metrics.reliableRttMs <= 0.0) {
                                    metrics.reliableRttMs = sampleMs;
                                    reliableRttVarianceMs = sampleMs * 0.5;
                                } else {
                                    const double error = std::abs(metrics.reliableRttMs - sampleMs);
                                    reliableRttVarianceMs = reliableRttVarianceMs * 0.75 + error * 0.25;
                                    metrics.reliableRttMs = metrics.reliableRttMs * 0.875 + sampleMs * 0.125;
                                }
                                metrics.reliableRetransmitTimeoutMs = std::clamp(
                                    metrics.reliableRttMs + std::max(20.0, reliableRttVarianceMs * 4.0),
                                    50.0, 500.0);
                                if (incomingServerPeer) {
                                    incomingServerPeer->reliableWindow = std::min<uint32_t>(64,
                                        incomingServerPeer->reliableWindow + 1);
                                    metrics.reliableWindow = incomingServerPeer->reliableWindow;
                                } else {
                                    reliableWindow = std::min<uint32_t>(64, reliableWindow + 1);
                                    metrics.reliableWindow = reliableWindow;
                                }
                            }
                            peerPending.erase(pending);
                        }
                    } else {
                        std::lock_guard<std::mutex> metricsLock(metricsMutex);
                        ++metrics.malformedPackets;
                    }
                } else {
                    std::lock_guard<std::mutex> metricsLock(metricsMutex);
                    ++metrics.malformedPackets;
                }
            }
        } else if (received < 0 && !wouldBlock() && !transientReceiveError() && running) {
            std::cerr << "[Network] UDP receive failed\n";
            break;
        }

        const auto now = std::chrono::steady_clock::now();
        if (received > 0 && receiveAllowed && packetAuthenticated) lastReceive = now;
        if (mode == Mode::Server) {
            const auto timeout = std::chrono::milliseconds(1500);
            for (auto it = serverPeers.begin(); it != serverPeers.end();) {
                if (now - it->lastReceive < timeout) {
                    ++it;
                    continue;
                }
                std::cout << "[Network] Peer timed out: " << it->peerId << "\n";
                it = serverPeers.erase(it);
            }
            connectedPeerCount = static_cast<uint32_t>(serverPeers.size());
            connectedState = !serverPeers.empty();
        }
        const auto timeout = std::chrono::milliseconds(750);
        if (mode == Mode::Client && peerConnected && now - lastReceive >= timeout) {
            peerConnected = false;
            connectedState = false;
            peerAuthenticated = false;
            connectedPeerCount = 0;
            localPeerId = 0;
            std::lock_guard<std::mutex> lock(reliableMutex);
            for (auto& pending : pendingReliable) {
                pending.second.attempts = 0;
                pending.second.lastSent = {};
            }
            std::cout << "[Network] Connection timed out; reconnecting\n";
        }

        if (mode == Mode::Client && hasPeer
                && (!peerConnected || now - lastHelloSent >= std::chrono::milliseconds(250))) {
            const auto hello = clientReservationId != 0
                ? network::encodeHelloWithReservation(clientNonce, nextSequence++, clientReservationId)
                : network::encodeHello(network::PacketType::Hello, clientNonce, nextSequence++);
            sendPacket(hello, peer, false);
            lastHelloSent = now;
        }

        const bool canSend = mode == Mode::Client ? hasPeer && peerConnected : !serverPeers.empty();
        if (canSend) {
            std::lock_guard<std::mutex> lock(reliableMutex);
            double retransmitTimeoutMs = 100.0;
            {
                std::lock_guard<std::mutex> metricsLock(metricsMutex);
                retransmitTimeoutMs = metrics.reliableRetransmitTimeoutMs;
            }
            if (mode == Mode::Client) {
                uint32_t inFlight = 0;
                for (const auto& pending : pendingReliable) {
                    if (pending.second.attempts > 0) ++inFlight;
                }
                for (auto it = pendingReliable.begin(); it != pendingReliable.end();) {
                    PendingReliable& pending = it->second;
                    const bool retry = pending.attempts == 0
                        || now - pending.lastSent >= std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                            std::chrono::duration<double, std::milli>(retransmitTimeoutMs));
                    if (!retry) {
                        ++it;
                        continue;
                    }
                    if (pending.attempts == 0 && inFlight >= reliableWindow) {
                        ++it;
                        continue;
                    }
                    if (pending.attempts >= 10) {
                        std::cerr << "[Network] reliable packet dropped after retries: " << it->first << "\n";
                        {
                            std::lock_guard<std::mutex> metricsLock(metricsMutex);
                            ++metrics.reliableDropped;
                            reliableWindow = std::max<uint32_t>(1, reliableWindow / 2);
                            metrics.reliableWindow = reliableWindow;
                        }
                        it = pendingReliable.erase(it);
                        continue;
                    }
                    const auto packet = network::encodeReliable(network::PacketType::Data, it->first, 0,
                        pending.payload.data(), pending.payload.size());
                    sendPacket(packet, peer, true);
                    pending.lastSent = now;
                    if (pending.attempts == 0) ++inFlight;
                    else if (pending.attempts == 1) {
                        std::lock_guard<std::mutex> metricsLock(metricsMutex);
                        reliableWindow = std::max<uint32_t>(1, reliableWindow / 2);
                        metrics.reliableWindow = reliableWindow;
                    }
                    {
                        std::lock_guard<std::mutex> metricsLock(metricsMutex);
                        if (pending.attempts > 0) ++metrics.reliableRetransmitted;
                    }
                    ++pending.attempts;
                    ++it;
                }
            } else {
                std::vector<std::vector<uint8_t>> broadcast;
                while (!pendingReliableBroadcast.empty()) {
                    broadcast.push_back(std::move(pendingReliableBroadcast.front()));
                    pendingReliableBroadcast.pop_front();
                }
                for (ServerPeerState& serverPeer : serverPeers) {
                    for (const std::vector<uint8_t>& payload : broadcast) {
                        if (serverPeer.pendingReliable.size() >= MAX_PENDING_RELIABLE) {
                            std::lock_guard<std::mutex> metricsLock(metricsMutex);
                            ++metrics.reliableDropped;
                            break;
                        }
                        const uint32_t sequence = serverPeer.nextReliableSequence++;
                        serverPeer.pendingReliable.emplace(sequence, PendingReliable{payload});
                    }
                    uint32_t inFlight = 0;
                    for (const auto& pending : serverPeer.pendingReliable) {
                        if (pending.second.attempts > 0) ++inFlight;
                    }
                    for (auto it = serverPeer.pendingReliable.begin();
                            it != serverPeer.pendingReliable.end();) {
                        PendingReliable& pending = it->second;
                        const bool retry = pending.attempts == 0
                            || now - pending.lastSent >= std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                std::chrono::duration<double, std::milli>(retransmitTimeoutMs));
                        if (!retry || (pending.attempts == 0 && inFlight >= serverPeer.reliableWindow)) {
                            ++it;
                            continue;
                        }
                        if (pending.attempts >= 10) {
                            std::lock_guard<std::mutex> metricsLock(metricsMutex);
                            ++metrics.reliableDropped;
                            serverPeer.reliableWindow = std::max<uint32_t>(1, serverPeer.reliableWindow / 2);
                            metrics.reliableWindow = serverPeer.reliableWindow;
                            it = serverPeer.pendingReliable.erase(it);
                            continue;
                        }
                        const auto packet = network::encodeReliable(network::PacketType::Data, it->first, 0,
                            pending.payload.data(), pending.payload.size());
                        sendPacket(packet, serverPeer.address, true);
                        pending.lastSent = now;
                        if (pending.attempts == 0) ++inFlight;
                        else if (pending.attempts == 1) {
                            serverPeer.reliableWindow = std::max<uint32_t>(1, serverPeer.reliableWindow / 2);
                            std::lock_guard<std::mutex> metricsLock(metricsMutex);
                            metrics.reliableWindow = serverPeer.reliableWindow;
                        }
                        {
                            std::lock_guard<std::mutex> metricsLock(metricsMutex);
                            if (pending.attempts > 0) ++metrics.reliableRetransmitted;
                        }
                        ++pending.attempts;
                        ++it;
                    }
                }
            }

            while (!pendingUnreliable.empty()) {
                const std::vector<uint8_t> payload = std::move(pendingUnreliable.front());
                if (mode == Mode::Client) {
                    const auto packet = network::encodeUnreliable(nextSequence++, payload.data(), payload.size());
                    sendPacket(packet, peer, true);
                } else {
                    for (const ServerPeerState& serverPeer : serverPeers) {
                        const auto packet = network::encodeUnreliable(nextSequence++, payload.data(), payload.size());
                        sendPacket(packet, serverPeer.address, true);
                    }
                }
                pendingUnreliable.pop_front();
            }

            if (mode == Mode::Client) {
                PeerInterest interest;
                {
                    std::lock_guard<std::mutex> interestLock(interestMutex);
                    interest = localInterest;
                }
                if (interest.valid && (lastInterestSent == std::chrono::steady_clock::time_point{}
                        || now - lastInterestSent >= std::chrono::milliseconds(100))) {
                    network::InterestUpdate update;
                    update.center[0] = interest.center.x;
                    update.center[1] = interest.center.y;
                    update.center[2] = interest.center.z;
                    update.radius = interest.radius;
                    update.group = interest.group;
                    sendPacket(network::encodeInterestUpdate(update, nextSequence++), peer, true);
                    lastInterestSent = now;
                }
            }

            if (mode == Mode::Server && scene && now - lastSnapshotSent >= std::chrono::milliseconds(50)) {
                std::vector<Entity> entities;
                {
                    std::lock_guard<std::mutex> entityLock(entityMutex);
                    entities = replicatedEntities;
                }
                for (ServerPeerState& serverPeer : serverPeers) {
                    const PeerInterest& interest = serverPeer.interest;
                    for (Entity entity : entities) {
                        NetworkComponent* networkComponent = scene->findComponent<NetworkComponent>(entity);
                        Transform* transform = scene->findComponent<Transform>(entity);
                        if (!networkComponent || !networkComponent->replicateTransform || !transform
                                || networkComponent->netId == 0) continue;
                        if (networkComponent->interestGroup != 0 && interest.valid
                                && interest.group != 0 && networkComponent->interestGroup != interest.group) {
                            continue;
                        }
                        if (networkComponent->interestRadius > 0.0f && interest.valid && interest.radius > 0.0f) {
                            const float dx = transform->position.x - interest.center.x;
                            const float dy = transform->position.y - interest.center.y;
                            const float dz = transform->position.z - interest.center.z;
                            const float maxDistance = networkComponent->interestRadius + interest.radius;
                            if (dx * dx + dy * dy + dz * dz > maxDistance * maxDistance) continue;
                        }
                        network::TransformSnapshot snapshot;
                        snapshot.netId = networkComponent->netId;
                        snapshot.position[0] = transform->position.x;
                        snapshot.position[1] = transform->position.y;
                        snapshot.position[2] = transform->position.z;
                        snapshot.rotation[0] = transform->rotation.w;
                        snapshot.rotation[1] = transform->rotation.x;
                        snapshot.rotation[2] = transform->rotation.y;
                        snapshot.rotation[3] = transform->rotation.z;
                        snapshot.scale[0] = transform->scale.x;
                        snapshot.scale[1] = transform->scale.y;
                        snapshot.scale[2] = transform->scale.z;
                        {
                            std::lock_guard<std::mutex> historyLock(historyMutex);
                            std::deque<HistoricalSnapshot>& history = snapshotHistory[snapshot.netId];
                            history.push_back(HistoricalSnapshot{now, snapshot});
                            while (!history.empty()
                                    && now - history.front().time > std::chrono::seconds(1)) {
                                history.pop_front();
                            }
                        }
                        const auto packet = network::encodeCompressedTransformSnapshot(snapshot, nextSequence++);
                        sendPacket(packet, serverPeer.address, true);
                        network::EntityReplicationPolicy policy;
                        policy.netId = networkComponent->netId;
                        policy.ownerId = networkComponent->ownerId;
                        policy.interestGroup = networkComponent->interestGroup;
                        policy.interestRadius = networkComponent->interestRadius;
                        policy.serverAuthority = networkComponent->serverAuthority;
                        const auto policyPacket = network::encodeReplicationPolicy(policy, nextSequence++);
                        sendPacket(policyPacket, serverPeer.address, true);
                        std::lock_guard<std::mutex> metricsLock(metricsMutex);
                        ++metrics.snapshotsSent;
                    }
                }
                lastSnapshotSent = now;
            }
        }
        flushDelayedPackets();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    closeSocket(socket);
#if defined(_WIN32)
    WSACleanup();
#endif
}

} // namespace quantum
