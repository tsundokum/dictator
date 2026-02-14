# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build & Test

```bash
make        # build
make test   # config parser unit tests
make clean  # remove build artifacts
```

Build deps: `sudo apt install gcc libx11-dev libasound2-dev libcurl4-openssl-dev libevdev-dev`

Linker flags: `-lX11 -lasound -lcurl -lpthread -levdev`

Compile-time flags: `-DUSE_X11 -DUSE_EVDEV` (both enabled by default in Makefile)

## Architecture

Single C file (`dictator.c`), two threads, two backends (X11 and evdev/Wayland). Tests in `test_config.c` which `#include`s `dictator.c` directly (with `#define main dictator_main` to avoid symbol collision) so it can access the static `cfg` struct and `load_config_file()`.

Key statics: `pcm_buf` (recording buffer), `cfg` (config struct with copy_key/paste_key hotkey sub-structs and notify flag), `api_key` (loaded from .env), `active_backend` (detected at runtime).

### Backend detection

Runtime detection via `XDG_SESSION_TYPE`: if `"wayland"` → evdev backend, otherwise → X11 backend. Controlled by `detect_backend()`. Backend-specific code is guarded by `#ifdef USE_X11` / `#ifdef USE_EVDEV`.

### Backend-agnostic modifier flags

Config parsing uses `MOD_SHIFT`, `MOD_CTRL`, `MOD_ALT`, `MOD_SUPER` (not X11 masks). The X11 backend converts these to X11 masks via `mod_to_x11()` at the boundary.

### Flow

`load_config()` → `load_env()` → `detect_backend()` → backend-specific init + event loop → on key press spawn `record_thread()` → on key release join thread, `handle_recording_done()` (builds WAV, transcribes, pastes).

1. **Main thread** — dispatches to `run_x11()` or `run_evdev()` based on detected backend. Each runs its own event loop.

2. **Recording thread** — ALSA capture at 16kHz/mono/16-bit into `pcm_buf` until `recording` flag clears or 60s max.

### Key components

| Function / Symbol | What it does |
|---------|-------------|
| `cfg` struct + `load_config()` | Parses `/etc/dictator.conf`, sets copy_key/paste_key hotkeys and notify |
| `parse_hotkey()` | Parses modifier prefixes + key name into a `struct hotkey` (using `MOD_*` flags) |
| `detect_backend()` | Checks `XDG_SESSION_TYPE` to pick X11 or evdev backend |
| `load_env()` | Reads `ASSEMBLYAI=` from `.env` |
| `notify()` | Fires `notify-send` (respects `cfg.notify`) |
| `record_thread()` | ALSA capture into `pcm_buf` |
| `build_wav()` | Wraps PCM buffer in a WAV header (in-memory) |
| `transcribe()` | Uploads WAV to AssemblyAI, submits transcript job, polls for result |
| `handle_recording_done()` | Shared post-recording logic: build WAV → transcribe → paste |
| `paste_text()` | Clipboard + paste: `xclip`/`xdotool` on X11, `wl-copy`/`ydotool` on Wayland |
| `run_x11()` | X11 backend: hotkey grab, X11 event loop |
| `run_evdev()` | Evdev backend: device discovery, poll-based event loop with libevdev |
| `keyname_to_evdev()` | Maps config key names to evdev keycodes via static lookup table |
| `open_keyboard_device()` | Scans `/dev/input/event*` for first keyboard (EV_KEY + KEY_A) |
| `update_mod_state()` | Tracks evdev modifier key state (shift/ctrl/alt/super) |
| `main()` | Config/env init, backend detection, dispatches to `run_x11()` or `run_evdev()` |
