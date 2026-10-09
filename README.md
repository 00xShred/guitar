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
# e.g.:
echo "bit 4" > /tmp/crush_pipe
echo "preset lofi" > /tmp/crush_pipe
echo "bypass" > /tmp/crush_pipe
```

Params: `bit`, `rate`, `gain`, `drive`, `tone`, `mix`, `trem`, `depth`, `gate`, `bypass`, `preset`, `sweep`.

### MIDI Control

Connect any MIDI controller to the JACK `BitCrusher:midi_in` port:

- **CC 20..28**: Knobs (`bit`, `rate`, `gain`, `drive`, `tone`, `mix`, `trem`, `depth`, `gate`)
- **CC 1 / CC 11**: Expression pedal / Mod wheel (`mix`)
- **CC 64**: Footswitch / Sustain pedal (`bypass`)
- **Program Change**: Presets 0..N

