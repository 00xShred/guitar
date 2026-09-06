# bitcrusher

Real-time audio bitcrusher and multi-effect pedal in C (JACK + raylib).

## Dependencies

- JACK (or PipeWire JACK)
- `raylib`, `gcc`, `make`, `jq`, `bc`

## Run

```bash
./start.sh
```

Or build manually:

```bash
make
./bitcrusher
```

## Controls

### Automation Scripts

```bash
./preset.sh <preset>       # Apply preset from presets.json (e.g. lofi, destroy)
./sweep.sh [mode] [speed]  # LFO sweep (modes: bit, rate, drive, tone, all)
./step_seq.sh [bpm] [pat]  # Sequencer (patterns: crush, stutter, wave, chaos)
```

### Pipe IPC

Control live via `/tmp/crush_pipe`:

```bash
echo "<param> <val>" > /tmp/crush_pipe
# e.g. echo "bit 4" > /tmp/crush_pipe
```

Params: `bit`, `rate`, `gain`, `drive`, `tone`, `mix`, `trem`, `depth`, `bypass`.
