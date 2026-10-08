#!/usr/bin/env python3
"""
Converts the recorded CC0 sound effects and music for the OpenAL showcase to the
project's asset format (16-bit PCM WAV at 48 kHz, mono for 3D sources, stereo for
the 2D music) and embeds them in ../../sound_assets_sfx.h.

    python make_sfx_assets.py <dir with the Freesound downloads>

ffmpeg (on PATH) decodes, cuts (sample-accurate atrim), downmixes and resamples
(soxr); this script trims quiet tails, crossfades loop seams, fades, normalises to
-1 dBFS and writes the WAVs and the header. Sources, licenses and edits are listed
in sounds.txt.
"""

import array
import os
import subprocess
import sys
import wave

RATE = 48000
HERE = os.path.dirname(os.path.abspath(__file__))
HEADER = os.path.join(HERE, "..", "..", "sound_assets_sfx.h")

# name, source file, output wav, C symbol, channels, cut (start, length) in s,
# loop crossfade s, tail threshold (linear, 0 = keep), fade in/out ms, note
ASSETS = [
    ("854493__qubodup__uh60-black-hawk-helicopter-hover-loop-1.wav", "helicopter_loop_48k.wav",
     "g_sfx_helicopter_loop_wav", 1, None, 0.0, 0.0, 0, 0,
     "UH-60 Black Hawk hover loop by qubodup (Freesound 854493), CC0. Unchanged (already a seamless 48 kHz loop)."),
    ("537684__doomyd__siren.wav", "siren_loop_48k.wav",
     "g_sfx_siren_loop_wav", 1, None, 0.25, 0.0, 0, 0,
     "Siren by doomyd (Freesound 537684), CC0. 24-bit 44.1 kHz to 16-bit 48 kHz; last 250 ms crossfaded into the start for a seamless loop."),
    ("840508__qubodup__illegal-explosion.wav", "explosion_48k.wav",
     "g_sfx_explosion_wav", 1, None, 0.0, 0.003, 0, 300,
     "Illegal Explosion by qubodup (Freesound 840508), CC0. Quiet tail trimmed, 300 ms fade-out."),
    ("404562__superphat__assaultrifle1.wav", "rifle_shot_48k.wav",
     "g_sfx_rifle_shot_wav", 1, None, 0.0, 0.001, 1, 40,
     "assaultrifle1 by superphat (Freesound 404562), CC0. 44.1 to 48 kHz; a single shot; reverb tail trimmed below -60 dBFS."),
    ("686609__gamer500__glass-broken.wav", "glass_break_48k.wav",
     "g_sfx_glass_break_wav", 1, None, 0.0, 0.002, 0, 100,
     "glass broken by gamer500 (Freesound 686609), CC0. 44.1 to 48 kHz, quiet tail trimmed."),
    ("607307__szegvari__space-dance-2-edm-house-techno-soundtrack-film-dance-music-sfx-eq-mastered.wav",
     "music_loop_48k.wav", "g_sfx_music_loop_wav", 2, (8.005, 8.02), 0.02, 0.0, 0, 0,
     "Space Dance 2 by szegvari (Freesound 607307), CC0. 24-bit to 16-bit 48 kHz stereo; 4 bars at 120 BPM from the "
     "bar at 8.005 s, 20 ms crossfade at the seam for a seamless 8 s loop."),
]


def decode(path, channels, cut):
    """Decode to interleaved float32 at 48 kHz via ffmpeg."""
    filters = []
    if cut:
        filters.append("atrim=start=%.6f:duration=%.6f,asetpts=PTS-STARTPTS" % cut)
    filters.append("aresample=%d:resampler=soxr:precision=28" % RATE)
    cmd = ["ffmpeg", "-v", "error", "-i", path, "-af", ",".join(filters),
           "-ac", str(channels), "-f", "f32le", "-acodec", "pcm_f32le", "-"]
    raw = subprocess.run(cmd, check=True, stdout=subprocess.PIPE).stdout
    pcm = array.array("f")
    pcm.frombytes(raw)
    return pcm


def trim_tail(pcm, ch, thresh):
    last = 0
    for i in range(len(pcm) - 1, -1, -1):
        if abs(pcm[i]) > thresh:
            last = i // ch
            break
    end = min(len(pcm) // ch, last + RATE // 20)       # keep 50 ms after the last loud sample
    del pcm[end * ch:]


def make_loop(pcm, ch, xfade_s):
    """Blend the last xfade_s seconds into the start and drop them: seamless loop."""
    x = int(xfade_s * RATE)
    body = len(pcm) // ch - x
    if x <= 0 or body <= x:
        return
    for i in range(x):
        t = i / x
        for c in range(ch):
            pcm[i * ch + c] = pcm[i * ch + c] * t + pcm[(body + i) * ch + c] * (1.0 - t)
    del pcm[body * ch:]


def fade(pcm, ch, in_ms, out_ms):
    frames = len(pcm) // ch
    fi, fo = int(in_ms * RATE / 1000), int(out_ms * RATE / 1000)
    for i in range(min(fi, frames)):
        for c in range(ch):
            pcm[i * ch + c] *= i / fi
    for i in range(min(fo, frames)):
        for c in range(ch):
            pcm[(frames - 1 - i) * ch + c] *= i / fo


def to_wav16(pcm, ch, peak=0.89):
    """-1 dBFS peak normalisation, 16-bit RIFF WAV bytes."""
    m = max((abs(v) for v in pcm), default=0.0)
    g = peak / m if m > 0 else 1.0
    out = array.array("h", (max(-32768, min(32767, round(v * g * 32767.0))) for v in pcm))
    if sys.byteorder != "little":
        out.byteswap()
    data = out.tobytes()
    header = b"RIFF" + (36 + len(data)).to_bytes(4, "little") + b"WAVEfmt " + \
        (16).to_bytes(4, "little") + (1).to_bytes(2, "little") + ch.to_bytes(2, "little") + \
        RATE.to_bytes(4, "little") + (RATE * ch * 2).to_bytes(4, "little") + \
        (ch * 2).to_bytes(2, "little") + (16).to_bytes(2, "little") + b"data" + len(data).to_bytes(4, "little")
    return header + data


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    src_dir = sys.argv[1]
    lines = [
        "/* Generated by assets/sfx/make_sfx_assets.py - DO NOT EDIT */",
        "/* Recorded sound effects and music from Freesound, all CC0 (see assets/sfx/sounds.txt). */",
        "#ifndef SOUND_ASSETS_SFX_H",
        "#define SOUND_ASSETS_SFX_H",
        "",
        "#include <stdint.h>",
        "",
    ]
    for src, out, sym, ch, cut, loop_xfade, tail, fade_in, fade_out, note in ASSETS:
        pcm = decode(os.path.join(src_dir, src), ch, cut)
        if tail > 0:
            trim_tail(pcm, ch, tail)
        if loop_xfade > 0:
            make_loop(pcm, ch, loop_xfade)
        if fade_in or fade_out:
            fade(pcm, ch, fade_in, fade_out)
        wav = to_wav16(pcm, ch)
        with open(os.path.join(HERE, out), "wb") as f:
            f.write(wav)
        print("%-26s %d ch %6.2f s %8d bytes" % (out, ch, len(pcm) / ch / RATE, len(wav)))

        lines.append("/* %s (%d bytes, channels=%d, 48kHz PCM16)" % (out, len(wav), ch))
        lines.append(" * %s */" % note)
        lines.append("static const uint8_t %s[%d] = {" % (sym, len(wav)))
        for i in range(0, len(wav), 16):
            chunk = wav[i:i + 16]
            tail_comma = "," if i + 16 < len(wav) else ""
            lines.append("    " + ", ".join("0x%02x" % b for b in chunk) + tail_comma)
        lines.append("};")
        lines.append("")
    lines.append("#endif /* SOUND_ASSETS_SFX_H */")
    with open(HEADER, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
