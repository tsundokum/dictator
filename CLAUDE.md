# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build & Test

```bash
make        # build
make test   # config parser unit tests
make clean  # remove build artifacts
```

Linker flags: `-lX11 -lasound -lcurl -lpthread`

## Architecture

Single C file (`dictator.c`), two threads. Tests in `test_config.c` which `#include`s `dictator.c` directly (with `#define main dictator_main` to avoid symbol collision) so it can access the static `cfg` struct and `load_config_file()`.

Key statics: `pcm_buf` (recording buffer), `cfg` (config struct with copy_key/paste_key hotkey sub-structs and notify flag), `api_key` (loaded from .env).

Flow: `load_config()` → `load_env()` → X11 grab hotkey → event loop → on KeyPress spawn `record_thread()` → on KeyRelease join thread, `build_wav()` → `transcribe()` → `paste_text()`.

1. **Main thread** — X11 event loop. On KeyPress spawns recording thread; on KeyRelease joins it, builds WAV, calls Groq API, pastes result.

2. **Recording thread** — ALSA capture at 16kHz/mono/16-bit into `pcm_buf` until `recording` flag clears or 60s max.

### Key components

| Function / Symbol | What it does |
|---------|-------------|
| `cfg` struct + `load_config()` | Parses `/etc/dictator.conf`, sets copy_key/paste_key hotkeys and notify |
| `parse_hotkey()` | Parses modifier prefixes + key name into a `struct hotkey` |
| `load_env()` | Reads `GROQ=` from `.env` |
| `notify()` | Fires `notify-send` (respects `cfg.notify`) |
| `record_thread()` | ALSA capture into `pcm_buf` |
| `build_wav()` | Wraps PCM buffer in a WAV header (in-memory) |
| `transcribe()` | POSTs WAV to Groq Whisper API via libcurl, returns text |
| `paste_text()` | Pipes text to `xclip`, optionally simulates Ctrl+V via `xdotool` (controlled by `autopaste` parameter) |
| `main()` | Config/env init, X11 hotkey grab, event loop |

