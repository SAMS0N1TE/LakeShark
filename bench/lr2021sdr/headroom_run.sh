# headroom_run.sh <tag> [board ip]: 60 s of 500k CS16 and 60 s of 1M IQ8 (source 1, code 25)
# over Wi-Fi, printing RX_END (FIFO peak, worst service gap, flags) for each. Git Bash.
IP=${2:-192.168.16.208}
B="$(cygpath -w "$(cd "$(dirname "$0")" && pwd)")\\bridge_test.py"
cd "${TEMP:-/tmp}"   # python -I from a neutral directory
for i in $(seq 1 30); do ping -n 1 -w 1000 $IP >/dev/null && break; sleep 1; done; sleep 4
echo "== $1 500k CS16"
timeout 120 python -I -u "$B" 0 --tcp $IP --hz 151600000 --bw 125000 --secs 60 2>&1 | grep -iE "peak|gap|flag|pair rate|host:|status|error" | grep -v "^gaps (t" | head -8
sleep 2
echo "== $1 1M IQ8"
timeout 120 python -I -u "$B" 0 --tcp $IP --hz 151600000 --route 2 --input 25 --secs 60 2>&1 | grep -iE "peak|gap|flag|pair rate|host:|status|error" | grep -v "^gaps (t" | head -8
