// (c) SR VINIX
// SPDX-License-Identifier: MIT

// VoxelNet.h - the multiplayer layer of the voxel world: one host that is authoritative for
// block edits, and up to 8 clients on the same LAN.
//
// Scope and honest limitations:
//   - Raw Winsock2 TCP, Windows only. On other platforms every entry point is a no-op and
//     netError() reports "networking not supported", so a build there still links and runs
//     single player.
//   - No encryption and no authentication: a client is trusted only as far as the protocol
//     version check and the host's block-edit validation go. This is a LAN toy, not an
//     internet service.
//   - One TCP connection per peer, non-blocking, drained from tickVoxelNet() on the main
//     thread. A peer that stalls for a few seconds is dropped rather than stalling the frame.
//   - Host-authoritative: a client predicts its own edit, the host validates it and either
//     accepts and broadcasts it or replies with the authoritative block (see SET/EDIT in the
//     protocol table at the top of VoxelNet.cpp).

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace voxel {

class VoxelWorld;

enum class NetMode : uint8_t {
    OFFLINE,
    HOST,
    CLIENT,
};

// Starts a mode. HOST listens on `port` and is authoritative for block edits; CLIENT connects
// to "host:port" and sends edit requests, receiving authoritative state. hostAddress may also
// carry its own port ("localhost:25565"), which then wins over `port`.
// Returns false on a socket error (callers print netError()).
bool startVoxelNet(voxel::VoxelWorld& world, NetMode mode, int port, const std::string& hostAddress);

// Sends BYE to every peer, closes the sockets and releases the Winsock library.
void stopVoxelNet();

// Call once per frame on the main thread. Host: accept, read, apply edits, broadcast; also
// pushes chunk columns to clients that entered the view radius. Client: read, apply remote
// edits, send own edits + player position.
void tickVoxelNet(voxel::VoxelWorld& world, float dt, float playerX, float playerY,
                  float playerZ, float yaw);

// The other players, for the HUD/remote avatars: id, name, position, yaw.
struct RemotePlayer {
    int id;
    std::string name;
    float x, y, z, yaw;
};

const std::vector<RemotePlayer>& remotePlayers();

// "Net: host 12345, 2 clients, 12 edits/s, 40 KB/s" (or the last socket error) for the overlay.
std::string netStats();

// True once the session is usable: a host that is listening, or a client that has been welcomed
// and greeted. A caller that wants to script an edit must wait for this, because the client's
// journal is only drained while linked — an edit written before the handshake is dropped.
bool netReady();

// The last socket/protocol error, empty when none. Points at stable storage until the next
// error, so a HUD can keep the pointer for one frame only.
const char* netError();

}  // namespace voxel
