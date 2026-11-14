# (c) SR VINIX
# SPDX-License-Identifier: MIT
"""Exercise the shipped UDP relay with two real headless engine processes."""

from __future__ import annotations

import argparse
import hashlib
import hmac
import os
from pathlib import Path
import re
import socket
import struct
import subprocess
import sys
import tempfile
import time


def free_udp_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.bind(("127.0.0.1", 0))
        return int(probe.getsockname()[1])


def executable(build_root: Path, config: str, name: str) -> Path:
    suffix = ".exe" if os.name == "nt" else ""
    configured = build_root / config / f"{name}{suffix}"
    if configured.exists():
        return configured
    fallback = build_root / f"{name}{suffix}"
    if fallback.exists():
        return fallback
    raise RuntimeError(f"missing executable: {configured}")


def stop_process(process: subprocess.Popen[str] | None) -> None:
    if process is None or process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=3)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=3)


def read_log(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace") if path.exists() else ""


PROTOCOL_MAGIC = 0x514E4554
PROTOCOL_VERSION = 1
RELAY_REGISTER = 14
RELAY_JOIN = 15
RELAY_READY = 16
RELAY_HEARTBEAT = 17


def relay_request(packet_type: int, nonce: int, sequence: int, room: str) -> bytes:
    room_bytes = room.encode("utf-8")
    payload = struct.pack(">IHH", nonce, len(room_bytes), 0) + room_bytes
    return struct.pack(">IHHII", PROTOCOL_MAGIC, PROTOCOL_VERSION, packet_type,
                       len(payload), sequence) + payload


def authenticated(packet: bytes, token: str) -> bytes:
    tag = hmac.new(token.encode("utf-8"), packet, hashlib.sha256).digest()
    return packet + tag


def decode_relay_response(packet: bytes, token: str) -> tuple[int, int, int, bool] | None:
    tag_size = hashlib.sha256().digest_size
    if len(packet) < 16 + 12 + tag_size:
        return None
    body, tag = packet[:-tag_size], packet[-tag_size:]
    expected = hmac.new(token.encode("utf-8"), body, hashlib.sha256).digest()
    if not hmac.compare_digest(tag, expected):
        return None
    magic, version, packet_type, payload_size, _sequence = struct.unpack(">IHHII", body[:16])
    if (magic != PROTOCOL_MAGIC or version != PROTOCOL_VERSION
            or packet_type != RELAY_READY or payload_size != 12
            or len(body) != 16 + payload_size):
        return None
    nonce, virtual_port, peer_port, accepted, _reserved = struct.unpack(">IHHHH", body[16:])
    return nonce, virtual_port, peer_port, accepted != 0


def receive_relay_response(sock: socket.socket, token: str, nonce: int) -> tuple[int, int, int, bool]:
    deadline = time.monotonic() + 2.0
    while time.monotonic() < deadline:
        remaining = max(0.05, deadline - time.monotonic())
        sock.settimeout(remaining)
        try:
            response, _source = sock.recvfrom(2048)
        except socket.timeout:
            continue
        decoded = decode_relay_response(response, token)
        if decoded is not None and decoded[0] == nonce:
            return decoded
    raise RuntimeError("relay did not answer the authenticated control request")


def run_failover(relay_exe: Path, root: Path) -> int:
    """Exercise host expiry with raw authenticated control packets.

    The normal mode above uses three real engines. This focused mode keeps the
    client lease alive with the same RelayHeartbeat packet that NetworkSystem
    emits, so the test isolates room ownership transfer from gameplay timeout.
    """
    relay_port = free_udp_port()
    room = f"relay-failover-{os.getpid()}"
    relay = None
    host = client = replacement = None
    with tempfile.TemporaryDirectory(prefix="quantum-relay-failover-") as temp:
        relay_log = Path(temp) / "relay.log"
        relay_file = relay_log.open("w", encoding="utf-8")
        try:
            relay = subprocess.Popen(
                [str(relay_exe), "--port", str(relay_port), "--token", "relay-key",
                 "--duration-ms", "12000", "--participant-timeout-ms", "3000"],
                stdout=relay_file, stderr=subprocess.STDOUT, text=True, cwd=root,
            )
            time.sleep(0.25)
            host = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            client = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            nonce = os.getpid() & 0xFFFFFFFF
            sequence = 1
            host.sendto(authenticated(relay_request(RELAY_REGISTER, nonce, sequence, room),
                                      "relay-key"), ("127.0.0.1", relay_port))
            host_response = receive_relay_response(host, "relay-key", nonce)
            if not host_response[3]:
                raise RuntimeError("initial host registration was rejected")
            sequence += 1
            client_nonce = (nonce + 1) & 0xFFFFFFFF
            client.sendto(authenticated(relay_request(RELAY_JOIN, client_nonce, sequence, room),
                                        "relay-key"), ("127.0.0.1", relay_port))
            client_response = receive_relay_response(client, "relay-key", client_nonce)
            if not client_response[3] or client_response[2] != host_response[1]:
                raise RuntimeError("client did not join the initial host")

            host.close()
            host = None
            expiry_deadline = time.monotonic() + 4.5
            while time.monotonic() < expiry_deadline:
                sequence += 1
                client.sendto(authenticated(relay_request(RELAY_HEARTBEAT, client_nonce,
                                                          sequence, room), "relay-key"),
                              ("127.0.0.1", relay_port))
                receive_relay_response(client, "relay-key", client_nonce)
                time.sleep(0.5)

            replacement = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            replacement_nonce = (nonce + 2) & 0xFFFFFFFF
            sequence += 1
            replacement.sendto(authenticated(relay_request(RELAY_REGISTER, replacement_nonce,
                                                            sequence, room), "relay-key"),
                               ("127.0.0.1", relay_port))
            replacement_response = receive_relay_response(replacement, "relay-key",
                                                          replacement_nonce)
            if not replacement_response[3]:
                raise RuntimeError("replacement host registration was rejected")
            for _ in range(4):
                sequence += 1
                client.sendto(authenticated(relay_request(RELAY_HEARTBEAT, client_nonce,
                                                          sequence, room), "relay-key"),
                              ("127.0.0.1", relay_port))
                receive_relay_response(client, "relay-key", client_nonce)
                time.sleep(0.25)
            relay_code = relay.wait(timeout=15)
        except (OSError, RuntimeError, subprocess.TimeoutExpired) as exc:
            stop_process(relay)
            print(f"network relay failover smoke failed: {exc}", file=sys.stderr)
            return 2
        finally:
            for sock in (replacement, client, host):
                if sock is not None:
                    sock.close()
            relay_file.close()

        log = read_log(relay_log)
        expired = re.search(r"expired-participants=(\d+)", log)
        heartbeat = re.search(r"heartbeats=(\d+)", log)
        if (relay_code != 0 or log.count("registered room=") < 2
                or not expired or int(expired.group(1)) < 1
                or not heartbeat or int(heartbeat.group(1)) < 1):
            print("network relay failover smoke failed", file=sys.stderr)
            print(log, file=sys.stderr, end="")
            return 1
    print(f"network relay smoke: PASS room={room} port={relay_port} failover=PASS")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path("."))
    parser.add_argument("--config", default="Release")
    parser.add_argument("--engine-executable", type=Path,
                        help="Use an explicit quantum-engine executable instead of build/<config>.")
    parser.add_argument("--relay-executable", type=Path,
                        help="Use an explicit quantum-network-relay executable instead of build/<config>.")
    parser.add_argument("--failover", action="store_true",
                        help="Expire the host and register a replacement in the same room.")
    args = parser.parse_args()

    root = args.root.resolve()
    build_root = root / "build"
    relay_exe = (args.relay_executable.resolve() if args.relay_executable
                 else executable(build_root, args.config, "quantum-network-relay"))
    if args.failover:
        return run_failover(relay_exe, root)
    engine_exe = (args.engine_executable.resolve() if args.engine_executable
                  else executable(build_root, args.config, "quantum-engine"))
    project = root / "tests" / "network" / "relay_fixture"
    relay_port = free_udp_port()
    room = f"relay-smoke-{os.getpid()}"

    relay = server = client_one = client_two = None
    with tempfile.TemporaryDirectory(prefix="quantum-relay-smoke-") as temp:
        temp_path = Path(temp)
        relay_log = temp_path / "relay.log"
        server_log = temp_path / "server.log"
        client_one_log = temp_path / "client-one.log"
        client_two_log = temp_path / "client-two.log"
        handles = []
        try:
            relay_file = relay_log.open("w", encoding="utf-8")
            handles.append(relay_file)
            relay = subprocess.Popen(
                [str(relay_exe), "--port", str(relay_port), "--token", "relay-key",
                 "--duration-ms", "12000"],
                stdout=relay_file, stderr=subprocess.STDOUT, text=True,
                cwd=root,
            )
            time.sleep(0.25)

            server_file = server_log.open("w", encoding="utf-8")
            handles.append(server_file)
            common = [
                str(engine_exe), "--project", str(project), "--scene", "main",
                "--headless", "--frames", "3000000", "--warmup", "0",
                "--network-token", "game-key", "--network-relay-token", "relay-key",
            ]
            server = subprocess.Popen(
                common + ["--network-relay-server", "127.0.0.1", str(relay_port), room],
                stdout=server_file, stderr=subprocess.STDOUT, text=True, cwd=root,
            )
            time.sleep(0.25)

            client_one_file = client_one_log.open("w", encoding="utf-8")
            handles.append(client_one_file)
            client_one = subprocess.Popen(
                common + ["--network-relay-client", "127.0.0.1", str(relay_port), room],
                stdout=client_one_file, stderr=subprocess.STDOUT, text=True, cwd=root,
            )
            time.sleep(0.25)

            client_two_file = client_two_log.open("w", encoding="utf-8")
            handles.append(client_two_file)
            client_two = subprocess.Popen(
                common + ["--network-relay-client", "127.0.0.1", str(relay_port), room],
                stdout=client_two_file, stderr=subprocess.STDOUT, text=True, cwd=root,
            )
            # Exercise the relay's flood budget after the valid room members
            # are connected. Oversized packets must be counted and dropped
            # without interrupting the established gameplay handshakes.
            time.sleep(0.5)
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as flood:
                flood.settimeout(1.0)
                for index in range(8192):
                    flood.sendto(b"x" * 1500, ("127.0.0.1", relay_port))
                    if index % 10 == 0:
                        time.sleep(0.001)
            server_code = server.wait(timeout=20)
            client_one_code = client_one.wait(timeout=20)
            client_two_code = client_two.wait(timeout=20)
            try:
                relay_code = relay.wait(timeout=12)
            except subprocess.TimeoutExpired:
                stop_process(relay)
                relay_code = relay.returncode
        except (OSError, subprocess.TimeoutExpired) as exc:
            stop_process(client_two)
            stop_process(client_one)
            stop_process(server)
            stop_process(relay)
            print(f"network relay smoke failed: {exc}", file=sys.stderr)
            return 2
        finally:
            for handle in handles:
                handle.close()

        logs = {
            "relay": read_log(relay_log),
            "server": read_log(server_log),
            "client-one": read_log(client_one_log),
            "client-two": read_log(client_two_log),
        }
        required = {
            "relay": ("registered room=", "joined room=", "joined room=",
                      "rate-limited control="),
            "server": ("Relay room registered", "Handshake accepted for peer 1",
                       "Handshake accepted for peer 2"),
            "client-one": ("Relay room joined", "Handshake completed"),
            "client-two": ("Relay room joined", "Handshake completed"),
        }
        missing = []
        if logs["relay"].count("joined room=") < 2:
            missing.append("relay:two joined room markers")
        rate_match = re.search(r"rate-limited control=(\d+)", logs["relay"])
        if not rate_match or int(rate_match.group(1)) == 0:
            missing.append("relay:effective control rate limit")
        heartbeat_match = re.search(r"heartbeats=(\d+)", logs["relay"])
        if not heartbeat_match or int(heartbeat_match.group(1)) == 0:
            missing.append("relay:authenticated heartbeat")
        for name, markers in required.items():
            for marker in markers:
                if marker == "joined room=":
                    continue
                if marker not in logs[name]:
                    missing.append(f"{name}:{marker}")
        if (server_code != 0 or client_one_code != 0 or client_two_code != 0
                or relay_code not in (0, None) or missing):
            print("network relay smoke failed", file=sys.stderr)
            print(f"exit codes: relay={relay_code} server={server_code} "
                  f"client-one={client_one_code} client-two={client_two_code}", file=sys.stderr)
            if missing:
                print("missing markers: " + ", ".join(missing), file=sys.stderr)
            for name, content in logs.items():
                print(f"--- {name} ---", file=sys.stderr)
                print(content, file=sys.stderr, end="")
            return 1

    rate_line = next((line.strip() for line in logs["relay"].splitlines()
                      if "rate-limited control=" in line), "rate-limited control=unknown")
    print(f"network relay smoke: PASS room={room} port={relay_port} {rate_line}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
