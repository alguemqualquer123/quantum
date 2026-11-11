#!/usr/bin/env python3
# (c) SR VINIX
# SPDX-License-Identifier: MIT
"""Generate the voxel game's sound pack as 16-bit PCM WAV files.

Licence boundary, stated first because it is the reason this file exists: the sounds of
Minecraft are art owned by Mojang/Microsoft, so nothing here is derived from them. What is
reproduced is the *taxonomy* - a digging hit per material, a break, a place, a footstep, a
splash, an ambience loop - which is a design fact, not a copyrightable expression. Every sample
is synthesised from noise and damped sine waves by this script, so the pack is as free to ship
as the texture atlas scripts/generate_voxel_atlas.py writes.

Why the engine takes WAV and not OGG: sound is SoLoud, and the loader hands the file bytes to
SoLoud::Wav. PCM needs no decoder compiled in, so a script can write samples the runtime reads.
Verified against the two files already shipping in engine/components/assets/sounds: mono,
16-bit, uncompressed, and the engine plays them.

Deterministic: every file gets its own seed derived from its name, so re-running the script
rebuilds a byte-identical pack. That is what makes a texture-like asset auditable in review.

Usage:
    python scripts/generate_voxel_sounds.py [--project <dir>] [--rate 44100] [--peak-db -3]
"""

import argparse
import math
import os
import struct
import sys
import zlib

# ---------------------------------------------------------------------------
# RIFF writer

def write_wav(path, samples, rate):
    """16-bit signed mono PCM in a canonical 44-byte-header RIFF file."""
    clipped = [max(-1.0, min(1.0, s)) for s in samples]
    frames = b"".join(struct.pack("<h", int(round(s * 32767.0))) for s in clipped)
    header = b"RIFF" + struct.pack("<I", 36 + len(frames)) + b"WAVE"
    header += b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16)
    header += b"data" + struct.pack("<I", len(frames))
    with open(path, "wb") as fh:
        fh.write(header + frames)
    return len(header) + len(frames)


def read_wav(path):
    """Read the pack back with the stdlib, which is how the manifest gets its measured numbers."""
    import wave
    with wave.open(path, "rb") as w:
        n = w.getnframes()
        raw = w.readframes(n)
    rate = w.getframerate()
    ch = w.getnchannels()
    vals = struct.unpack("<%dh" % (len(raw) // 2), raw)
    return rate, ch, n, [v / 32768.0 for v in vals]


# ---------------------------------------------------------------------------
# Deterministic noise: one xorshift per file so a name always sounds the same

class Rng:
    def __init__(self, name):
        h = 0x9E3779B97F4A7C15
        for c in name.encode("utf-8"):
            h = ((h + c) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
        self.state = h or 1

    def unit(self):
        """Lehmer-ish step of the hashed state; uniform in [-1, 1)."""
        self.state = (self.state * 6364136223846793005 + 1442695040888963407) & 0xFFFFFFFFFFFFFFFF
        return ((self.state >> 11) & 0xFFFFFFFF) / 2147483648.0 - 1.0


def noise(n, rng):
    return [rng.unit() for _ in range(n)]


# ---------------------------------------------------------------------------
# Filters. One-pole sections: cheap, and an impulse response with no ringing, which is what a
# percussive hit wants. Cutoffs are in Hz of a 44.1 kHz rate unless stated.

def lowpass(x, cutoff, rate):
    a = math.exp(-2.0 * math.pi * cutoff / rate)
    y = []
    acc = 0.0
    for v in x:
        acc = a * acc + (1.0 - a) * v
        y.append(acc)
    return y


def highpass(x, cutoff, rate):
    lo = lowpass(x, cutoff, rate)
    return [v - s for v, s in zip(x, lo)]


def bandpass(x, lo_hz, hi_hz, rate):
    return highpass(lowpass(x, hi_hz, rate), lo_hz, rate)


def periodic_filter(period, rate, sections, cutoff_hz, laps=2):
    """Run cascaded one-poles over a repeating period and keep the last lap.

    A loop only wraps silently if the filter holds the same state at sample 0 as at the end. Feeding
    the same period `laps` times drives each section to its periodic fixed point - the transient
    decays by exp(-2*pi*f_c/N) per lap, which for a 6 s loop is nothing - so the returned samples
    wrap by construction and no cross-fade has to smear them. `cutoff_hz(i, k)` sees the index
    inside the period and the section number, and must itself be periodic.
    """
    n = len(period)
    accs = [0.0] * sections
    taps = []
    for idx in range(laps * n):
        x = period[idx % n]
        k_in = idx % n
        for k in range(sections):
            a = math.exp(-2.0 * math.pi * cutoff_hz(k_in, k) / rate)
            accs[k] = a * accs[k] + (1.0 - a) * x
            x = accs[k]
        taps.append(list(accs))
    return taps[-n:]


def ping(freq, n, rate, decay, amp=1.0, phase=0.0):
    """A damped sine - the struck-glass and struck-wood body."""
    out = []
    for i in range(n):
        t = i / rate
        env = math.exp(-t / decay)
        out.append(amp * env * math.sin(2.0 * math.pi * freq * t + phase))
    return out


def adsr(n, rate, attack, decay, sustain_level, release):
    out = []
    a = max(1, int(attack * rate))
    d = max(1, int(decay * rate))
    r = max(1, int(release * rate))
    body = n - a - d - r
    if body < 0:
        # A hit shorter than its own envelope: scale everything down instead of clipping to zero,
        # so a 20 ms click still has a 20 ms envelope rather than a silent tail.
        total = a + d + r
        a, d, r = max(1, a * n // total), max(1, d * n // total), 0
        body = max(0, n - a - d - r)
    for i in range(a):
        out.append(i / a)
    for i in range(d):
        out.append(1.0 - (1.0 - sustain_level) * (i / d))
    for _ in range(body):
        out.append(sustain_level)
    for i in range(r):
        out.append(sustain_level * (1.0 - i / r))
    return out[:n]


def scale(x, f):
    return [v * f for v in x]


def add(a, b):
    n = max(len(a), len(b))
    return [(a[i] if i < len(a) else 0.0) + (b[i] if i < len(b) else 0.0) for i in range(n)]


def peak_db(x):
    p = max(abs(v) for v in x) if x else 0.0
    return 20.0 * math.log10(p) if p > 1e-9 else -120.0


def normalise(x, target_db):
    return peak_to(x, 10.0 ** (target_db / 20.0))


def peak_to(x, target):
    """Scale a layer so its loudest sample sits at `target`, which is what makes a mix a design."""
    p = max((abs(v) for v in x), default=0.0)
    if p < 1e-9:
        return x
    return scale(x, target / p)


# ---------------------------------------------------------------------------
# Measurement. The manifest carries these numbers, so a sound-design change has to move a number
# here to be real - "it sounds like stone" is not checkable, the band balance is.

def rms(x):
    return math.sqrt(sum(v * v for v in x) / len(x)) if x else 0.0


def zero_crossings(x):
    """Sign changes per second: the cheap proxy for "bright", and it separates hiss from tone.

    The generator's own one-pole filters leak, and a full FFT would cost a dependency, so this is
    the coarse instrument. Sand and glass both sit high in zcr; what tells them apart is the ring,
    which shows up as a *low* zcr at the same high-frequency content.
    """
    return sum(1 for i in range(1, len(x)) if (x[i] < 0.0) != (x[i - 1] < 0.0))


def band_balance(x, rate):
    """Energy fraction in low (under 250 Hz), mid and high, from two one-pole taps."""
    def lp(cutoff):
        a = math.exp(-2.0 * math.pi * cutoff / rate)
        acc = 0.0
        out = []
        for v in x:
            acc = a * acc + (1.0 - a) * v
            out.append(acc)
        return out

    lo = lp(250.0)
    mid = lp(2500.0)
    hi = [v - m for v, m in zip(x, mid)]
    mid = [m - l for m, l in zip(mid, lo)]
    total = rms(lo) ** 2 + rms(mid) ** 2 + rms(hi) ** 2
    if total <= 1e-12:
        return (0.0, 0.0, 0.0)
    return (rms(lo) ** 2 / total, rms(mid) ** 2 / total, rms(hi) ** 2 / total)


def loop_seam(x, rate):
    """(head rms, tail rms, |first - last|) over a 50 ms window - what a looping buffer clicks on.

    A periodic bed has all three small and head equal to tail. The cross-fade version measured a
    wrap delta larger than the head, which is a tick once per lap.
    """
    w = max(1, int(rate * 0.05))
    return (rms(x[:w]), rms(x[-w:]), abs(x[0] - x[-1]))


# ---------------------------------------------------------------------------
# The sounds. Each returns a float list at `rate`.

def hit_dig(mat, variant, rate, peak_db_target):
    """One pickaxe blow: a transient of the material's own band plus a low body."""
    rng = Rng("dig|%s|%d" % (mat, variant))
    n = int(rate * 0.13)
    grain = noise(n, rng)

    if mat == "stone":
        body = bandpass(grain, 1200.0 + 260.0 * variant, 5200.0, rate)
        thump = ping(96.0 + 8.0 * variant, n, rate, 0.045, 0.5)
        ring = ping(2100.0 + 180.0 * variant, n, rate, 0.02, 0.16)
    elif mat == "dirt":
        # Grass and dirt are the soft case: no ring at all, and the band sits under 1 kHz so the
        # hit reads as soil moving rather than as a click.
        body = lowpass(grain, 820.0 + 90.0 * variant, rate)
        body = add(body, scale(bandpass(grain, 900.0, 1900.0, rate), 0.35))
        thump = ping(72.0, n, rate, 0.055, 0.42)
        ring = [0.0] * n
    elif mat == "wood":
        body = bandpass(grain, 500.0 + 120.0 * variant, 2600.0, rate)
        thump = ping(168.0 + 14.0 * variant, n, rate, 0.07, 0.7)
        ring = ping(430.0 + 40.0 * variant, n, rate, 0.05, 0.5)
    elif mat == "sand":
        # Sand is hiss without a body: it has no resonant pitch, so the low end is cut hard.
        body = bandpass(grain, 2600.0 + 400.0 * variant, 7400.0, rate)
        thump = [0.0] * n
        ring = [0.0] * n
    elif mat == "glass":
        # Glass is the tonal case: the noise is a tick of an attack and the body is two partials, so
        # it cannot be mistaken for sand, which is pure hiss with no pitch to hold onto.
        body = scale(highpass(grain, 3200.0, rate), 0.35)
        thump = [0.0] * n
        ring = add(ping(4200.0 + 300.0 * variant, n, rate, 0.1, 1.0),
                   ping(6300.0, n, rate, 0.055, 0.55))

    x = add(add(scale(body, 0.9), thump), ring)
    x = [v * e for v, e in zip(x, adsr(n, rate, 0.001, 0.03, 0.35, 0.07))]
    return normalise(x, peak_db_target - 4.0)


def hit_break(mat, rate, peak_db_target):
    """The block giving way: the dig's band, longer, plus a per-material thud and a debris rattle.

    Every layer is normalised to an explicit peak before the mix. One shared 58 Hz body at 0.85 used
    to own the peak of all five breaks - a filter's gain varies by an order of magnitude across
    these bands, so the material's own colour was inaudible and stone measured as dirt.
    """
    rng = Rng("break|%s" % mat)
    n = int(rate * 0.34)
    grain = noise(n, rng)

    bright = {"stone": 4600.0, "dirt": 900.0, "wood": 2300.0, "sand": 6800.0, "glass": 7400.0}[mat]
    soft = {"stone": 1100.0, "dirt": 120.0, "wood": 480.0, "sand": 2200.0, "glass": 3000.0}[mat]
    body = bandpass(grain, soft, bright, rate)
    if mat == "dirt":
        body = lowpass(grain, bright, rate)
    body = peak_to(body, 1.0)

    # The thud's pitch follows the mass of the block: sand and dirt are the low, soft ones and glass
    # barely thuds at all, because it goes away as shards rather than as a lump.
    boom_hz = {"stone": 62.0, "dirt": 48.0, "wood": 78.0, "sand": 55.0, "glass": 96.0}[mat]
    boom_peak = {"stone": 0.40, "dirt": 0.44, "wood": 0.42, "sand": 0.30, "glass": 0.18}[mat]
    boom = peak_to(ping(boom_hz, n, rate, 0.09), boom_peak)

    ring = peak_to({"stone": ping(1750.0, n, rate, 0.07),
                   "dirt": [0.0] * n,
                   "wood": ping(390.0, n, rate, 0.14),
                   "sand": [0.0] * n,
                   # Glass does not thud, it shatters: two partials and a bright rattle after them.
                   "glass": add(ping(3900.0, n, rate, 0.1), ping(5600.0, n, rate, 0.07))}[mat],
                   {"stone": 0.34, "dirt": 0.0, "wood": 0.40, "sand": 0.0, "glass": 0.55}[mat])

    x = add(add(body, boom), ring)
    x = [v * e for v, e in zip(x, adsr(n, rate, 0.002, 0.08, 0.42, 0.18))]
    return normalise(x, peak_db_target)


def hit_place(mat, rate, peak_db_target):
    """Setting a block down: one clean short contact, quieter than a break by design."""
    rng = Rng("place|%s" % mat)
    n = int(rate * 0.11)
    grain = noise(n, rng)
    bright = {"stone": 3800.0, "dirt": 760.0, "wood": 2100.0, "sand": 5600.0, "glass": 6200.0}[mat]
    soft = {"stone": 900.0, "dirt": 140.0, "wood": 420.0, "sand": 2400.0, "glass": 3200.0}[mat]
    body = bandpass(grain, soft, bright, rate)
    if mat == "dirt":
        body = lowpass(grain, bright, rate)
    thump_freq = {"stone": 110.0, "dirt": 74.0, "wood": 150.0, "sand": 90.0, "glass": 240.0}[mat]
    x = add(body, ping(thump_freq, n, rate, 0.05, 0.6))
    x = [v * e for v, e in zip(x, adsr(n, rate, 0.001, 0.025, 0.25, 0.06))]
    return normalise(x, peak_db_target - 6.0)


def hit_step(mat, variant, rate, peak_db_target):
    """A footstep: a scuff of the material's band and a heel thud, a fraction of a dig's level.

    Two variants per material so a walk cycle alternates instead of repeating one transient, which is
    the thing that makes a looped footstep read as a machine rather than as walking.
    """
    rng = Rng("step|%s|%d" % (mat, variant))
    n = int(rate * 0.16)
    grain = noise(n, rng)

    # The scuff's band is the dig's band a fifth lower: the same material, heard as a sole dragging
    # over it rather than a pickaxe hitting it.
    bright = {"stone": 3400.0, "dirt": 640.0, "wood": 1900.0, "sand": 5200.0, "glass": 4400.0}[mat]
    soft = {"stone": 700.0, "dirt": 110.0, "wood": 330.0, "sand": 1900.0, "glass": 1500.0}[mat]
    shift = 1.0 + 0.07 * variant
    scuff = bandpass(grain, soft * shift, bright * shift, rate)
    if mat == "dirt":
        scuff = lowpass(grain, bright * shift, rate)
    scuff = peak_to(scuff, 1.0)

    heel_hz = {"stone": 132.0, "dirt": 88.0, "wood": 176.0, "sand": 104.0, "glass": 210.0}[mat]
    # Sand and dirt swallow the heel: a loose surface takes the weight without ringing.
    heel_peak = {"stone": 0.34, "dirt": 0.20, "wood": 0.38, "sand": 0.12, "glass": 0.26}[mat]
    heel = peak_to(ping(heel_hz * (1.0 + 0.05 * variant), n, rate, 0.05), heel_peak)

    x = add(scuff, heel)
    # The attack is slower than a pickaxe's: a foot arrives, it does not strike.
    x = [v * e for v, e in zip(x, adsr(n, rate, 0.004, 0.045, 0.30, 0.09))]
    return normalise(x, peak_db_target - 9.0)


def splash(rate, peak_db_target):
    """Entry noise: a broad swoosh whose band falls, then three bubbles at different pitches."""
    rng = Rng("splash")
    n = int(rate * 0.55)
    grain = noise(n, rng)

    # Running the cutoff down over the burst is what reads as something going *into* water rather
    # than as a generic noise hit: the high end is the surface breaking, the low end is the body.
    out = []
    acc = 0.0
    lo_acc = 0.0
    for i in range(n):
        t = i / n
        cutoff = 5200.0 * (1.0 - 0.82 * t) + 380.0
        a = math.exp(-2.0 * math.pi * cutoff / rate)
        acc = a * acc + (1.0 - a) * grain[i]
        a_lo = math.exp(-2.0 * math.pi * 220.0 / rate)
        lo_acc = a_lo * lo_acc + (1.0 - a_lo) * grain[i]
        out.append(acc - lo_acc * 0.55)

    env = [math.exp(-3.4 * (i / n)) * min(1.0, i / (rate * 0.004)) for i in range(n)]
    out = [v * e for v, e in zip(out, env)]

    bubbles = [0.0] * n
    for k, (freq, start, dec) in enumerate([(1180.0, 0.09, 0.035), (820.0, 0.17, 0.045),
                                            (1520.0, 0.26, 0.03), (640.0, 0.34, 0.06)]):
        off = int(start * rate)
        # A bubble glides up: the cavity closing raises its pitch, so the sine's own frequency is
        # a function of the time since it started rather than a constant.
        glide = [math.sin(2.0 * math.pi * (freq + 900.0 * (i / rate)) * (i / rate)) *
                 math.exp(-(i / rate) / dec) for i in range(min(int(rate * 0.12), n - off))]
        for i in range(len(glide)):
            bubbles[off + i] += glide[i] * (0.5 - 0.08 * k)

    return normalise(add(out, bubbles), peak_db_target - 2.0)


def period_sine(cycles, n, phase=0.0):
    """A sine spanning the loop with a whole number of cycles - the only kind that can repeat.

    Modulators are written in cycles-per-loop rather than hertz for this reason: 0.23 Hz over a 4 s
    bed is 0.92 of a cycle, so the loop ended mid-breath and the wrap delta measured larger than the
    head itself.
    """
    return [math.sin(2.0 * math.pi * cycles * i / n + phase) for i in range(n)]


def underwater_loop(seconds, rate, peak_db_target):
    """The muffling bed: heavily lowpassed noise whose cutoff breathes on a slow LFO."""
    rng = Rng("underwater")
    n = int(rate * seconds)
    grain = noise(n, rng)

    # Two cascaded sections, as before, but both driven by the same periodic period, so the whole
    # chain returns to its starting state at the wrap.
    cutoff_lfo = period_sine(1, n)

    def cutoff(i, k):
        if k == 1:
            return 70.0
        return 260.0 + 90.0 * cutoff_lfo[i]

    taps = periodic_filter(grain, rate, 2, cutoff)
    pressure = period_sine(int(round(46.0 * seconds)), n)
    out = [t[0] * 0.8 + t[1] * 0.5 + 0.06 * pressure[i] for i, t in enumerate(taps)]
    return normalise(out, peak_db_target - 12.0)


def ambience_loop(kind, seconds, rate, peak_db_target):
    """Wind bed plus the material of the hour: chirps by day, crickets by night."""
    rng = Rng("ambience|%s" % kind)
    n = int(rate * seconds)
    grain = noise(n, rng)

    # Two LFOs one and two cycles long, so they stay unrelated in shape while both closing exactly
    # at the wrap; the amplitude breath rides the first a quarter-cycle out of step with it.
    slow = period_sine(1, n)
    fast = period_sine(2, n, 1.1)
    breath = period_sine(1, n, 0.6)

    def cutoff(i, k):
        return max(120.0, 700.0 + 420.0 * slow[i] + 180.0 * fast[i])

    taps = periodic_filter(grain, rate, 1, cutoff)
    out = [t[0] * (0.75 + 0.25 * breath[i]) for i, t in enumerate(taps)]

    events = []
    if kind == "day":
        # Bird calls: a short FM sweep up then down. Sparse - four in six seconds reads as distance.
        for pos in (0.42, 1.85, 3.4, 4.95):
            f0 = 2400.0 + 900.0 * rng.unit()
            seg = int(rate * 0.09)
            chirp = []
            for i in range(seg):
                t = i / rate
                sweep = f0 + 1500.0 * math.sin(math.pi * t / 0.09)
                chirp.append(math.sin(2.0 * math.pi * sweep * t) *
                             math.sin(math.pi * t / 0.09) * 0.4)
            events.append((int(pos * rate), chirp))
    else:
        # Crickets: a 4.6 kHz tone gated at 22 Hz, which is the insect's own rhythm.
        for pos in (0.3, 1.1, 2.2, 3.05, 4.1, 5.2):
            seg = int(rate * 0.16)
            train = []
            for i in range(seg):
                t = i / rate
                gate = max(0.0, math.sin(2.0 * math.pi * 22.0 * t))
                train.append(math.sin(2.0 * math.pi * 4600.0 * t) * gate * 0.16 *
                             math.sin(math.pi * t / 0.16))
            events.append((int(pos * rate), train))

    # Modulo, not a bounds check: a call that runs off the end of the period has to arrive at the
    # start of it, which is where the loop plays it on the next lap.
    for off, ev in events:
        for i in range(len(ev)):
            out[(off + i) % n] += ev[i]

    return normalise(out, peak_db_target - 14.0)


# ---------------------------------------------------------------------------
# Pack layout: name -> builder. The names are the wire format between this file, the module and
# the game, so a rename here has to land in VoxelAudio.cpp's table too.

MATERIALS = ("stone", "dirt", "wood", "sand", "glass")

# How many dig variants each material gets. Stone and dirt are the blocks a player spends the most
# time hitting, so they get three grains to repeat; glass has one because it only ever breaks once.
DIG_VARIANTS = {"stone": 3, "dirt": 3, "wood": 2, "sand": 2, "glass": 1}

# Footsteps alternate: one foot, one variant. Two is enough to break the repetition without turning a
# walk into a sample lottery.
STEP_VARIANTS = 2

# The three buffers the module plays with `looping = true`. They are measured for their seam because
# a hit that clicks at the end is silent for one sample; a loop that clicks does it forever.
LOOP_NAMES = ("underwater.wav", "ambience_day.wav", "ambience_night.wav")


def build_pack(rate):
    pack = []
    for mat in MATERIALS:
        for v in range(DIG_VARIANTS[mat]):
            pack.append(("dig_%s_%d.wav" % (mat, v), lambda m=mat, k=v: hit_dig(m, k, rate, 0)))
    for mat in MATERIALS:
        pack.append(("break_%s.wav" % mat, lambda m=mat: hit_break(m, rate, 0)))
    for mat in MATERIALS:
        pack.append(("place_%s.wav" % mat, lambda m=mat: hit_place(m, rate, 0)))
    for mat in MATERIALS:
        for v in range(STEP_VARIANTS):
            pack.append(("step_%s_%d.wav" % (mat, v), lambda m=mat, k=v: hit_step(m, k, rate, 0)))
    pack.append(("splash.wav", lambda: splash(rate, 0)))
    pack.append(("underwater.wav", lambda: underwater_loop(4.0, rate, 0)))
    pack.append(("ambience_day.wav", lambda: ambience_loop("day", 6.0, rate, 0)))
    pack.append(("ambience_night.wav", lambda: ambience_loop("night", 6.0, rate, 0)))
    return pack


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--project", default=os.path.join(os.path.dirname(os.path.dirname(
        os.path.abspath(__file__))), "templates", "voxelworld-main"))
    ap.add_argument("--rate", type=int, default=44100)
    ap.add_argument("--peak-db", type=float, default=-3.0, help="peak of the loudest file")
    args = ap.parse_args()

    out_dir = os.path.join(args.project, "sounds", "voxel")
    os.makedirs(out_dir, exist_ok=True)

    rows = []
    loops = []
    total_bytes = 0
    digest = 0
    for name, builder in build_pack(args.rate):
        samples = builder()
        # The normalise() calls inside each builder target 0 dBFS; scale the whole pack to the
        # requested headroom here so the master peak is one number in one place.
        samples = scale(samples, 10.0 ** (args.peak_db / 20.0))
        path = os.path.join(out_dir, name)
        size = write_wav(path, samples, args.rate)
        total_bytes += size

        rate_back, ch, frames, vals = read_wav(path)
        assert rate_back == args.rate and ch == 1 and frames == len(samples), name
        # The digest hashes the bytes on disk, so a re-run that prints the same value is proof the
        # pack is deterministic rather than proof the script ran twice.
        with open(path, "rb") as fh:
            digest = (digest * 0x100000001B3 + zlib.crc32(fh.read())) & 0xFFFFFFFFFFFFFFFF

        low, mid, hi = band_balance(vals, rate_back)
        seconds = frames / rate_back
        rows.append((name, seconds, peak_db(vals), size, zero_crossings(vals) / seconds, low, mid, hi))
        if name in LOOP_NAMES:
            head, tail, wrap = loop_seam(vals, rate_back)
            loops.append((name, head, tail, wrap))

    manifest = os.path.join(out_dir, "manifest.txt")
    line = "%-22s %6s %8s %8s %8s %5s %5s %5s\n" % (
        "file", "sec", "peak", "bytes", "zcr/s", "low", "mid", "hi")
    with open(manifest, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("# generated by scripts/generate_voxel_sounds.py - do not edit\n")
        fh.write("# rate %d Hz, 16-bit mono PCM, master peak %.1f dBFS, pack digest %016x\n" %
                 (args.rate, args.peak_db, digest))
        fh.write(line)
        fh.write("# low/mid/hi are the energy fraction under 250 Hz, 250-2500 Hz and above\n")
        for name, seconds, db, size, zcr, low, mid, hi in rows:
            fh.write("%-22s %6.3f %8.1f %8d %8.0f %5.2f %5.2f %5.2f\n" %
                     (name, seconds, db, size, zcr, low, mid, hi))
        fh.write("# loops: head and tail are 50 ms of rms, wrap d is |first sample - last sample|\n")
        fh.write("%-22s %8s %8s %8s\n" % ("file", "head", "tail", "wrap d"))
        for name, head, tail, wrap in loops:
            fh.write("%-22s %8.5f %8.5f %8.5f\n" % (name, head, tail, wrap))

    print("voxel sound pack: %d files, %d bytes, digest %016x -> %s" %
          (len(rows), total_bytes, digest, out_dir))
    print("  " + line.rstrip())
    for name, seconds, db, size, zcr, low, mid, hi in rows:
        print("  %-22s %6.3f %8.1f %8d %8.0f %5.2f %5.2f %5.2f" %
              (name, seconds, db, size, zcr, low, mid, hi))
    for name, head, tail, wrap in loops:
        print("  loop %-17s head %.5f tail %.5f wrap d %.5f" % (name, head, tail, wrap))
    print("manifest -> %s" % manifest)
    return 0


if __name__ == "__main__":
    sys.exit(main())
