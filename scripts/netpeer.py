#!/usr/bin/env python3
#
# netpeer.py - Network test peer for macos9rpi in QEMU ("ether usb")
#
# QEMU tunnels the emulated USB Ethernet adapter's frames over UDP to this
# script, which plays a tiny LAN: it answers ARP, hands out 10.0.3.15 by
# DHCP, and then pings Mac OS with small and full-size packets, printing
# round-trip times and losses.
#
#   python3 scripts/netpeer.py [count] &
#   QEMU_BIN=tools/qemu-circle/bin/qemu-system-arm scripts/run_qemu.sh out/qemu-sd.img \
#     -device usb-net,netdev=n0 -netdev dgram,id=n0,local.type=inet,local.host=127.0.0.1,\
#   local.port=10110,remote.type=inet,remote.host=127.0.0.1,remote.port=10111
#
import socket, struct, sys, time

QEMU = ('127.0.0.1', 10110)
ME = ('127.0.0.1', 10111)
MY_MAC = bytes.fromhex('020000000001')
MY_IP = socket.inet_aton('10.0.3.2')
MAC_IP = socket.inet_aton('10.0.3.15')
BCAST = b'\xff' * 6
COUNT = int(sys.argv[1]) if len(sys.argv) > 1 else 60
SIZES = [56, 512, 1472]			# ICMP payload bytes (1472 = full 1514-byte frame)

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind(ME)
sock.settimeout(0.05)

def log(*a):
    print(time.strftime('%H:%M:%S'), *a, flush=True)

def csum(data):
    if len(data) % 2:
        data += b'\0'
    s = sum(struct.unpack('!%dH' % (len(data) // 2), data))
    s = (s >> 16) + (s & 0xffff)
    s += s >> 16
    return ~s & 0xffff

def send_eth(dst, etype, payload):
    frame = dst + MY_MAC + struct.pack('!H', etype) + payload
    sock.sendto(frame, QEMU)

def ip_packet(proto, src, dst, payload, ident=[0]):
    ident[0] = (ident[0] + 1) & 0xffff
    hdr = struct.pack('!BBHHHBBH4s4s', 0x45, 0, 20 + len(payload), ident[0], 0, 64, proto, 0, src, dst)
    hdr = hdr[:10] + struct.pack('!H', csum(hdr)) + hdr[12:]
    return hdr + payload

def udp(sport, dport, src, dst, data):
    u = struct.pack('!HHHH', sport, dport, 8 + len(data), 0) + data
    pseudo = src + dst + struct.pack('!BBH', 0, 17, len(u))
    c = csum(pseudo + u) or 0xffff
    return u[:6] + struct.pack('!H', c) + u[8:]

mac_hw = None			# Mac OS's MAC address
mac_ready_at = None		# when Mac OS owns its address (after ARP probes)
pings = {}			# seq -> (send time, size)
rtts = {s: [] for s in SIZES}
sent = {s: 0 for s in SIZES}

def handle_dhcp(frame, ip, data):
    global mac_hw, mac_ready_at
    if len(data) < 240 or data[0] != 1:
        return
    xid, chaddr = data[4:8], data[28:34]
    opts, i, msgtype = data[240:], 0, None
    while i < len(opts) and opts[i] != 255:
        if opts[i] == 0:
            i += 1
            continue
        if opts[i] == 53:
            msgtype = opts[i + 2]
        i += 2 + opts[i + 1]
    reply = {1: 2, 3: 5}.get(msgtype)	# Discover -> Offer, Request -> Ack
    if reply is None:
        return
    mac_hw = chaddr
    log('DHCP', {1: 'Discover', 3: 'Request'}[msgtype], 'from', chaddr.hex(':'), '->', {2: 'Offer', 5: 'Ack'}[reply], '10.0.3.15')
    bootp = struct.pack('!BBBB4sHH4s4s4s4s16s64s128s', 2, 1, 6, 0, xid, 0, 0x8000,
                        b'\0' * 4, MAC_IP, MY_IP, b'\0' * 4, chaddr + b'\0' * 10, b'', b'')
    o = bytes([53, 1, reply, 54, 4]) + MY_IP + bytes([51, 4]) + struct.pack('!I', 86400)
    o += bytes([1, 4, 255, 255, 255, 0, 3, 4]) + MY_IP + bytes([6, 4]) + MY_IP + bytes([255])
    payload = bootp + b'\x63\x82\x53\x63' + o
    pkt = ip_packet(17, MY_IP, b'\xff' * 4, udp(67, 68, MY_IP, b'\xff' * 4, payload))
    send_eth(BCAST, 0x0800, pkt)
    if reply == 5:
        mac_ready_at = time.time() + 12	# Mac OS ARP-probes its new address first

def handle(frame):
    global mac_ready_at
    if len(frame) < 14:
        return
    etype = struct.unpack('!H', frame[12:14])[0]
    if etype == 0x0806 and len(frame) >= 42:		# ARP
        op = struct.unpack('!H', frame[20:22])[0]
        sha, spa, tpa = frame[22:28], frame[28:32], frame[38:42]
        if op == 1 and tpa == MY_IP:
            arp = struct.pack('!HHBBH6s4s6s4s', 1, 0x0800, 6, 4, 2, MY_MAC, MY_IP, sha, spa)
            send_eth(sha, 0x0806, arp)
            log('ARP: told', spa and socket.inet_ntoa(spa), 'that 10.0.3.2 is', MY_MAC.hex(':'))
        elif spa == MAC_IP and tpa == MAC_IP:
            log('ARP: Mac OS announces 10.0.3.15')
    elif etype == 0x0800:
        ip = frame[14:]
        ihl = (ip[0] & 15) * 4
        proto, src = ip[9], ip[12:16]
        body = ip[ihl:struct.unpack('!H', ip[2:4])[0]]
        if proto == 17 and struct.unpack('!H', body[2:4])[0] == 67:
            handle_dhcp(frame, ip, body[8:])
        elif proto == 1 and body[0] == 0 and src == MAC_IP:	# echo reply
            seq = struct.unpack('!H', body[6:8])[0]
            if seq in pings:
                t, size = pings.pop(seq)
                rtt = (time.time() - t) * 1000
                ok = body[8:] == bytes((seq + i) & 0xff for i in range(size))
                rtts[size].append(rtt)
                if not ok:
                    log('ping seq', seq, 'size', size, 'PAYLOAD MISMATCH')
        elif proto == 1 and body[0] == 8:
            log('ICMP echo request from', socket.inet_ntoa(src))

def send_ping(seq, size):
    data = bytes((seq + i) & 0xff for i in range(size))
    icmp = struct.pack('!BBHHH', 8, 0, 0, 0x4d39, seq) + data
    icmp = icmp[:2] + struct.pack('!H', csum(icmp)) + icmp[4:]
    send_eth(mac_hw, 0x0800, ip_packet(1, MY_IP, MAC_IP, icmp))
    pings[seq] = (time.time(), size)
    sent[size] += 1

log('waiting for Mac OS on UDP', ME)
seq, next_ping, start = 0, 0, time.time()
while True:
    try:
        frame, _ = sock.recvfrom(4096)
        handle(frame)
    except socket.timeout:
        pass
    now = time.time()
    if mac_ready_at and now >= mac_ready_at and seq < COUNT * len(SIZES) and now >= next_ping:
        if seq == 0:
            log('pinging Mac OS (10.0.3.15)...')
        send_ping(seq, SIZES[seq % len(SIZES)])
        seq += 1
        next_ping = now + 0.25
    if seq >= COUNT * len(SIZES) and (not pings or now > next_ping + 5):
        break
    if now - start > 900:
        log('timeout')
        break

for s in SIZES:
    r = sorted(rtts[s])
    if r:
        log('ICMP %4d bytes: %d/%d replies, rtt min %.1f / median %.1f / max %.1f ms'
            % (s, len(r), sent[s], r[0], r[len(r) // 2], r[-1]))
    else:
        log('ICMP %4d bytes: 0/%d replies' % (s, sent[s]))
