#!/usr/bin/env python3
"""Sends an OSC message repeatedly (tests): osc_send.py port address value seconds"""
import socket, struct, sys, time

def pad(b):
    return b + b'\0' * (4 - len(b) % 4)

def message(address, value):
    if isinstance(value, float):
        return pad(address.encode()) + pad(b',f') + struct.pack('>f', value)
    if isinstance(value, int):
        return pad(address.encode()) + pad(b',i') + struct.pack('>i', value)
    return pad(address.encode()) + pad(b',s') + pad(value.encode('utf-8'))

port, address, value, seconds = int(sys.argv[1]), sys.argv[2], sys.argv[3], float(sys.argv[4])
try:
    value = int(value)
except ValueError:
    try:
        value = float(value)
    except ValueError:
        pass
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
end = time.time() + seconds
while time.time() < end:
    s.sendto(message(address, value), ('127.0.0.1', port))
    time.sleep(0.05)
