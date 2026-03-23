#!/bin/bash
set -e

INSTALL_PATH="$HOME/.local/bin/kb-debounce.py"
SERVICE_PATH="$HOME/.config/systemd/user/kb-debounce.service"

echo "Uninstalling kb-debounce..."

# Stop and disable service
if systemctl --user is-active kb-debounce.service &>/dev/null; then
    systemctl --user stop kb-debounce.service
    echo "Stopped service."
fi

if systemctl --user is-enabled kb-debounce.service &>/dev/null; then
    systemctl --user disable kb-debounce.service
    echo "Disabled service."
fi

# Remove files
[ -f "$SERVICE_PATH" ] && rm "$SERVICE_PATH" && echo "Removed $SERVICE_PATH"
[ -f "$INSTALL_PATH" ] && rm "$INSTALL_PATH" && echo "Removed $INSTALL_PATH"

systemctl --user daemon-reload

echo "Done. Keyboard debounce filter has been removed."
