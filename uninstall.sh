#!/usr/bin/env bash
# uninstall.sh - Remove Clean Mic service, configuration, CLI, and plugin
set -euo pipefail

echo "==> Stopping and disabling clean-mic.service if active..."
systemctl --user stop clean-mic.service 2>/dev/null || true
systemctl --user disable clean-mic.service 2>/dev/null || true

echo "==> Removing systemd unit..."
rm -f "$HOME/.config/systemd/user/clean-mic.service"
systemctl --user daemon-reload 2>/dev/null || true

echo "==> Removing binaries..."
rm -f "$HOME/.local/bin/cleanmic"
rm -f "$HOME/.local/bin/cleanmic-gui"
rm -f "$HOME/.local/share/applications/cleanmic.desktop"
if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database "$HOME/.local/share/applications" 2>/dev/null || true
fi

echo "==> Removing plugin library..."
rm -rf "$HOME/.local/lib/cleanmic"

echo "==> Removing configuration..."
rm -rf "$HOME/.config/cleanmic"

echo "Clean Mic uninstallation completed successfully."
