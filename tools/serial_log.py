#!/usr/bin/env python3
"""Log the ESP32 console to a file without resetting the board.

usage: serial_log.py PORT SECONDS LOGFILE [--reset]
--reset pulses RTS to reset the ESP32 first.
"""
import sys
import time

import serial

port, secs, path = sys.argv[1], float(sys.argv[2]), sys.argv[3]
s = serial.Serial()
s.port = port
s.baudrate = 115200
s.timeout = 0.2
s.dtr = False          # do not reset the ESP32 when opening the port
s.rts = False
s.open()
if '--reset' in sys.argv:
    s.rts = True
    time.sleep(0.1)
    s.rts = False
end = time.time() + secs
with open(path, 'a') as f:
    while time.time() < end:
        data = s.read(4096)
        if data:
            f.write(data.decode('utf-8', 'replace'))
            f.flush()
