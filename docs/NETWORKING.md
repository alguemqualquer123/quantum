// (c) SR VINIX
// SPDX-License-Identifier: MIT

# Quantum networking

Quantum networking uses a versioned UDP protocol shared by `NetworkProtocol` and
`NetworkSystem`. The current wire version remains `1`.

## Protocol

Every datagram starts with the existing 16-byte header:

| Field | Size | Meaning |
|---|---:|---|
| magic | 4 | `0x514E4554` (`QNET`) |
| version | 2 | `1` |
| type | 2 | `Hello`, `HelloAck`, `Data`, `Ack`, `Snapshot`, `Unreliable`, `SnapshotCompressed`, `InterestUpdate`, `ReplicationPolicy`, `LobbyQuery`, `LobbyAdvertisement`, `LobbyReservationRequest`, `LobbyReservationResponse`, `RelayRegister`, `RelayJoin` or `RelayReady` |
| payload size | 4 | Big-endian payload bytes |
| sequence | 4 | Channel sequence |

The new `InterestUpdate` packet is additive and keeps the version and all
existing packet layouts unchanged. Its reliable payload contains three floats
for the area center, one float radius and one `uint32` interest group.

The server authenticates each UDP endpoint accepted by the handshake and keeps
an independent bounded session for every peer (default maximum: 16; configure
with `NetworkSystem:setMaxPeers`). `getConnectedPeerCount()` exposes the active
count. Each accepted peer receives a distinct non-zero `peerId`; datagrams from
unknown endpoints are rejected. Reliable retry windows, ordering, ACKs and
interest centers are isolated per peer, while server application messages and
authoritative snapshots are broadcast only to currently connected peers.
A client cannot author snapshots: snapshots are accepted only in client mode,
while the server is the only peer that emits authoritative snapshots.
The authenticated `HelloAck` also carries a non-zero session `peerId` assigned
by the server. `NetworkSystem:getLocalPeerId()` exposes it to C++/Lua; the
server reports `0`, and the client returns `0` again while disconnected. This
identity is the stable input for gameplay authority and lobby membership.

The runtime also provides bounded LAN room discovery. A server answers
`LobbyQuery` on its game UDP port without creating a peer session or consuming a
peer slot. `LobbyAdvertisement` returns the server name, map, build ID, game
port, current peer count and maximum peer count. The response uses the same
HMAC token as the gameplay transport when one is configured, so unsigned or
wrong-token queries do not reveal room metadata. C++ callers can use
`NetworkSystem::discoverLan(port, timeoutMs, token)`; it broadcasts by default,
or accepts an explicit IPv4 address for deterministic tests and local tools.
An authenticated client can reserve one slot for a short period with
`NetworkSystem::reserveLan(port, timeoutMs, token, address, seconds)`. The server
counts unexpired reservations against `maxPeers`, returns a reservation ID, and
accepts that ID on the client's first `Hello`; reservations expire automatically
and do not create a peer session. Retries with the same request nonce are
idempotent, while different clients behind one NAT can hold distinct reservations.
An expired or unknown reservation ID is rejected during `Hello`; the client must
reserve again rather than silently falling back to an unreserved admission.
Internet matchmaking and ICE/STUN-based hole punching still belong to the game
or an external service. Quantum also ships an authenticated UDP relay for the
dedicated-server/NAT fallback path. The relay authenticates room control and
forwards opaque QNET datagrams; gameplay HMAC remains end-to-end between the
server and clients.

Start the relay with a deployment-only secret, then launch one authoritative
peer and clients with the same room code:

```powershell
build\Release\quantum-network-relay.exe --port 39279 `
    --token $env:QUANTUM_RELAY_TOKEN
quantum-engine.exe --project <project> --network-relay-server 127.0.0.1 39279 room-a `
    --network-token-env QUANTUM_NETWORK_TOKEN `
    --network-relay-token-env QUANTUM_RELAY_TOKEN
quantum-engine.exe --project <project> --network-relay-client 127.0.0.1 39279 room-a `
    --network-token-env QUANTUM_NETWORK_TOKEN `
    --network-relay-token-env QUANTUM_RELAY_TOKEN
```

The relay allocates one virtual UDP port per participant, bounds rooms and
participants, expires idle registrations and rejects wrong-token control
packets. Expired host/member slots are removed from room membership and safely
reused; a new authenticated `RelayRegister` can take over a room after the host
lease expires, while existing clients receive the new virtual peer port on their
next heartbeat. It also rate-limits the authenticated control socket to 2,400 packets
or 256 KiB per second and each virtual destination to 1,200 packets or 2 MiB per
second, counting oversized datagrams before rejecting them. After the initial
register/join, each runtime sends an authenticated `RelayHeartbeat` every ten
seconds so an idle participant remains alive without gameplay traffic. The
relay reports accepted and rejected heartbeats in its shutdown counters. It is installed by
CMake/CPack as `quantum-network-relay`; use a region-local hosted instance in
production. ICE/STUN/TURN, hosted matchmaking and multi-region failover remain
deployment services around it.

## Optional datagram authentication

Production deployments can configure the same pre-shared token on both peers:

```lua
network:setAuthToken("a-secret-kept-out-of-source-control")
```

or, for the headless runner:

```powershell
$env:QUANTUM_NETWORK_TOKEN = "a-secret-kept-out-of-source-control"
quantum-engine.exe --project <project> --network-server 39179 \
    --network-token-env QUANTUM_NETWORK_TOKEN
quantum-engine.exe --project <project> --network-client 127.0.0.1 39179 \
    --network-token-env QUANTUM_NETWORK_TOKEN
```

The token is used as the HMAC-SHA256 key, implemented with the engine's existing
`engine/core/util/Checksum.*` SHA-256 code. The HMAC covers the complete existing
datagram (header plus declared payload), and its 32-byte tag is appended as a
trailer. The header's payload size does not include the trailer. Verification is
performed before decoding or accepting a handshake, and the tag comparison is
constant-time. Invalid or unsigned datagrams increment
`NetworkSystem::Metrics::unauthorizedPackets` and do not refresh the peer timeout.
The legacy `--network-token TOKEN` option remains available for local scripts, but
the editor and production launchers use the environment form so the secret does
not appear in process command lines.

An empty token is the compatibility mode: no trailer is added, packet sizes and
all version-1 wire layouts remain exactly as before. `setAuthToken` should be
called before `startServer`/`startClient`; changing it while running is thread-safe
but only affects subsequent datagrams, so both peers should switch at a planned
reconnect boundary. The token is never printed by the runtime.

## Replication policy

`NetworkComponent` keeps the existing `netId` and `replicateTransform` fields and
adds optional policy fields:

- `ownerId`: logical owner; `0` means server-owned.
- `interestGroup`: non-zero entities are filtered against the client's group.
- `interestRadius`: additional world-space radius around the client's area of
  interest; `0` disables per-entity distance filtering.
- `serverAuthority`: documents that the server owns the authoritative state.

Clients can publish their area with `NetworkSystem:setInterestCenter(center,
radius, group)`. The server filters snapshots without changing the snapshot
wire format. Calling `clearInterestCenter()` restores an unrestricted view.
Lua gameplay can configure an entity with
`NetworkSystem:setEntityReplicationPolicy(entity, ownerId, group, radius,
serverAuthority)`.

During live replication, the server sends an additive `ReplicationPolicy`
packet beside the transform snapshot. The client applies it on the main thread,
so `hasEntityAuthority` can use the server-assigned owner without gameplay
manually copying policy fields. Peers that do not understand packet type 9 can
continue consuming the legacy transform snapshots.

The complete policy (`netId`, `replicateTransform`, `ownerId`,
`interestGroup`, `interestRadius` and `serverAuthority`) is persisted by the
editor's YAML stream and by the `QNETECS3` SaveGame serializer. The latter uses
schema 2 for new snapshots and accepts schema 1 for backward compatibility;
legacy snapshots keep their transform replication flag and use the component's
safe defaults for fields that did not exist yet.

The built-in component serializers expose a read-only deterministic catalog through
`NetworkSystem::getReplicatedComponentCatalog()`. It contains only names and schema
versions, never encode/decode callbacks, so tooling can audit registrations without
coupling to serializer internals. The MSVC `quantum-serialization-smoke` validates
that the catalog has unique names and non-zero schemas alongside the `QNETECS3`
round-trip.

For ECS diagnostics, `EntityRegistry::getRegisteredComponentCatalog()` exposes the
component names and signature IDs generated by `registerComponent<T>()`. The same
MSVC smoke reports 59 unique component IDs, making registry drift visible without
maintaining a second name table.

## Abuse limits and metrics

The runtime rejects datagrams above 1400 bytes, limits a peer to 600 packets or
512 KiB per second, bounds reliable/unreliable/snapshot queues, and rejects
packets from unauthenticated endpoints. `NetworkSystem::Metrics` exposes RTT,
estimated retransmission loss, queue depths/high-water mark, bytes and packets,
malformed/unauthorized/rate-limited counts, retransmissions and simulated loss
or reordering.

Game-specific reliable commands can be registered with
`NetworkSystem::registerServerMessageSchema`. The server then migrates the
versioned envelope, enforces its maximum payload size, and invokes a validator
with the authenticated `peerId` before queueing it for gameplay. Invalid
commands are ACKed to stop retransmission but are not delivered; legacy
unversioned payloads remain compatible.

`setPacketLossSimulationForTesting(percent)` and
`setPacketReorderingSimulationForTesting(percent)` are QA-only controls. They
delay/drop outgoing application datagrams and are not intended as gameplay
configuration.

## Headless verification

Build the smoke target and run the complete in-process suite:

```powershell
cmake --build build --target quantum-network-smoke --config Debug -- /m:1
build\Debug\quantum-network-smoke.exe
```

The smoke also supports two real `NetworkSystem` processes. Start the server
first, then a client in another terminal:

```powershell
Start-Process -FilePath build\Debug\quantum-network-smoke.exe -ArgumentList '--instance-server 39201 5000' -NoNewWindow
build\Debug\quantum-network-smoke.exe --instance-client 127.0.0.1 39201 5000
```

The client exits successfully only after observing the server handshake and an
authoritative snapshot. Neither command launches the editor.

The in-process smoke additionally checks the RFC 4231 HMAC-SHA256 vector, exact
legacy framing with an empty token, tamper/unsigned rejection, authenticated
handshake/data/snapshot delivery, and the unauthorized metric. Keep the token in
an environment variable or secret store; command-line arguments can be visible
to local process inspection.

The real relay path can be checked without opening the editor:

```powershell
python scripts/network-relay-smoke.py --root . --config Release
```

It starts `quantum-network-relay`, one headless server and two simultaneous
headless clients, then requires both room joins and two distinct server-side
gameplay handshakes.

To validate host lease expiry and takeover in the same room, use the deterministic
failover mode. It uses the same authenticated relay-control packets as
`NetworkSystem`, shortens only the relay lease for the test, drops the first host,
keeps the client heartbeat alive, and registers a replacement host:

```powershell
python scripts/network-relay-smoke.py --root . --config Release --failover
```

It also starts an authenticated server, discovers it over UDP loopback, checks
the advertised metadata and verifies that a wrong token returns no lobby. The
same query path can be used with the default broadcast address by omitting the
optional address argument.

For a longer production-like reconnect check, run the soak mode. It restarts the
server between cycles, queues reliable messages during the outage, and reports
retransmissions, simulated loss/reordering and the peer identity after each
reconnect. The harness keeps a margin above the 750 ms client timeout and the
asynchronous server startup, so the final HelloAck is observed instead of being
classified as a flaky test failure:

```powershell
build\Debug\quantum-network-smoke.exe --reconnect-soak 12000 3 20 35
```
