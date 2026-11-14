// (c) SR VINIX
// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include <chrono>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include "engine/core/subsystem/NetworkProtocol.h"
#include "engine/core/subsystem/NetworkSystem.h"
#include "engine/core/script/LuaDebugger.h"
#include "lua.hpp"
#include "engine/core/subsystem/AudioSystem.h"
#include "engine/core/Scene.h"
#include "engine/core/manager/SceneManager.h"
#include "engine/core/io/SaveGame.h"
#include "engine/core/InputActionMap.h"
#include "engine/core/Input.h"
#include "engine/core/math/Ray.h"
#include "engine/core/event/EventBus.h"
#include "engine/core/component/Body2DComponent.h"
#include "engine/core/component/Body3DComponent.h"
#include "engine/core/component/ParticlesComponent.h"
#include "engine/core/component/PointsComponent.h"
#include "engine/core/component/LinesComponent.h"
#include "engine/core/component/Joint3DComponent.h"
#include "engine/core/component/AnimatorControllerComponent.h"
#include "engine/core/component/AnimationComponent.h"
#include "engine/core/component/ActionComponent.h"
#include "engine/core/component/CameraComponent.h"
#include "engine/core/component/Transform.h"
#include "engine/core/component/SoundComponent.h"
#include "engine/core/ecs/components/TagComponent.h"
#include "engine/core/subsystem/PhysicsSystem.h"
#include "engine/core/subsystem/ActionSystem.h"
#include "engine/core/subsystem/MeshSystem.h"
#include "engine/core/object/physics/Body3D.h"
#include "engine/voxel/Mesher.h"
#include "engine/voxel/World.h"
#include "Jolt/Physics/Collision/ObjectLayerPairFilterMask.h"

#if defined(_WIN32)
using Socket = SOCKET;
constexpr Socket INVALID_SOCKET_VALUE = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
using Socket = int;
constexpr Socket INVALID_SOCKET_VALUE = -1;
#endif

namespace {

void closeSocket(Socket socket);

bool migrateSmokeMessage(uint16_t fromVersion, std::vector<uint8_t>& payload) {
    if (fromVersion != 1) return false;
    payload.push_back(0xEE);
    return true;
}

bool validateGameplaySmokeCommand(uint32_t peerId, const std::vector<uint8_t>& payload) {
    return peerId != 0 && payload == std::vector<uint8_t>({0xCA, 0xFE});
}

bool runEventBusSmoke() {
    quantum::EventBus bus;
    std::atomic<uint32_t> received{0};
    const auto subscription = bus.subscribe("network.ready", [&received](const quantum::EventBus::Event& event) {
        if (event.type == "network.ready" && event.payload.size() == 2) ++received;
    });
    if (subscription == 0) return false;
    std::vector<std::thread> publishers;
    for (int i = 0; i < 4; ++i) {
        publishers.emplace_back([&bus]() {
            for (int j = 0; j < 250; ++j) bus.publish("network.ready", {1, 2});
        });
    }
    for (std::thread& publisher : publishers) publisher.join();
    const bool unsubscribed = bus.unsubscribe(subscription);
    const bool secondUnsubscribe = bus.unsubscribe(subscription);
    if (received != 1000 || !unsubscribed || secondUnsubscribe) return false;

    std::atomic<uint32_t> queuedReceived{0};
    const auto queuedSubscription = bus.subscribe("network.queued", [&queuedReceived](const quantum::EventBus::Event& event) {
        if (event.payload.size() == 1 && event.payload[0] == 9) ++queuedReceived;
    });
    if (queuedSubscription == 0) return false;
    bus.publishQueued("network.queued", {9});
    bus.publishQueued("network.queued", {9});
    const size_t firstDispatch = bus.dispatchQueued(1);
    if (firstDispatch != 1 || queuedReceived != 1) {
        std::cerr << "network-smoke: queued first dispatch count=" << firstDispatch
            << " received=" << queuedReceived.load() << "\n";
        return false;
    }
    const size_t secondDispatch = bus.dispatchQueued();
    if (secondDispatch != 1 || queuedReceived != 2) {
        std::cerr << "network-smoke: queued second dispatch count=" << secondDispatch
            << " received=" << queuedReceived.load() << "\n";
        return false;
    }
    return bus.unsubscribe(queuedSubscription);
}

bool runTextCompositionSmoke() {
    std::string observed;
    int observedCursor = -1;
    bool observedCommitted = true;
    const bool subscribed = quantum::Engine::onTextComposition.add(
        "text-composition-smoke",
        [&observed, &observedCursor, &observedCommitted](const std::string& text,
            int cursor, bool committed) {
            observed = text;
            observedCursor = cursor;
            observedCommitted = committed;
        });
    if (!subscribed) return false;
    quantum::Engine::systemTextComposition("kanji", 2, false);
    const bool preedit = observed == "kanji" && observedCursor == 2 && !observedCommitted;
    quantum::Engine::systemTextComposition("漢字", 0, true);
    const bool committed = observed == "漢字" && observedCursor == 0 && observedCommitted;
    quantum::Engine::onTextComposition.remove("text-composition-smoke");
    return preedit && committed;
}

bool runPredictionSmoke() {
    quantum::Scene scene;
    const quantum::Entity entity = scene.createEntity();
    scene.addComponent<quantum::Transform>(entity);
    scene.addComponent<quantum::NetworkComponent>(entity);
    scene.addComponent<quantum::TagComponent>(entity);
    scene.addComponent<quantum::SoundComponent>(entity);
    scene.addComponent<quantum::ParticlesComponent>(entity);
    scene.setEntityName(entity, "predicted-player");
    quantum::TagComponent& tag = scene.getComponent<quantum::TagComponent>(entity);
    tag.categories = quantum::TagComponent::PLAYER | quantum::TagComponent::DAMAGABLE;
    tag.tag = "hero";
    tag.layer = 3;
    quantum::SoundComponent& sound = scene.getComponent<quantum::SoundComponent>(entity);
    sound.filename = "audio/engine.wav";
    sound.bus = "music";
    sound.volume = 0.63;
    sound.speed = 1.15f;
    sound.pan = -0.2f;
    sound.looping = true;
    sound.occlusionEnabled = true;
    sound.occlusionVolume = 0.18f;
    quantum::ParticlesComponent& particles = scene.getComponent<quantum::ParticlesComponent>(entity);
    particles.maxParticles = 240;
    particles.emitter = true;
    particles.loop = false;
    particles.localSpace = true;
    particles.collisionEnabled = true;
    particles.collisionBounce = 0.72f;
    particles.collisionFriction = 0.18f;
    particles.trailsEnabled = true;
    particles.trailLength = 12;
    particles.rate = 37;
    particles.maxPerUpdate = 19;
    const quantum::Entity excludedEntity = scene.createEntity();
    scene.addComponent<quantum::Transform>(excludedEntity);
    scene.addComponent<quantum::NetworkComponent>(excludedEntity);
    scene.getComponent<quantum::Transform>(excludedEntity).position.x = 42.0f;
    scene.getComponent<quantum::NetworkComponent>(excludedEntity).replicateTransform = false;
    const auto network = scene.getSystem<quantum::NetworkSystem>();
    network->registerEntity(entity);
    network->registerEntity(entity);
    network->registerEntity(excludedEntity);
    network->setPredictionVelocity(entity, quantum::Vector3(2.0f, 0.0f, 0.0f));
    network->update(0.25);
    const bool moved = scene.getComponent<quantum::Transform>(entity).position.x > 0.49f;
    network->clearPrediction(entity);
    quantum::SaveGame writer("engine-completion-ecs-snapshot");
    writer.deleteSlot();
    const bool saved = network->saveReplicatedState(writer, "ecs") && writer.save();
    scene.getComponent<quantum::Transform>(entity).position = quantum::Vector3::ZERO;
    scene.getComponent<quantum::Transform>(excludedEntity).position = quantum::Vector3::ZERO;
    scene.getComponent<quantum::NetworkComponent>(entity).replicateTransform = false;
    scene.setEntityName(entity, "cleared-name");
    tag.categories = quantum::TagComponent::NONE;
    tag.tag.clear();
    tag.layer = 0;
    sound.filename.clear();
    sound.bus = "master";
    sound.volume = 1.0;
    sound.speed = 1.0f;
    sound.pan = 0.0f;
    sound.looping = false;
    sound.occlusionEnabled = false;
    particles.maxParticles = 100;
    particles.emitter = false;
    particles.loop = true;
    particles.localSpace = false;
    particles.collisionEnabled = false;
    particles.collisionBounce = 0.0f;
    particles.collisionFriction = 0.0f;
    particles.trailsEnabled = false;
    particles.trailLength = 8;
    particles.rate = 5;
    particles.maxPerUpdate = 100;
    quantum::SaveGame reader("engine-completion-ecs-snapshot");
    const bool restored = saved && reader.load() && network->loadReplicatedState(reader, "ecs")
        && scene.getComponent<quantum::Transform>(entity).position.x > 0.49f
        && scene.getEntityName(entity) == "predicted-player"
        && tag.categories == (quantum::TagComponent::PLAYER | quantum::TagComponent::DAMAGABLE)
        && tag.tag == "hero" && tag.layer == 3
        && sound.filename == "audio/engine.wav" && sound.bus == "music"
        && std::abs(sound.volume - 0.63) < 0.0001
        && std::abs(sound.speed - 1.15f) < 0.0001f
        && std::abs(sound.pan + 0.2f) < 0.0001f && sound.looping
        && sound.occlusionEnabled && std::abs(sound.occlusionVolume - 0.18f) < 0.0001f
        && particles.maxParticles == 240 && particles.emitter && !particles.loop
        && particles.localSpace && particles.collisionEnabled
        && std::abs(particles.collisionBounce - 0.72f) < 0.0001f
        && std::abs(particles.collisionFriction - 0.18f) < 0.0001f
        && particles.trailsEnabled && particles.trailLength == 12
        && particles.rate == 37 && particles.maxPerUpdate == 19
        && scene.getComponent<quantum::NetworkComponent>(entity).replicateTransform
        && scene.getComponent<quantum::Transform>(excludedEntity).position.x == 0.0f;
    if (!restored) {
        const std::string snapshot = reader.getString("ecs");
        std::cerr << "network-smoke: ECS restore details saved=" << saved
            << " loaded=" << reader.isValid() << " bytes=" << snapshot.size()
            << " magic=" << snapshot.substr(0, std::min<size_t>(8, snapshot.size()))
            << " position=" << scene.getComponent<quantum::Transform>(entity).position.x
            << " name=" << scene.getEntityName(entity)
            << " tag=" << tag.tag << " categories=" << tag.categories
            << " layer=" << static_cast<int>(tag.layer) << " excluded="
            << scene.getComponent<quantum::Transform>(excludedEntity).position.x << "\n";
    }
    auto appendLegacyU32 = [](std::string& output, uint32_t value) {
        output.push_back(static_cast<char>(value & 0xffu));
        output.push_back(static_cast<char>((value >> 8) & 0xffu));
        output.push_back(static_cast<char>((value >> 16) & 0xffu));
        output.push_back(static_cast<char>((value >> 24) & 0xffu));
    };
    auto appendLegacyFloat = [&appendLegacyU32](std::string& output, float value) {
        uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        appendLegacyU32(output, bits);
    };
    std::string legacy("QNETECS1", 8);
    appendLegacyU32(legacy, 1);
    appendLegacyU32(legacy, scene.getComponent<quantum::NetworkComponent>(entity).netId);
    for (float value : {9.0f, 8.0f, 7.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f}) {
        appendLegacyFloat(legacy, value);
    }
    writer.setString("legacy", legacy);
    const bool legacySaved = writer.save();
    scene.getComponent<quantum::Transform>(entity).position = quantum::Vector3::ZERO;
    quantum::SaveGame legacyReader("engine-completion-ecs-snapshot");
    const bool legacyRestored = legacySaved && legacyReader.load()
        && network->loadReplicatedState(legacyReader, "legacy")
        && scene.getComponent<quantum::Transform>(entity).position.x == 9.0f;
    std::string legacyV2("QNETECS2", 8);
    appendLegacyU32(legacyV2, 1);
    appendLegacyU32(legacyV2, scene.getComponent<quantum::NetworkComponent>(entity).netId);
    appendLegacyU32(legacyV2, 9);
    legacyV2.append("legacy-v2");
    legacyV2.push_back('\0');
    for (float value : {11.0f, 12.0f, 13.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f}) {
        appendLegacyFloat(legacyV2, value);
    }
    writer.setString("legacy-v2", legacyV2);
    const bool legacyV2Saved = writer.save();
    scene.getComponent<quantum::Transform>(entity).position = quantum::Vector3::ZERO;
    quantum::SaveGame legacyV2Reader("engine-completion-ecs-snapshot");
    const bool legacyV2Restored = legacyV2Saved && legacyV2Reader.load()
        && network->loadReplicatedState(legacyV2Reader, "legacy-v2")
        && scene.getComponent<quantum::Transform>(entity).position.x == 11.0f
        && scene.getEntityName(entity) == "legacy-v2";
    if (!legacyV2Restored) {
        std::cerr << "network-smoke: QNETECS2 details saved=" << legacyV2Saved
            << " loaded=" << legacyV2Reader.isValid()
            << " position=" << scene.getComponent<quantum::Transform>(entity).position.x
            << " name=" << scene.getEntityName(entity) << " bytes=" << legacyV2.size() << "\n";
    }
    writer.deleteSlot();
    return moved && restored && legacyRestored && legacyV2Restored;
}

bool runEntityReplicationSmoke() {
    quantum::Scene serverScene;
    quantum::Scene clientScene;
    const quantum::Entity serverEntity = serverScene.createEntity();
    serverScene.addComponent<quantum::Transform>(serverEntity);
    serverScene.addComponent<quantum::NetworkComponent>(serverEntity);
    serverScene.getComponent<quantum::Transform>(serverEntity).position.x = 3.25f;
    quantum::NetworkComponent& serverNetwork = serverScene.getComponent<quantum::NetworkComponent>(serverEntity);
    serverNetwork.ownerId = 1;
    serverNetwork.interestGroup = 7;
    serverNetwork.interestRadius = 24.0f;
    serverNetwork.serverAuthority = true;
    const auto server = serverScene.getSystem<quantum::NetworkSystem>();
    const auto client = clientScene.getSystem<quantum::NetworkSystem>();
    server->registerEntity(serverEntity);
    server->setAuthToken("authority-smoke-token");
    client->setAuthToken("authority-smoke-token");
    server->startServer(39180);
    client->startClient("127.0.0.1", 39180);
    for (int attempt = 0; attempt < 180; ++attempt) {
        client->update(0.01);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    auto* components = clientScene.getComponentArray<quantum::NetworkComponent>();
    if (!components || components->size() != 1) return false;
    const quantum::Entity clientEntity = components->getEntity(0);
    const uint32_t clientPeerId = client->getLocalPeerId();
    quantum::NetworkComponent& clientNetwork = clientScene.getComponent<quantum::NetworkComponent>(clientEntity);
    const bool policyReplicated = clientNetwork.ownerId == 1
        && clientNetwork.interestGroup == 7
        && std::abs(clientNetwork.interestRadius - 24.0f) < 0.0001f
        && clientNetwork.serverAuthority;
    const bool localAuthority = clientPeerId == 1 && client->hasEntityAuthority(clientEntity);
    clientNetwork.ownerId = clientPeerId + 1;
    const bool remoteDenied = !client->hasEntityAuthority(clientEntity);
    const bool serverAuthority = server->hasEntityAuthority(serverEntity);
    if (!server->registerServerMessageSchema(88, 1, nullptr, 2,
            validateGameplaySmokeCommand)) return false;
    const uint32_t validSequence = client->sendReliableMessage(88, 1, {0xCA, 0xFE});
    const uint32_t invalidSequence = client->sendReliableMessage(88, 1, {0x00});
    std::vector<uint8_t> acceptedCommand;
    for (int attempt = 0; attempt < 100; ++attempt) {
        client->update(0.01);
        server->pollReliable(acceptedCommand);
        if (!acceptedCommand.empty()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    quantum::network::MessageEnvelope acceptedEnvelope;
    const bool commandAccepted = validSequence != 0 && invalidSequence != 0
        && quantum::network::decodeMessage(acceptedCommand.data(), acceptedCommand.size(), acceptedEnvelope)
        && quantum::network::migrateMessage(acceptedEnvelope)
        && acceptedEnvelope.messageType == 88
        && acceptedEnvelope.payload == std::vector<uint8_t>({0xCA, 0xFE});
    std::vector<uint8_t> discardedCommand;
    for (int attempt = 0; attempt < 60; ++attempt) {
        client->update(0.01);
        server->pollReliable(discardedCommand);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const auto metrics = server->getMetrics();
    const bool replicated = clientScene.getComponent<quantum::Transform>(clientEntity).position.x > 3.2f;
    const bool passed = replicated
        && policyReplicated && localAuthority && remoteDenied && serverAuthority
        && commandAccepted && metrics.unauthorizedPackets > 0;
    if (!passed) {
        std::cerr << "network-smoke: entity details replicated=" << replicated
            << " policy=" << policyReplicated << " localAuthority=" << localAuthority
            << " remoteDenied=" << remoteDenied << " serverAuthority=" << serverAuthority
            << " commandAccepted=" << commandAccepted
            << " unauthorized=" << metrics.unauthorizedPackets
            << " reliableReceived=" << metrics.reliableReceived
            << " commandBytes=" << acceptedCommand.size() << "\n";
    }
    return passed;
}

bool runMultiPeerReplicationSmoke() {
    constexpr uint16_t port = 39181;
    quantum::Scene serverScene;
    quantum::Scene firstClientScene;
    quantum::Scene secondClientScene;
    quantum::Scene rejectedClientScene;

    const quantum::Entity serverEntity = serverScene.createEntity();
    serverScene.addComponent<quantum::Transform>(serverEntity);
    serverScene.addComponent<quantum::NetworkComponent>(serverEntity);
    serverScene.getComponent<quantum::Transform>(serverEntity).position = quantum::Vector3(6.0f, 1.0f, 2.0f);
    auto& serverNetworkComponent = serverScene.getComponent<quantum::NetworkComponent>(serverEntity);
    serverNetworkComponent.interestGroup = 4;
    serverNetworkComponent.interestRadius = 100.0f;

    const auto server = serverScene.getSystem<quantum::NetworkSystem>();
    const auto firstClient = firstClientScene.getSystem<quantum::NetworkSystem>();
    const auto secondClient = secondClientScene.getSystem<quantum::NetworkSystem>();
    const auto rejectedClient = rejectedClientScene.getSystem<quantum::NetworkSystem>();
    const std::string token = "multi-peer-smoke-token";
    for (const auto& network : {server, firstClient, secondClient, rejectedClient}) {
        network->setAuthToken(token);
    }
    server->setMaxPeers(2);
    server->registerEntity(serverEntity);
    server->startServer(port);
    firstClient->startClient("127.0.0.1", port);
    secondClient->startClient("127.0.0.1", port);

    auto pump = [&](int iterations, bool includeRejected) {
        for (int attempt = 0; attempt < iterations; ++attempt) {
            firstClient->update(0.01);
            secondClient->update(0.01);
            if (includeRejected) rejectedClient->update(0.01);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    };
    pump(220, false);
    const bool connected = server->getConnectedPeerCount() == 2
        && firstClient->isConnected() && secondClient->isConnected()
        && firstClient->getLocalPeerId() != 0 && secondClient->getLocalPeerId() != 0
        && firstClient->getLocalPeerId() != secondClient->getLocalPeerId();
    if (!connected) {
        std::cerr << "network-smoke: multi-peer handshake failed serverPeers="
            << server->getConnectedPeerCount() << " first=" << firstClient->getLocalPeerId()
            << " second=" << secondClient->getLocalPeerId() << "\n";
        server->destroy();
        firstClient->destroy();
        secondClient->destroy();
        return false;
    }

    const std::vector<uint8_t> reliablePayload = {0x51, 0x31, 0x02};
    const std::vector<uint8_t> unreliablePayload = {0x55, 0x44};
    const bool queued = server->sendReliable(reliablePayload) != 0
        && server->sendUnreliable(unreliablePayload) != 0;
    std::vector<uint8_t> firstReliable;
    std::vector<uint8_t> secondReliable;
    std::vector<uint8_t> firstUnreliable;
    std::vector<uint8_t> secondUnreliable;
    bool firstSnapshot = false;
    bool secondSnapshot = false;
    for (int attempt = 0; attempt < 220; ++attempt) {
        pump(1, false);
        firstClient->pollReliable(firstReliable);
        secondClient->pollReliable(secondReliable);
        firstClient->pollUnreliable(firstUnreliable);
        secondClient->pollUnreliable(secondUnreliable);
        const auto firstComponents = firstClientScene.getComponentArray<quantum::NetworkComponent>();
        const auto secondComponents = secondClientScene.getComponentArray<quantum::NetworkComponent>();
        if (firstComponents && firstComponents->size() == 1) {
            const quantum::Entity entity = firstComponents->getEntity(0);
            firstSnapshot = firstClientScene.getComponent<quantum::Transform>(entity).position.x > 5.9f;
        }
        if (secondComponents && secondComponents->size() == 1) {
            const quantum::Entity entity = secondComponents->getEntity(0);
            secondSnapshot = secondClientScene.getComponent<quantum::Transform>(entity).position.x > 5.9f;
        }
        if (firstReliable == reliablePayload && secondReliable == reliablePayload
                && firstUnreliable == unreliablePayload && secondUnreliable == unreliablePayload
                && firstSnapshot && secondSnapshot) break;
    }

    rejectedClient->startClient("127.0.0.1", port);
    pump(180, true);
    const bool rejected = !rejectedClient->isConnected() && server->getConnectedPeerCount() == 2;
    const bool passed = queued
        && firstReliable == reliablePayload && secondReliable == reliablePayload
        && firstUnreliable == unreliablePayload && secondUnreliable == unreliablePayload
        && firstSnapshot && secondSnapshot && rejected;
    if (!passed) {
        std::cerr << "network-smoke: multi-peer delivery failed queued=" << queued
            << " reliable=" << (firstReliable == reliablePayload) << "/"
            << (secondReliable == reliablePayload)
            << " unreliable=" << (firstUnreliable == unreliablePayload) << "/"
            << (secondUnreliable == unreliablePayload)
            << " snapshots=" << firstSnapshot << "/" << secondSnapshot
            << " rejected=" << rejected << " peers=" << server->getConnectedPeerCount() << "\n";
    }
    server->destroy();
    firstClient->destroy();
    secondClient->destroy();
    rejectedClient->destroy();
    return passed;
}

bool runLobbyDiscoverySmoke() {
    constexpr uint16_t port = 39182;
    quantum::Scene serverScene;
    const auto server = serverScene.getSystem<quantum::NetworkSystem>();
    const std::string token = "lobby-discovery-smoke-token";
    server->setAuthToken(token);
    server->setMaxPeers(8);
    server->setLobbyInfo({"Smoke Lobby", "network-test-map", "smoke-build-2026"});
    server->startServer(port);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    const std::vector<quantum::NetworkSystem::LobbyServer> discovered =
        quantum::NetworkSystem::discoverLan(port, 350, token, "127.0.0.1");
    const std::vector<quantum::NetworkSystem::LobbyServer> rejected =
        quantum::NetworkSystem::discoverLan(port, 120, "wrong-lobby-token", "127.0.0.1");
    const bool passed = discovered.size() == 1 && rejected.empty()
        && discovered[0].address == "127.0.0.1"
        && discovered[0].advertisement.gamePort == port
        && discovered[0].advertisement.currentPeers == 0
        && discovered[0].advertisement.maxPeers == 8
        && discovered[0].advertisement.serverName == "Smoke Lobby"
        && discovered[0].advertisement.mapName == "network-test-map"
        && discovered[0].advertisement.buildId == "smoke-build-2026";
    if (!passed) {
        std::cerr << "network-smoke: lobby discovery failed discovered=" << discovered.size()
            << " rejected=" << rejected.size();
        if (!discovered.empty()) {
            std::cerr << " address=" << discovered[0].address
                << " port=" << discovered[0].advertisement.gamePort
                << " peers=" << discovered[0].advertisement.currentPeers << "/"
                << discovered[0].advertisement.maxPeers;
        }
        std::cerr << "\n";
    } else {
        std::cout << "network-smoke: LAN lobby discovery and HMAC filtering OK\n";
    }
    server->destroy();
    return passed;
}

bool runLobbyReservationSmoke() {
    constexpr uint16_t port = 39183;
    constexpr uint16_t expirationPort = 39184;
    quantum::Scene serverScene;
    quantum::Scene reservedClientScene;
    quantum::Scene secondReservedClientScene;
    quantum::Scene unreservedClientScene;
    const auto server = serverScene.getSystem<quantum::NetworkSystem>();
    const auto reservedClient = reservedClientScene.getSystem<quantum::NetworkSystem>();
    const auto secondReservedClient = secondReservedClientScene.getSystem<quantum::NetworkSystem>();
    const auto unreservedClient = unreservedClientScene.getSystem<quantum::NetworkSystem>();
    const std::string token = "lobby-reservation-smoke-token";
    server->setAuthToken(token);
    reservedClient->setAuthToken(token);
    secondReservedClient->setAuthToken(token);
    unreservedClient->setAuthToken(token);
    server->setMaxPeers(2);
    server->startServer(port);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    const quantum::NetworkSystem::LobbyReservation reservation =
        quantum::NetworkSystem::reserveLan(port, 350, token, "127.0.0.1", 5);
    const quantum::NetworkSystem::LobbyReservation secondReservation =
        quantum::NetworkSystem::reserveLan(port, 350, token, "127.0.0.1", 5);
    const quantum::NetworkSystem::LobbyReservation wrongToken =
        quantum::NetworkSystem::reserveLan(port, 120, "wrong-reservation-token", "127.0.0.1", 5);
    if (!reservation.accepted || reservation.reservationId == 0
            || !secondReservation.accepted || secondReservation.reservationId == 0
            || secondReservation.reservationId == reservation.reservationId
            || wrongToken.accepted) {
        std::cerr << "network-smoke: lobby reservation request failed accepted="
            << reservation.accepted << " id=" << reservation.reservationId
            << " secondAccepted=" << secondReservation.accepted
            << " secondId=" << secondReservation.reservationId
            << " wrongToken=" << wrongToken.accepted << "\n";
        server->destroy();
        return false;
    }

    reservedClient->startClient("127.0.0.1", port, reservation.reservationId);
    secondReservedClient->startClient("127.0.0.1", port, secondReservation.reservationId);
    unreservedClient->startClient("127.0.0.1", port);
    for (int attempt = 0; attempt < 220; ++attempt) {
        reservedClient->update(0.01);
        secondReservedClient->update(0.01);
        unreservedClient->update(0.01);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const bool passed = reservedClient->isConnected()
        && secondReservedClient->isConnected()
        && !unreservedClient->isConnected()
        && server->getConnectedPeerCount() == 2;
    if (!passed) {
        const auto secondMetrics = secondReservedClient->getMetrics();
        const auto serverMetrics = server->getMetrics();
        std::cerr << "network-smoke: lobby reservation admission failed reserved="
            << reservedClient->isConnected() << " secondReserved="
            << secondReservedClient->isConnected() << " unreserved="
            << unreservedClient->isConnected() << " peers="
            << server->getConnectedPeerCount() << " secondSent="
            << secondMetrics.packetsSent << " secondReceived="
            << secondMetrics.packetsReceived << " secondUnauthorized="
            << secondMetrics.unauthorizedPackets << " serverReceived="
            << serverMetrics.packetsReceived << " serverUnauthorized="
            << serverMetrics.unauthorizedPackets << "\n";
    } else {
        std::cout << "network-smoke: authenticated lobby reservation admission and same-NAT slots OK\n";
    }
    server->destroy();
    reservedClient->destroy();
    secondReservedClient->destroy();
    unreservedClient->destroy();

    quantum::Scene expirationServerScene;
    quantum::Scene expiredClientScene;
    const auto expirationServer = expirationServerScene.getSystem<quantum::NetworkSystem>();
    const auto expiredClient = expiredClientScene.getSystem<quantum::NetworkSystem>();
    expirationServer->setAuthToken(token);
    expiredClient->setAuthToken(token);
    expirationServer->setMaxPeers(1);
    expirationServer->startServer(expirationPort);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    const quantum::NetworkSystem::LobbyReservation expiringReservation =
        quantum::NetworkSystem::reserveLan(expirationPort, 350, token, "127.0.0.1", 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    if (!expiringReservation.accepted || expiringReservation.reservationId == 0) {
        std::cerr << "network-smoke: short reservation was not issued\n";
        expirationServer->destroy();
        return false;
    }
    expiredClient->startClient("127.0.0.1", expirationPort, expiringReservation.reservationId);
    for (int attempt = 0; attempt < 120; ++attempt) {
        expiredClient->update(0.01);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const bool expirationPassed = !expiredClient->isConnected()
        && expirationServer->getConnectedPeerCount() == 0;
    if (!expirationPassed) {
        std::cerr << "network-smoke: expired lobby reservation was admitted\n";
    } else {
        std::cout << "network-smoke: expired lobby reservation rejected OK\n";
    }
    expirationServer->destroy();
    expiredClient->destroy();
    return passed && expirationPassed;
}

bool runAttachedLuaDebuggerSmoke() {
    lua_State* state = luaL_newstate();
    if (!state) return false;
    luaL_openlibs(state);

    quantum::LuaDebugger debugger(state);
    bool pauseCallbackCalled = false;
    debugger.setPauseCallback([&pauseCallbackCalled](bool paused) {
        pauseCallbackCalled = paused;
    });
    const int breakpointId = debugger.addBreakpoint("attached_smoke.lua", 2);
    const bool attached = breakpointId != 0 && debugger.attach();
    const char source[] = "local value = 1\nvalue = value + 1\nreturn value\n";
    const int loaded = attached
        ? luaL_loadbuffer(state, source, sizeof(source) - 1, "@attached_smoke.lua") : LUA_ERRRUN;
    const int executed = loaded == LUA_OK ? lua_pcall(state, 0, 0, 0) : loaded;
    const bool stopped = executed == LUA_OK && pauseCallbackCalled
        && debugger.getState() == quantum::LuaDebugger::State::Paused
        && !debugger.getCallStack().empty();
    const bool resumed = stopped && debugger.resume()
        && debugger.getState() == quantum::LuaDebugger::State::Running
        && !pauseCallbackCalled;
    debugger.shutdown();
    lua_close(state);
    return stopped && resumed;
}

bool runNetworkProtocolCompletionSmoke() {
    const std::string authKey = "network-smoke-auth-key";
    const std::string authMessage = "The quick brown fox jumps over the lazy dog";
    const std::string rfcKey(20, static_cast<char>(0x0b));
    const std::string rfcMessage = "Hi There";
    const std::vector<uint8_t> rfcHmac = quantum::network::hmacSha256(rfcKey,
        reinterpret_cast<const uint8_t*>(rfcMessage.data()), rfcMessage.size());
    const std::vector<uint8_t> expectedRfcHmac = {
        0xb0, 0x34, 0x4c, 0x61, 0xd8, 0xdb, 0x38, 0x53,
        0x5c, 0xa8, 0xaf, 0xce, 0xaf, 0x0b, 0xf1, 0x2b,
        0x88, 0x1d, 0xc2, 0x00, 0xc9, 0x83, 0x3d, 0xa7,
        0x26, 0xe9, 0x37, 0x6c, 0x2e, 0x32, 0xcf, 0xf7
    };
    if (rfcHmac != expectedRfcHmac) {
        std::cerr << "network-smoke: HMAC-SHA256 vector failed\n";
        return false;
    }
    const std::vector<uint8_t> packet = quantum::network::encodeReliable(
        quantum::network::PacketType::Data, 9, 0,
        reinterpret_cast<const uint8_t*>(authMessage.data()), authMessage.size());
    const std::vector<uint8_t> legacyPacket = quantum::network::authenticatePacket(packet, "");
    if (legacyPacket != packet) {
        std::cerr << "network-smoke: empty auth token changed legacy wire format\n";
        return false;
    }
    const std::vector<uint8_t> authenticated = quantum::network::authenticatePacket(packet, authKey);
    size_t authenticatedSize = 0;
    if (authenticated.size() != packet.size() + quantum::network::AUTHENTICATION_TAG_SIZE
            || !quantum::network::verifyPacketAuthentication(authenticated.data(), authenticated.size(),
                authKey, authenticatedSize)
            || authenticatedSize != packet.size()) {
        std::cerr << "network-smoke: authenticated packet verification failed\n";
        return false;
    }
    std::vector<uint8_t> tampered = authenticated;
    tampered[0] ^= 0x01;
    if (quantum::network::verifyPacketAuthentication(tampered.data(), tampered.size(), authKey,
            authenticatedSize)
            || quantum::network::verifyPacketAuthentication(packet.data(), packet.size(), authKey,
                authenticatedSize)) {
        std::cerr << "network-smoke: invalid authentication was accepted\n";
        return false;
    }

    if (!quantum::network::registerMessageSchema(77, 2, migrateSmokeMessage)) return false;
    const std::vector<uint8_t> oldPayload = {1, 2};
    const std::vector<uint8_t> encoded = quantum::network::encodeMessage(77, 1,
        oldPayload.data(), oldPayload.size());
    quantum::network::MessageEnvelope message;
    if (!quantum::network::decodeMessage(encoded.data(), encoded.size(), message)
            || !quantum::network::migrateMessage(message)
            || message.schemaVersion != 2
            || message.payload != std::vector<uint8_t>({1, 2, 0xEE})) {
        std::cerr << "network-smoke: schema details version=" << message.schemaVersion
            << " payload=" << message.payload.size() << "\n";
        return false;
    }

    std::vector<uint8_t> truncated(encoded.begin(), encoded.begin() + 3);
    quantum::network::MessageEnvelope malformedMessage;
    if (quantum::network::decodeMessage(truncated.data(), truncated.size(), malformedMessage)) {
        std::cerr << "network-smoke: truncated envelope was accepted\n";
        return false;
    }
    std::vector<uint8_t> badType = encoded;
    badType[0] = 0;
    badType[1] = 0;
    if (quantum::network::decodeMessage(badType.data(), badType.size(), malformedMessage)) {
        std::cerr << "network-smoke: zero message type was accepted\n";
        return false;
    }
    std::vector<uint8_t> badSchema = encoded;
    badSchema[2] = 0;
    badSchema[3] = 0;
    if (quantum::network::decodeMessage(badSchema.data(), badSchema.size(), malformedMessage)) {
        std::cerr << "network-smoke: zero schema version was accepted\n";
        return false;
    }
    const std::vector<uint8_t> future = quantum::network::encodeMessage(77, 99, oldPayload.data(), oldPayload.size());
    if (!quantum::network::decodeMessage(future.data(), future.size(), malformedMessage)
            || quantum::network::migrateMessage(malformedMessage)) {
        std::cerr << "network-smoke: unsupported future schema was accepted\n";
        return false;
    }

    quantum::network::TransformSnapshot source;
    source.netId = 9;
    source.position[0] = 12.34f;
    source.position[1] = -5.67f;
    source.position[2] = 0.01f;
    source.rotation[0] = 1.0f;
    source.scale[0] = 1.5f;
    const auto compressed = quantum::network::encodeCompressedTransformSnapshot(source, 3);
    const auto uncompressed = quantum::network::encodeTransformSnapshot(source, 3);
    quantum::network::PacketHeader header;
    quantum::network::TransformSnapshot decoded;
    const bool decodedOk = quantum::network::decodeTransformSnapshot(compressed.data(), compressed.size(), header, decoded);
    if (!(compressed.size() < uncompressed.size() && decodedOk && decoded.netId == source.netId
            && std::abs(decoded.position[0] - source.position[0]) < 0.011f
            && std::abs(decoded.position[1] - source.position[1]) < 0.011f)) {
        std::cerr << "network-smoke: compressed details sizes=" << compressed.size() << "/" << uncompressed.size()
            << " decoded=" << decodedOk << " netId=" << decoded.netId
            << " position=" << decoded.position[0] << "," << decoded.position[1] << "\n";
        return false;
    }
    quantum::network::EntityReplicationPolicy sourcePolicy;
    sourcePolicy.netId = 9;
    sourcePolicy.ownerId = 3;
    sourcePolicy.interestGroup = 5;
    sourcePolicy.interestRadius = 42.5f;
    sourcePolicy.serverAuthority = false;
    const auto policyPacket = quantum::network::encodeReplicationPolicy(sourcePolicy, 4);
    quantum::network::EntityReplicationPolicy decodedPolicy;
    const bool policyDecoded = quantum::network::decodeReplicationPolicy(
        policyPacket.data(), policyPacket.size(), header, decodedPolicy);
    if (!policyDecoded || decodedPolicy.netId != sourcePolicy.netId
            || decodedPolicy.ownerId != sourcePolicy.ownerId
            || decodedPolicy.interestGroup != sourcePolicy.interestGroup
            || std::abs(decodedPolicy.interestRadius - sourcePolicy.interestRadius) > 0.0001f
            || decodedPolicy.serverAuthority != sourcePolicy.serverAuthority) {
        std::cerr << "network-smoke: replication policy packet round-trip failed\n";
        return false;
    }
    return true;
}

bool runSaveGameSmoke() {
    quantum::SaveGame writer("engine-completion-smoke");
    if (!writer.isValid()) return false;
    writer.deleteSlot();
    writer.setString("player.name", "Quantum");
    writer.setInt("player.level", 7);
    writer.setNumber("player.x", 12.5);
    writer.setBlob("player.payload", {0x00, 0x01, 0x7f, 0xff});
    if (!writer.save() || !writer.exists()) return false;
    // Saving over an existing slot must replace it atomically and leave the newest complete
    // record readable, rather than deleting the previous target before the replacement.
    writer.setString("player.name", "Quantum Reloaded");
    if (!writer.save()) return false;

    quantum::SaveGame reader("engine-completion-smoke");
    std::vector<uint8_t> blob;
    if (!reader.load() || reader.getString("player.name") != "Quantum Reloaded"
        || reader.getInt("player.level", -1) != 7
        || reader.getNumber("player.x", -1.0) != 12.5
        || !reader.getBlob("player.payload", blob)
        || blob != std::vector<uint8_t>({0x00, 0x01, 0x7f, 0xff})) return false;

    const std::filesystem::path savedPath =
        std::filesystem::current_path() / "saves" / "engine-completion-smoke.qsave";
    std::ifstream savedInput(savedPath, std::ios::binary);
    std::vector<uint8_t> savedBytes((std::istreambuf_iterator<char>(savedInput)),
        std::istreambuf_iterator<char>());
    savedInput.close();
    if (savedBytes.size() < 4) return false;
    savedBytes.back() ^= 0x01;
    std::ofstream corrupted(savedPath, std::ios::binary | std::ios::trunc);
    corrupted.write(reinterpret_cast<const char*>(savedBytes.data()),
        static_cast<std::streamsize>(savedBytes.size()));
    corrupted.close();
    const bool corruptedAccepted = reader.load();
    if (corruptedAccepted) {
        std::cerr << "network-smoke: corrupted QSV1 checksum was accepted\n";
        return false;
    }
    if (!reader.deleteSlot()) {
        std::cerr << "network-smoke: corrupted QSV1 slot could not be deleted\n";
        return false;
    }

    // Write a v1 slot directly to prove the built-in v1 -> v3 migration path.
    const std::filesystem::path legacyPath =
        std::filesystem::current_path() / "saves" / "engine-completion-smoke.qsave";
    std::filesystem::create_directories(legacyPath.parent_path());
    std::ofstream legacy(legacyPath, std::ios::binary | std::ios::trunc);
    auto writeU32 = [&legacy](uint32_t value) {
        const char bytes[4] = { static_cast<char>(value & 0xffu),
            static_cast<char>((value >> 8) & 0xffu), static_cast<char>((value >> 16) & 0xffu),
            static_cast<char>((value >> 24) & 0xffu) };
        legacy.write(bytes, sizeof(bytes));
    };
    legacy.write("QSV1", 4);
    writeU32(1);
    writeU32(1);
    writeU32(6);
    legacy.write("legacy", 6);
    writeU32(5);
    legacy.write("ready", 5);
    legacy.close();

    quantum::SaveGame migrated("engine-completion-smoke");
    const bool migratedOk = migrated.load() && migrated.getString("legacy") == "ready";
    return migratedOk && migrated.deleteSlot();
}

bool runInputActionSmoke() {
    quantum::InputActionMap map;
    if (quantum::Input::setGamepadVibration(-1, 1.0f, 1.0f)
            || quantum::Input::stopGamepadVibration(-1)) return false;
    map.bindKey("move_forward", D_KEY_W);
    map.bindKeyChord("quick_save", D_KEY_S, D_MODIFIER_CONTROL);
    map.bindMouseButton("fire", D_MOUSE_BUTTON_LEFT, 1.0f, "gameplay");
    map.bindGamepadAxis("look_x", 0, D_GAMEPAD_AXIS_RIGHT_X, 1.0f, 0.2f, "gameplay");
    map.bindGesture("tap", static_cast<int>(quantum::GestureType::Tap));
    map.setContext("menu", true, 10);
    map.setContext("gameplay", false, 0);
    map.update();
    if (!map.hasAction("move_forward") || !map.hasAction("quick_save") || map.pressed("move_forward") || map.value("look_x") != 0.0f
        || map.isContextEnabled("gameplay")) return false;

    quantum::SaveGame save("engine-completion-input-smoke");
    save.deleteSlot();
    if (!map.saveBindings(save) || !save.save()) return false;
    quantum::SaveGame loadedSave("engine-completion-input-smoke");
    quantum::InputActionMap loaded;
    const bool loadedOk = loadedSave.load() && loaded.loadBindings(loadedSave)
        && loaded.hasAction("move_forward") && loaded.hasAction("fire") && loaded.hasAction("look_x")
        && loaded.hasAction("quick_save") && loaded.hasAction("tap")
        && !loaded.isContextEnabled("gameplay") && loaded.isContextEnabled("menu");
    loadedSave.deleteSlot();
    if (!loadedOk) return false;

    quantum::InputActionMap gestureMap;
    gestureMap.bindGesture("swipe", static_cast<int>(quantum::GestureType::Swipe));
    gestureMap.bindGesture("hold", static_cast<int>(quantum::GestureType::LongPress));
    quantum::Input::injectTouchStart(7, 10.0f, 10.0f);
    quantum::Input::injectTouchMove(7, 90.0f, 10.0f);
    quantum::Input::injectTouchEnd(7, 90.0f, 10.0f, 0.1f);
    quantum::Input::injectTouchStart(8, 20.0f, 20.0f);
    quantum::Input::injectTouchEnd(8, 20.0f, 20.0f, 0.75f);
    gestureMap.update();
    if (!gestureMap.justPressed("swipe") || !gestureMap.justPressed("hold")) return false;
    gestureMap.update();
    if (!gestureMap.justReleased("swipe") || !gestureMap.justReleased("hold")) return false;

    quantum::InputActionMap eventMap;
    eventMap.bindKey("jump", 65);
    quantum::EventBus eventBus;
    std::atomic<uint32_t> actionEvents{0};
    std::atomic<uint32_t> callbackPresses{0};
    std::atomic<uint32_t> callbackReleases{0};
    std::string callbackAction;
    float callbackValue = 0.0f;
    eventMap.onActionPressed = [&callbackPresses, &callbackAction, &callbackValue](const std::string& action, float value) {
        callbackAction = action;
        callbackValue = value;
        ++callbackPresses;
    };
    eventMap.onActionReleased = [&callbackReleases](const std::string& action, float value) {
        (void)action;
        (void)value;
        ++callbackReleases;
    };
    const auto subscription = eventBus.subscribe("input.action.jump.pressed",
        [&actionEvents](const quantum::EventBus::Event& event) {
            if (event.payload.size() == sizeof(float)) ++actionEvents;
        });
    if (subscription == 0) return false;
    quantum::Input::injectKeyState(65, true);
    eventMap.update();
    eventMap.publishTransitions(eventBus);
    eventBus.dispatchQueued();
    if (callbackPresses != 1 || callbackAction != "jump" || callbackValue < 0.99f) return false;
    quantum::Input::injectKeyState(65, false);
    eventMap.update();
    if (callbackReleases != 1) return false;
    eventBus.clear();
    if (actionEvents != 1) return false;

    // A modal context must mask lower-priority bindings even when its own
    // binding is currently idle; otherwise gameplay actions leak into menus.
    quantum::InputActionMap priorityMap;
    priorityMap.bindKey("confirm", 66, 1.0f, "gameplay");
    priorityMap.bindKey("confirm", 67, 1.0f, "menu");
    priorityMap.setContext("gameplay", true, 0);
    priorityMap.setContext("menu", true, 10);
    quantum::Input::injectKeyState(66, true);
    quantum::Input::injectKeyState(67, false);
    priorityMap.update();
    const bool masked = priorityMap.value("confirm") == 0.0f;
    quantum::Input::injectKeyState(66, false);
    quantum::Input::injectKeyState(67, true);
    priorityMap.update();
    const bool menuWins = priorityMap.justPressed("confirm") && priorityMap.value("confirm") > 0.99f;
    quantum::Input::injectKeyState(67, false);

    quantum::SaveGame prioritySave("engine-completion-input-priority");
    prioritySave.deleteSlot();
    if (!priorityMap.saveBindings(prioritySave) || !prioritySave.save()) return false;
    quantum::SaveGame loadedPrioritySave("engine-completion-input-priority");
    quantum::InputActionMap loadedPriority;
    const bool loadedPriorityOk = loadedPrioritySave.load() && loadedPriority.loadBindings(loadedPrioritySave);
    quantum::Input::injectKeyState(66, true);
    quantum::Input::injectKeyState(67, false);
    loadedPriority.update();
    const bool persistedMask = loadedPriorityOk && loadedPriority.value("confirm") == 0.0f;
    quantum::Input::injectKeyState(66, false);
    loadedPrioritySave.deleteSlot();
    return masked && menuWins && persistedMask;
}

bool runAudioBusSmoke() {
    const quantum::Vector3 sourceVelocity = quantum::AudioSystem::calculateDopplerVelocity(
        quantum::Vector3(10.0f, 0.0f, 0.0f), quantum::Vector3::ZERO, 0.5);
    const quantum::Vector3 listenerVelocity = quantum::AudioSystem::calculateDopplerVelocity(
        quantum::Vector3::ZERO, quantum::Vector3(0.0f, 4.0f, 0.0f), 0.0);
    if (std::abs(sourceVelocity.x - 20.0f) > 0.0001f
            || std::abs(sourceVelocity.y) > 0.0001f
            || listenerVelocity.length() > 0.0001f) return false;
    quantum::AudioSystem::setBusVolume("master", 0.8f);
    quantum::AudioSystem::setBusVolume("music", 0.5f);
    quantum::AudioSystem::setBusVolume("music/combat", 0.25f);
    const float nested = quantum::AudioSystem::getBusVolume("music/combat");
    quantum::AudioSystem::setBusVolume("master", 1.0f);
    quantum::AudioSystem::setBusVolume("music", 1.0f);
    quantum::AudioSystem::setBusVolume("music/combat", 1.0f);
    return nested > 0.099f && nested < 0.101f;
}

bool runCollisionFilterSmoke() {
    quantum::Body2DComponent body2D;
    quantum::Body3DComponent body3D;
    if (body2D.numShapes != 0 || body3D.categoryBits != 1 || body3D.maskBits != 0xFFFF) {
        return false;
    }

    const JPH::ObjectLayer layerA = JPH::ObjectLayerPairFilterMask::sGetObjectLayer(1, 2);
    const JPH::ObjectLayer layerB = JPH::ObjectLayerPairFilterMask::sGetObjectLayer(2, 1);
    const JPH::ObjectLayer layerC = JPH::ObjectLayerPairFilterMask::sGetObjectLayer(4, 1);
    JPH::ObjectLayerPairFilterMask filter;
    return filter.ShouldCollide(layerA, layerB) && !filter.ShouldCollide(layerA, layerC);
}

bool runPhysicsSimulationSmoke() {
    quantum::Scene scene;
    auto physics = scene.getSystem<quantum::PhysicsSystem>();
    physics->setGravity2D(0.0f, -9.81f);
    physics->setDebugDrawEnabled(true);

    const quantum::Entity floor = scene.createEntity();
    scene.addComponent<quantum::Transform>(floor);
    scene.addComponent<quantum::Body2DComponent>(floor);
    auto& floorBody = scene.getComponent<quantum::Body2DComponent>(floor);
    floorBody.type = quantum::BodyType::STATIC;
    floorBody.numShapes = 1;
    floorBody.shapes[0].type = quantum::Shape2DType::POLYGON;
    floorBody.shapes[0].pointA = quantum::Vector2(-200.0f, -5.0f);
    floorBody.shapes[0].pointB = quantum::Vector2(200.0f, 5.0f);

    const quantum::Entity ball = scene.createEntity();
    scene.addComponent<quantum::Transform>(ball);
    scene.getComponent<quantum::Transform>(ball).position = quantum::Vector3(0.0f, 120.0f, 0.0f);
    scene.addComponent<quantum::Body2DComponent>(ball);
    auto& ballBody = scene.getComponent<quantum::Body2DComponent>(ball);
    ballBody.type = quantum::BodyType::DYNAMIC;
    ballBody.numShapes = 1;
    ballBody.shapes[0].type = quantum::Shape2DType::CIRCLE;
    ballBody.shapes[0].radius = 10.0f;
    ballBody.shapes[0].density = 1.0f;

    const quantum::Entity sensor = scene.createEntity();
    scene.addComponent<quantum::Transform>(sensor);
    scene.getComponent<quantum::Transform>(sensor).position = quantum::Vector3(40.0f, 120.0f, 0.0f);
    scene.addComponent<quantum::Body2DComponent>(sensor);
    auto& sensorBody = scene.getComponent<quantum::Body2DComponent>(sensor);
    sensorBody.type = quantum::BodyType::STATIC;
    sensorBody.numShapes = 1;
    sensorBody.shapes[0].type = quantum::Shape2DType::CIRCLE;
    sensorBody.shapes[0].radius = 6.0f;
    sensorBody.shapes[0].sensor = true;

    physics->fixedUpdate(1.0 / 60.0);
    const quantum::Ray ballQuery(quantum::Vector3(0.0f, 120.0f, 0.0f), quantum::Vector3::ZERO);
    const std::vector<quantum::Entity> ballOverlaps = ballQuery.overlaps(&scene, quantum::RayFilter::BODY_2D, 12.0f);
    const quantum::RayReturn ballCast = quantum::Ray(
        quantum::Vector3(-100.0f, 120.0f, 0.0f), quantum::Vector3(200.0f, 0.0f, 0.0f))
        .cast(&scene, quantum::RayFilter::BODY_2D, 5.0f);
    const bool query2D = std::find(ballOverlaps.begin(), ballOverlaps.end(), ball) != ballOverlaps.end()
        && ballCast.hit && ballCast.body == ball;

    for (int step = 0; step < 180; ++step) {
        physics->fixedUpdate(1.0 / 60.0);
    }

    const float finalY = scene.getComponent<quantum::Transform>(ball).position.y;
    const bool settledOnFloor = finalY > 5.0f && finalY < 35.0f;
    const bool debug2D = physics->getDebugLineCount() > 0;
    bool sensorColor = false;
    const quantum::Entity debugEntity = scene.findEntity("__quantum_physics_debug__");
    if (debugEntity != NULL_ENTITY) {
        const quantum::LinesComponent* debugLines = scene.findComponent<quantum::LinesComponent>(debugEntity);
        if (debugLines) {
            for (const quantum::LineData& line : debugLines->lines) {
                if (std::abs(line.colorA.x - 1.0f) < 0.001f
                    && std::abs(line.colorA.y - 0.75f) < 0.001f
                    && std::abs(line.colorA.z - 0.15f) < 0.001f) {
                    sensorColor = true;
                    break;
                }
            }
        }
    }
    const size_t debugLineCount = physics->getDebugLineCount();
    physics->setDebugDrawEnabled(false);
    scene.destroy();
    if (!settledOnFloor || !debug2D || !sensorColor || !query2D) {
        std::cerr << "network-smoke: 2D details finalY=" << finalY
                  << " settled=" << settledOnFloor
                  << " debugLines=" << debugLineCount
                  << " sensorColor=" << sensorColor
                  << " query=" << query2D << "\n";
        return false;
    }

    quantum::Scene scene3D;
    auto physics3D = scene3D.getSystem<quantum::PhysicsSystem>();
    physics3D->setGravity3D(0.0f, -9.81f, 0.0f);
    physics3D->setDebugDrawEnabled(true);

    const quantum::Entity floor3D = scene3D.createEntity();
    scene3D.addComponent<quantum::Transform>(floor3D);
    scene3D.addComponent<quantum::Body3DComponent>(floor3D);
    auto& floorBody3D = scene3D.getComponent<quantum::Body3DComponent>(floor3D);
    floorBody3D.type = quantum::BodyType::STATIC;
    floorBody3D.numShapes = 1;
    floorBody3D.shapes[0].type = quantum::Shape3DType::BOX;
    floorBody3D.shapes[0].width = 20.0f;
    floorBody3D.shapes[0].height = 1.0f;
    floorBody3D.shapes[0].depth = 20.0f;
    floorBody3D.shapes[0].friction = 0.3f;
    floorBody3D.shapes[0].restitution = 0.25f;

    const quantum::Entity sphere3D = scene3D.createEntity();
    scene3D.addComponent<quantum::Transform>(sphere3D);
    scene3D.getComponent<quantum::Transform>(sphere3D).position = quantum::Vector3(0.0f, 5.0f, 0.0f);
    scene3D.addComponent<quantum::Body3DComponent>(sphere3D);
    auto& sphereBody3D = scene3D.getComponent<quantum::Body3DComponent>(sphere3D);
    sphereBody3D.type = quantum::BodyType::DYNAMIC;
    sphereBody3D.numShapes = 1;
    sphereBody3D.shapes[0].type = quantum::Shape3DType::SPHERE;
    sphereBody3D.shapes[0].radius = 0.5f;
    sphereBody3D.shapes[0].friction = 0.8f;
    sphereBody3D.shapes[0].restitution = 0.4f;
    sphereBody3D.linearDamping = 0.37f;
    sphereBody3D.angularDamping = 0.41f;

    const quantum::Entity occluder = scene3D.createEntity();
    scene3D.addComponent<quantum::Transform>(occluder);
    scene3D.getComponent<quantum::Transform>(occluder).position = quantum::Vector3(0.0f, 1.0f, 0.0f);
    scene3D.addComponent<quantum::Body3DComponent>(occluder);
    auto& occluderBody = scene3D.getComponent<quantum::Body3DComponent>(occluder);
    occluderBody.type = quantum::BodyType::STATIC;
    occluderBody.numShapes = 1;
    occluderBody.shapes[0].type = quantum::Shape3DType::BOX;
    occluderBody.shapes[0].width = 0.5f;
    occluderBody.shapes[0].height = 4.0f;
    occluderBody.shapes[0].depth = 4.0f;
    occluderBody.shapes[0].friction = 0.3f;
    occluderBody.shapes[0].restitution = 0.25f;

    float materialFriction = -1.0f;
    float materialRestitution = -1.0f;
    const bool materialSubscription = physics3D->onContactAdded3D.add(
        "physics-material-smoke",
        [&](quantum::Body3D bodyA, quantum::Body3D bodyB, quantum::Contact3D contact) {
            const quantum::Entity a = bodyA.getEntity();
            const quantum::Entity b = bodyB.getEntity();
            if ((a == sphere3D && b == floor3D) || (a == floor3D && b == sphere3D)) {
                materialFriction = contact.getCombinedFriction();
                materialRestitution = contact.getCombinedRestitution();
            }
        });

    physics3D->fixedUpdate(1.0 / 60.0);
    quantum::Body3D sphereHandle(&scene3D, sphere3D);
    const bool persistedDamping = std::abs(sphereHandle.getLinearDamping() - 0.37f) < 0.001f
        && std::abs(sphereHandle.getAngularDamping() - 0.41f) < 0.001f;
    sphereHandle.setLinearVelocity(quantum::Vector3(1.0f, 0.0f, 0.0f));
    physics3D->fixedUpdate(1.0 / 60.0);
    const float dampedVelocityX = sphereHandle.getLinearVelocity().x;
    const bool damping3D = persistedDamping && dampedVelocityX > 0.98f && dampedVelocityX < 1.0f;
    const quantum::Ray sphereQuery(quantum::Vector3(0.0f, 5.0f, 0.0f), quantum::Vector3::ZERO);
    const std::vector<quantum::Entity> sphereOverlaps = sphereQuery.overlaps(
        &scene3D, quantum::RayFilter::BODY_3D, 1.0f);
    const quantum::RayReturn sphereCast = quantum::Ray(
        quantum::Vector3(-5.0f, 5.0f, 0.0f), quantum::Vector3(10.0f, 0.0f, 0.0f))
        .cast(&scene3D, quantum::RayFilter::BODY_3D, 0.25f);
    const bool query3D = std::find(sphereOverlaps.begin(), sphereOverlaps.end(), sphere3D) != sphereOverlaps.end()
        && sphereCast.hit && sphereCast.body == sphere3D;
    const quantum::Ray blockedRay(
        quantum::Vector3(-5.0f, 1.0f, 0.0f), quantum::Vector3(10.0f, 0.0f, 0.0f));
    const bool blocked = blockedRay.intersects(&scene3D, quantum::RayFilter::BODY_3D, true).hit;
    scene3D.destroyEntity(occluder);
    physics3D->fixedUpdate(1.0 / 60.0);
    const bool clear = !blockedRay.intersects(&scene3D, quantum::RayFilter::BODY_3D, true).hit;

    for (int step = 0; step < 180; ++step) {
        physics3D->fixedUpdate(1.0 / 60.0);
    }

    const float finalY3D = scene3D.getComponent<quantum::Transform>(sphere3D).position.y;
    const bool debug3D = physics3D->getDebugLineCount() > 0;
    const bool material3D = materialSubscription
        && materialFriction > 0.48f && materialFriction < 0.50f
        && materialRestitution > 0.39f && materialRestitution < 0.41f;
    physics3D->onContactAdded3D.remove("physics-material-smoke");
    physics3D->setDebugDrawEnabled(false);
    scene3D.destroy();
    if (!(finalY3D > 0.0f && finalY3D < 2.0f && blocked && clear && debug3D && query3D && material3D && damping3D)) {
        std::cerr << "network-smoke: 3D details finalY=" << finalY3D
                  << " blocked=" << blocked << " clear=" << clear
                  << " debug=" << debug3D << " query=" << query3D
                  << " materialSubscription=" << materialSubscription
                  << " friction=" << materialFriction
                  << " restitution=" << materialRestitution
                  << " damping=" << damping3D
                  << " velocityX=" << dampedVelocityX << "\n";
        return false;
    }
    return true;
}

bool runParticleCollisionSmoke() {
    quantum::Scene scene;
    auto physics = scene.getSystem<quantum::PhysicsSystem>();
    physics->setGravity3D(0.0f, 0.0f, 0.0f);

    const quantum::Entity floor = scene.createEntity();
    scene.addComponent<quantum::Transform>(floor);
    scene.addComponent<quantum::Body3DComponent>(floor);
    auto& floorBody = scene.getComponent<quantum::Body3DComponent>(floor);
    floorBody.type = quantum::BodyType::STATIC;
    floorBody.numShapes = 1;
    floorBody.shapes[0].type = quantum::Shape3DType::BOX;
    floorBody.shapes[0].width = 10.0f;
    floorBody.shapes[0].height = 1.0f;
    floorBody.shapes[0].depth = 10.0f;

    const quantum::Entity target = scene.createEntity();
    scene.addComponent<quantum::Transform>(target);
    scene.addComponent<quantum::PointsComponent>(target);
    scene.addComponent<quantum::LinesComponent>(target);

    const quantum::Entity emitter = scene.createEntity();
    scene.addComponent<quantum::ParticlesComponent>(emitter);
    scene.addComponent<quantum::ActionComponent>(emitter);
    auto& particles = scene.getComponent<quantum::ParticlesComponent>(emitter);
    particles.maxParticles = 1;
    particles.emitter = true;
    particles.loop = false;
    particles.rate = 60;
    particles.maxPerUpdate = 1;
    particles.collisionEnabled = true;
    particles.collisionBounce = 1.0f;
    particles.collisionFriction = 0.0f;
    particles.trailsEnabled = true;
    particles.trailLength = 4;
    particles.lifeInitializer.minLife = 5.0f;
    particles.lifeInitializer.maxLife = 5.0f;
    particles.positionInitializer.minPosition = quantum::Vector3(0.0f, 2.0f, 0.0f);
    particles.positionInitializer.maxPosition = particles.positionInitializer.minPosition;
    particles.velocityInitializer.minVelocity = quantum::Vector3(0.0f, -5.0f, 0.0f);
    particles.velocityInitializer.maxVelocity = particles.velocityInitializer.minVelocity;
    auto& action = scene.getComponent<quantum::ActionComponent>(emitter);
    action.target = target;
    action.startTrigger = true;

    auto actions = scene.getSystem<quantum::ActionSystem>();
    physics->fixedUpdate(1.0 / 60.0);
    for (int step = 0; step < 30; ++step) {
        actions->update(1.0 / 60.0);
        physics->fixedUpdate(1.0 / 60.0);
    }

    const bool spawned = particles.particles.size() == 1 && !scene.getComponent<quantum::PointsComponent>(target).points.empty();
    const quantum::ParticleData& particle = particles.particles[0];
    const bool bounced = spawned && particle.position.y > 0.0f && particle.velocity.y > 0.0f;
    const auto& trailLines = scene.getComponent<quantum::LinesComponent>(target).lines;
    const size_t trailLineCount = trailLines.size();
    const bool trailGenerated = trailLineCount >= 3 && trailLines[0].colorA.w > 0.0f;
    scene.destroy();
    if (!bounced || !trailGenerated) {
        std::cerr << "network-smoke: particle collision details spawned=" << spawned
                  << " positionY=" << particle.position.y
                  << " velocityY=" << particle.velocity.y
                  << " trailLines=" << trailLineCount
                  << " trailGenerated=" << trailGenerated << "\n";
    }
    return bounced && trailGenerated;
}

bool runJoint3DSmoke() {
    quantum::Scene scene;
    auto physics = scene.getSystem<quantum::PhysicsSystem>();
    physics->setGravity3D(0.0f, -9.81f, 0.0f);

    const quantum::Entity bodyA = scene.createEntity();
    scene.addComponent<quantum::Transform>(bodyA);
    scene.getComponent<quantum::Transform>(bodyA).position = quantum::Vector3(-1.0f, 4.0f, 0.0f);
    scene.addComponent<quantum::Body3DComponent>(bodyA);
    auto& bodyAData = scene.getComponent<quantum::Body3DComponent>(bodyA);
    bodyAData.type = quantum::BodyType::DYNAMIC;
    bodyAData.numShapes = 1;
    bodyAData.shapes[0].type = quantum::Shape3DType::BOX;
    bodyAData.shapes[0].width = 0.5f;
    bodyAData.shapes[0].height = 0.5f;
    bodyAData.shapes[0].depth = 0.5f;

    const quantum::Entity bodyB = scene.createEntity();
    scene.addComponent<quantum::Transform>(bodyB);
    scene.getComponent<quantum::Transform>(bodyB).position = quantum::Vector3(1.0f, 4.0f, 0.0f);
    scene.addComponent<quantum::Body3DComponent>(bodyB);
    auto& bodyBData = scene.getComponent<quantum::Body3DComponent>(bodyB);
    bodyBData.type = quantum::BodyType::DYNAMIC;
    bodyBData.numShapes = 1;
    bodyBData.shapes[0].type = quantum::Shape3DType::BOX;
    bodyBData.shapes[0].width = 0.5f;
    bodyBData.shapes[0].height = 0.5f;
    bodyBData.shapes[0].depth = 0.5f;

    const quantum::Entity jointEntity = scene.createEntity();
    scene.addComponent<quantum::Joint3DComponent>(jointEntity);
    auto& joint = scene.getComponent<quantum::Joint3DComponent>(jointEntity);
    joint.type = quantum::Joint3DType::FIXED;
    joint.bodyA = bodyA;
    joint.bodyB = bodyB;

    physics->fixedUpdate(1.0 / 60.0);
    const bool created = physics->loadFixedJoint3D(joint, bodyA, bodyB) && joint.joint != nullptr;
    for (int step = 0; step < 120; ++step) physics->fixedUpdate(1.0 / 60.0);

    const quantum::Vector3 a = scene.getComponent<quantum::Transform>(bodyA).position;
    const quantum::Vector3 b = scene.getComponent<quantum::Transform>(bodyB).position;
    const quantum::Vector3 delta = b - a;
    const float separation = delta.length();
    const bool preservedDistance = separation > 1.5f && separation < 2.5f;
    physics->destroyJoint3D(joint);
    const bool destroyed = joint.joint == nullptr;
    scene.destroy();
    return created && preservedDistance && destroyed;
}

bool runShutdownStressSmoke() {
    // Repeated scene teardown is intentionally kept in one thread: ECS, physics and render
    // resources are main-thread-owned, while EventBus concurrency is covered independently above.
    for (int cycle = 0; cycle < 32; ++cycle) {
        quantum::Scene scene;
        auto physics = scene.getSystem<quantum::PhysicsSystem>();
        physics->setGravity3D(0.0f, -9.81f, 0.0f);

        const quantum::Entity body = scene.createEntity();
        scene.addComponent<quantum::Transform>(body);
        scene.getComponent<quantum::Transform>(body).position = quantum::Vector3(0.0f, 2.0f, 0.0f);
        scene.addComponent<quantum::Body3DComponent>(body);
        auto& bodyData = scene.getComponent<quantum::Body3DComponent>(body);
        bodyData.type = quantum::BodyType::DYNAMIC;
        bodyData.numShapes = 1;
        bodyData.shapes[0].type = quantum::Shape3DType::SPHERE;
        bodyData.shapes[0].radius = 0.25f;

        for (int frame = 0; frame < 4; ++frame) physics->fixedUpdate(1.0 / 60.0);
        scene.destroy();
    }
    return true;
}

bool runAnimatorStateMachineSmoke() {
    quantum::Scene scene;
    const quantum::Entity entity = scene.createEntity();
    scene.addComponent<quantum::AnimatorControllerComponent>(entity);
    auto& animator = scene.getComponent<quantum::AnimatorControllerComponent>(entity);
    const quantum::Entity animationEntity = scene.createEntity();
    scene.addComponent<quantum::AnimationComponent>(animationEntity);
    scene.addComponent<quantum::ActionComponent>(animationEntity);
    auto& animation = scene.getComponent<quantum::AnimationComponent>(animationEntity);
    animation.duration = 1.0f;

    quantum::AnimatorState idle;
    idle.id = 1;
    idle.name = "Idle";
    idle.isDefault = true;
    quantum::AnimatorClip clip;
    clip.animation = animationEntity;
    clip.loop = true;
    clip.events.push_back({0.2f, "footstep", "left"});
    idle.clips.push_back(clip);
    quantum::AnimatorState run;
    run.id = 2;
    run.name = "Run";
    animator.states = {idle, run};

    quantum::AnimatorParameter sprint;
    sprint.name = "sprint";
    sprint.type = quantum::AnimatorParameter::Type::Trigger;
    animator.parameters.push_back(sprint);

    quantum::AnimatorTransition toRun;
    toRun.id = 10;
    toRun.fromState = idle.id;
    toRun.toState = run.id;
    toRun.transitionDuration = 0.1f;
    quantum::AnimatorTransition::Condition condition;
    condition.parameter = "sprint";
    toRun.conditions.push_back(condition);

    quantum::AnimatorTransition toExit;
    toExit.id = 11;
    toExit.fromState = run.id;
    toExit.toState = quantum::ANIMATOR_EXIT_STATE;
    toExit.hasExitTime = true;
    toExit.exitTime = 0.5f;
    animator.transitions = {toRun, toExit};

    std::string previous;
    std::string current;
    std::string eventName;
    std::string eventParameter;
    animator.onStateChange.add("animator-smoke", [&previous, &current](const std::string& from, const std::string& to) {
        previous = from;
        current = to;
    });
    animator.onEvent.add("animator-event-smoke", [&eventName, &eventParameter](const std::string& name, const std::string& parameter) {
        eventName = name;
        eventParameter = parameter;
    });

    scene.getSystem<quantum::ActionSystem>()->update(0.016);
    if (!animator.isPlaying || animator.getCurrentStateName() != "Idle") {
        return false;
    }

    scene.getSystem<quantum::ActionSystem>()->update(0.1);
    if (eventName != "" || eventParameter != "") {
        return false;
    }
    scene.getSystem<quantum::ActionSystem>()->update(0.1);
    if (eventName != "footstep" || eventParameter != "left") {
        return false;
    }

    animator.setTrigger("sprint");
    scene.getSystem<quantum::ActionSystem>()->update(0.016);
    if (!animator.isPlaying || animator.getCurrentStateName() != "Run"
            || previous != "Idle" || current != "Run" || animator.getBool("sprint")) {
        return false;
    }

    scene.getSystem<quantum::ActionSystem>()->update(0.5);
    return !animator.isPlaying && animator.getCurrentStateName() == "Run"
        && previous == "Run" && current.empty();
}

bool runRootMotionSmoke() {
    quantum::Scene scene;
    const quantum::Entity controllerEntity = scene.createEntity();
    scene.addComponent<quantum::Transform>(controllerEntity);
    scene.addComponent<quantum::AnimatorControllerComponent>(controllerEntity);
    auto& animator = scene.getComponent<quantum::AnimatorControllerComponent>(controllerEntity);
    animator.rootMotionEnabled = true;
    animator.rootMotionScale = 1.0f;

    const quantum::Entity rootEntity = scene.createEntity();
    scene.addComponent<quantum::Transform>(rootEntity);
    animator.rootMotionTarget = rootEntity;

    const quantum::Entity animationEntity = scene.createEntity();
    scene.addComponent<quantum::AnimationComponent>(animationEntity);
    scene.addComponent<quantum::ActionComponent>(animationEntity);
    auto& animation = scene.getComponent<quantum::AnimationComponent>(animationEntity);
    animation.duration = 1.0f;
    animation.loop = true;

    const quantum::Entity trackEntity = scene.createEntity();
    scene.addComponent<quantum::ActionComponent>(trackEntity);
    scene.addComponent<quantum::KeyframeTracksComponent>(trackEntity);
    scene.addComponent<quantum::TranslateTracksComponent>(trackEntity);
    auto& trackAction = scene.getComponent<quantum::ActionComponent>(trackEntity);
    trackAction.target = rootEntity;
    auto& keyframes = scene.getComponent<quantum::KeyframeTracksComponent>(trackEntity);
    keyframes.times = {0.0f, 1.0f};
    keyframes.loop = true;
    auto& translations = scene.getComponent<quantum::TranslateTracksComponent>(trackEntity);
    translations.values = {quantum::Vector3::ZERO, quantum::Vector3(2.0f, 0.0f, 0.0f)};
    animation.actions.push_back({0.0f, 1.0f, trackEntity, 0});

    quantum::AnimatorState state;
    state.id = 1;
    state.name = "RootMotion";
    state.isDefault = true;
    quantum::AnimatorClip clip;
    clip.animation = animationEntity;
    clip.loop = true;
    state.clips.push_back(clip);
    animator.states.push_back(state);

    auto actionSystem = scene.getSystem<quantum::ActionSystem>();
    for (int frame = 0; frame < 6; ++frame) {
        actionSystem->update(0.1);
    }

    const float controllerX = scene.getComponent<quantum::Transform>(controllerEntity).position.x;
    const float rootX = scene.getComponent<quantum::Transform>(rootEntity).position.x;
    return animator.isPlaying && controllerX > 0.5f && controllerX < 1.5f && std::abs(rootX) < 0.001f;
}

bool runAnimatorLayerSmoke() {
    quantum::Scene scene;
    const quantum::Entity controllerEntity = scene.createEntity();
    scene.addComponent<quantum::AnimatorControllerComponent>(controllerEntity);
    auto& animator = scene.getComponent<quantum::AnimatorControllerComponent>(controllerEntity);
    animator.layers = {{"Base", 1.0f, false, true}, {"Upper", 0.5f, false, true}};

    const quantum::Entity target = scene.createEntity();
    scene.addComponent<quantum::Transform>(target);
    scene.addComponent<quantum::MeshComponent>(target);

    quantum::AnimatorState state;
    state.id = 1;
    state.name = "Layered";
    state.isDefault = true;
    for (const float value : {2.0f, 6.0f}){
        const quantum::Entity animationEntity = scene.createEntity();
        scene.addComponent<quantum::AnimationComponent>(animationEntity);
        scene.addComponent<quantum::ActionComponent>(animationEntity);
        auto& animation = scene.getComponent<quantum::AnimationComponent>(animationEntity);
        animation.duration = 1.0f;
        animation.loop = true;

        const quantum::Entity trackEntity = scene.createEntity();
        scene.addComponent<quantum::ActionComponent>(trackEntity);
        scene.addComponent<quantum::KeyframeTracksComponent>(trackEntity);
        scene.addComponent<quantum::TranslateTracksComponent>(trackEntity);
        scene.addComponent<quantum::MorphTracksComponent>(trackEntity);
        auto& trackAction = scene.getComponent<quantum::ActionComponent>(trackEntity);
        trackAction.target = target;
        auto& keyframes = scene.getComponent<quantum::KeyframeTracksComponent>(trackEntity);
        keyframes.times = {0.0f, 1.0f};
        keyframes.loop = true;
        scene.getComponent<quantum::TranslateTracksComponent>(trackEntity).values = {
            quantum::Vector3(value, 0.0f, 0.0f), quantum::Vector3(value, 0.0f, 0.0f)};
        scene.getComponent<quantum::MorphTracksComponent>(trackEntity).values = {{value / 10.0f}, {value / 10.0f}};
        animation.actions.push_back({0.0f, 1.0f, trackEntity, 0});

        quantum::AnimatorClip clip;
        clip.animation = animationEntity;
        clip.loop = true;
        clip.layer = value > 2.0f ? 1 : 0;
        state.clips.push_back(clip);
    }
    animator.states.push_back(state);

    scene.getSystem<quantum::ActionSystem>()->update(0.016);
    const float x = scene.getComponent<quantum::Transform>(target).position.x;
    const float morph = scene.getComponent<quantum::MeshComponent>(target).morphWeights[0];
    return std::abs(x - 4.0f) < 0.001f && std::abs(morph - 0.4f) < 0.001f;
}

bool runAnimatorIKSmoke() {
    quantum::Scene scene;
    const quantum::Entity controller = scene.createEntity();
    scene.addComponent<quantum::AnimatorControllerComponent>(controller);
    auto& animator = scene.getComponent<quantum::AnimatorControllerComponent>(controller);

    const quantum::Entity root = scene.createEntity();
    const quantum::Entity middle = scene.createEntity();
    const quantum::Entity tip = scene.createEntity();
    const quantum::Entity target = scene.createEntity();
    const quantum::Entity pole = scene.createEntity();
    for (const quantum::Entity entity : {root, middle, tip, target, pole}){
        scene.addComponent<quantum::Transform>(entity);
    }
    scene.getComponent<quantum::Transform>(root).position = quantum::Vector3(0, 0, 0);
    scene.getComponent<quantum::Transform>(middle).position = quantum::Vector3(1, 0, 0);
    scene.getComponent<quantum::Transform>(tip).position = quantum::Vector3(2, 0, 0);
    scene.getComponent<quantum::Transform>(target).position = quantum::Vector3(1, 1, 0);
    scene.getComponent<quantum::Transform>(pole).position = quantum::Vector3(0, 1, 0);
    animator.ikChains.push_back({root, middle, tip, target, pole, 1.0f, true, true});

    scene.getSystem<quantum::ActionSystem>()->update(0.016);
    const quantum::Vector3 solvedMiddle = scene.getComponent<quantum::Transform>(middle).position;
    const quantum::Vector3 solvedTip = scene.getComponent<quantum::Transform>(tip).position;
    const quantum::Quaternion rootRotation = scene.getComponent<quantum::Transform>(root).rotation;
    const quantum::Quaternion middleRotation = scene.getComponent<quantum::Transform>(middle).rotation;
    const quantum::Vector3 solvedRootDirection = (solvedMiddle - scene.getComponent<quantum::Transform>(root).position).normalized();
    const quantum::Vector3 solvedMiddleDirection = (solvedTip - solvedMiddle).normalized();
    const float upperLength = (solvedMiddle - scene.getComponent<quantum::Transform>(root).position).length();
    const float lowerLength = (solvedTip - solvedMiddle).length();
    return std::abs(solvedTip.x - 1.0f) < 0.001f && std::abs(solvedTip.y - 1.0f) < 0.001f &&
        std::abs(upperLength - 1.0f) < 0.001f && std::abs(lowerLength - 1.0f) < 0.001f &&
        solvedMiddle.y > 0.0f &&
        (rootRotation * quantum::Vector3::UNIT_Z).dotProduct(solvedRootDirection) > 0.999f &&
        (middleRotation * quantum::Vector3::UNIT_Z).dotProduct(solvedMiddleDirection) > 0.999f;
}

bool runVoxelRenderBridgeSmoke() {
    Chunk solidChunk;
    for (int x = 0; x < Chunk::SIZE; ++x) {
        for (int y = 0; y < Chunk::SIZE; ++y) {
            for (int z = 0; z < Chunk::SIZE; ++z) solidChunk.set(x, y, z, 1);
        }
    }
    solidChunk.rebuildLighting();
    VoxelMesher::Neighbors noNeighbors{};
    VoxelMeshData fineMesh;
    VoxelMeshData coarseMesh;
    VoxelMesher::buildLOD(solidChunk, noNeighbors, 0, fineMesh);
    VoxelMesher::buildLOD(solidChunk, noNeighbors, 1, coarseMesh);
    if (fineMesh.vertices.empty() || fineMesh.indices.empty()
        || coarseMesh.vertices.empty() || coarseMesh.indices.empty()
        || coarseMesh.transitionSkirtFaces == 0
        || coarseMesh.transitionSkirtFaces >= coarseMesh.faceCount()
        || coarseMesh.transitionSkirtFaces * 6 > coarseMesh.indices.size()) {
        std::cerr << "network-smoke: voxel LOD transition geometry failed fineFaces="
            << fineMesh.faceCount() << " coarseFaces=" << coarseMesh.faceCount()
            << " skirts=" << coarseMesh.transitionSkirtFaces << "\n";
        return false;
    }

    VoxelMeshData voxel;
    voxel.vertices = {
        {0.0f, 1.0f, 0.0f, 2, 15},
        {0.0f, 1.0f, 1.0f, 2, 15},
        {1.0f, 1.0f, 1.0f, 2, 15},
        {1.0f, 1.0f, 0.0f, 2, 15},
    };
    voxel.indices = {0, 1, 2, 0, 2, 3};

    quantum::Scene scene;
    quantum::MeshSystem meshSystem(&scene);
    quantum::MeshComponent mesh;
    if (!meshSystem.createVoxelMesh(mesh, voxel, 4, 4)) {
        std::cerr << "network-smoke: voxel bridge create failed\n";
        return false;
    }
    if (mesh.vertexCount != 4 || mesh.buffer.getCount() != 4 || mesh.indices.getCount() != 6
        || mesh.numSubmeshes != 1 || mesh.verticesAABB.isNull()) {
        std::cerr << "network-smoke: voxel bridge buffers failed vertices=" << mesh.vertexCount
            << " buffer=" << mesh.buffer.getCount() << " indices=" << mesh.indices.getCount()
            << " submeshes=" << mesh.numSubmeshes << "\n";
        return false;
    }

    const quantum::Vector3 normal = mesh.buffer.getVector3(quantum::AttributeType::NORMAL, 0);
    const quantum::Vector4 color = mesh.buffer.getVector4(quantum::AttributeType::COLOR, 0);
    if (normal.y <= 0.99f || color.x <= 0.99f || color.w <= 0.99f) {
        std::cerr << "network-smoke: voxel bridge attributes normalY=" << normal.y
            << " color=" << color.x << "," << color.w << "\n";
        return false;
    }

    World world(0xC0FFEEu);
    world.update(0, 0, 0);
    quantum::MeshSystem worldMeshSystem(&scene);
    const size_t initialCount = worldMeshSystem.updateVoxelWorld(world, 0, 0, 0, 1, 4, 4);
    if (initialCount != 1) {
        std::cerr << "network-smoke: voxel initial count=" << initialCount << "\n";
        return false;
    }
    auto* meshes = scene.getComponentArray<quantum::MeshComponent>();
    if (!meshes || meshes->size() == 0) {
        std::cerr << "network-smoke: voxel initial mesh array empty\n";
        return false;
    }
    const quantum::Entity chunkEntity = meshes->getEntity(meshes->size() - 1);
    const quantum::Transform* chunkTransform = scene.findComponent<quantum::Transform>(chunkEntity);
    if (!chunkTransform || scene.getEntityName(chunkEntity) != "VoxelChunk_0_0") {
        std::cerr << "network-smoke: voxel initial entity name='"
            << scene.getEntityName(chunkEntity) << "'\n";
        return false;
    }
    if (world.meshDirtyChunkCount() != 0) {
        std::cerr << "network-smoke: voxel initial mesh dirtiness was not consumed\n";
        return false;
    }
    const uint8_t editedBlock = world.getBlock(0, 0, 0) == 7 ? 6 : 7;
    if (!world.setBlock(0, 0, 0, editedBlock) || world.getBlock(0, 0, 0) != editedBlock
        || world.dirtyChunkCount() == 0 || world.meshDirtyChunkCount() == 0) {
        std::cerr << "network-smoke: voxel edit was not recorded\n";
        return false;
    }
    worldMeshSystem.updateVoxelWorld(world, 0, 0, 0, 1, 4, 4);
    if (world.dirtyChunkCount() == 0 || world.meshDirtyChunkCount() != 0) {
        std::cerr << "network-smoke: voxel edit lost dirty state\n";
        return false;
    }

    const std::filesystem::path regionPath =
        std::filesystem::current_path() / "voxel-completion-smoke.qvx";
    std::filesystem::remove(regionPath);
    if (!world.saveRegion(regionPath.string())) {
        std::cerr << "network-smoke: voxel region save failed\n";
        return false;
    }
    World restoredWorld(0xC0FFEEu);
    if (!restoredWorld.loadRegion(regionPath.string())
        || restoredWorld.getBlock(0, 0, 0) != editedBlock) {
        std::cerr << "network-smoke: voxel region round-trip failed\n";
        std::filesystem::remove(regionPath);
        return false;
    }
    std::ifstream regionInput(regionPath, std::ios::binary);
    std::vector<uint8_t> regionBytes((std::istreambuf_iterator<char>(regionInput)),
        std::istreambuf_iterator<char>());
    regionInput.close();
    if (regionBytes.size() < 4) {
        std::filesystem::remove(regionPath);
        return false;
    }
    regionBytes.back() ^= 0x01;
    std::ofstream corruptedRegion(regionPath, std::ios::binary | std::ios::trunc);
    corruptedRegion.write(reinterpret_cast<const char*>(regionBytes.data()),
        static_cast<std::streamsize>(regionBytes.size()));
    corruptedRegion.close();
    World rejectedWorld(0xC0FFEEu);
    const bool rejectedCorruption = !rejectedWorld.loadRegion(regionPath.string());
    std::filesystem::remove(regionPath);
    if (!rejectedCorruption) {
        std::cerr << "network-smoke: corrupted voxel region was accepted\n";
        return false;
    }
    world.update(4, 4, 0);
    worldMeshSystem.updateVoxelWorld(world, 4, 4, 0, 1, 4, 4);
    if (scene.findEntity("VoxelChunk_0_0") != NULL_ENTITY
        || scene.findEntity("VoxelChunk_4_4") == NULL_ENTITY) {
        std::cerr << "network-smoke: voxel moved old=" << scene.findEntity("VoxelChunk_0_0")
            << " new=" << scene.findEntity("VoxelChunk_4_4") << "\n";
        return false;
    }
    worldMeshSystem.destroyVoxelWorld();

    World lodWorld(0xBADC0DEu);
    lodWorld.update(0, 0, 2);
    quantum::MeshSystem lodSystem(&scene);
    const size_t lodCount = lodSystem.updateVoxelWorld(lodWorld, 0, 0, 2, 25, 4, 4);
    if (lodCount != 25 || lodSystem.getVoxelChunkLOD(0, 0) != 0
        || lodSystem.getVoxelChunkLOD(2, 2) != 2){
        std::cerr << "network-smoke: voxel LOD values count=" << lodCount
            << " center=" << lodSystem.getVoxelChunkLOD(0, 0)
            << " far=" << lodSystem.getVoxelChunkLOD(2, 2) << "\n";
        return false;
    }
    lodSystem.destroyVoxelWorld();

    const quantum::Entity camera = scene.createEntity();
    scene.addComponent<quantum::CameraComponent>(camera);
    scene.addComponent<quantum::Transform>(camera);
    scene.setCamera(camera);
    quantum::Transform* cameraTransform = scene.findComponent<quantum::Transform>(
        scene.getRenderCamera());
    if (!cameraTransform) {
        std::cerr << "network-smoke: voxel camera transform missing camera="
            << scene.getRenderCamera() << "\n";
        return false;
    }
    cameraTransform->worldPosition = quantum::Vector3(34.0f, 12.0f, -18.0f);
    World cameraWorld(0x12345678u);
    cameraWorld.update(2, -2, 0);
    const size_t cameraCount = lodSystem.updateVoxelWorldFromCamera(cameraWorld, 0, 1, 4, 4);
    if (cameraCount != 1 || lodSystem.getVoxelChunkLOD(2, -2) != 0){
        std::cerr << "network-smoke: voxel camera values count=" << cameraCount
            << " lod=" << lodSystem.getVoxelChunkLOD(2, -2) << "\n";
        return false;
    }
    lodSystem.destroyVoxelWorld();
    return true;
}

bool runEntityPoolSmoke() {
    quantum::Scene scene;
    const quantum::Entity first = scene.createEntity();
    const uint32_t firstGeneration = scene.getEntityGeneration(first);
    scene.setEntityName(first, "pooled");
    scene.destroyEntity(first);

    if (scene.isEntityCreated(first) || scene.getEntityCount() != 0
            || !scene.getEntityList().empty() || scene.getEntityFreeListSize() != 1
            || scene.getEntityGeneration(first) != firstGeneration + 1) {
        return false;
    }

    const quantum::Entity recycled = scene.createEntity();
    if (recycled != first || !scene.isEntityCreated(recycled)
            || scene.getEntityCount() != 1 || scene.getEntityName(recycled) != "") {
        return false;
    }

    scene.destroyEntity(recycled);
    if (!scene.recreateEntity(recycled) || scene.getEntityFreeListSize() != 0
            || scene.createEntity() == recycled) {
        return false;
    }
    return scene.getEntityCount() == 2;
}

bool runSceneReloadSmoke() {
    constexpr uint32_t sceneId = 91001;
    int factoryRuns = 0;

    quantum::SceneManager::clearAll();
    quantum::Engine::removeAllScenes();
    quantum::SceneManager::registerScene(sceneId, "SceneReloadSmoke", [&]() {
        ++factoryRuns;
    });

    bool passed = quantum::SceneManager::loadScene(sceneId)
        && factoryRuns == 1
        && quantum::SceneManager::getCurrentSceneId() == sceneId;
    passed = passed && quantum::SceneManager::reloadCurrentScene()
        && factoryRuns == 2
        && quantum::SceneManager::getCurrentSceneId() == sceneId;

    quantum::Engine::removeAllScenes();
    quantum::SceneManager::clearAll();
    return passed;
}

void closeSocket(Socket socket) {
#if defined(_WIN32)
    closesocket(socket);
#else
    close(socket);
#endif
}

bool setNonBlocking(Socket socket) {
#if defined(_WIN32)
    u_long enabled = 1;
    return ioctlsocket(socket, FIONBIO, &enabled) == 0;
#else
    const int flags = fcntl(socket, F_GETFL, 0);
    return flags >= 0 && fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

bool wouldBlock() {
#if defined(_WIN32)
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

int runProcessMode(bool serverMode, uint16_t port) {
    Socket socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket == INVALID_SOCKET_VALUE || !setNonBlocking(socket)) {
        std::cerr << "network-smoke: process socket setup failed\n";
        if (socket != INVALID_SOCKET_VALUE) closeSocket(socket);
        return 1;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (serverMode && ::bind(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        std::cerr << "network-smoke: process server bind failed\n";
        closeSocket(socket);
        return 1;
    }

    const uint32_t nonce = serverMode ? 0 : 0x13572468u;
    if (!serverMode) {
        const auto hello = quantum::network::encodeHello(quantum::network::PacketType::Hello, nonce, 1);
        ::sendto(socket, reinterpret_cast<const char*>(hello.data()), static_cast<int>(hello.size()), 0,
            reinterpret_cast<const sockaddr*>(&address), sizeof(address));
    }

    uint8_t buffer[1500];
    for (int attempt = 0; attempt < 500; ++attempt) {
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
            quantum::network::PacketHeader header;
            quantum::network::HelloPayload payload;
            if (quantum::network::decodeHello(buffer, static_cast<size_t>(received), header, payload)) {
                if (serverMode && header.type == quantum::network::PacketType::Hello) {
                    const auto ack = quantum::network::encodeHello(quantum::network::PacketType::HelloAck,
                        payload.nonce, 2);
                    ::sendto(socket, reinterpret_cast<const char*>(ack.data()), static_cast<int>(ack.size()), 0,
                        reinterpret_cast<const sockaddr*>(&source), sizeof(source));
                    closeSocket(socket);
                    std::cout << "network-smoke: process server accepted handshake\n";
                    return 0;
                }
                if (!serverMode && header.type == quantum::network::PacketType::HelloAck
                        && payload.nonce == nonce) {
                    closeSocket(socket);
                    std::cout << "network-smoke: process client completed handshake\n";
                    return 0;
                }
            }
        } else if (received < 0 && !wouldBlock()) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    closeSocket(socket);
    std::cerr << "network-smoke: process handshake timed out\n";
    return 1;
}

bool runEngineNetworkIntegration() {
    quantum::Scene serverScene;
    quantum::Scene clientScene;
    const quantum::Entity serverEntity = serverScene.createEntity();
    const quantum::Entity clientEntity = clientScene.createEntity();
    serverScene.addComponent<quantum::Transform>(serverEntity);
    serverScene.addComponent<quantum::NetworkComponent>(serverEntity);
    clientScene.addComponent<quantum::Transform>(clientEntity);
    clientScene.addComponent<quantum::NetworkComponent>(clientEntity);
    serverScene.getComponent<quantum::Transform>(serverEntity).position = quantum::Vector3(4.5f, 2.0f, -1.0f);
    clientScene.getComponent<quantum::Transform>(clientEntity).position = quantum::Vector3::ZERO;

    const auto server = serverScene.getSystem<quantum::NetworkSystem>();
    const auto client = clientScene.getSystem<quantum::NetworkSystem>();
    const std::string authToken = "network-smoke-shared-token";
    server->setAuthToken(authToken);
    client->setAuthToken(authToken);
    server->registerEntity(serverEntity);
    client->registerEntity(clientEntity);
    server->startServer(39179);
    client->startClient("127.0.0.1", 39179);

    bool connected = false;
    for (int attempt = 0; attempt < 120; ++attempt) {
        client->update(0.005);
        connected = connected || client->isConnected();
        if (connected) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (!connected) {
        std::cerr << "network-smoke: authenticated handshake did not complete\n";
        return false;
    }
    const uint32_t assignedPeerId = client->getLocalPeerId();
    if (assignedPeerId == 0 || !server->isServer() || !client->isClient()) {
        std::cerr << "network-smoke: session identity was not assigned\n";
        return false;
    }

    // An unsigned hello must not be able to create or refresh an authenticated peer.
    Socket attacker = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (attacker != INVALID_SOCKET_VALUE) {
        sockaddr_in target{};
        target.sin_family = AF_INET;
        target.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        target.sin_port = htons(39179);
        const auto unsignedHello = quantum::network::encodeHello(
            quantum::network::PacketType::Hello, 0xabcdef01u, 1);
        ::sendto(attacker, reinterpret_cast<const char*>(unsignedHello.data()),
            static_cast<int>(unsignedHello.size()), 0,
            reinterpret_cast<const sockaddr*>(&target), sizeof(target));
        closeSocket(attacker);
    }
    for (int attempt = 0; attempt < 20; ++attempt) {
        client->update(0.005);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    const std::vector<uint8_t> expected = {10, 20, 30, 40, 50};
    const std::vector<uint8_t> expectedUnreliable = {90, 80, 70};
    if (server->sendReliable(expected) == 0) return false;
    if (server->sendUnreliable(expectedUnreliable) == 0) return false;

    std::vector<uint8_t> received;
    std::vector<uint8_t> receivedUnreliable;
    bool initialSnapshotApplied = false;
    bool movedSnapshotApplied = false;
    for (int attempt = 0; attempt < 240; ++attempt) {
        if (attempt == 80) {
            serverScene.getComponent<quantum::Transform>(serverEntity).position.x = 9.0f;
        }
        client->update(0.005);
        client->pollReliable(received);
        client->pollUnreliable(receivedUnreliable);
        const float clientX = clientScene.getComponent<quantum::Transform>(clientEntity).position.x;
        initialSnapshotApplied = initialSnapshotApplied || clientX == 4.5f;
        movedSnapshotApplied = movedSnapshotApplied || (attempt > 100 && clientX > 8.5f);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    quantum::network::TransformSnapshot historical;
    if (!server->getHistoricalTransform(1, 0.0, historical)
            || historical.netId != 1 || historical.position[0] < 8.9f) {
        std::cerr << "network-smoke: historical transform query failed\n";
        return false;
    }
    const float finalClientX = clientScene.getComponent<quantum::Transform>(clientEntity).position.x;
    if (received != expected || receivedUnreliable != expectedUnreliable
        || !initialSnapshotApplied || !movedSnapshotApplied || finalClientX <= 8.5f) {
        std::cerr << "network-smoke: snapshot state initial=" << initialSnapshotApplied
            << " moved=" << movedSnapshotApplied << " finalX=" << finalClientX << "\n";
        return false;
    }
    if (received != expected || !initialSnapshotApplied || !movedSnapshotApplied
        || finalClientX <= 8.5f
        || clientScene.getComponent<quantum::Transform>(clientEntity).position.y != 2.0f) {
        return false;
    }

    //Stop and restart the server to prove that the client detects a dead peer,
    //re-handshakes and flushes queued reliable data after reconnection.
    server->destroy();
    bool timedOut = false;
    for (int attempt = 0; attempt < 220; ++attempt) {
        client->update(0.005);
        timedOut = timedOut || !client->isConnected();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (!timedOut) {
        std::cerr << "network-smoke: client did not time out after server shutdown\n";
        return false;
    }

    server->startServer(39179);
    const std::vector<uint8_t> afterReconnect = {7, 8, 9, 10};
    client->setPacketLossSimulationForTesting(100);
    if (client->sendReliable(afterReconnect) == 0) return false;
    std::vector<uint8_t> receivedAfterReconnect;
    bool reconnected = false;
    for (int attempt = 0; attempt < 260; ++attempt) {
        client->update(0.005);
        reconnected = reconnected || client->isConnected();
        server->pollReliable(receivedAfterReconnect);
        if (attempt == 50) client->setPacketLossSimulationForTesting(0);
        if (receivedAfterReconnect == afterReconnect) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (!reconnected || receivedAfterReconnect != afterReconnect) {
        std::cerr << "network-smoke: reconnect reliable delivery failed\n";
        return false;
    }
    if (client->getLocalPeerId() == 0) {
        std::cerr << "network-smoke: session identity was not restored after reconnect\n";
        return false;
    }
    // Let the authenticated ACK for the final retransmission reach the client
    // before taking the metrics snapshot.
    for (int attempt = 0; attempt < 40; ++attempt) {
        client->update(0.005);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    const auto serverMetrics = server->getMetrics();
    const auto clientMetrics = client->getMetrics();
    if (serverMetrics.reliableSent == 0 || serverMetrics.snapshotsSent == 0
            || clientMetrics.reliableReceived == 0 || clientMetrics.snapshotsReceived == 0
            || clientMetrics.reliableRttMs <= 0.0
            || clientMetrics.reliableRetransmitTimeoutMs < 50.0
            || clientMetrics.reliableRetransmitTimeoutMs > 500.0
            || clientMetrics.reliableWindow == 0 || clientMetrics.reliableWindow > 64
            || clientMetrics.simulatedPacketDrops == 0
            || clientMetrics.reliableRetransmitted == 0) {
        std::cerr << "network-smoke: network metrics incomplete serverReliable="
            << serverMetrics.reliableSent << " serverSnapshots=" << serverMetrics.snapshotsSent
            << " clientReliable=" << clientMetrics.reliableReceived
            << " clientSnapshots=" << clientMetrics.snapshotsReceived
            << " rtt=" << clientMetrics.reliableRttMs
            << " timeout=" << clientMetrics.reliableRetransmitTimeoutMs
            << " window=" << clientMetrics.reliableWindow
            << " drops=" << clientMetrics.simulatedPacketDrops
            << " retransmits=" << clientMetrics.reliableRetransmitted << "\n";
        return false;
    }
    if (serverMetrics.unauthorizedPackets == 0) {
        std::cerr << "network-smoke: unsigned datagram was not rejected\n";
        return false;
    }

    return received == expected
        && initialSnapshotApplied && movedSnapshotApplied
        && finalClientX > 8.5f
        && clientScene.getComponent<quantum::Transform>(clientEntity).position.y == 2.0f;
}

int runNetworkInstance(bool serverMode, const std::string& address, uint16_t port,
    int durationMs, uint32_t lossPercent, uint32_t reorderPercent) {
    quantum::Scene scene;
    const quantum::Entity entity = scene.createEntity();
    scene.addComponent<quantum::Transform>(entity);
    scene.addComponent<quantum::NetworkComponent>(entity);
    auto& transform = scene.getComponent<quantum::Transform>(entity);
    transform.position = quantum::Vector3(4.5f, 2.0f, -1.0f);
    auto& networkComponent = scene.getComponent<quantum::NetworkComponent>(entity);
    networkComponent.replicateTransform = true;
    networkComponent.serverAuthority = true;

    const auto network = scene.getSystem<quantum::NetworkSystem>();
    if (serverMode) {
        network->registerEntity(entity);
        network->startServer(port);
    } else {
        network->setInterestCenter(quantum::Vector3::ZERO, 100.0f);
        network->setPacketLossSimulationForTesting(lossPercent);
        network->setPacketReorderingSimulationForTesting(reorderPercent);
        network->startClient(address, port);
    }

    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(std::clamp(durationMs, 500, 30000));
    bool connected = false;
    while (std::chrono::steady_clock::now() < deadline) {
        network->update(0.01);
        connected = connected || network->isConnected();
        if (serverMode && connected) transform.position.x += 0.01f;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    const auto metrics = network->getMetrics();
    std::cout << "network-instance: role=" << (serverMode ? "server" : "client")
        << " connected=" << (connected ? 1 : 0)
        << " packets=" << metrics.packetsSent << "/" << metrics.packetsReceived
        << " snapshots=" << metrics.snapshotsSent << "/" << metrics.snapshotsReceived
        << " rttMs=" << metrics.reliableRttMs
        << " lossPercent=" << metrics.packetLossPercent
        << " queue=" << metrics.reliableQueueDepth << "/" << metrics.unreliableQueueDepth
        << " rejected=" << metrics.unauthorizedPackets + metrics.rateLimitedPackets << "\n";
    network->destroy();
    if (serverMode) return connected && metrics.snapshotsSent > 0 ? 0 : 1;
    return connected && metrics.snapshotsReceived > 0 ? 0 : 1;
}

int runReconnectSoak(int durationMs, uint32_t requestedCycles,
    uint32_t lossPercent, uint32_t reorderPercent) {
    durationMs = std::clamp(durationMs, 6000, 120000);
    const uint32_t maxCyclesForDuration = static_cast<uint32_t>(
        std::max(1, durationMs / 3000));
    const uint32_t cycleCount = std::clamp(requestedCycles, 1u,
        std::min(8u, maxCyclesForDuration));
    lossPercent = std::min<uint32_t>(100, lossPercent);
    reorderPercent = std::min<uint32_t>(100, reorderPercent);

    quantum::Scene serverScene;
    quantum::Scene clientScene;
    const quantum::Entity serverEntity = serverScene.createEntity();
    const quantum::Entity clientEntity = clientScene.createEntity();
    serverScene.addComponent<quantum::Transform>(serverEntity);
    serverScene.addComponent<quantum::NetworkComponent>(serverEntity);
    clientScene.addComponent<quantum::Transform>(clientEntity);
    clientScene.addComponent<quantum::NetworkComponent>(clientEntity);
    serverScene.getComponent<quantum::Transform>(serverEntity).position =
        quantum::Vector3(2.0f, 1.0f, -3.0f);
    clientScene.getComponent<quantum::Transform>(clientEntity).position =
        quantum::Vector3::ZERO;
    serverScene.getComponent<quantum::NetworkComponent>(serverEntity).replicateTransform = true;
    clientScene.getComponent<quantum::NetworkComponent>(clientEntity).replicateTransform = true;

    const auto server = serverScene.getSystem<quantum::NetworkSystem>();
    const auto client = clientScene.getSystem<quantum::NetworkSystem>();
    const std::string authToken = "network-smoke-reconnect-soak-token";
    constexpr uint16_t port = 39281;
    server->setAuthToken(authToken);
    client->setAuthToken(authToken);
    server->setPacketLossSimulationForTesting(lossPercent);
    server->setPacketReorderingSimulationForTesting(reorderPercent);
    client->setPacketLossSimulationForTesting(lossPercent);
    client->setPacketReorderingSimulationForTesting(reorderPercent);
    server->registerEntity(serverEntity);
    client->registerEntity(clientEntity);
    client->setInterestCenter(quantum::Vector3::ZERO, 100.0f);
    server->startServer(port);
    client->startClient("127.0.0.1", port);

    uint32_t connectedEdges = 0;
    uint32_t disconnectEdges = 0;
    uint32_t clientMessagesSent = 0;
    uint32_t serverMessagesSent = 0;
    uint32_t clientMessagesReceived = 0;
    uint32_t serverMessagesReceived = 0;
    uint32_t queuedDuringDisconnect = 0;
    bool previousConnected = false;
    uint32_t messageOrdinal = 0;
    auto makePayload = [&messageOrdinal](uint8_t direction, uint32_t cycle) {
        const uint32_t ordinal = messageOrdinal++;
        return std::vector<uint8_t>{0x4e, 0x53, direction,
            static_cast<uint8_t>(cycle & 0xffu),
            static_cast<uint8_t>(ordinal & 0xffu),
            static_cast<uint8_t>((ordinal >> 8) & 0xffu),
            static_cast<uint8_t>((ordinal >> 16) & 0xffu),
            static_cast<uint8_t>((ordinal >> 24) & 0xffu)};
    };
    auto isSoakPayload = [](const std::vector<uint8_t>& payload, uint8_t direction) {
        return payload.size() == 8 && payload[0] == 0x4e && payload[1] == 0x53
            && payload[2] == direction;
    };
    auto pump = [&](int milliseconds, bool serverActive, uint32_t cycle) {
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(milliseconds);
        auto nextMessage = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() < deadline) {
            client->update(0.01);
            if (serverActive) {
                server->update(0.01);
                serverScene.getComponent<quantum::Transform>(serverEntity).position.x += 0.0025f;
            }

            const bool connected = client->isConnected();
            if (connected && !previousConnected) ++connectedEdges;
            if (!connected && previousConnected) ++disconnectEdges;
            previousConnected = connected;

            const auto now = std::chrono::steady_clock::now();
            if (connected && now >= nextMessage) {
                const auto clientPayload = makePayload(0x01, cycle);
                const auto serverPayload = makePayload(0x02, cycle);
                if (client->sendReliable(clientPayload) != 0) ++clientMessagesSent;
                if (server->sendReliable(serverPayload) != 0) ++serverMessagesSent;
                nextMessage = now + std::chrono::milliseconds(70);
            }

            std::vector<uint8_t> payload;
            while (client->pollReliable(payload)) {
                if (isSoakPayload(payload, 0x02)) ++clientMessagesReceived;
            }
            while (serverActive && server->pollReliable(payload)) {
                if (isSoakPayload(payload, 0x01)) ++serverMessagesReceived;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    };

    // Leave enough wall-clock margin for the client timeout (750 ms), the
    // asynchronous server thread startup and one complete Hello/HelloAck turn.
    // Without this floor the final restart can be accepted by the server just
    // after the pump ends, making the soak report a false negative under load.
    const int activeMilliseconds = std::max(2200,
        durationMs / static_cast<int>(cycleCount * 2));
    const int downtimeMilliseconds = std::clamp(
        durationMs / static_cast<int>(cycleCount * 8), 1000, 1400);
    for (uint32_t cycle = 0; cycle < cycleCount; ++cycle) {
        pump(activeMilliseconds, true, cycle);
        if (!client->isConnected()) {
            std::cerr << "network-soak: failed to establish cycle " << cycle << " before restart\n";
            server->destroy();
            client->destroy();
            return 1;
        }

        server->destroy();
        const auto queuedProbe = makePayload(0x03, cycle);
        if (client->sendReliable(queuedProbe) != 0) ++queuedDuringDisconnect;
        pump(downtimeMilliseconds, false, cycle);
        server->startServer(port);
        pump(activeMilliseconds, true, cycle);
    }

    const auto settleDeadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(500);
    while (std::chrono::steady_clock::now() < settleDeadline) {
        client->update(0.01);
        server->update(0.01);
        std::vector<uint8_t> payload;
        while (client->pollReliable(payload)) {
            if (isSoakPayload(payload, 0x02)) ++clientMessagesReceived;
        }
        while (server->pollReliable(payload)) {
            if (isSoakPayload(payload, 0x01) || isSoakPayload(payload, 0x03)) {
                ++serverMessagesReceived;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    const auto serverMetrics = server->getMetrics();
    const auto clientMetrics = client->getMetrics();
    const uint64_t retransmissions = serverMetrics.reliableRetransmitted
        + clientMetrics.reliableRetransmitted;
    const bool lossObserved = lossPercent == 0
        || serverMetrics.simulatedPacketDrops + clientMetrics.simulatedPacketDrops > 0;
    const bool reorderObserved = reorderPercent == 0
        || serverMetrics.simulatedPacketReorders + clientMetrics.simulatedPacketReorders > 0;
    const bool passed = connectedEdges >= cycleCount + 1
        && disconnectEdges >= cycleCount
        && queuedDuringDisconnect == cycleCount
        && clientMessagesSent > 0 && serverMessagesSent > 0
        && clientMessagesReceived > 0 && serverMessagesReceived >= cycleCount
        && client->isConnected() && client->getLocalPeerId() != 0
        && lossObserved && reorderObserved
        && (lossPercent == 0 || retransmissions > 0);

    std::cout << "network-soak: durationMs=" << durationMs
        << " cycles=" << cycleCount
        << " connectedEdges=" << connectedEdges
        << " disconnectEdges=" << disconnectEdges
        << " queuedDuringDisconnect=" << queuedDuringDisconnect
        << " messages=" << clientMessagesSent << "/" << clientMessagesReceived
        << "," << serverMessagesSent << "/" << serverMessagesReceived
        << " retransmits=" << retransmissions
        << " drops=" << serverMetrics.simulatedPacketDrops + clientMetrics.simulatedPacketDrops
        << " reorders=" << serverMetrics.simulatedPacketReorders + clientMetrics.simulatedPacketReorders
        << " rttMs=" << clientMetrics.reliableRttMs
        << " result=" << (passed ? "PASS" : "FAIL") << "\n";
    if (!passed) {
        std::cerr << "network-soak: reconnect/loss/reordering soak criteria failed\n";
    }
    server->destroy();
    client->destroy();
    return passed ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::cerr << "network-smoke: WSAStartup failed\n";
        return 1;
    }
#endif

    if (argc >= 3 && (std::string(argv[1]) == "--instance-server"
            || std::string(argv[1]) == "--instance-client")) {
        const bool serverMode = std::string(argv[1]) == "--instance-server";
        const std::string address = serverMode ? "127.0.0.1" : argv[2];
        const int portIndex = serverMode ? 2 : 3;
        if (argc <= portIndex) {
            std::cerr << "network-smoke: instance mode needs a port\n";
#if defined(_WIN32)
            WSACleanup();
#endif
            return 2;
        }
        const uint16_t port = static_cast<uint16_t>(std::stoul(argv[portIndex]));
        const int durationIndex = portIndex + 1;
        const int durationMs = argc > durationIndex ? std::stoi(argv[durationIndex]) : 5000;
        const int lossIndex = durationIndex + 1;
        const int reorderIndex = durationIndex + 2;
        const uint32_t loss = argc > lossIndex ? static_cast<uint32_t>(std::stoul(argv[lossIndex])) : 0;
        const uint32_t reorder = argc > reorderIndex ? static_cast<uint32_t>(std::stoul(argv[reorderIndex])) : 0;
        const int result = runNetworkInstance(serverMode, address, port, durationMs, loss, reorder);
#if defined(_WIN32)
        WSACleanup();
#endif
        return result;
    }

    if (argc >= 2 && std::string(argv[1]) == "--reconnect-soak") {
        const int durationMs = argc > 2 ? std::stoi(argv[2]) : 15000;
        const uint32_t cycles = argc > 3 ? static_cast<uint32_t>(std::stoul(argv[3])) : 3;
        const uint32_t loss = argc > 4 ? static_cast<uint32_t>(std::stoul(argv[4])) : 20;
        const uint32_t reorder = argc > 5 ? static_cast<uint32_t>(std::stoul(argv[5])) : 35;
        const int result = runReconnectSoak(durationMs, cycles, loss, reorder);
#if defined(_WIN32)
        WSACleanup();
#endif
        return result;
    }

    if (!runSaveGameSmoke()) {
        std::cerr << "network-smoke: SaveGame persistence failed\n";
#if defined(_WIN32)
        WSACleanup();
#endif
        return 1;
    }
    if (!runInputActionSmoke()) {
        std::cerr << "network-smoke: InputActionMap failed\n";
#if defined(_WIN32)
        WSACleanup();
#endif
        return 1;
    }
    if (!runAudioBusSmoke()) {
        std::cerr << "network-smoke: AudioSystem bus hierarchy failed\n";
#if defined(_WIN32)
        WSACleanup();
#endif
        return 1;
    }
    if (!runCollisionFilterSmoke()) {
        std::cerr << "network-smoke: collision filter mapping failed\n";
#if defined(_WIN32)
        WSACleanup();
#endif
        return 1;
    }
    if (!runPhysicsSimulationSmoke()) {
        std::cerr << "network-smoke: 2D physics simulation failed\n";
#if defined(_WIN32)
        WSACleanup();
#endif
        return 1;
    }
    if (!runParticleCollisionSmoke()) {
        std::cerr << "network-smoke: particle collision failed\n";
#if defined(_WIN32)
        WSACleanup();
#endif
        return 1;
    }
    if (!runJoint3DSmoke()) {
        std::cerr << "network-smoke: 3D joint simulation failed\n";
#if defined(_WIN32)
        WSACleanup();
#endif
        return 1;
    }
    if (!runShutdownStressSmoke()) {
        std::cerr << "network-smoke: shutdown stress failed\n";
#if defined(_WIN32)
        WSACleanup();
#endif
        return 1;
    }
    if (!runAnimatorStateMachineSmoke()) {
        std::cerr << "network-smoke: animator state machine failed\n";
#if defined(_WIN32)
        WSACleanup();
#endif
        return 1;
    }
    if (!runRootMotionSmoke()) {
        std::cerr << "network-smoke: root motion failed\n";
#if defined(_WIN32)
        WSACleanup();
#endif
        return 1;
    }
    if (!runAnimatorLayerSmoke()) {
        std::cerr << "network-smoke: animator layers failed\n";
#if defined(_WIN32)
        WSACleanup();
#endif
        return 1;
    }
    if (!runAnimatorIKSmoke()) {
        std::cerr << "network-smoke: animator IK failed\n";
#if defined(_WIN32)
        WSACleanup();
#endif
        return 1;
    }
    if (!runVoxelRenderBridgeSmoke()) {
        std::cerr << "network-smoke: voxel render bridge failed\n";
#if defined(_WIN32)
        WSACleanup();
#endif
        return 1;
    }
    if (!runEntityPoolSmoke()) {
        std::cerr << "network-smoke: entity pool failed\n";
#if defined(_WIN32)
        WSACleanup();
#endif
        return 1;
    }
    if (!runSceneReloadSmoke()) {
        std::cerr << "network-smoke: SceneManager reload failed\n";
#if defined(_WIN32)
        WSACleanup();
#endif
        return 1;
    }
    if (!runNetworkProtocolCompletionSmoke()) {
        std::cerr << "network-smoke: schema migration or compressed snapshot failed\n";
#if defined(_WIN32)
        WSACleanup();
#endif
        return 1;
    }
    if (!runEventBusSmoke()) {
        std::cerr << "network-smoke: global event bus failed\n";
#if defined(_WIN32)
        WSACleanup();
#endif
        return 1;
    }
    if (!runTextCompositionSmoke()) {
        std::cerr << "network-smoke: text composition event failed\n";
#if defined(_WIN32)
        WSACleanup();
#endif
        return 1;
    }
    if (!runPredictionSmoke()) {
        std::cerr << "network-smoke: prediction failed\n";
#if defined(_WIN32)
        WSACleanup();
#endif
        return 1;
    }
    if (!runEntityReplicationSmoke()) {
        std::cerr << "network-smoke: entity replication failed\n";
#if defined(_WIN32)
        WSACleanup();
#endif
        return 1;
    }
    if (!runMultiPeerReplicationSmoke()) {
        std::cerr << "network-smoke: multi-peer replication failed\n";
#if defined(_WIN32)
        WSACleanup();
#endif
        return 1;
    }
    if (!runLobbyDiscoverySmoke()) {
        std::cerr << "network-smoke: LAN lobby discovery failed\n";
#if defined(_WIN32)
        WSACleanup();
#endif
        return 1;
    }
    if (!runLobbyReservationSmoke()) {
        std::cerr << "network-smoke: LAN lobby reservation failed\n";
#if defined(_WIN32)
        WSACleanup();
#endif
        return 1;
    }
    if (!runAttachedLuaDebuggerSmoke()) {
        std::cerr << "network-smoke: attached Lua debugger failed\n";
#if defined(_WIN32)
        WSACleanup();
#endif
        return 1;
    }

    if (argc == 3 && (std::string(argv[1]) == "--server" || std::string(argv[1]) == "--client")) {
        const uint16_t port = static_cast<uint16_t>(std::stoul(argv[2]));
        const int result = runProcessMode(std::string(argv[1]) == "--server", port);
#if defined(_WIN32)
        WSACleanup();
#endif
        return result;
    }

    Socket server = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    Socket client = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (server == INVALID_SOCKET_VALUE || client == INVALID_SOCKET_VALUE
            || !setNonBlocking(server) || !setNonBlocking(client)) {
        std::cerr << "network-smoke: socket setup failed\n";
        if (server != INVALID_SOCKET_VALUE) closeSocket(server);
        if (client != INVALID_SOCKET_VALUE) closeSocket(client);
#if defined(_WIN32)
        WSACleanup();
#endif
        return 1;
    }

    sockaddr_in serverAddress{};
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    serverAddress.sin_port = 0;
    if (::bind(server, reinterpret_cast<const sockaddr*>(&serverAddress), sizeof(serverAddress)) != 0) {
        std::cerr << "network-smoke: server bind failed\n";
        closeSocket(server);
        closeSocket(client);
#if defined(_WIN32)
        WSACleanup();
#endif
        return 1;
    }

#if defined(_WIN32)
    int addressLength = sizeof(serverAddress);
#else
    socklen_t addressLength = sizeof(serverAddress);
#endif
    if (::getsockname(server, reinterpret_cast<sockaddr*>(&serverAddress), &addressLength) != 0) {
        std::cerr << "network-smoke: getsockname failed\n";
        closeSocket(server);
        closeSocket(client);
#if defined(_WIN32)
        WSACleanup();
#endif
        return 1;
    }

    const uint32_t nonce = 0x12345678u;
    const auto hello = quantum::network::encodeHello(quantum::network::PacketType::Hello, nonce, 1);
    const int sent = ::sendto(client, reinterpret_cast<const char*>(hello.data()), static_cast<int>(hello.size()), 0,
        reinterpret_cast<const sockaddr*>(&serverAddress), sizeof(serverAddress));
    if (sent != static_cast<int>(hello.size())) {
        std::cerr << "network-smoke: hello send failed\n";
        closeSocket(server);
        closeSocket(client);
#if defined(_WIN32)
        WSACleanup();
#endif
        return 1;
    }

    uint8_t buffer[1500];
    sockaddr_in source{};
    bool accepted = false;
    for (int attempt = 0; attempt < 100 && !accepted; ++attempt) {
#if defined(_WIN32)
        int sourceLength = sizeof(source);
        const int received = ::recvfrom(server, reinterpret_cast<char*>(buffer), sizeof(buffer), 0,
            reinterpret_cast<sockaddr*>(&source), &sourceLength);
#else
        socklen_t sourceLength = sizeof(source);
        const int received = static_cast<int>(::recvfrom(server, buffer, sizeof(buffer), 0,
            reinterpret_cast<sockaddr*>(&source), &sourceLength));
#endif
        if (received > 0) {
            quantum::network::PacketHeader header;
            quantum::network::HelloPayload payload;
            accepted = quantum::network::decodeHello(buffer, static_cast<size_t>(received), header, payload)
                && header.type == quantum::network::PacketType::Hello && payload.nonce == nonce;
            if (accepted) {
                const auto ack = quantum::network::encodeHello(quantum::network::PacketType::HelloAck, payload.nonce, 2);
                ::sendto(server, reinterpret_cast<const char*>(ack.data()), static_cast<int>(ack.size()), 0,
                    reinterpret_cast<const sockaddr*>(&source), sizeof(source));
            }
        } else if (received < 0 && !wouldBlock()) {
            break;
        }
        if (!accepted) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    bool completed = false;
    for (int attempt = 0; attempt < 100 && accepted && !completed; ++attempt) {
#if defined(_WIN32)
        int sourceLength = sizeof(source);
        const int received = ::recvfrom(client, reinterpret_cast<char*>(buffer), sizeof(buffer), 0,
            reinterpret_cast<sockaddr*>(&source), &sourceLength);
#else
        socklen_t sourceLength = sizeof(source);
        const int received = static_cast<int>(::recvfrom(client, buffer, sizeof(buffer), 0,
            reinterpret_cast<sockaddr*>(&source), &sourceLength));
#endif
        if (received > 0) {
            quantum::network::PacketHeader header;
            quantum::network::HelloPayload payload;
            completed = quantum::network::decodeHello(buffer, static_cast<size_t>(received), header, payload)
                && header.type == quantum::network::PacketType::HelloAck && payload.nonce == nonce;
        } else if (received < 0 && !wouldBlock()) {
            break;
        }
        if (!completed) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    closeSocket(server);
    closeSocket(client);
#if defined(_WIN32)
    WSACleanup();
#endif

    if (!accepted || !completed) {
        std::cerr << "network-smoke: UDP handshake failed\n";
        return 1;
    }
    const std::vector<uint8_t> message = {1, 2, 3, 5, 8};
    const auto dataPacket = quantum::network::encodeReliable(quantum::network::PacketType::Data, 9, 0,
        message.data(), message.size());
    quantum::network::PacketHeader dataHeader;
    uint32_t acknowledgedSequence = 0;
    std::vector<uint8_t> decodedMessage;
    const bool reliableFraming = quantum::network::decodeReliable(dataPacket.data(), dataPacket.size(), dataHeader,
        acknowledgedSequence, decodedMessage)
        && dataHeader.sequence == 9 && acknowledgedSequence == 0 && decodedMessage == message;
    if (!reliableFraming) {
        std::cerr << "network-smoke: reliable framing failed\n";
        return 1;
    }
    const auto versionedMessage = quantum::network::encodeMessage(42, 3,
        message.data(), message.size());
    quantum::network::MessageEnvelope decodedEnvelope;
    const bool versionedFraming = quantum::network::decodeMessage(versionedMessage.data(),
        versionedMessage.size(), decodedEnvelope)
        && decodedEnvelope.messageType == 42 && decodedEnvelope.schemaVersion == 3
        && decodedEnvelope.payload == message;
    if (!versionedFraming) {
        std::cerr << "network-smoke: versioned message framing failed\n";
        return 1;
    }
    quantum::network::TransformSnapshot sourceSnapshot;
    sourceSnapshot.netId = 17;
    sourceSnapshot.position[0] = 1.5f;
    sourceSnapshot.position[1] = -2.0f;
    sourceSnapshot.position[2] = 3.25f;
    sourceSnapshot.rotation[0] = 1.0f;
    sourceSnapshot.rotation[1] = 0.1f;
    sourceSnapshot.rotation[2] = 0.2f;
    sourceSnapshot.rotation[3] = 0.3f;
    sourceSnapshot.scale[0] = 2.0f;
    sourceSnapshot.scale[1] = 1.0f;
    sourceSnapshot.scale[2] = 0.5f;
    const auto snapshotPacket = quantum::network::encodeTransformSnapshot(sourceSnapshot, 12);
    quantum::network::PacketHeader snapshotHeader;
    quantum::network::TransformSnapshot decodedSnapshot;
    const bool snapshotFraming = quantum::network::decodeTransformSnapshot(snapshotPacket.data(), snapshotPacket.size(),
        snapshotHeader, decodedSnapshot)
        && decodedSnapshot.netId == sourceSnapshot.netId
        && decodedSnapshot.position[0] == sourceSnapshot.position[0]
        && decodedSnapshot.rotation[3] == sourceSnapshot.rotation[3]
        && decodedSnapshot.scale[2] == sourceSnapshot.scale[2];
    if (!snapshotFraming) {
        std::cerr << "network-smoke: transform snapshot framing failed\n";
        return 1;
    }
    if (!runEngineNetworkIntegration()) {
        std::cerr << "network-smoke: NetworkSystem reliable delivery failed\n";
        return 1;
    }
    std::cout << "network-smoke: UDP handshake, reliable/unreliable channels, NetworkSystem delivery "
        "and attached Lua debugger OK\n";
    return 0;
}
