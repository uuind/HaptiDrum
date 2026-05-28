"""
drum_sound_player.py
=====================================================================
Listens to the Arduino haptic-drumset over USB serial and plays a
sound for each drum strike, with playback volume scaled to strike
force.

EXPECTED SERIAL MESSAGE (one per line, from the Arduino):
    DRUM,<id>,<force>
        <id>    : integer drum number   (1..4)
        <force> : integer strike force  (e.g. the ADC "jump" value)
    example:  DRUM,2,480

SETUP
  1. pip install pyserial pygame
  2. Put four sound files next to this script (see SOUND_FILES below).
  3. Set SERIAL_PORT to your Arduino's port:
        Windows : "COM3", "COM4", ...
        macOS   : "/dev/cu.usbmodem________"
        Linux   : "/dev/ttyACM0", "/dev/ttyUSB0"
     Or leave it as "AUTO" to let the script try to find it.
  4. Run:  python drum_sound_player.py
=====================================================================
"""

import sys
import time
import serial
import serial.tools.list_ports
import pygame

# #####################################################################
# ##                    >>>  CONFIG SECTION  <<<                     ##
# ##           Adjust these while setting up / tuning.               ##
# #####################################################################

# --- Serial port -----------------------------------------------------
# Set explicitly (e.g. "COM3" or "/dev/cu.usbmodem1101"),
# or leave "AUTO" to auto-detect the first Arduino-like port.
SERIAL_PORT = "COM5"
BAUD_RATE   = 9600           # MUST match Serial.begin() in the sketch

# --- Sound file for each drum id ------------------------------------
# Keys are the drum numbers sent by the Arduino. Values are file paths
# (put the .wav files in the same folder as this script).
SOUND_FILES = {
    1: "kick.wav",
    2: "snare.wav",
    3: "hihat.wav",
    4: "tom.wav"
}

# --- Force -> volume mapping -----------------------------------------
# Incoming force values are clamped to [FORCE_MIN, FORCE_MAX], then
# mapped linearly to a playback volume in [VOLUME_MIN, VOLUME_MAX].
# Tune FORCE_MIN/MAX to the range your FSRs actually produce.
FORCE_MIN  = 100            # force at/below this -> quietest
FORCE_MAX  = 800            # force at/above this -> loudest
VOLUME_MIN = 0.15           # pygame volume range is 0.0 - 1.0
VOLUME_MAX = 1.00

# --- Playback --------------------------------------------------------
# Channels = how many sounds can overlap at once. 8 is plenty.
MIX_CHANNELS = 8
# Small audio buffer = lower latency. 512 is a good low-latency value;
# raise to 1024 if you hear crackling/underruns.
AUDIO_BUFFER = 512

# --- Debug -----------------------------------------------------------
VERBOSE = True              # print every strike to the console

# #####################################################################
# ##                  >>>  END CONFIG SECTION  <<<                   ##
# #####################################################################


def find_serial_port():
    """Return a likely Arduino serial port, or None if not found."""
    ports = list(serial.tools.list_ports.comports())
    if not ports:
        return None
    # Prefer ports whose description hints at an Arduino / USB serial.
    hints = ("arduino", "usbmodem", "usbserial", "ttyacm", "ttyusb", "ch340", "wchusb")
    for p in ports:
        text = (p.description + " " + p.device).lower()
        if any(h in text for h in hints):
            return p.device
    # Otherwise just return the first available port.
    return ports[0].device


def open_serial():
    """Open and return the serial connection, or exit on failure."""
    port = SERIAL_PORT
    if port == "AUTO":
        port = find_serial_port()
        if port is None:
            print("ERROR: no serial ports found. Is the Arduino plugged in?")
            sys.exit(1)
        print(f"Auto-detected serial port: {port}")

    try:
        ser = serial.Serial(port, BAUD_RATE, timeout=0.05)
    except serial.SerialException as e:
        print(f"ERROR: could not open serial port '{port}': {e}")
        print("Available ports:")
        for p in serial.tools.list_ports.comports():
            print(f"   {p.device}  -  {p.description}")
        sys.exit(1)

    # Many Arduino boards reset when the serial port opens; wait it out.
    time.sleep(2.0)
    ser.reset_input_buffer()
    print(f"Serial connected on {port} at {BAUD_RATE} baud.")
    return ser


def load_sounds():
    """Load every drum sound file. Exit if one is missing."""
    sounds = {}
    for drum_id, path in SOUND_FILES.items():
        try:
            sounds[drum_id] = pygame.mixer.Sound(path)
        except (pygame.error, FileNotFoundError) as e:
            print(f"ERROR: could not load sound for drum {drum_id} "
                  f"('{path}'): {e}")
            print("Make sure the .wav files sit next to this script.")
            sys.exit(1)
    print(f"Loaded {len(sounds)} drum sounds: "
          f"{', '.join(SOUND_FILES[k] for k in sorted(SOUND_FILES))}")
    return sounds


def force_to_volume(force):
    """Map a raw strike-force value to a 0.0-1.0 playback volume."""
    # Clamp the force into the configured window.
    f = max(FORCE_MIN, min(FORCE_MAX, force))
    # Linear interpolation from force window to volume window.
    frac = (f - FORCE_MIN) / (FORCE_MAX - FORCE_MIN)
    return VOLUME_MIN + frac * (VOLUME_MAX - VOLUME_MIN)


def parse_line(line):
    """
    Parse one serial line. Return (drum_id, force) or None if the line
    is not a valid strike message. Bad/garbled lines are skipped.
    """
    line = line.strip()
    if not line.startswith("DRUM"):
        return None
    parts = line.split(",")
    if len(parts) != 3:
        return None
    try:
        drum_id = int(parts[1])
        force   = int(parts[2])
    except ValueError:
        return None
    return drum_id, force


def play_strike(sounds, drum_id, force):
    """Play the sound for one strike at a force-scaled volume."""
    sound = sounds.get(drum_id)
    if sound is None:
        if VERBOSE:
            print(f"  (no sound mapped for drum {drum_id} - ignored)")
        return

    volume = force_to_volume(force)

    # Grab a free mixer channel so overlapping strikes don't cut each
    # other off. find_channel(True) forces the oldest channel if all
    # are busy, so a strike never gets silently dropped.
    channel = pygame.mixer.find_channel(True)
    channel.set_volume(volume)
    channel.play(sound)

    if VERBOSE:
        print(f"  drum {drum_id}  force {force:>4}  ->  volume {volume:0.2f}")


def main():
    print("=== Haptic Drumset - Sound Player ===")

    # --- Audio init (do this before loading sounds) ---
    # Pre-init lets us request a small buffer for low latency.
    pygame.mixer.pre_init(frequency=44100, size=-16, channels=2,
                          buffer=AUDIO_BUFFER)
    pygame.init()
    pygame.mixer.set_num_channels(MIX_CHANNELS)

    sounds = load_sounds()
    ser = open_serial()

    print("Listening for strikes. Press Ctrl+C to quit.\n")

    try:
        while True:
            # readline() returns b"" on timeout - just loop again.
            raw = ser.readline()
            if not raw:
                continue

            # Decode defensively; ignore any non-text bytes.
            line = raw.decode("utf-8", errors="ignore")

            result = parse_line(line)
            if result is None:
                # Not a strike message (could be a boot banner, noise,
                # or a partial line). Optionally show it in verbose mode.
                if VERBOSE and line.strip():
                    print(f"  [ignored] {line.strip()}")
                continue

            drum_id, force = result
            play_strike(sounds, drum_id, force)

    except KeyboardInterrupt:
        print("\nStopping (Ctrl+C).")
    finally:
        ser.close()
        pygame.mixer.quit()
        pygame.quit()
        print("Serial closed, audio shut down. Bye.")


if __name__ == "__main__":
    main()
