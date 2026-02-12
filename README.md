# Dictator

Voice-to-text tool for Ubuntu with X11. Hold a configurable hotkey to dictate, release to transcribe and auto-paste from clipboard

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
