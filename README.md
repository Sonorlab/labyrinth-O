# Labyrinth-o

**Dual generative MIDI sequencer for the Critter & Guitari Organelle**
Inspired by the Moog Labyrinth — built in C++, not Pure Data.

---

## What it does

Two independent 8-step sequencers run simultaneously, each outputting MIDI on its own channel. Gates and pitches mutate in real time using a probabilistic corruption algorithm, so patterns evolve continuously without repeating. Each sequencer has its own clock division, scale, root note, CV range, and corruption amount — run them at different rates for polymetric grooves, or chain them for a 16-step sequence.

---

## Why C++ instead of Pure Data

Most Organelle patches are Pure Data (`.pd`) files. Labyrinth-o is different — it's a compiled C++ binary that runs as the main patch process. This gives you:

- **POSIX threads** for a dedicated MIDI input thread and a clock thread, fully decoupled from the UI poll loop
- **ALSA MIDI** for low-latency note output and real-time clock input
- **Direct OSC/UDP** communication with `mother.pd` for knobs, keys, OLED, and LED — no pd externals required
- **Deterministic timing** — note-off scheduling is done with microsecond timestamps (`CLOCK_MONOTONIC`), not pd metro objects

The tradeoff is that you need to cross-compile for the Organelle's ARM processor, or compile directly on the device.

---

## Architecture overview

```
mother.pd  ──OSC UDP──►  labyrinth-o  ──ALSA──►  MIDI out
  (knobs/keys/OLED)            │
                               ▼
                        ALSA MIDI input
                         (ttymidi, TRS)
```

The patch has three threads:

| Thread | Role |
|---|---|
| Main | OSC poll loop, OLED refresh (50 ms), note-off queue |
| Clock | Internal BPM timer; yields to MIDI sync when external clock arrives |
| MIDI input | Blocking ALSA poll on ttymidi port; dispatches clock/start/stop |

State shared between threads is protected by `g_state_mutex`. The note-off queue has its own `g_event_mutex`.

---

## Patch folder structure

```
LabyrinthO/
├── labyrinth-o        ← compiled binary (ARM)
├── main.pd            ← minimal pd file that launches the binary
├── info.json          ← Organelle patch metadata
└── crash.log          ← written to USB drive at runtime
```

### main.pd

The Organelle expects a `main.pd` file as its entry point. For a C++ patch, this file just needs to launch your binary and pass through the OSC ports:

```pd
#N canvas 0 0 450 300 10;
#X obj 10 10 r #loadbang;
#X obj 10 40 shell labyrinth-o;
#X connect 0 0 1 0;
```

Alternatively, use `Makefile`-style launch via `system~` or a wrapper script — whatever matches your Organelle OS version.

### info.json

```json
{
  "name": "Labyrinth-o",
  "description": "Dual generative MIDI sequencer",
  "author": "Your Name",
  "version": "1.0"
}
```

---

## OSC protocol (mother.pd ↔ your binary)

The Organelle's `mother.pd` communicates with the running patch process over UDP/OSC on localhost.

| Direction | Port | Purpose |
|---|---|---|
| mother.pd → patch | `4000` | Knob values, key events |
| patch → mother.pd | `4001` | OLED lines, LED colour |

### Incoming messages (your binary receives these)

```
/knobs  i i i i        — K1–K4 raw values (0–1023), sent continuously
/key    i i            — key index, velocity (0 = key up)
/quit                  — sent on patch unload
```

Knob values are raw 0–1023 integers. Map them yourself — a simple linear map to 0.0–1.0 is enough for most parameters. Implement **soft takeover** per page (track last raw value per knob per page, only apply changes once the physical knob passes the stored value) to prevent jumps when switching pages.

Key index 0 is the AUX button. Keys 1–24 are the keyboard keys left to right.

### Outgoing messages (your binary sends these)

```
/oled/line/1  s "..."  — set OLED line 1 (21 chars max)
/oled/line/2  s "..."
/oled/line/3  s "..."
/oled/line/4  s "..."
/oled/line/5  s "..."
/led          i <val>  — set LED colour (0 = off, 1 = green, etc.)
```

The OLED has 5 lines of 21 characters. Send only lines that have changed — dirty-check against your last-sent buffer to avoid flooding the OSC socket.

### Minimal OSC implementation

You don't need liblo. OSC over UDP is simple enough to implement inline:

```c
// Build /address ,i <val> — returns byte count
static int osc_msg_i(uint8_t* buf, const char* addr, int32_t val) {
    int p = 0;
    int al = strlen(addr) + 1, ap = (al + 3) & ~3;
    memcpy(buf+p, addr, al); memset(buf+p+al, 0, ap-al); p += ap;
    const char* tag = ",i";
    memcpy(buf+p, tag, 3); buf[p+3] = 0; p += 4;
    // Write big-endian int32
    buf[p]=(val>>24)&0xFF; buf[p+1]=(val>>16)&0xFF;
    buf[p+2]=(val>>8)&0xFF; buf[p+3]=val&0xFF; p += 4;
    return p;
}
```

Same pattern for `,s` (string) messages. Parse incoming by reading the address string, skipping to the type tag, then reading 4 bytes per `i` or `f` argument.

---

## ALSA MIDI

### Output (notes + CC)

Open a non-blocking output sequencer, create a port, then auto-connect to the first available writable MIDI port that isn't `Midi Through` or your own client:

```c
snd_seq_open(&seq, "default", SND_SEQ_OPEN_OUTPUT, SND_SEQ_NONBLOCK);
snd_seq_set_client_name(seq, "MY-PATCH");
int port = snd_seq_create_simple_port(seq, "Out",
    SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
    SND_SEQ_PORT_TYPE_APPLICATION | SND_SEQ_PORT_TYPE_MIDI_GENERIC);
// then snd_seq_connect_to(seq, port, dest_client, dest_port)
```

Send events with `snd_seq_ev_set_direct` and `snd_seq_drain_output` — this is synchronous enough for a sequencer.

### Input (clock sync via TRS jack)

MIDI clock arrives via `ttymidi`, which bridges the Organelle's hardware TRS MIDI jack to ALSA. Subscribe **only to ttymidi** — not `Midi Through` (which is an empty kernel loopback) and not your own ports:

```c
// Only connect to ttymidi — skip Midi Through and own ports
if (!strstr(cname, "ttymidi")) continue;
snd_seq_connect_from(seq, in_port, client, port);
```

If ttymidi is not found (e.g. running on a non-Organelle Linux machine), fall back to any readable port so the patch still works for development.

Run the MIDI input subscription in a **dedicated thread** using a blocking `poll()` loop on the ALSA file descriptors. Dispatch `SND_SEQ_EVENT_CLOCK`, `SND_SEQ_EVENT_START`, `SND_SEQ_EVENT_STOP`, and `SND_SEQ_EVENT_CONTINUE` from that thread.

### MIDI clock sync

MIDI clock runs at 24 PPQN. 6 pulses = one 16th note (one base tick in Labyrinth-o). On each clock pulse, accumulate a rolling average of inter-pulse intervals to calculate the actual BPM — this is what you use for note-off scheduling, so note lengths stay correct even at non-round tempos.

When no clock pulse arrives for 2 seconds, revert to internal BPM (`MIDI_SYNC_TIMEOUT_US`).

---

## Timing

Use `CLOCK_MONOTONIC` for all timing — it doesn't jump on system clock changes:

```c
static uint64_t now_us() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}
```

Schedule note-offs as absolute timestamps in a queue, processed on each main loop iteration. This decouples note duration from the step rate cleanly.

---

## Building

### Dependencies

```
libasound2-dev   (ALSA)
g++ / arm-linux-gnueabihf-g++
```

### On the Organelle directly

```bash
ssh -l patch organelle.local   # default password: organelle
cd /usbdrive/Patches/LabyrinthO
g++ -O2 -o labyrinth-o labyrinth.cpp -lasound -lpthread -lm
```

### Cross-compile from macOS / Linux

```bash
# Install ARM cross-compiler (Ubuntu/Debian)
sudo apt install g++-arm-linux-gnueabihf

# Build
arm-linux-gnueabihf-g++ -O2 -o labyrinth-o labyrinth.cpp \
    -lasound -lpthread -lm

# Copy to Organelle over SSH
scp labyrinth-o patch@organelle.local:/usbdrive/Patches/LabyrinthO/
```

The Organelle M runs a 32-bit ARMv7 Linux. The binary is statically or dynamically linked against the system ALSA — check which version is on your device with `aplay --version` before cross-compiling if you hit linker issues.

---

## Crash logging

Write a crash log to the USB drive so you can inspect it after a reboot:

```c
g_crash_log = fopen("/usbdrive/Patches/LabyrinthO/crash.log", "a");
```

All `log_msg()` calls write to both `stderr` and this file. Flush after every write (`fflush`) so the log survives a hard crash.

---

## Key gotchas

**Soft takeover on knobs** — when the user switches pages, the physical knob position won't match the stored value for that page. Track the last raw value per knob per page and only start applying changes once the knob crosses the stored value, otherwise you get parameter jumps.

**Midi Through is not ttymidi** — ALSA exposes a `Midi Through` port at client 14 that looks like a real port but is an empty kernel loopback. Always filter it out when searching for clock input.

**OLED width** — lines are 21 characters wide. Pad or truncate before sending, otherwise the display clips inconsistently.

**Note-off overlap** — if a new step fires the same note as the previous step before its note-off has fired, cancel the queued note-off and send a new note-on, otherwise you get stuck notes.

**AUX long press** — the AUX button sends key 0. Record the press timestamp on key-down and decide the action (page cycle vs. transport toggle) on key-up based on hold duration. 600 ms is a comfortable threshold.

---

## License

CC O

---

*SonorLab / Arthur Vincent — 2026*
