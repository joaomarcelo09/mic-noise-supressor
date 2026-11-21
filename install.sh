#!/usr/bin/env bash
# install.sh - Install Clean Mic LADSPA plugin, pipewire config, systemd unit, and CLI
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "==> Building LADSPA plugin..."
make -C "$SCRIPT_DIR/plugin"

PLUGIN_SO="$SCRIPT_DIR/build/librnnoise_gate_ladspa.so"
if [ ! -f "$PLUGIN_SO" ]; then
    echo "Error: plugin binary not found at $PLUGIN_SO" >&2
    exit 1
fi

echo "==> Installing plugin to ~/.local/lib/cleanmic/..."
mkdir -p "$HOME/.local/lib/cleanmic"
INSTALLED_SO="$HOME/.local/lib/cleanmic/librnnoise_gate_ladspa.so"
OLD_SO_SUM=""
if [ -f "$INSTALLED_SO" ]; then
    OLD_SO_SUM="$(sha256sum "$INSTALLED_SO" | cut -d' ' -f1)"
fi
install -m 755 "$PLUGIN_SO" "$INSTALLED_SO"
PLUGIN_CHANGED=0
if [ "$OLD_SO_SUM" != "$(sha256sum "$INSTALLED_SO" | cut -d' ' -f1)" ]; then
    PLUGIN_CHANGED=1
fi

echo "==> Setting up configuration in ~/.config/cleanmic/..."
mkdir -p "$HOME/.config/cleanmic"
install -m 644 "$SCRIPT_DIR/pipewire/clean-mic.conf.in" "$HOME/.config/cleanmic/clean-mic.conf.in"

# Install CLI tool
echo "==> Installing CLI to ~/.local/bin/cleanmic..."
mkdir -p "$HOME/.local/bin"
install -m 755 "$SCRIPT_DIR/bin/cleanmic" "$HOME/.local/bin/cleanmic"

# Write default settings if missing
SETTINGS_FILE="$HOME/.config/cleanmic/settings.json"
if [ ! -f "$SETTINGS_FILE" ]; then
    echo "==> Writing initial settings.json..."
    # Detect default input source
    DEFAULT_SRC=""
    if command -v pactl >/dev/null 2>&1; then
        DEFAULT_SRC="$(pactl get-default-source 2>/dev/null || true)"
        if [[ -z "$DEFAULT_SRC" || "$DEFAULT_SRC" == *"clean_mic"* || "$DEFAULT_SRC" == *".monitor" ]]; then
            DEFAULT_SRC=""
        fi
    fi
    if [ -z "$DEFAULT_SRC" ]; then
        DEFAULT_SRC="$("$HOME/.local/bin/cleanmic" sources 2>/dev/null | python3 -c 'import sys, json; s=json.load(sys.stdin); print(s[0] if s else "")' || true)"
    fi

    cat > "$SETTINGS_FILE" <<EOF
{
  "source": "${DEFAULT_SRC}",
  "suppression": 1.0,
  "gate_enable": 1,
  "vad_threshold": 0.6,
  "release_ms": 200.0,
  "floor_db": -80.0
}
EOF
else
    # Migrate floor_db if exactly the old default -60.0
    python3 -c "
import json
from pathlib import Path
p = Path('$SETTINGS_FILE')
try:
    d = json.loads(p.read_text(encoding='utf-8'))
    if isinstance(d, dict) and d.get('floor_db') in (-60.0, -60, '-60.0', '-60'):
        d['floor_db'] = -80.0
        p.write_text(json.dumps(d, indent=2) + '\n', encoding='utf-8')
        print('==> Migrated floor_db from -60.0 to -80.0 in settings.json')
except Exception:
    pass
" || true
fi

# (Re)generate clean-mic.conf from settings on every install, so template
# updates and settings migrations (e.g. floor_db) reach the conf.
CONF_FILE="$HOME/.config/cleanmic/clean-mic.conf"
OLD_CONF_SUM=""
if [ -f "$CONF_FILE" ]; then
    OLD_CONF_SUM="$(sha256sum "$CONF_FILE" | cut -d' ' -f1)"
fi
echo "==> Generating clean-mic.conf from settings..."
"$HOME/.local/bin/cleanmic" generate-conf
NEW_CONF_SUM="$(sha256sum "$CONF_FILE" | cut -d' ' -f1)"
CONF_CHANGED=0
if [ "$OLD_CONF_SUM" != "$NEW_CONF_SUM" ]; then
    CONF_CHANGED=1
fi

# Install systemd user service unit
echo "==> Installing systemd user unit to ~/.config/systemd/user/clean-mic.service..."
mkdir -p "$HOME/.config/systemd/user"
install -m 644 "$SCRIPT_DIR/systemd/clean-mic.service" "$HOME/.config/systemd/user/clean-mic.service"

# Install GUI and desktop file if present
if [ -f "$SCRIPT_DIR/gui/cleanmic_gui.py" ]; then
    echo "==> Installing GUI..."
    install -m 755 "$SCRIPT_DIR/gui/cleanmic_gui.py" "$HOME/.local/bin/cleanmic-gui"
fi

if [ -f "$SCRIPT_DIR/gui/cleanmic.desktop" ] || [ -f "$SCRIPT_DIR/cleanmic.desktop" ]; then
    echo "==> Installing desktop file to ~/.local/share/applications/cleanmic.desktop..."
    mkdir -p "$HOME/.local/share/applications"
    cat > "$HOME/.local/share/applications/cleanmic.desktop" <<EOF
[Desktop Entry]
Name=Clean Mic
Comment=Real-time microphone noise suppression and voice gating
Exec=$HOME/.local/bin/cleanmic-gui
Icon=audio-input-microphone
Terminal=false
Type=Application
Categories=AudioVideo;Audio;
Keywords=microphone;audio;noise;suppression;filter;pipewire;
StartupNotify=true
StartupWMClass=io.github.cleanmic.CleanMic
EOF
    chmod 644 "$HOME/.local/share/applications/cleanmic.desktop"
    if command -v update-desktop-database >/dev/null 2>&1; then
        update-desktop-database "$HOME/.local/share/applications" 2>/dev/null || true
    fi
fi

echo "==> Reloading systemd user daemon..."
systemctl --user daemon-reload

# Restart the service only if it is already running from this HOME's unit and
# the conf or the plugin changed (a running process keeps the old plugin
# loaded); never enable or start it here.
UNIT_PATH="$HOME/.config/systemd/user/clean-mic.service"
if { [ "$CONF_CHANGED" = 1 ] || [ "$PLUGIN_CHANGED" = 1 ]; } \
    && systemctl --user is-active --quiet clean-mic.service 2>/dev/null \
    && [ "$(systemctl --user show clean-mic.service -p FragmentPath --value 2>/dev/null)" = "$UNIT_PATH" ]; then
    echo "==> Configuration or plugin changed; restarting running clean-mic.service..."
    systemctl --user reset-failed clean-mic.service 2>/dev/null || true
    systemctl --user restart clean-mic.service
fi

echo "Clean Mic installation completed successfully."
