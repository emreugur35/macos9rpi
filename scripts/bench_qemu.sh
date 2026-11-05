#!/bin/bash
#
# bench_qemu.sh - Boot a fresh QEMU image with extra prefs, run a fixed Finder
# window workload and report how busy the emulated CPU was (from the
# "jitsample" 60 Hz samples: everything but idle_wait counts as busy).
#   scripts/bench_qemu.sh "jit68k true"
#
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LOG="$ROOT/out/qemu-bench.log"
pkill -f qemu-system-arm 2>/dev/null
"$ROOT/scripts/fresh_qemu.sh" "jittest 5" "jitsample true" "$@" >/dev/null
(timeout 600 "$ROOT/scripts/run_qemu.sh" "$ROOT/out/qemu-sd.img" > "$LOG" 2>&1 &)
sleep ${SETTLE:-150}
START=$(wc -l < "$LOG")
args=()
for i in $(seq 1 10); do args+=("sendkey alt-n" "sleep:3" "sendkey alt-w" "sleep:2"); done
python3 "$ROOT/scripts/qmon.py" "$ROOT/out/qemu-monitor.sock" "${args[@]}"
sleep 3
END=$(wc -l < "$LOG")
IDLE=$(source "$ROOT/env.sh"; arm-none-eabi-nm "$ROOT/build/rpi1/kernel.elf" | awk '/ _Z9idle_waitv$/{print $1}')
python3 - "$LOG" "$START" "$END" "$IDLE" <<'PY'
import sys,re
log,start,end,idle=sys.argv[1],int(sys.argv[2]),int(sys.argv[3]),int(sys.argv[4],16)
lines=open(log,errors='replace').read().replace('\r','').split('\n')[start:end]
busy=[]
cur=None
for l in lines:
    if l.startswith('JIT samples'):
        if cur is not None: busy.append(cur)
        cur=100
    m=re.match(r'\s+arm ([0-9a-f]+): (\d+)%',l)
    if m and cur is not None and abs(int(m.group(1),16)-(idle&~63))<128:
        cur-=int(m.group(2))
if cur is not None: busy.append(cur)
print('busy %% per 10 s window during workload:', busy, ' average %.1f' % (sum(busy)/max(1,len(busy))))
PY
pkill -f qemu-system-arm
