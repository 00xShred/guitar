#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# Compile if needed
if [ "$SCRIPT_DIR/crusher.c" -nt "$SCRIPT_DIR/bitcrusher" ]; then
    echo "Compiling bitcrusher..."
    gcc -O2 -o "$SCRIPT_DIR/bitcrusher" "$SCRIPT_DIR/crusher.c" \
        -ljack -lpthread -lm $(pkg-config --cflags --libs raylib)
    echo "Compiled."
fi

# Kill any existing instance
pkill -f bitcrusher 2>/dev/null || true
sleep 0.3

# Wire JACK ports in background: waits for ports to appear, then connects
(
    for i in $(seq 1 20); do
        if jack_lsp 2>/dev/null | grep -q "BitCrusher:input"; then break; fi
        sleep 0.25
    done

    if ! jack_lsp 2>/dev/null | grep -q "BitCrusher:input"; then
        exit 0
    fi

    CAPTURE=$(jack_lsp 2>/dev/null | grep "system:capture_" | head -1)
    if [ -n "$CAPTURE" ]; then
        jack_connect "$CAPTURE" BitCrusher:input 2>/dev/null || true
    fi

    if jack_lsp 2>/dev/null | grep -q "Carla:audio-in1"; then
        jack_connect BitCrusher:output Carla:audio-in1 2>/dev/null || true
    else
        jack_connect BitCrusher:output system:playback_1 2>/dev/null || true
    fi
) &

# Launch bitcrusher GUI
exec "$SCRIPT_DIR/bitcrusher"
