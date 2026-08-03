#!/usr/bin/env bash
# CLI smoke tests for destination/soundfont rules and hybrid WAV render.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
player="${MPXADRV_BIN:-$root/build/mpxadrv}"
soundfont="${MPXADRV_TEST_SOUNDFONT:-$root/SoundFonts/Roland_SC-55.sf2}"
tmpdir="$(mktemp -d "${TMPDIR:-/tmp}/mpxadrv-cli.XXXXXX")"
trap 'rm -rf "$tmpdir"' EXIT

if [[ ! -x "$player" ]]; then
  echo "mpxadrv not found at $player" >&2
  exit 1
fi

# Minimal hybrid MDR: FM track 0 + MIDI track 16, no PDX.
python3 - "$tmpdir/hybrid.mdr" <<'PY'
import struct, sys
path = sys.argv[1]
title = b"CLI\x0d\x0a\x1a\x00"  # empty PDX
table = len(title)
data = bytearray(title + bytes(66))
tracks = [bytearray() for _ in range(32)]
# Track 0 must start with E0 FF; longer rest so mdxmini reports duration.
tracks[0] = bytearray([0xe0, 0xff] + [0x7f] * 8 + [0xf1, 0x00])
# Track 16: MIDI ch1, volume, one note, end
tracks[16] = bytearray([
    0xe0, 0x08, 0x80,
    0xfb, 0x0c,
    0x80, 0x40,
    0xf1, 0x00,
])
for i in range(32):
    if not tracks[i]:
        tracks[i] = bytearray([0xf1, 0x00])
offset = 66
for i, tr in enumerate(tracks):
    struct.pack_into(">H", data, table + 2 + i * 2, offset)
    data.extend(tr)
    offset += len(tr)
struct.pack_into(">H", data, table, offset)
open(path, "wb").write(data)
PY

fail() { echo "FAIL: $*" >&2; exit 1; }
pass() { echo "PASS: $*"; }

# Mutual exclusion: destination + soundfont
if "$player" play "$tmpdir/hybrid.mdr" --destination 0 --soundfont "$soundfont" \
    >/dev/null 2>"$tmpdir/err.txt"; then
  fail "expected destination+soundfont to be rejected"
fi
grep -q 'cannot be used together' "$tmpdir/err.txt" \
  || fail "missing mutual-exclusion message"
pass "destination and soundfont are mutually exclusive"

# Hybrid render requires SF2
if "$player" render "$tmpdir/hybrid.mdr" -o "$tmpdir/no_sf.wav" \
    >/dev/null 2>"$tmpdir/err.txt"; then
  fail "expected render without soundfont to be rejected"
fi
grep -qi 'soundfont' "$tmpdir/err.txt" \
  || fail "missing soundfont requirement message"
pass "hybrid render requires --soundfont"

if [[ ! -f "$soundfont" ]]; then
  echo "SKIP: hybrid WAV render (SoundFont not found at $soundfont)"
  exit 0
fi

"$player" render "$tmpdir/hybrid.mdr" -o "$tmpdir/out.wav" \
  --soundfont "$soundfont" -l 1 >/dev/null
[[ -s "$tmpdir/out.wav" ]] || fail "WAV output missing or empty"
python3 - "$tmpdir/out.wav" <<'PY'
import sys, wave
with wave.open(sys.argv[1]) as w:
    assert w.getnchannels() == 2, w.getnchannels()
    assert w.getframerate() == 48000, w.getframerate()
    assert w.getnframes() > 0
print("wav ok", w.getnframes(), "frames")
PY
pass "hybrid MDR render writes stereo WAV"
