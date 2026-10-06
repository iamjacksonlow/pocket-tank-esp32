#!/usr/bin/env python3
"""director.py - send a scenario command to the pocket-tank over USB serial
and print what the tank says back. The firmware's director console
(firmware/main/director.c) reads lines on the same port the log uses.

  tools/director.py hungry 3          # three fish starving, trickle held
  tools/director.py feed              # keeper pellets on cue
  tools/director.py state             # drives, pellets, veg, algae
  tools/director.py help              # the full list
  tools/director.py --watch           # just stream the log

A plain open leaves the tank running; forcing DTR/RTS low on open RESETS
the chip (USB-Serial-JTAG auto-reset), so don't. Port: first /dev/cu.usbmodem*,
or -p PATH. -t SECONDS how long to listen for the reply (default 2)."""
import sys, glob, time, argparse
import serial

ap = argparse.ArgumentParser()
ap.add_argument("-p", "--port")
ap.add_argument("-t", "--time", type=float, default=2.0)
ap.add_argument("--watch", action="store_true")
ap.add_argument("cmd", nargs="*")
a = ap.parse_args()
port = a.port or next(iter(sorted(glob.glob("/dev/cu.usbmodem*"))), None)
if not port:
    sys.exit("no /dev/cu.usbmodem* - is the tank awake (press BOOT) and on a data USB port?")
s = serial.Serial()
s.port, s.baudrate, s.timeout = port, 115200, 0.1
s.open()
s.reset_input_buffer()
if a.cmd:
    s.write((" ".join(a.cmd) + "\n").encode())
    s.flush()
end = time.time() + (1e9 if a.watch else a.time)
try:
    while time.time() < end:
        line = s.readline()
        if not line:
            continue
        text = line.decode("utf-8", "replace").rstrip()
        if a.watch or "director" in text or not a.cmd:
            print(text)
except KeyboardInterrupt:
    pass
s.close()
