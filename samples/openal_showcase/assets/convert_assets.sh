#!/bin/bash
# convert_assets.sh - Adapts audio assets for the MCPX APU in nxdk
# Enforces strictly 48kHz, 16-bit PCM (Mono for 3D positional, Stereo for 2D direct)

set -e
mkdir -p output/

# 1. 3D Mono Source (Helicopter/orbiting loop for 3D positional and ITD/HRTF)
if [ -f input_orbit.wav ]; then
    ffmpeg -y -i input_orbit.wav -ac 1 -ar 48000 -c:a pcm_s16le output/orbit_mono_48k.wav
fi

# 2. Tonal Doppler Source (Vehicle siren / tonal missile)
if [ -f input_siren.wav ]; then
    ffmpeg -y -i input_siren.wav -ac 1 -ar 48000 -c:a pcm_s16le output/siren_doppler_48k.wav
fi

# 3. Impact / LFE Sound (Explosion / artillery for 5.1 Subwoofer MixBin 5)
if [ -f input_explosion.wav ]; then
    ffmpeg -y -i input_explosion.wav -ac 1 -ar 48000 -c:a pcm_s16le output/explosion_lfe_48k.wav
fi

# 4. Dry Sound for 64-Voice Polyphony Stress Test (Laser / sparks / click)
if [ -f input_laser.wav ]; then
    ffmpeg -y -i input_laser.wav -ac 1 -ar 48000 -c:a pcm_s16le output/laser_stress_48k.wav
fi

# 5. Voice for Center Channel (MixBin 4 radio transmission)
if [ -f input_voice.wav ]; then
    ffmpeg -y -i input_voice.wav -ac 1 -ar 48000 -c:a pcm_s16le output/voice_center_48k.wav
fi

# 6. Ambient 2D Music (Direct 2-channel stereo loop)
if [ -f input_bgm.wav ]; then
    ffmpeg -y -i input_bgm.wav -ac 2 -ar 48000 -c:a pcm_s16le output/bgm_stereo_48k.wav
fi

echo "Asset conversion complete."
