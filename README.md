# kb-debounce

A software debounce filter for mechanical keyboards on Linux. Fixes key chatter (double-typing) without replacing the switch.

Original repo : https://github.com/markdborden/kb-debounce.git

This is a C port (for lightness, performance and lower requirements) which also addresses problems with multiple keypresses and missed events the original implementation had. 

## The Problem

Mechanical keyboard switches wear out over time. When the metal contacts inside a switch start to degrade, they bounce on contact and register a single keypress as two. This is called **key chatter**, and it looks like this:

- You type "r" and get "rr"
- You type "the" and get "tthe"
- Every keystroke on the affected key fires twice

## How It Works

The script uses the `evdev` library to:

1. Find your keyboard's input device
2. Create a virtual clone of it via Linux's `uinput` subsystem
3. Grab exclusive access to the physical keyboard
4. Forward all events through, **except** rapid duplicate key-down events that arrive within the debounce threshold

A 75ms default threshold catches chatter (which happens in 2-10ms) while staying well below the fastest deliberate double-tap (~100-150ms). Normal typing, held keys, and key repeats are unaffected.

## Requirements

- libevdev-dev
- User must be in the `input` group: `sudo usermod -aG input $USER`

## Quick Install

```bash
git clone https://github.com/markdborden/kb-debounce.git
cd kb-debounce
chmod +x install.sh
./install.sh
```

The installer will detect your keyboard, ask for your preferred threshold, and set up a systemd user service that starts on login.

## Manual Usage

```bash
# List available keyboards
./kb-debounce --list

# Run with auto-detect
./kb-debounce

# Target a specific keyboard by name
./kb-debounce --name "BlackWidow"

# Target by device path
./kb-debounce --device /dev/input/event5

# Custom threshold (in ms)
./kb-debounce --threshold 50
```

## Useful Commands

```bash
systemctl --user status kb-debounce     # check if running
systemctl --user stop kb-debounce       # stop the filter
systemctl --user restart kb-debounce    # restart after config change
journalctl --user -u kb-debounce        # view logs
```

## Tuning the Threshold

| Threshold | Use Case |
|-----------|----------|
| 30ms | Mild chatter, fast-paced gaming |
| 50-75ms | Most worn switches (recommended) |
| 100ms+ | Severe chatter, but may affect fast double-taps |

Edit the threshold in the service file and restart:

```bash
nano ~/.config/systemd/user/kb-debounce.service
systemctl --user daemon-reload
systemctl --user restart kb-debounce.service
```

## Uninstall

```bash
chmod +x uninstall.sh
./uninstall.sh
```

## License

MIT
