#!/usr/bin/env python3
# fake-compositor.py SOCKET MODE: a stand-in Wayland compositor for the
# mesa-m3-session-confirm fixtures. It listens on SOCKET (a Unix socket) and,
# per MODE:
#   serve        answers wl_display.get_registry with one wl_compositor global
#                and wl_display.sync with wl_callback.done (a real round trip)
#   silent       accepts connections but never answers (socket there, no round trip)
#   die-after=S  serves for S seconds, then exits (reaches the target, then dies)
# The process environment is whatever the test gives it (MESA_M3_HOOK_RUN_ID,
# XDG_SESSION_ID), as a compositor's would be.
import os, socket, struct, sys, threading, time

path, mode = sys.argv[1], sys.argv[2]
try:
    os.unlink(path)
except FileNotFoundError:
    pass
srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
srv.bind(path)
srv.listen(16)

def wl_string(s):
    b = s.encode() + b'\0'
    return struct.pack('<I', len(b)) + b + b'\0' * (-len(b) % 4)

def event(obj, opcode, payload):
    return struct.pack('<II', obj, ((8 + len(payload)) << 16) | opcode) + payload

def client(conn):
    buf = b''
    serial = 1
    while True:
        try:
            data = conn.recv(4096)
        except OSError:
            return
        if not data:
            return
        if mode == 'silent':
            continue
        buf += data
        while len(buf) >= 8:
            obj, word = struct.unpack('<II', buf[:8])
            size, opcode = word >> 16, word & 0xffff
            if len(buf) < size:
                break
            msg, buf = buf[:size], buf[size:]
            if obj != 1:
                continue
            (new_id,) = struct.unpack('<I', msg[8:12])
            if opcode == 1:  # get_registry
                conn.sendall(event(new_id, 0, struct.pack('<I', 1) + wl_string('wl_compositor') + struct.pack('<I', 6)))
            elif opcode == 0:  # sync
                conn.sendall(event(new_id, 0, struct.pack('<I', serial)) + event(1, 1, struct.pack('<I', new_id)))
                serial += 1

if mode.startswith('die-after='):
    t = float(mode.split('=', 1)[1])
    mode = 'serve'
    threading.Timer(t, lambda: os._exit(0)).start()
while True:
    c, _ = srv.accept()
    threading.Thread(target=client, args=(c,), daemon=True).start()
