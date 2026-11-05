#!/usr/bin/env python3
# screenshot.py - take a QEMU screendump via the monitor socket and save as PNG
#   screenshot.py <monitor.sock> <out.png>
import socket, sys, time, zlib, struct, os

sock_path, out_png = sys.argv[1], sys.argv[2]
ppm = out_png + '.ppm'
if os.path.exists(ppm):
    os.remove(ppm)
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(sock_path)
time.sleep(0.2)
s.recv(65536)
s.sendall(('screendump %s\n' % ppm).encode())
for _ in range(50):
    time.sleep(0.1)
    if os.path.exists(ppm) and os.path.getsize(ppm) > 15:
        break
time.sleep(0.3)
s.close()

data = open(ppm, 'rb').read()
# parse P6 header
parts = []
i = 0
while len(parts) < 4:
    while data[i:i+1].isspace():
        i += 1
    if data[i:i+1] == b'#':
        while data[i:i+1] != b'\n':
            i += 1
        continue
    j = i
    while not data[j:j+1].isspace():
        j += 1
    parts.append(data[i:j])
    i = j
i += 1
w, h = int(parts[1]), int(parts[2])
pix = data[i:i + w * h * 3]
raw = b''.join(b'\0' + pix[y * w * 3:(y + 1) * w * 3] for y in range(h))
def chunk(t, d):
    return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) \
      + chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b'')
open(out_png, 'wb').write(png)
os.remove(ppm)
print('%dx%d -> %s' % (w, h, out_png))
