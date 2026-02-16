# Dictator

Voice-to-text tool for Ubuntu. Hold a hotkey to dictate, release to transcribe. Three hotkeys: one copies to clipboard, one copies and auto-pastes, and one translates to English and pastes. Works on both X11 and Wayland.

## Install

### Build dependencies

```bash
sudo apt install gcc libasound2-dev libcurl4-openssl-dev libx11-dev libevdev-dev
```

### Runtime dependencies

**X11:** `xdotool`, `xclip`
```bash
sudo apt install xdotool xclip
```

**Wayland:** `wl-clipboard`, `ydotool` + user must be in the `input` group
```bash
sudo apt install wl-clipboard ydotool
sudo usermod -aG input $USER   # log out and back in
```

### Build

```bash
make
```

Create `.env` with your [Groq API key](https://console.groq.com/keys) and/or [AssemblyAI API key](https://www.assemblyai.com/):
```
GROQ=gsk_...
ASSEMBLYAI=...
```

Groq is the primary transcription backend. If both keys are present, AssemblyAI is used as a fallback when Groq fails. At least one key is required.

Install and enable the systemd service:
```bash
make install
systemctl --user start dictator.service
```

Check status / logs:
```bash
systemctl --user status dictator.service
journalctl --user -u dictator.service -f
```

## Uninstall

```bash
make uninstall
```

Your API key in `~/.config/dictator/.env` is preserved.

## How it works

The backend is detected automatically at startup via `XDG_SESSION_TYPE`:

- **X11** — uses `XGrabKey` for global hotkeys, `xclip` for clipboard, `xdotool` for paste simulation
- **Wayland** — uses evdev (`/dev/input/event*`) for global hotkeys, `wl-copy` for clipboard, `ydotool` for paste simulation

ALSA recording and transcription (Groq primary, AssemblyAI fallback) work identically on both.

## Configuration

Optional config file at `/etc/dictator.conf`. If missing, defaults apply. Format is `key = value`, with `#` comments and blank lines allowed.

```ini
# /etc/dictator.conf
speech2text_key = F1
speech2text_paste_key = shift+F1
speech2text_translate_paste_key = ctrl+F1
notify = true
# optional, default: whisper-large-v3
groq_model = whisper-large-v3
# optional, default: no proxy
proxy = http://user:pass@www.example.com:1234
```

### Options

| Key | Description | Format | Default |
|-----|-------------|--------|---------|
| `speech2text_key` | Hotkey: transcribe + clipboard only | `[shift+][ctrl+][alt+][super+]KeyName` | `F1` |
| `speech2text_paste_key` | Hotkey: transcribe + clipboard + paste | `[shift+][ctrl+][alt+][super+]KeyName` | `shift+F1` |
| `speech2text_translate_paste_key` | Hotkey: translate to English + paste | `[shift+][ctrl+][alt+][super+]KeyName` | `ctrl+F1` |
| `notify` | Desktop notifications | `true` / `false` | `true` |
| `groq_model` | Groq Whisper model name | string | `whisper-large-v3` |


- **KeyName** on X11: any keysym name recognized by `XStringToKeysym()` (e.g. `F1`, `F5`, `space`, `a`). Case-sensitive.
- **KeyName** on Wayland: looked up from a built-in table (`F1`–`F12`, `a`–`z`, `0`–`9`, `space`, `Return`, `Tab`, etc.). Case-insensitive.
- Modifier prefixes are case-insensitive on both backends (`Shift+F1` and `shift+F1` both work).
- When `notify = false`, no `notify-send` desktop notifications are shown.
- Invalid key names cause a clear error on stderr and exit.
