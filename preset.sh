#!/usr/bin/env bash
# Reads params from presets.json and sends them to /tmp/crush_pipe

PRESET="$1"
FILE="$(dirname "$0")/presets.json"
PIPE=/tmp/crush_pipe

if [ -z "$PRESET" ]; then
    echo "Usage: $0 <preset>"
    echo "Available: $(jq -r 'keys[]' "$FILE" | tr '\n' ' ')"
    exit 1
fi

if ! jq -e ".\"$PRESET\"" "$FILE" >/dev/null 2>&1; then
    echo "Unknown preset: $PRESET"
    echo "Available: $(jq -r 'keys[]' "$FILE" | tr '\n' ' ')"
    exit 1
fi

for param in bit rate drive tone mix trem depth; do
    val=$(jq -r ".\"$PRESET\".\"$param\" // empty" "$FILE")
    if [ -n "$val" ]; then
        echo "$param $val" >"$PIPE"
        sleep 0.05
    fi
done

echo "Applied preset: $PRESET"
