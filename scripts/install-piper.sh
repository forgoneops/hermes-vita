#!/bin/sh
# Spoken answers: install Piper (a local text-to-speech engine) and an English voice into ~/.local/share/piper.
# Downloads about 90 MB: the Piper release from github.com/rhasspy/piper and the voice from huggingface.co/rhasspy/piper-voices.
# The voice is the quality "medium" English (US) female "hfc_female". Any other Piper voice works: set PIPER_VOICE to its .onnx file.
set -e
D="$HOME/.local/share/piper"; mkdir -p "$D/dl"; cd "$D"
[ -x piper/piper ] || { curl -fL -o dl/piper.tgz https://github.com/rhasspy/piper/releases/download/2023.11.14-2/piper_linux_x86_64.tar.gz && tar xzf dl/piper.tgz; }
V=https://huggingface.co/rhasspy/piper-voices/resolve/main/en/en_US/hfc_female/medium
[ -f en_US-hfc_female-medium.onnx ] || { curl -fL -o en_US-hfc_female-medium.onnx $V/en_US-hfc_female-medium.onnx; curl -fL -o en_US-hfc_female-medium.onnx.json $V/en_US-hfc_female-medium.onnx.json; }
echo "ok: $(echo 'Hello, I am Hermes.' | ./piper/piper --model en_US-hfc_female-medium.onnx --output-raw 2>/dev/null | wc -c) bytes of speech"
command -v ffmpeg >/dev/null || echo "note: ffmpeg is needed too (the bridge converts the voice to 48 kHz for the Vita)"
