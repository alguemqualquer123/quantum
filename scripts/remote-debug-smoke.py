#!/usr/bin/env python3
# (c) SR VINIX
# SPDX-License-Identifier: MIT
"""Exercise the standalone Quantum live Lua debugger over loopback TCP."""

from __future__ import annotations

import argparse
import json
import queue
import re
import socket
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path


PORT_RE = re.compile(r"remote Lua debugger listening on 127\.0\.0\.1:(\d+)")


def read_output(process: subprocess.Popen[str], lines: queue.Queue[str]) -> None:
    assert process.stdout is not None
    for line in process.stdout:
        lines.put(line.rstrip("\r\n"))


def wait_for_port(process: subprocess.Popen[str], lines: queue.Queue[str], timeout: float) -> int:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"quantum-engine exited with {process.returncode} before opening debugger")
        try:
            line = lines.get(timeout=0.05)
        except queue.Empty:
            continue
        match = PORT_RE.search(line)
        if match:
            return int(match.group(1))
    raise RuntimeError("timed out waiting for the remote debugger port")


def connect(port: int, timeout: float) -> tuple[socket.socket, object]:
    deadline = time.monotonic() + timeout
    last_error: OSError | None = None
    while time.monotonic() < deadline:
        try:
            sock = socket.create_connection(("127.0.0.1", port), timeout=timeout)
            sock.settimeout(timeout)
            return sock, sock.makefile("rwb")
        except OSError as error:
            last_error = error
            time.sleep(0.01)
    raise RuntimeError(f"could not connect to debugger: {last_error}")


def request(stream: object, command: str) -> dict[str, object]:
    stream.write((command + "\n").encode("utf-8"))
    stream.flush()
    line = stream.readline()
    if not line:
        raise RuntimeError(f"debugger closed while handling {command!r}")
    response = json.loads(line.decode("utf-8"))
    if not isinstance(response, dict):
        raise RuntimeError(f"debugger response is not an object: {response!r}")
    return response


def make_project(root: Path) -> Path:
    project = root / "project.yaml"
    lua_dir = root / "lua"
    lua_dir.mkdir(parents=True)
    project.write_text(
        "# (c) SR VINIX\n# SPDX-License-Identifier: MIT\n"
        "name: Quantum Remote Debug Probe\ncanvasWidth: 320\ncanvasHeight: 180\n"
        "luaDir: lua\nassetsDir: assets\nvsync: false\n",
        encoding="utf-8",
    )
    (lua_dir / "main.lua").write_text(
        "-- (c) SR VINIX\n-- SPDX-License-Identifier: MIT\n"
        "local counter = 0\n\n"
        "Engine.onUpdate = function()\n"
        "    local current = counter + 1\n"
        "    counter = current\n"
        "    if current == 10000 and not _G.__remoteRaised then\n"
        "        _G.__remoteRaised = true\n"
        "        error('expected remote debugger error')\n"
        "    end\n"
        "end\n",
        encoding="utf-8",
    )
    return project


def resolve_executable(value: str | None) -> Path:
    if value:
        return Path(value).resolve()
    candidates = (
        Path("build/Release/quantum-engine.exe"),
        Path("build/Release/quantum-engine"),
        Path("build/quantum-engine.exe"),
        Path("build/quantum-engine"),
    )
    for candidate in candidates:
        if candidate.is_file():
            return candidate.resolve()
    raise RuntimeError("quantum-engine executable not found; pass --executable")


def run(executable: Path, timeout: float) -> None:
    token = "remote-debug-smoke-token"
    process: subprocess.Popen[str] | None = None
    clients: list[tuple[socket.socket, object]] = []
    try:
        with tempfile.TemporaryDirectory(prefix="quantum-remote-debug-") as temporary:
            project = make_project(Path(temporary))
            process = subprocess.Popen(
                [
                    str(executable),
                    "--project",
                    str(project.parent),
                    "--headless",
                    "--frames",
                    "20000",
                    "--warmup",
                    "0",
                    "--debug-port",
                    "0",
                    "--debug-token",
                    token,
                    "--log-level",
                    "1",
                ],
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                bufsize=1,
            )
            output: queue.Queue[str] = queue.Queue()
            threading.Thread(target=read_output, args=(process, output), daemon=True).start()
            port = wait_for_port(process, output, timeout)

            wrong_socket, wrong_stream = connect(port, timeout)
            clients.append((wrong_socket, wrong_stream))
            hello = json.loads(wrong_stream.readline().decode("utf-8"))
            if hello.get("authRequired") is not True:
                raise RuntimeError(f"debugger did not require auth: {hello!r}")
            auth_error = request(wrong_stream, "auth wrong")
            if "authentication failed" not in str(auth_error.get("message", "")):
                raise RuntimeError(f"wrong token was accepted: {auth_error!r}")
            wrong_stream.close()
            wrong_socket.close()
            clients.pop()

            sock, stream = connect(port, timeout)
            clients.append((sock, stream))
            stream.readline()  # hello
            ready = request(stream, f"auth {token}")
            if ready.get("event") != "ready":
                raise RuntimeError(f"debugger did not authenticate: {ready!r}")
            identified_status = request(stream, "@probe-42 status")
            if identified_status.get("requestId") != "probe-42":
                raise RuntimeError(f"request id was not echoed: {identified_status!r}")
            errors_off = request(stream, "@filter-off errors off")
            if errors_off.get("event") != "errorFilter" or errors_off.get("requestId") != "filter-off" \
                    or errors_off.get("stopOnError") is not False:
                raise RuntimeError(f"error filter could not be disabled: {errors_off!r}")
            errors_on = request(stream, "@filter-on errors on")
            if errors_on.get("event") != "errorFilter" or errors_on.get("requestId") != "filter-on" \
                    or errors_on.get("stopOnError") is not True:
                raise RuntimeError(f"error filter could not be enabled: {errors_on!r}")
            callback_only = request(stream, "@filter-callback errors callback")
            if callback_only.get("event") != "errorFilter" \
                    or callback_only.get("errorFilter") != "callback" \
                    or callback_only.get("errorMask") != 2:
                raise RuntimeError(f"callback error filter was not selected: {callback_only!r}")
            all_categories = request(stream, "@filter-all errors all")
            if all_categories.get("errorFilter") != "runtime,callback,syntax" \
                    or all_categories.get("errorMask") != 7:
                raise RuntimeError(f"all error categories were not selected: {all_categories!r}")
            runtime_only = request(stream, "@filter-runtime errors runtime")
            if runtime_only.get("errorFilter") != "runtime" or runtime_only.get("errorMask") != 1:
                raise RuntimeError(f"runtime error filter was not selected: {runtime_only!r}")
            breakpoint = request(stream, "break main.lua:7 if current >= 1")
            if breakpoint.get("event") != "breakpoint":
                raise RuntimeError(f"breakpoint was rejected: {breakpoint!r}")

            paused: dict[str, object] | None = None
            for _ in range(300):
                status = request(stream, "status")
                if status.get("state") == "paused":
                    paused = status
                    break
                time.sleep(0.005)
            if paused is None:
                raise RuntimeError("conditional breakpoint did not pause the live VM")

            current = request(stream, "watch current")
            counter = request(stream, "watch counter")
            for name, response in (("current", current), ("counter", counter)):
                if response.get("event") != "watch" or response.get("ok") is not True:
                    raise RuntimeError(f"{name} watch failed: {response!r}")
                if response.get("value") in (None, "nil"):
                    raise RuntimeError(f"{name} watch lost its paused lexical context: {response!r}")

            step = request(stream, "next")
            cleared = request(stream, "clear")
            continued = request(stream, "continue")
            if step.get("command") != "next" or cleared.get("cleared") is not True:
                raise RuntimeError(f"step/clear failed: {step!r} {cleared!r}")
            if continued.get("command") != "continue":
                raise RuntimeError(f"continue failed: {continued!r}")

            runtime_error: dict[str, object] | None = None
            for index in range(300):
                status = request(stream, f"@error-status-{index} status")
                if status.get("state") == "paused" and status.get("stopReason") == "error":
                    runtime_error = status
                    break
                time.sleep(0.005)
            if runtime_error is None:
                raise RuntimeError("runtime Lua error did not pause the debugger")
            if "expected remote debugger error" not in str(runtime_error.get("diagnostic", "")):
                raise RuntimeError(f"runtime error diagnostic was not captured: {runtime_error!r}")
            resumed_after_error = request(stream, "@error-continue continue")
            if resumed_after_error.get("command") != "continue" or resumed_after_error.get("requestId") != "error-continue":
                raise RuntimeError(f"debugger did not resume after the runtime error: {resumed_after_error!r}")

            stream.close()
            sock.close()
            clients.pop()
            try:
                exit_code = process.wait(timeout=5)
            except subprocess.TimeoutExpired as error:
                process.terminate()
                process.wait(timeout=2)
                raise RuntimeError("quantum-engine did not exit after the debugger resumed") from error
            if exit_code != 0:
                raise RuntimeError(f"quantum-engine exited with {exit_code}")
            print(
                "remote-debug-smoke: passed "
                f"port={port} pausedLine={paused.get('line')} "
                f"current={current.get('value')} counter={counter.get('value')}"
            )
    finally:
        for sock, stream in clients:
            try:
                stream.close()
            finally:
                sock.close()
        if process is not None and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", help="path to quantum-engine")
    parser.add_argument("--timeout", type=float, default=10.0)
    arguments = parser.parse_args()
    try:
        run(resolve_executable(arguments.executable), arguments.timeout)
    except (OSError, RuntimeError, json.JSONDecodeError) as error:
        print(f"remote-debug-smoke: FAILED: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
