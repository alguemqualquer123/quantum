// (c) SR VINIX
// SPDX-License-Identifier: MIT

#include "NetworkProtocol.h"

#include "../util/Checksum.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace quantum::network {

namespace {

constexpr size_t HEADER_SIZE = 16;
constexpr size_t HELLO_SIZE = 8;
constexpr size_t HELLO_IDENTITY_SIZE = 12;
constexpr size_t HELLO_RESERVATION_SIZE = 16;
constexpr size_t LOBBY_QUERY_SIZE = 4;
constexpr size_t LOBBY_RESERVATION_REQUEST_SIZE = 8;
constexpr size_t LOBBY_RESERVATION_RESPONSE_SIZE = 12;
constexpr size_t LOBBY_FIXED_SIZE = 10;
constexpr size_t MAX_LOBBY_STRING_SIZE = 96;
constexpr size_t RELAY_REQUEST_FIXED_SIZE = 8;
constexpr size_t RELAY_RESPONSE_SIZE = 12;
constexpr size_t MAX_RELAY_ROOM_SIZE = 64;
constexpr size_t RELIABLE_PREFIX_SIZE = 4;
constexpr size_t MESSAGE_PREFIX_SIZE = 4;
constexpr size_t COMPRESSED_SNAPSHOT_SIZE = 24;
constexpr size_t INTEREST_UPDATE_SIZE = 20;
constexpr size_t REPLICATION_POLICY_SIZE = 17;
constexpr float MAX_INTEREST_RADIUS = 100000.0f;

std::vector<uint8_t> decodeHexDigest(const std::string& hex) {
    if (hex.size() != AUTHENTICATION_TAG_SIZE * 2) return {};
    auto nibble = [](char value) -> int {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        if (value >= 'A' && value <= 'F') return value - 'A' + 10;
        return -1;
    };
    std::vector<uint8_t> digest(AUTHENTICATION_TAG_SIZE);
    for (size_t i = 0; i < digest.size(); ++i) {
        const int high = nibble(hex[i * 2]);
        const int low = nibble(hex[i * 2 + 1]);
        if (high < 0 || low < 0) return {};
        digest[i] = static_cast<uint8_t>((high << 4) | low);
    }
    return digest;
}

void appendBytes(std::string& output, const uint8_t* data, size_t size) {
    if (data && size > 0) {
        output.append(reinterpret_cast<const char*>(data), size);
    }
}

bool constantTimeEqual(const uint8_t* left, const uint8_t* right, size_t size) {
    uint8_t difference = 0;
    for (size_t i = 0; i < size; ++i) difference |= static_cast<uint8_t>(left[i] ^ right[i]);
    return difference == 0;
}

struct SchemaDefinition {
    uint16_t currentVersion = 0;
    SchemaMigration migration = nullptr;
    size_t maxPayload = 0;
    SchemaValidation validation = nullptr;
};

std::mutex& schemaMutex() {
    static std::mutex mutex;
    return mutex;
}

std::unordered_map<uint16_t, SchemaDefinition>& schemas() {
    static std::unordered_map<uint16_t, SchemaDefinition> value;
    return value;
}

void writeU16(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value));
}

void writeU32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value >> 24));
    out.push_back(static_cast<uint8_t>(value >> 16));
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value));
}

uint16_t readU16(const uint8_t* data) {
    return static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8) | data[1]);
}

uint32_t readU32(const uint8_t* data) {
    return (static_cast<uint32_t>(data[0]) << 24)
        | (static_cast<uint32_t>(data[1]) << 16)
        | (static_cast<uint32_t>(data[2]) << 8)
        | static_cast<uint32_t>(data[3]);
}

void writeI16(std::vector<uint8_t>& out, int16_t value) {
    writeU16(out, static_cast<uint16_t>(value));
}

int16_t readI16(const uint8_t* data) {
    return static_cast<int16_t>(readU16(data));
}

int16_t quantize(float value, float scale) {
    const float scaled = std::clamp(value * scale, -32767.0f, 32767.0f);
    return static_cast<int16_t>(scaled >= 0.0f ? scaled + 0.5f : scaled - 0.5f);
}

float dequantize(int16_t value, float scale) {
    return static_cast<float>(value) / scale;
}

} // namespace

std::vector<uint8_t> hmacSha256(const std::string& key, const uint8_t* data, size_t size) {
    constexpr size_t BLOCK_SIZE = 64;
    std::array<uint8_t, BLOCK_SIZE> keyBlock{};
    if (key.size() > BLOCK_SIZE) {
        const std::string keyDigest = sha256Hex(key);
        const std::vector<uint8_t> decoded = decodeHexDigest(keyDigest);
        std::copy(decoded.begin(), decoded.end(), keyBlock.begin());
    } else if (!key.empty()) {
        std::copy(key.begin(), key.end(), keyBlock.begin());
    }

    std::string inner;
    std::string outer;
    inner.reserve(BLOCK_SIZE + size);
    outer.reserve(BLOCK_SIZE + AUTHENTICATION_TAG_SIZE);
    for (size_t i = 0; i < BLOCK_SIZE; ++i) {
        inner.push_back(static_cast<char>(keyBlock[i] ^ 0x36u));
        outer.push_back(static_cast<char>(keyBlock[i] ^ 0x5cu));
    }
    appendBytes(inner, data, size);
    const std::string innerDigestHex = sha256Hex(inner);
    const std::vector<uint8_t> innerDigest = decodeHexDigest(innerDigestHex);
    appendBytes(outer, innerDigest.data(), innerDigest.size());
    return decodeHexDigest(sha256Hex(outer));
}

std::vector<uint8_t> authenticatePacket(const uint8_t* data, size_t size, const std::string& token) {
    std::vector<uint8_t> packet;
    if (data && size > 0) packet.assign(data, data + size);
    if (token.empty()) return packet;
    const std::vector<uint8_t> tag = hmacSha256(token, packet.data(), packet.size());
    packet.insert(packet.end(), tag.begin(), tag.end());
    return packet;
}

std::vector<uint8_t> authenticatePacket(const std::vector<uint8_t>& packet, const std::string& token) {
    return authenticatePacket(packet.data(), packet.size(), token);
}

bool verifyPacketAuthentication(const uint8_t* data, size_t size, const std::string& token,
    size_t& authenticatedSize) {
    authenticatedSize = size;
    if (token.empty()) return true;
    if (!data || size < AUTHENTICATION_TAG_SIZE) return false;
    authenticatedSize = size - AUTHENTICATION_TAG_SIZE;
    const std::vector<uint8_t> expected = hmacSha256(token, data, authenticatedSize);
    return expected.size() == AUTHENTICATION_TAG_SIZE
        && constantTimeEqual(expected.data(), data + authenticatedSize, AUTHENTICATION_TAG_SIZE);
}

std::vector<uint8_t> encodeHello(PacketType type, uint32_t nonce, uint32_t sequence,
    uint32_t peerId) {
    std::vector<uint8_t> result;
    const size_t payloadSize = peerId == 0 ? HELLO_SIZE : HELLO_IDENTITY_SIZE;
    result.reserve(HEADER_SIZE + payloadSize);
    writeU32(result, PROTOCOL_MAGIC);
    writeU16(result, PROTOCOL_VERSION);
    writeU16(result, static_cast<uint16_t>(type));
    writeU32(result, static_cast<uint32_t>(payloadSize));
    writeU32(result, sequence);
    writeU32(result, PROTOCOL_VERSION);
    writeU32(result, nonce);
    if (peerId != 0) writeU32(result, peerId);
    return result;
}

std::vector<uint8_t> encodeHelloWithReservation(uint32_t nonce, uint32_t sequence,
    uint32_t reservationId) {
    std::vector<uint8_t> result;
    result.reserve(HEADER_SIZE + HELLO_RESERVATION_SIZE);
    writeU32(result, PROTOCOL_MAGIC);
    writeU16(result, PROTOCOL_VERSION);
    writeU16(result, static_cast<uint16_t>(PacketType::Hello));
    writeU32(result, static_cast<uint32_t>(HELLO_RESERVATION_SIZE));
    writeU32(result, sequence);
    writeU32(result, PROTOCOL_VERSION);
    writeU32(result, nonce);
    writeU32(result, 0);
    writeU32(result, reservationId);
    return result;
}

bool decodeHeader(const uint8_t* data, size_t size, PacketHeader& header) {
    if (!data || size < HEADER_SIZE) return false;
    header.magic = readU32(data);
    header.version = readU16(data + 4);
    header.type = static_cast<PacketType>(readU16(data + 6));
    header.payloadSize = readU32(data + 8);
    header.sequence = readU32(data + 12);
    return header.magic == PROTOCOL_MAGIC
        && header.version == PROTOCOL_VERSION
        && header.payloadSize <= size - HEADER_SIZE;
}

bool decodeHello(const uint8_t* data, size_t size, PacketHeader& header, HelloPayload& payload) {
    if (!decodeHeader(data, size, header)
            || (header.payloadSize != HELLO_SIZE && header.payloadSize != HELLO_IDENTITY_SIZE
                && header.payloadSize != HELLO_RESERVATION_SIZE)
            || size < HEADER_SIZE + header.payloadSize
            || (header.type != PacketType::Hello && header.type != PacketType::HelloAck)) {
        return false;
    }
    payload.protocolVersion = readU32(data + HEADER_SIZE);
    payload.nonce = readU32(data + HEADER_SIZE + 4);
    payload.peerId = header.payloadSize == HELLO_IDENTITY_SIZE
        ? readU32(data + HEADER_SIZE + 8) : 0;
    payload.reservationId = header.payloadSize == HELLO_RESERVATION_SIZE
        ? readU32(data + HEADER_SIZE + 12) : 0;
    return payload.protocolVersion == PROTOCOL_VERSION;
}

std::vector<uint8_t> encodeLobbyQuery(uint32_t nonce, uint32_t sequence) {
    std::vector<uint8_t> result;
    result.reserve(HEADER_SIZE + LOBBY_QUERY_SIZE);
    writeU32(result, PROTOCOL_MAGIC);
    writeU16(result, PROTOCOL_VERSION);
    writeU16(result, static_cast<uint16_t>(PacketType::LobbyQuery));
    writeU32(result, static_cast<uint32_t>(LOBBY_QUERY_SIZE));
    writeU32(result, sequence);
    writeU32(result, nonce);
    return result;
}

bool decodeLobbyQuery(const uint8_t* data, size_t size, PacketHeader& header, LobbyQuery& query) {
    if (!decodeHeader(data, size, header)
            || header.type != PacketType::LobbyQuery
            || header.payloadSize != LOBBY_QUERY_SIZE
            || size != HEADER_SIZE + LOBBY_QUERY_SIZE) {
        return false;
    }
    query.nonce = readU32(data + HEADER_SIZE);
    return true;
}

std::vector<uint8_t> encodeLobbyReservationRequest(uint32_t nonce, uint16_t requestedSeconds,
    uint32_t sequence) {
    std::vector<uint8_t> result;
    result.reserve(HEADER_SIZE + LOBBY_RESERVATION_REQUEST_SIZE);
    writeU32(result, PROTOCOL_MAGIC);
    writeU16(result, PROTOCOL_VERSION);
    writeU16(result, static_cast<uint16_t>(PacketType::LobbyReservationRequest));
    writeU32(result, static_cast<uint32_t>(LOBBY_RESERVATION_REQUEST_SIZE));
    writeU32(result, sequence);
    writeU32(result, nonce);
    writeU16(result, requestedSeconds);
    writeU16(result, 0);
    return result;
}

bool decodeLobbyReservationRequest(const uint8_t* data, size_t size, PacketHeader& header,
    LobbyReservationRequest& request) {
    if (!decodeHeader(data, size, header)
            || header.type != PacketType::LobbyReservationRequest
            || header.payloadSize != LOBBY_RESERVATION_REQUEST_SIZE
            || size != HEADER_SIZE + LOBBY_RESERVATION_REQUEST_SIZE) {
        return false;
    }
    request.nonce = readU32(data + HEADER_SIZE);
    request.requestedSeconds = readU16(data + HEADER_SIZE + 4);
    return true;
}

std::vector<uint8_t> encodeLobbyReservationResponse(const LobbyReservationResponse& response,
    uint32_t sequence) {
    std::vector<uint8_t> result;
    result.reserve(HEADER_SIZE + LOBBY_RESERVATION_RESPONSE_SIZE);
    writeU32(result, PROTOCOL_MAGIC);
    writeU16(result, PROTOCOL_VERSION);
    writeU16(result, static_cast<uint16_t>(PacketType::LobbyReservationResponse));
    writeU32(result, static_cast<uint32_t>(LOBBY_RESERVATION_RESPONSE_SIZE));
    writeU32(result, sequence);
    writeU32(result, response.nonce);
    writeU32(result, response.reservationId);
    writeU16(result, response.expiresSeconds);
    writeU16(result, response.accepted ? 1 : 0);
    return result;
}

bool decodeLobbyReservationResponse(const uint8_t* data, size_t size, PacketHeader& header,
    LobbyReservationResponse& response) {
    if (!decodeHeader(data, size, header)
            || header.type != PacketType::LobbyReservationResponse
            || header.payloadSize != LOBBY_RESERVATION_RESPONSE_SIZE
            || size != HEADER_SIZE + LOBBY_RESERVATION_RESPONSE_SIZE) {
        return false;
    }
    response.nonce = readU32(data + HEADER_SIZE);
    response.reservationId = readU32(data + HEADER_SIZE + 4);
    response.expiresSeconds = readU16(data + HEADER_SIZE + 8);
    response.accepted = readU16(data + HEADER_SIZE + 10) != 0;
    return true;
}

std::vector<uint8_t> encodeLobbyAdvertisement(const LobbyAdvertisement& advertisement, uint32_t sequence) {
    const auto bounded = [](const std::string& value) {
        return value.substr(0, MAX_LOBBY_STRING_SIZE);
    };
    const std::string serverName = bounded(advertisement.serverName);
    const std::string mapName = bounded(advertisement.mapName);
    const std::string buildId = bounded(advertisement.buildId);
    std::vector<uint8_t> payload;
    payload.reserve(LOBBY_FIXED_SIZE + 3 + serverName.size() + mapName.size() + buildId.size());
    writeU32(payload, advertisement.nonce);
    writeU16(payload, advertisement.gamePort);
    writeU16(payload, advertisement.currentPeers);
    writeU16(payload, advertisement.maxPeers);
    const auto appendString = [&payload](const std::string& value) {
        payload.push_back(static_cast<uint8_t>(value.size()));
        payload.insert(payload.end(), value.begin(), value.end());
    };
    appendString(serverName);
    appendString(mapName);
    appendString(buildId);

    std::vector<uint8_t> result;
    result.reserve(HEADER_SIZE + payload.size());
    writeU32(result, PROTOCOL_MAGIC);
    writeU16(result, PROTOCOL_VERSION);
    writeU16(result, static_cast<uint16_t>(PacketType::LobbyAdvertisement));
    writeU32(result, static_cast<uint32_t>(payload.size()));
    writeU32(result, sequence);
    result.insert(result.end(), payload.begin(), payload.end());
    return result;
}

bool decodeLobbyAdvertisement(const uint8_t* data, size_t size, PacketHeader& header,
    LobbyAdvertisement& advertisement) {
    if (!decodeHeader(data, size, header)
            || header.type != PacketType::LobbyAdvertisement
            || header.payloadSize < LOBBY_FIXED_SIZE + 3
            || size != HEADER_SIZE + header.payloadSize) {
        return false;
    }
    const uint8_t* payload = data + HEADER_SIZE;
    size_t offset = 0;
    advertisement.nonce = readU32(payload + offset);
    offset += 4;
    advertisement.gamePort = readU16(payload + offset);
    offset += 2;
    advertisement.currentPeers = readU16(payload + offset);
    offset += 2;
    advertisement.maxPeers = readU16(payload + offset);
    offset += 2;
    auto readString = [&payload, &offset, &header](std::string& value) {
        if (offset >= header.payloadSize) return false;
        const size_t length = payload[offset++];
        if (length > MAX_LOBBY_STRING_SIZE || offset + length > header.payloadSize) return false;
        value.assign(reinterpret_cast<const char*>(payload + offset), length);
        offset += length;
        return true;
    };
    if (!readString(advertisement.serverName) || !readString(advertisement.mapName)
            || !readString(advertisement.buildId) || offset != header.payloadSize) {
        return false;
    }
    return true;
}

std::vector<uint8_t> encodeRelaySessionRequest(PacketType type, uint32_t nonce,
    uint32_t sequence, const std::string& room) {
    if (type != PacketType::RelayRegister && type != PacketType::RelayJoin
            && type != PacketType::RelayHeartbeat) return {};
    const std::string boundedRoom = room.substr(0, MAX_RELAY_ROOM_SIZE);
    std::vector<uint8_t> payload;
    payload.reserve(RELAY_REQUEST_FIXED_SIZE + boundedRoom.size());
    writeU32(payload, nonce);
    writeU16(payload, static_cast<uint16_t>(boundedRoom.size()));
    writeU16(payload, 0);
    payload.insert(payload.end(), boundedRoom.begin(), boundedRoom.end());

    std::vector<uint8_t> result;
    result.reserve(HEADER_SIZE + payload.size());
    writeU32(result, PROTOCOL_MAGIC);
    writeU16(result, PROTOCOL_VERSION);
    writeU16(result, static_cast<uint16_t>(type));
    writeU32(result, static_cast<uint32_t>(payload.size()));
    writeU32(result, sequence);
    result.insert(result.end(), payload.begin(), payload.end());
    return result;
}

bool decodeRelaySessionRequest(const uint8_t* data, size_t size, PacketHeader& header,
    RelaySessionRequest& request) {
    if (!decodeHeader(data, size, header)
            || (header.type != PacketType::RelayRegister && header.type != PacketType::RelayJoin
                && header.type != PacketType::RelayHeartbeat)
            || header.payloadSize < RELAY_REQUEST_FIXED_SIZE
            || header.payloadSize > RELAY_REQUEST_FIXED_SIZE + MAX_RELAY_ROOM_SIZE
            || size != HEADER_SIZE + header.payloadSize) {
        return false;
    }
    const uint8_t* payload = data + HEADER_SIZE;
    const uint16_t roomSize = readU16(payload + 4);
    if (roomSize == 0 || roomSize > MAX_RELAY_ROOM_SIZE
            || RELAY_REQUEST_FIXED_SIZE + roomSize != header.payloadSize) {
        return false;
    }
    request.nonce = readU32(payload);
    request.room.assign(reinterpret_cast<const char*>(payload + RELAY_REQUEST_FIXED_SIZE), roomSize);
    return true;
}

std::vector<uint8_t> encodeRelaySessionResponse(const RelaySessionResponse& response,
    uint32_t sequence) {
    std::vector<uint8_t> result;
    result.reserve(HEADER_SIZE + RELAY_RESPONSE_SIZE);
    writeU32(result, PROTOCOL_MAGIC);
    writeU16(result, PROTOCOL_VERSION);
    writeU16(result, static_cast<uint16_t>(PacketType::RelayReady));
    writeU32(result, static_cast<uint32_t>(RELAY_RESPONSE_SIZE));
    writeU32(result, sequence);
    writeU32(result, response.nonce);
    writeU16(result, response.virtualPort);
    writeU16(result, response.peerPort);
    writeU16(result, response.accepted ? 1 : 0);
    writeU16(result, 0);
    return result;
}

bool decodeRelaySessionResponse(const uint8_t* data, size_t size, PacketHeader& header,
    RelaySessionResponse& response) {
    if (!decodeHeader(data, size, header)
            || header.type != PacketType::RelayReady
            || header.payloadSize != RELAY_RESPONSE_SIZE
            || size != HEADER_SIZE + RELAY_RESPONSE_SIZE) {
        return false;
    }
    const uint8_t* payload = data + HEADER_SIZE;
    response.nonce = readU32(payload);
    response.virtualPort = readU16(payload + 4);
    response.peerPort = readU16(payload + 6);
    response.accepted = readU16(payload + 8) != 0;
    return true;
}

std::vector<uint8_t> encodeReliable(PacketType type, uint32_t sequence, uint32_t acknowledgedSequence,
    const uint8_t* payload, size_t payloadSize) {
    std::vector<uint8_t> result;
    result.reserve(HEADER_SIZE + RELIABLE_PREFIX_SIZE + payloadSize);
    writeU32(result, PROTOCOL_MAGIC);
    writeU16(result, PROTOCOL_VERSION);
    writeU16(result, static_cast<uint16_t>(type));
    writeU32(result, static_cast<uint32_t>(RELIABLE_PREFIX_SIZE + payloadSize));
    writeU32(result, sequence);
    writeU32(result, acknowledgedSequence);
    if (payload && payloadSize > 0) result.insert(result.end(), payload, payload + payloadSize);
    return result;
}

std::vector<uint8_t> encodeUnreliable(uint32_t sequence, const uint8_t* payload, size_t payloadSize) {
    return encodeReliable(PacketType::Unreliable, sequence, 0, payload, payloadSize);
}

bool decodeReliable(const uint8_t* data, size_t size, PacketHeader& header, uint32_t& acknowledgedSequence,
    std::vector<uint8_t>& payload) {
    if (!decodeHeader(data, size, header)
            || (header.type != PacketType::Data && header.type != PacketType::Ack
                && header.type != PacketType::Snapshot && header.type != PacketType::SnapshotCompressed
                && header.type != PacketType::Unreliable && header.type != PacketType::InterestUpdate
                && header.type != PacketType::ReplicationPolicy)
            || header.payloadSize < RELIABLE_PREFIX_SIZE
            || size < HEADER_SIZE + header.payloadSize) {
        return false;
    }
    acknowledgedSequence = readU32(data + HEADER_SIZE);
    const size_t dataSize = header.payloadSize - RELIABLE_PREFIX_SIZE;
    payload.assign(data + HEADER_SIZE + RELIABLE_PREFIX_SIZE,
        data + HEADER_SIZE + RELIABLE_PREFIX_SIZE + dataSize);
    return true;
}

std::vector<uint8_t> encodeTransformSnapshot(const TransformSnapshot& snapshot, uint32_t sequence) {
    std::vector<uint8_t> payload;
    payload.reserve(44);
    writeU32(payload, snapshot.netId);
    for (float value : snapshot.position) {
        uint32_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        writeU32(payload, bits);
    }
    for (float value : snapshot.rotation) {
        uint32_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        writeU32(payload, bits);
    }
    for (float value : snapshot.scale) {
        uint32_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        writeU32(payload, bits);
    }
    return encodeReliable(PacketType::Snapshot, sequence, 0, payload.data(), payload.size());
}

bool decodeTransformSnapshot(const uint8_t* data, size_t size, PacketHeader& header,
    TransformSnapshot& snapshot) {
    uint32_t acknowledgedSequence = 0;
    std::vector<uint8_t> payload;
    if (!decodeReliable(data, size, header, acknowledgedSequence, payload)
            || (header.type != PacketType::Snapshot && header.type != PacketType::SnapshotCompressed)
            || acknowledgedSequence != 0
            || (header.type == PacketType::Snapshot && payload.size() != 44)
            || (header.type == PacketType::SnapshotCompressed && payload.size() != COMPRESSED_SNAPSHOT_SIZE)) {
        return false;
    }
    if (header.type == PacketType::SnapshotCompressed) {
        snapshot.sequence = header.sequence;
        size_t compressedOffset = 0;
        snapshot.netId = readU32(payload.data());
        compressedOffset += 4;
        for (float& value : snapshot.position) {
            value = dequantize(readI16(payload.data() + compressedOffset), 100.0f);
            compressedOffset += 2;
        }
        for (float& value : snapshot.rotation) {
            value = dequantize(readI16(payload.data() + compressedOffset), 32767.0f);
            compressedOffset += 2;
        }
        for (float& value : snapshot.scale) {
            value = dequantize(readI16(payload.data() + compressedOffset), 100.0f);
            compressedOffset += 2;
        }
        return true;
    }
    snapshot.sequence = header.sequence;
    size_t offset = 0;
    snapshot.netId = readU32(payload.data());
    offset += 4;
    auto readFloat = [&payload, &offset]() {
        const uint32_t bits = readU32(payload.data() + offset);
        offset += 4;
        float value;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    };
    for (float& value : snapshot.position) value = readFloat();
    for (float& value : snapshot.rotation) value = readFloat();
    for (float& value : snapshot.scale) value = readFloat();
    return true;
}

std::vector<uint8_t> encodeCompressedTransformSnapshot(const TransformSnapshot& snapshot, uint32_t sequence) {
    std::vector<uint8_t> payload;
    payload.reserve(COMPRESSED_SNAPSHOT_SIZE);
    writeU32(payload, snapshot.netId);
    for (float value : snapshot.position) writeI16(payload, quantize(value, 100.0f));
    for (float value : snapshot.rotation) writeI16(payload, quantize(value, 32767.0f));
    for (float value : snapshot.scale) writeI16(payload, quantize(value, 100.0f));
    return encodeReliable(PacketType::SnapshotCompressed, sequence, 0, payload.data(), payload.size());
}

std::vector<uint8_t> encodeInterestUpdate(const InterestUpdate& interest, uint32_t sequence) {
    std::vector<uint8_t> payload;
    payload.reserve(INTEREST_UPDATE_SIZE);
    for (float value : interest.center) {
        uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        writeU32(payload, bits);
    }
    uint32_t radiusBits = 0;
    std::memcpy(&radiusBits, &interest.radius, sizeof(radiusBits));
    writeU32(payload, radiusBits);
    writeU32(payload, interest.group);
    return encodeReliable(PacketType::InterestUpdate, sequence, 0, payload.data(), payload.size());
}

bool decodeInterestUpdate(const uint8_t* data, size_t size, PacketHeader& header,
    InterestUpdate& interest) {
    uint32_t acknowledgedSequence = 0;
    std::vector<uint8_t> payload;
    if (!decodeReliable(data, size, header, acknowledgedSequence, payload)
            || header.type != PacketType::InterestUpdate
            || acknowledgedSequence != 0
            || payload.size() != INTEREST_UPDATE_SIZE) {
        return false;
    }
    size_t offset = 0;
    auto readFloat = [&payload, &offset](float& value) {
        if (offset + 4 > payload.size()) return false;
        const uint32_t bits = readU32(payload.data() + offset);
        offset += 4;
        std::memcpy(&value, &bits, sizeof(value));
        return std::isfinite(value);
    };
    for (float& value : interest.center) {
        if (!readFloat(value)) return false;
    }
    if (!readFloat(interest.radius) || offset + 4 > payload.size()) return false;
    interest.group = readU32(payload.data() + offset);
    return std::isfinite(interest.radius) && interest.radius >= 0.0f;
}

std::vector<uint8_t> encodeReplicationPolicy(const EntityReplicationPolicy& policy, uint32_t sequence) {
    if (policy.netId == 0 || !std::isfinite(policy.interestRadius)
            || policy.interestRadius < 0.0f || policy.interestRadius > MAX_INTEREST_RADIUS) return {};
    std::vector<uint8_t> payload;
    payload.reserve(REPLICATION_POLICY_SIZE);
    writeU32(payload, policy.netId);
    writeU32(payload, policy.ownerId);
    writeU32(payload, policy.interestGroup);
    uint32_t radiusBits = 0;
    std::memcpy(&radiusBits, &policy.interestRadius, sizeof(radiusBits));
    writeU32(payload, radiusBits);
    payload.push_back(policy.serverAuthority ? 1 : 0);
    return encodeReliable(PacketType::ReplicationPolicy, sequence, 0, payload.data(), payload.size());
}

bool decodeReplicationPolicy(const uint8_t* data, size_t size, PacketHeader& header,
    EntityReplicationPolicy& policy) {
    uint32_t acknowledgedSequence = 0;
    std::vector<uint8_t> payload;
    if (!decodeReliable(data, size, header, acknowledgedSequence, payload)
            || header.type != PacketType::ReplicationPolicy || acknowledgedSequence != 0
            || payload.size() != REPLICATION_POLICY_SIZE) return false;
    policy.netId = readU32(payload.data());
    policy.ownerId = readU32(payload.data() + 4);
    policy.interestGroup = readU32(payload.data() + 8);
    const uint32_t radiusBits = readU32(payload.data() + 12);
    std::memcpy(&policy.interestRadius, &radiusBits, sizeof(policy.interestRadius));
    policy.serverAuthority = payload[16] != 0;
    return policy.netId != 0 && payload[16] <= 1
        && std::isfinite(policy.interestRadius) && policy.interestRadius >= 0.0f
        && policy.interestRadius <= MAX_INTEREST_RADIUS;
}

bool registerMessageSchema(uint16_t messageType, uint16_t currentVersion, SchemaMigration migration,
    size_t maxPayload, SchemaValidation validation) {
    if (messageType == 0 || currentVersion == 0) return false;
    if (maxPayload > 1024 * 1024) return false;
    std::lock_guard<std::mutex> lock(schemaMutex());
    schemas()[messageType] = SchemaDefinition{currentVersion, migration, maxPayload, validation};
    return true;
}

bool migrateMessage(MessageEnvelope& message) {
    std::lock_guard<std::mutex> lock(schemaMutex());
    const auto it = schemas().find(message.messageType);
    if (it == schemas().end()) return true;
    const SchemaDefinition& schema = it->second;
    if (message.schemaVersion > schema.currentVersion || !schema.migration) {
        return message.schemaVersion == schema.currentVersion;
    }
    while (message.schemaVersion < schema.currentVersion) {
        if (!schema.migration(message.schemaVersion, message.payload)) return false;
        ++message.schemaVersion;
    }
    return true;
}

bool validateMessage(const MessageEnvelope& message, uint32_t peerId) {
    SchemaDefinition schema;
    {
        std::lock_guard<std::mutex> lock(schemaMutex());
        const auto it = schemas().find(message.messageType);
        if (it == schemas().end()) return true;
        schema = it->second;
    }
    if (message.schemaVersion != schema.currentVersion
            || (schema.maxPayload > 0 && message.payload.size() > schema.maxPayload)) return false;
    return !schema.validation || schema.validation(peerId, message.payload);
}

std::vector<uint8_t> encodeMessage(uint16_t messageType, uint16_t schemaVersion,
    const uint8_t* payload, size_t payloadSize) {
    std::vector<uint8_t> result;
    result.reserve(MESSAGE_PREFIX_SIZE + payloadSize);
    writeU16(result, messageType);
    writeU16(result, schemaVersion);
    if (payload && payloadSize > 0) result.insert(result.end(), payload, payload + payloadSize);
    return result;
}

bool decodeMessage(const uint8_t* data, size_t size, MessageEnvelope& message) {
    if (!data || size < MESSAGE_PREFIX_SIZE) return false;
    message.messageType = readU16(data);
    message.schemaVersion = readU16(data + 2);
    message.payload.assign(data + MESSAGE_PREFIX_SIZE, data + size);
    return message.messageType != 0 && message.schemaVersion != 0;
}

} // namespace quantum::network
