#!/usr/bin/env python3
"""
CC0 48kHz PCM Audio Asset Synthesizer and Embedded Header Generator
Generates standardized 16-bit 48kHz PCM WAV files and C header for OpenAL showcase.
All generated audio content is CC0 (Public Domain Dedication).
"""

import os
import math
import struct

SAMPLE_RATE = 48000

def clamp16(v):
    return max(-32768, min(32767, int(round(v))))

def pack_wave_pcm16(samples, channels, sample_rate):
    """
    Constructs a standard RIFF WAVE file buffer for 16-bit PCM.
    `samples` is a list of int16 values. For stereo, samples are interleaved [L, R, L, R...].
    """
    byte_rate = sample_rate * channels * 2
    block_align = channels * 2
    data_bytes = len(samples) * 2
    riff_chunk_size = 36 + data_bytes

    header = bytearray()
    header.extend(b'RIFF')
    header.extend(struct.pack('<I', riff_chunk_size))
    header.extend(b'WAVE')
    header.extend(b'fmt ')
    header.extend(struct.pack('<I', 16))             # Subchunk1Size (16 for PCM)
    header.extend(struct.pack('<H', 1))              # AudioFormat (1 = PCM)
    header.extend(struct.pack('<H', channels))       # NumChannels
    header.extend(struct.pack('<I', sample_rate))    # SampleRate
    header.extend(struct.pack('<I', byte_rate))      # ByteRate
    header.extend(struct.pack('<H', block_align))    # BlockAlign
    header.extend(struct.pack('<H', 16))             # BitsPerSample
    header.extend(b'data')
    header.extend(struct.pack('<I', data_bytes))     # Subchunk2Size

    raw_data = struct.pack(f'<{len(samples)}h', *samples)
    return bytes(header + raw_data)

def gen_orbit_mono(duration=1.0):
    """
    Rich harmonic waveform for 3D positional orbit and ITD/HRTF headphone testing.
    440 Hz fundamental with 880 Hz and 1320 Hz harmonics plus subtle phase modulation.
    Seamless loop at 1.0s.
    """
    n_samples = int(SAMPLE_RATE * duration)
    samples = []
    for i in range(n_samples):
        t = i / SAMPLE_RATE
        # Seamless loop phase (integer number of cycles: 440 cycles in 1 sec)
        f0 = 440.0
        # Envelope window: subtle amplitude tremolo (4 Hz)
        trem = 0.85 + 0.15 * math.sin(2.0 * math.pi * 4.0 * t)
        # Harmonics
        h1 = math.sin(2.0 * math.pi * f0 * t)
        h2 = 0.5 * math.sin(2.0 * math.pi * 2.0 * f0 * t + 0.3)
        h3 = 0.25 * math.sin(2.0 * math.pi * 3.0 * f0 * t + 0.6)
        h4 = 0.125 * math.sin(2.0 * math.pi * 4.0 * f0 * t + 0.9)
        val = (h1 + h2 + h3 + h4) * trem * 0.5 * 32767.0
        samples.append(clamp16(val))
    return samples

def gen_siren_doppler(duration=1.0):
    """
    Constant tonal horn/siren for Doppler pitch shifter verification.
    750 Hz + 1500 Hz clean dual-tone (exactly 750 cycles in 1s for seamless looping).
    """
    n_samples = int(SAMPLE_RATE * duration)
    samples = []
    f0 = 750.0
    for i in range(n_samples):
        t = i / SAMPLE_RATE
        s = math.sin(2.0 * math.pi * f0 * t) + 0.4 * math.sin(2.0 * math.pi * 2.0 * f0 * t)
        val = s * 0.6 * 32767.0
        samples.append(clamp16(val))
    return samples

def gen_explosion_lfe(duration=1.0):
    """
    Heavy low-frequency explosion impact for 5.1 Subwoofer LFE channel testing (MixBin 5).
    60 Hz fundamental swept down to 30 Hz with exponential decay envelope and initial punch.
    """
    n_samples = int(SAMPLE_RATE * duration)
    samples = []
    for i in range(n_samples):
        t = i / SAMPLE_RATE
        env = math.exp(-3.5 * t)
        freq = 60.0 - 30.0 * (t ** 0.5)
        # Deep sine wave + sub-harmonic
        sub = math.sin(2.0 * math.pi * freq * t) + 0.3 * math.sin(2.0 * math.pi * (freq * 0.5) * t)
        # Initial transient punch
        transient = math.exp(-25.0 * t) * (math.sin(2.0 * math.pi * 120.0 * t) + 0.2 * math.sin(2.0 * math.pi * 240.0 * t))
        val = (sub * env * 0.75 + transient * 0.25) * 32767.0
        samples.append(clamp16(val))
    return samples

def gen_laser_stress(duration=0.12):
    """
    Dry transient laser click/blaster chirp for 64-voice polyphony stress test.
    Fast exponential down-sweep from 3200 Hz to 400 Hz over 120ms with linear decay.
    """
    n_samples = int(SAMPLE_RATE * duration)
    samples = []
    for i in range(n_samples):
        t = i / SAMPLE_RATE
        progress = t / duration
        env = 1.0 - progress
        freq = 3200.0 * math.exp(-2.5 * progress)
        val = math.sin(2.0 * math.pi * freq * t) * env * 0.8 * 32767.0
        samples.append(clamp16(val))
    return samples

def gen_voice_center(duration=1.0):
    """
    Synthetic speech formant radio communication for Center channel test (MixBin 4).
    Simulates radio transmission with vocal bandpass formants (800 Hz, 1400 Hz, 2400 Hz).
    """
    n_samples = int(SAMPLE_RATE * duration)
    samples = []
    for i in range(n_samples):
        t = i / SAMPLE_RATE
        # Formants
        pulse_train = math.sin(2.0 * math.pi * 130.0 * t)  # 130 Hz fundamental
        f1 = math.sin(2.0 * math.pi * 800.0 * t)
        f2 = math.sin(2.0 * math.pi * 1400.0 * t)
        f3 = math.sin(2.0 * math.pi * 2400.0 * t)
        vocal = (pulse_train * 0.3 + f1 * 0.35 + f2 * 0.25 + f3 * 0.1)
        # Radio bandpass gate
        gate = 0.8 if 0.05 < t < 0.95 else (t / 0.05 if t <= 0.05 else (1.0 - t) / 0.05)
        val = vocal * gate * 0.65 * 32767.0
        samples.append(clamp16(val))
    return samples

def gen_bgm_stereo(duration=2.0):
    """
    2-channel stereo ambient synthwave music loop for 2D direct background playback.
    Stereo separation: Left and Right play complementary arpeggiated melodic voices.
    """
    n_samples = int(SAMPLE_RATE * duration)
    samples = []  # Interleaved [L, R, L, R...]
    # Chord notes frequencies (Am: A3, C4, E4, A4; F: F3, A3, C4, F4)
    chords = [
        (220.0, 261.63, 329.63, 440.0),   # Am
        (174.61, 220.0, 261.63, 349.23),  # F
        (261.63, 329.63, 392.0, 523.25),  # C
        (196.0, 246.94, 293.66, 392.0)    # G
    ]
    step_duration = duration / 8.0  # 8 arpeggio steps

    for i in range(n_samples):
        t = i / SAMPLE_RATE
        step_idx = int(t / step_duration) % 8
        chord_idx = (step_idx // 2) % len(chords)
        chord = chords[chord_idx]

        note_l = chord[step_idx % 4]
        note_r = chord[(step_idx + 2) % 4]

        step_t = (t % step_duration) / step_duration
        env = math.exp(-3.0 * step_t)

        val_l = math.sin(2.0 * math.pi * note_l * t) * env * 0.45
        val_r = math.sin(2.0 * math.pi * note_r * t) * env * 0.45

        # Bass foundation
        bass = 0.25 * math.sin(2.0 * math.pi * (chord[0] * 0.5) * t)

        out_l = clamp16((val_l + bass) * 32767.0)
        out_r = clamp16((val_r + bass) * 32767.0)

        samples.append(out_l)
        samples.append(out_r)
    return samples

def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    assets_dir = os.path.join(script_dir, "assets")
    os.makedirs(assets_dir, exist_ok=True)

    print("Generating CC0 48kHz audio assets...")

    assets = [
        ("orbit_mono_48k.wav", gen_orbit_mono(1.0), 1, "g_sound_orbit_mono_wav"),
        ("siren_doppler_48k.wav", gen_siren_doppler(1.0), 1, "g_sound_siren_doppler_wav"),
        ("explosion_lfe_48k.wav", gen_explosion_lfe(1.0), 1, "g_sound_explosion_lfe_wav"),
        ("laser_stress_48k.wav", gen_laser_stress(0.12), 1, "g_sound_laser_stress_wav"),
        ("voice_center_48k.wav", gen_voice_center(1.0), 1, "g_sound_voice_center_wav"),
        ("bgm_stereo_48k.wav", gen_bgm_stereo(2.0), 2, "g_sound_bgm_stereo_wav"),
    ]

    header_lines = [
        "/* Automatically generated by generate_assets.py - DO NOT EDIT */",
        "/* CC0 1.0 Universal (Public Domain Dedication) */",
        "#ifndef SOUND_ASSETS_H",
        "#define SOUND_ASSETS_H",
        "",
        "#include <stdint.h>",
        "#include <stddef.h>",
        ""
    ]

    for fname, smp, ch, symbol in assets:
        wav_bytes = pack_wave_pcm16(smp, ch, SAMPLE_RATE)
        fpath = os.path.join(assets_dir, fname)
        with open(fpath, "wb") as f:
            f.write(wav_bytes)
        print(f"  -> Generated {fname}: {len(wav_bytes)} bytes (channels={ch}, rate={SAMPLE_RATE})")

        header_lines.append(f"/* {fname} ({len(wav_bytes)} bytes, channels={ch}, 48kHz PCM16) */")
        header_lines.append(f"static const uint8_t {symbol}[{len(wav_bytes)}] = {{")
        # Format bytes in lines of 16
        for i in range(0, len(wav_bytes), 16):
            chunk = wav_bytes[i:i+16]
            hex_str = ", ".join(f"0x{b:02x}" for b in chunk)
            sep = "," if i + 16 < len(wav_bytes) else ""
            header_lines.append(f"    {hex_str}{sep}")
        header_lines.append("};\n")

    header_lines.append("#endif /* SOUND_ASSETS_H */\n")

    header_path = os.path.join(script_dir, "sound_assets.h")
    with open(header_path, "w", encoding="utf-8") as f:
        f.write("\n".join(header_lines))
    print(f"Successfully generated embedded C header at: {header_path}")

if __name__ == "__main__":
    main()
