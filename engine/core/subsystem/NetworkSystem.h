// (c) SR VINIX
// SPDX-License-Identifier: MIT

#pragma once

#include "../ecs/SubSystem.h"
#include "../component/Transform.h"
#include "../ecs/components/NetworkComponent.h"
#include "NetworkProtocol.h"
#include <vector>
#include <deque>
#include <map>
#include <unordered_map>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>
#include <functional>
#include <string>

namespace quantum {

class SaveGame;

class QUANTUM_API NetworkSystem : public SubSystem {
public:
    using ReplicatedComponentEncode = std::function<bool(Scene*, Entity, std::string&)>;
    using ReplicatedComponentDecode = std::function<bool(uint16_t, Scene*, Entity, const std::string&)>;

    struct ReplicatedComponentSerializer {
        std::string name;
        uint16_t schemaVersion = 1;
        ReplicatedComponentEncode encode;
        ReplicatedComponentDecode decode;
    };

    struct ReplicatedComponentDescriptor {
        std::string name;
        uint16_t schemaVersion = 0;
    };

    struct Metrics {
        uint64_t reliableSent = 0;
        uint64_t reliableReceived = 0;
        uint64_t reliableRetransmitted = 0;
        uint64_t reliableDropped = 0;
        uint64_t reliableOutOfOrder = 0;
        uint64_t unreliableSent = 0;
        uint64_t unreliableReceived = 0;
        uint64_t unreliableDropped = 0;
        uint64_t snapshotsSent = 0;
        uint64_t snapshotsReceived = 0;
        uint64_t simulatedPacketDrops = 0;
        uint64_t simulatedPacketReorders = 0;
        uint64_t packetsSent = 0;
        uint64_t packetsReceived = 0;
        uint64_t bytesSent = 0;
        uint64_t bytesReceived = 0;
        uint64_t malformedPackets = 0;
        uint64_t unauthorizedPackets = 0;
        uint64_t rateLimitedPackets = 0;
        double packetLossPercent = 0.0;
        double reliableRttMs = 0.0;
        double reliableRetransmitTimeoutMs = 100.0;
        uint32_t reliableWindow = 8;
        uint32_t reliableQueueDepth = 0;
        uint32_t unreliableQueueDepth = 0;
        uint32_t receivedQueueDepth = 0;
        uint32_t queueHighWatermark = 0;
    };

    struct LobbyInfo {
        std::string serverName = "Quantum Server";
        std::string mapName;
        std::string buildId;
    };

    struct LobbyServer {
        std::string address;
        network::LobbyAdvertisement advertisement;
    };

    struct LobbyReservation {
        std::string address;
        uint32_t reservationId = 0;
        uint16_t expiresSeconds = 0;
        bool accepted = false;
    };

    NetworkSystem(Scene* scene);
    ~NetworkSystem();

    // Initialize as client or server based on command‑line args.
    void startServer(uint16_t port);
    void startClient(const std::string& address, uint16_t port, uint32_t reservationId = 0);
    // Route the existing UDP protocol through an authenticated relay room. The
    // relay never terminates gameplay HMAC or replication; it only forwards
    // datagrams and assigns a virtual UDP port per participant.
    void startRelayServer(const std::string& relayAddress, uint16_t relayPort,
        const std::string& room, const std::string& relayToken, uint16_t localPort = 0);
    void startRelayClient(const std::string& relayAddress, uint16_t relayPort,
        const std::string& room, const std::string& relayToken);
    // Optional pre-shared token. An empty token keeps the legacy unauthenticated wire format.
    void setAuthToken(const std::string& token);
    bool isConnected() const { return connectedState.load(); }
    bool isServer() const { return mode == Mode::Server; }
    bool isClient() const { return mode == Mode::Client; }
    // Zero identifies the authoritative server; clients receive a non-zero
    // session identity in the authenticated HelloAck when supported.
    uint32_t getLocalPeerId() const { return localPeerId.load(); }
    // A server may accept multiple authenticated UDP peers. Clients report zero
    // or one; the value is safe to poll from gameplay/UI code.
    uint32_t getConnectedPeerCount() const { return connectedPeerCount.load(); }
    void setMaxPeers(uint32_t count);
    uint32_t getMaxPeers() const { return maxPeers.load(); }
    void setLobbyInfo(const LobbyInfo& info);
    LobbyInfo getLobbyInfo() const;
    static std::vector<LobbyServer> discoverLan(uint16_t discoveryPort, uint32_t timeoutMs,
        const std::string& authToken = {}, const std::string& address = "255.255.255.255");
    // Reserves one server slot for a short period. Reservations require the
    // same non-empty auth token used by gameplay datagrams.
    static LobbyReservation reserveLan(uint16_t discoveryPort, uint32_t timeoutMs,
        const std::string& authToken, const std::string& address = "255.255.255.255",
        uint16_t requestedSeconds = 10);

    // Called each frame to process incoming packets and send updates.
    void update(double dt) override;
    void load() override {}
    void draw() override {}
    void destroy() override;

    // Register a new entity for replication.
    void registerEntity(Entity entity);

    // Queue a reliable application message for the connected peer.
    uint32_t sendReliable(const std::vector<uint8_t>& payload);
    uint32_t sendUnreliable(const std::vector<uint8_t>& payload);
    uint32_t sendReliableMessage(uint16_t messageType, uint16_t schemaVersion,
        const std::vector<uint8_t>& payload);

    // Lua-friendly reliable message helpers.
    uint32_t sendReliableText(const std::string& payload);
    uint32_t sendUnreliableText(const std::string& payload);

    // Read the next reliable application message received by the network thread.
    bool pollReliable(std::vector<uint8_t>& payload);
    bool pollUnreliable(std::vector<uint8_t>& payload);
    bool pollReliableMessage(uint16_t& messageType, uint16_t& schemaVersion,
        std::vector<uint8_t>& payload);
    std::string pollReliableText();
    std::string pollUnreliableText();
    Metrics getMetrics() const;

    // Client-side transform prediction. The velocity is simulated locally until
    // the authoritative snapshot arrives, then reconciled over the interpolation window.
    void setPredictionVelocity(Entity entity, Vector3 velocity);
    void clearPrediction(Entity entity);
    // Returns whether this runtime is allowed to author the entity's replicated
    // state. The standalone mode remains permissive for offline tooling/tests.
    bool hasEntityAuthority(Entity entity) const;
    // QA hook: drops reliable data packets before send to exercise retransmission.
    void setPacketLossSimulationForTesting(uint32_t percent);
    // QA hook: delays a percentage of outgoing data packets to exercise reordering.
    void setPacketReorderingSimulationForTesting(uint32_t percent);

    // Sets the client's area-of-interest. The server filters snapshots by this
    // value without changing the existing snapshot wire format.
    void setInterestCenter(Vector3 center, float radius, uint32_t group = 0);
    void clearInterestCenter();
    bool setEntityReplicationPolicy(Entity entity, uint32_t ownerId, uint32_t interestGroup,
        float interestRadius, bool serverAuthority = true);

    // Returns the server transform closest to the requested point in the recent past.
    bool getHistoricalTransform(uint32_t netId, double secondsAgo,
        network::TransformSnapshot& snapshot) const;
    bool saveReplicatedState(SaveGame& save, const std::string& key) const;
    bool loadReplicatedState(const SaveGame& save, const std::string& key);
    // Registers a versioned application message in the module that owns the
    // UDP loop. Server sessions use the optional validator before queueing a
    // command, avoiding duplicated registries across executable/DLL builds.
    static bool registerServerMessageSchema(uint16_t messageType, uint16_t currentVersion,
        network::SchemaMigration migration = nullptr, size_t maxPayload = 0,
        network::SchemaValidation validation = nullptr);
    static bool registerReplicatedComponent(const ReplicatedComponentSerializer& serializer);
    // Returns a deterministic public catalog without exposing serializer callbacks.
    static std::vector<ReplicatedComponentDescriptor> getReplicatedComponentCatalog();

private:
    enum class Mode { None, Server, Client } mode = Mode::None;
    uint16_t listenPort = 0;
    std::string serverAddress;
    bool relayEnabled = false;
    std::string relayAddress;
    uint16_t relayPort = 0;
    std::string relayRoom;
    std::string relayToken;
    std::atomic<uint16_t> relayPeerPort{0};
    std::atomic<bool> relayReady{false};
    std::thread networkThread;
    std::atomic<bool> running{false};
    std::atomic<bool> connectedState{false};
    std::atomic<uint32_t> localPeerId{0};
    std::atomic<uint32_t> connectedPeerCount{0};
    std::atomic<uint32_t> maxPeers{16};
    std::atomic<uint32_t> packetLossSimulationPercent{0};
    std::atomic<uint32_t> packetReorderSimulationPercent{0};
    mutable std::mutex entityMutex;
    std::vector<Entity> replicatedEntities;
    uint32_t nextSequence = 1;
    uint32_t clientNonce = 0;
    uint32_t clientReservationId = 0;
    uint32_t nextPeerId = 1;
    uint32_t nextNetworkEntityId = 1;
    uint32_t nextReliableSequence = 1;
    uint32_t nextExpectedReliableSequence = 1;
    uint32_t reliableWindow = 8;

    struct PendingReliable {
        std::vector<uint8_t> payload;
        std::chrono::steady_clock::time_point lastSent{};
        uint32_t attempts = 0;
    };
    mutable std::mutex reliableMutex;
    std::unordered_map<uint32_t, PendingReliable> pendingReliable;
    // Server-side messages are fanned out to every currently connected peer.
    // Per-peer sequence numbers and retry state live in networkLoop so a slow
    // client cannot acknowledge or evict another client's packet.
    std::deque<std::vector<uint8_t>> pendingReliableBroadcast;
    std::deque<std::vector<uint8_t>> receivedReliable;
    std::map<uint32_t, std::vector<uint8_t>> orderedReliable;
    std::deque<std::vector<uint8_t>> pendingUnreliable;
    std::deque<std::vector<uint8_t>> receivedUnreliable;
    std::deque<network::TransformSnapshot> receivedSnapshots;
    std::deque<network::EntityReplicationPolicy> receivedReplicationPolicies;
    std::unordered_map<uint32_t, network::EntityReplicationPolicy> pendingReplicationPolicies;
    struct SnapshotBlend {
        network::TransformSnapshot current;
        network::TransformSnapshot target;
        float elapsed = 0.0f;
        bool initialized = false;
        uint32_t lastSnapshotSequence = 0;
    };
    std::unordered_map<uint32_t, SnapshotBlend> snapshotBlends;
    struct PredictionState {
        Vector3 velocity = Vector3::ZERO;
    };
    std::unordered_map<Entity, PredictionState> predictions;

    struct HistoricalSnapshot {
        std::chrono::steady_clock::time_point time;
        network::TransformSnapshot snapshot;
    };
    mutable std::mutex historyMutex;
    std::unordered_map<uint32_t, std::deque<HistoricalSnapshot>> snapshotHistory;
    mutable std::mutex metricsMutex;
    Metrics metrics;
    double reliableRttVarianceMs = 50.0;
    mutable std::mutex authMutex;
    std::string authToken;
    mutable std::mutex lobbyMutex;
    LobbyInfo lobbyInfo;

    struct PeerInterest {
        Vector3 center = Vector3::ZERO;
        float radius = 0.0f;
        uint32_t group = 0;
        bool valid = false;
    };
    mutable std::mutex interestMutex;
    PeerInterest localInterest;
    PeerInterest peerInterest;
    std::chrono::steady_clock::time_point lastInterestSent{};

    std::string getAuthToken() const;
    void networkLoop(); // runs in background thread
};

}
