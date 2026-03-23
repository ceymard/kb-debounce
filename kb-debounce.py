#!/usr/bin/env python3
"""
Keyboard debounce filter for Linux.

Intercepts a keyboard's evdev device, creates a virtual clone via uinput,
and suppresses key-down events that arrive too quickly after the previous
key-down for the same key. Eliminates key chatter / double-firing on
mechanical keyboards with worn switches.

Only filters rapid duplicate presses. Normal typing, held-key repeats,
and key-up events are unaffected.

Requirements:
    - Python 3
    - python-evdev (pip install evdev)
    - User must be in the 'input' group (or run as root)
"""

import argparse
import sys
import time

import evdev
from evdev import UInput, ecodes

DEFAULT_DEBOUNCE_MS = 75
DEFAULT_NAME_FRAGMENT = None


def list_keyboards():
    """List all keyboard input devices."""
    print("Available keyboard devices:\n")
    for path in evdev.list_devices():
        dev = evdev.InputDevice(path)
        caps = dev.capabilities(verbose=False)
        if ecodes.EV_KEY in caps and len(caps[ecodes.EV_KEY]) > 50:
            key_count = len(caps[ecodes.EV_KEY])
            print(f"  {dev.path}  {dev.name}  ({key_count} keys)")
    print("\nUse -d /dev/input/eventX to select a device,")
    print("or -n 'partial name' to match by name.")


def find_keyboard(name_fragment=None):
    """Find a keyboard evdev device, optionally matching by name."""
    for path in evdev.list_devices():
        dev = evdev.InputDevice(path)
        caps = dev.capabilities(verbose=False)
        if ecodes.EV_KEY in caps and len(caps[ecodes.EV_KEY]) > 100:
            if name_fragment is None or name_fragment.lower() in dev.name.lower():
                return dev
    return None


def run(debounce_ms, device_path=None, name_fragment=None):
    if device_path:
        kbd = evdev.InputDevice(device_path)
    else:
        kbd = find_keyboard(name_fragment)

    if kbd is None:
        print("No keyboard device found.", file=sys.stderr)
        print("Run with --list to see available devices.", file=sys.stderr)
        sys.exit(1)

    print(f"Debouncing: {kbd.name} ({kbd.path}), threshold={debounce_ms}ms")

    ui = UInput.from_device(kbd, name=f"{kbd.name} (debounced)")
    kbd.grab()

    last_down = {}
    debounce_s = debounce_ms / 1000.0

    try:
        for event in kbd.read_loop():
            if event.type == ecodes.EV_KEY:
                key_event = evdev.categorize(event)
                if key_event.keystate == evdev.KeyEvent.key_down:
                    now = time.monotonic()
                    if (now - last_down.get(event.code, 0)) < debounce_s:
                        continue
                    last_down[event.code] = now

            ui.write_event(event)
            ui.syn()
    except KeyboardInterrupt:
        pass
    finally:
        kbd.ungrab()
        ui.close()
        print("Debounce stopped, keyboard restored.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description="Software debounce filter for mechanical keyboards on Linux."
    )
    parser.add_argument(
        "-t", "--threshold", type=int, default=DEFAULT_DEBOUNCE_MS,
        help=f"Debounce threshold in milliseconds (default: {DEFAULT_DEBOUNCE_MS})"
    )
    parser.add_argument(
        "-d", "--device", type=str, default=None,
        help="Evdev device path, e.g. /dev/input/event5"
    )
    parser.add_argument(
        "-n", "--name", type=str, default=DEFAULT_NAME_FRAGMENT,
        help="Match keyboard by name fragment, e.g. 'BlackWidow' or 'Corsair'"
    )
    parser.add_argument(
        "--list", action="store_true",
        help="List available keyboard devices and exit"
    )
    args = parser.parse_args()

    if args.list:
        list_keyboards()
        sys.exit(0)

    run(args.threshold, args.device, args.name)
