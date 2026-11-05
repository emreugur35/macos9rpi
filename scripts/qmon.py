#!/usr/bin/env python3
# qmon.py - send commands to the QEMU monitor socket
#   qmon.py <monitor.sock> "cmd1" "cmd2" ...   (use "sleep:0.5" for delays)
import socket, sys, time
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(sys.argv[1])
time.sleep(0.2)
s.recv(65536)
for c in sys.argv[2:]:
    if c.startswith('sleep:'):
        time.sleep(float(c[6:]))
        continue
    s.sendall((c + '\n').encode())
    time.sleep(0.15)
    try:
        s.settimeout(0.3)
        out = s.recv(65536).decode(errors='replace')
        lines = [l for l in out.splitlines() if l.strip() and not l.startswith('(qemu)') and c not in l]
        if lines:
            print('\n'.join(lines))
    except socket.timeout:
        pass
s.close()
