---
name: hermes-vita
description: "Use when the user wants a PS Vita as a voice panel for Hermes: pair it, run the bridge, install the plugin and the VPK, or fix a connection."
version: 1.0.0
license: MIT
platforms: [linux, macos]
metadata:
  hermes:
    tags: [PS-Vita, Voice, Bridge, Companion, Hardware]
---

# hermes-vita: a PS Vita as a panel for Hermes

The Vita (with VitaShell and a homebrew-enabled firmware) talks to a small bridge on this PC over the home network. You talk to Hermes by holding SELECT, and Hermes answers in text and, if Piper is installed, aloud. With the plugin on, the Vita also shows what Hermes is doing in real desktop and CLI sessions (thinking, which tool, the answer).

Repo: the `hermes-vita` folder this skill came from (`bridge/`, `app/`, `hermes-plugin/`, `scripts/`). Run commands from its root.

## One-time setup (ask before each step that installs or enables something)
1. **Hermes API server.** The bridge chats through Hermes' OpenAI-compatible API on 127.0.0.1:8642. It needs `API_SERVER_ENABLED=true` and an `API_SERVER_KEY=...` in `~/.hermes/.env`, then a gateway restart. Check first: `grep API_SERVER ~/.hermes/.env`. Do not print the key.
2. **Speech to text** (so your voice becomes text): set `STT_URL` to any OpenAI-compatible transcription endpoint (a local faster-whisper server, or OpenAI), optionally `STT_MODEL` and `STT_KEY`.
3. **Spoken answers** (optional): `scripts/install-piper.sh` (downloads about 90 MB: Piper and an English voice; needs `ffmpeg`).
4. **The plugin** (optional, live view of real sessions): `scripts/install-plugin.sh`, then `hermes plugins enable hermes-vita` and restart Hermes. Plugins are off until the user enables them; never edit `plugins.enabled` yourself without being asked.
5. **The app.** `scripts/build-vpk.sh` makes `dist/HermesVita.vpk` (needs Docker). Copy it to the Vita: in VitaShell press SELECT to start FTP, then `curl -T dist/HermesVita.vpk ftp://<vita-ip>:1337/ux0:/downloads/`, then press X on the file in VitaShell to install. Keep the Vita awake: FTP stops when the screen sleeps.

## Pairing (every time a new Vita is added)
1. Start the bridge: `bun bridge/bridge.ts serve` (keep it running; `VITA_HOST=0.0.0.0` is the default). The first lines tell what is on or off.
2. Get a code: `bun bridge/bridge.ts pair`. It prints the code and the exact text to type, e.g. `192.168.1.20 123456`. The code works once, for five minutes.
3. On the Vita: open HermesVita, press R twice to the LINK page, press X, and type the printed text (PC address, a space, the code).
4. Check: `bun bridge/bridge.ts devices`. To remove a Vita: `bun bridge/bridge.ts revoke <name>`.

## Using it
- Hold **SELECT** (or the bar on the touch screen) and talk; release to send. **TRIANGLE** types a message. **SQUARE** turns spoken answers on or off. **L/R** switch TALK, PANEL, LINK. PANEL has eight prompt cards; add your own in `ux0:data/pocket-hermes/presets.txt` (`LABEL|prompt`).
- The Vita app writes a startup log to `ux0:data/pocket-hermes/log.txt` (read it over FTP when it will not start).

## Security notes to tell the user
- The bridge is plain HTTP on the home network on purpose (the Vita has no modern TLS). It has a one-use pairing code, a revocable device token, a small allow-list, a rate lock and an audit log (`~/.hermes-vita/audit.jsonl`). Hermes' API key never leaves the PC.
- Do not expose port 4410 to the internet or through a tunnel.

## Troubleshooting
- App exits at launch: read `log.txt`. It should show `portrait ok` and `init net=00000000`.
- "bridge not reachable": the bridge is not running, the PC address is wrong (edit `ux0:data/pocket-hermes/config.txt`), or a firewall blocks 4410.
- "wrong or expired code": run `pair` again.
- "Hermes API is off on the PC": step 1 above.
- Nothing is spoken: `scripts/install-piper.sh`, then restart the bridge; the startup line says "spoken answers on".
