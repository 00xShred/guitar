#!/bin/bash
# LFO-style parameter sweeper for live use
# Usage: ./sweep.sh [mode] [speed]
# Modes: bit, rate, both, drive, tone, all
# Speed: slow, mid, fast (default: mid)

PIPE=/tmp/crush_pipe
MODE=${1:-bit}
SPEED=${2:-mid}

case "$SPEED" in
    slow) STEP_TIME=0.15 ;;
    mid)  STEP_TIME=0.07 ;;
    fast) STEP_TIME=0.03 ;;
    *)    STEP_TIME=0.07 ;;
esac

echo "Sweeping: $MODE | Speed: $SPEED | Ctrl+C to stop"

BIT=16.0
RATE=1
BIT_DIR=-1
RATE_DIR=1

DRIVE=0.0
DRIVE_DIR=1

TONE=0.3
TONE_DIR=1

send() {
    echo "$1 $2" > $PIPE
}

while true; do
    case "$MODE" in
        bit)
            BIT=$(echo "$BIT + $BIT_DIR * 0.3" | bc)
            (( $(echo "$BIT <= 1.5" | bc -l) )) && BIT_DIR=1
            (( $(echo "$BIT >= 16"  | bc -l) )) && BIT_DIR=-1
            send bit $BIT
            ;;
        rate)
            RATE=$((RATE + RATE_DIR))
            (( RATE <= 1  )) && RATE_DIR=1
            (( RATE >= 16 )) && RATE_DIR=-1
            send rate $RATE
            ;;
        both)
            BIT=$(echo "$BIT + $BIT_DIR * 0.3" | bc)
            (( $(echo "$BIT <= 1.5" | bc -l) )) && BIT_DIR=1
            (( $(echo "$BIT >= 16"  | bc -l) )) && BIT_DIR=-1
            RATE=$((RATE + RATE_DIR))
            (( RATE <= 1  )) && RATE_DIR=1
            (( RATE >= 16 )) && RATE_DIR=-1
            send bit $BIT
            sleep 0.02
            send rate $RATE
            ;;
        drive)
            DRIVE=$(echo "$DRIVE + $DRIVE_DIR * 0.1" | bc)
            (( $(echo "$DRIVE <= 0.0" | bc -l) )) && DRIVE_DIR=1
            (( $(echo "$DRIVE >= 9.0" | bc -l) )) && DRIVE_DIR=-1
            send drive $DRIVE
            ;;
        tone)
            TONE=$(echo "$TONE + $TONE_DIR * 0.01" | bc)
            (( $(echo "$TONE <= 0.3" | bc -l) )) && TONE_DIR=1
            (( $(echo "$TONE >= 1.0" | bc -l) )) && TONE_DIR=-1
            send tone $TONE
            ;;
        all)
            BIT=$(echo "$BIT + $BIT_DIR * 0.3" | bc)
            (( $(echo "$BIT <= 1.5" | bc -l) )) && BIT_DIR=1
            (( $(echo "$BIT >= 16"  | bc -l) )) && BIT_DIR=-1
            RATE=$((RATE + RATE_DIR))
            (( RATE <= 1  )) && RATE_DIR=1
            (( RATE >= 16 )) && RATE_DIR=-1
            DRIVE=$(echo "$DRIVE + $DRIVE_DIR * 0.1" | bc)
            (( $(echo "$DRIVE <= 0.0" | bc -l) )) && DRIVE_DIR=1
            (( $(echo "$DRIVE >= 9.0" | bc -l) )) && DRIVE_DIR=-1
            send bit $BIT
            sleep 0.02
            send rate $RATE
            sleep 0.02
            send drive $DRIVE
            ;;
    esac
    sleep $STEP_TIME
done
