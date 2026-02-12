# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What This Is

Dictator is a voice-to-text tool for Ubuntu/X11. Hold a hotkey to record audio via ALSA, release to transcribe via the Groq Whisper API, and auto-paste the result. Single C file architecture with two threads (main event loop + ALSA recording).

## Build & Test

```bash
# Install dependencies (Ubuntu)
sudo apt install libasound2-dev libcurl4-openssl-dev xdotool libx11-dev xclip

# Build
make

# Build and run tests (config parser unit tests)
make test
```

Linker flags: `-lX11 -lasound -lcurl -lpthread`

## Runtime Setup

Requires a `.env` file in the working directory with `GROQ=gsk_...` (Groq API key). Optional config at `/etc/dictator.conf`.

## Architecture

Everything is in `dictator.c`. Tests are in `test_config.c`, which `#include`s `dictator.c` directly (with `#define main dictator_main` to avoid symbol collision) so it can access the static `cfg` struct and `load_config_file()`.

Key statics: `pcm_buf` (recording buffer), `cfg` (config struct with key_name/mod_mask/notify/autopaste), `api_key` (loaded from .env).

Flow: `load_config()` → `load_env()` → X11 grab hotkey → event loop → on KeyPress spawn `record_thread()` → on KeyRelease join thread, `build_wav()` → `transcribe()` → `paste_text()`.
