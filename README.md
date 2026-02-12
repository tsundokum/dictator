# Dictator

Voice-to-text tool for Ubuntu. Hold a configurable hotkey to dictate, release to transcribe and auto-paste.

## Build

```
sudo apt install libasound2-dev libcurl4-openssl-dev xdotool libx11-dev xclip
make
```

## Run

Create `.env` with your Groq API key:
```
GROQ=gsk_...
```

Then run:
```
./dictator
```

## Configuration

Optional config file at `/etc/dictator.conf`. If missing, defaults apply. Format is `key = value`, with `#` comments and blank lines allowed.

```ini
# /etc/dictator.conf
key = shift+F1
notify = true
autopaste = true
```

### Options

| Key | Description | Format | Default |
|-----|-------------|--------|---------|
| `key` | Hotkey combo | `[shift+][ctrl+][alt+][super+]KeyName` | `F1` |
| `notify` | Desktop notifications | `true` / `false` | `true` |
| `autopaste` | Auto Ctrl+V after clipboard copy | `true` / `false` | `true` |

- **KeyName** is any X11 keysym name recognized by `XStringToKeysym()` (e.g. `F1`, `F5`, `space`, `a`).
- Modifier prefixes are case-insensitive (`Shift+F1` and `shift+f1` prefixes both work, but the KeyName itself is case-sensitive per X11).
- When `autopaste = false`, text is copied to clipboard only (no simulated Ctrl+V).
- When `notify = false`, no `notify-send` desktop notifications are shown.
- Invalid key names cause a clear error on stderr and exit.

## Architecture

Single C file (`dictator.c`), two threads:

1. **Main thread** — loads config from `/etc/dictator.conf`, loads API key from `.env`, opens X11 display, grabs the configured hotkey (crossed with NumLock/CapsLock combos), then enters the event loop. On KeyPress it spawns the recording thread; on KeyRelease it joins the thread, builds a WAV, calls the Groq Whisper API, and pastes the result.

2. **Recording thread** — opens ALSA default capture device at 16kHz/mono/16-bit, reads PCM into a static buffer until the `recording` flag is cleared or the 60-second max is reached.

### Key components

| Section | What it does |
|---------|-------------|
| `cfg` struct + `load_config()` | Parses `/etc/dictator.conf`, sets hotkey/modifiers/notify/autopaste |
| `load_env()` | Reads `GROQ=` from `.env` |
| `notify()` | Fires `notify-send` (respects `cfg.notify`) |
| `record_thread()` | ALSA capture into `pcm_buf` |
| `build_wav()` | Wraps PCM buffer in a WAV header (in-memory) |
| `transcribe()` | POSTs WAV to Groq Whisper API via libcurl, returns text |
| `paste_text()` | Pipes text to `xclip`, optionally simulates Ctrl+V via `xdotool` |
| `main()` | Config/env init, X11 hotkey grab, event loop |

### Dependencies

- **libasound2** (ALSA) — audio capture
- **libcurl** — HTTP for Groq API
- **libX11** — global hotkey grab
- **xclip** — clipboard (runtime)
- **xdotool** — simulated keypress for paste (runtime)
- **notify-send** — desktop notifications (runtime, optional)

 Very good job.