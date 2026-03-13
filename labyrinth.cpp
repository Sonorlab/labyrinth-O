//==========================================================================
// LABYRINTH-O — Dual Generative MIDI Sequencer for Organelle
// Sonor Lab / Arthur Vincent — 2026
// Single-file C++ | ALSA MIDI | OSC/UDP | POSIX threads
// Spec v1.0
//==========================================================================

#include <alsa/asoundlib.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <poll.h>
#include <algorithm>
#include <vector>
#include <string>
#include <cstdarg>

//==========================================================================
// CONSTANTS
//==========================================================================

#define MOTHER_RECV_PORT   4000       // We listen on this port (from mother.pd)
#define MOTHER_SEND_PORT   4001       // We send OSC here (to mother.pd)
#define MOTHER_IP          "127.0.0.1"
#define CRASH_LOG_PATH     "/usbdrive/Patches/LabyrinthO/crash.log"
#define OLED_RATE_MS       50
#define NUM_SEQS           2
#define NUM_STEPS          8
#define NUM_PAGES          5

// Negative values = speed multipliers (fire every tick * |val| times using sub-tick counter)
// Positive values = clock dividers (fire every N ticks)
// Table order: x4, x2, /1, /2, /3, /4, /6, /8
static const int CLOCK_DIVS[]   = {-4, -2, 1, 2, 3, 4, 6, 8};
static const int NUM_CLOCK_DIVS = 8;
static const char* CLOCK_DIV_NAMES[] = {"x4","x2","/1","/2","/3","/4","/6","/8"};
static const char* NOTE_NAMES[] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};

//==========================================================================
// SCALE TABLES  (Section 3.5)
//==========================================================================

#define NUM_SCALES 16

static const char* SCALE_NAMES[NUM_SCALES] = {
    "Unquantized", "Chromatic",  "Major",     "Pentatonic",
    "MelodMinor",  "HarmMinor",  "Dim6th",    "WholeTone",
    "Hirajoshi",   "7sus4",      "Major7th",  "Major13th",
    "Minor7th",    "Minor11th",  "HangDrum",  "Quads"
};

static const int SCALE_LEN[NUM_SCALES] = {
    0, 12, 7, 5, 7, 7, 8, 6, 5, 4, 4, 6, 4, 6, 7, 4
};

// Max 12 entries; scales 11/13 have interval 14 — handled in quantizer
static const int SCALE_IVLS[NUM_SCALES][12] = {
    {0,0,0,0,0,0,0,0,0,0,0,0},           // 0: Unquantized (unused)
    {0,1,2,3,4,5,6,7,8,9,10,11},          // 1: Chromatic
    {0,2,4,5,7,9,11,0,0,0,0,0},           // 2: Major
    {0,2,4,7,9,0,0,0,0,0,0,0},            // 3: Pentatonic
    {0,2,3,5,7,9,11,0,0,0,0,0},           // 4: Melodic Minor
    {0,2,3,5,7,8,11,0,0,0,0,0},           // 5: Harmonic Minor
    {0,2,3,5,6,8,9,11,0,0,0,0},           // 6: Diminished 6th
    {0,2,4,6,8,10,0,0,0,0,0,0},           // 7: Whole Tone
    {0,2,3,7,8,0,0,0,0,0,0,0},            // 8: Hirajoshi Pent
    {0,2,5,7,10,0,0,0,0,0,0,0},           // 9: 7sus4
    {0,4,7,11,0,0,0,0,0,0,0,0},           // 10: Major 7th
    {0,4,7,9,11,14,0,0,0,0,0,0},          // 11: Major 13th  (14 = M2 above octave)
    {0,3,7,10,0,0,0,0,0,0,0,0},           // 12: Minor 7th
    {0,3,5,7,10,14,0,0,0,0,0,0},          // 13: Minor 11th  (14 = M2 above octave)
    {0,2,3,5,7,9,10,0,0,0,0,0},           // 14: Hang Drum
    {0,3,6,9,0,0,0,0,0,0,0,0}             // 15: Quads
};

//==========================================================================
// DATA STRUCTURES
//==========================================================================

struct NoteOffEvent {
    uint64_t fire_us;   // absolute time to fire note-off
    int      channel;
    int      note;
};

struct SeqState {
    bool  gate[NUM_STEPS];
    float cv[NUM_STEPS];
    int   length;
    int   play_head;
    float cv_range;         // 0.0–1.0
    float corrupt;          // 0.0–1.0 (effective, resolved from master/per-seq)
    int   clock_div_idx;    // index into CLOCK_DIVS[]
    int   midi_channel;     // 1–16
    int   midi_cc;          // 0–127
    int   scale_idx;        // 0–15
    int   root_note;        // 0–11
    // Runtime
    int   tick_counter;     // counts base-clock ticks
    int   last_note;        // last MIDI note fired (for overlap check), -1=none
};

// Velocity modes
enum VelocityMode { VEL_FIXED = 0, VEL_FROM_CV = 1, VEL_ACCENT = 2 };

//==========================================================================
// GLOBAL STATE
//==========================================================================

static volatile bool g_running        = true;   // main loop alive
static volatile bool g_seq_running    = false;  // sequencer transport
static int           g_page           = 1;      // 1–5
static bool          g_chain_seq      = false;
static int           g_chain_state    = 0;      // 0=SEQ1 active, 1=SEQ2 active
static int           g_bpm            = 120;
static float         g_master_corrupt = 0.0f;   // page 1 K4
static float         g_p2_corrupt[2]  = {0.0f, 0.0f}; // page 2 per-seq corrupt
static int           g_velocity_mode  = VEL_FIXED;
static int           g_note_length_pct= 50;     // 10–100%

// KB Mode — controls how Organelle keyboard keys behave
enum KbMode { KB_BITFLIP = 0, KB_TRANSPOSE = 1 };
static int           g_kb_mode        = KB_BITFLIP;
// Per-sequencer semitone transpose offset (used in KB_TRANSPOSE mode)
static int           g_transpose[NUM_SEQS] = {0, 0};  // ±48 semitones

static SeqState g_seq[NUM_SEQS];

// ALSA
static snd_seq_t* g_alsa_seq  = NULL;
static int        g_alsa_port = -1;
static bool       g_midi_connected = false;
static pthread_t  g_midi_thread;

// OSC sockets
static int g_recv_sock = -1;
static int g_send_sock = -1;
static struct sockaddr_in g_send_addr;

// OLED dirty-check
static char g_oled_lines[5][64];
static char g_oled_prev[5][64];

// Note-off queue (protected by g_event_mutex)
static std::vector<NoteOffEvent> g_noteoff_queue;
static pthread_mutex_t g_event_mutex = PTHREAD_MUTEX_INITIALIZER;

// State mutex (protects SeqState, g_seq_running, etc.)
static pthread_mutex_t g_state_mutex = PTHREAD_MUTEX_INITIALIZER;

// Encoder tracking
static int      g_encoder_prev    = -1;

// Soft takeover: prevent page-4 MIDI channel jumps on page entry
static int      g_knob_prev_page    = 0;
static int      g_knob_entry_raw[4] = {-1, -1, -1, -1};
static bool     g_knob_has_moved[4] = {};
// Per-knob delta tracking: only apply a knob when it physically moves
// (-1 = not yet initialised; reset on page change)
static int      g_knob_last_raw[4]  = {-1, -1, -1, -1};

// AUX long-press: tap = cycle pages, hold ≥600ms = toggle transport
static uint64_t g_aux_press_time  = 0;
#define AUX_LONG_PRESS_US 600000ULL

// MIDI clock sync
static volatile bool g_midi_sync        = false;  // true = locked to external clock
static int           g_midi_pulse_cnt   = 0;      // counts pulses mod 6 (6 = 1 base tick)
static uint64_t      g_midi_sync_last_us= 0;      // time of last received clock pulse
#define MIDI_SYNC_TIMEOUT_US 2000000ULL            // 2s without pulse = fallback to internal

// Rolling average of MIDI clock pulse intervals (for detected BPM display)
static uint64_t g_pulse_intervals[24]   = {};
static int      g_pulse_int_idx         = 0;
static int      g_pulse_int_cnt         = 0;
static uint64_t g_midi_last_pulse_us    = 0;

// Knob raw cache (reserved for soft-takeover — Phase 2)
// static int  g_knob_raw[NUM_PAGES][4];
// static bool g_knob_active[NUM_PAGES][4];

// ── Serial (DIN MIDI) ─────────────────────────────────────────────────────
// Serial bypass removed: ttymidi correctly passes all realtime messages
// through to ALSA port 128:0. LABYRINTH-O uses ALSA exclusively.

// Crash log file
static FILE* g_crash_log = NULL;

//==========================================================================
// LOGGING
//==========================================================================

static void log_msg(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    if (g_crash_log) {
        va_start(args, fmt);
        vfprintf(g_crash_log, fmt, args);
        fflush(g_crash_log);
        va_end(args);
    }
}

//==========================================================================
// TIME
//==========================================================================

static uint64_t now_us() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

//==========================================================================
// OSC CODEC  (minimal UDP/OSC implementation — no liblo dependency)
//==========================================================================

static int pad4(int n) { return (n + 3) & ~3; }

static int32_t osc_rd_i32(const uint8_t* b) {
    return ((int32_t)b[0] << 24) | ((int32_t)b[1] << 16) |
           ((int32_t)b[2] << 8)  |  (int32_t)b[3];
}

static void osc_wr_i32(uint8_t* b, int32_t v) {
    b[0] = (v >> 24) & 0xFF; b[1] = (v >> 16) & 0xFF;
    b[2] = (v >>  8) & 0xFF; b[3] =  v        & 0xFF;
}

// Build: /address ,s "str"  → returns byte count written to buf
static int osc_msg_s(uint8_t* buf, const char* addr, const char* str) {
    int p = 0;
    int al = strlen(addr) + 1, ap = pad4(al);
    memcpy(buf+p, addr, al); memset(buf+p+al, 0, ap-al); p += ap;
    const char* tag = ",s";
    int tl = 3, tp = 4;
    memcpy(buf+p, tag, tl); buf[p+tl] = 0; p += tp;
    int sl = strlen(str) + 1, sp = pad4(sl);
    memcpy(buf+p, str, sl); memset(buf+p+sl, 0, sp-sl); p += sp;
    return p;
}

// Build: /address ,i <val>  → returns byte count
static int osc_msg_i(uint8_t* buf, const char* addr, int32_t val) {
    int p = 0;
    int al = strlen(addr) + 1, ap = pad4(al);
    memcpy(buf+p, addr, al); memset(buf+p+al, 0, ap-al); p += ap;
    const char* tag = ",i";
    int tl = 3, tp = 4;
    memcpy(buf+p, tag, tl); buf[p+tl] = 0; p += tp;
    osc_wr_i32(buf+p, val); p += 4;
    return p;
}

// Parse incoming OSC: returns address string and fills int args[]
// Returns number of int args found (up to max_args)
static int osc_parse(const uint8_t* buf, int len, char* addr_out, int* args, int max_args) {
    if (len < 8) return -1;
    // Address
    int addr_len = strnlen((const char*)buf, len);
    if (addr_len >= 64) return -1;
    strncpy(addr_out, (const char*)buf, 64);
    addr_out[63] = 0;
    int p = pad4(addr_len + 1);
    if (p >= len) return 0;
    // Type tag
    if (buf[p] != ',') return 0;
    const char* types = (const char*)buf + p + 1;
    int tp = pad4(strlen(types) + 2);  // +1 for ',', +1 for '\0'
    p += tp;
    // Args
    int n = 0;
    for (int i = 0; types[i] != '\0' && n < max_args; i++) {
        if (types[i] == 'i' || types[i] == 'f') {
            if (p + 4 > len) break;
            args[n++] = osc_rd_i32(buf + p);
            p += 4;
        }
    }
    return n;
}

//==========================================================================
// OSC SEND
//==========================================================================

static void osc_send(const uint8_t* buf, int len) {
    if (g_send_sock < 0) return;
    sendto(g_send_sock, buf, len, 0,
           (struct sockaddr*)&g_send_addr, sizeof(g_send_addr));
}

static void oled_set(int line, const char* fmt, ...) {
    // line 1–5
    char tmp[64];
    va_list args;
    va_start(args, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, args);
    va_end(args);
    // Pad to 21 chars (OLED width)
    int l = strlen(tmp);
    if (l < 21) { memset(tmp+l, ' ', 21-l); tmp[21] = 0; }
    else tmp[21] = 0;
    strncpy(g_oled_lines[line-1], tmp, 63);
}

static void oled_flush() {
    uint8_t buf[256];
    char addr[32];
    for (int i = 0; i < 5; i++) {
        if (strcmp(g_oled_lines[i], g_oled_prev[i]) != 0) {
            snprintf(addr, sizeof(addr), "/oled/line/%d", i+1);
            int n = osc_msg_s(buf, addr, g_oled_lines[i]);
            osc_send(buf, n);
            strncpy(g_oled_prev[i], g_oled_lines[i], 63);
        }
    }
}

static void led_set(int colour) {
    uint8_t buf[64];
    int n = osc_msg_i(buf, "/led", colour);
    osc_send(buf, n);
}

//==========================================================================
// ALSA MIDI
//==========================================================================

static bool alsa_setup() {
    // Output only — MIDI clock input is handled by the dedicated MIDI thread
    // which opens its own blocking ALSA handle.
    if (snd_seq_open(&g_alsa_seq, "default", SND_SEQ_OPEN_OUTPUT, SND_SEQ_NONBLOCK) < 0) {
        log_msg("[ALSA] snd_seq_open failed\n");
        return false;
    }
    snd_seq_set_client_name(g_alsa_seq, "LABYRINTH-O");

    // Output port (MIDI notes + CC)
    g_alsa_port = snd_seq_create_simple_port(
        g_alsa_seq, "Out",
        SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
        SND_SEQ_PORT_TYPE_APPLICATION | SND_SEQ_PORT_TYPE_MIDI_GENERIC);
    if (g_alsa_port < 0) {
        log_msg("[ALSA] create output port failed\n");
        return false;
    }

    snd_seq_client_info_t* cinfo;
    snd_seq_port_info_t*   pinfo;
    snd_seq_client_info_alloca(&cinfo);
    snd_seq_port_info_alloca(&pinfo);

    bool out_connected = false;

    snd_seq_client_info_set_client(cinfo, -1);
    while (snd_seq_query_next_client(g_alsa_seq, cinfo) >= 0) {
        int client = snd_seq_client_info_get_client(cinfo);
        if (client == SND_SEQ_CLIENT_SYSTEM) continue;
        const char* cname = snd_seq_client_info_get_name(cinfo);

        snd_seq_port_info_set_client(pinfo, client);
        snd_seq_port_info_set_port(pinfo, -1);
        while (snd_seq_query_next_port(g_alsa_seq, pinfo) >= 0) {
            unsigned int cap  = snd_seq_port_info_get_capability(pinfo);
            int port          = snd_seq_port_info_get_port(pinfo);

            if (strstr(cname, "LABYRINTH")) continue;

            if (!out_connected &&
                !strstr(cname, "Midi Through") &&
                (cap & SND_SEQ_PORT_CAP_WRITE) &&
                (cap & SND_SEQ_PORT_CAP_SUBS_WRITE)) {
                int rc = snd_seq_connect_to(g_alsa_seq, g_alsa_port, client, port);
                if (rc >= 0) {
                    log_msg("[ALSA] Out → %d:%d \"%s\"\n", client, port, cname);
                    out_connected = true;
                } else {
                    log_msg("[ALSA] Out → %d:%d failed: %s\n", client, port, snd_strerror(rc));
                }
            }
        }
    }

    if (!out_connected)
        log_msg("[ALSA] No MIDI output found — running without MIDI out\n");
    return out_connected;
}

static void midi_note_on(int channel, int note, int vel) {
    note = std::max(0, std::min(127, note));
    vel  = std::max(1, std::min(127, vel));

    if (g_alsa_seq && g_alsa_port >= 0) {
        snd_seq_event_t ev;
        snd_seq_ev_clear(&ev);
        snd_seq_ev_set_source(&ev, g_alsa_port);
        snd_seq_ev_set_subs(&ev);
        snd_seq_ev_set_direct(&ev);
        snd_seq_ev_set_noteon(&ev, channel - 1, note, vel);
        snd_seq_event_output(g_alsa_seq, &ev);
        snd_seq_drain_output(g_alsa_seq);
    }
}

static void midi_note_off(int channel, int note) {
    note = std::max(0, std::min(127, note));

    if (g_alsa_seq && g_alsa_port >= 0) {
        snd_seq_event_t ev;
        snd_seq_ev_clear(&ev);
        snd_seq_ev_set_source(&ev, g_alsa_port);
        snd_seq_ev_set_subs(&ev);
        snd_seq_ev_set_direct(&ev);
        snd_seq_ev_set_noteoff(&ev, channel - 1, note, 0);
        snd_seq_event_output(g_alsa_seq, &ev);
        snd_seq_drain_output(g_alsa_seq);
    }
}

static void midi_cc(int channel, int cc, int val) {
    val = std::max(0, std::min(127, val));

    if (g_alsa_seq && g_alsa_port >= 0) {
        snd_seq_event_t ev;
        snd_seq_ev_clear(&ev);
        snd_seq_ev_set_source(&ev, g_alsa_port);
        snd_seq_ev_set_subs(&ev);
        snd_seq_ev_set_direct(&ev);
        snd_seq_ev_set_controller(&ev, channel - 1, cc, val);
        snd_seq_event_output(g_alsa_seq, &ev);
        snd_seq_drain_output(g_alsa_seq);
    }
}

static void schedule_note_off(int channel, int note, uint64_t fire_us) {
    pthread_mutex_lock(&g_event_mutex);
    NoteOffEvent e;
    e.fire_us = fire_us;
    e.channel = channel;
    e.note    = note;
    g_noteoff_queue.push_back(e);
    pthread_mutex_unlock(&g_event_mutex);
}

static void process_note_offs() {
    uint64_t t = now_us();
    pthread_mutex_lock(&g_event_mutex);
    for (int i = (int)g_noteoff_queue.size() - 1; i >= 0; i--) {
        if (t >= g_noteoff_queue[i].fire_us) {
            midi_note_off(g_noteoff_queue[i].channel, g_noteoff_queue[i].note);
            g_noteoff_queue.erase(g_noteoff_queue.begin() + i);
        }
    }
    pthread_mutex_unlock(&g_event_mutex);
}

// Cancel any pending note-off for this channel/note (for overlap handling)
static void cancel_note_off(int channel, int note) {
    pthread_mutex_lock(&g_event_mutex);
    for (int i = (int)g_noteoff_queue.size() - 1; i >= 0; i--) {
        if (g_noteoff_queue[i].channel == channel &&
            g_noteoff_queue[i].note == note) {
            g_noteoff_queue.erase(g_noteoff_queue.begin() + i);
        }
    }
    pthread_mutex_unlock(&g_event_mutex);
}

//==========================================================================
// QUANTIZER  (Section 3.5)
//==========================================================================

static int quantize_note(float cv, float cv_range, int scale_idx, int root_note) {
    // cv in [-1,+1], cv_range in [0,1]
    // Map to ±2 octaves (±24 semitones) centred on C4 + root_note offset
    float semitones = cv * cv_range * 24.0f;
    int centre = 60 + root_note;

    if (scale_idx == 0) {
        // Unquantized — raw pitch
        return std::max(0, std::min(127, centre + (int)roundf(semitones)));
    }

    int target = centre + (int)roundf(semitones);
    int len    = SCALE_LEN[scale_idx];
    if (len == 0) return std::max(0, std::min(127, target));

    int best = target, best_dist = 10000;
    for (int oct = -3; oct <= 3; oct++) {
        for (int i = 0; i < len; i++) {
            int candidate = centre + oct * 12 + SCALE_IVLS[scale_idx][i];
            int dist = abs(candidate - target);
            if (dist < best_dist) { best_dist = dist; best = candidate; }
        }
    }
    return std::max(0, std::min(127, best));
}

//==========================================================================
// CORRUPT ALGORITHM  (Section 3.4)
//==========================================================================

static float rand_float_11() {
    return ((float)rand() / (float)RAND_MAX) * 2.0f - 1.0f;
}

static void apply_corrupt(SeqState& s, int step) {
    float c = s.corrupt;
    if (c <= 0.0f) return;

    float cv_prob, gate_prob;
    if (c <= 0.5f) {
        cv_prob   = c * 0.5f;    // 0–0.25
        gate_prob = 0.0f;
    } else {
        cv_prob   = 0.25f + (c - 0.5f) * 0.5f;   // 0.25–0.5
        gate_prob = (c - 0.5f) * 1.0f;             // 0–0.5
    }

    float r1 = (float)rand() / (float)RAND_MAX;
    if (s.gate[step] && r1 < cv_prob) {
        s.cv[step] = rand_float_11();
    }
    if (c > 0.5f) {
        float r2 = (float)rand() / (float)RAND_MAX;
        if (r2 < gate_prob) {
            s.gate[step] = !s.gate[step];
            if (s.gate[step]) s.cv[step] = rand_float_11();
        }
    }
}

//==========================================================================
// EFFECTIVE CORRUPT HELPER
//==========================================================================

static float effective_corrupt(int seq_idx) {
    bool both_zero = (g_p2_corrupt[0] < 0.001f && g_p2_corrupt[1] < 0.001f);
    return both_zero ? g_master_corrupt : g_p2_corrupt[seq_idx];
}

//==========================================================================
// STEP TRIGGER  (Section 3.3)
//==========================================================================

// Returns the current step duration in microseconds.
// When locked to external MIDI clock, derives duration from the rolling average of
// received pulse intervals (24 PPQN; 6 pulses = 1 base tick / 16th note).
// Falls back to the internal BPM setting otherwise.
static uint64_t get_step_us() {
    if (g_midi_sync && g_pulse_int_cnt >= 4) {
        int n = (g_pulse_int_cnt < 24) ? g_pulse_int_cnt : 24;
        uint64_t sum = 0;
        for (int i = 0; i < n; i++) sum += g_pulse_intervals[i];
        uint64_t avg_pulse_us = sum / (uint64_t)n;
        // 6 MIDI clock pulses = 1 base tick (16th note at 24 PPQN)
        return avg_pulse_us * 6;
    }
    return (uint64_t)(60000000.0 / (double)g_bpm) / 4;
}

static void fire_step(int si) {
    SeqState& s = g_seq[si];
    int ph = s.play_head;

    // Resolve effective corrupt
    s.corrupt = effective_corrupt(si);

    if (s.gate[ph]) {
        int note = quantize_note(s.cv[ph], s.cv_range, s.scale_idx, s.root_note);
        // Apply KB_TRANSPOSE offset
        note = std::max(0, std::min(127, note + g_transpose[si]));

        // Handle overlap — send note-off for previous note if same pitch/channel
        if (s.last_note >= 0 && s.last_note != note) {
            cancel_note_off(s.midi_channel, s.last_note);
            midi_note_off(s.midi_channel, s.last_note);
        } else if (s.last_note == note) {
            cancel_note_off(s.midi_channel, s.last_note);
            midi_note_off(s.midi_channel, s.last_note);
        }

        // Velocity
        int vel = 100;
        if (g_velocity_mode == VEL_FROM_CV) {
            vel = (int)(40.0f + (s.cv[ph] * s.cv_range + 1.0f) * 0.5f * 87.0f);
            vel = std::max(40, std::min(127, vel));
        } else if (g_velocity_mode == VEL_ACCENT) {
            vel = (ph == 0) ? 127 : 80;
        }

        midi_note_on(s.midi_channel, note, vel);
        s.last_note = note;

        // CC output: scale cv → 0–127
        int cc_val = (int)((s.cv[ph] * s.cv_range + 1.0f) * 63.5f);
        cc_val = std::max(0, std::min(127, cc_val));
        midi_cc(s.midi_channel, s.midi_cc, cc_val);

        // Schedule note-off — use measured external step duration when sync'd,
        // internal BPM otherwise, so note lengths always track the actual tempo.
        uint64_t step_us = get_step_us();
        uint64_t note_dur_us = step_us * g_note_length_pct / 100;
        schedule_note_off(s.midi_channel, note, now_us() + note_dur_us);
    } else {
        // Gate off — make sure last note is cut
        if (s.last_note >= 0) {
            cancel_note_off(s.midi_channel, s.last_note);
            midi_note_off(s.midi_channel, s.last_note);
            s.last_note = -1;
        }
    }

    // Apply CORRUPT mutation at play head (write=play in v1.0)
    apply_corrupt(s, ph);

    // Advance play head
    s.play_head = (s.play_head + 1) % s.length;
}

//==========================================================================
// CLOCK THREAD  (Section 3.2)
//==========================================================================

// Advance all sequencers by one base tick. Must be called with g_state_mutex held.
static void tick_all() {
    if (!g_seq_running) return;
    if (g_chain_seq) {
        // Chain mode — SEQ1 then SEQ2 in series
        if (g_chain_state == 0) {
            fire_step(0);
            if (g_seq[0].play_head == 0) {
                g_chain_state = 1;
                g_seq[1].play_head = 0;
            }
        } else {
            fire_step(1);
            if (g_seq[1].play_head == 0) {
                g_chain_state = 0;
                g_seq[0].play_head = 0;
            }
        }
    } else {
        // Independent polymetric mode
        for (int si = 0; si < NUM_SEQS; si++) {
            int cdiv = CLOCK_DIVS[g_seq[si].clock_div_idx];
            if (cdiv < 0) {
                // Speed multiplier: fire |cdiv| steps per base tick
                int mult = -cdiv;
                for (int f = 0; f < mult; f++) fire_step(si);
            } else {
                // Divider: fire one step every cdiv ticks
                g_seq[si].tick_counter++;
                if (g_seq[si].tick_counter >= cdiv) {
                    g_seq[si].tick_counter = 0;
                    fire_step(si);
                }
            }
        }
    }
}

static void* clock_thread_fn(void*) {
    while (g_running) {
        // When external clock is active, poll every 5 ms and yield —
        // this lets the thread notice g_midi_sync going true within 5 ms
        // instead of waiting up to one full step interval (up to 375 ms at 40 BPM).
        if (g_midi_sync) {
            struct timespec ts = {0, 5000000L};  // 5 ms
            nanosleep(&ts, NULL);
            continue;
        }

        // Internal BPM timing
        uint64_t step_us = (uint64_t)(60000000.0 / (double)g_bpm) / 4;
        struct timespec ts;
        ts.tv_sec  = (time_t)(step_us / 1000000ULL);
        ts.tv_nsec = (long)((step_us % 1000000ULL) * 1000ULL);
        nanosleep(&ts, NULL);

        // Re-check after sleep — external clock may have arrived during the sleep
        if (g_midi_sync) continue;

        pthread_mutex_lock(&g_state_mutex);
        tick_all();
        pthread_mutex_unlock(&g_state_mutex);
    }
    return NULL;
}

//==========================================================================
// MIDI CLOCK INPUT  (External sync)
//==========================================================================

static void update_led();  // forward declaration

// ── MIDI event handlers (called from dedicated MIDI input thread) ─────────

static void on_midi_clock() {
    uint64_t t = now_us();
    if (g_midi_last_pulse_us > 0) {
        uint64_t interval = t - g_midi_last_pulse_us;
        g_pulse_intervals[g_pulse_int_idx % 24] = interval;
        g_pulse_int_idx++;
        if (g_pulse_int_cnt < 24) g_pulse_int_cnt++;
    }
    g_midi_last_pulse_us = t;
    g_midi_sync_last_us  = t;
    g_midi_sync          = true;

    // 6 MIDI clock pulses = 1 base step (16th note at 24 PPQN)
    g_midi_pulse_cnt++;
    if (g_midi_pulse_cnt >= 6) {
        g_midi_pulse_cnt = 0;
        pthread_mutex_lock(&g_state_mutex);
        tick_all();
        pthread_mutex_unlock(&g_state_mutex);
    }
}

static void on_midi_start() {
    pthread_mutex_lock(&g_state_mutex);
    g_seq_running      = true;
    g_midi_pulse_cnt   = 0;
    g_seq[0].play_head = 0;
    g_seq[1].play_head = 0;
    g_chain_state      = 0;
    tick_all();                  // ← fire step 0 immediately on START
    pthread_mutex_unlock(&g_state_mutex);
    g_midi_sync          = true;
    g_midi_sync_last_us  = now_us();
    g_midi_last_pulse_us = 0;
    g_pulse_int_cnt      = 0;
    g_pulse_int_idx      = 0;
    update_led();
    log_msg("[MIDI] START\n");
}

static void on_midi_stop() {
    pthread_mutex_lock(&g_state_mutex);
    g_seq_running = false;
    for (int si = 0; si < NUM_SEQS; si++) {
        if (g_seq[si].last_note >= 0) {
            midi_note_off(g_seq[si].midi_channel, g_seq[si].last_note);
            g_seq[si].last_note = -1;
        }
    }
    pthread_mutex_unlock(&g_state_mutex);
    // Release sync immediately — don't wait for the 2-second timeout.
    g_midi_sync          = false;
    g_midi_sync_last_us  = 0;
    g_midi_last_pulse_us = 0;
    g_pulse_int_cnt      = 0;
    g_pulse_int_idx      = 0;
    update_led();
    log_msg("[MIDI] STOP\n");
}

static void on_midi_continue() {
    pthread_mutex_lock(&g_state_mutex);
    g_seq_running = true;
    pthread_mutex_unlock(&g_state_mutex);
    update_led();
    log_msg("[MIDI] CONTINUE\n");
}

// ── Dedicated MIDI input thread ──────────────────────────────────────────
// Subscribes to ttymidi via ALSA and dispatches clock/transport messages.
static void* midi_input_thread_fn(void*) {

    // ── Part 1: ALSA subscription for USB MIDI channel messages ──────────
    snd_seq_t* seq = NULL;
    int alsa_npfd  = 0;
    struct pollfd alsa_pfd[8];

    if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_INPUT, 0) >= 0) {
        snd_seq_set_client_name(seq, "LABYRINTH-MIDI");
        int in_port = snd_seq_create_simple_port(
            seq, "ClockIn",
            SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
            SND_SEQ_PORT_TYPE_APPLICATION | SND_SEQ_PORT_TYPE_MIDI_GENERIC);
        if (in_port >= 0) {
            snd_seq_client_info_t* cinfo;
            snd_seq_port_info_t*   pinfo;
            snd_seq_client_info_alloca(&cinfo);
            snd_seq_port_info_alloca(&pinfo);

            // ── Connect exclusively to ttymidi ────────────────────────────
            // Confirmed on Organelle M (2026-03): ttymidi is the correct and
            // only source for DIN/TRS MIDI clock. It correctly passes 0xF8
            // Clock, 0xFA Start, 0xFB Continue, 0xFC Stop to ALSA port 128:0.
            //
            // Previous code grabbed the first readable port alphabetically,
            // which resolved to "Midi Through" (14:0) — a kernel loopback
            // with nothing in it — causing LABYRINTH-O to never receive clock.
            //
            // We explicitly skip everything that is NOT ttymidi:
            //   • Midi Through (14:x) — empty kernel loopback
            //   • Our own LABYRINTH-O / LABYRINTH-MIDI ports — avoid loopback
            //   • Any other application ports — not our clock source
            //
            // If ttymidi is not found (e.g. different hardware), we log a
            // warning and fall back to accepting any readable port so the
            // patch still works on non-Organelle Linux systems.
            bool ttymidi_found = false;

            snd_seq_client_info_set_client(cinfo, -1);
            while (snd_seq_query_next_client(seq, cinfo) >= 0) {
                int client = snd_seq_client_info_get_client(cinfo);
                if (client == SND_SEQ_CLIENT_SYSTEM) continue;
                const char* cname = snd_seq_client_info_get_name(cinfo);

                // Only connect to ttymidi
                if (!strstr(cname, "ttymidi")) continue;

                snd_seq_port_info_set_client(pinfo, client);
                snd_seq_port_info_set_port(pinfo, -1);
                while (snd_seq_query_next_port(seq, pinfo) >= 0) {
                    unsigned int cap = snd_seq_port_info_get_capability(pinfo);
                    int port         = snd_seq_port_info_get_port(pinfo);
                    if ((cap & SND_SEQ_PORT_CAP_READ) &&
                        (cap & SND_SEQ_PORT_CAP_SUBS_READ)) {
                        if (snd_seq_connect_from(seq, in_port, client, port) >= 0) {
                            ttymidi_found = true;
                        }
                    }
                }
            }

            // Fallback: if ttymidi not found, accept any readable port
            if (!ttymidi_found) {
                snd_seq_client_info_set_client(cinfo, -1);
                while (snd_seq_query_next_client(seq, cinfo) >= 0) {
                    int client = snd_seq_client_info_get_client(cinfo);
                    if (client == SND_SEQ_CLIENT_SYSTEM) continue;
                    const char* cname = snd_seq_client_info_get_name(cinfo);
                    if (strstr(cname, "LABYRINTH"))   continue;
                    if (strstr(cname, "Midi Through")) continue;
                    snd_seq_port_info_set_client(pinfo, client);
                    snd_seq_port_info_set_port(pinfo, -1);
                    while (snd_seq_query_next_port(seq, pinfo) >= 0) {
                        unsigned int cap = snd_seq_port_info_get_capability(pinfo);
                        int port         = snd_seq_port_info_get_port(pinfo);
                        if ((cap & SND_SEQ_PORT_CAP_READ) &&
                            (cap & SND_SEQ_PORT_CAP_SUBS_READ)) {
                            snd_seq_connect_from(seq, in_port, client, port);
                        }
                    }
                }
            }
            alsa_npfd = snd_seq_poll_descriptors_count(seq, POLLIN);
            if (alsa_npfd > 8) alsa_npfd = 8;
            snd_seq_poll_descriptors(seq, alsa_pfd, alsa_npfd, POLLIN);
        }
    }
    // ── Part 2: Poll loop ─────────────────────────────────────────────────
    while (g_running) {

        if (alsa_npfd == 0) {
            struct timespec ts = {0, 50000000L};  // 50 ms
            nanosleep(&ts, NULL);
            continue;
        }

        int ret = poll(alsa_pfd, (nfds_t)alsa_npfd, 100);
        if (ret < 0) {
            if (errno != EINTR)
                log_msg("[MIDI-THR] poll error: %s\n", strerror(errno));
            continue;
        }
        if (ret == 0) continue;

        // ── ALSA events from ttymidi ──────────────────────────────────────
        if (seq && alsa_npfd > 0) {
            snd_seq_event_t* ev;
            while (snd_seq_event_input_pending(seq, 1) > 0) {
                if (snd_seq_event_input(seq, &ev) < 0) break;
                switch (ev->type) {
                    case SND_SEQ_EVENT_CLOCK:    on_midi_clock();    break;
                    case SND_SEQ_EVENT_START:    on_midi_start();    break;
                    case SND_SEQ_EVENT_STOP:     on_midi_stop();     break;
                    case SND_SEQ_EVENT_CONTINUE: on_midi_continue(); break;
                    default: break;
                }
            }
        }
    }

    if (seq) snd_seq_close(seq);
    return NULL;
}

// ── Timeout check (runs from main loop at 1 ms) ───────────────────────────
// If no CLOCK pulse arrives for 2 seconds, assume the master stopped without
// sending MIDI STOP and revert to internal BPM.
static void process_midi_input() {
    if (g_midi_sync && g_midi_sync_last_us > 0 &&
        now_us() - g_midi_sync_last_us > MIDI_SYNC_TIMEOUT_US) {
        g_midi_sync     = false;
        g_pulse_int_cnt = 0;
        log_msg("[MIDI] Clock timeout — reverting to internal BPM\n");
    }
}

//==========================================================================
// OLED DISPLAY  (Section 5)
//==========================================================================

// Build 8-char step display string
static void build_step_display(SeqState& s, char* out) {
    // 8 characters, one per step
    for (int i = 0; i < NUM_STEPS; i++) {
        if (i == s.play_head) {
            out[i] = '>';
        } else if (i >= s.length) {
            out[i] = '-';          // inactive beyond length
        } else if (s.gate[i]) {
            out[i] = (char)('1' + i);  // '1'–'8'
        } else {
            out[i] = '.';
        }
    }
    out[NUM_STEPS] = 0;
}

// Map common CC numbers to short label
static const char* cc_label(int cc) {
    switch(cc) {
        case  1: return "Mod";    case  7: return "Vol";
        case 10: return "Pan";    case 11: return "Exp";
        case 64: return "Sus";    case 71: return "Res";
        case 74: return "Flt";    case 91: return "Rev";
        default: return "";
    }
}

static void update_oled() {
    char sd0[16], sd1[16];
    build_step_display(g_seq[0], sd0);
    build_step_display(g_seq[1], sd1);

    switch (g_page) {
        case 1: { // PERFORM
            // Detect BPM from MIDI clock rolling average (24 pulses = 1 beat)
            int disp_bpm = g_bpm;
            if (g_midi_sync && g_pulse_int_cnt >= 4) {
                int cnt = (g_pulse_int_cnt < 24) ? g_pulse_int_cnt : 24;
                uint64_t sum = 0;
                for (int i = 0; i < cnt; i++) sum += g_pulse_intervals[i];
                uint64_t avg_us = sum / (uint64_t)cnt;
                if (avg_us > 0)
                    disp_bpm = (int)roundf(60000000.0f / ((float)avg_us * 24.0f));
                disp_bpm = std::max(20, std::min(300, disp_bpm));
            }
            oled_set(1, "PERF BPM:%d%s %s", disp_bpm,
                     g_midi_sync ? "*" : "",
                     g_seq_running ? "RUN" : "STP");
            oled_set(2, "S1:[%s] L:%d %s", sd0, g_seq[0].length,
                     CLOCK_DIV_NAMES[g_seq[0].clock_div_idx]);
            oled_set(3, "S2:[%s] L:%d %s", sd1, g_seq[1].length,
                     CLOCK_DIV_NAMES[g_seq[1].clock_div_idx]);
            oled_set(4, "CRP:%3d%% C1:%-2d C2:%-2d",
                     (int)(g_master_corrupt * 100),
                     g_seq[0].midi_channel, g_seq[1].midi_channel);
            break;
        }
        case 2: { // CORRUPT+CLK
            oled_set(1, "[CORRUPT+CLK]");
            oled_set(2, "S1 CRP:%2d%%  DIV: %s",
                     (int)(g_p2_corrupt[0] * 100),
                     CLOCK_DIV_NAMES[g_seq[0].clock_div_idx]);
            oled_set(3, "S2 CRP:%2d%%  DIV: %s",
                     (int)(g_p2_corrupt[1] * 100),
                     CLOCK_DIV_NAMES[g_seq[1].clock_div_idx]);
            oled_set(4, "tap=pg  hold=run%s",
                     g_midi_sync ? " EXT" : "");
            break;
        }
        case 3: { // SCALE
            oled_set(1, "[SCALE] KB:%s",
                     g_kb_mode == KB_TRANSPOSE ? "XPOSE" : "BFLIP");
            if (g_kb_mode == KB_TRANSPOSE) {
                // 21 chars: "S1:MelodicMinor  +12" = 4+12+2+3 = 21 ✓
                oled_set(2, "S1:%-12s%+4dst",
                         SCALE_NAMES[g_seq[0].scale_idx],
                         g_transpose[0]);
                oled_set(3, "S2:%-12s%+4dst",
                         SCALE_NAMES[g_seq[1].scale_idx],
                         g_transpose[1]);
                oled_set(4, "Key S1:%s  S2:%s",
                         NOTE_NAMES[g_seq[0].root_note],
                         NOTE_NAMES[g_seq[1].root_note]);
            } else {
                oled_set(2, "S1:%-13s %s",
                         SCALE_NAMES[g_seq[0].scale_idx],
                         NOTE_NAMES[g_seq[0].root_note]);
                oled_set(3, "S2:%-13s %s",
                         SCALE_NAMES[g_seq[1].scale_idx],
                         NOTE_NAMES[g_seq[1].root_note]);
                oled_set(4, "K1-4:scl/key S1/S2");
            }
            break;
        }
        case 4: { // MIDI
            const char* l0 = cc_label(g_seq[0].midi_cc);
            const char* l1 = cc_label(g_seq[1].midi_cc);
            oled_set(1, "[MIDI]");
            oled_set(2, "S1: CH:%-2d  CC:%-3d%s",
                     g_seq[0].midi_channel, g_seq[0].midi_cc, l0);
            oled_set(3, "S2: CH:%-2d  CC:%-3d%s",
                     g_seq[1].midi_channel, g_seq[1].midi_cc, l1);
            oled_set(4, "Note+CC per step");
            break;
        }
        case 5: { // SETTINGS
            const char* vm_names[] = {"Fixed","FromCV","Accent"};
            const char* kb_names[] = {"BitFlip","Xpose"};
            oled_set(1, "[SETTINGS]");
            oled_set(2, "Vel:%-6s Len:%d%%",
                     vm_names[g_velocity_mode], g_note_length_pct);
            oled_set(3, "KB:%-7s",
                     kb_names[g_kb_mode]);
            oled_set(4, "");
            break;
        }
    }
    oled_set(5, g_chain_seq ? "CHAIN:%s" : "", g_chain_seq ? "ON" : "");
}

static void update_led() {
    int colour = 0;
    if (g_seq_running && g_chain_seq) {
        colour = 6; // purple — chain mode running
    } else {
        switch (g_page) {
            case 1: colour = 3; break; // green
            case 2: colour = 4; break; // cyan
            case 3: colour = 5; break; // blue
            case 4: colour = 2; break; // yellow
            case 5: colour = 7; break; // white
        }
    }
    led_set(colour);
}

//==========================================================================
// KNOB MAPPING HELPERS
//==========================================================================

// Map raw 0–1023 to float 0.0–1.0
static float knob_f(int raw) { return raw / 1023.0f; }

// Map raw to BPM using exponential scaling (40–240)
// Using: bpm = 40 * (240/40)^(raw/1023) = 40 * 6^(t)
static int knob_bpm(int raw) {
    float t = raw / 1023.0f;
    float bpm = 40.0f * powf(6.0f, t);  // 40 at t=0, 240 at t=1
    return std::max(40, std::min(240, (int)roundf(bpm)));
}

// Map raw to stepped integer in [lo, hi]
static int knob_stepped(int raw, int lo, int hi) {
    int range = hi - lo + 1;
    int v = (int)((raw / 1024.0f) * range) + lo;
    return std::max(lo, std::min(hi, v));
}

// Map raw to clock_div index (6 values)
static int knob_clock_div(int raw) {
    int idx = (int)((raw / 1024.0f) * NUM_CLOCK_DIVS);
    return std::max(0, std::min(NUM_CLOCK_DIVS-1, idx));
}

// Map raw to note length pct (10–100 %)
static int knob_note_len(int raw) {
    int v = 10 + (int)((raw / 1023.0f) * 90.0f);
    return std::max(10, std::min(100, v));
}

//==========================================================================
// KNOB HANDLERS  (Section 4.3–4.7)
//==========================================================================

static void handle_knobs(int k[4]) {
    // Soft takeover: when the page changes, latch entry knob positions.
    // Page 4 (MIDI) uses this to prevent channels from jumping the moment
    // the user enters the page with knobs at arbitrary positions.
    if (g_page != g_knob_prev_page) {
        for (int i = 0; i < 4; i++) {
            g_knob_entry_raw[i] = k[i];
            g_knob_has_moved[i] = false;
            g_knob_last_raw[i]  = k[i];  // reset delta baseline on page change
        }
        g_knob_prev_page = g_page;
    }
    // Unlock a knob once it moves > 32 ticks (~3% of range) from entry position
    for (int i = 0; i < 4; i++) {
        if (!g_knob_has_moved[i] && g_knob_entry_raw[i] >= 0 &&
            abs(k[i] - g_knob_entry_raw[i]) > 32)
            g_knob_has_moved[i] = true;
    }

    // Per-knob delta: only apply a knob that physically moved this frame.
    // Threshold >2 absorbs ADC jitter (±1–2 units at rest).
    // g_knob_last_raw[i] < 0 means not yet initialised — skip silently.
    bool knob_moved[4] = {};
    for (int i = 0; i < 4; i++) {
        if (g_knob_last_raw[i] >= 0)
            knob_moved[i] = (abs(k[i] - g_knob_last_raw[i]) > 2);
        g_knob_last_raw[i] = k[i];
    }

    pthread_mutex_lock(&g_state_mutex);
    switch (g_page) {
        case 1: // PERFORM
            if (knob_moved[0]) g_bpm             = knob_bpm(k[0]);
            if (knob_moved[1]) g_seq[0].cv_range = knob_f(k[1]);
            if (knob_moved[2]) g_seq[1].cv_range = knob_f(k[2]);
            if (knob_moved[3]) g_master_corrupt  = knob_f(k[3]);
            break;
        case 2: // CORRUPT+CLK
            if (knob_moved[0]) g_p2_corrupt[0]        = knob_f(k[0]);
            if (knob_moved[1]) g_seq[0].clock_div_idx = knob_clock_div(k[1]);
            if (knob_moved[2]) g_p2_corrupt[1]        = knob_f(k[2]);
            if (knob_moved[3]) g_seq[1].clock_div_idx = knob_clock_div(k[3]);
            break;
        case 3: // SCALE
            if (knob_moved[0]) g_seq[0].scale_idx = knob_stepped(k[0], 0, 15);
            if (knob_moved[1]) g_seq[0].root_note = knob_stepped(k[1], 0, 11);
            if (knob_moved[2]) g_seq[1].scale_idx = knob_stepped(k[2], 0, 15);
            if (knob_moved[3]) g_seq[1].root_note = knob_stepped(k[3], 0, 11);
            break;
        case 4: // MIDI — soft-takeover: only apply after knob moves from entry position
            if (g_knob_has_moved[0]) g_seq[0].midi_channel = knob_stepped(k[0], 1, 16);
            if (g_knob_has_moved[1]) g_seq[0].midi_cc      = knob_stepped(k[1], 0, 127);
            if (g_knob_has_moved[2]) g_seq[1].midi_channel = knob_stepped(k[2], 1, 16);
            if (g_knob_has_moved[3]) g_seq[1].midi_cc      = knob_stepped(k[3], 0, 127);
            break;
        case 5: // SETTINGS
            if (knob_moved[0]) g_velocity_mode   = knob_stepped(k[0], 0, 2);
            if (knob_moved[1]) g_note_length_pct = knob_note_len(k[1]);
            if (knob_moved[2]) g_kb_mode         = knob_stepped(k[2], 0, 1);
            // k[3] reserved
            break;
    }
    pthread_mutex_unlock(&g_state_mutex);
}

//==========================================================================
// KEY HANDLERS  (Section 4.3 — Page 1 keys, always active)
//==========================================================================

static void handle_key(int key_idx, int vel) {
    // AUX = key 0 — long/short press detection (handles both press and release)
    if (key_idx == 0) {
        if (vel > 0) {
            // Key down: record timestamp
            g_aux_press_time = now_us();
        } else {
            // Key up: decide action based on hold duration
            uint64_t held = now_us() - g_aux_press_time;
            if (held >= AUX_LONG_PRESS_US) {
                // Long press: toggle transport (ignored when MIDI sync drives transport)
                if (!g_midi_sync) {
                    pthread_mutex_lock(&g_state_mutex);
                    g_seq_running = !g_seq_running;
                    if (!g_seq_running) {
                        for (int si = 0; si < NUM_SEQS; si++) {
                            if (g_seq[si].last_note >= 0) {
                                midi_note_off(g_seq[si].midi_channel, g_seq[si].last_note);
                                g_seq[si].last_note = -1;
                            }
                        }
                    }
                    pthread_mutex_unlock(&g_state_mutex);
                    update_led();
                }
            } else {
                // Short press: cycle through pages
                g_page = (g_page % NUM_PAGES) + 1;
                update_led();
            }
        }
        return;
    }

    if (vel == 0) return;  // ignore key-up for all other keys

    pthread_mutex_lock(&g_state_mutex);

    // ── KB_TRANSPOSE mode ────────────────────────────────────────────────────
    // Each octave of the Organelle keyboard transposes one sequencer.
    // Organelle keyboard layout (keys 1–24, two 12-note octaves):
    //   Keys  1–12  → SEQ1 transpose  (key N maps to semitone offset (N-7))
    //   Keys 13–24  → SEQ2 transpose  (key N maps to semitone offset (N-19))
    // Centre key (key 7 / key 19) = 0 semitones (no transpose).
    // Range: ±5 semitones per octave zone (one fifth up/down from centre).
    if (g_kb_mode == KB_TRANSPOSE) {
        if (key_idx >= 1 && key_idx <= 12) {
            // Map key 1–12 to -6..+5 semitones centred on key 7
            g_transpose[0] = std::max(-12, std::min(12, key_idx - 7));
        } else if (key_idx >= 13 && key_idx <= 24) {
            // Map key 13–24 to -6..+5 semitones centred on key 19
            g_transpose[1] = std::max(-12, std::min(12, key_idx - 19));
        }
        pthread_mutex_unlock(&g_state_mutex);
        return;
    }

    // ── KB_BITFLIP mode (default) ────────────────────────────────────────────
    // Keys 1–8: SEQ1 BIT FLIP
    if (key_idx >= 1 && key_idx <= 8) {
        int step = key_idx - 1;
        g_seq[0].gate[step] = !g_seq[0].gate[step];
        if (g_seq[0].gate[step])  g_seq[0].cv[step] = rand_float_11();
        else                      g_seq[0].cv[step]  = 0.0f;
    }
    // Key 9: SEQ1 BIT SHIFT
    else if (key_idx == 9) {
        SeqState& s = g_seq[0];
        bool  last_g = s.gate[s.length-1];
        float last_c = s.cv[s.length-1];
        for (int i = s.length-1; i > 0; i--) { s.gate[i]=s.gate[i-1]; s.cv[i]=s.cv[i-1]; }
        s.gate[0] = last_g; s.cv[0] = last_c;
    }
    // Key 10: SEQ1 LENGTH--
    else if (key_idx == 10) {
        g_seq[0].length = (g_seq[0].length == 1) ? 8 : g_seq[0].length - 1;
        if (g_seq[0].play_head >= g_seq[0].length) g_seq[0].play_head = 0;
    }
    // Keys 13–20: SEQ2 BIT FLIP
    else if (key_idx >= 13 && key_idx <= 20) {
        int step = key_idx - 13;
        g_seq[1].gate[step] = !g_seq[1].gate[step];
        if (g_seq[1].gate[step])  g_seq[1].cv[step] = rand_float_11();
        else                      g_seq[1].cv[step]  = 0.0f;
    }
    // Key 21: SEQ2 BIT SHIFT
    else if (key_idx == 21) {
        SeqState& s = g_seq[1];
        bool  last_g = s.gate[s.length-1];
        float last_c = s.cv[s.length-1];
        for (int i = s.length-1; i > 0; i--) { s.gate[i]=s.gate[i-1]; s.cv[i]=s.cv[i-1]; }
        s.gate[0] = last_g; s.cv[0] = last_c;
    }
    // Key 22: SEQ2 LENGTH--
    else if (key_idx == 22) {
        g_seq[1].length = (g_seq[1].length == 1) ? 8 : g_seq[1].length - 1;
        if (g_seq[1].play_head >= g_seq[1].length) g_seq[1].play_head = 0;
    }
    // Key 23: CHAIN SEQ toggle
    else if (key_idx == 23) {
        g_chain_seq   = !g_chain_seq;
        g_chain_state = 0;
        g_seq[0].play_head = 0;
        g_seq[1].play_head = 0;
    }
    // Key 24: RESET both play heads
    else if (key_idx == 24) {
        g_seq[0].play_head = 0;
        g_seq[1].play_head = 0;
        g_chain_state = 0;
    }

    pthread_mutex_unlock(&g_state_mutex);
}

//==========================================================================
// ENCODER / PAGE NAVIGATION  (Section 4.2)
//==========================================================================

static void handle_encoder_delta(int delta) {
    g_page += delta;
    if (g_page < 1) g_page = NUM_PAGES;
    if (g_page > NUM_PAGES) g_page = 1;
    update_led();
}

//==========================================================================
// OSC RECEIVE LOOP
//==========================================================================

static bool osc_setup() {
    // Receive socket
    g_recv_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_recv_sock < 0) { log_msg("[OSC] recv socket failed\n"); return false; }
    int flags = fcntl(g_recv_sock, F_GETFL, 0);
    fcntl(g_recv_sock, F_SETFL, flags | O_NONBLOCK);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(MOTHER_RECV_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(g_recv_sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        log_msg("[OSC] bind port %d failed: %s\n", MOTHER_RECV_PORT, strerror(errno));
        return false;
    }

    // Send socket
    g_send_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_send_sock < 0) { log_msg("[OSC] send socket failed\n"); return false; }
    memset(&g_send_addr, 0, sizeof(g_send_addr));
    g_send_addr.sin_family      = AF_INET;
    g_send_addr.sin_port        = htons(MOTHER_SEND_PORT);
    inet_pton(AF_INET, MOTHER_IP, &g_send_addr.sin_addr);

    log_msg("[OSC] Listening on port %d\n", MOTHER_RECV_PORT);
    return true;
}

static void osc_poll() {
    uint8_t buf[1024];
    struct sockaddr_in src;
    socklen_t src_len = sizeof(src);
    int n;

    while ((n = recvfrom(g_recv_sock, buf, sizeof(buf)-1, 0,
                         (struct sockaddr*)&src, &src_len)) > 0) {
        buf[n] = 0;
        char addr[64] = {0};
        int  args[8]  = {0};
        int  nargs    = osc_parse(buf, n, addr, args, 8);

        if (strcmp(addr, "/knobs") == 0 && nargs >= 4) {
            int k[4] = { args[0], args[1], args[2], args[3] };
            handle_knobs(k);
            // Handle encoder as knob5 (index 4)
            if (nargs >= 5) {
                int enc_raw = args[4];
                if (g_encoder_prev >= 0) {
                    int delta = enc_raw - g_encoder_prev;
                    if (abs(delta) > 32) { // threshold to avoid noise
                        handle_encoder_delta(delta > 0 ? 1 : -1);
                    }
                }
                g_encoder_prev = enc_raw;
            }
        }
        else if (strcmp(addr, "/encoder") == 0 && nargs >= 1) {
            // Relative encoder: +1 or -1
            handle_encoder_delta(args[0] > 0 ? 1 : -1);
        }
        else if (strcmp(addr, "/key") == 0 && nargs >= 2) {
            handle_key(args[0], args[1]);
        }
        else if (strcmp(addr, "/quit") == 0) {
            log_msg("[OSC] /quit received — shutting down\n");
            g_running = false;
        }
    }
}

//==========================================================================
// INITIALISE DEFAULTS
//==========================================================================

static void init_defaults() {
    srand((unsigned)time(NULL));

    for (int si = 0; si < NUM_SEQS; si++) {
        SeqState& s = g_seq[si];
        memset(&s, 0, sizeof(s));
        s.length       = 8;
        s.play_head    = 0;
        s.cv_range     = 0.6f;
        s.corrupt      = 0.0f;
        s.clock_div_idx= 2;          // /1 (index 2: x4,x2,/1,/2,/3,/4,/6,/8)
        s.midi_channel = si + 1;   // SEQ1→ch1, SEQ2→ch2
        s.midi_cc      = (si == 0) ? 74 : 71;  // Filter / Resonance
        s.scale_idx    = 2;         // Major
        s.root_note    = 0;         // C
        s.tick_counter = 0;
        s.last_note    = -1;
        // Default gate pattern: alternating for demo
        for (int i = 0; i < NUM_STEPS; i++) {
            s.gate[i] = (i % 2 == 0);
            s.cv[i]   = s.gate[i] ? rand_float_11() : 0.0f;
        }
    }
    g_bpm           = 120;
    g_master_corrupt= 0.0f;
    g_velocity_mode = VEL_FIXED;
    g_note_length_pct = 50;
    g_kb_mode         = KB_BITFLIP;
    g_transpose[0]    = 0;
    g_transpose[1]    = 0;
    g_chain_seq       = false;
    g_chain_state     = 0;

    memset(g_oled_lines, 0, sizeof(g_oled_lines));
    memset(g_oled_prev,  ' ', sizeof(g_oled_prev));
}

//==========================================================================
// SIGNAL HANDLER
//==========================================================================

static void sig_handler(int sig) {
    log_msg("[SIG] Signal %d received — shutting down\n", sig);
    g_running = false;
}

//==========================================================================
// MAIN
//==========================================================================

int main(int argc, char* argv[]) {
    // Create patch directory and open crash log
    mkdir("/usbdrive/Patches/LabyrinthO", 0755);
    g_crash_log = fopen(CRASH_LOG_PATH, "a");
    log_msg("\n=== LABYRINTH-O START ===\n");

    // Signal handling
    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);

    init_defaults();

    // ALSA MIDI setup
    g_midi_connected = alsa_setup();
    if (!g_midi_connected)
        log_msg("[WARN] No MIDI output connected\n");

    // Dedicated MIDI input thread — ALSA via ttymidi
    pthread_create(&g_midi_thread, NULL, midi_input_thread_fn, NULL);

    // OSC setup
    if (!osc_setup()) {
        log_msg("[FATAL] OSC setup failed\n");
        return 1;
    }

    // Clock thread
    pthread_t clock_tid;
    pthread_create(&clock_tid, NULL, clock_thread_fn, NULL);

    // Initial OLED + LED
    update_oled();
    oled_flush();
    update_led();

    // Show startup state on OLED line 5
    if (!g_midi_connected) {
        oled_set(5, "NO MIDI - check USB");
        oled_flush();
    }

    log_msg("[MAIN] Running. Page=%d BPM=%d\n", g_page, g_bpm);

    uint64_t last_oled = now_us();

    // ── Main loop ────────────────────────────────────────────────────────
    while (g_running) {
        // Poll OSC (non-blocking)
        osc_poll();

        // Poll MIDI input for clock/transport sync
        process_midi_input();

        // Process scheduled note-offs
        process_note_offs();

        // Rate-limited OLED update
        uint64_t t = now_us();
        if (t - last_oled >= (uint64_t)OLED_RATE_MS * 1000ULL) {
            last_oled = t;
            update_oled();
            oled_flush();
        }

        // Sleep 1ms to yield CPU
        struct timespec ts = {0, 1000000L};
        nanosleep(&ts, NULL);
    }

    // ── Shutdown ─────────────────────────────────────────────────────────
    log_msg("[MAIN] Shutting down\n");

    // Stop sequencer, silence all notes
    g_seq_running = false;
    for (int si = 0; si < NUM_SEQS; si++) {
        if (g_seq[si].last_note >= 0)
            midi_note_off(g_seq[si].midi_channel, g_seq[si].last_note);
    }

    pthread_join(g_midi_thread, NULL);
    pthread_join(clock_tid, NULL);

    if (g_alsa_seq) snd_seq_close(g_alsa_seq);
    if (g_recv_sock >= 0) close(g_recv_sock);
    if (g_send_sock >= 0) close(g_send_sock);

    // Clear OLED
    oled_set(1, "LABYRINTH-O");
    oled_set(2, "Stopped.");
    oled_set(3, ""); oled_set(4, ""); oled_set(5, "");
    oled_flush();
    led_set(0);

    if (g_crash_log) fclose(g_crash_log);
    log_msg("=== LABYRINTH-O STOP ===\n");
    return 0;
}