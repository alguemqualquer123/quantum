"""Generate the kit's precipitation sprites and thunder sample.

Every pixel and every sample here is written by this script, so the library ships assets with no
third-party licence to track. Output goes to engine/components/assets/ next to the Lua components
that point at them.
"""
import os
import numpy as np
from PIL import Image

ROOT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                     "engine", "components", "assets")
TEX = os.path.join(ROOT, "textures")
SND = os.path.join(ROOT, "sounds")
os.makedirs(TEX, exist_ok=True)
os.makedirs(SND, exist_ok=True)


def rain_streak(width=48, height=192):
    """A soft vertical drop: bright core, alpha tapering to nothing at both ends."""
    y = np.linspace(-1.0, 1.0, height)[:, None]
    x = np.linspace(-1.0, 1.0, width)[None, :]
    core = np.exp(-(x / 0.28) ** 2)
    body = np.clip(1.0 - np.abs(y) ** 1.6, 0.0, 1.0)
    head = np.exp(-((y - 0.55) / 0.22) ** 2) * 0.6
    a = core * (body * 0.85 + head)
    a = np.clip(a, 0.0, 1.0)
    lum = np.clip(0.72 + 0.28 * (core * body), 0.0, 1.0)
    rgb = np.stack([lum, lum, np.ones_like(lum)], axis=-1)
    rgba = np.dstack([rgb, a])
    Image.fromarray((rgba * 255.0).astype(np.uint8), "RGBA").save(os.path.join(TEX, "rain.png"))
    return (width, height)


def snow_flake(size=96):
    """Six arms with side branches, drawn as distance-field strokes so the sprite stays soft."""
    yy, xx = np.mgrid[0:size, 0:size]
    cx = (xx + 0.5) / size * 2.0 - 1.0
    cy = (yy + 0.5) / size * 2.0 - 1.0
    r = np.sqrt(cx * cx + cy * cy)
    theta = np.arctan2(cy, cx)

    arms = np.zeros_like(r)
    for k in range(6):
        arm_angle = k * np.pi / 3.0
        # distance from the arm's centre line, only along the arm
        along = cx * np.cos(arm_angle) + cy * np.sin(arm_angle)
        across = -cx * np.sin(arm_angle) + cy * np.cos(arm_angle)
        on = np.clip(along, 0.0, 0.86)
        arms = np.maximum(arms, np.exp(-(across / 0.055) ** 2) * (on == along) * np.exp(-along * 0.55))
        for side in (-1.0, 1.0):
            branch_at = 0.42 + 0.2 * (1.0 if k % 2 else 0.0)
            d = np.abs(along - branch_at) + np.abs(across - side * 0.13)
            arms = np.maximum(arms, np.exp(-(d / 0.05) ** 2) * 0.8)
    core = np.exp(-(r / 0.16) ** 2)
    plate = np.exp(-((r - 0.55) / 0.05) ** 2) * 0.18
    a = np.clip(arms * 0.9 + core + plate, 0.0, 1.0) * np.clip(1.0 - (r / 0.98) ** 8, 0.0, 1.0)
    rgba = np.dstack([np.ones_like(a), np.ones_like(a), np.ones_like(a), a])
    Image.fromarray((rgba * 255.0).astype(np.uint8), "RGBA").save(os.path.join(TEX, "snow.png"))
    return (size, size)


def hail_pellet(size=64):
    """A hard-ish round graupel grain with a bright top-left highlight."""
    yy, xx = np.mgrid[0:size, 0:size]
    cx = (xx + 0.5) / size * 2.0 - 1.0
    cy = (yy + 0.5) / size * 2.0 - 1.0
    r = np.sqrt(cx * cx + cy * cy)
    edge = np.clip(1.0 - (r - 0.62) / 0.16, 0.0, 1.0)
    shade = np.clip(0.72 + 0.34 * (-cx * 0.7 - cy * 0.7), 0.0, 1.0)
    facet = 0.06 * np.sin(cx * 21.0) * np.sin(cy * 17.0)
    lum = np.clip(shade + facet, 0.0, 1.0)
    a = edge * np.clip(1.0 - (r / 0.99) ** 12, 0.0, 1.0)
    rgba = np.dstack([lum, lum, np.clip(lum + 0.05, 0.0, 1.0), a])
    Image.fromarray((rgba * 255.0).astype(np.uint8), "RGBA").save(os.path.join(TEX, "hail.png"))
    return (size, size)


def thunder(seconds=4.0, rate=44100):
    """A crack that then turns into a long rumble: a sharp broadband onset, resonating low bands,
    and a slow decay. Mono float, written as WAV and transcoded to ogg by ffmpeg."""
    n = int(seconds * rate)
    t = np.arange(n) / rate
    rng = np.random.default_rng(20261025)

    crack = np.zeros(n)
    onset = int(0.02 * rate)
    burst = rng.standard_normal(onset) * np.linspace(1.0, 0.0, onset) ** 0.35
    crack[:onset] = burst
    tail = rng.standard_normal(n) * np.exp(-t / 0.09)
    crack += np.clip(tail, -1.0, 1.0) * 0.55

    rumble = np.zeros(n)
    for freq, tau, gain in ((38.0, 1.9, 1.0), (57.0, 1.5, 0.7), (86.0, 1.1, 0.5),
                            (133.0, 0.8, 0.32), (210.0, 0.55, 0.2)):
        phase = rng.uniform(0.0, 2.0 * np.pi)
        band = np.sin(2.0 * np.pi * freq * t + phase) * np.exp(-t / tau)
        wobble = 1.0 + 0.45 * np.sin(2.0 * np.pi * (0.6 + freq / 900.0) * t)
        rumble += gain * band * wobble
    rumble *= np.clip((t / 0.06), 0.0, 1.0) ** 2

    mid = rng.standard_normal(n)
    lp = np.convolve(mid, np.ones(140) / 140.0, mode="same")
    rumble += 0.42 * lp * np.exp(-t / 2.4) * (1.0 + 0.5 * np.sin(2.0 * np.pi * 0.31 * t))

    signal = 0.62 * crack + 0.85 * rumble
    # rolling the high band down as time passes is what makes the crack become a rumble
    for i in range(0, n, 512):
        span = signal[i:i + 512]
        k = min(len(span), 512)
        smooth = np.convolve(span, np.ones(3 + int(20 * i / n)) / (3 + int(20 * i / n)), mode="same")
        mix = min(0.85, 0.06 + 0.8 * i / max(n, 1))
        signal[i:i + k] = span * (1.0 - mix) + smooth * mix

    peaks = [(0.9, 0.55), (1.7, 0.4), (2.8, 0.3)]
    for when, amp in peaks:
        start = int(when * rate)
        length = int(0.5 * rate)
        if start + length >= n:
            continue
        envelope = np.exp(-np.arange(length) / rate / 0.12) * (1.0 + 0.6 * np.sin(2.0 * np.pi * 3.0 * np.arange(length) / rate))
        signal[start:start + length] += amp * rng.standard_normal(length) * envelope * 0.5

    fade_in = int(0.004 * rate)
    signal[:fade_in] *= np.linspace(0.0, 1.0, fade_in)
    fade_out = int(0.25 * rate)
    signal[-fade_out:] *= np.linspace(1.0, 0.0, fade_out)
    signal = np.clip(signal, -1.0, 1.0) * 0.9

    pcm = (signal * 32767.0).astype("<i2")
    wav = os.path.join(SND, "thunder.wav")
    import wave
    with wave.open(wav, "wb") as handle:
        handle.setnchannels(1)
        handle.setsampwidth(2)
        handle.setframerate(rate)
        handle.writeframes(pcm.tobytes())
    return wav


if __name__ == "__main__":
    print("rain.png  %dx%d" % rain_streak())
    print("snow.png  %dx%d" % snow_flake())
    print("hail.png  %dx%d" % hail_pellet())
    thunder(4.0, 44100)
    print("thunder.wav 4.0 s mono, transcode it with:")
    print("  ffmpeg -y -i assets/sounds/thunder.wav -c:a libvorbis -q:a 4 assets/sounds/thunder.ogg")
