// (c) SR VINIX
// SPDX-License-Identifier: MIT

#include "engine/core/subsystem/NetworkProtocol.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#ifdef max
#undef max
#endif
using RelaySocket = SOCKET;
constexpr RelaySocket INVALID_RELAY_SOCKET = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
using RelaySocket = int;
constexpr RelaySocket INVALID_RELAY_SOCKET = -1;
#endif

namespace {

namespace network = quantum::network;
using RelaySessionRequest = network::RelaySessionRequest;
using RelaySessionResponse = network::RelaySessionResponse;

constexpr size_t MAX_PACKET_BYTES = 1400;
constexpr size_t MAX_ROOM_MEMBERS = 16;
constexpr size_t MAX_ROOMS = 256;
constexpr size_t MAX_PARTICIPANTS = 256;
constexpr uint32_t CONTROL_PACKETS_PER_SECOND = 2400;
constexpr size_t CONTROL_BYTES_PER_SECOND = 256u * 1024u;
constexpr uint32_t PARTICIPANT_PACKETS_PER_SECOND = 1200;
constexpr size_t PARTICIPANT_BYTES_PER_SECOND = 2u * 1024u * 1024u;
constexpr uint32_t DEFAULT_PARTICIPANT_TIMEOUT_MS = 45000;
constexpr size_t NO_PARTICIPANT = std::numeric_limits<size_t>::max();

bool wouldBlock() {
#if defined(_WIN32)
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

void closeSocket(RelaySocket socket) {
#if defined(_WIN32)
    closesocket(socket);
#else
    close(socket);
#endif
}

bool makeNonBlocking(RelaySocket socket) {
#if defined(_WIN32)
    u_long enabled = 1;
    return ioctlsocket(socket, FIONBIO, &enabled) == 0;
#else
    const int flags = fcntl(socket, F_GETFL, 0);
    return flags >= 0 && fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

bool sameEndpoint(const sockaddr_in& left, const sockaddr_in& right) {
    return left.sin_family == right.sin_family
        && left.sin_port == right.sin_port
        && left.sin_addr.s_addr == right.sin_addr.s_addr;
}

bool receiveDatagram(RelaySocket socket, uint8_t* buffer, size_t capacity,
    sockaddr_in& source, int& received) {
#if defined(_WIN32)
    int length = sizeof(source);
    received = ::recvfrom(socket, reinterpret_cast<char*>(buffer), static_cast<int>(capacity), 0,
        reinterpret_cast<sockaddr*>(&source), &length);
#else
    socklen_t length = sizeof(source);
    received = static_cast<int>(::recvfrom(socket, buffer, capacity, 0,
        reinterpret_cast<sockaddr*>(&source), &length));
#endif
    return received > 0;
}

bool sendDatagram(RelaySocket socket, const uint8_t* buffer, size_t size,
    const sockaddr_in& destination) {
    if (!buffer || size == 0 || size > MAX_PACKET_BYTES) return false;
    const int sent = ::sendto(socket, reinterpret_cast<const char*>(buffer),
        static_cast<int>(size), 0, reinterpret_cast<const sockaddr*>(&destination),
        sizeof(destination));
    return sent == static_cast<int>(size);
}

struct RateWindow {
    std::chrono::steady_clock::time_point started{};
    uint32_t packets = 0;
    size_t bytes = 0;
};

bool allowRate(RateWindow& window, std::chrono::steady_clock::time_point now,
    size_t bytes, uint32_t packetLimit, size_t byteLimit) {
    if (window.started.time_since_epoch().count() == 0
            || now - window.started >= std::chrono::seconds(1)) {
        window.started = now;
        window.packets = 0;
        window.bytes = 0;
    }
    if (window.packets >= packetLimit || bytes > byteLimit
            || window.bytes > byteLimit - bytes) return false;
    ++window.packets;
    window.bytes += bytes;
    return true;
}

bool parsePort(const char* value, uint16_t& port, bool allowZero = false) {
    if (!value || !*value) return false;
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    if (end == value || *end != '\0' || parsed > 65535
            || (!allowZero && parsed == 0)) return false;
    port = static_cast<uint16_t>(parsed);
    return true;
}

struct Participant {
    RelaySocket socket = INVALID_RELAY_SOCKET;
    sockaddr_in physical{};
    std::string room;
    bool host = false;
    bool active = true;
    uint16_t virtualPort = 0;
    uint32_t nonce = 0;
    std::chrono::steady_clock::time_point lastActivity{};
    RateWindow rateWindow;
};

struct Room {
    std::string name;
    size_t host = NO_PARTICIPANT;
    std::vector<size_t> members;
};

struct Options {
    bool help = false;
    uint16_t port = 0;
    std::string token;
    size_t maxPeers = MAX_ROOM_MEMBERS;
    uint32_t durationMs = 0;
    uint32_t participantTimeoutMs = DEFAULT_PARTICIPANT_TIMEOUT_MS;
};

void printUsage() {
    std::cout
        << "quantum-network-relay - authenticated UDP relay for Quantum rooms\n"
        << "Usage: quantum-network-relay --port PORT --token TOKEN [options]\n"
        << "  --port PORT          Bind relay control socket (0 chooses a free port).\n"
        << "  --token TOKEN        HMAC token for register/join control packets.\n"
        << "  --max-peers COUNT    Maximum clients per room (1-16, default 16).\n"
        << "  --participant-timeout-ms N  Expire idle participants after N ms (default 45000).\n"
        << "  --duration-ms N      Exit after N milliseconds (headless smoke only).\n";
}

bool parseOptions(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        auto next = [&](const char*& value) {
            if (i + 1 >= argc) return false;
            value = argv[++i];
            return value && *value;
        };
        const char* value = nullptr;
        if (argument == "--help" || argument == "-h") {
            printUsage();
            options.help = true;
            return true;
        }
        if (argument == "--port") {
            if (!next(value) || !parsePort(value, options.port, true)) return false;
        } else if (argument == "--token") {
            if (!next(value)) return false;
            options.token = value;
        } else if (argument == "--max-peers") {
            if (!next(value)) return false;
            char* end = nullptr;
            const unsigned long parsed = std::strtoul(value, &end, 10);
            if (end == value || *end != '\0' || parsed == 0 || parsed > MAX_ROOM_MEMBERS) {
                return false;
            }
            options.maxPeers = static_cast<size_t>(parsed);
        } else if (argument == "--duration-ms") {
            if (!next(value)) return false;
            char* end = nullptr;
            const unsigned long parsed = std::strtoul(value, &end, 10);
            if (end == value || *end != '\0' || parsed > 3600000) return false;
            options.durationMs = static_cast<uint32_t>(parsed);
        } else if (argument == "--participant-timeout-ms") {
            if (!next(value)) return false;
            char* end = nullptr;
            const unsigned long parsed = std::strtoul(value, &end, 10);
            if (end == value || *end != '\0' || parsed < 1000 || parsed > 3600000) {
                return false;
            }
            options.participantTimeoutMs = static_cast<uint32_t>(parsed);
        } else {
            std::cerr << "quantum-network-relay: unknown argument '" << argument << "'\n";
            return false;
        }
    }
    if (options.port == 0 && options.token.empty()) {
        std::cerr << "quantum-network-relay: --token is required\n";
        return false;
    }
    return options.port <= 65535 && !options.token.empty();
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parseOptions(argc, argv, options)) return 2;
    if (options.help) return 0;

#if defined(_WIN32)
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::cerr << "quantum-network-relay: WSAStartup failed\n";
        return 1;
    }
#endif

    RelaySocket controlSocket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (controlSocket == INVALID_RELAY_SOCKET || !makeNonBlocking(controlSocket)) {
        if (controlSocket != INVALID_RELAY_SOCKET) closeSocket(controlSocket);
#if defined(_WIN32)
        WSACleanup();
#endif
        std::cerr << "quantum-network-relay: failed to create UDP socket\n";
        return 1;
    }
    sockaddr_in controlAddress{};
    controlAddress.sin_family = AF_INET;
    controlAddress.sin_addr.s_addr = htonl(INADDR_ANY);
    controlAddress.sin_port = htons(options.port);
    if (::bind(controlSocket, reinterpret_cast<const sockaddr*>(&controlAddress),
            sizeof(controlAddress)) != 0) {
        closeSocket(controlSocket);
#if defined(_WIN32)
        WSACleanup();
#endif
        std::cerr << "quantum-network-relay: bind failed\n";
        return 1;
    }
#if defined(_WIN32)
    int windowsAddressLength = sizeof(controlAddress);
    getsockname(controlSocket, reinterpret_cast<sockaddr*>(&controlAddress), &windowsAddressLength);
#else
    socklen_t addressLength = sizeof(controlAddress);
    getsockname(controlSocket, reinterpret_cast<sockaddr*>(&controlAddress), &addressLength);
#endif
    const uint16_t actualPort = ntohs(controlAddress.sin_port);
    std::cout << "quantum-network-relay: listening on 127.0.0.1:" << actualPort << "\n";
    std::cout.flush();

    std::vector<Room> rooms;
    std::vector<Participant> participants;
    uint32_t responseSequence = 1;
    uint64_t rateLimitedControlPackets = 0;
    uint64_t rateLimitedParticipantPackets = 0;
    uint64_t heartbeatPackets = 0;
    uint64_t rejectedHeartbeats = 0;
    uint64_t expiredParticipants = 0;
    uint64_t rejectedRooms = 0;
    const auto startedAt = std::chrono::steady_clock::now();
    RateWindow controlRate;
    bool running = true;

    const auto findRoom = [&](const std::string& name) -> Room* {
        for (Room& room : rooms) if (room.name == name) return &room;
        return nullptr;
    };
    const auto activeMemberCount = [](const Room& room, const std::vector<Participant>& all) {
        size_t count = 0;
        for (const size_t index : room.members) {
            if (index < all.size() && all[index].active) ++count;
        }
        return count;
    };
    const auto activeParticipantCount = [&]() {
        size_t count = 0;
        for (const Participant& participant : participants) {
            if (participant.active) ++count;
        }
        return count;
    };
    const auto retireParticipant = [&](size_t index) {
        if (index >= participants.size()) return;
        Participant& participant = participants[index];
        if (participant.socket != INVALID_RELAY_SOCKET) {
            closeSocket(participant.socket);
            participant.socket = INVALID_RELAY_SOCKET;
        }
        participant.active = false;
        for (Room& room : rooms) {
            if (room.host == index) room.host = NO_PARTICIPANT;
            room.members.erase(std::remove(room.members.begin(), room.members.end(), index),
                               room.members.end());
        }
    };
    const auto findParticipant = [&](const sockaddr_in& endpoint, const std::string& roomName) {
        for (size_t i = 0; i < participants.size(); ++i) {
            if (participants[i].active && participants[i].room == roomName
                    && sameEndpoint(participants[i].physical, endpoint)) return i;
        }
        return NO_PARTICIPANT;
    };
    const auto sendResponse = [&](const sockaddr_in& destination, const network::RelaySessionResponse& response) {
        const auto packet = network::authenticatePacket(
            network::encodeRelaySessionResponse(response, responseSequence++), options.token);
        sendDatagram(controlSocket, packet.data(), packet.size(), destination);
    };
    const auto makeVirtualSocket = [&](RelaySocket& socket, uint16_t& port) {
        socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket == INVALID_RELAY_SOCKET || !makeNonBlocking(socket)) {
            if (socket != INVALID_RELAY_SOCKET) closeSocket(socket);
            socket = INVALID_RELAY_SOCKET;
            return false;
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_ANY);
        address.sin_port = htons(0);
        if (::bind(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
            closeSocket(socket);
            socket = INVALID_RELAY_SOCKET;
            return false;
        }
#if defined(_WIN32)
        int length = sizeof(address);
        if (getsockname(socket, reinterpret_cast<sockaddr*>(&address), &length) != 0) return false;
#else
        socklen_t length = sizeof(address);
        if (getsockname(socket, reinterpret_cast<sockaddr*>(&address), &length) != 0) return false;
#endif
        port = ntohs(address.sin_port);
        return port != 0;
    };

    while (running) {
        const auto now = std::chrono::steady_clock::now();
        if (options.durationMs > 0
                && now - startedAt >= std::chrono::milliseconds(options.durationMs)) break;

        uint8_t buffer[1500]{};
        sockaddr_in source{};
        int received = 0;
        while (receiveDatagram(controlSocket, buffer, sizeof(buffer), source, received)) {
            // Control traffic is authenticated, but a leaked relay token must not
            // turn the single-threaded matchmaker into an unbounded packet sink.
            if (!allowRate(controlRate, now, static_cast<size_t>(received),
                    CONTROL_PACKETS_PER_SECOND, CONTROL_BYTES_PER_SECOND)) {
                ++rateLimitedControlPackets;
                continue;
            }
            if (static_cast<size_t>(received) > MAX_PACKET_BYTES) continue;
            size_t authenticatedSize = static_cast<size_t>(received);
            if (!network::verifyPacketAuthentication(buffer, authenticatedSize, options.token,
                    authenticatedSize)) continue;
            network::PacketHeader header;
            network::RelaySessionRequest request;
            if (!network::decodeRelaySessionRequest(buffer, authenticatedSize, header, request)) continue;

            const bool heartbeat = header.type == network::PacketType::RelayHeartbeat;
            const size_t existing = findParticipant(source, request.room);
            if (heartbeat && existing == NO_PARTICIPANT) {
                ++rejectedHeartbeats;
                RelaySessionResponse response;
                response.nonce = request.nonce;
                sendResponse(source, response);
                continue;
            }
            Room* room = findRoom(request.room);
            if (!room) {
                if (rooms.size() >= MAX_ROOMS) {
                    ++rejectedRooms;
                    RelaySessionResponse response;
                    response.nonce = request.nonce;
                    sendResponse(source, response);
                    continue;
                }
                rooms.push_back(Room{});
                rooms.back().name = request.room;
                room = &rooms.back();
            }
            const bool wantsHost = header.type == network::PacketType::RelayRegister;
            if (existing != NO_PARTICIPANT) {
                Participant& participant = participants[existing];
                participant.lastActivity = now;
                if (heartbeat) ++heartbeatPackets;
                RelaySessionResponse response;
                response.nonce = request.nonce;
                response.virtualPort = participant.virtualPort;
                response.peerPort = participant.host ? 0
                    : (room->host < participants.size() ? participants[room->host].virtualPort : 0);
                response.accepted = heartbeat || participant.host == wantsHost;
                sendResponse(source, response);
                continue;
            }

            const bool hostAvailable = room->host != NO_PARTICIPANT
                && room->host < participants.size() && participants[room->host].active;
            const bool accepted = activeParticipantCount() < MAX_PARTICIPANTS
                && ((wantsHost && !hostAvailable)
                    || (!wantsHost && hostAvailable
                        && activeMemberCount(*room, participants) < options.maxPeers));
            RelaySessionResponse response;
            response.nonce = request.nonce;
            response.accepted = accepted;
            if (accepted) {
                Participant participant;
                participant.physical = source;
                participant.room = request.room;
                participant.host = wantsHost;
                participant.nonce = request.nonce;
                participant.lastActivity = now;
                if (!makeVirtualSocket(participant.socket, participant.virtualPort)) {
                    response.accepted = false;
                } else {
                    size_t index = NO_PARTICIPANT;
                    for (size_t candidate = 0; candidate < participants.size(); ++candidate) {
                        if (!participants[candidate].active) {
                            index = candidate;
                            break;
                        }
                    }
                    if (index == NO_PARTICIPANT) {
                        participants.push_back(std::move(participant));
                        index = participants.size() - 1;
                    } else {
                        participants[index] = std::move(participant);
                    }
                    room->members.push_back(index);
                    if (wantsHost) room->host = index;
                    response.virtualPort = participants[index].virtualPort;
                    response.peerPort = wantsHost ? 0 : participants[room->host].virtualPort;
                    std::cout << "quantum-network-relay: " << (wantsHost ? "registered" : "joined")
                        << " room=" << request.room << " virtualPort=" << response.virtualPort << "\n";
                }
            }
            if (!response.accepted) {
                response.virtualPort = 0;
                response.peerPort = 0;
            }
            sendResponse(source, response);
        }

        for (size_t targetIndex = 0; targetIndex < participants.size(); ++targetIndex) {
            Participant& target = participants[targetIndex];
            if (!target.active) continue;
            for (int packetIndex = 0; packetIndex < 32; ++packetIndex) {
                sockaddr_in packetSource{};
                int packetSize = 0;
                if (!receiveDatagram(target.socket, buffer, sizeof(buffer), packetSource, packetSize)) {
                    if (!wouldBlock()) retireParticipant(targetIndex);
                    break;
                }
                // The relay deliberately cannot inspect the end-to-end gameplay
                // HMAC. Limit each virtual destination socket before forwarding
                // opaque datagrams so a valid client cannot starve its room.
                if (!allowRate(target.rateWindow, now, static_cast<size_t>(packetSize),
                        PARTICIPANT_PACKETS_PER_SECOND, PARTICIPANT_BYTES_PER_SECOND)) {
                    ++rateLimitedParticipantPackets;
                    continue;
                }
                if (static_cast<size_t>(packetSize) > MAX_PACKET_BYTES) continue;
                network::PacketHeader packetHeader;
                if (!network::decodeHeader(buffer, static_cast<size_t>(packetSize), packetHeader)) continue;
                const size_t senderIndex = findParticipant(packetSource, target.room);
                if (senderIndex == NO_PARTICIPANT || senderIndex == targetIndex) continue;
                Participant& sender = participants[senderIndex];
                bool validDirection = false;
                sockaddr_in destination{};
                if (target.host && !sender.host) {
                    validDirection = true;
                    destination = target.physical;
                } else if (!target.host && sender.host) {
                    validDirection = true;
                    destination = target.physical;
                }
                if (!validDirection) continue;
                if (sendDatagram(sender.socket, buffer, static_cast<size_t>(packetSize), destination)) {
                    target.lastActivity = now;
                    sender.lastActivity = now;
                }
            }
        }

        for (size_t index = 0; index < participants.size(); ++index) {
            if (participants[index].active
                    && now - participants[index].lastActivity
                        > std::chrono::milliseconds(options.participantTimeoutMs)) {
                retireParticipant(index);
                ++expiredParticipants;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    for (Participant& participant : participants) {
        if (participant.socket != INVALID_RELAY_SOCKET) closeSocket(participant.socket);
    }
    closeSocket(controlSocket);
#if defined(_WIN32)
    WSACleanup();
#endif
    std::cout << "quantum-network-relay: rate-limited control=" << rateLimitedControlPackets
        << " participant=" << rateLimitedParticipantPackets
        << " heartbeats=" << heartbeatPackets
        << " rejected-heartbeats=" << rejectedHeartbeats
        << " expired-participants=" << expiredParticipants
        << " rejected-rooms=" << rejectedRooms << "\n";
    std::cout << "quantum-network-relay: stopped\n";
    return 0;
}
