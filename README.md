# Dictator

Voice-to-text tool for Ubuntu with X11. Hold a hotkey to dictate, release to transcribe. Two hotkeys: one copies to clipboard, the other copies and auto-pastes.

## Install

```bash
sudo apt install libasound2-dev libcurl4-openssl-dev xdotool libx11-dev xclip
make
```

Create `.env` with your [Groq API key](https://console.groq.com/keys):
```
GROQ=gsk_...
```

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

## Configuration

Optional config file at `/etc/dictator.conf`. If missing, defaults apply. Format is `key = value`, with `#` comments and blank lines allowed.

```ini
# /etc/dictator.conf
copy_key = F1
paste_key = shift+F1
notify = true
```

### Options

| Key | Description | Format | Default |
|-----|-------------|--------|---------|
| `copy_key` | Hotkey: transcribe + clipboard only | `[shift+][ctrl+][alt+][super+]KeyName` | `F1` |
| `paste_key` | Hotkey: transcribe + clipboard + Ctrl+V | `[shift+][ctrl+][alt+][super+]KeyName` | `shift+F1` |
| `notify` | Desktop notifications | `true` / `false` | `true` |

- **KeyName** is any X11 keysym name recognized by `XStringToKeysym()` (e.g. `F1`, `F5`, `space`, `a`).
- Modifier prefixes are case-insensitive (`Shift+F1` and `shift+f1` prefixes both work, but the KeyName itself is case-sensitive per X11).
- When `notify = false`, no `notify-send` desktop notifications are shown.
- Invalid key names cause a clear error on stderr and exit.
