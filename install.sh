#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
INSTALL_PATH="$HOME/.local/bin/kb-debounce"
SERVICE_PATH="$HOME/.config/systemd/user/kb-debounce.service"

echo "kb-debounce installer"
echo "====================="
echo ""

# Check for python3-evdev
if ! python3 -c "import evdev" 2>/dev/null; then
    echo "python-evdev is required but not installed."
    echo "Install it with:  pip install evdev"
    echo "            or:   sudo apt install python3-evdev"
    exit 1
fi

# Check input group
if ! groups | grep -qw input; then
    echo "Warning: your user is not in the 'input' group."
    echo "You may need to run:  sudo usermod -aG input \$USER"
    echo "Then log out and back in."
    echo ""
fi

# Check uinput access
if [ ! -w /dev/uinput ]; then
    echo "Warning: /dev/uinput is not writable by your user."
    echo "You may need to add your user to the 'input' group."
    echo ""
fi

# Detect keyboard
echo "Detecting keyboards..."
echo ""
"$SCRIPT_DIR/kb-debounce" --list
echo ""

# Prompt for keyboard name
read -rp "Enter a name fragment to match your keyboard (or leave blank for auto-detect): " KB_NAME
read -rp "Debounce threshold in ms (default: 75): " THRESHOLD
THRESHOLD="${THRESHOLD:-75}"

# Install script
mkdir -p "$(dirname "$INSTALL_PATH")"
cp "$SCRIPT_DIR/kb-debounce" "$INSTALL_PATH"
chmod +x "$INSTALL_PATH"
echo "Installed script to $INSTALL_PATH"

# Build ExecStart line
EXEC_START="$INSTALL_PATH --threshold $THRESHOLD"
if [ -n "$KB_NAME" ]; then
    EXEC_START="$EXEC_START --name '$KB_NAME'"
fi

# Create systemd service
mkdir -p "$(dirname "$SERVICE_PATH")"
cat > "$SERVICE_PATH" << EOF
[Unit]
Description=Keyboard debounce filter

[Service]
Type=simple
ExecStart=$EXEC_START
Restart=on-failure
RestartSec=3

[Install]
WantedBy=default.target
EOF

echo "Created systemd service at $SERVICE_PATH"

# Enable and start
systemctl --user daemon-reload
systemctl --user enable kb-debounce.service
systemctl --user start kb-debounce.service

echo ""
echo "Done! Debounce filter is running."
echo ""
echo "Useful commands:"
echo "  systemctl --user status kb-debounce    # check status"
echo "  systemctl --user stop kb-debounce      # stop"
echo "  systemctl --user restart kb-debounce   # restart"
echo "  journalctl --user -u kb-debounce       # view logs"
