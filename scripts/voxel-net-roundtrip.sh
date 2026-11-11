#!/bin/sh
# (c) SR VINIX
# SPDX-License-Identifier: MIT

# VoxelNet round trip - proves the multiplayer of templates/voxelworld-main over two real processes.
#
# VoxelNet is host-authoritative TCP, so no single process can demonstrate the wire: a test inside one
# instance could only ever measure a function call. This starts the same VoxelWorld.exe twice on one
# machine, using the environment overrides of the scene properties (VOXEL_NET / VOXEL_PORT /
# VOXEL_ADDR / VOXEL_SAVEDIR) so the shared scene file carries no machine-specific state, and arms one
# scripted block edit per side (VOXEL_NETEDIT="dx dy dz id", relative to each player's eyes). Then it
# reads each peer's log for the effect, matched on the coordinates:
#
#   host writes at X Y Z   -> the client's log must say "client: SET applied at X Y Z: _ -> ID"
#   client writes at X Y Z -> the host's log must say "host: APPLIED edit from _ at X Y Z -> ID"
#
# The two edits are at different coordinates and different block ids, so a line from the wrong peer
# cannot satisfy the other assertion. VOXEL_NETLOG=1 is what turns the module's tracing on; without it
# VoxelNet prints nothing at all.
#
#   scripts/voxel-net-roundtrip.sh [client frames] [port]
#
# Prints "PROBE net: N passed, M failed" like the in-game probes and exits 1 on any failure. Requires
# the project to be built first:
#   cd templates/voxelworld-main && cmake --build .quantum/build-standalone --config Debug

set -u

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
PROJ="$ROOT/templates/voxelworld-main"
EXE="$PROJ/.quantum/build-standalone/VoxelWorld.exe"
FRAMES=${1:-240}
PORT=${2:-25777}

# The host has to outlive the client by the client's whole boot: the second process spends a few
# seconds in engine init before its first frame, and a host that closes during that window leaves the
# client with a socket that never completes. The margin is 600 frames (~9 s at 60 fps).
HOST_FRAMES=$((FRAMES + 600))

# One side places cobblestone four blocks ahead of the camera, the other oak planks two columns over
# at the same height. Both sit above the local surface (the spawn column's top is y=60, the eye is at
# y=62.6) and inside the frustum, so each frame shows the block the other side sent: the logs prove
# the wire, the pictures prove the receiving mesh was rebuilt.
HOST_EDIT="0 2 -4 4"
CLIENT_EDIT="2 2 -4 16"

RUN=${TMPDIR:-/tmp}/voxel-net-roundtrip
case "$RUN" in
  */voxel-net-roundtrip) ;;                                # only ever clean the directory this script owns
  *) echo "refusing to use run directory '$RUN'" >&2; exit 2 ;;
esac
rm -rf "$RUN"
mkdir -p "$RUN" || { echo "could not create '$RUN'" >&2; exit 2; }
WINRUN=$(cygpath -w "$RUN")
LOGH="$RUN/host.log"
LOGC="$RUN/client.log"

pass=0
fail=0
ok()  { pass=$((pass + 1)); printf '  PASS  %s\n' "$1"; }
no()  { fail=$((fail + 1)); printf '  FAIL  %s  [%s]\n' "$1" "$2"; }
has()  { grep -c -- "$1" "$2" 2>/dev/null | head -1; }     # count of matching lines
line() { grep -m1 -- "$1" "$2" 2>/dev/null; }              # first matching line, for failure evidence
check() { if [ "${2:-0}" -gt 0 ]; then ok "$1"; else no "$1" "$3"; fi; }

# The executable's stdout is block-buffered when redirected, so a log line is NOT a reliable signal
# that something happened just now. The listener is therefore confirmed at the kernel: netstat shows
# the port in LISTENING the moment listen() returns, whatever the log buffer is doing.
wait_listening() {
  _i=0
  while [ "$_i" -lt "$(($1 * 4))" ]; do
    if netstat -an 2>/dev/null | grep -q "TCP.*:$2 .*LISTENING"; then return 0; fi
    if ! kill -0 "$3" 2>/dev/null; then return 1; fi
    sleep 0.25
    _i=$((_i + 1))
  done
  return 2
}

[ -f "$EXE" ] || { echo "missing $EXE - build the project first" >&2; exit 2; }

printf 'voxel net round trip: host (%s frames) + client (%s frames) on 127.0.0.1:%s\n' \
       "$HOST_FRAMES" "$FRAMES" "$PORT"

# VOXEL_SHOT + VOXEL_FRAMES also closes the process once the screenshot lands, so each side has to
# produce a PNG: that is what makes this a rendering proof and not only a socket proof.
( cd "$PROJ" && VOXEL_NET=host VOXEL_PORT="$PORT" VOXEL_SAVEDIR="$WINRUN\\host" \
      VOXEL_NETLOG=1 VOXEL_NETEDIT="$HOST_EDIT" \
      VOXEL_SHOT="$WINRUN\\host.png" VOXEL_FRAMES="$HOST_FRAMES" \
      "$EXE" > "$LOGH" 2>&1 ) &
HOSTPID=$!

wait_listening 60 "$PORT" "$HOSTPID"; _rc=$?
if [ "$_rc" = 0 ]; then
  ok "the host binds and listens on the requested port"
else
  no "the host binds and listens on the requested port" "netstat never saw it (rc=$_rc)"
  kill "$HOSTPID" 2>/dev/null
  printf -- '--- host log tail\n'; tail -20 "$LOGH" 2>/dev/null
  printf 'PROBE net: %s passed, %s failed\n' "$pass" "$fail"
  exit 1
fi

( cd "$PROJ" && VOXEL_NET=client VOXEL_PORT="$PORT" VOXEL_ADDR=127.0.0.1 VOXEL_SAVEDIR="$WINRUN\\client" \
      VOXEL_NETLOG=1 VOXEL_NETEDIT="$CLIENT_EDIT" \
      VOXEL_SHOT="$WINRUN\\client.png" VOXEL_FRAMES="$FRAMES" \
      "$EXE" > "$LOGC" 2>&1 ) &
CLIENTPID=$!

# ------------------------------------------------------------------ wait for both to finish
for _p in $CLIENTPID $HOSTPID; do
  _i=0
  while kill -0 "$_p" 2>/dev/null && [ "$_i" -lt 480 ]; do sleep 0.5; _i=$((_i + 1)); done
  if kill -0 "$_p" 2>/dev/null; then
    printf '  (pid %s outlived four minutes - killing it)\n' "$_p"
    kill "$_p" 2>/dev/null
  fi
done
wait "$CLIENTPID"; CEXIT=$?
wait "$HOSTPID"; HEXIT=$?

# ------------------------------------------------------------------ the handshake
check "the client is welcomed by the host" "$(has 'NET client: welcomed as' "$LOGC")" \
      "$(line 'Net: client' "$LOGC")"
check "the host records the peer as linked" "$(has 'NET host: peer' "$LOGH")" \
      "$(line 'Net: host' "$LOGH")"

# ------------------------------------------------------------------ the two directions, matched
# "NET LOCAL EDIT x y z: was -> id". The first sed drops everything before the coordinates, the second
# collapses ": was -> id" down to the id, so the line reads "x y z id".
edit_of() { grep -m1 'NET LOCAL EDIT' "$1" 2>/dev/null | sed 's/.*NET LOCAL EDIT //; s/:.*-> / /'; }
set -- $(edit_of "$LOGH"); HX=${1:-}; HY=${2:-}; HZ=${3:-}; HID=${4:-}
set -- $(edit_of "$LOGC"); CX=${1:-}; CY=${2:-}; CZ=${3:-}; CID=${4:-}

check "the host wrote its scripted block" "$(has 'NET LOCAL EDIT' "$LOGH")" "no NET LOCAL EDIT in the host log"
check "the client wrote its scripted block" "$(has 'NET LOCAL EDIT' "$LOGC")" "no NET LOCAL EDIT in the client log"

if [ -n "$HX" ] && [ "$(has "client: SET applied at $HX $HY $HZ: .* -> $HID" "$LOGC")" -gt 0 ]; then
  ok "the client receives the host's block at the same coordinates"
else
  no "the client receives the host's block at the same coordinates" \
     "host wrote '$HX $HY $HZ -> $HID'; client has '$(line 'SET applied' "$LOGC")'"
fi

if [ -n "$CX" ] && [ "$(has "host: APPLIED edit from .* at $CX $CY $CZ -> id $CID" "$LOGH")" -gt 0 ]; then
  ok "the host accepts the client's edit at the same coordinates"
else
  no "the host accepts the client's edit at the same coordinates" \
     "client wrote '$CX $CY $CZ -> $CID'; host has '$(line 'REFUSED\|APPLIED' "$LOGH")'"
fi

# Host authority is only worth having if it refuses what it should. A clean run has nothing rejected:
# both edits are made against an identical world, so the optimistic `previous` matches.
_rej=$(has 'host: REFUSED' "$LOGH")
if [ "$_rej" = 0 ]; then ok "no edit was refused by the host's validation"; else
  no "no edit was refused by the host's validation" "$_rej refused: $(line 'host: REFUSED' "$LOGH")"
fi

# ------------------------------------------------------------------ the session, as the HUD sees it
# The overlay text is only ever written into the log by the HUD dump at capture time, and the host is
# asked to outlive the client, so its dump is necessarily taken after the peer left: it reads
# "0 clients" and there is no way to make it read otherwise without weakening the round trip above.
# What both sides do prove is that each one rendered its own net line at all.
check "the host's overlay carries its net line" "$(has "Net: host $PORT" "$LOGH")" "$(line 'Net: host' "$LOGH")"
check "the client's overlay reports the host as a live player" \
      "$(has "Net: client of 127.0.0.1:$PORT, 1 players" "$LOGC")" "$(line 'Net: client' "$LOGC")"
_mismatch=$(has 'SEED MISMATCH' "$LOGC")
if [ "$_mismatch" = 0 ]; then ok "both sides agree on the world seed"; else
  no "both sides agree on the world seed" "$(line 'SEED MISMATCH' "$LOGC")"
fi
# A peer whose process exits without a BYE is a reset, and the host outlives the client, so its final
# HUD line is written after that reset. Ordinary traffic must not be filed as a local failure.
_err=$(has 'last error' "$LOGH")
if [ "$_err" = 0 ]; then ok "a peer leaving is not recorded as a socket error"; else
  no "a peer leaving is not recorded as a socket error" "$(line 'last error' "$LOGH")"
fi
if [ "$CEXIT" = 0 ] && [ "$HEXIT" = 0 ]; then ok "both processes closed on their own"
else no "both processes closed on their own" "client exit $CEXIT, host exit $HEXIT"; fi

# ------------------------------------------------------------------ the pictures
for _side in host client; do
  _png="$RUN/$_side.png"
  if [ -s "$_png" ] && [ "$(wc -c < "$_png")" -gt 20000 ]; then ok "$_side captured a frame"
  else no "$_side captured a frame" "missing or tiny: $_png"; fi
done

printf -- '--- evidence\n'
grep -h -e 'NET host: listening' -e 'NET host: peer' -e 'NET client: welcomed' \
       -e 'NET LOCAL EDIT' -e 'NET host: APPLIED' -e 'NET client: SET applied' \
       "$LOGH" "$LOGC" 2>/dev/null | sed 's/^/  /'
printf '  logs:  %s\n         %s\n' "$LOGH" "$LOGC"
printf '  shots: %s/host.png  %s/client.png\n' "$RUN" "$RUN"

printf 'PROBE net: %s passed, %s failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ] || exit 1
exit 0
