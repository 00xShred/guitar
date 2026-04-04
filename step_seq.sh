#!/bin/bash
# Rhythmic step sequencer — steps through settings on a BPM grid
# Usage: ./step_seq.sh [bpm] [pattern]
# Patterns: crush, stutter, wave, chaos

PIPE=/tmp/crush_pipe
BPM=${1:-120}
PATTERN=${2:-crush}
STEP=$(echo "scale=4; 60 / $BPM" | bc)  # quarter note duration

send() {
    echo "$1 $2" > $PIPE
    sleep 0.02
    echo "$3 $4" > $PIPE
}

echo "BPM: $BPM | Pattern: $PATTERN | Ctrl+C to stop"

while true; do
    case "$PATTERN" in
        crush)
            # Alternate between clean and destroyed
            send bit 16 rate 1;  sleep $STEP
            send bit 4  rate 4;  sleep $STEP
            send bit 16 rate 1;  sleep $STEP
            send bit 2  rate 8;  sleep $STEP
            ;;
        stutter)
            # High rate reduction creates stutter effect
            send bit 8 rate 1;  sleep $STEP
            send bit 8 rate 16; sleep $STEP
            send bit 8 rate 1;  sleep $STEP
            send bit 8 rate 8;  sleep $(echo "$STEP / 2" | bc -l)
            send bit 8 rate 1;  sleep $(echo "$STEP / 2" | bc -l)
            ;;
        wave)
            # Sweep through bit depths in steps
            for b in 16 12 8 6 4 3 4 6 8 12; do
                send bit $b rate 2
                sleep $(echo "$STEP / 2" | bc -l)
            done
            ;;
        chaos)
            # Random settings each beat
            b=$((RANDOM % 14 + 2))
            r=$((RANDOM % 8 + 1))
            send bit $b rate $r
            sleep $STEP
            ;;
    esac
done
