// (c) SR VINIX
// SPDX-License-Identifier: MIT

#ifndef QUANTUM_NETWORK_PROTOCOL_H
#define QUANTUM_NETWORK_PROTOCOL_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace quantum::network {

constexpr uint32_t PROTOCOL_MAGIC = 0x514E4554u;
constexpr uint16_t PROTOCOL_VERSION = 1;
constexpr size_t AUTHENTICATION_TAG_SIZE = 32;

enum class PacketType : uint16_t {
    Hello = 1,
    HelloAck = 2,
    Data = 3,
    Ack = 4,
    Snapshot = 5,
    Unreliable = 6,
    SnapshotCompressed = 7,
    InterestUpdate = 8,
    ReplicationPolicy = 9,
    LobbyQuery = 10,
    LobbyAdvertisement = 11,
    LobbyReservationRequest = 12,
    LobbyReservationResponse = 13,
    RelayRegister = 14,
    RelayJoin = 15,
    RelayReady = 16,
    RelayHeartbeat = 17,
};

struct PacketHeader {
    uint32_t magic = PROTOCOL_MAGIC;
    uint16_t version = PROTOCOL_VERSION;
    PacketType type = PacketType::Hello;
    uint32_t payloadSize = 0;
    uint32_t sequence = 0;
};

struct HelloPayload {
    uint32_t protocolVersion = PROTOCOL_VERSION;
    uint32_t nonce = 0;
    // Assigned by the authoritative server in HelloAck. Zero is the legacy
    // value used by peers that predate session identity.
    uint32_t peerId = 0;
    // A client may attach a short-lived authenticated lobby reservation to its
    // first Hello. Legacy peers leave this field zero and keep the old framing.
    uint32_t reservationId = 0;
};

struct TransformSnapshot {
    uint32_t netId = 0;
    uint32_t sequence = 0;
    float position[3]{};
    float rotation[4]{};
    float scale[3]{};
};

struct MessageEnvelope {
    uint16_t messageType = 0;
    uint16_t schemaVersion = 1;
    std::vector<uint8_t> payload;
};

struct InterestUpdate {
    float center[3]{};
    float radius = 0.0f;
    uint32_t group = 0;
};

// Additive metadata sent beside transform snapshots. Older peers can ignore
// this packet and continue to consume the snapshot stream unchanged.
struct EntityReplicationPolicy {
    uint32_t netId = 0;
    uint32_t ownerId = 0;
    uint32_t interestGroup = 0;
    float interestRadius = 0.0f;
    bool serverAuthority = true;
};

struct LobbyQuery {
    uint32_t nonce = 0;
};

struct LobbyReservationRequest {
    uint32_t nonce = 0;
    uint16_t requestedSeconds = 0;
};

struct LobbyReservationResponse {
    uint32_t nonce = 0;
    uint32_t reservationId = 0;
    uint16_t expiresSeconds = 0;
    bool accepted = false;
};

struct LobbyAdvertisement {
    uint32_t nonce = 0;
    uint16_t gamePort = 0;
    uint16_t currentPeers = 0;
    uint16_t maxPeers = 0;
    std::string serverName;
    std::string mapName;
    std::string buildId;
};

struct RelaySessionRequest {
    uint32_t nonce = 0;
    std::string room;
};

struct RelaySessionResponse {
    uint32_t nonce = 0;
    uint16_t virtualPort = 0;
    uint16_t peerPort = 0;
    bool accepted = false;
};

// HMAC-SHA256 helpers. The authentication tag is deliberately kept outside the
// version-1 packet header so an empty token preserves the legacy wire format.
std::vector<uint8_t> hmacSha256(const std::string& key, const uint8_t* data, size_t size);
std::vector<uint8_t> authenticatePacket(const uint8_t* data, size_t size, const std::string& token);
std::vector<uint8_t> authenticatePacket(const std::vector<uint8_t>& packet, const std::string& token);
bool verifyPacketAuthentication(const uint8_t* data, size_t size, const std::string& token,
    size_t& authenticatedSize);

std::vector<uint8_t> encodeHello(PacketType type, uint32_t nonce, uint32_t sequence,
    uint32_t peerId = 0);
std::vector<uint8_t> encodeHelloWithReservation(uint32_t nonce, uint32_t sequence,
    uint32_t reservationId);
bool decodeHeader(const uint8_t* data, size_t size, PacketHeader& header);
bool decodeHello(const uint8_t* data, size_t size, PacketHeader& header, HelloPayload& payload);
std::vector<uint8_t> encodeLobbyQuery(uint32_t nonce, uint32_t sequence);
bool decodeLobbyQuery(const uint8_t* data, size_t size, PacketHeader& header, LobbyQuery& query);
std::vector<uint8_t> encodeLobbyReservationRequest(uint32_t nonce, uint16_t requestedSeconds,
    uint32_t sequence);
bool decodeLobbyReservationRequest(const uint8_t* data, size_t size, PacketHeader& header,
    LobbyReservationRequest& request);
std::vector<uint8_t> encodeLobbyReservationResponse(const LobbyReservationResponse& response,
    uint32_t sequence);
bool decodeLobbyReservationResponse(const uint8_t* data, size_t size, PacketHeader& header,
    LobbyReservationResponse& response);
std::vector<uint8_t> encodeLobbyAdvertisement(const LobbyAdvertisement& advertisement, uint32_t sequence);
bool decodeLobbyAdvertisement(const uint8_t* data, size_t size, PacketHeader& header,
    LobbyAdvertisement& advertisement);
std::vector<uint8_t> encodeRelaySessionRequest(PacketType type, uint32_t nonce,
    uint32_t sequence, const std::string& room);
bool decodeRelaySessionRequest(const uint8_t* data, size_t size, PacketHeader& header,
    RelaySessionRequest& request);
std::vector<uint8_t> encodeRelaySessionResponse(const RelaySessionResponse& response,
    uint32_t sequence);
bool decodeRelaySessionResponse(const uint8_t* data, size_t size, PacketHeader& header,
    RelaySessionResponse& response);

std::vector<uint8_t> encodeReliable(PacketType type, uint32_t sequence, uint32_t acknowledgedSequence,
    const uint8_t* payload, size_t payloadSize);
std::vector<uint8_t> encodeUnreliable(uint32_t sequence, const uint8_t* payload, size_t payloadSize);
bool decodeReliable(const uint8_t* data, size_t size, PacketHeader& header, uint32_t& acknowledgedSequence,
    std::vector<uint8_t>& payload);
std::vector<uint8_t> encodeTransformSnapshot(const TransformSnapshot& snapshot, uint32_t sequence);
bool decodeTransformSnapshot(const uint8_t* data, size_t size, PacketHeader& header, TransformSnapshot& snapshot);
std::vector<uint8_t> encodeCompressedTransformSnapshot(const TransformSnapshot& snapshot, uint32_t sequence);
std::vector<uint8_t> encodeInterestUpdate(const InterestUpdate& interest, uint32_t sequence);
bool decodeInterestUpdate(const uint8_t* data, size_t size, PacketHeader& header,
    InterestUpdate& interest);
std::vector<uint8_t> encodeReplicationPolicy(const EntityReplicationPolicy& policy, uint32_t sequence);
bool decodeReplicationPolicy(const uint8_t* data, size_t size, PacketHeader& header,
    EntityReplicationPolicy& policy);

using SchemaMigration = bool (*)(uint16_t fromVersion, std::vector<uint8_t>& payload);
using SchemaValidation = bool (*)(uint32_t peerId, const std::vector<uint8_t>& payload);
bool registerMessageSchema(uint16_t messageType, uint16_t currentVersion,
    SchemaMigration migration = nullptr, size_t maxPayload = 0,
    SchemaValidation validation = nullptr);
bool migrateMessage(MessageEnvelope& message);
bool validateMessage(const MessageEnvelope& message, uint32_t peerId);

std::vector<uint8_t> encodeMessage(uint16_t messageType, uint16_t schemaVersion,
    const uint8_t* payload, size_t payloadSize);
bool decodeMessage(const uint8_t* data, size_t size, MessageEnvelope& message);

} // namespace quantum::network

#endif
