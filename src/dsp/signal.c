/**
 * Signal — Polymetric microsound rhythm generator
 * Author: fillioning  |  License: GPL-3.0
 *
 * Inspired by Ryoji Ikeda, Nicolas Bernier (frequencies/sound quanta),
 * The Dillinger Escape Plan, and Converge.
 *
 * Architecture: 4 independent self-sequencing micro-synth voices.
 * Each voice = rhythm engine (one of 25 presets) + micro-synth (one of 25 presets).
 * 25^4 = 390,625 unique rhythmic combinations.
 *
 * API: plugin_api_v2_t, 44100 Hz, 128 frames/block, stereo int16 output.
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>

#define SAMPLE_RATE   44100.0f
#define SR_INV        (1.0f / 44100.0f)
#define TWO_PI        6.283185307f
#define NUM_VOICES    4
#define NUM_PATTERN_PRESETS    25   /* 25 existing voice-dependent pattern presets (idx 0-24) */
#define NUM_BREAKBEAT_PRESETS  10   /* 10 voice-independent breakbeat presets (idx 25-34) */
#define NUM_SEQ_PRESETS        (NUM_PATTERN_PRESETS + NUM_BREAKBEAT_PRESETS)  /* 35 */
#define NUM_SYN_PRESETS        50   /* 40 existing + 10 kit synthesis presets */
#define BB_STEP_GRID_MAX       32   /* anchor curves are 32-element arrays */

/* ── Schwung API typedefs ──────────────────────────────────────────────────── */
typedef struct {
    uint32_t api_version;
    void* (*create_instance)(const char *, const char *);
    void  (*destroy_instance)(void *);
    void  (*on_midi)(void *, const uint8_t *, int, int);
    void  (*set_param)(void *, const char *, const char *);
    int   (*get_param)(void *, const char *, char *, int);
    int   (*get_error)(void *, char *, int);
    void  (*render_block)(void *, int16_t *, int);
} plugin_api_v2_t;

/* ── Enumerations ─────────────────────────────────────────────────────────── */
enum { WAVE_SINE=0, WAVE_IMPULSE, WAVE_NOISE, WAVE_DAMPED,
       WAVE_CLICK, WAVE_SQUARE, WAVE_TRI, WAVE_FM,
       WAVE_PINK, WAVE_BROWN, WAVE_AM, WAVE_PING, WAVE_TRAIN,
       WAVE_KICK, WAVE_SNARE, WAVE_HAT, WAVE_COUNT };
static const char *WAVE_NAMES[] = {
    "Sine","Impulse","Noise","Damped","Click","Square","Tri","FM",
    "Pink","Brown","AM","Ping","Train","Kick","Snare","Hat"
};

/* ── Breakbeat / Holzman / Scene enums ────────────────────────────────────── */
enum { ROLE_ANY=0, ROLE_KICK, ROLE_SNARE, ROLE_HAT, ROLE_GHOST, ROLE_PERC, ROLE_COUNT };
static const char *ROLE_NAMES[] = { "Any","Kick","Snare","Hat","Ghost","Perc" };

enum { FILL_SNARE_ROLL=0, FILL_TOM_RUN, FILL_KICK_FILL, FILL_HAT_SPLATTER,
       FILL_CRASH_DROP, FILL_RANDOM, FILL_COUNT };
static const char *FILL_NAMES[] = {
    "Snare Roll","Tom Run","Kick Fill","Hat Splatter","Crash Drop","Random"
};

enum { PHRASE_OFF=0, PHRASE_2, PHRASE_4, PHRASE_8, PHRASE_16, PHRASE_COUNT };
static const char *PHRASE_NAMES[] = { "Off","2","4","8","16" };
static const int PHRASE_BARS[] = { 0, 2, 4, 8, 16 };

enum { STEP_GRID_8=0, STEP_GRID_16, STEP_GRID_32, STEP_GRID_COUNT };
static const char *STEP_GRID_NAMES[] = { "8","16","32" };
static const int STEP_GRID_VALUES[] = { 8, 16, 32 };

enum { RETRIG_2X=0, RETRIG_3X, RETRIG_4X, RETRIG_8X, RETRIG_RAND, RETRIG_COUNT };
static const char *RETRIG_NAMES[] = { "2x","3x","4x","8x","Rand" };
static const int RETRIG_RATES[] = { 2, 3, 4, 8, 0 };

enum { DRUMMER_BRAIN_OFF=0, DRUMMER_BRAIN_LIGHT, DRUMMER_BRAIN_HEAVY, DRUMMER_BRAIN_COUNT };
static const char *DRUMMER_BRAIN_NAMES[] = { "Off","Light","Heavy" };
static const float DRUMMER_BRAIN_AMP[] = { 0.0f, 0.2f, 0.5f };

enum { MORPH_LINEAR=0, MORPH_EXP, MORPH_STEPPED, MORPH_CURVE_COUNT };
static const char *MORPH_CURVE_NAMES[] = { "Linear","Exp","Stepped" };

enum { BANK_OFF=0, BANK_RULE, BANK_RANDOM, BANK_MODE_COUNT };
static const char *BANK_MODE_NAMES[] = { "Off","Rule","Random" };

/* ── Momentary action knobs ─────────────────────────────────────────────────
 * For knobs whose user-facing behaviour is "tap to fire, snap back to 0",
 * we route through a small state machine: rising-edge fires once, then a
 * flash counter holds the read-back value at "1" for ~80 ms so the user
 * sees the gesture register on screen, then automatically reverts to "0".
 * Next right-turn fires again — no need to turn the knob left first. */
enum { ACT_RND_PATCH=0, ACT_RND_RYTM, ACT_RND_VOICES, ACT_SAME_VOICE,
       ACT_RND_MOD, ACT_RND_PITCH, ACT_RND_PAN, ACT_MOD_RESET,
       ACT_SAVE_SCENE_A, ACT_SAVE_SCENE_B, NUM_ACTIONS };
#define ACTION_FLASH_BLOCKS  28   /* ~80 ms at 44.1 kHz / 128-frame blocks */

enum { ROOT_FREE=0, ROOT_C, ROOT_CS, ROOT_D, ROOT_DS, ROOT_E,
       ROOT_F, ROOT_FS, ROOT_G, ROOT_GS, ROOT_A, ROOT_AS, ROOT_B, ROOT_COUNT };
static const char *ROOT_NAMES[] = {
    "Free","C","C#","D","D#","E","F","F#","G","G#","A","A#","B"
};

enum { SCALE_FREE=0, SCALE_CHROMATIC, SCALE_MAJOR, SCALE_MINOR,
       SCALE_PENTATONIC, SCALE_WHOLETONE, SCALE_DIMINISHED, SCALE_HARM_MIN,
       SCALE_COUNT };
static const char *SCALE_NAMES[] = {
    "Free","Chromatic","Major","Minor","Pentatonic","WholeTone","Diminished","HarmMin"
};

enum { PAGE_ROOT=0, PAGE_GENERATE, PAGE_PATCH, PAGE_PARAMS, PAGE_MODULATION,
       PAGE_MIX, PAGE_VOICE1, PAGE_VOICE2, PAGE_VOICE3, PAGE_VOICE4, PAGE_GENERAL };
static const char *PAGE_NAMES[] = {
    "Signal","generate","patch","params","modulation","mix",
    "voice1","voice2","voice3","voice4","general"
};

/* ── Rhythm pattern data ──────────────────────────────────────────────────── */
/*
 * subdivision = notes per beat (1=quarter, 2=8th, 4=16th, 8=32nd, 16=64th)
 * Morse dit=1 step, dah=3 steps, inter-element=1, inter-char=3, inter-word=7
 * At sub=8 and 120 BPM: 1 step = 62.5 ms (fast Morse, fine for rhythm)
 */
typedef struct {
    const uint8_t *hits;
    int            length;
    int            subdivision; /* notes per beat */
} rhythm_pattern_t;

/* ─── Voice 0: Morse focus + mathcore 7-feel + Ikeda-3 + prime sparse + CA ─ */
/* Morse SOS (sub=8) */
static const uint8_t V0P00[] = {1,0,1,0,1,0,0,0,1,1,1,0,1,1,1,0,1,1,1,0,0,0,1,0,1,0,1,0,0,0,0,0};
/* Morse DE (sub=8) */
static const uint8_t V0P01[] = {1,1,1,0,1,0,1,0,0,1,0,0,0,0,0,0};
/* Morse CQ (sub=8) */
static const uint8_t V0P02[] = {1,1,1,0,1,0,1,1,1,0,1,0,0,1,1,1,0,1,1,1,0,1,0,1,1,1,0,0,0,0,0,0};
/* Morse K / invitation (sub=8) */
static const uint8_t V0P03[] = {1,1,1,0,1,0,1,1,1,0,0,0,0,0,0,0};
/* Morse AR / end of message (sub=8) */
static const uint8_t V0P04[] = {1,0,1,1,1,0,1,0,1,0,1,1,1,0,0,0};
/* Mathcore [3+2+2] 7-feel (sub=4) */
static const uint8_t V0P05[] = {1,0,0,1,0,1,0};
/* Mathcore [2+3+2] 7-feel variant (sub=4) */
static const uint8_t V0P06[] = {1,0,1,0,0,1,0};
/* Mathcore [5+3+3+4] 15-feel (sub=4) */
static const uint8_t V0P07[] = {1,0,0,0,0,1,0,0,1,0,0,1,0,0,0};
/* Mathcore [4+7] 11-feel (sub=4) */
static const uint8_t V0P08[] = {1,0,0,0,1,0,0,0,0,0,0};
/* Mathcore [5+4+4] 13-feel (sub=4) */
static const uint8_t V0P09[] = {1,0,0,0,0,1,0,0,0,1,0,0,0};
/* Ikeda every-3-dropout 32nd grid (sub=8) */
static const uint8_t V0P10[] = {1,1,0,1,1,0,1,1,0,1,1,0,1,1,0,1,1,0,1,1,0,1,1,0};
/* Ikeda alternating pairs (sub=8) */
static const uint8_t V0P11[] = {1,1,0,0,1,1,0,0,1,1,0,0,1,1,0,0};
/* Ikeda dense burst then silence (sub=8) */
static const uint8_t V0P12[] = {1,1,1,1,0,0,0,0,1,1,1,0,0,0,0,0};
/* Ikeda triplet 32nds (sub=8) */
static const uint8_t V0P13[] = {1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0};
/* Ikeda prime-position dropout (sub=8) — rests at prime steps 2,3,5,7,11,13 */
static const uint8_t V0P14[] = {1,1,0,0,1,0,1,0,1,1,1,0,1,0,1,1,1,1,1,0,1,1,1,0,1,1,1,1,1,0,1,0};
/* Bernier primes [2,3,5,7,11] (sub=2) */
static const uint8_t V0P15[] = {1,0,1,0,0,1,0,0,0,0,1,0,1,0,0,0};
/* Bernier Fibonacci [1,2,3,5,8,13] (sub=2) */
static const uint8_t V0P16[] = {1,1,0,1,0,0,1,0,0,0,0,1,0,0,0,0};
/* Bernier single ping 7-step (sub=2) */
static const uint8_t V0P17[] = {1,0,0,0,0,0,0};
/* Bernier two pings 11-step (sub=2) */
static const uint8_t V0P18[] = {1,0,0,0,0,0,1,0,0,0,0};
/* Bernier ultra-sparse quarter notes (sub=1) */
static const uint8_t V0P19[] = {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
/* Pi binary fraction (sub=4) */
static const uint8_t V0P20[] = {1,0,0,1,0,0,0,0,1,1,1,1,1,1,0,0};
/* e binary fraction (sub=4) */
static const uint8_t V0P21[] = {1,0,1,1,0,1,1,1,1,1,1,0,0,0,0,1};
/* Phi binary fraction (sub=4) */
static const uint8_t V0P22[] = {1,0,0,1,1,1,1,0,0,0,1,1,0,1,1,1};
/* Rule 30 CA sequence (sub=4) */
static const uint8_t V0P23[] = {1,1,0,1,1,0,1,0,1,1,1,0,1,0,1,0};
/* Rule 110 CA sequence (sub=4) */
static const uint8_t V0P24[] = {1,1,0,1,1,1,0,0,1,0,1,1,1,1,0,0};

/* ─── Voice 1: Morse QRZ + mathcore 11-feel + Ikeda burst + different sparse ─ */
/* Morse QR (sub=8) */
static const uint8_t V1P00[] = {1,1,1,0,1,1,1,0,1,0,1,1,1,0,0,1,0,1,1,1,0,1,0,0};
/* Morse 73 best regards (sub=8) */
static const uint8_t V1P01[] = {1,1,1,0,1,1,1,0,1,0,1,0,1,0,0,1,0,1,0,1,0,1,1,1,0,1,1,1,0,0,0,0};
/* Morse RST (sub=8) */
static const uint8_t V1P02[] = {1,0,1,1,1,0,1,0,0,1,0,1,0,1,0,0,1,1,1,0,0,0,0,0};
/* Morse PSE (please) (sub=8) */
static const uint8_t V1P03[] = {1,0,1,1,1,0,1,1,1,0,1,0,0,1,0,1,0,1,0,0,1,0,0,0};
/* Morse HH (error) — 8 dits (sub=8) */
static const uint8_t V1P04[] = {1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0};
/* Mathcore [5+3] 8-feel (sub=4) */
static const uint8_t V1P05[] = {1,0,0,0,0,1,0,0};
/* Mathcore [3+4] 7-feel (sub=4) */
static const uint8_t V1P06[] = {1,0,0,1,0,0,0};
/* Mathcore [3+4+4] 11-feel (sub=4) */
static const uint8_t V1P07[] = {1,0,0,1,0,0,0,1,0,0,0};
/* Mathcore [5+4+4] 13-feel (sub=4) */
static const uint8_t V1P08[] = {1,0,0,0,0,1,0,0,0,1,0,0,0};
/* Mathcore [5+7+5] 17-feel (sub=4) */
static const uint8_t V1P09[] = {1,0,0,0,0,1,0,0,0,0,0,0,1,0,0,0,0};
/* Ikeda 64th note grid (sub=16) */
static const uint8_t V1P10[] = {1,0,1,0,1,0,1,0};
/* Ikeda 32nd burst groups (sub=8) */
static const uint8_t V1P11[] = {1,1,1,0,0,0,0,0,1,1,1,0,0,0,0,0,1,1,1,0,0,0,0,0,1,1,1,0,0,0,0,0};
/* Ikeda alternating 1+2 (sub=8) */
static const uint8_t V1P12[] = {1,0,1,1,0,0,1,0,1,1,0,0,1,0,1,1,0,0,1,0,1,1,0,0};
/* Ikeda quintuplet (sub=8) */
static const uint8_t V1P13[] = {1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1,0,0,0,0};
/* Ikeda stutter build (sub=8) */
static const uint8_t V1P14[] = {1,0,0,0,0,0,0,0,1,1,0,0,0,0,0,0,1,1,1,0,0,0,0,0,1,1,1,1,0,0,0,0};
/* Bernier [3,5,7,11] primes (sub=2) */
static const uint8_t V1P15[] = {1,0,0,1,0,0,0,0,1,0,0,0,0,0,0,1};
/* Bernier 17-step cascade (sub=2) */
static const uint8_t V1P16[] = {1,0,0,0,0,0,0,1,0,0,0,0,0,1,0,0,0};
/* Bernier 11-step single (sub=2) */
static const uint8_t V1P17[] = {1,0,0,0,0,0,0,0,0,0,0};
/* Bernier 13-step two pings (sub=2) */
static const uint8_t V1P18[] = {1,0,0,0,0,0,0,1,0,0,0,0,0};
/* Bernier ultra-slow 8-step (sub=1) */
static const uint8_t V1P19[] = {1,0,0,0,0,0,0,0};
/* Sqrt(2) binary fraction (sub=4) */
static const uint8_t V1P20[] = {1,1,0,1,0,1,0,0,0,0,0,1,0,0,1,0};
/* Sqrt(3) binary fraction (sub=4) */
static const uint8_t V1P21[] = {1,0,1,1,1,0,1,1,0,0,0,0,1,0,1,1};
/* ln(2) binary fraction (sub=4) */
static const uint8_t V1P22[] = {1,0,1,1,0,0,0,1,0,1,1,1,0,0,1,0};
/* Rule 30 shifted (sub=4) */
static const uint8_t V1P23[] = {0,1,1,0,1,0,1,0,1,1,1,0,1,0,1,0};
/* Rule 90 XOR pattern (sub=4) */
static const uint8_t V1P24[] = {1,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1};

/* ─── Voice 2: Morse prosigns + mathcore + Ikeda density + Bernier + chaos ── */
/* Morse VVV (attention) (sub=8) */
static const uint8_t V2P00[] = {1,0,1,0,1,0,1,1,1,0,1,0,1,0,1,0,1,1,1,0,1,0,1,0,1,1,1,0,0,0,0,0};
/* Morse SK end-of-contact prosign (sub=8) */
static const uint8_t V2P01[] = {1,0,1,0,1,0,1,1,1,0,1,0,1,1,1,0,0,0,0,0,0,0,0,0};
/* Morse BK break (sub=8) */
static const uint8_t V2P02[] = {1,1,1,0,1,0,1,0,1,0,0,1,1,1,0,1,0,1,1,1,0,0,0,0};
/* Morse BT paragraph separator (sub=8) */
static const uint8_t V2P03[] = {1,1,1,0,1,0,1,0,1,0,1,1,1,0,0,0};
/* Morse SOS backward (sub=8) — ...---... reversed */
static const uint8_t V2P04[] = {0,0,0,0,1,0,1,0,1,0,0,0,1,1,1,0,1,1,1,0,1,1,1,0,0,0,1,0,1,0,1,0};
/* Mathcore [3+2+3+2] 10-feel (sub=4) */
static const uint8_t V2P05[] = {1,0,0,1,0,1,0,0,1,0};
/* Mathcore [4+3+4] 11-feel (sub=4) */
static const uint8_t V2P06[] = {1,0,0,0,1,0,0,1,0,0,0};
/* Mathcore [4+4+5] 13-feel (sub=4) */
static const uint8_t V2P07[] = {1,0,0,0,1,0,0,0,1,0,0,0,0};
/* Mathcore [7+6+4] 17-feel (sub=4) */
static const uint8_t V2P08[] = {1,0,0,0,0,0,0,1,0,0,0,0,0,1,0,0,0};
/* Mathcore [5+5+5+4] 19-feel (sub=4) */
static const uint8_t V2P09[] = {1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1,0,0,0};
/* Ikeda full 32nd density (sub=8) */
static const uint8_t V2P10[] = {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1};
/* Ikeda 3-on 3-off (sub=8) */
static const uint8_t V2P11[] = {1,1,1,0,0,0,1,1,1,0,0,0,1,1,1,0,0,0,1,1,1,0,0,0};
/* Ikeda gallop 64th (sub=16) */
static const uint8_t V2P12[] = {1,1,0,1,1,0,1,0};
/* Ikeda stutter accelerando (sub=8) */
static const uint8_t V2P13[] = {1,0,0,0,0,0,1,0,0,0,0,1,0,0,0,1,0,0,1,0,1,0,1,1};
/* Ikeda hocket (sub=8) */
static const uint8_t V2P14[] = {1,0,0,1,0,1,0,0,1,0,0,1,0,1,0,0};
/* Bernier 31-step prime (sub=2) */
static const uint8_t V2P15[] = {1,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0};
/* Bernier 13-step cascade (sub=2) */
static const uint8_t V2P16[] = {1,0,1,0,0,0,0,0,0,1,0,0,0};
/* Bernier 13-step single (sub=2) */
static const uint8_t V2P17[] = {1,0,0,0,0,0,0,0,0,0,0,0,0};
/* Bernier 16-step triple (sub=2) */
static const uint8_t V2P18[] = {1,0,0,0,1,0,0,0,0,0,1,0,0,0,0,0};
/* Bernier 3 pings quarter notes (sub=1) */
static const uint8_t V2P19[] = {1,0,0,0,0,1,0,0,0,0,0,0,1,0,0,0};
/* Logistic map r=3.8 (sub=4) */
static const uint8_t V2P20[] = {1,0,0,1,1,0,1,0,0,1,0,1,1,0,0,1};
/* Rule 60 CA (sub=4) */
static const uint8_t V2P21[] = {1,1,1,0,1,1,1,0,1,1,1,0,1,1,1,0};
/* Rule 90 pure alternating (sub=4) */
static const uint8_t V2P22[] = {1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0};
/* Cantor dust (sub=4, length=27) */
static const uint8_t V2P23[] = {1,0,0,1,0,0,1,0,0,0,0,0,0,0,0,0,0,0,1,0,0,1,0,0,1,0,0};
/* Stern-Brocot self-similar (sub=4) */
static const uint8_t V2P24[] = {1,1,0,1,0,1,0,0,1,1,0,1,0,1,0,0};

/* ─── Voice 3: Morse short IDs + mathcore prime lengths + Ikeda + Bernier min ─ */
/* Morse UR (sub=8) */
static const uint8_t V3P00[] = {1,0,1,0,1,1,1,0,0,1,0,1,1,1,0,1,0,0,0,0,0,0,0,0};
/* Morse NW (sub=8) */
static const uint8_t V3P01[] = {1,1,1,0,1,0,0,1,0,1,1,1,0,1,1,1,0,0,0,0,0,0,0,0};
/* Morse TU (sub=8) */
static const uint8_t V3P02[] = {1,1,1,0,0,1,0,1,0,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0};
/* Morse RR (repeat) (sub=8) */
static const uint8_t V3P03[] = {1,0,1,1,1,0,1,0,0,1,0,1,1,1,0,1,0,0,0,0,0,0,0,0};
/* Morse RIG (rigid) (sub=8) */
static const uint8_t V3P04[] = {1,0,1,1,1,0,1,0,0,1,0,1,0,0,1,1,1,0,1,1,1,0,1,0,0,0,0,0,0,0,0,0};
/* Mathcore [2+2+3] 7-feel (sub=4) */
static const uint8_t V3P05[] = {1,0,1,0,1,0,0};
/* Mathcore [6+5] 11-feel (sub=4) */
static const uint8_t V3P06[] = {1,0,0,0,0,0,1,0,0,0,0};
/* Mathcore [6+7] 13-feel (sub=4) */
static const uint8_t V3P07[] = {1,0,0,0,0,0,1,0,0,0,0,0,0};
/* Mathcore [9+8] 17-feel (sub=4) */
static const uint8_t V3P08[] = {1,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0};
/* Mathcore [7+12] 19-feel (sub=4) */
static const uint8_t V3P09[] = {1,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0};
/* Ikeda triplet 32nds (sub=8) — same as V0 but different length */
static const uint8_t V3P10[] = {1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0};
/* Ikeda every 5 (sub=8) */
static const uint8_t V3P11[] = {1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1,0,0,0,0};
/* Ikeda accelerando (sub=8) */
static const uint8_t V3P12[] = {1,0,0,0,0,0,1,0,0,0,0,1,0,0,0,1,0,0,1,0,1,0,1,1};
/* Ikeda syncopated 16ths (sub=4) */
static const uint8_t V3P13[] = {1,0,0,1,1,0,1,0,0,1,0,1,1,0,1,0};
/* Ikeda XOR grid (sub=8) */
static const uint8_t V3P14[] = {1,0,1,0,0,1,0,1,1,0,1,0,0,1,0,1};
/* Bernier 16-step primes mod (sub=2) */
static const uint8_t V3P15[] = {1,1,0,1,0,1,0,0,0,1,0,1,0,0,1,0};
/* Bernier near-asymmetric (sub=2) */
static const uint8_t V3P16[] = {1,0,0,0,1,0,0,0,0,1,0,0,0,0,0,0};
/* Bernier minimal 3-step (sub=2) */
static const uint8_t V3P17[] = {1,0,0};
/* Bernier 17-step single (sub=2) */
static const uint8_t V3P18[] = {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
/* Bernier 16-step two pings quarter notes (sub=1) */
static const uint8_t V3P19[] = {1,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0};
/* Thue-Morse sequence (sub=4) */
static const uint8_t V3P20[] = {1,0,0,1,0,1,1,0,0,1,1,0,1,0,0,1};
/* Fibonacci word (sub=4) */
static const uint8_t V3P21[] = {1,0,1,1,0,1,0,1,1,0,1,1,0,1,0,1};
/* Regular paperfolding (sub=4) */
static const uint8_t V3P22[] = {1,1,0,1,1,0,0,1,1,1,0,0,1,0,0,1};
/* Kolakoski sequence (sub=4) */
static const uint8_t V3P23[] = {1,0,0,1,1,0,1,0,0,1,0,0,1,1,0,1};
/* pi^2 binary fraction (sub=4) */
static const uint8_t V3P24[] = {1,0,1,0,0,1,1,0,1,1,1,0,0,1,1,1};

/* ── Pattern lookup table [4 voices][25 presets] ─────────────────────────── */
static const rhythm_pattern_t PATTERNS[NUM_VOICES][NUM_SEQ_PRESETS] = {
    { /* Voice 0 */
        {V0P00,32,8},{V0P01,16,8},{V0P02,32,8},{V0P03,16,8},{V0P04,16,8},
        {V0P05, 7,4},{V0P06, 7,4},{V0P07,15,4},{V0P08,11,4},{V0P09,13,4},
        {V0P10,24,8},{V0P11,16,8},{V0P12,16,8},{V0P13,24,8},{V0P14,32,8},
        {V0P15,16,2},{V0P16,16,2},{V0P17, 7,2},{V0P18,11,2},{V0P19,16,1},
        {V0P20,16,4},{V0P21,16,4},{V0P22,16,4},{V0P23,16,4},{V0P24,16,4}
    },
    { /* Voice 1 */
        {V1P00,24,8},{V1P01,32,8},{V1P02,24,8},{V1P03,24,8},{V1P04,16,8},
        {V1P05, 8,4},{V1P06, 7,4},{V1P07,11,4},{V1P08,13,4},{V1P09,17,4},
        {V1P10, 8,16},{V1P11,32,8},{V1P12,24,8},{V1P13,20,8},{V1P14,32,8},
        {V1P15,16,2},{V1P16,17,2},{V1P17,11,2},{V1P18,13,2},{V1P19, 8,1},
        {V1P20,16,4},{V1P21,16,4},{V1P22,16,4},{V1P23,16,4},{V1P24,16,4}
    },
    { /* Voice 2 */
        {V2P00,32,8},{V2P01,24,8},{V2P02,24,8},{V2P03,16,8},{V2P04,32,8},
        {V2P05,10,4},{V2P06,11,4},{V2P07,13,4},{V2P08,17,4},{V2P09,19,4},
        {V2P10,16,8},{V2P11,24,8},{V2P12, 8,16},{V2P13,24,8},{V2P14,16,8},
        {V2P15,31,2},{V2P16,13,2},{V2P17,13,2},{V2P18,16,2},{V2P19,16,1},
        {V2P20,16,4},{V2P21,16,4},{V2P22,16,4},{V2P23,27,4},{V2P24,16,4}
    },
    { /* Voice 3 */
        {V3P00,24,8},{V3P01,24,8},{V3P02,24,8},{V3P03,24,8},{V3P04,32,8},
        {V3P05, 7,4},{V3P06,11,4},{V3P07,13,4},{V3P08,17,4},{V3P09,19,4},
        {V3P10,24,8},{V3P11,20,8},{V3P12,24,8},{V3P13,16,4},{V3P14,16,8},
        {V3P15,16,2},{V3P16,16,2},{V3P17, 3,2},{V3P18,17,2},{V3P19,16,1},
        {V3P20,16,4},{V3P21,16,4},{V3P22,16,4},{V3P23,16,4},{V3P24,16,4}
    }
};

/* ── Breakbeat preset data ─────────────────────────────────────────────────── */
/*
 * 10 breakbeat presets (indices 25-34 in the seq_preset namespace, where 0=OFF
 * and 1-25 are the existing PATTERNS table — so user-facing knob values 26..35
 * map to BREAKBEAT_PRESETS[0..9]).
 *
 * Each curve is 32 elements (BB_STEP_GRID_MAX). At step_grid=32 the engine
 * reads every element; at step_grid=16 it reads every other (0,2,4,...);
 * at step_grid=8 it reads every fourth (0,4,8,...).
 *
 * Weight 0 = step never fires.  Weight 1.0 = step fires aggressively at high
 * Anchor (the canonical kick/snare anchor positions).
 */
typedef struct {
    const char *name;
    int   role;          /* ROLE_KICK / SNARE / HAT / GHOST / PERC / ANY */
    float anchor;        /* default Anchor 0-1 */
    float complexity;    /* default Complexity 0-1 */
    float roll;          /* default Roll 0-1 */
    float retrigger;     /* probability of stutter burst on each trigger */
    float curve[BB_STEP_GRID_MAX];
} breakbeat_preset_t;

static const breakbeat_preset_t BREAKBEAT_PRESETS[NUM_BREAKBEAT_PRESETS] = {
    /* 0 (seq=26) — Amen Kick: halftime kick on beat 1, beat 3, plus 1+ syncopation */
    {"Amen Kick", ROLE_KICK, 0.80f, 0.20f, 0.10f, 0.05f,
     {1.0f,0,0,0,  0,0,0,0,    0,0,0,0,    0,0,0,0,
      1.0f,0,0,0,  0.4f,0,0,0, 0,0,0,0,    0,0,0,0}},
    /* 1 (seq=27) — Amen Snare: beats 2 and 4 with ghost cluster on the 'and' */
    {"Amen Snare", ROLE_SNARE, 0.90f, 0.10f, 0.20f, 0.10f,
     {0,0,0,0,    0,0,0,0,    1.0f,0,0,0, 0.3f,0,0.4f,0,
      0,0,0,0,    0,0,0,0,    1.0f,0,0,0, 0.6f,0,0.5f,0}},
    /* 2 (seq=28) — Amen Hat: steady 8th-notes (sub-beats 0, 4, 8, ...) */
    {"Amen Hat", ROLE_HAT, 0.40f, 0.30f, 0.40f, 0.15f,
     {0.6f,0,0,0, 0.5f,0,0,0, 0.6f,0,0,0, 0.5f,0,0,0,
      0.6f,0,0,0, 0.5f,0,0,0, 0.6f,0,0,0, 0.5f,0,0,0}},
    /* 3 (seq=29) — Amen Ghost: anti-anchored, fills the gaps between kicks/snares */
    {"Amen Ghost", ROLE_GHOST, 0.50f, 0.50f, 0.60f, 0.20f,
     {0,0.3f,0,0.2f, 0,0.4f,0,0.3f, 0,0.5f,0,0.3f, 0,0.6f,0.2f,0.4f,
      0,0.4f,0,0.3f, 0,0.5f,0,0.3f, 0,0.4f,0,0.3f, 0,0.7f,0.3f,0.5f}},
    /* 4 (seq=30) — Think Snare: extra ghost approach into the main snare hits */
    {"Think Snare", ROLE_SNARE, 0.85f, 0.15f, 0.30f, 0.10f,
     {0,0.4f,0,0,   0,0,0,0,    1.0f,0,0,0, 0.5f,0,0.6f,0,
      0,0,0,0,     0,0,0,0,    1.0f,0,0,0, 0.4f,0,0.5f,0}},
    /* 5 (seq=31) — Funky Drummer Kick: Clyde Stubblefield syncopation */
    {"Funky Kick", ROLE_KICK, 0.70f, 0.30f, 0.20f, 0.08f,
     {1.0f,0,0,0,  0,0,0,0,    0,0,0.3f,0, 0,0,0,0,
      0,0,0,0,    1.0f,0,0,0, 0,0,0,0,    0,0,0,0}},
    /* 6 (seq=32) — Jungle Roll: high Roll value, ±1 walks dominate */
    {"Jungle Roll", ROLE_HAT, 0.30f, 0.50f, 0.80f, 0.30f,
     {0.7f,0.4f,0.5f,0.3f, 0.6f,0.4f,0.5f,0.3f, 0.7f,0.4f,0.5f,0.3f, 0.6f,0.4f,0.5f,0.3f,
      0.7f,0.4f,0.5f,0.3f, 0.6f,0.4f,0.5f,0.3f, 0.7f,0.4f,0.5f,0.3f, 0.6f,0.4f,0.5f,0.3f}},
    /* 7 (seq=33) — Liquid DnB Snare: sparse clean snares on 5 and 13 */
    {"Liquid Snare", ROLE_SNARE, 0.95f, 0.05f, 0.10f, 0.05f,
     {0,0,0,0,    0,0,0,0,    1.0f,0,0,0, 0,0,0.3f,0,
      0,0,0,0,    0,0,0,0,    1.0f,0,0,0, 0,0,0.3f,0}},
    /* 8 (seq=34) — Holzman Improv: mutation-receptive base curve */
    {"Holzman Improv", ROLE_ANY, 0.60f, 0.40f, 0.30f, 0.15f,
     {0.4f,0.2f,0.3f,0.2f, 0.3f,0.2f,0.4f,0.3f, 0.5f,0.2f,0.3f,0.2f, 0.8f,0.2f,0.4f,0.3f,
      0.3f,0.2f,0.3f,0.2f, 0.3f,0.2f,0.4f,0.3f, 0.5f,0.2f,0.3f,0.2f, 0.8f,0.2f,0.4f,0.3f}},
    /* 9 (seq=35) — Breakbeat Random: maximum stochastic, uniform weight */
    {"Breakbeat Rnd", ROLE_ANY, 0.20f, 0.80f, 0.50f, 0.20f,
     {0.3f,0.3f,0.3f,0.3f, 0.3f,0.3f,0.3f,0.3f, 0.3f,0.3f,0.3f,0.3f, 0.3f,0.3f,0.3f,0.3f,
      0.3f,0.3f,0.3f,0.3f, 0.3f,0.3f,0.3f,0.3f, 0.3f,0.3f,0.3f,0.3f, 0.3f,0.3f,0.3f,0.3f}},
};

/* ── Synth preset data ────────────────────────────────────────────────────── */
typedef struct {
    int   wave;
    float attack;    /* seconds */
    float decay;     /* seconds */
    float tone;      /* brightness 0–1 */
    float fm_ratio;  /* FM mod/car ratio */
    float fm_depth;  /* FM index */
    float sweep;     /* pitch arc 0–1 (presets 40-49 only; existing presets stay 0) */
} synth_preset_t;

/* 40 shared micro-synth presets + 10 kit synths = 50 total */
static const synth_preset_t SYN_PRESETS[NUM_SYN_PRESETS] = {
    /* 0-4: Ultra-short sine pips — Ikeda test tone. tone=1 bypasses LP, no filtering. */
    {WAVE_SINE, 0.0001f, 0.0002f, 1.0f, 0, 0, 0},  /* 0: 0.2ms — single pip */
    {WAVE_SINE, 0.0001f, 0.0005f, 1.0f, 0, 0, 0},  /* 1: 0.5ms */
    {WAVE_SINE, 0.0001f, 0.002f,  1.0f, 0, 0, 0},  /* 2: 2ms */
    {WAVE_SINE, 0.0001f, 0.007f,  1.0f, 0, 0, 0},  /* 3: 7ms */
    {WAVE_SINE, 0.0001f, 0.020f,  1.0f, 0, 0, 0},  /* 4: 20ms — audible pitch */
    /* 5-9: Time-domain tuning fork (Bernier solenoid strikes).
     * Voice decay=0.5s ensures waveform's own tau controls the sound completely. */
    {WAVE_DAMPED, 0.0001f, 0.5f, 0.05f, 0, 0, 0}, /* 5: tau~3ms  — sharp transient click */
    {WAVE_DAMPED, 0.0001f, 0.5f, 0.20f, 0, 0, 0}, /* 6: tau~8ms  */
    {WAVE_DAMPED, 0.0001f, 0.5f, 0.40f, 0, 0, 0}, /* 7: tau~25ms */
    {WAVE_DAMPED, 0.0001f, 0.5f, 0.65f, 0, 0, 0}, /* 8: tau~70ms */
    {WAVE_DAMPED, 0.0001f, 0.5f, 0.90f, 0, 0, 0}, /* 9: tau~200ms — long bell */
    /* 10-12: Filtered noise bursts */
    {WAVE_NOISE, 0.0001f, 0.001f,  0.8f, 0, 0, 0},
    {WAVE_NOISE, 0.0001f, 0.005f,  0.6f, 0, 0, 0},
    {WAVE_NOISE, 0.0001f, 0.018f,  0.4f, 0, 0, 0},
    /* 13-14: Percussive transients */
    {WAVE_CLICK,   0.0001f, 0.0003f, 1.0f, 0, 0, 0}, /* 13: crisp click */
    {WAVE_IMPULSE, 0.0001f, 0.001f,  1.0f, 0, 0, 0}, /* 14: broad impulse */
    /* 15-19: FM metallic/bell — ratios ≤ 3.0 to prevent alias sidebands */
    {WAVE_FM, 0.0001f, 0.006f, 0.7f, 1.0f, 0.5f, 0}, /* 15: gentle FM shimmer */
    {WAVE_FM, 0.0001f, 0.010f, 0.7f, 1.5f, 1.5f, 0}, /* 16: medium FM bell */
    {WAVE_FM, 0.0001f, 0.018f, 0.8f, 2.0f, 2.5f, 0}, /* 17: bright FM */
    {WAVE_FM, 0.0001f, 0.030f, 0.9f, 2.5f, 3.0f, 0}, /* 18: rich FM */
    {WAVE_FM, 0.0002f, 0.045f, 1.0f, 3.0f, 4.0f, 0}, /* 19: dense FM (ratio=3.0 max) */
    /* 20-22: PolyBLEP Square (alias-free) + Triangle */
    {WAVE_SQUARE, 0.0001f, 0.0005f, 1.0f, 0, 0, 0}, /* 20: digital blip */
    {WAVE_SQUARE, 0.0001f, 0.003f,  0.9f, 0, 0, 0}, /* 21: square burst */
    {WAVE_TRI,    0.0001f, 0.005f,  0.8f, 0, 0, 0}, /* 22: triangle burst */
    /* 23-25: Pink noise bursts (freq=LP cutoff) */
    {WAVE_PINK, 0.0001f, 0.003f,  0.9f, 0, 0, 0},
    {WAVE_PINK, 0.0001f, 0.012f,  0.6f, 0, 0, 0},
    {WAVE_PINK, 0.0002f, 0.035f,  0.3f, 0, 0, 0},
    /* 26-27: Brown noise — deep, warm */
    {WAVE_BROWN, 0.0001f, 0.007f,  0.8f, 0, 0, 0},
    {WAVE_BROWN, 0.0002f, 0.030f,  0.5f, 0, 0, 0},
    /* 28-32: AM textures (tone → AM rate 100–5000 Hz) */
    {WAVE_AM, 0.0001f, 0.006f,  0.05f, 0, 0, 0}, /* 28: ~350 Hz AM — woody */
    {WAVE_AM, 0.0001f, 0.008f,  0.20f, 0, 0, 0}, /* 29: ~1k Hz AM */
    {WAVE_AM, 0.0001f, 0.012f,  0.40f, 0, 0, 0}, /* 30: ~2k Hz AM */
    {WAVE_AM, 0.0002f, 0.020f,  0.65f, 0, 0, 0}, /* 31: ~3.3k Hz AM */
    {WAVE_AM, 0.0002f, 0.035f,  0.90f, 0, 0, 0}, /* 32: ~4.6k Hz AM — metallic */
    /* 33-36: Special */
    {WAVE_DAMPED, 0.0001f, 0.5f,   0.95f, 0, 0, 0},    /* 33: very long bell (tau~300ms) */
    {WAVE_FM,     0.0001f, 0.025f, 0.5f, 3.0f, 5.0f, 0}, /* 34: hard FM crunch */
    {WAVE_SINE,   0.0001f, 0.120f, 1.0f, 0, 0, 0},      /* 35: long pure sine */
    {WAVE_NOISE,  0.0001f, 0.070f, 0.3f, 0, 0, 0},      /* 36: long filtered noise */
    /* 37-39: Gaussian Ping (WAVE_PING) — Ikeda signature sound */
    {WAVE_CLICK, 0.0001f, 0.0001f, 1.0f, 0, 0, 0},      /* 37: sharpest click */
    {WAVE_PING,  0.0001f, 0.05f,   0.05f, 0, 0, 0},     /* 38: ping ultra-short (~0.5ms burst) */
    {WAVE_PING,  0.0001f, 0.05f,   0.30f, 0, 0, 0},     /* 39: ping medium (~4ms burst) */
    /* 40-49: KIT SYNTH PRESETS — for breakbeat voices. tone shapes character. */
    {WAVE_KICK,  0.0010f, 0.200f,  0.30f, 0, 0, 0.70f}, /* 40: Kick Synth   — sine + click + 200→60Hz sweep */
    {WAVE_SINE,  0.0020f, 0.400f,  0.10f, 0, 0, 0.30f}, /* 41: Kick Sub     — pure sub sine */
    {WAVE_SNARE, 0.0005f, 0.150f,  0.70f, 0, 0, 0},     /* 42: Snare Body   — noise + sine body */
    {WAVE_SNARE, 0.0003f, 0.080f,  0.85f, 0, 0, 0},     /* 43: Piccolo Snare— high-pitched, brighter */
    {WAVE_CLICK, 0.0001f, 0.030f,  0.90f, 0, 0, 0},     /* 44: Rim Click    — sharp transient */
    {WAVE_HAT,   0.0005f, 0.050f,  0.95f, 0, 0, 0},     /* 45: Closed Hat   — HP-filtered noise */
    {WAVE_HAT,   0.0010f, 0.200f,  0.80f, 0, 0, 0},     /* 46: Open Hat     — longer HP noise */
    {WAVE_DAMPED,0.0005f, 0.300f,  0.70f, 0, 0, 0},     /* 47: Ride Ping    — damped bell shimmer */
    {WAVE_KICK,  0.0010f, 0.250f,  0.40f, 0, 0, 0.50f}, /* 48: Tom Synth    — kick-like with mid pitch */
    {WAVE_NOISE, 0.0005f, 0.060f,  0.50f, 0, 0, 0},     /* 49: Ghost Snare  — soft filtered noise */
};

/* ── Scale quantization ───────────────────────────────────────────────────── */
/* Semitone intervals from root for each scale (0=root ... 11=major7th) */
static const int SCALE_INTERVALS[SCALE_COUNT][12] = {
    {0,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},  /* FREE  — unused */
    {0,1,2,3,4,5,6,7,8,9,10,11},           /* CHROMATIC */
    {0,2,4,5,7,9,11,-1,-1,-1,-1,-1},       /* MAJOR */
    {0,2,3,5,7,8,10,-1,-1,-1,-1,-1},       /* MINOR */
    {0,2,4,7,9,-1,-1,-1,-1,-1,-1,-1},      /* PENTATONIC */
    {0,2,4,6,8,10,-1,-1,-1,-1,-1,-1},      /* WHOLETONE */
    {0,2,3,5,6,8,9,11,-1,-1,-1,-1},        /* DIMINISHED */
    {0,2,3,5,7,8,11,-1,-1,-1,-1,-1}        /* HARM_MIN */
};
static const int SCALE_NOTES[SCALE_COUNT] = {0,12,7,7,5,6,8,7};

static float quantize_freq(float freq, int root, int scale) {
    if (root == ROOT_FREE || scale == SCALE_FREE) return freq;
    if (freq < 16.0f) return freq;

    /* MIDI note number (float) */
    float midi = 12.0f * log2f(freq / 440.0f) + 69.0f;
    int midi_i = (int)roundf(midi);

    /* root semitone: ROOT_C=1 → 0, ROOT_CS=2 → 1, ... */
    int root_st = (root - 1) % 12;

    /* Distance of current pitch from root, mod 12 */
    int dist = ((midi_i - root_st) % 12 + 12) % 12;

    /* Find nearest scale degree (with octave wrapping) */
    const int *ivals = SCALE_INTERVALS[scale];
    int n = SCALE_NOTES[scale];
    int best_diff = 13, best_delta = 0;
    for (int i = 0; i < n; i++) {
        if (ivals[i] < 0) break;
        int d = dist - ivals[i];
        /* wrap to -6..+6 */
        if (d >  6) d -= 12;
        if (d < -6) d += 12;
        int ad = d < 0 ? -d : d;
        if (ad < best_diff) {
            best_diff = ad;
            best_delta = ivals[i] - dist; /* how many semitones to shift */
        }
    }
    float q_midi = (float)(midi_i + best_delta);
    return 440.0f * powf(2.0f, (q_midi - 69.0f) / 12.0f);
}

/* ── Voice state ──────────────────────────────────────────────────────────── */
typedef struct {
    /* Sequencer */
    int    seq_preset;      /* 0=OFF, 1–25 */
    int    syn_preset;      /* 0=OFF, 1–25 */
    int    step;
    float  step_accum;      /* sample accumulator for free-running clock */
    float  tick_accum;      /* tick accumulator for MIDI clock */

    /* Envelope */
    float  env;
    int    env_stage;       /* 0=attack, 1=decay, -1=idle */

    /* Oscillator */
    float  phase;           /* 0–1 */
    float  fm_phase;        /* FM modulator phase */
    uint32_t noise_state;

    /* Tone LP filter state + 20ms smoothed tone coefficient */
    float  lp_state;
    float  tone_smooth;     /* 20ms-smoothed tone for click-free LP coeff */
    float  vel;             /* pad velocity scale 0–1 (1.0 for sequencer hits) */
    int    hit_sub_div;     /* row-based sub_div override for this hit (0 = use vp->sub_div) */
    float  damp_t;          /* time accumulator for WAVE_DAMPED, WAVE_PING, WAVE_TRAIN */

    /* Artist-inspired features */
    float  tone_rnd;        /* per-event tone randomization depth (Bretschneider) */
    float  tone_eff;        /* effective tone for current trigger (base + rnd offset) */
    float  sat;             /* sub-threshold saturation 0–1 (Emptyset harmonic onset) */
    float  res;             /* noise resonance 0–1: LP→BP filter (Alva Noto narrow-band) */
    float  noise_bp;        /* SVF bandpass state for resonant noise filter */
    int    sparsity_lockout; /* steps locked after trigger (Deupree sparsity gate) */

    /* Noise LP filter state (cutoff = freq, applied to Noise/Pink/Brown) */
    float  noise_lp;

    /* Heterodyne second oscillator */
    float  phase2;

    /* Hainbach features */
    float  sweep;           /* pitch arc depth 0–1 (peaks 2^sweep octaves up at env=1) */
    float  detune;          /* heterodyne Hz offset 0–20 Hz */
    int    sub_div;         /* sub-harmonic divider 1–8 */

    /* Colored noise state */
    float  pink_b[7];       /* Paul Kellett 7-stage pink noise filter */
    float  brown_last;      /* brown noise integrator */

    /* Per-voice params (pages 4–7 and Mix) */
    float  vol;
    float  freq;            /* Hz — shared by v_freq (Mix) and v_vfreq (Voice) */
    float  level;           /* Mix page level */
    int    wave;
    float  attack;
    float  decay;
    float  tone;
    float  pan;

    /* ── Breakbeat engine state (used when seq_preset >= NUM_PATTERN_PRESETS+1) ── */
    int      bb_step;            /* step index within current bar (0..step_grid-1) */
    int      bb_bar_index;       /* bar index for phrase tracking (incrementing) */
    int      bb_last_fired;      /* did previous step fire? (for Roll stickiness) */
    int      bb_skip_count;      /* 5% escape: skip this many steps before next trigger */
    float    bb_pitch_offset;    /* semitones offset for current trigger (snare bank) */
    int      bb_retrig_count;    /* pending retrigger bursts after current hit */
    float    bb_retrig_accum;    /* sample accumulator for retrigger spacing */
    float    bb_retrig_period;   /* period of retrigger in samples */
    float    bb_ghost_vel;       /* velocity for current trigger (ghost crescendo) */
    uint32_t bb_rng;             /* per-voice PRNG for breakbeat decisions */
    float    bb_mutated_curve[BB_STEP_GRID_MAX]; /* per-bar mutated curve (Improv) */
    int      bb_curve_bar_seed;  /* bar_index used to seed the current mutation */
    float    bb_recent_fires;    /* exponential moving average of recent triggers (Drummer Brain) */

    /* ── Holzman per-voice params (Voice page menu) ── */
    float    improv;             /* 0-1 — bar-to-bar curve mutation depth */
    float    push_pull;          /* -20..+20 ms directional micro-timing */
    int      bank_mode;          /* BANK_OFF / BANK_RULE / BANK_RANDOM */
    float    bank_pitch[5];      /* 5 semitone offsets (snare bank) */
    int      ghost_crescendo;    /* 0/1 — apply velocity ramp into accents */
} voice_t;

/* ── Scene snapshot ───────────────────────────────────────────────────────── */
/* Captures the full performance-relevant module state for A/B morphing.
 * Excludes: BPM, Tempo Sync, Master Vol, Bit Crush, Bit Rate, Stereo Width,
 * Drift, Jitter, Morph itself (these stay fixed across scene flips). */
typedef struct {
    /* Per-voice */
    int     v_seq_preset[4];
    int     v_syn_preset[4];
    float   v_vol[4];
    float   v_freq[4];
    float   v_level[4];
    int     v_wave[4];
    float   v_tone[4];
    float   v_decay[4];
    float   v_attack[4];
    float   v_detune[4];
    float   v_pan[4];
    int     v_sub_div[4];
    float   v_sweep[4];
    float   v_tone_rnd[4];
    /* Holzman per-voice */
    float   v_improv[4];
    float   v_push_pull[4];
    int     v_bank_mode[4];
    float   v_bank_pitch[4][5];
    int     v_ghost_crescendo[4];
    /* Global rhythm params */
    int     root, scale;
    float   density, chaos, gravity;
    int     clk_div;
    float   morse_spd, swing;
    /* Global breakbeat params */
    int     phrase_length;
    int     fill_shape;
    int     drummer_brain;
    int     step_grid;
    int     retrig_rate;
    /* Modulation */
    float   mod_amount, mod_speed, mod_offset;
    float   mod_freq, mod_decay, mod_pan, mod_density;
    int     mod_shape;
    /* All-decay */
    float   all_decay;
    /* Validity */
    int     populated;          /* 0 if never saved (treat as no-op on morph) */
} signal_scene_t;

/* ── Instance state ───────────────────────────────────────────────────────── */
typedef struct {
    voice_t voices[NUM_VOICES];

    /* Page 3 — Params */
    int    root;
    int    scale;
    float  density;
    float  chaos;
    float  gravity;
    int    clk_div;
    float  morse_spd;
    float  swing;

    /* Page 8 — General */
    float  master_vol;
    int    tempo_sync;      /* 0=Free, 1=Sync */
    float  stereo_w;
    int    bit_crush;
    float  bit_rate;        /* 0=off, 1=max decimation (sample-rate reduction) */
    float  drift;
    float  jitter;
    int    dc_filter;
    int    out_mode;

    /* Bit-rate decimation state */
    float  bit_rate_accum;
    float  bit_rate_hold_l;
    float  bit_rate_hold_r;

    /* Modulation LFO */
    float    all_decay;      /* 0–1 extends all voice decays toward max (0.5s) */
    float    mod_amount;     /* 0–1 global LFO depth multiplier */
    float    mod_speed;      /* 0–1 → 0–10 Hz */
    float    mod_offset;     /* 0–1 phase stagger between voices */
    float    mod_freq;       /* freq mod depth */
    float    mod_decay;      /* decay mod depth */
    float    mod_pan;        /* pan mod depth */
    float    mod_density;    /* density (trigger probability) mod depth */
    int      mod_shape;      /* 0=Sine 1=Tri 2=Saw 3=Square 4=S&H 5=Random */
    float    vlfo_phase[NUM_VOICES];
    float    vlfo_sh_val[NUM_VOICES];
    uint32_t vlfo_rng[NUM_VOICES];
    float    vlfo_value[NUM_VOICES];
    int      vlfo_shape[NUM_VOICES];

    /* Clock/transport */
    float    bpm;           /* host-provided or default */
    int      midi_running;  /* MIDI transport running */

    /* DC blocker state (per channel) */
    float  dc_x[2];

    /* PRNG for density/chaos/jitter */
    uint32_t rng;

    /* Sparsity gate: minimum step gap between triggers (Deupree) */
    float  sparsity;

    /* Current UI page (for knob overlay) */
    int    current_page;

    /* Current patch index */
    int    patch_idx;

    /* ── Breakbeat globals ── */
    int    phrase_length;       /* PHRASE_OFF / PHRASE_2..16 */
    int    fill_shape;          /* FILL_* */
    int    drummer_brain;       /* DRUMMER_BRAIN_* */
    int    step_grid;           /* STEP_GRID_* (0=8, 1=16, 2=32 steps per bar) */
    int    retrig_rate;         /* RETRIG_* */
    int    fill_shape_cached;   /* resolved fill shape for current phrase (handles Random) */
    int    fill_shape_bar;      /* bar_index that fill_shape_cached was picked for */

    /* ── Scene A/B + Morph ── */
    signal_scene_t scene_a;
    signal_scene_t scene_b;
    float          scene_morph;          /* knob value 0-1 */
    float          scene_morph_smoothed; /* one-pole filtered */
    float          last_applied_morph;   /* skip apply_scene_morph if unchanged */
    float          morph_smooth_ms;      /* smoothing time 0-200ms */
    int            morph_curve;          /* MORPH_LINEAR / EXP / STEPPED */
    int            morph_was_below_half; /* tracks 0.5 crossing for discrete snap */

    /* Momentary-action knobs: per-action countdown that holds get_param at "1"
     * for a few render blocks after fire, then reverts to "0".  Also acts as a
     * debounce so a single user gesture only fires the action once. */
    int            action_flash[NUM_ACTIONS];
} signal_instance_t;

/* ── Helpers ──────────────────────────────────────────────────────────────── */
static inline float clampf(float x, float lo, float hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

static inline uint32_t xorshift32(uint32_t *s) {
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x;
    return x;
}

static inline float randf(uint32_t *s) {
    return (float)(xorshift32(s) & 0xFFFF) / 65535.0f;
}

/* PolyBLEP residual — corrects discontinuities in naive waveforms */
static inline float polyblep(float t, float dt) {
    if (t < dt) {
        t /= dt;
        return t + t - t * t - 1.0f;
    } else if (t > 1.0f - dt) {
        t = (t - 1.0f) / dt;
        return t * t + t + t + 1.0f;
    }
    return 0.0f;
}

/* ── LFO shapes ───────────────────────────────────────────────────────────── */
#define MOD_SHAPE_SINE    0
#define MOD_SHAPE_TRI     1
#define MOD_SHAPE_SAW     2
#define MOD_SHAPE_SQUARE  3
#define MOD_SHAPE_SH      4
#define MOD_SHAPE_RANDOM  5
#define MOD_NUM_SHAPES    6
static const char *MOD_SHAPE_NAMES[] = {"Sine","Tri","Saw","Square","S&H","Random"};

static float mod_lfo_value(float phase, int shape,
                            float *sh_val, uint32_t *rng) {
    switch (shape) {
        case MOD_SHAPE_SINE:
            return sinf(phase * TWO_PI);
        case MOD_SHAPE_TRI:
            return (phase < 0.5f) ? (4.0f*phase - 1.0f) : (3.0f - 4.0f*phase);
        case MOD_SHAPE_SAW:
            return 2.0f * phase - 1.0f;
        case MOD_SHAPE_SQUARE:
            return (phase < 0.5f) ? 1.0f : -1.0f;
        case MOD_SHAPE_SH:
            return *sh_val; /* value sampled on zero-crossing, held until next */
        case MOD_SHAPE_RANDOM: {
            uint32_t x = *rng;
            x = x * 1664525u + 1013904223u;
            *rng = x;
            return (float)(int32_t)x / 2147483648.0f;
        }
        default: return 0.0f;
    }
}

/* ── Voice helpers ────────────────────────────────────────────────────────── */
static void voice_apply_synth_preset(voice_t *v, int preset_idx) {
    if (preset_idx < 0 || preset_idx >= NUM_SYN_PRESETS) return;
    const synth_preset_t *p = &SYN_PRESETS[preset_idx];
    v->wave   = p->wave;
    v->attack = p->attack;
    v->decay  = p->decay;
    v->tone   = p->tone;
    if (p->sweep > 0.0f) v->sweep = p->sweep; /* kit presets carry sweep; others preserve */
}

static void voice_trigger(voice_t *v) {
    v->env       = 0.0f;
    v->env_stage = 0;
    v->phase     = 0.0f;
    v->phase2    = 0.0f;
    v->fm_phase  = 0.0f;
    v->lp_state  = 0.0f;
    v->vel         = 1.0f;
    v->hit_sub_div = 0;
    v->damp_t      = 0.0f;
    v->tone_eff    = v->tone; /* reset to base (caller applies tone_rnd after if needed) */
    v->noise_bp    = 0.0f;   /* reset BP state on new hit for clean attack */
    v->bb_pitch_offset = 0.0f; /* default: no bank pitch shift */
    v->bb_ghost_vel    = 1.0f; /* default: full velocity (overridden by ghost crescendo) */
    /* brown_last kept — avoids click from hard reset */
}

static void voice_trigger_pad(voice_t *v, float velocity, int row_sub_div) {
    voice_trigger(v);
    v->vel         = velocity;
    v->hit_sub_div = row_sub_div; /* row-based octave override */
}

/* ── Breakbeat engine ─────────────────────────────────────────────────────── */
/* A voice runs in "breakbeat mode" whenever its seq_preset > NUM_PATTERN_PRESETS.
 * In that mode, the rhythm comes from a stochastic step-trigger algorithm using
 * an anchor weight curve (see BREAKBEAT_PRESETS) rather than the PATTERNS table.
 */

static inline int seq_is_breakbeat(int seq_preset) {
    return seq_preset > NUM_PATTERN_PRESETS && seq_preset <= NUM_SEQ_PRESETS;
}

static inline int seq_to_bb_index(int seq_preset) {
    return seq_preset - NUM_PATTERN_PRESETS - 1;  /* 0..NUM_BREAKBEAT_PRESETS-1 */
}

static int voice_role(const voice_t *v) {
    if (!seq_is_breakbeat(v->seq_preset)) return ROLE_ANY;
    int bb = seq_to_bb_index(v->seq_preset);
    if (bb < 0 || bb >= NUM_BREAKBEAT_PRESETS) return ROLE_ANY;
    return BREAKBEAT_PRESETS[bb].role;
}

/* Look up curve weight at step `s` for a given step_grid setting.
 * step_grid_value ∈ {8, 16, 32}; curve has BB_STEP_GRID_MAX (32) entries. */
static float bb_curve_at(const float *curve, int step, int step_grid_value) {
    int stride = BB_STEP_GRID_MAX / step_grid_value;
    int idx = (step * stride) & (BB_STEP_GRID_MAX - 1);
    return curve[idx];
}

/* Splitmix32 for seeded per-bar/per-voice deterministic mutations.
 * Mutations need to be reproducible at fixed transport position. */
static uint32_t splitmix32(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU;
    x ^= x >> 15; x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

/* Apply Improv mutations to the bar's mutated curve.
 * Called once per bar boundary, before the first step of the new bar is ticked.
 * Seeded by (voice_index, bar_index) so the same bar always produces the same mutations. */
static void bb_apply_improv(voice_t *vp, int voice_index, int bar_index,
                             const float *base_curve, float improv) {
    /* Start from base curve */
    for (int i = 0; i < BB_STEP_GRID_MAX; i++) vp->bb_mutated_curve[i] = base_curve[i];
    vp->bb_curve_bar_seed = bar_index;
    if (improv <= 0.0f) return;

    /* Seed RNG deterministically per (voice, bar) */
    uint32_t rng = splitmix32(0x9E3779B9u + (uint32_t)voice_index * 0x85EBCA77u
                              + (uint32_t)bar_index * 0xC2B2AE3Du);

    /* Number of mutations scales with improv: 0.0 → 0, 1.0 → 3 mutations */
    int mutations = (int)(improv * 3.0f + 0.5f);
    if (mutations < 1) mutations = 1;

    for (int m = 0; m < mutations; m++) {
        rng = splitmix32(rng + 0x1u);
        float r = (float)(rng & 0xFFFFFF) / (float)0xFFFFFF;  /* 0-1 */
        rng = splitmix32(rng + 0x2u);
        int slot = (int)((rng & 0x1F)); /* 0..31 */
        if (slot >= BB_STEP_GRID_MAX) slot = BB_STEP_GRID_MAX - 1;
        float w = vp->bb_mutated_curve[slot];

        if (r < 0.35f) {
            /* Add a ghost note at a zero-weight step */
            if (w < 0.05f) vp->bb_mutated_curve[slot] = 0.3f * improv;
        } else if (r < 0.60f) {
            /* Move one hit ±1 step */
            if (w > 0.2f) {
                rng = splitmix32(rng + 0x3u);
                int dir = (rng & 1) ? 1 : -1;
                int neighbor = (slot + dir + BB_STEP_GRID_MAX) % BB_STEP_GRID_MAX;
                vp->bb_mutated_curve[slot] = 0.0f;
                vp->bb_mutated_curve[neighbor] = w;
            }
        } else if (r < 0.80f) {
            /* Drop a non-strong hit */
            if (w > 0.05f && w < 0.8f) vp->bb_mutated_curve[slot] = 0.0f;
        } else if (r < 0.95f) {
            /* Subtle level perturbation */
            rng = splitmix32(rng + 0x4u);
            float delta = ((float)(rng & 0xFFFF) / 65535.0f - 0.5f) * 0.3f * improv;
            float nw = w + delta;
            if (nw < 0.0f) nw = 0.0f;
            if (nw > 1.0f) nw = 1.0f;
            vp->bb_mutated_curve[slot] = nw;
        } else {
            /* Strong-step accent (rare, only on already-high slots) */
            if (w > 0.8f) vp->bb_mutated_curve[slot] = 1.0f;
        }
    }
}

/* Resolve the active fill_shape (handles "Random" picking once per phrase).
 * Returns one of FILL_SNARE_ROLL..FILL_CRASH_DROP. */
static int bb_resolve_fill_shape(signal_instance_t *inst, int bar_index) {
    int shape = inst->fill_shape;
    if (shape == FILL_RANDOM) {
        if (inst->fill_shape_bar != bar_index) {
            inst->fill_shape_bar = bar_index;
            inst->fill_shape_cached = (int)(randf(&inst->rng) * 5.0f);
            if (inst->fill_shape_cached >= 5) inst->fill_shape_cached = 4;
        }
        return inst->fill_shape_cached;
    }
    return shape;
}

/* Fill shape overlay — return modified curve weight for this step.
 * Only invoked on the fill bar of a phrase. */
static float bb_fill_overlay_weight(int shape, int role,
                                     int step_in_bar, int step_grid_value,
                                     float base_weight) {
    /* "Last beat" of a bar = last quarter of the steps */
    int beat_steps = step_grid_value / 4;
    int last_beat_start = step_grid_value - beat_steps;

    switch (shape) {
        case FILL_SNARE_ROLL:
            if (role == ROLE_SNARE) {
                /* Snare voices fire on every step of the last beat */
                if (step_in_bar >= last_beat_start) return 1.0f;
            }
            return base_weight;
        case FILL_TOM_RUN:
            if (role == ROLE_SNARE || role == ROLE_PERC || role == ROLE_GHOST) {
                /* Sequential tom hits across the bar */
                if ((step_in_bar % (beat_steps / 2)) == 0) return 1.0f;
            }
            return base_weight;
        case FILL_KICK_FILL:
            if (role == ROLE_KICK) {
                /* Kick fires on every step of the last beat */
                if (step_in_bar >= last_beat_start) return 1.0f;
            }
            return base_weight;
        case FILL_HAT_SPLATTER:
            if (role == ROLE_HAT) {
                /* Hat retriggers on every step */
                return 1.0f;
            }
            /* Other voices drop out on the fill bar */
            return base_weight * 0.2f;
        case FILL_CRASH_DROP:
            /* All voices fire on step 0 of the fill bar, then silent */
            if (step_in_bar == 0) return (role == ROLE_KICK || role == ROLE_HAT) ? 1.0f : 0.0f;
            return 0.0f;
        default:
            return base_weight;
    }
}

/* Drummer Brain bias on base probability — cross-voice influence.
 * Inferred role mapping uses the active rhythm preset's role hint. */
static float bb_drummer_brain_bias(signal_instance_t *inst, int my_voice, int my_role,
                                    int is_fill_bar) {
    if (inst->drummer_brain == DRUMMER_BRAIN_OFF) return 0.0f;
    float amp = DRUMMER_BRAIN_AMP[inst->drummer_brain];
    float bias = 0.0f;

    for (int ov = 0; ov < NUM_VOICES; ov++) {
        if (ov == my_voice) continue;
        const voice_t *other = &inst->voices[ov];
        if (!seq_is_breakbeat(other->seq_preset)) continue;
        int other_role = voice_role(other);

        /* Snare fill activity → kick drops, hat boosts */
        if (other_role == ROLE_SNARE && is_fill_bar && other->bb_recent_fires > 0.4f) {
            if (my_role == ROLE_KICK) bias -= amp;
            else if (my_role == ROLE_HAT) bias += amp;
        }
        /* Kick just fired → ghost backs off */
        if (other_role == ROLE_KICK && other->bb_last_fired) {
            if (my_role == ROLE_GHOST) bias -= amp * 0.5f;
        }
        /* Hat doing 32nds (high recent_fires) → snare locks tighter */
        if (other_role == ROLE_HAT && other->bb_recent_fires > 0.6f
            && inst->step_grid == STEP_GRID_32) {
            if (my_role == ROLE_SNARE) bias += amp * 0.3f;
        }
    }
    return bias;
}

/* Look-ahead in the curve for the next "strong" step (weight >= 0.8) within `max_steps`.
 * Returns -1 if none found. */
static int bb_next_strong_step(const float *curve, int from_step, int step_grid_value,
                                int max_steps) {
    int stride = BB_STEP_GRID_MAX / step_grid_value;
    for (int d = 1; d <= max_steps; d++) {
        int s = (from_step + d) % step_grid_value;
        int idx = (s * stride) & (BB_STEP_GRID_MAX - 1);
        if (curve[idx] >= 0.8f) return d;
    }
    return -1;
}

/* Snare bank slot pick. Returns semitone offset for current trigger. */
static float bb_pick_bank_offset(voice_t *vp, float curve_weight) {
    if (vp->bank_mode == BANK_OFF) return 0.0f;
    int slot;
    if (vp->bank_mode == BANK_RULE) {
        if      (curve_weight >= 0.8f) slot = 0;
        else if (curve_weight >= 0.5f) slot = 1;
        else if (curve_weight >= 0.3f) slot = 2;
        else                            slot = ((xorshift32(&vp->bb_rng) & 1) ? 3 : 4);
    } else {
        slot = (int)((xorshift32(&vp->bb_rng) >> 8) % 5);
    }
    if (slot < 0) slot = 0;
    if (slot > 4) slot = 4;
    return vp->bank_pitch[slot];
}

/* Apply ghost crescendo: returns velocity (0..1) for current step.
 * If a strong step is approaching within 3 steps, ramp up. */
static float bb_ghost_velocity(voice_t *vp, const float *curve, int step,
                                int step_grid_value, float base_weight) {
    if (!vp->ghost_crescendo) return 1.0f;
    /* Only meaningful for ghost-position hits (weight < 0.5) */
    if (base_weight >= 0.5f) return 1.0f;
    int d = bb_next_strong_step(curve, step, step_grid_value, 3);
    if (d < 1) return 1.0f;
    /* d=1 → 0.9, d=2 → 0.7, d=3 → 0.5 — ramping crescendo */
    return 0.3f + (4 - d) * 0.2f;
}

/* Step decision: should this step trigger? Returns the curve weight used (>= 0 if trigger),
 * or -1 if no trigger. Also stores any per-trigger state on the voice. */
static int breakbeat_step_decide(signal_instance_t *inst, int v, int is_fill_bar,
                                  float *out_weight, float *out_ghost_vel) {
    voice_t *vp = &inst->voices[v];
    int bb_idx = seq_to_bb_index(vp->seq_preset);
    if (bb_idx < 0 || bb_idx >= NUM_BREAKBEAT_PRESETS) return 0;
    const breakbeat_preset_t *bp = &BREAKBEAT_PRESETS[bb_idx];

    int step_grid_value = STEP_GRID_VALUES[inst->step_grid];
    int step = vp->bb_step % step_grid_value;

    /* Decay recent_fires regardless of outcome */
    vp->bb_recent_fires *= 0.95f;

    /* Effective curve (mutated for Improv) */
    const float *curve = (vp->improv > 0.0f) ? vp->bb_mutated_curve : bp->curve;
    float w = bb_curve_at(curve, step, step_grid_value);

    /* Fill-shape override (only on fill bar) */
    if (is_fill_bar && inst->fill_shape >= FILL_SNARE_ROLL && inst->fill_shape < FILL_COUNT) {
        int resolved = bb_resolve_fill_shape(inst, vp->bb_bar_index);
        w = bb_fill_overlay_weight(resolved, bp->role, step, step_grid_value, w);
    }

    /* Phrase modulation: complexity↑, roll↓, anchor↓ on fill bar */
    float anchor_eff      = bp->anchor;
    float complexity_eff  = bp->complexity;
    float roll_eff        = bp->roll;
    if (is_fill_bar) {
        complexity_eff = clampf(complexity_eff + 0.3f, 0.0f, 1.0f);
        roll_eff       = clampf(roll_eff       - 0.3f, 0.0f, 1.0f);
        anchor_eff     = clampf(anchor_eff     - 0.2f, 0.0f, 1.0f);
    }

    /* Drummer Brain cross-voice bias */
    float drummer_bias = bb_drummer_brain_bias(inst, v, bp->role, is_fill_bar);

    /* Skip count: 5% escape hatch decrements down to 0 before resuming */
    if (vp->bb_skip_count > 0) {
        vp->bb_skip_count--;
        vp->bb_last_fired = 0;
        return 0;
    }

    *out_ghost_vel = bb_ghost_velocity(vp, curve, step, step_grid_value, w);
    *out_weight = w;

    /* Base probability — weight × anchor + (1-anchor) × 0.4 floor */
    float base_prob = w * anchor_eff + (1.0f - anchor_eff) * 0.4f * w;
    base_prob += drummer_bias;
    if (base_prob < 0.0f) base_prob = 0.0f;
    if (base_prob > 1.0f) base_prob = 1.0f;

    float r = randf(&vp->bb_rng);

    /* Roll (Stay) branch — only meaningful if previous step fired */
    if (r < roll_eff) {
        if (vp->bb_last_fired) {
            /* 5% escape hatch: skip 2-4 steps and trigger */
            if (randf(&vp->bb_rng) < 0.05f) {
                vp->bb_skip_count = 2 + (int)(randf(&vp->bb_rng) * 3.0f);
                vp->bb_last_fired = 1;
                vp->bb_recent_fires += 0.05f;
                return 1;
            }
            /* Repeat (no trigger) with probability (1 - w) */
            if (randf(&vp->bb_rng) < (1.0f - w)) {
                vp->bb_last_fired = 0;
                return 0;
            }
            /* ±1 walk — fire if neighbor weight is high */
            int dir = (xorshift32(&vp->bb_rng) & 1) ? 1 : -1;
            int neighbor_step = (step + dir + step_grid_value) % step_grid_value;
            float neighbor_w = bb_curve_at(curve, neighbor_step, step_grid_value);
            if (randf(&vp->bb_rng) < neighbor_w) {
                vp->bb_last_fired = 1;
                vp->bb_recent_fires += 0.05f;
                return 1;
            }
            vp->bb_last_fired = 0;
            return 0;
        }
        /* Previous step didn't fire, fall through to Move branch */
    }

    /* Move branch — independent decision */
    float p_swap = complexity_eff * w;
    if (randf(&vp->bb_rng) < p_swap) {
        vp->bb_last_fired = 1;
        vp->bb_recent_fires += 0.05f;
        return 1;
    }
    if (randf(&vp->bb_rng) < base_prob) {
        vp->bb_last_fired = 1;
        vp->bb_recent_fires += 0.05f;
        return 1;
    }
    vp->bb_last_fired = 0;
    return 0;
}

/* On every step boundary, advance bar/step counters and (if it's a new bar) re-mutate.
 * Returns the step_grid_value used so the caller can adjust step_accum threshold. */
static int breakbeat_advance(signal_instance_t *inst, int v, int *out_is_fill_bar) {
    voice_t *vp = &inst->voices[v];
    int bb_idx = seq_to_bb_index(vp->seq_preset);
    int step_grid_value = STEP_GRID_VALUES[inst->step_grid];

    /* Bar boundary: bb_step wrapped back to 0 */
    if (vp->bb_step == 0 && vp->bb_curve_bar_seed != vp->bb_bar_index) {
        if (bb_idx >= 0 && bb_idx < NUM_BREAKBEAT_PRESETS) {
            const breakbeat_preset_t *bp = &BREAKBEAT_PRESETS[bb_idx];
            bb_apply_improv(vp, v, vp->bb_bar_index, bp->curve, vp->improv);
        }
    }

    /* Fill-bar detection (only when phrase_length != Off) */
    int is_fill = 0;
    if (inst->phrase_length != PHRASE_OFF) {
        int phrase_bars = PHRASE_BARS[inst->phrase_length];
        if (phrase_bars > 0) {
            int phrase_pos = vp->bb_bar_index % phrase_bars;
            is_fill = (phrase_pos == phrase_bars - 1);
        }
    }
    if (out_is_fill_bar) *out_is_fill_bar = is_fill;
    return step_grid_value;
}

/* ── Scene A/B + Morph ────────────────────────────────────────────────────── */

/* Capture the live module state into a scene snapshot. */
static void capture_scene(signal_instance_t *inst, signal_scene_t *scene) {
    for (int v = 0; v < NUM_VOICES; v++) {
        const voice_t *vp = &inst->voices[v];
        scene->v_seq_preset[v] = vp->seq_preset;
        scene->v_syn_preset[v] = vp->syn_preset;
        scene->v_vol[v]        = vp->vol;
        scene->v_freq[v]       = vp->freq;
        scene->v_level[v]      = vp->level;
        scene->v_wave[v]       = vp->wave;
        scene->v_tone[v]       = vp->tone;
        scene->v_decay[v]      = vp->decay;
        scene->v_attack[v]     = vp->attack;
        scene->v_detune[v]     = vp->detune;
        scene->v_pan[v]        = vp->pan;
        scene->v_sub_div[v]    = vp->sub_div;
        scene->v_sweep[v]      = vp->sweep;
        scene->v_tone_rnd[v]   = vp->tone_rnd;
        scene->v_improv[v]     = vp->improv;
        scene->v_push_pull[v]  = vp->push_pull;
        scene->v_bank_mode[v]  = vp->bank_mode;
        for (int p = 0; p < 5; p++) scene->v_bank_pitch[v][p] = vp->bank_pitch[p];
        scene->v_ghost_crescendo[v] = vp->ghost_crescendo;
    }
    scene->root       = inst->root;
    scene->scale      = inst->scale;
    scene->density    = inst->density;
    scene->chaos      = inst->chaos;
    scene->gravity    = inst->gravity;
    scene->clk_div    = inst->clk_div;
    scene->morse_spd  = inst->morse_spd;
    scene->swing      = inst->swing;
    scene->phrase_length = inst->phrase_length;
    scene->fill_shape    = inst->fill_shape;
    scene->drummer_brain = inst->drummer_brain;
    scene->step_grid     = inst->step_grid;
    scene->retrig_rate   = inst->retrig_rate;
    scene->mod_amount  = inst->mod_amount;
    scene->mod_speed   = inst->mod_speed;
    scene->mod_offset  = inst->mod_offset;
    scene->mod_freq    = inst->mod_freq;
    scene->mod_decay   = inst->mod_decay;
    scene->mod_pan     = inst->mod_pan;
    scene->mod_density = inst->mod_density;
    scene->mod_shape   = inst->mod_shape;
    scene->all_decay   = inst->all_decay;
    scene->populated   = 1;
}

static inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }

/* Apply a (smoothed, curve-mapped) morph value 0..1 between scene_a and scene_b.
 * Continuous params lerp; discrete enums snap at 0.5 threshold. */
static void apply_scene_morph(signal_instance_t *inst, float morph) {
    const signal_scene_t *A = &inst->scene_a;
    const signal_scene_t *B = &inst->scene_b;
    if (!A->populated && !B->populated) return; /* nothing saved yet */
    if (!A->populated) { morph = 1.0f; }
    if (!B->populated) { morph = 0.0f; }

    /* Apply morph curve */
    float m = clampf(morph, 0.0f, 1.0f);
    switch (inst->morph_curve) {
        case MORPH_EXP: {
            /* Slow start/end, fast middle */
            m = (m < 0.5f) ? (2.0f * m * m) : (1.0f - 2.0f * (1.0f - m) * (1.0f - m));
            break;
        }
        case MORPH_STEPPED:
            m = (m < 0.5f) ? 0.0f : 1.0f;
            break;
        case MORPH_LINEAR:
        default:
            break;
    }

    const signal_scene_t *D = (m < 0.5f) ? A : B; /* discrete-snap source */

    for (int v = 0; v < NUM_VOICES; v++) {
        voice_t *vp = &inst->voices[v];
        /* Continuous */
        vp->vol     = lerpf(A->v_vol[v],     B->v_vol[v],     m);
        vp->freq    = lerpf(A->v_freq[v],    B->v_freq[v],    m);
        vp->level   = lerpf(A->v_level[v],   B->v_level[v],   m);
        vp->tone    = lerpf(A->v_tone[v],    B->v_tone[v],    m);
        vp->decay   = lerpf(A->v_decay[v],   B->v_decay[v],   m);
        vp->attack  = lerpf(A->v_attack[v],  B->v_attack[v],  m);
        vp->detune  = lerpf(A->v_detune[v],  B->v_detune[v],  m);
        vp->pan     = lerpf(A->v_pan[v],     B->v_pan[v],     m);
        vp->sweep   = lerpf(A->v_sweep[v],   B->v_sweep[v],   m);
        vp->tone_rnd= lerpf(A->v_tone_rnd[v],B->v_tone_rnd[v],m);
        vp->improv  = lerpf(A->v_improv[v],  B->v_improv[v],  m);
        vp->push_pull = lerpf(A->v_push_pull[v], B->v_push_pull[v], m);
        for (int p = 0; p < 5; p++)
            vp->bank_pitch[p] = lerpf(A->v_bank_pitch[v][p], B->v_bank_pitch[v][p], m);
        /* Discrete */
        if (vp->seq_preset != D->v_seq_preset[v]) {
            vp->seq_preset = D->v_seq_preset[v];
            vp->bb_step = 0;
            vp->bb_bar_index = 0;
            vp->bb_last_fired = 0;
            vp->bb_curve_bar_seed = -1;
        }
        if (vp->syn_preset != D->v_syn_preset[v]) {
            vp->syn_preset = D->v_syn_preset[v];
            if (D->v_syn_preset[v] > 0)
                voice_apply_synth_preset(vp, D->v_syn_preset[v] - 1);
        }
        vp->wave         = D->v_wave[v];
        vp->sub_div      = D->v_sub_div[v];
        vp->bank_mode    = D->v_bank_mode[v];
        vp->ghost_crescendo = D->v_ghost_crescendo[v];
    }

    /* Continuous globals */
    inst->density    = lerpf(A->density,    B->density,    m);
    inst->chaos      = lerpf(A->chaos,      B->chaos,      m);
    inst->gravity    = lerpf(A->gravity,    B->gravity,    m);
    inst->morse_spd  = lerpf(A->morse_spd,  B->morse_spd,  m);
    inst->swing      = lerpf(A->swing,      B->swing,      m);
    inst->mod_amount = lerpf(A->mod_amount, B->mod_amount, m);
    inst->mod_speed  = lerpf(A->mod_speed,  B->mod_speed,  m);
    inst->mod_offset = lerpf(A->mod_offset, B->mod_offset, m);
    inst->mod_freq   = lerpf(A->mod_freq,   B->mod_freq,   m);
    inst->mod_decay  = lerpf(A->mod_decay,  B->mod_decay,  m);
    inst->mod_pan    = lerpf(A->mod_pan,    B->mod_pan,    m);
    inst->mod_density= lerpf(A->mod_density,B->mod_density,m);
    inst->all_decay  = lerpf(A->all_decay,  B->all_decay,  m);

    /* Discrete globals */
    inst->root          = D->root;
    inst->scale         = D->scale;
    inst->clk_div       = D->clk_div;
    inst->phrase_length = D->phrase_length;
    inst->fill_shape    = D->fill_shape;
    inst->drummer_brain = D->drummer_brain;
    inst->step_grid     = D->step_grid;
    inst->retrig_rate   = D->retrig_rate;
    inst->mod_shape     = D->mod_shape;
}

static float voice_generate(voice_t *v, float freq) {
    float sample;
    float p_inc = freq * SR_INV;

    switch (v->wave) {
        case WAVE_SINE:
            sample = sinf(v->phase * TWO_PI);
            break;
        case WAVE_IMPULSE:
            sample = (v->phase < p_inc * 2.0f) ? 1.0f : 0.0f;
            break;
        case WAVE_NOISE: {
            uint32_t s = v->noise_state;
            s = s * 1664525u + 1013904223u;
            v->noise_state = s;
            sample = (float)(int32_t)s * (1.0f / 2147483648.0f);
            if (v->res > 0.0f) {
                /* Chamberlin SVF bandpass — Alva Noto narrow-band noise */
                float f = 2.0f * sinf(3.14159265f * freq * SR_INV);
                float q = 1.0f - v->res * 0.97f;
                float hp = sample - v->noise_lp - q * v->noise_bp;
                v->noise_bp += f * hp;
                v->noise_lp += f * v->noise_bp;
                sample = v->noise_bp * (1.5f + v->res);
            } else {
                float c = 1.0f - expf(-TWO_PI * freq * SR_INV);
                v->noise_lp += c * (sample - v->noise_lp);
                sample = v->noise_lp;
            }
            break;
        }
        case WAVE_DAMPED: {
            /* Time-domain tuning fork: solenoid-struck fork character (Bernier).
             * Decay is frequency-independent — same wall-clock ring at all pitches.
             * tone=0 → ~3ms tau (sharp click), tone=1 → ~360ms tau (long bell).
             * Presets keep voice decay=0.5s so waveform's own decay controls the sound. */
            float tau = 0.003f * powf(120.0f, v->tone_smooth);
            sample = sinf(v->phase * TWO_PI) * expf(-v->damp_t * SR_INV / tau);
            v->damp_t += 1.0f;
            break;
        }
        case WAVE_CLICK:
            sample = (v->phase < 0.005f) ?
                     (1.0f - v->phase / 0.005f) : 0.0f;
            break;
        case WAVE_SQUARE: {
            /* PolyBLEP anti-aliased square — eliminates aliasing noise (Ikeda-clean) */
            sample = (v->phase < 0.5f) ? 1.0f : -1.0f;
            sample += polyblep(v->phase, p_inc);
            sample -= polyblep(fmodf(v->phase + 0.5f, 1.0f), p_inc);
            break;
        }
        case WAVE_TRI:
            /* Triangle has 1/n² harmonic rolloff — aliasing already very low */
            sample = 4.0f * fabsf(v->phase - 0.5f) - 1.0f;
            break;
        case WAVE_PING: {
            /* Gaussian-windowed sine burst — Ikeda's signature pip.
             * One complete gaussian burst per trigger, independent of oscillator.
             * tone=0 → 0.5ms burst, tone=1 → 15ms burst.
             * Voice envelope should be set long (0.05s) so it doesn't cut the burst. */
            float burst_dur = 0.0005f + v->tone_smooth * 0.0145f;
            float sigma = burst_dur * 0.28f;
            float t_c   = burst_dur * 0.5f;
            float t     = v->damp_t * SR_INV;
            float gauss = expf(-(t - t_c) * (t - t_c) / (2.0f * sigma * sigma));
            sample = sinf(v->phase * TWO_PI) * gauss;
            v->damp_t += 1.0f;
            break;
        }
        case WAVE_FM: {
            const synth_preset_t *sp = &SYN_PRESETS[v->syn_preset > 0 ?
                                                     (v->syn_preset - 1) : 0];
            float mod_freq = freq * sp->fm_ratio;
            float mod = sinf(v->fm_phase * TWO_PI) * sp->fm_depth;
            sample = sinf((v->phase + mod) * TWO_PI);
            v->fm_phase += mod_freq * SR_INV;
            if (v->fm_phase >= 1.0f) v->fm_phase -= 1.0f;
            break;
        }
        case WAVE_PINK: {
            /* Paul Kellett 7-stage pink noise (MIT) */
            uint32_t s = v->noise_state;
            s = s * 1664525u + 1013904223u;
            v->noise_state = s;
            float w = (float)(int32_t)s / 2147483648.0f;
            v->pink_b[0] =  0.99886f * v->pink_b[0] + w * 0.0555179f;
            v->pink_b[1] =  0.99332f * v->pink_b[1] + w * 0.0750759f;
            v->pink_b[2] =  0.96900f * v->pink_b[2] + w * 0.1538520f;
            v->pink_b[3] =  0.86650f * v->pink_b[3] + w * 0.3104856f;
            v->pink_b[4] =  0.55000f * v->pink_b[4] + w * 0.5329522f;
            v->pink_b[5] = -0.76160f * v->pink_b[5] - w * 0.0168980f;
            sample = (v->pink_b[0] + v->pink_b[1] + v->pink_b[2] + v->pink_b[3]
                    + v->pink_b[4] + v->pink_b[5] + v->pink_b[6] + w * 0.5362f) * 0.11f;
            v->pink_b[6] = w * 0.115926f;
            if (v->res > 0.0f) {
                float f = 2.0f * sinf(3.14159265f * freq * SR_INV);
                float q = 1.0f - v->res * 0.97f;
                float hp = sample - v->noise_lp - q * v->noise_bp;
                v->noise_bp += f * hp;
                v->noise_lp += f * v->noise_bp;
                sample = v->noise_bp * (1.5f + v->res);
            } else {
                float c = 1.0f - expf(-TWO_PI * freq * SR_INV);
                v->noise_lp += c * (sample - v->noise_lp);
                sample = v->noise_lp;
            }
            break;
        }
        case WAVE_BROWN: {
            /* Brown noise: integrated white noise */
            uint32_t s = v->noise_state;
            s = s * 1664525u + 1013904223u;
            v->noise_state = s;
            float w = (float)(int32_t)s / 2147483648.0f * 0.02f;
            v->brown_last = clampf(v->brown_last + w, -1.0f, 1.0f);
            sample = v->brown_last;
            if (v->res > 0.0f) {
                float f = 2.0f * sinf(3.14159265f * freq * SR_INV);
                float q = 1.0f - v->res * 0.97f;
                float hp = sample - v->noise_lp - q * v->noise_bp;
                v->noise_bp += f * hp;
                v->noise_lp += f * v->noise_bp;
                sample = v->noise_bp * (1.5f + v->res);
            } else {
                float c = 1.0f - expf(-TWO_PI * freq * SR_INV);
                v->noise_lp += c * (sample - v->noise_lp);
                sample = v->noise_lp;
            }
            break;
        }
        case WAVE_AM: {
            /* AM carrier × modulator; tone_smooth → AM rate 100–5000 Hz */
            float am_rate = 100.0f + v->tone_smooth * 4900.0f;
            v->fm_phase += am_rate * SR_INV;
            if (v->fm_phase >= 1.0f) v->fm_phase -= 1.0f;
            float am_mod = 0.5f + 0.5f * sinf(v->fm_phase * TWO_PI); /* 0..1 */
            sample = sinf(v->phase * TWO_PI) * am_mod;
            break;
        }
        case WAVE_TRAIN: {
            /* Click-train-to-tone continuum (Messier sewing machine / Bretschneider).
             * At low freq (0.5-10 Hz): audible as spaced rhythmic transients.
             * At mid freq (40-80 Hz): sub-bass buzz.
             * At high freq (200-2000 Hz): pitched tone with chosen character.
             * tone=0 → very short pip (0.5ms), tone=1 → long pip (30ms).
             * Works through the voice sequencer OR as continuous self-oscillator. */
            if (v->phase < p_inc) v->damp_t = 0.0f; /* new cycle = new pip */
            float tau = 0.0005f + v->tone_smooth * 0.029f;
            sample = sinf(v->phase * TWO_PI) * expf(-v->damp_t * SR_INV / tau);
            v->damp_t += 1.0f;
            break;
        }
        case WAVE_KICK: {
            /* Kick = sine body + click transient + internal pitch envelope.
             * Pitch sweeps from ~3.5× freq at peak down to freq at silence,
             * stacked on top of any v->sweep already in effect.
             * tone mixes body vs click (high tone = more click). */
            float kick_freq = freq * (1.0f + v->env * 2.5f);
            p_inc = kick_freq * SR_INV;
            float body = sinf(v->phase * TWO_PI);
            float click = 0.0f;
            float t = v->damp_t * SR_INV;
            if (t < 0.002f) click = 1.0f - (t / 0.002f);
            sample = body * (1.0f - v->tone_smooth * 0.4f)
                   + click * (0.4f + v->tone_smooth * 0.6f);
            v->damp_t += 1.0f;
            break;
        }
        case WAVE_SNARE: {
            /* Snare = filtered noise + sine body. tone mixes noise/body. */
            uint32_t s = v->noise_state;
            s = s * 1664525u + 1013904223u;
            v->noise_state = s;
            float w = (float)(int32_t)s / 2147483648.0f;
            float body = sinf(v->phase * TWO_PI);
            float lp_cut = 1500.0f + v->tone_smooth * 6500.0f; /* 1.5-8 kHz */
            float c = 1.0f - expf(-TWO_PI * lp_cut * SR_INV);
            v->noise_lp += c * (w - v->noise_lp);
            sample = body * (1.0f - v->tone_smooth * 0.7f)
                   + v->noise_lp * (0.3f + v->tone_smooth * 0.7f);
            break;
        }
        case WAVE_HAT: {
            /* Hat = HP-filtered noise. tone controls HP cutoff (brightness). */
            uint32_t s = v->noise_state;
            s = s * 1664525u + 1013904223u;
            v->noise_state = s;
            float w = (float)(int32_t)s / 2147483648.0f;
            float hp_cut = 3000.0f + v->tone_smooth * 9000.0f; /* 3-12 kHz HP */
            float c = 1.0f - expf(-TWO_PI * hp_cut * SR_INV);
            v->noise_lp += c * (w - v->noise_lp);
            sample = (w - v->noise_lp) * 0.8f;
            break;
        }
        default:
            sample = 0.0f;
    }

    v->phase += p_inc;
    if (v->phase >= 1.0f) v->phase -= 1.0f;
    return sample;
}

/* ── Patch system ─────────────────────────────────────────────────────────── */
#define NUM_PATCHES 50

typedef struct {
    const char *name;
    int   seq[4];         /* rhythm preset 1-35 (0=off; 1-25 pattern, 26-35 breakbeat) */
    int   syn[4];         /* synth preset 1-50 (0=off; 1-40 micro, 41-50 kit) */
    float freq[4];        /* Hz */
    float pan[4];         /* -1..1 */
    float decay[4];       /* s */
    float sweep[4];       /* 0..1 */
    float detune[4];      /* Hz */
    int   sub_div[4];     /* 1..8 */
    float vol[4];         /* per-voice level 0-1, for equal loudness across patches */
    float density;
    float chaos;
    float gravity;
    float mod_speed;
    float mod_freq;
    float mod_density;
    int   mod_shape;
    /* v0.2 additions (zero-initialized by C99 in v0.1 patch initializers) */
    float improv[4];
    float push_pull[4];      /* ms */
    int   bank_mode[4];      /* 0=Off, 1=Rule, 2=Random */
    float bank_pitch[4][5];  /* semitone offsets */
    int   ghost_crescendo[4]; /* 0/1 */
    int   phrase_length;     /* 0=Off, 1=2, 2=4, 3=8, 4=16 */
    int   fill_shape;
    int   drummer_brain;
    int   step_grid;         /* 0=8, 1=16, 2=32 */
    int   retrig_rate;
} patch_def_t;

/* patch format: name, seq[4], syn[4], freq[4], pan[4], decay[4], sweep[4], detune[4],
 * sub_div[4], vol[4], density, chaos, gravity, mod_speed, mod_freq, mod_density, mod_shape */
static const patch_def_t PATCHES[NUM_PATCHES] = {
/* ORIGINAL 30 PATCHES — vol[4] calibrated for equal perceived loudness */
/*00 Init       */{"Init",      {1,1,1,1},{1,1,1,1},{440,880,1320,2200},{-0.6f,-0.2f,0.2f,0.6f},{0.005f,0.005f,0.005f,0.005f},{0,0,0,0},{0,0,0,0},{1,1,1,1},{0.75f,0.75f,0.75f,0.75f},1.0f,0.0f,0.0f,0.0f,0.0f,0.0f,MOD_SHAPE_SINE},
/*01 Ikeda Grid */{"Ikeda Grid",{11,12,13,14},{1,2,3,1},{1000,2000,4000,500},{-0.5f,0.5f,-0.3f,0.3f},{0.002f,0.001f,0.003f,0.001f},{0,0,0,0},{0,0,0,0},{1,1,1,1},{0.50f,0.50f,0.50f,0.50f},0.8f,0.1f,0.0f,0.0f,0.0f,0.0f,MOD_SHAPE_SINE},
/*02 Bernier    */{"Bernier",   {16,17,18,19},{7,7,8,6},{330,440,528,660},{-0.4f,0.4f,-0.2f,0.2f},{0.5f,0.5f,0.5f,0.5f},{0,0,0,0},{0,0,0,0},{1,1,1,1},{0.85f,0.85f,0.85f,0.85f},0.9f,0.05f,0.0f,0.0f,0.0f,0.0f,MOD_SHAPE_SINE},
/*03 Morse CQ   */{"Morse CQ",  {1,2,3,4},{1,3,5,2},{800,1000,600,1200},{-0.3f,0.3f,-0.5f,0.5f},{0.003f,0.003f,0.003f,0.003f},{0,0,0,0},{0,0,0,0},{1,1,1,1},{0.80f,0.80f,0.80f,0.80f},1.0f,0.0f,0.0f,0.0f,0.0f,0.0f,MOD_SHAPE_SINE},
/*04 Mathcore   */{"Mathcore",  {6,7,8,9},{14,14,13,13},{200,400,800,1600},{-0.7f,-0.2f,0.2f,0.7f},{0.0005f,0.0005f,0.0005f,0.0005f},{0,0,0,0},{0,0,0,0},{1,1,1,1},{0.65f,0.65f,0.65f,0.65f},0.9f,0.2f,0.0f,0.0f,0.0f,0.0f,MOD_SHAPE_SINE},
/*05 Pink Rain  */{"Pink Rain", {11,15,16,20},{23,24,25,23},{2000,3000,1500,4000},{-0.6f,0.6f,-0.4f,0.4f},{0.008f,0.005f,0.012f,0.006f},{0,0,0,0},{0,0,0,0},{1,1,1,1},{0.70f,0.70f,0.70f,0.70f},0.7f,0.2f,0.0f,0.15f,0.0f,0.3f,MOD_SHAPE_SINE},
/*06 Heterodyne */{"Heterodyne",{16,17,18,19},{4,4,4,4},{440,440,440,440},{-0.5f,-0.15f,0.15f,0.5f},{0.020f,0.025f,0.018f,0.022f},{0,0,0,0},{3.0f,5.0f,7.0f,11.0f},{1,1,1,1},{0.80f,0.80f,0.80f,0.80f},0.8f,0.0f,0.3f,0.0f,0.0f,0.0f,MOD_SHAPE_SINE},
/*07 Sub Harm   */{"Sub Harm",  {11,12,15,16},{4,4,3,3},{880,660,1320,440},{-0.4f,0.4f,-0.6f,0.6f},{0.020f,0.020f,0.007f,0.007f},{0,0,0,0},{0,0,0,0},{2,3,4,5},{0.80f,0.80f,0.80f,0.80f},0.8f,0.0f,0.0f,0.0f,0.0f,0.0f,MOD_SHAPE_SINE},
/*08 CA Automata*/{"CA Automata",{21,22,23,24},{1,21,22,14},{500,1000,750,2000},{-0.3f,0.3f,-0.5f,0.5f},{0.004f,0.003f,0.005f,0.002f},{0,0,0,0},{0,0,0,0},{1,1,1,1},{0.70f,0.70f,0.70f,0.70f},0.85f,0.15f,0.0f,0.0f,0.0f,0.0f,MOD_SHAPE_SINE},
/*09 FM Bell    */{"FM Bell",   {13,14,16,17},{16,17,18,19},{440,880,660,1100},{-0.5f,0.5f,-0.3f,0.3f},{0.025f,0.020f,0.030f,0.015f},{0,0,0,0},{0,0,0,0},{1,1,1,1},{0.70f,0.70f,0.70f,0.70f},0.7f,0.1f,0.0f,0.0f,0.0f,0.0f,MOD_SHAPE_SINE},
/*10 AM Texture */{"AM Texture",{11,16,20,21},{28,29,30,31},{1000,2000,500,3000},{-0.4f,0.4f,-0.7f,0.7f},{0.015f,0.012f,0.018f,0.010f},{0,0,0,0},{0,0,0,0},{1,1,1,1},{0.70f,0.70f,0.70f,0.70f},0.8f,0.1f,0.0f,0.2f,0.2f,0.0f,MOD_SHAPE_TRI},
/*11 Brown Pulse*/{"Brown Pulse",{6,8,10,12},{26,27,26,27},{200,150,300,100},{-0.3f,0.3f,-0.5f,0.5f},{0.060f,0.040f,0.080f,0.035f},{0,0,0,0},{0,0,0,0},{1,1,1,1},{0.80f,0.80f,0.80f,0.80f},0.85f,0.0f,0.0f,0.0f,0.0f,0.0f,MOD_SHAPE_SINE},
/*12 Clk Matrix */{"Clk Matrix",{10,11,12,13},{13,14,13,14},{1000,2000,4000,500},{-0.6f,-0.2f,0.2f,0.6f},{0.0005f,0.0005f,0.0005f,0.0005f},{0,0,0,0},{0,0,0,0},{1,1,1,1},{0.55f,0.55f,0.55f,0.55f},1.0f,0.3f,0.0f,0.0f,0.0f,0.0f,MOD_SHAPE_SINE},
/*13 Sweep Casc */{"Sweep Casc",{16,18,17,19},{4,3,2,1},{220,330,440,660},{-0.5f,0.5f,-0.3f,0.3f},{0.020f,0.020f,0.020f,0.020f},{0.8f,0.6f,0.5f,0.4f},{0,0,0,0},{1,1,1,1},{0.80f,0.80f,0.80f,0.80f},0.7f,0.05f,0.0f,0.0f,0.0f,0.0f,MOD_SHAPE_SINE},
/*14 Fibonacci  */{"Fibonacci", {16,17,18,19},{7,7,8,6},{528,396,660,792},{-0.3f,0.3f,-0.6f,0.6f},{0.5f,0.5f,0.5f,0.5f},{0,0,0,0},{0,0,0,0},{1,1,1,1},{0.85f,0.85f,0.85f,0.85f},0.9f,0.0f,0.0f,0.0f,0.0f,0.0f,MOD_SHAPE_SINE},
/*15 Digi Glitch*/{"Digi Glitch",{6,9,7,8},{20,21,22,20},{440,880,220,1760},{-0.5f,0.5f,-0.8f,0.8f},{0.003f,0.002f,0.004f,0.002f},{0,0,0,0},{0,0,0,0},{1,1,1,1},{0.60f,0.60f,0.60f,0.60f},0.8f,0.3f,0.0f,0.0f,0.0f,0.0f,MOD_SHAPE_SQUARE},
/*16 Chirp Field*/{"Chirp Field",{11,14,21,16},{35,35,35,35},{500,1000,750,2000},{-0.4f,0.4f,-0.6f,0.6f},{0.020f,0.015f,0.025f,0.012f},{0.5f,0.7f,0.3f,0.6f},{0,0,0,0},{1,1,1,1},{0.75f,0.75f,0.75f,0.75f},0.75f,0.1f,0.0f,0.15f,0.1f,0.0f,MOD_SHAPE_SINE},
/*17 Phase Cloud*/{"Phase Cloud",{16,17,18,19},{4,3,2,1},{220,330,440,550},{-0.6f,0.6f,-0.4f,0.4f},{0.020f,0.020f,0.020f,0.020f},{0.2f,0.15f,0.25f,0.1f},{1.0f,2.0f,0.5f,3.0f},{1,1,1,1},{0.80f,0.80f,0.80f,0.80f},0.8f,0.0f,0.5f,0.3f,0.4f,0.2f,MOD_SHAPE_SINE},
/*18 Test Signal*/{"Test Signal",{11,11,11,11},{14,14,14,14},{1000,2000,4000,500},{-0.8f,-0.3f,0.3f,0.8f},{0.001f,0.001f,0.001f,0.001f},{0,0,0,0},{0,0,0,0},{1,1,1,1},{0.55f,0.55f,0.55f,0.55f},1.0f,0.0f,0.0f,0.0f,0.0f,0.0f,MOD_SHAPE_SINE},
/*19 CW Radio   */{"CW Radio",  {1,2,5,3},{1,1,1,1},{700,700,700,700},{-0.5f,-0.5f,0.5f,0.5f},{0.005f,0.004f,0.006f,0.005f},{0,0,0,0},{3.0f,0.0f,5.0f,0.0f},{1,1,1,1},{0.75f,0.75f,0.75f,0.75f},1.0f,0.0f,0.0f,0.0f,0.0f,0.0f,MOD_SHAPE_SINE},
/*20 Cantor     */{"Cantor",    {23,24,21,22},{1,6,4,8},{880,440,1760,220},{-0.4f,0.4f,-0.2f,0.2f},{0.008f,0.020f,0.005f,0.030f},{0,0.3f,0,0.5f},{0,0,0,0},{1,1,2,1},{0.75f,0.75f,0.75f,0.75f},0.9f,0.1f,0.0f,0.0f,0.0f,0.0f,MOD_SHAPE_SINE},
/*21 Noise Gate */{"Noise Gate",{11,12,13,14},{10,11,14,12},{3000,5000,2000,8000},{-0.5f,0.5f,-0.3f,0.3f},{0.010f,0.005f,0.012f,0.004f},{0,0,0,0},{0,0,0,0},{1,1,1,1},{0.65f,0.65f,0.65f,0.65f},0.8f,0.2f,0.0f,0.0f,0.0f,0.0f,MOD_SHAPE_SINE},
/*22 Sub Bass   */{"Sub Bass",  {16,19,15,20},{26,27,5,6},{220,330,440,110},{0.0f,-0.2f,0.2f,0.0f},{0.100f,0.080f,0.060f,0.120f},{0,0,0,0},{0,0,0,0},{4,5,3,6},{0.80f,0.80f,0.80f,0.80f},0.7f,0.0f,0.0f,0.0f,0.0f,0.0f,MOD_SHAPE_SINE},
/*23 Thue-Morse */{"Thue-Morse",{20,20,21,21},{20,21,22,20},{440,550,660,330},{-0.3f,0.3f,-0.6f,0.6f},{0.004f,0.005f,0.003f,0.006f},{0,0,0,0},{0,0,0,0},{1,1,1,1},{0.65f,0.65f,0.65f,0.65f},0.85f,0.1f,0.0f,0.0f,0.0f,0.0f,MOD_SHAPE_SINE},
/*24 Pink Grid  */{"Pink Grid", {10,11,12,13},{23,24,25,23},{4000,6000,3000,8000},{-0.6f,-0.2f,0.2f,0.6f},{0.010f,0.008f,0.012f,0.006f},{0,0,0,0},{0,0,0,0},{1,1,1,1},{0.60f,0.60f,0.60f,0.60f},0.9f,0.2f,0.0f,0.1f,0.0f,0.2f,MOD_SHAPE_SINE},
/*25 Metallic FM*/{"Metallic FM",{6,7,8,9},{17,18,19,34},{300,600,900,1200},{-0.5f,0.5f,-0.3f,0.3f},{0.015f,0.012f,0.020f,0.010f},{0,0,0,0},{0,0,0,0},{1,1,1,1},{0.70f,0.70f,0.70f,0.70f},0.75f,0.15f,0.0f,0.0f,0.0f,0.0f,MOD_SHAPE_SINE},
/*26 Minimal    */{"Minimal",   {19,0,0,0},{4,0,0,0},{440,440,440,440},{0,0,0,0},{0.020f,0.005f,0.005f,0.005f},{0,0,0,0},{0,0,0,0},{1,1,1,1},{0.90f,0.90f,0.90f,0.90f},0.8f,0.0f,0.0f,0.0f,0.0f,0.0f,MOD_SHAPE_SINE},
/*27 Maximum    */{"Maximum",   {10,11,6,7},{2,11,18,26},{220,440,880,1760},{-0.7f,-0.2f,0.2f,0.7f},{0.002f,0.010f,0.030f,0.015f},{0.3f,0.0f,0.5f,0.0f},{0,2.0f,0,5.0f},{1,1,1,2},{0.50f,0.50f,0.50f,0.50f},0.9f,0.2f,0.3f,0.2f,0.3f,0.2f,MOD_SHAPE_TRI},
/*28 Rule 30    */{"Rule 30",   {21,22,23,24},{1,7,19,10},{660,440,880,330},{-0.4f,0.4f,-0.2f,0.2f},{0.005f,0.5f,0.030f,0.010f},{0,0.3f,0,0.5f},{0,0,0,0},{1,1,1,1},{0.75f,0.75f,0.75f,0.75f},0.85f,0.05f,0.0f,0.0f,0.0f,0.0f,MOD_SHAPE_SINE},
/*29 Freq Lat.  */{"Freq Lat.", {11,13,15,17},{1,1,1,1},{200,400,600,800},{-0.6f,-0.2f,0.2f,0.6f},{0.007f,0.007f,0.007f,0.007f},{0,0,0,0},{0,0,0,0},{1,1,1,1},{0.65f,0.65f,0.65f,0.65f},0.95f,0.0f,0.0f,0.0f,0.0f,0.0f,MOD_SHAPE_SINE},

/* NEW 10 PATCHES — covering WAVE_PING, time-domain DAMPED, PolyBLEP Square, Bernier close */
/*30 Gauss Pings */{"Gauss Pings",{20,21,16,23},{39,38,39,38},{440,880,660,1320},{-0.6f,-0.2f,0.2f,0.6f},{0.05f,0.05f,0.05f,0.05f},{0,0,0,0},{0,0,0,0},{1,1,1,1},{0.95f,0.95f,0.95f,0.95f},0.9f,0.05f,0.0f,0.0f,0.0f,0.0f,MOD_SHAPE_SINE},
/*31 Ikeda Data  */{"Ikeda Data",{10,11,13,14},{1,1,2,1},{1000,2000,4000,500},{-0.7f,-0.2f,0.2f,0.7f},{0.001f,0.001f,0.002f,0.001f},{0,0,0,0},{0,0,0,0},{1,1,1,1},{0.50f,0.50f,0.50f,0.50f},0.85f,0.1f,0.0f,0.0f,0.0f,0.0f,MOD_SHAPE_SINE},
/*32 Tuning Forks*/{"Tuning Fork",{16,17,19,18},{7,7,8,6},{528,396,660,792},{-0.4f,0.4f,-0.2f,0.2f},{0.5f,0.5f,0.5f,0.5f},{0,0,0,0},{0,0,0,0},{1,1,1,1},{0.85f,0.85f,0.85f,0.85f},0.9f,0.0f,0.0f,0.0f,0.0f,0.0f,MOD_SHAPE_SINE},
/*33 Fork Harmony*/{"Fork Harmony",{15,16,17,19},{8,8,9,7},{440,550,660,880},{-0.5f,0.5f,-0.3f,0.3f},{0.5f,0.5f,0.5f,0.5f},{0,0,0,0},{2.0f,0.0f,4.0f,0.0f},{1,1,1,1},{0.80f,0.80f,0.80f,0.80f},0.85f,0.0f,0.0f,0.1f,0.2f,0.0f,MOD_SHAPE_SINE},
/*34 BL Square   */{"BL Square", {6,7,9,8},{20,21,20,22},{440,880,220,1760},{-0.6f,-0.1f,0.1f,0.6f},{0.002f,0.003f,0.002f,0.004f},{0,0,0,0},{0,0,0,0},{1,1,1,1},{0.60f,0.60f,0.60f,0.60f},0.8f,0.3f,0.0f,0.0f,0.0f,0.0f,MOD_SHAPE_SQUARE},
/*35 Data Quanta */{"Data Quanta",{21,23,24,22},{1,39,2,38},{2000,1000,4000,500},{-0.5f,0.5f,-0.7f,0.7f},{0.001f,0.05f,0.002f,0.05f},{0,0,0,0},{0,0,0,0},{1,1,1,1},{0.80f,0.80f,0.80f,0.80f},0.85f,0.05f,0.0f,0.0f,0.0f,0.0f,MOD_SHAPE_SINE},
/*36 Resonant Bel*/{"Resonant Bell",{15,16,20,19},{9,9,33,8},{330,440,528,660},{-0.4f,0.4f,-0.6f,0.6f},{0.5f,0.5f,0.5f,0.5f},{0,0,0,0},{0,0,0,0},{1,1,1,1},{0.75f,0.75f,0.75f,0.75f},0.8f,0.0f,0.0f,0.05f,0.1f,0.0f,MOD_SHAPE_SINE},
/*37 Sine Study  */{"Sine Study", {22,23,20,21},{2,3,4,5},{440,660,880,1100},{-0.4f,0.4f,-0.6f,0.6f},{0.002f,0.007f,0.020f,0.050f},{0,0,0,0},{0,0,0,0},{1,1,1,1},{0.75f,0.75f,0.75f,0.75f},0.9f,0.0f,0.0f,0.0f,0.0f,0.0f,MOD_SHAPE_SINE},
/*38 Noise Field */{"Noise Field",{11,14,16,21},{23,27,24,26},{3000,500,6000,200},{-0.5f,0.5f,-0.3f,0.3f},{0.008f,0.025f,0.005f,0.035f},{0,0,0,0},{0,0,0,0},{1,1,1,1},{0.70f,0.70f,0.70f,0.70f},0.75f,0.1f,0.0f,0.0f,0.0f,0.0f,MOD_SHAPE_SINE},
/*39 Aleatoric   */{"Aleatoric", {10,16,23,5},{1,7,19,32},{880,440,1320,220},{-0.6f,0.6f,-0.4f,0.4f},{0.001f,0.5f,0.030f,0.020f},{0,0,0,0},{0,0,0,0},{1,1,1,1},{0.70f,0.70f,0.70f,0.70f},0.8f,0.1f,0.0f,0.1f,0.1f,0.1f,MOD_SHAPE_SINE},

/* ── BREAKBEAT PATCHES (40-49) — v0.2 ─────────────────────────────────────── */
/* Field order:
 *   name, seq[4], syn[4], freq[4], pan[4], decay[4], sweep[4], detune[4],
 *   sub_div[4], vol[4],
 *   density, chaos, gravity, mod_speed, mod_freq, mod_density, mod_shape,
 *   improv[4], push_pull[4], bank_mode[4], bank_pitch[4][5], ghost_crescendo[4],
 *   phrase_length, fill_shape, drummer_brain, step_grid, retrig_rate
 * Breakbeat seq values: 26=Amen Kick, 27=Amen Snare, 28=Amen Hat, 29=Amen Ghost,
 *   30=Think Snare, 31=Funky Kick, 32=Jungle Roll, 33=Liquid Snare,
 *   34=Holzman Improv, 35=Breakbeat Rnd
 * Kit syn values: 41=Kick Synth, 42=Kick Sub, 43=Snare Body, 44=Piccolo Snare,
 *   45=Rim Click, 46=Closed Hat, 47=Open Hat, 48=Ride Ping, 49=Tom Synth, 50=Ghost Snare */

/*40 Amen        */{"Amen",        {26,27,28,29},{41,43,46,50},{60,200,8000,250},{-0.1f,0.1f,0.5f,-0.3f},{0.200f,0.150f,0.050f,0.060f},{0.7f,0,0,0},{0,0,0,0},{1,1,1,1},{0.85f,0.80f,0.65f,0.55f},1.0f,0.0f,0.0f,0.0f,0.0f,0.0f,MOD_SHAPE_SINE,
 /*improv*/{0,0,0,0},/*push_pull*/{0,0,0,0},/*bank_mode*/{0,0,0,0},/*bank_pitch*/{{0,5,7,-3,12},{0,5,7,-3,12},{0,5,7,-3,12},{0,5,7,-3,12}},/*ghost*/{0,0,0,1},
 /*phrase*/3,/*fill*/5,/*brain*/0,/*step_grid*/2,/*retrig*/0},

/*41 Think       */{"Think",       {26,30,28,29},{41,43,46,50},{65,220,7500,280},{-0.1f,0.15f,0.5f,-0.3f},{0.200f,0.150f,0.050f,0.060f},{0.7f,0,0,0},{0,0,0,0},{1,1,1,1},{0.85f,0.80f,0.65f,0.55f},1.0f,0.0f,0.0f,0.0f,0.0f,0.0f,MOD_SHAPE_SINE,
 /*improv*/{0,0.2f,0,0.1f},/*push_pull*/{0,0,0,0},/*bank_mode*/{0,0,0,0},/*bank_pitch*/{{0,5,7,-3,12},{0,5,7,-3,12},{0,5,7,-3,12},{0,5,7,-3,12}},/*ghost*/{0,1,0,1},
 /*phrase*/3,/*fill*/5,/*brain*/1,/*step_grid*/2,/*retrig*/0},

/*42 Funky Drum  */{"Funky Drummer",{31,27,28,29},{41,43,46,50},{55,190,8500,260},{-0.1f,0.1f,0.5f,-0.3f},{0.200f,0.150f,0.050f,0.060f},{0.7f,0,0,0},{0,0,0,0},{1,1,1,1},{0.85f,0.80f,0.65f,0.55f},1.0f,0.0f,0.0f,0.0f,0.0f,0.0f,MOD_SHAPE_SINE,
 /*improv*/{0,0,0,0},/*push_pull*/{0,0,0,0},/*bank_mode*/{0,0,0,0},/*bank_pitch*/{{0,5,7,-3,12},{0,5,7,-3,12},{0,5,7,-3,12},{0,5,7,-3,12}},/*ghost*/{0,0,0,1},
 /*phrase*/2,/*fill*/5,/*brain*/1,/*step_grid*/2,/*retrig*/0},

/*43 Piccolo Jng */{"Piccolo Jungle",{26,33,28,29},{41,44,46,50},{50,450,9000,380},{-0.1f,0.2f,0.5f,-0.3f},{0.180f,0.080f,0.050f,0.060f},{0.7f,0,0,0},{0,3.0f,0,0},{1,1,1,1},{0.85f,0.78f,0.65f,0.55f},1.0f,0.0f,0.0f,0.0f,0.0f,0.0f,MOD_SHAPE_SINE,
 /*improv*/{0,0.3f,0,0.2f},/*push_pull*/{0,0,0,0},/*bank_mode*/{0,1,0,0},/*bank_pitch*/{{0,5,7,-3,12},{0,5,7,-3,12},{0,5,7,-3,12},{0,5,7,-3,12}},/*ghost*/{0,1,0,1},
 /*phrase*/3,/*fill*/5,/*brain*/1,/*step_grid*/2,/*retrig*/0},

/*44 Liquid DnB  */{"Liquid DnB",   {26,33,28,32},{41,43,47,48},{55,250,6500,2200},{-0.1f,0.1f,0.5f,0.2f},{0.200f,0.150f,0.200f,0.300f},{0.7f,0,0,0},{0,0,0,0},{1,1,1,1},{0.85f,0.78f,0.55f,0.50f},1.0f,0.0f,0.0f,0.0f,0.0f,0.0f,MOD_SHAPE_SINE,
 /*improv*/{0,0.2f,0,0.2f},/*push_pull*/{0,4.0f,0,0},/*bank_mode*/{0,0,0,0},/*bank_pitch*/{{0,5,7,-3,12},{0,5,7,-3,12},{0,5,7,-3,12},{0,5,7,-3,12}},/*ghost*/{0,1,0,0},
 /*phrase*/3,/*fill*/0,/*brain*/1,/*step_grid*/2,/*retrig*/1},

/*45 Holzman Live*/{"Holzman Live", {26,34,28,34},{41,44,46,50},{60,200,8000,260},{-0.1f,0.1f,0.5f,-0.3f},{0.200f,0.150f,0.050f,0.060f},{0.7f,0,0,0},{0,3.0f,0,0},{1,1,1,1},{0.85f,0.78f,0.65f,0.55f},1.0f,0.0f,0.0f,0.0f,0.0f,0.0f,MOD_SHAPE_SINE,
 /*improv*/{0.2f,0.7f,0.3f,0.6f},/*push_pull*/{0,3.0f,2.0f,0},/*bank_mode*/{0,1,0,2},/*bank_pitch*/{{0,5,7,-3,12},{0,5,7,-3,12},{0,5,7,-3,12},{0,5,7,-3,12}},/*ghost*/{0,1,0,1},
 /*phrase*/3,/*fill*/5,/*brain*/2,/*step_grid*/2,/*retrig*/1},

/*46 Microsound  */{"Microsound Jungle",{26,27,28,29},{2,3,15,8},{60,200,8000,300},{-0.5f,0.5f,-0.3f,0.3f},{0.005f,0.005f,0.005f,0.005f},{0,0,0,0},{0,0,0,0},{1,1,1,1},{0.80f,0.80f,0.75f,0.70f},1.0f,0.0f,0.0f,0.0f,0.0f,0.0f,MOD_SHAPE_SINE,
 /*improv*/{0,0,0,0},/*push_pull*/{0,0,0,0},/*bank_mode*/{0,0,0,0},/*bank_pitch*/{{0,5,7,-3,12},{0,5,7,-3,12},{0,5,7,-3,12},{0,5,7,-3,12}},/*ghost*/{0,0,0,0},
 /*phrase*/3,/*fill*/5,/*brain*/0,/*step_grid*/2,/*retrig*/0},

/*47 Bernier Jng */{"Bernier Jungle",{26,27,28,29},{7,8,9,10},{80,250,1500,600},{-0.4f,0.4f,-0.2f,0.2f},{0.5f,0.5f,0.5f,0.5f},{0,0,0,0},{0,0,0,0},{1,1,1,1},{0.85f,0.85f,0.80f,0.80f},1.0f,0.0f,0.0f,0.0f,0.0f,0.0f,MOD_SHAPE_SINE,
 /*improv*/{0,0,0,0},/*push_pull*/{0,0,0,0},/*bank_mode*/{0,0,0,0},/*bank_pitch*/{{0,5,7,-3,12},{0,5,7,-3,12},{0,5,7,-3,12},{0,5,7,-3,12}},/*ghost*/{0,0,0,1},
 /*phrase*/3,/*fill*/5,/*brain*/0,/*step_grid*/2,/*retrig*/0},

/*48 Mathcore Brk*/{"Mathcore Break",{8,27,9,29},{41,43,46,50},{60,200,8000,280},{-0.1f,0.1f,0.5f,-0.3f},{0.200f,0.150f,0.050f,0.060f},{0.7f,0,0,0},{0,0,0,0},{1,1,1,1},{0.85f,0.80f,0.65f,0.55f},1.0f,0.0f,0.0f,0.0f,0.0f,0.0f,MOD_SHAPE_SINE,
 /*improv*/{0,0.2f,0,0.3f},/*push_pull*/{0,0,0,0},/*bank_mode*/{0,0,0,0},/*bank_pitch*/{{0,5,7,-3,12},{0,5,7,-3,12},{0,5,7,-3,12},{0,5,7,-3,12}},/*ghost*/{0,0,0,1},
 /*phrase*/3,/*fill*/5,/*brain*/1,/*step_grid*/2,/*retrig*/0},

/*49 Bretsch Rol */{"Bretschneider Roll",{26,27,28,32},{18,19,17,18},{65,210,7500,320},{-0.5f,0.5f,-0.3f,0.3f},{0.030f,0.025f,0.020f,0.030f},{0,0,0,0},{0,0,0,0},{1,1,1,1},{0.75f,0.75f,0.70f,0.65f},1.0f,0.0f,0.0f,0.0f,0.0f,0.0f,MOD_SHAPE_SINE,
 /*improv*/{0.3f,0.3f,0.3f,0.5f},/*push_pull*/{0,0,0,0},/*bank_mode*/{0,0,0,0},/*bank_pitch*/{{0,5,7,-3,12},{0,5,7,-3,12},{0,5,7,-3,12},{0,5,7,-3,12}},/*ghost*/{0,0,0,1},
 /*phrase*/3,/*fill*/5,/*brain*/1,/*step_grid*/2,/*retrig*/1},
};

static const char *PATCH_NAMES[NUM_PATCHES] = {
    "Init","Ikeda Grid","Bernier","Morse CQ","Mathcore",
    "Pink Rain","Heterodyne","Sub Harm","CA Automata","FM Bell",
    "AM Texture","Brown Pulse","Clk Matrix","Sweep Casc","Fibonacci",
    "Digi Glitch","Chirp Field","Phase Cloud","Test Signal","CW Radio",
    "Cantor","Noise Gate","Sub Bass","Thue-Morse","Pink Grid",
    "Metallic FM","Minimal","Maximum","Rule 30","Freq Lat.",
    "Gauss Pings","Ikeda Data","Tuning Fork","Fork Harmony","BL Square",
    "Data Quanta","Resonant Bell","Sine Study","Noise Field","Aleatoric",
    "Amen","Think","Funky Drummer","Piccolo Jungle","Liquid DnB",
    "Holzman Live","Microsound Jungle","Bernier Jungle","Mathcore Break","Bretschneider Roll"
};

static void apply_patch(signal_instance_t *inst, int idx) {
    if (idx < 0 || idx >= NUM_PATCHES) return;
    const patch_def_t *p = &PATCHES[idx];
    inst->patch_idx = idx;
    for (int v = 0; v < NUM_VOICES; v++) {
        voice_t *vp = &inst->voices[v];
        vp->seq_preset = p->seq[v];
        vp->syn_preset = p->syn[v];
        if (p->syn[v] > 0) voice_apply_synth_preset(vp, p->syn[v] - 1);
        vp->freq    = p->freq[v];
        vp->pan     = p->pan[v];
        vp->decay   = p->decay[v];
        if (p->sweep[v] > 0.0f) vp->sweep = p->sweep[v];
        vp->detune  = p->detune[v];
        vp->sub_div = p->sub_div[v];
        vp->vol     = p->vol[v] > 0.0f ? p->vol[v] : 0.8f;
        /* v0.2 Holzman per-voice */
        vp->improv          = p->improv[v];
        vp->push_pull       = p->push_pull[v];
        vp->bank_mode       = p->bank_mode[v];
        for (int b = 0; b < 5; b++) vp->bank_pitch[b] = p->bank_pitch[v][b];
        vp->ghost_crescendo = p->ghost_crescendo[v];
        /* Reset breakbeat state on patch load so the new preset starts clean */
        vp->bb_step = 0;
        vp->bb_bar_index = 0;
        vp->bb_last_fired = 0;
        vp->bb_skip_count = 0;
        vp->bb_curve_bar_seed = -1;
        vp->bb_retrig_count = 0;
    }
    inst->density     = p->density;
    inst->chaos       = p->chaos;
    inst->gravity     = p->gravity;
    inst->mod_speed   = p->mod_speed;
    inst->mod_freq    = p->mod_freq;
    inst->mod_density = p->mod_density;
    inst->mod_shape   = p->mod_shape;
    for (int v = 0; v < NUM_VOICES; v++) inst->vlfo_shape[v] = p->mod_shape;
    /* v0.2 breakbeat globals */
    inst->phrase_length = p->phrase_length;
    inst->fill_shape    = p->fill_shape;
    inst->drummer_brain = p->drummer_brain;
    inst->step_grid     = p->step_grid;
    inst->retrig_rate   = p->retrig_rate;
}

static float rand_scale_freq(signal_instance_t *inst) {
    /* Pick a random MIDI note in musical range and quantize to current scale */
    int midi = 36 + (int)(randf(&inst->rng) * 48.99f); /* C2–C6, capped to 48 */
    if (midi < 21) midi = 21;
    if (midi > 108) midi = 108;
    float freq = 440.0f * powf(2.0f, (float)(midi - 69) / 12.0f);
    freq = quantize_freq(freq, inst->root, inst->scale);
    /* NaN/Inf guard */
    if (freq < 20.0f || freq > 20000.0f || freq != freq) freq = 440.0f;
    return freq;
}

/* Safe bounded random integer: returns 0..count-1 */
static inline int rnd_int(uint32_t *rng, int count) {
    int v = (int)(randf(rng) * (float)count);
    if (v < 0) v = 0;
    if (v >= count) v = count - 1;
    return v;
}

/* All helpers defined before do_rnd_patch which calls them */
static void do_rnd_rytm(signal_instance_t *inst) {
    for (int v = 0; v < NUM_VOICES; v++)
        inst->voices[v].seq_preset = 1 + rnd_int(&inst->rng, NUM_SEQ_PRESETS);
}

static void do_rnd_voices(signal_instance_t *inst) {
    for (int v = 0; v < NUM_VOICES; v++) {
        voice_t *vp = &inst->voices[v];
        int syn = 1 + rnd_int(&inst->rng, NUM_SYN_PRESETS);
        vp->syn_preset = syn;
        voice_apply_synth_preset(vp, syn - 1);
        vp->freq  = 100.0f * powf(80.0f, randf(&inst->rng));
        vp->decay = 0.001f + randf(&inst->rng) * 0.149f;
        vp->tone  = randf(&inst->rng);
        vp->pan   = clampf((randf(&inst->rng) - 0.5f) * 2.0f, -1.0f, 1.0f);
    }
}

static void do_same_voice(signal_instance_t *inst) {
    int syn = 1 + rnd_int(&inst->rng, NUM_SYN_PRESETS);
    float freq  = 100.0f * powf(80.0f, randf(&inst->rng));
    float decay = 0.001f + randf(&inst->rng) * 0.149f;
    float tone  = randf(&inst->rng);
    for (int v = 0; v < NUM_VOICES; v++) {
        voice_t *vp = &inst->voices[v];
        vp->syn_preset = syn;
        voice_apply_synth_preset(vp, syn - 1);
        vp->freq  = freq;
        vp->decay = decay;
        vp->tone  = tone;
        vp->pan   = clampf((randf(&inst->rng) - 0.5f) * 0.4f, -1.0f, 1.0f);
    }
}

static void do_rnd_mod(signal_instance_t *inst) {
    inst->mod_speed   = randf(&inst->rng) * 0.5f;
    inst->mod_offset  = randf(&inst->rng);
    inst->mod_freq    = randf(&inst->rng) * 0.4f;
    inst->mod_decay   = randf(&inst->rng) * 0.3f;
    inst->mod_pan     = randf(&inst->rng) * 0.5f;
    inst->mod_density = randf(&inst->rng) * 0.4f;
    int sh = rnd_int(&inst->rng, MOD_NUM_SHAPES);
    inst->mod_shape = sh;
    for (int v = 0; v < NUM_VOICES; v++) inst->vlfo_shape[v] = sh;
}

static void do_rnd_pitch(signal_instance_t *inst) {
    for (int v = 0; v < NUM_VOICES; v++)
        inst->voices[v].freq = rand_scale_freq(inst);
}

static void do_rnd_pan(signal_instance_t *inst) {
    for (int v = 0; v < NUM_VOICES; v++)
        inst->voices[v].pan = clampf((randf(&inst->rng) - 0.5f) * 2.0f, -1.0f, 1.0f);
}

/* do_rnd_patch last — calls all helpers above */
static void do_rnd_patch(signal_instance_t *inst) {
    do_rnd_rytm(inst);
    do_rnd_voices(inst);
    do_rnd_mod(inst);
    do_rnd_pan(inst);
    inst->density   = 0.5f + randf(&inst->rng) * 0.5f;
    inst->chaos     = randf(&inst->rng) * 0.35f;
    inst->swing     = randf(&inst->rng) * 0.4f;
    inst->gravity   = randf(&inst->rng) * 0.3f;
    inst->all_decay = randf(&inst->rng) * 0.5f;
}

/* ── Lifecycle ────────────────────────────────────────────────────────────── */
static void *create_instance(const char *module_dir, const char *json_defaults) {
    (void)module_dir; (void)json_defaults;

    signal_instance_t *inst = calloc(1, sizeof(signal_instance_t));
    if (!inst) return NULL;

    inst->master_vol = 1.0f;
    inst->bpm        = 120.0f;
    inst->tempo_sync = 1;
    inst->clk_div    = 1;
    inst->density    = 1.0f;
    inst->stereo_w   = 1.0f;
    inst->bit_crush  = 16;
    inst->bit_rate   = 0.0f;
    inst->dc_filter  = 1;
    inst->rng        = 0xDEADBEEFu;
    inst->patch_idx  = 0;

    for (int v = 0; v < NUM_VOICES; v++) {
        voice_t *vp = &inst->voices[v];
        vp->vol        = 0.8f;
        vp->level      = 0.8f;
        /* Default voice frequencies: 440, 880, 1320, 2200 Hz */
        float freqs[4] = {440.0f, 880.0f, 1320.0f, 2200.0f};
        vp->freq       = freqs[v];
        vp->wave       = WAVE_SINE;
        vp->attack     = 0.0001f;
        vp->decay      = 0.005f;
        vp->tone        = 0.5f;
        vp->tone_smooth = 0.5f;
        vp->noise_lp    = 0.0f;
        vp->damp_t           = 0.0f;
        vp->tone_rnd         = 0.0f;
        vp->tone_eff         = vp->tone;
        vp->sat              = 0.0f;
        vp->res              = 0.0f;
        vp->noise_bp         = 0.0f;
        vp->sparsity_lockout = 0;
        vp->vel         = 1.0f;
        vp->pan         = (v - 1.5f) * 0.4f; /* spread -0.6 -0.2 +0.2 +0.6 */
        vp->noise_state = 12345u + (uint32_t)v * 111u;
        vp->env_stage  = -1; /* idle */
        vp->phase2     = 0.0f;
        vp->sweep      = 0.0f;
        vp->detune     = 0.0f;
        vp->sub_div    = 1;
        memset(vp->pink_b, 0, sizeof(vp->pink_b));
        vp->brown_last = 0.0f;
        /* Breakbeat state defaults */
        vp->bb_step          = 0;
        vp->bb_bar_index     = 0;
        vp->bb_last_fired    = 0;
        vp->bb_skip_count    = 0;
        vp->bb_pitch_offset  = 0.0f;
        vp->bb_retrig_count  = 0;
        vp->bb_retrig_accum  = 0.0f;
        vp->bb_retrig_period = 0.0f;
        vp->bb_ghost_vel     = 1.0f;
        vp->bb_rng           = 0xABCDEF12u + (uint32_t)v * 0x9E3779B1u;
        vp->bb_curve_bar_seed = -1;
        vp->bb_recent_fires  = 0.0f;
        /* Holzman per-voice defaults */
        vp->improv     = 0.0f;
        vp->push_pull  = 0.0f;
        vp->bank_mode  = BANK_OFF;
        /* Default snare bank pitches: main, +5 (rim), +7 (high tom), -3 (deep ghost), +12 (piccolo) */
        vp->bank_pitch[0] = 0.0f;
        vp->bank_pitch[1] = 5.0f;
        vp->bank_pitch[2] = 7.0f;
        vp->bank_pitch[3] = -3.0f;
        vp->bank_pitch[4] = 12.0f;
        vp->ghost_crescendo = 0;
    }

    /* Modulation LFO defaults */
    inst->all_decay   = 0.0f;
    inst->sparsity    = 0.0f;
    inst->mod_amount  = 1.0f;
    inst->mod_speed   = 0.0f;
    inst->mod_offset  = 0.0f;
    inst->mod_freq    = 0.0f;
    inst->mod_decay   = 0.0f;
    inst->mod_pan     = 0.0f;
    inst->mod_density = 0.0f;
    inst->mod_shape   = MOD_SHAPE_SINE;
    for (int v = 0; v < NUM_VOICES; v++) {
        inst->vlfo_phase[v]  = 0.0f;
        inst->vlfo_sh_val[v] = 0.0f;
        inst->vlfo_rng[v]    = 111111u * (uint32_t)(v + 1);
        inst->vlfo_value[v]  = 0.0f;
        inst->vlfo_shape[v]  = MOD_SHAPE_SINE;
    }

    /* Breakbeat / scene globals */
    inst->phrase_length = PHRASE_8;
    inst->fill_shape    = FILL_RANDOM;
    inst->drummer_brain = DRUMMER_BRAIN_OFF;
    inst->step_grid     = STEP_GRID_32;
    inst->retrig_rate   = RETRIG_2X;
    inst->fill_shape_cached = 0;
    inst->fill_shape_bar    = -1;

    inst->scene_morph          = 0.0f;
    inst->scene_morph_smoothed = 0.0f;
    inst->last_applied_morph   = -1.0f; /* force first apply if morph != 0 */
    inst->morph_smooth_ms      = 50.0f;
    inst->morph_curve          = MORPH_LINEAR;
    inst->scene_a.populated    = 0;
    inst->scene_b.populated    = 0;

    /* Default Scene B = Amen (patch index 40) */
    apply_patch(inst, 40);
    capture_scene(inst, &inst->scene_b);

    /* Default Scene A = Ikeda Grid (patch index 1) — leaves live state on Scene A */
    apply_patch(inst, 1);
    capture_scene(inst, &inst->scene_a);

    /* live state is Scene A (Ikeda Grid) at boot */
    inst->last_applied_morph = 0.0f;

    return inst;
}

static void destroy_instance(void *instance) { free(instance); }

/* ── MIDI ─────────────────────────────────────────────────────────────────── */
static void on_midi(void *instance, const uint8_t *msg, int len, int source) {
    signal_instance_t *inst = (signal_instance_t *)instance;
    if (!inst || len < 1) return;
    (void)source;

    uint8_t status = msg[0];

    /* MIDI clock — advance sequencer when in Sync mode */
    if (status == 0xF8) { /* tick */
        if (!inst->tempo_sync) return;
        for (int v = 0; v < NUM_VOICES; v++) {
            voice_t *vp = &inst->voices[v];
            if (vp->seq_preset == 0) continue;

            if (seq_is_breakbeat(vp->seq_preset)) {
                /* Breakbeat path under MIDI clock */
                int step_grid_value = STEP_GRID_VALUES[inst->step_grid];
                int sub = step_grid_value / 4;
                if (sub < 1) sub = 1;
                float tps = 24.0f / (float)sub / (float)inst->clk_div;

                float swing_off = 0.0f;
                if ((vp->bb_step & 1) && inst->swing > 0.0f)
                    swing_off = tps * inst->swing * 0.5f;
                float push_off = 0.0f;
                if ((vp->bb_step & 1) && vp->push_pull != 0.0f && inst->bpm > 0.0f) {
                    /* push_pull is in ms; convert to ticks via host BPM */
                    push_off = vp->push_pull * inst->bpm / 2500.0f;
                    if (push_off < -(tps - 0.1f)) push_off = -(tps - 0.1f);
                }

                vp->tick_accum += 1.0f;
                if (vp->tick_accum >= tps + swing_off + push_off) {
                    vp->tick_accum -= (tps + swing_off + push_off);
                    vp->bb_step++;
                    if (vp->bb_step >= step_grid_value) {
                        vp->bb_step = 0;
                        vp->bb_bar_index++;
                    }
                    int is_fill = 0;
                    breakbeat_advance(inst, v, &is_fill);
                    if (vp->sparsity_lockout > 0) vp->sparsity_lockout--;
                    if (vp->sparsity_lockout == 0) {
                        float weight = 0.0f;
                        float ghost_vel = 1.0f;
                        int fire = breakbeat_step_decide(inst, v, is_fill, &weight, &ghost_vel);
                        if (fire) {
                            float prob = inst->density;
                            if (inst->mod_density > 0.0f) {
                                float lnorm = (inst->vlfo_value[v] * inst->mod_amount + 1.0f) * 0.5f;
                                prob *= (1.0f - inst->mod_density + inst->mod_density * lnorm);
                            }
                            prob = clampf(prob, 0.0f, 1.0f);
                            if (randf(&inst->rng) < prob) {
                                if (inst->gravity > 0.0f && randf(&inst->rng) < inst->gravity) {
                                    for (int ov = 0; ov < NUM_VOICES; ov++)
                                        if (ov != v) inst->voices[ov].tick_accum = 0.0f;
                                }
                                vp->bb_pitch_offset = bb_pick_bank_offset(vp, weight);
                                voice_trigger(vp);
                                vp->vel = ghost_vel;
                                if (vp->tone_rnd > 0.0f)
                                    vp->tone_eff = clampf(vp->tone + (randf(&inst->rng) - 0.5f) * 2.0f * vp->tone_rnd, 0.0f, 1.0f);
                                if (inst->sparsity > 0.0f)
                                    vp->sparsity_lockout = 1 + (int)(inst->sparsity * 31.0f);
                            }
                        }
                    }
                }
                continue;
            }

            int idx = vp->seq_preset - 1;
            if (idx < 0 || idx >= NUM_PATTERN_PRESETS) continue;
            const rhythm_pattern_t *pat = &PATTERNS[v][idx];

            /* ticks_per_step = 24 / subdivision / clk_div */
            float tps = 24.0f / (float)pat->subdivision / (float)inst->clk_div;

            /* Morse speed modifier: scale tps for Morse presets (idx 0-4) */
            if (idx < 5) {
                float spd = 0.25f + inst->morse_spd * 1.75f; /* 0.25× to 2× */
                tps *= spd;
            }

            vp->tick_accum += 1.0f;
            if (vp->tick_accum >= tps) {
                vp->tick_accum -= tps;
                vp->step = (vp->step + 1) % pat->length;
                if (pat->hits[vp->step]) {
                    /* Density + mod_density + chaos gate */
                    float prob = inst->density;
                    if (inst->mod_density > 0.0f) {
                        float lnorm = (inst->vlfo_value[v] * inst->mod_amount + 1.0f) * 0.5f;
                        prob *= (1.0f - inst->mod_density + inst->mod_density * lnorm);
                    }
                    if (inst->chaos > 0.0f)
                        prob += (randf(&inst->rng) - 0.5f) * inst->chaos;
                    prob = clampf(prob, 0.0f, 1.0f);
                    if (randf(&inst->rng) < prob) {
                        /* Gravity: with probability=gravity, sync all other voices */
                        if (inst->gravity > 0.0f && randf(&inst->rng) < inst->gravity) {
                            for (int ov = 0; ov < NUM_VOICES; ov++) {
                                if (ov != v) inst->voices[ov].tick_accum = 0.0f;
                            }
                        }
                        voice_trigger(vp);
                        if (vp->tone_rnd > 0.0f)
                            vp->tone_eff = clampf(vp->tone + (randf(&inst->rng) - 0.5f) * 2.0f * vp->tone_rnd, 0.0f, 1.0f);
                    }
                }
            }
        }
        return;
    }

    if (status == 0xFA || status == 0xFB) { /* Start / Continue */
        inst->midi_running = 1;
        for (int v = 0; v < NUM_VOICES; v++) {
            inst->voices[v].step          = 0;
            inst->voices[v].step_accum    = 0.0f;
            inst->voices[v].tick_accum    = 0.0f;
            inst->voices[v].bb_step       = 0;
            inst->voices[v].bb_bar_index  = 0;
            inst->voices[v].bb_last_fired = 0;
            inst->voices[v].bb_skip_count = 0;
            inst->voices[v].bb_retrig_count = 0;
            inst->voices[v].bb_curve_bar_seed = -1;
        }
        return;
    }

    if (status == 0xFC) { /* Stop */
        inst->midi_running = 0;
        return;
    }

    if (len < 3) return;
    uint8_t ch_status = status & 0xF0;

    /* Drum kit pad layout:
     *   Column (note % 4)     → voice 0-3
     *   Row    ((note/4) % 4) → sub_div octave override
     *   Bottom row (notes 36-39): row index 1 → sub_div 1  (normal pitch)
     *   Row 2  (notes 40-43): row index 2 → sub_div 2  (1 octave down)
     *   Row 3  (notes 44-47): row index 3 → sub_div 4  (2 octaves down)
     *   Top row (notes 48-51): row index 0 → sub_div 8  (3 octaves down)
     * Table indexed by (note/4)%4 = {8,1,2,4} maps to sub_div per row. */
    if (ch_status == 0x90 && msg[2] > 0) {
        static const int ROW_SUBDIV[4] = {8, 1, 2, 4};
        int vi  = (int)msg[1] % NUM_VOICES;
        int row = ((int)msg[1] / 4) % 4;
        voice_trigger_pad(&inst->voices[vi], (float)msg[2] / 127.0f, ROW_SUBDIV[row]);
        /* Per-pad tone randomization */
        if (inst->voices[vi].tone_rnd > 0.0f)
            inst->voices[vi].tone_eff = clampf(inst->voices[vi].tone + (randf(&inst->rng) - 0.5f) * 2.0f * inst->voices[vi].tone_rnd, 0.0f, 1.0f);
        return;
    }
    if (ch_status == 0x80 || (ch_status == 0x90 && msg[2] == 0)) {
        /* Note Off — voices are one-shot envelopes, nothing to do */
        return;
    }

    /* CC 1 (mod wheel) → chaos */
    if (ch_status == 0xB0 && msg[1] == 1)
        inst->chaos = msg[2] / 127.0f;
}

/* ── Parameters ───────────────────────────────────────────────────────────── */
/* Forward declaration */
static void signal_set_param(signal_instance_t *inst, const char *key, const char *val);

static void set_param(void *instance, const char *key, const char *val) {
    signal_instance_t *inst = (signal_instance_t *)instance;
    if (!inst || !key || !val) return;
    signal_set_param(inst, key, val);
}

static void signal_set_param(signal_instance_t *inst, const char *key, const char *val) {
    char k[24];

    /* Page navigation */
    if (strcmp(key, "_level") == 0 || strcmp(key, "current_level") == 0) {
        int n = (int)(sizeof(PAGE_NAMES) / sizeof(PAGE_NAMES[0]));
        for (int p = 0; p < n; p++) {
            if (strcmp(val, PAGE_NAMES[p]) == 0) { inst->current_page = p; return; }
        }
        return;
    }

    /* BPM from host */
    if (strcmp(key, "bpm") == 0) { inst->bpm = clampf(atof(val), 20.0f, 500.0f); return; }

    /* Page 1: seq/syn presets */
    for (int v = 0; v < NUM_VOICES; v++) {
        snprintf(k, sizeof(k), "seq%d", v + 1);
        if (strcmp(key, k) == 0) {
            int new_seq = (int)clampf(atof(val), 0, NUM_SEQ_PRESETS);
            voice_t *vp = &inst->voices[v];
            if (vp->seq_preset != new_seq) {
                /* Reset breakbeat counters when switching rhythm preset */
                vp->bb_step = 0;
                vp->bb_bar_index = 0;
                vp->bb_last_fired = 0;
                vp->bb_skip_count = 0;
                vp->bb_curve_bar_seed = -1;
                vp->bb_retrig_count = 0;
                vp->step = 0;
                vp->step_accum = 0.0f;
            }
            vp->seq_preset = new_seq;
            return;
        }
        snprintf(k, sizeof(k), "syn%d", v + 1);
        if (strcmp(key, k) == 0) {
            int p = (int)clampf(atof(val), 0, NUM_SYN_PRESETS);
            inst->voices[v].syn_preset = p;
            if (p > 0) voice_apply_synth_preset(&inst->voices[v], p - 1);
            return;
        }
    }

    /* Page 2: Mix */
    for (int v = 0; v < NUM_VOICES; v++) {
        snprintf(k, sizeof(k), "v%d_level", v + 1);
        if (strcmp(key, k) == 0) { inst->voices[v].level = clampf(atof(val), 0, 1); return; }
        snprintf(k, sizeof(k), "v%d_freq", v + 1);
        if (strcmp(key, k) == 0) { inst->voices[v].freq = clampf(atof(val), 20, 20000); return; }
    }

    /* Page 3: Params */
    if (strcmp(key, "root") == 0) {
        for (int i = 0; i < ROOT_COUNT; i++) {
            if (strcmp(val, ROOT_NAMES[i]) == 0) { inst->root = i; return; }
        }
        int iv = atoi(val);
        if (iv >= 0 && iv < ROOT_COUNT) inst->root = iv;
        return;
    }
    if (strcmp(key, "scale") == 0) {
        for (int i = 0; i < SCALE_COUNT; i++) {
            if (strcmp(val, SCALE_NAMES[i]) == 0) { inst->scale = i; return; }
        }
        int iv = atoi(val);
        if (iv >= 0 && iv < SCALE_COUNT) inst->scale = iv;
        return;
    }
    if (strcmp(key, "density")   == 0) { inst->density   = clampf(atof(val), 0, 1); return; }
    if (strcmp(key, "chaos")     == 0) { inst->chaos     = clampf(atof(val), 0, 1); return; }
    if (strcmp(key, "gravity")   == 0) { inst->gravity   = clampf(atof(val), 0, 1); return; }
    if (strcmp(key, "clk_div")   == 0) { inst->clk_div   = (int)clampf(atof(val), 1, 32); return; }
    if (strcmp(key, "morse_spd") == 0) { inst->morse_spd = clampf(atof(val), 0, 1); return; }
    if (strcmp(key, "swing")     == 0) { inst->swing     = clampf(atof(val), 0, 1); return; }

    /* Pages 4–7: Per-voice params */
    for (int v = 0; v < NUM_VOICES; v++) {
        snprintf(k, sizeof(k), "v%d_preset", v + 1);
        if (strcmp(key, k) == 0) {
            int p = (int)clampf(atof(val), 0, NUM_SYN_PRESETS);
            inst->voices[v].syn_preset = p;
            if (p > 0) voice_apply_synth_preset(&inst->voices[v], p - 1);
            return;
        }
        snprintf(k, sizeof(k), "v%d_vol", v + 1);
        if (strcmp(key, k) == 0) { inst->voices[v].vol = clampf(atof(val), 0, 1); return; }
        snprintf(k, sizeof(k), "v%d_vfreq", v + 1);
        if (strcmp(key, k) == 0) { inst->voices[v].freq = clampf(atof(val), 20, 20000); return; }
        snprintf(k, sizeof(k), "v%d_wave", v + 1);
        if (strcmp(key, k) == 0) {
            for (int w = 0; w < WAVE_COUNT; w++) {
                if (strcmp(val, WAVE_NAMES[w]) == 0) { inst->voices[v].wave = w; return; }
            }
            int iw = atoi(val);
            if (iw >= 0 && iw < WAVE_COUNT) inst->voices[v].wave = iw;
            return;
        }
        snprintf(k, sizeof(k), "v%d_tone", v + 1);
        if (strcmp(key, k) == 0) { inst->voices[v].tone = clampf(atof(val), 0, 1); return; }
        snprintf(k, sizeof(k), "v%d_attack", v + 1);
        if (strcmp(key, k) == 0) { inst->voices[v].attack = clampf(atof(val), 0.0001f, 0.05f); return; }
        snprintf(k, sizeof(k), "v%d_decay", v + 1);
        if (strcmp(key, k) == 0) { inst->voices[v].decay = clampf(atof(val), 0.0001f, 0.5f); return; }
        snprintf(k, sizeof(k), "v%d_pan", v + 1);
        if (strcmp(key, k) == 0) { inst->voices[v].pan = clampf(atof(val), -1, 1); return; }
        /* Hainbach / Modulation-applied per-voice params */
        snprintf(k, sizeof(k), "v%d_sweep", v + 1);
        if (strcmp(key, k) == 0) { inst->voices[v].sweep   = clampf(atof(val), 0, 1); return; }
        snprintf(k, sizeof(k), "v%d_detune", v + 1);
        if (strcmp(key, k) == 0) { inst->voices[v].detune  = clampf(atof(val), 0, 20); return; }
        snprintf(k, sizeof(k), "v%d_sub_div", v + 1);
        if (strcmp(key, k) == 0) { inst->voices[v].sub_div = (int)clampf(atof(val), 1, 8); return; }
        snprintf(k, sizeof(k), "v%d_tone_rnd", v + 1);
        if (strcmp(key, k) == 0) {
            char temp[16];
            snprintf(temp, sizeof(temp), "%s", val);
            char *p = strchr(temp, '%');
            if (p) *p = '\0';
            inst->voices[v].tone_rnd = clampf(atof(temp) / 100.0f, 0, 1);
            return;
        }
    }

    /* Patch page */
    if (strcmp(key, "patch") == 0) {
        for (int i = 0; i < NUM_PATCHES; i++) {
            if (strcmp(val, PATCH_NAMES[i]) == 0) { apply_patch(inst, i); return; }
        }
        int iv = atoi(val);
        if (iv >= 0 && iv < NUM_PATCHES) apply_patch(inst, iv);
        return;
    }
    /* Momentary action knobs — fire-on-edge tap-to-trigger behaviour.
     *
     * Schwung's enum-knob position is sticky: a right-turn sets the value to 1
     * and the host won't send another set_param until the user turns left to
     * reset.  We work around it by exposing these knobs in chain_params as
     * wide-range ints (0..127) so every right-turn sends a NEW value
     * (1, 2, 3, ...) that the DSP can fire on independently — without the user
     * having to turn left first.  get_param still reports the value as "0"
     * (with a brief "1" flash after fire) so if the host does happen to poll,
     * the knob snaps back. */
    if (strcmp(key, "rnd_patch")  == 0) {
        if (atof(val) != 0.0) {
            do_rnd_patch(inst);
            inst->action_flash[ACT_RND_PATCH] = ACTION_FLASH_BLOCKS;
        }
        return;
    }
    if (strcmp(key, "rnd_rytm")   == 0) {
        if (atof(val) != 0.0) {
            do_rnd_rytm(inst);
            inst->action_flash[ACT_RND_RYTM] = ACTION_FLASH_BLOCKS;
        }
        return;
    }
    if (strcmp(key, "rnd_voices") == 0) {
        if (atof(val) != 0.0) {
            do_rnd_voices(inst);
            inst->action_flash[ACT_RND_VOICES] = ACTION_FLASH_BLOCKS;
        }
        return;
    }
    if (strcmp(key, "same_voice") == 0) {
        if (atof(val) != 0.0) {
            do_same_voice(inst);
            inst->action_flash[ACT_SAME_VOICE] = ACTION_FLASH_BLOCKS;
        }
        return;
    }
    if (strcmp(key, "rnd_mod")    == 0) {
        if (atof(val) != 0.0) {
            do_rnd_mod(inst);
            inst->action_flash[ACT_RND_MOD] = ACTION_FLASH_BLOCKS;
        }
        return;
    }
    if (strcmp(key, "rnd_pitch")  == 0) {
        if (atof(val) != 0.0) {
            do_rnd_pitch(inst);
            inst->action_flash[ACT_RND_PITCH] = ACTION_FLASH_BLOCKS;
        }
        return;
    }
    if (strcmp(key, "rnd_pan")    == 0) {
        if (atof(val) != 0.0) {
            do_rnd_pan(inst);
            inst->action_flash[ACT_RND_PAN] = ACTION_FLASH_BLOCKS;
        }
        return;
    }

    /* Patch page — all_decay */
    if (strcmp(key, "all_decay")   == 0) { inst->all_decay   = clampf(atof(val), 0, 1); return; }

    /* Modulation page */
    if (strcmp(key, "mod_amount")  == 0) { inst->mod_amount  = clampf(atof(val), 0, 1); return; }
    if (strcmp(key, "mod_speed")   == 0) { inst->mod_speed   = clampf(atof(val), 0, 1); return; }
    if (strcmp(key, "mod_offset")  == 0) { inst->mod_offset  = clampf(atof(val), 0, 1); return; }
    if (strcmp(key, "mod_freq")    == 0) { inst->mod_freq    = clampf(atof(val), 0, 1); return; }
    if (strcmp(key, "mod_decay")   == 0) { inst->mod_decay   = clampf(atof(val), 0, 1); return; }
    if (strcmp(key, "mod_pan")     == 0) { inst->mod_pan     = clampf(atof(val), 0, 1); return; }
    if (strcmp(key, "mod_density") == 0) { inst->mod_density = clampf(atof(val), 0, 1); return; }
    if (strcmp(key, "mod_shape") == 0) {
        for (int i = 0; i < MOD_NUM_SHAPES; i++) {
            if (strcmp(val, MOD_SHAPE_NAMES[i]) == 0) {
                inst->mod_shape = i;
                for (int v = 0; v < NUM_VOICES; v++) {
                    if (i == MOD_SHAPE_RANDOM) {
                        inst->vlfo_rng[v] = inst->vlfo_rng[v] * 1664525u + 1013904223u;
                        inst->vlfo_shape[v] = (int)(inst->vlfo_rng[v] % MOD_NUM_SHAPES);
                    } else {
                        inst->vlfo_shape[v] = i;
                    }
                }
                return;
            }
        }
        int iv = atoi(val);
        if (iv >= 0 && iv < MOD_NUM_SHAPES) {
            inst->mod_shape = iv;
            for (int v = 0; v < NUM_VOICES; v++) inst->vlfo_shape[v] = iv;
        }
        return;
    }
    if (strcmp(key, "mod_reset") == 0) {
        if (atof(val) != 0.0) {
            for (int v = 0; v < NUM_VOICES; v++) {
                inst->vlfo_phase[v]  = 0.0f;
                inst->vlfo_sh_val[v] = 0.0f;
                inst->vlfo_value[v]  = 0.0f;
            }
            inst->action_flash[ACT_MOD_RESET] = ACTION_FLASH_BLOCKS;
        }
        return;
    }

    /* Page 8: General */
    if (strcmp(key, "master_vol") == 0) { inst->master_vol = clampf(atof(val), 0, 1); return; }
    if (strcmp(key, "bit_rate")   == 0) { inst->bit_rate   = clampf(atof(val), 0, 1); return; }
    if (strcmp(key, "tempo_sync") == 0) {
        if (strcmp(val, "Sync") == 0 || atoi(val) == 1) inst->tempo_sync = 1;
        else inst->tempo_sync = 0;
        return;
    }
    if (strcmp(key, "stereo_w")  == 0) { inst->stereo_w  = clampf(atof(val), 0, 1); return; }
    if (strcmp(key, "bit_crush") == 0) { inst->bit_crush = (int)clampf(atof(val), 1, 16); return; }
    if (strcmp(key, "drift")     == 0) { inst->drift     = clampf(atof(val), 0, 1); return; }
    if (strcmp(key, "jitter")    == 0) { inst->jitter    = clampf(atof(val), 0, 1); return; }
    if (strcmp(key, "dc_filter") == 0) {
        inst->dc_filter = (strcmp(val, "On") == 0 || atoi(val) == 1) ? 1 : 0;
        return;
    }
    if (strcmp(key, "out_mode") == 0) {
        if (strcmp(val, "Mono")   == 0 || atoi(val) == 1) inst->out_mode = 1;
        else if (strcmp(val, "Spread") == 0 || atoi(val) == 2) inst->out_mode = 2;
        else inst->out_mode = 0;
        return;
    }

    /* ── v0.2: Scene morph + breakbeat globals + Holzman ── */

    if (strcmp(key, "scene_morph") == 0) {
        inst->scene_morph = clampf(atof(val), 0.0f, 1.0f);
        return;
    }
    if (strcmp(key, "save_scene_a") == 0) {
        if (atof(val) != 0.0) {
            capture_scene(inst, &inst->scene_a);
            inst->action_flash[ACT_SAVE_SCENE_A] = ACTION_FLASH_BLOCKS;
        }
        return;
    }
    if (strcmp(key, "save_scene_b") == 0) {
        if (atof(val) != 0.0) {
            capture_scene(inst, &inst->scene_b);
            inst->action_flash[ACT_SAVE_SCENE_B] = ACTION_FLASH_BLOCKS;
        }
        return;
    }
    if (strcmp(key, "morph_curve") == 0) {
        for (int i = 0; i < MORPH_CURVE_COUNT; i++) {
            if (strcmp(val, MORPH_CURVE_NAMES[i]) == 0) { inst->morph_curve = i; return; }
        }
        int iv = atoi(val);
        if (iv >= 0 && iv < MORPH_CURVE_COUNT) inst->morph_curve = iv;
        return;
    }
    if (strcmp(key, "morph_smooth") == 0) {
        inst->morph_smooth_ms = clampf(atof(val), 0.0f, 200.0f);
        return;
    }
    if (strcmp(key, "phrase_length") == 0) {
        for (int i = 0; i < PHRASE_COUNT; i++) {
            if (strcmp(val, PHRASE_NAMES[i]) == 0) { inst->phrase_length = i; return; }
        }
        int iv = atoi(val);
        if (iv >= 0 && iv < PHRASE_COUNT) inst->phrase_length = iv;
        return;
    }
    if (strcmp(key, "fill_shape") == 0) {
        for (int i = 0; i < FILL_COUNT; i++) {
            if (strcmp(val, FILL_NAMES[i]) == 0) { inst->fill_shape = i; return; }
        }
        int iv = atoi(val);
        if (iv >= 0 && iv < FILL_COUNT) inst->fill_shape = iv;
        return;
    }
    if (strcmp(key, "step_grid") == 0) {
        for (int i = 0; i < STEP_GRID_COUNT; i++) {
            if (strcmp(val, STEP_GRID_NAMES[i]) == 0) { inst->step_grid = i; return; }
        }
        int iv = atoi(val);
        if (iv >= 0 && iv < STEP_GRID_COUNT) inst->step_grid = iv;
        return;
    }
    if (strcmp(key, "retrig_rate") == 0) {
        for (int i = 0; i < RETRIG_COUNT; i++) {
            if (strcmp(val, RETRIG_NAMES[i]) == 0) { inst->retrig_rate = i; return; }
        }
        int iv = atoi(val);
        if (iv >= 0 && iv < RETRIG_COUNT) inst->retrig_rate = iv;
        return;
    }
    if (strcmp(key, "drummer_brain") == 0) {
        for (int i = 0; i < DRUMMER_BRAIN_COUNT; i++) {
            if (strcmp(val, DRUMMER_BRAIN_NAMES[i]) == 0) { inst->drummer_brain = i; return; }
        }
        int iv = atoi(val);
        if (iv >= 0 && iv < DRUMMER_BRAIN_COUNT) inst->drummer_brain = iv;
        return;
    }

    /* Per-voice Holzman params */
    for (int v = 0; v < NUM_VOICES; v++) {
        snprintf(k, sizeof(k), "v%d_improv", v + 1);
        if (strcmp(key, k) == 0) { inst->voices[v].improv = clampf(atof(val), 0, 1); return; }
        snprintf(k, sizeof(k), "v%d_push_pull", v + 1);
        if (strcmp(key, k) == 0) { inst->voices[v].push_pull = clampf(atof(val), -20.0f, 20.0f); return; }
        snprintf(k, sizeof(k), "v%d_bank_mode", v + 1);
        if (strcmp(key, k) == 0) {
            for (int i = 0; i < BANK_MODE_COUNT; i++) {
                if (strcmp(val, BANK_MODE_NAMES[i]) == 0) { inst->voices[v].bank_mode = i; return; }
            }
            int iv = atoi(val);
            if (iv >= 0 && iv < BANK_MODE_COUNT) inst->voices[v].bank_mode = iv;
            return;
        }
        snprintf(k, sizeof(k), "v%d_ghost_crescendo", v + 1);
        if (strcmp(key, k) == 0) {
            inst->voices[v].ghost_crescendo = (strcmp(val, "On") == 0 || atoi(val) == 1) ? 1 : 0;
            return;
        }
        for (int b = 0; b < 5; b++) {
            snprintf(k, sizeof(k), "v%d_bank_pitch_%d", v + 1, b);
            if (strcmp(key, k) == 0) {
                inst->voices[v].bank_pitch[b] = clampf(atof(val), -24.0f, 24.0f);
                return;
            }
        }
    }

    /* State restore */
    if (strcmp(key, "state") == 0) {
        /* Simple key=value CSV parsing */
        char buf[4096];
        snprintf(buf, sizeof(buf), "%s", val);
        char *p = buf;
        while (*p) {
            /* Find key */
            char *kstart = p;
            while (*p && *p != '=') p++;
            if (!*p) break;
            *p = '\0'; p++;
            char *vstart = p;
            while (*p && *p != ';') p++;
            if (*p) { *p = '\0'; p++; }
            signal_set_param(inst, kstart, vstart);
        }
    }
}

/* ── chain_params JSON ────────────────────────────────────────────────────── */
static const char CHAIN_PARAMS_JSON[] =
    "[{\"key\":\"seq1\",\"name\":\"Seq 1\",\"type\":\"int\",\"min\":0,\"max\":35,\"step\":1},"
    "{\"key\":\"seq2\",\"name\":\"Seq 2\",\"type\":\"int\",\"min\":0,\"max\":35,\"step\":1},"
    "{\"key\":\"seq3\",\"name\":\"Seq 3\",\"type\":\"int\",\"min\":0,\"max\":35,\"step\":1},"
    "{\"key\":\"seq4\",\"name\":\"Seq 4\",\"type\":\"int\",\"min\":0,\"max\":35,\"step\":1},"
    "{\"key\":\"syn1\",\"name\":\"Syn 1\",\"type\":\"int\",\"min\":0,\"max\":50,\"step\":1},"
    "{\"key\":\"syn2\",\"name\":\"Syn 2\",\"type\":\"int\",\"min\":0,\"max\":50,\"step\":1},"
    "{\"key\":\"syn3\",\"name\":\"Syn 3\",\"type\":\"int\",\"min\":0,\"max\":50,\"step\":1},"
    "{\"key\":\"syn4\",\"name\":\"Syn 4\",\"type\":\"int\",\"min\":0,\"max\":50,\"step\":1},"
    "{\"key\":\"v1_level\",\"name\":\"V1 Level\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"v2_level\",\"name\":\"V2 Level\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"v3_level\",\"name\":\"V3 Level\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"v4_level\",\"name\":\"V4 Level\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"v1_freq\",\"name\":\"V1 Freq\",\"type\":\"float\",\"min\":20,\"max\":20000,\"step\":1},"
    "{\"key\":\"v2_freq\",\"name\":\"V2 Freq\",\"type\":\"float\",\"min\":20,\"max\":20000,\"step\":1},"
    "{\"key\":\"v3_freq\",\"name\":\"V3 Freq\",\"type\":\"float\",\"min\":20,\"max\":20000,\"step\":1},"
    "{\"key\":\"v4_freq\",\"name\":\"V4 Freq\",\"type\":\"float\",\"min\":20,\"max\":20000,\"step\":1},"
    "{\"key\":\"root\",\"name\":\"Root\",\"type\":\"enum\",\"options\":[\"Free\",\"C\",\"C#\",\"D\",\"D#\",\"E\",\"F\",\"F#\",\"G\",\"G#\",\"A\",\"A#\",\"B\"]},"
    "{\"key\":\"scale\",\"name\":\"Scale\",\"type\":\"enum\",\"options\":[\"Free\",\"Chromatic\",\"Major\",\"Minor\",\"Pentatonic\",\"WholeTone\",\"Diminished\",\"HarmMin\"]},"
    "{\"key\":\"density\",\"name\":\"Density\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"chaos\",\"name\":\"Chaos\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"gravity\",\"name\":\"Gravity\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"clk_div\",\"name\":\"Clk Div\",\"type\":\"int\",\"min\":1,\"max\":32,\"step\":1},"
    "{\"key\":\"morse_spd\",\"name\":\"Morse Spd\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"swing\",\"name\":\"Swing\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"v1_preset\",\"name\":\"V1 Preset\",\"type\":\"int\",\"min\":0,\"max\":50,\"step\":1},"
    "{\"key\":\"v1_vol\",\"name\":\"V1 Vol\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"v1_vfreq\",\"name\":\"V1 Freq\",\"type\":\"float\",\"min\":20,\"max\":20000,\"step\":1},"
    "{\"key\":\"v1_wave\",\"name\":\"V1 Wave\",\"type\":\"enum\",\"options\":[\"Sine\",\"Impulse\",\"Noise\",\"Damped\",\"Click\",\"Square\",\"Tri\",\"FM\",\"Pink\",\"Brown\",\"AM\",\"Ping\",\"Train\",\"Kick\",\"Snare\",\"Hat\"]},"
    "{\"key\":\"v1_tone\",\"name\":\"V1 Tone\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"v1_attack\",\"name\":\"V1 Attack\",\"type\":\"float\",\"min\":0.0001,\"max\":0.05,\"step\":0.0001},"
    "{\"key\":\"v1_decay\",\"name\":\"V1 Decay\",\"type\":\"float\",\"min\":0.0001,\"max\":0.5,\"step\":0.001},"
    "{\"key\":\"v1_pan\",\"name\":\"V1 Pan\",\"type\":\"float\",\"min\":-1,\"max\":1,\"step\":0.01},"
    "{\"key\":\"v1_sub_div\",\"short_name\":\"Sub Div\",\"name\":\"V1 Sub Div\",\"type\":\"int\",\"min\":1,\"max\":8,\"step\":1},"
    "{\"key\":\"v1_sweep\",\"name\":\"V1 Sweep\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"v1_detune\",\"name\":\"V1 Detune\",\"type\":\"float\",\"min\":0,\"max\":20,\"step\":0.1},"
    "{\"key\":\"v1_tone_rnd\",\"short_name\":\"Rnd\",\"name\":\"Tone Rnd\",\"type\":\"enum\",\"options\":[\"0%\",\"1%\",\"2%\",\"3%\",\"4%\",\"5%\",\"6%\",\"7%\",\"8%\",\"9%\",\"10%\",\"11%\",\"12%\",\"13%\",\"14%\",\"15%\",\"16%\",\"17%\",\"18%\",\"19%\",\"20%\",\"21%\",\"22%\",\"23%\",\"24%\",\"25%\",\"26%\",\"27%\",\"28%\",\"29%\",\"30%\",\"31%\",\"32%\",\"33%\",\"34%\",\"35%\",\"36%\",\"37%\",\"38%\",\"39%\",\"40%\",\"41%\",\"42%\",\"43%\",\"44%\",\"45%\",\"46%\",\"47%\",\"48%\",\"49%\",\"50%\",\"51%\",\"52%\",\"53%\",\"54%\",\"55%\",\"56%\",\"57%\",\"58%\",\"59%\",\"60%\",\"61%\",\"62%\",\"63%\",\"64%\",\"65%\",\"66%\",\"67%\",\"68%\",\"69%\",\"70%\",\"71%\",\"72%\",\"73%\",\"74%\",\"75%\",\"76%\",\"77%\",\"78%\",\"79%\",\"80%\",\"81%\",\"82%\",\"83%\",\"84%\",\"85%\",\"86%\",\"87%\",\"88%\",\"89%\",\"90%\",\"91%\",\"92%\",\"93%\",\"94%\",\"95%\",\"96%\",\"97%\",\"98%\",\"99%\",\"100%\"]},"
    "{\"key\":\"v2_preset\",\"name\":\"V2 Preset\",\"type\":\"int\",\"min\":0,\"max\":50,\"step\":1},"
    "{\"key\":\"v2_vol\",\"name\":\"V2 Vol\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"v2_vfreq\",\"name\":\"V2 Freq\",\"type\":\"float\",\"min\":20,\"max\":20000,\"step\":1},"
    "{\"key\":\"v2_wave\",\"name\":\"V2 Wave\",\"type\":\"enum\",\"options\":[\"Sine\",\"Impulse\",\"Noise\",\"Damped\",\"Click\",\"Square\",\"Tri\",\"FM\",\"Pink\",\"Brown\",\"AM\",\"Ping\",\"Train\",\"Kick\",\"Snare\",\"Hat\"]},"
    "{\"key\":\"v2_tone\",\"name\":\"V2 Tone\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"v2_attack\",\"name\":\"V2 Attack\",\"type\":\"float\",\"min\":0.0001,\"max\":0.05,\"step\":0.0001},"
    "{\"key\":\"v2_decay\",\"name\":\"V2 Decay\",\"type\":\"float\",\"min\":0.0001,\"max\":0.5,\"step\":0.001},"
    "{\"key\":\"v2_pan\",\"name\":\"V2 Pan\",\"type\":\"float\",\"min\":-1,\"max\":1,\"step\":0.01},"
    "{\"key\":\"v2_sub_div\",\"short_name\":\"Sub Div\",\"name\":\"V2 Sub Div\",\"type\":\"int\",\"min\":1,\"max\":8,\"step\":1},"
    "{\"key\":\"v2_sweep\",\"name\":\"V2 Sweep\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"v2_detune\",\"name\":\"V2 Detune\",\"type\":\"float\",\"min\":0,\"max\":20,\"step\":0.1},"
    "{\"key\":\"v2_tone_rnd\",\"short_name\":\"Rnd\",\"name\":\"Tone Rnd\",\"type\":\"enum\",\"options\":[\"0%\",\"1%\",\"2%\",\"3%\",\"4%\",\"5%\",\"6%\",\"7%\",\"8%\",\"9%\",\"10%\",\"11%\",\"12%\",\"13%\",\"14%\",\"15%\",\"16%\",\"17%\",\"18%\",\"19%\",\"20%\",\"21%\",\"22%\",\"23%\",\"24%\",\"25%\",\"26%\",\"27%\",\"28%\",\"29%\",\"30%\",\"31%\",\"32%\",\"33%\",\"34%\",\"35%\",\"36%\",\"37%\",\"38%\",\"39%\",\"40%\",\"41%\",\"42%\",\"43%\",\"44%\",\"45%\",\"46%\",\"47%\",\"48%\",\"49%\",\"50%\",\"51%\",\"52%\",\"53%\",\"54%\",\"55%\",\"56%\",\"57%\",\"58%\",\"59%\",\"60%\",\"61%\",\"62%\",\"63%\",\"64%\",\"65%\",\"66%\",\"67%\",\"68%\",\"69%\",\"70%\",\"71%\",\"72%\",\"73%\",\"74%\",\"75%\",\"76%\",\"77%\",\"78%\",\"79%\",\"80%\",\"81%\",\"82%\",\"83%\",\"84%\",\"85%\",\"86%\",\"87%\",\"88%\",\"89%\",\"90%\",\"91%\",\"92%\",\"93%\",\"94%\",\"95%\",\"96%\",\"97%\",\"98%\",\"99%\",\"100%\"]},"
    "{\"key\":\"v3_preset\",\"name\":\"V3 Preset\",\"type\":\"int\",\"min\":0,\"max\":50,\"step\":1},"
    "{\"key\":\"v3_vol\",\"name\":\"V3 Vol\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"v3_vfreq\",\"name\":\"V3 Freq\",\"type\":\"float\",\"min\":20,\"max\":20000,\"step\":1},"
    "{\"key\":\"v3_wave\",\"name\":\"V3 Wave\",\"type\":\"enum\",\"options\":[\"Sine\",\"Impulse\",\"Noise\",\"Damped\",\"Click\",\"Square\",\"Tri\",\"FM\",\"Pink\",\"Brown\",\"AM\",\"Ping\",\"Train\",\"Kick\",\"Snare\",\"Hat\"]},"
    "{\"key\":\"v3_tone\",\"name\":\"V3 Tone\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"v3_attack\",\"name\":\"V3 Attack\",\"type\":\"float\",\"min\":0.0001,\"max\":0.05,\"step\":0.0001},"
    "{\"key\":\"v3_decay\",\"name\":\"V3 Decay\",\"type\":\"float\",\"min\":0.0001,\"max\":0.5,\"step\":0.001},"
    "{\"key\":\"v3_pan\",\"name\":\"V3 Pan\",\"type\":\"float\",\"min\":-1,\"max\":1,\"step\":0.01},"
    "{\"key\":\"v3_sub_div\",\"short_name\":\"Sub Div\",\"name\":\"V3 Sub Div\",\"type\":\"int\",\"min\":1,\"max\":8,\"step\":1},"
    "{\"key\":\"v3_sweep\",\"name\":\"V3 Sweep\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"v3_detune\",\"name\":\"V3 Detune\",\"type\":\"float\",\"min\":0,\"max\":20,\"step\":0.1},"
    "{\"key\":\"v3_tone_rnd\",\"short_name\":\"Rnd\",\"name\":\"Tone Rnd\",\"type\":\"enum\",\"options\":[\"0%\",\"1%\",\"2%\",\"3%\",\"4%\",\"5%\",\"6%\",\"7%\",\"8%\",\"9%\",\"10%\",\"11%\",\"12%\",\"13%\",\"14%\",\"15%\",\"16%\",\"17%\",\"18%\",\"19%\",\"20%\",\"21%\",\"22%\",\"23%\",\"24%\",\"25%\",\"26%\",\"27%\",\"28%\",\"29%\",\"30%\",\"31%\",\"32%\",\"33%\",\"34%\",\"35%\",\"36%\",\"37%\",\"38%\",\"39%\",\"40%\",\"41%\",\"42%\",\"43%\",\"44%\",\"45%\",\"46%\",\"47%\",\"48%\",\"49%\",\"50%\",\"51%\",\"52%\",\"53%\",\"54%\",\"55%\",\"56%\",\"57%\",\"58%\",\"59%\",\"60%\",\"61%\",\"62%\",\"63%\",\"64%\",\"65%\",\"66%\",\"67%\",\"68%\",\"69%\",\"70%\",\"71%\",\"72%\",\"73%\",\"74%\",\"75%\",\"76%\",\"77%\",\"78%\",\"79%\",\"80%\",\"81%\",\"82%\",\"83%\",\"84%\",\"85%\",\"86%\",\"87%\",\"88%\",\"89%\",\"90%\",\"91%\",\"92%\",\"93%\",\"94%\",\"95%\",\"96%\",\"97%\",\"98%\",\"99%\",\"100%\"]},"
    "{\"key\":\"v4_preset\",\"name\":\"V4 Preset\",\"type\":\"int\",\"min\":0,\"max\":50,\"step\":1},"
    "{\"key\":\"v4_vol\",\"name\":\"V4 Vol\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"v4_vfreq\",\"name\":\"V4 Freq\",\"type\":\"float\",\"min\":20,\"max\":20000,\"step\":1},"
    "{\"key\":\"v4_wave\",\"name\":\"V4 Wave\",\"type\":\"enum\",\"options\":[\"Sine\",\"Impulse\",\"Noise\",\"Damped\",\"Click\",\"Square\",\"Tri\",\"FM\",\"Pink\",\"Brown\",\"AM\",\"Ping\",\"Train\",\"Kick\",\"Snare\",\"Hat\"]},"
    "{\"key\":\"v4_tone\",\"name\":\"V4 Tone\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"v4_attack\",\"name\":\"V4 Attack\",\"type\":\"float\",\"min\":0.0001,\"max\":0.05,\"step\":0.0001},"
    "{\"key\":\"v4_decay\",\"name\":\"V4 Decay\",\"type\":\"float\",\"min\":0.0001,\"max\":0.5,\"step\":0.001},"
    "{\"key\":\"v4_pan\",\"name\":\"V4 Pan\",\"type\":\"float\",\"min\":-1,\"max\":1,\"step\":0.01},"
    "{\"key\":\"v4_sub_div\",\"short_name\":\"Sub Div\",\"name\":\"V4 Sub Div\",\"type\":\"int\",\"min\":1,\"max\":8,\"step\":1},"
    "{\"key\":\"v4_sweep\",\"name\":\"V4 Sweep\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"v4_detune\",\"name\":\"V4 Detune\",\"type\":\"float\",\"min\":0,\"max\":20,\"step\":0.1},"
    "{\"key\":\"v4_tone_rnd\",\"short_name\":\"Rnd\",\"name\":\"Tone Rnd\",\"type\":\"enum\",\"options\":[\"0%\",\"1%\",\"2%\",\"3%\",\"4%\",\"5%\",\"6%\",\"7%\",\"8%\",\"9%\",\"10%\",\"11%\",\"12%\",\"13%\",\"14%\",\"15%\",\"16%\",\"17%\",\"18%\",\"19%\",\"20%\",\"21%\",\"22%\",\"23%\",\"24%\",\"25%\",\"26%\",\"27%\",\"28%\",\"29%\",\"30%\",\"31%\",\"32%\",\"33%\",\"34%\",\"35%\",\"36%\",\"37%\",\"38%\",\"39%\",\"40%\",\"41%\",\"42%\",\"43%\",\"44%\",\"45%\",\"46%\",\"47%\",\"48%\",\"49%\",\"50%\",\"51%\",\"52%\",\"53%\",\"54%\",\"55%\",\"56%\",\"57%\",\"58%\",\"59%\",\"60%\",\"61%\",\"62%\",\"63%\",\"64%\",\"65%\",\"66%\",\"67%\",\"68%\",\"69%\",\"70%\",\"71%\",\"72%\",\"73%\",\"74%\",\"75%\",\"76%\",\"77%\",\"78%\",\"79%\",\"80%\",\"81%\",\"82%\",\"83%\",\"84%\",\"85%\",\"86%\",\"87%\",\"88%\",\"89%\",\"90%\",\"91%\",\"92%\",\"93%\",\"94%\",\"95%\",\"96%\",\"97%\",\"98%\",\"99%\",\"100%\"]},"
    "{\"key\":\"patch\",\"name\":\"Patch\",\"type\":\"enum\",\"options\":[\"Init\",\"Ikeda Grid\",\"Bernier\",\"Morse CQ\",\"Mathcore\",\"Pink Rain\",\"Heterodyne\",\"Sub Harm\",\"CA Automata\",\"FM Bell\",\"AM Texture\",\"Brown Pulse\",\"Clk Matrix\",\"Sweep Casc\",\"Fibonacci\",\"Digi Glitch\",\"Chirp Field\",\"Phase Cloud\",\"Test Signal\",\"CW Radio\",\"Cantor\",\"Noise Gate\",\"Sub Bass\",\"Thue-Morse\",\"Pink Grid\",\"Metallic FM\",\"Minimal\",\"Maximum\",\"Rule 30\",\"Freq Lat.\",\"Gauss Pings\",\"Ikeda Data\",\"Tuning Fork\",\"Fork Harmony\",\"BL Square\",\"Data Quanta\",\"Resonant Bell\",\"Sine Study\",\"Noise Field\",\"Aleatoric\",\"Amen\",\"Think\",\"Funky Drummer\",\"Piccolo Jungle\",\"Liquid DnB\",\"Holzman Live\",\"Microsound Jungle\",\"Bernier Jungle\",\"Mathcore Break\",\"Bretschneider Roll\"]},"
    /* Momentary action knobs — wide-range int so each right-turn sends a
     * fresh value (1, 2, 3, ...) that triggers a fire.  Schwung tracks the
     * value locally for display and does not re-poll get_param, so the
     * displayed number counts up; the trade-off is that every right-turn
     * fires the action without needing to turn left to reset. */
    "{\"key\":\"rnd_patch\",\"name\":\"Rnd Patch\",\"type\":\"int\",\"min\":0,\"max\":127,\"step\":1},"
    "{\"key\":\"rnd_rytm\",\"name\":\"Rnd Rytm\",\"type\":\"int\",\"min\":0,\"max\":127,\"step\":1},"
    "{\"key\":\"rnd_voices\",\"name\":\"Rnd Voices\",\"type\":\"int\",\"min\":0,\"max\":127,\"step\":1},"
    "{\"key\":\"same_voice\",\"short_name\":\"Voice\",\"name\":\"Same Voice\",\"type\":\"int\",\"min\":0,\"max\":127,\"step\":1},"
    "{\"key\":\"rnd_mod\",\"name\":\"Rnd Mod\",\"type\":\"int\",\"min\":0,\"max\":127,\"step\":1},"
    "{\"key\":\"rnd_pitch\",\"name\":\"Rnd Pitch\",\"type\":\"int\",\"min\":0,\"max\":127,\"step\":1},"
    "{\"key\":\"rnd_pan\",\"name\":\"Rnd Pan\",\"type\":\"int\",\"min\":0,\"max\":127,\"step\":1},"
    "{\"key\":\"all_decay\",\"name\":\"All Decay\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"mod_amount\",\"name\":\"Mod Amt\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"mod_speed\",\"name\":\"Mod Speed\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"mod_offset\",\"short_name\":\"Offset\",\"name\":\"Mod Offset\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"mod_freq\",\"name\":\"Freq Mod\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"mod_decay\",\"name\":\"Decay Mod\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"mod_pan\",\"name\":\"Pan Mod\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"mod_density\",\"name\":\"Density Mod\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"mod_shape\",\"name\":\"Mod Shape\",\"type\":\"enum\",\"options\":[\"Sine\",\"Tri\",\"Saw\",\"Square\",\"S&H\",\"Random\"]},"
    "{\"key\":\"mod_reset\",\"name\":\"Mod Reset\",\"type\":\"int\",\"min\":0,\"max\":127,\"step\":1},"
    "{\"key\":\"master_vol\",\"name\":\"Volume\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"tempo_sync\",\"name\":\"Sync\",\"type\":\"enum\",\"options\":[\"Free\",\"Sync\"]},"
    "{\"key\":\"bpm\",\"name\":\"BPM\",\"type\":\"float\",\"min\":20,\"max\":500,\"step\":1},"
    "{\"key\":\"bit_crush\",\"name\":\"Bit Crush\",\"type\":\"int\",\"min\":1,\"max\":16,\"step\":1},"
    "{\"key\":\"bit_rate\",\"name\":\"Bit Rate\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"stereo_w\",\"name\":\"Stereo W\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"drift\",\"name\":\"Drift\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"jitter\",\"name\":\"Jitter\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"dc_filter\",\"name\":\"DC Filt\",\"type\":\"enum\",\"options\":[\"Off\",\"On\"]},"
    "{\"key\":\"out_mode\",\"short_name\":\"Out\",\"name\":\"Out Mode\",\"type\":\"enum\",\"options\":[\"Stereo\",\"Mono\",\"Spread\"]},"
    /* v0.2: Scene morph + breakbeat globals */
    "{\"key\":\"scene_morph\",\"name\":\"Scene\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"save_scene_a\",\"short_name\":\"A\",\"name\":\"Save \xe2\x86\x92 A\",\"type\":\"int\",\"min\":0,\"max\":127,\"step\":1},"
    "{\"key\":\"save_scene_b\",\"short_name\":\"B\",\"name\":\"Save \xe2\x86\x92 B\",\"type\":\"int\",\"min\":0,\"max\":127,\"step\":1},"
    "{\"key\":\"morph_curve\",\"short_name\":\"Curve\",\"name\":\"Morph Curve\",\"type\":\"enum\",\"options\":[\"Linear\",\"Exp\",\"Stepped\"]},"
    "{\"key\":\"morph_smooth\",\"short_name\":\"Smooth\",\"name\":\"Morph Smooth\",\"type\":\"float\",\"min\":0,\"max\":200,\"step\":1},"
    "{\"key\":\"phrase_length\",\"name\":\"Phrase\",\"type\":\"enum\",\"options\":[\"Off\",\"2\",\"4\",\"8\",\"16\"]},"
    "{\"key\":\"fill_shape\",\"name\":\"Fill\",\"type\":\"enum\",\"options\":[\"Snare Roll\",\"Tom Run\",\"Kick Fill\",\"Hat Splatter\",\"Crash Drop\",\"Random\"]},"
    "{\"key\":\"step_grid\",\"short_name\":\"Grid\",\"name\":\"Step Grid\",\"type\":\"enum\",\"options\":[\"8\",\"16\",\"32\"]},"
    "{\"key\":\"retrig_rate\",\"short_name\":\"Retrig\",\"name\":\"Retrig Rate\",\"type\":\"enum\",\"options\":[\"2x\",\"3x\",\"4x\",\"8x\",\"Rand\"]},"
    "{\"key\":\"drummer_brain\",\"short_name\":\"Brain\",\"name\":\"Drummer Brain\",\"type\":\"enum\",\"options\":[\"Off\",\"Light\",\"Heavy\"]},"
    /* v0.2: per-voice Holzman params */
    "{\"key\":\"v1_improv\",\"short_name\":\"Improv\",\"name\":\"V1 Improv\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"v1_push_pull\",\"short_name\":\"Push\",\"name\":\"V1 Push/Pull\",\"type\":\"float\",\"min\":-20,\"max\":20,\"step\":0.5},"
    "{\"key\":\"v1_bank_mode\",\"short_name\":\"Bank\",\"name\":\"V1 Bank\",\"type\":\"enum\",\"options\":[\"Off\",\"Rule\",\"Random\"]},"
    "{\"key\":\"v1_bank_pitch_0\",\"short_name\":\"V1 0\",\"name\":\"V1 Bank 0\",\"type\":\"float\",\"min\":-24,\"max\":24,\"step\":1},"
    "{\"key\":\"v1_bank_pitch_1\",\"name\":\"V1 Bank 1\",\"type\":\"float\",\"min\":-24,\"max\":24,\"step\":1},"
    "{\"key\":\"v1_bank_pitch_2\",\"short_name\":\"V1 2\",\"name\":\"V1 Bank 2\",\"type\":\"float\",\"min\":-24,\"max\":24,\"step\":1},"
    "{\"key\":\"v1_bank_pitch_3\",\"name\":\"V1 Bank 3\",\"type\":\"float\",\"min\":-24,\"max\":24,\"step\":1},"
    "{\"key\":\"v1_bank_pitch_4\",\"short_name\":\"V1 4\",\"name\":\"V1 Bank 4\",\"type\":\"float\",\"min\":-24,\"max\":24,\"step\":1},"
    "{\"key\":\"v1_ghost_crescendo\",\"short_name\":\"Ghost Cresc\",\"name\":\"V1 Ghost Cresc\",\"type\":\"enum\",\"options\":[\"Off\",\"On\"]},"
    "{\"key\":\"v2_improv\",\"short_name\":\"Improv\",\"name\":\"V2 Improv\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"v2_push_pull\",\"short_name\":\"Push\",\"name\":\"V2 Push/Pull\",\"type\":\"float\",\"min\":-20,\"max\":20,\"step\":0.5},"
    "{\"key\":\"v2_bank_mode\",\"short_name\":\"Bank\",\"name\":\"V2 Bank\",\"type\":\"enum\",\"options\":[\"Off\",\"Rule\",\"Random\"]},"
    "{\"key\":\"v2_bank_pitch_0\",\"short_name\":\"V2 0\",\"name\":\"V2 Bank 0\",\"type\":\"float\",\"min\":-24,\"max\":24,\"step\":1},"
    "{\"key\":\"v2_bank_pitch_1\",\"short_name\":\"V2 1\",\"name\":\"V2 Bank 1\",\"type\":\"float\",\"min\":-24,\"max\":24,\"step\":1},"
    "{\"key\":\"v2_bank_pitch_2\",\"name\":\"V2 Bank 2\",\"type\":\"float\",\"min\":-24,\"max\":24,\"step\":1},"
    "{\"key\":\"v2_bank_pitch_3\",\"name\":\"V2 Bank 3\",\"type\":\"float\",\"min\":-24,\"max\":24,\"step\":1},"
    "{\"key\":\"v2_bank_pitch_4\",\"short_name\":\"V2 4\",\"name\":\"V2 Bank 4\",\"type\":\"float\",\"min\":-24,\"max\":24,\"step\":1},"
    "{\"key\":\"v2_ghost_crescendo\",\"short_name\":\"Ghost Cresc\",\"name\":\"V2 Ghost Cresc\",\"type\":\"enum\",\"options\":[\"Off\",\"On\"]},"
    "{\"key\":\"v3_improv\",\"short_name\":\"Improv\",\"name\":\"V3 Improv\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"v3_push_pull\",\"short_name\":\"Push\",\"name\":\"V3 Push/Pull\",\"type\":\"float\",\"min\":-20,\"max\":20,\"step\":0.5},"
    "{\"key\":\"v3_bank_mode\",\"short_name\":\"Bank\",\"name\":\"V3 Bank\",\"type\":\"enum\",\"options\":[\"Off\",\"Rule\",\"Random\"]},"
    "{\"key\":\"v3_bank_pitch_0\",\"short_name\":\"V3 0\",\"name\":\"V3 Bank 0\",\"type\":\"float\",\"min\":-24,\"max\":24,\"step\":1},"
    "{\"key\":\"v3_bank_pitch_1\",\"short_name\":\"V3 1\",\"name\":\"V3 Bank 1\",\"type\":\"float\",\"min\":-24,\"max\":24,\"step\":1},"
    "{\"key\":\"v3_bank_pitch_2\",\"short_name\":\"V3 2\",\"name\":\"V3 Bank 2\",\"type\":\"float\",\"min\":-24,\"max\":24,\"step\":1},"
    "{\"key\":\"v3_bank_pitch_3\",\"short_name\":\"V3 Bank\",\"name\":\"V3 Bank 3\",\"type\":\"float\",\"min\":-24,\"max\":24,\"step\":1},"
    "{\"key\":\"v3_bank_pitch_4\",\"short_name\":\"V3 4\",\"name\":\"V3 Bank 4\",\"type\":\"float\",\"min\":-24,\"max\":24,\"step\":1},"
    "{\"key\":\"v3_ghost_crescendo\",\"short_name\":\"Ghost Cresc\",\"name\":\"V3 Ghost Cresc\",\"type\":\"enum\",\"options\":[\"Off\",\"On\"]},"
    "{\"key\":\"v4_improv\",\"short_name\":\"Improv\",\"name\":\"V4 Improv\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
    "{\"key\":\"v4_push_pull\",\"short_name\":\"Push\",\"name\":\"V4 Push/Pull\",\"type\":\"float\",\"min\":-20,\"max\":20,\"step\":0.5},"
    "{\"key\":\"v4_bank_mode\",\"short_name\":\"Bank\",\"name\":\"V4 Bank\",\"type\":\"enum\",\"options\":[\"Off\",\"Rule\",\"Random\"]},"
    "{\"key\":\"v4_bank_pitch_0\",\"short_name\":\"V4 0\",\"name\":\"V4 Bank 0\",\"type\":\"float\",\"min\":-24,\"max\":24,\"step\":1},"
    "{\"key\":\"v4_bank_pitch_1\",\"short_name\":\"V4 1\",\"name\":\"V4 Bank 1\",\"type\":\"float\",\"min\":-24,\"max\":24,\"step\":1},"
    "{\"key\":\"v4_bank_pitch_2\",\"short_name\":\"V4 2\",\"name\":\"V4 Bank 2\",\"type\":\"float\",\"min\":-24,\"max\":24,\"step\":1},"
    "{\"key\":\"v4_bank_pitch_3\",\"name\":\"V4 Bank 3\",\"type\":\"float\",\"min\":-24,\"max\":24,\"step\":1},"
    "{\"key\":\"v4_bank_pitch_4\",\"name\":\"V4 Bank 4\",\"type\":\"float\",\"min\":-24,\"max\":24,\"step\":1},"
    "{\"key\":\"v4_ghost_crescendo\",\"short_name\":\"Ghost Cresc\",\"name\":\"V4 Ghost Cresc\",\"type\":\"enum\",\"options\":[\"Off\",\"On\"]}]";

/* ── Knob page maps ───────────────────────────────────────────────────────── */
static const char *KNOB_KEYS[11][8] = {
    /* PAGE_ROOT — mirrors generate for chain_edit hover */
    {"seq1","seq2","seq3","seq4","syn1","syn2","syn3","syn4"},
    /* PAGE_GENERATE */
    {"seq1","seq2","seq3","seq4","syn1","syn2","syn3","syn4"},
    /* PAGE_PATCH — SameVoice menu-only, AllDecay on knob 8 */
    {"patch","rnd_patch","rnd_rytm","rnd_voices","rnd_mod","rnd_pitch","rnd_pan","all_decay"},
    /* PAGE_PARAMS (Sequences) */
    {"root","scale","density","chaos","gravity","clk_div","morse_spd","swing"},
    /* PAGE_MODULATION — Mod Offset menu-only */
    {"mod_amount","mod_speed","mod_freq","mod_decay","mod_pan","mod_density","mod_shape","mod_reset"},
    /* PAGE_MIX */
    {"v1_level","v2_level","v3_level","v4_level","v1_freq","v2_freq","v3_freq","v4_freq"},
    /* PAGE_VOICE1: Preset,Vol,Freq,Wave,Tone,Decay,Detune,Pan — Attack menu-only */
    {"v1_preset","v1_vol","v1_vfreq","v1_wave","v1_tone","v1_decay","v1_detune","v1_pan"},
    /* PAGE_VOICE2 */
    {"v2_preset","v2_vol","v2_vfreq","v2_wave","v2_tone","v2_decay","v2_detune","v2_pan"},
    /* PAGE_VOICE3 */
    {"v3_preset","v3_vol","v3_vfreq","v3_wave","v3_tone","v3_decay","v3_detune","v3_pan"},
    /* PAGE_VOICE4 */
    {"v4_preset","v4_vol","v4_vfreq","v4_wave","v4_tone","v4_decay","v4_detune","v4_pan"},
    /* PAGE_GENERAL — v0.2 reorganized: bpm K1 → master_vol K8, scene_morph K7 */
    {"bpm","drift","jitter","bit_crush","bit_rate","stereo_w","scene_morph","master_vol"}
};

/* ── get_param ────────────────────────────────────────────────────────────── */
static int get_param(void *instance, const char *key, char *buf, int buf_len) {
    signal_instance_t *inst = (signal_instance_t *)instance;
    if (!inst || !key || !buf || buf_len <= 0) return -1;

    if (strcmp(key, "name")         == 0) return snprintf(buf, buf_len, "Signal");
    if (strcmp(key, "chain_params") == 0) return snprintf(buf, buf_len, "%s", CHAIN_PARAMS_JSON);

    /* ui_hierarchy — Schwung calls this to discover page structure.
     * Without it, getComponentHierarchy() returns null and Schwung falls
     * back to the preset browser instead of showing the menu. */
    if (strcmp(key, "ui_hierarchy") == 0) {
        return snprintf(buf, buf_len,
            "{\"modes\":null,\"levels\":{"
            "\"root\":{\"name\":\"Signal\","
            "\"knobs\":[\"seq1\",\"seq2\",\"seq3\",\"seq4\",\"syn1\",\"syn2\",\"syn3\",\"syn4\"],"
            "\"params\":["
            "{\"level\":\"generate\",\"label\":\"Generate\"},"
            "{\"level\":\"patch\",\"label\":\"Patch\"},"
            "{\"level\":\"params\",\"label\":\"Sequences\"},"
            "{\"level\":\"modulation\",\"label\":\"Modulation\"},"
            "{\"level\":\"mix\",\"label\":\"Mix\"},"
            "{\"level\":\"voice1\",\"label\":\"Voice 1\"},"
            "{\"level\":\"voice2\",\"label\":\"Voice 2\"},"
            "{\"level\":\"voice3\",\"label\":\"Voice 3\"},"
            "{\"level\":\"voice4\",\"label\":\"Voice 4\"},"
            "{\"level\":\"general\",\"label\":\"General\"}"
            "]},"
            "\"generate\":{\"name\":\"Generate\","
            "\"knobs\":[\"seq1\",\"seq2\",\"seq3\",\"seq4\",\"syn1\",\"syn2\",\"syn3\",\"syn4\"],"
            "\"params\":[\"seq1\",\"seq2\",\"seq3\",\"seq4\",\"syn1\",\"syn2\",\"syn3\",\"syn4\"]},"
            "\"params\":{\"name\":\"Sequences\","
            "\"knobs\":[\"root\",\"scale\",\"density\",\"chaos\",\"gravity\",\"clk_div\",\"morse_spd\",\"swing\"],"
            "\"params\":[\"root\",\"scale\",\"density\",\"chaos\",\"gravity\",\"clk_div\",\"morse_spd\",\"swing\"]},"
            "\"modulation\":{\"name\":\"Modulation\","
            "\"knobs\":[\"mod_amount\",\"mod_speed\",\"mod_freq\",\"mod_decay\",\"mod_pan\",\"mod_density\",\"mod_shape\",\"mod_reset\"],"
            "\"params\":[\"mod_amount\",\"mod_speed\",\"mod_freq\",\"mod_decay\",\"mod_pan\",\"mod_density\",\"mod_shape\",\"mod_reset\",\"mod_offset\"]},"
            "\"patch\":{\"name\":\"Patch\","
            "\"knobs\":[\"patch\",\"rnd_patch\",\"rnd_rytm\",\"rnd_voices\",\"rnd_mod\",\"rnd_pitch\",\"rnd_pan\",\"all_decay\"],"
            "\"params\":[\"patch\",\"rnd_patch\",\"rnd_rytm\",\"rnd_voices\",\"rnd_mod\",\"rnd_pitch\",\"rnd_pan\",\"all_decay\",\"same_voice\"]},"
            "\"mix\":{\"name\":\"Mix\","
            "\"knobs\":[\"v1_level\",\"v2_level\",\"v3_level\",\"v4_level\",\"v1_freq\",\"v2_freq\",\"v3_freq\",\"v4_freq\"],"
            "\"params\":[\"v1_level\",\"v2_level\",\"v3_level\",\"v4_level\",\"v1_freq\",\"v2_freq\",\"v3_freq\",\"v4_freq\"]},"
            "\"voice1\":{\"name\":\"Voice 1\","
            "\"knobs\":[\"v1_preset\",\"v1_vol\",\"v1_vfreq\",\"v1_wave\",\"v1_tone\",\"v1_decay\",\"v1_detune\",\"v1_pan\"],"
            "\"params\":[\"v1_preset\",\"v1_vol\",\"v1_vfreq\",\"v1_wave\",\"v1_tone\",\"v1_decay\",\"v1_detune\",\"v1_pan\","
            "\"v1_attack\",\"v1_sub_div\",\"v1_sweep\",\"v1_tone_rnd\","
            "\"v1_improv\",\"v1_push_pull\",\"v1_bank_mode\","
            "\"v1_bank_pitch_0\",\"v1_bank_pitch_1\",\"v1_bank_pitch_2\",\"v1_bank_pitch_3\",\"v1_bank_pitch_4\","
            "\"v1_ghost_crescendo\"]},"
            "\"voice2\":{\"name\":\"Voice 2\","
            "\"knobs\":[\"v2_preset\",\"v2_vol\",\"v2_vfreq\",\"v2_wave\",\"v2_tone\",\"v2_decay\",\"v2_detune\",\"v2_pan\"],"
            "\"params\":[\"v2_preset\",\"v2_vol\",\"v2_vfreq\",\"v2_wave\",\"v2_tone\",\"v2_decay\",\"v2_detune\",\"v2_pan\","
            "\"v2_attack\",\"v2_sub_div\",\"v2_sweep\",\"v2_tone_rnd\","
            "\"v2_improv\",\"v2_push_pull\",\"v2_bank_mode\","
            "\"v2_bank_pitch_0\",\"v2_bank_pitch_1\",\"v2_bank_pitch_2\",\"v2_bank_pitch_3\",\"v2_bank_pitch_4\","
            "\"v2_ghost_crescendo\"]},"
            "\"voice3\":{\"name\":\"Voice 3\","
            "\"knobs\":[\"v3_preset\",\"v3_vol\",\"v3_vfreq\",\"v3_wave\",\"v3_tone\",\"v3_decay\",\"v3_detune\",\"v3_pan\"],"
            "\"params\":[\"v3_preset\",\"v3_vol\",\"v3_vfreq\",\"v3_wave\",\"v3_tone\",\"v3_decay\",\"v3_detune\",\"v3_pan\","
            "\"v3_attack\",\"v3_sub_div\",\"v3_sweep\",\"v3_tone_rnd\","
            "\"v3_improv\",\"v3_push_pull\",\"v3_bank_mode\","
            "\"v3_bank_pitch_0\",\"v3_bank_pitch_1\",\"v3_bank_pitch_2\",\"v3_bank_pitch_3\",\"v3_bank_pitch_4\","
            "\"v3_ghost_crescendo\"]},"
            "\"voice4\":{\"name\":\"Voice 4\","
            "\"knobs\":[\"v4_preset\",\"v4_vol\",\"v4_vfreq\",\"v4_wave\",\"v4_tone\",\"v4_decay\",\"v4_detune\",\"v4_pan\"],"
            "\"params\":[\"v4_preset\",\"v4_vol\",\"v4_vfreq\",\"v4_wave\",\"v4_tone\",\"v4_decay\",\"v4_detune\",\"v4_pan\","
            "\"v4_attack\",\"v4_sub_div\",\"v4_sweep\",\"v4_tone_rnd\","
            "\"v4_improv\",\"v4_push_pull\",\"v4_bank_mode\","
            "\"v4_bank_pitch_0\",\"v4_bank_pitch_1\",\"v4_bank_pitch_2\",\"v4_bank_pitch_3\",\"v4_bank_pitch_4\","
            "\"v4_ghost_crescendo\"]},"
            "\"general\":{\"name\":\"General\","
            "\"knobs\":[\"bpm\",\"drift\",\"jitter\",\"bit_crush\",\"bit_rate\",\"stereo_w\",\"scene_morph\",\"master_vol\"],"
            "\"params\":[\"bpm\",\"drift\",\"jitter\",\"bit_crush\",\"bit_rate\",\"stereo_w\",\"scene_morph\",\"master_vol\","
            "\"tempo_sync\",\"dc_filter\",\"out_mode\","
            "\"save_scene_a\",\"save_scene_b\",\"morph_curve\",\"morph_smooth\","
            "\"phrase_length\",\"fill_shape\",\"step_grid\",\"retrig_rate\",\"drummer_brain\"]}"
            "}}");
    }

    /* Knob overlay (page-aware) */
    if (strncmp(key, "knob_", 5) == 0) {
        int kn = atoi(key + 5) - 1; /* 0-based */
        if (kn < 0 || kn > 7) return -1;
        int pg = inst->current_page;
        if (pg < 0 || pg > 10) pg = 0;
        const char *kkey = KNOB_KEYS[pg][kn];
        if (strstr(key, "_name"))
            return snprintf(buf, buf_len, "%s", kkey);
        if (strstr(key, "_value"))
            return get_param(inst, kkey, buf, buf_len);
        return -1;
    }

    char k[24];

    /* Page 1 */
    for (int v = 0; v < NUM_VOICES; v++) {
        snprintf(k, sizeof(k), "seq%d", v + 1);
        if (strcmp(key, k) == 0) return snprintf(buf, buf_len, "%d", inst->voices[v].seq_preset);
        snprintf(k, sizeof(k), "syn%d", v + 1);
        if (strcmp(key, k) == 0) return snprintf(buf, buf_len, "%d", inst->voices[v].syn_preset);
    }

    /* Page 2 */
    for (int v = 0; v < NUM_VOICES; v++) {
        snprintf(k, sizeof(k), "v%d_level", v + 1);
        if (strcmp(key, k) == 0) return snprintf(buf, buf_len, "%.4f", inst->voices[v].level);
        snprintf(k, sizeof(k), "v%d_freq", v + 1);
        if (strcmp(key, k) == 0) return snprintf(buf, buf_len, "%.2f", inst->voices[v].freq);
    }

    /* Page 3 */
    if (strcmp(key, "root")      == 0) return snprintf(buf, buf_len, "%s", ROOT_NAMES[inst->root]);
    if (strcmp(key, "scale")     == 0) return snprintf(buf, buf_len, "%s", SCALE_NAMES[inst->scale]);
    if (strcmp(key, "density")   == 0) return snprintf(buf, buf_len, "%.4f", inst->density);
    if (strcmp(key, "chaos")     == 0) return snprintf(buf, buf_len, "%.4f", inst->chaos);
    if (strcmp(key, "gravity")   == 0) return snprintf(buf, buf_len, "%.4f", inst->gravity);
    if (strcmp(key, "clk_div")   == 0) return snprintf(buf, buf_len, "%d", inst->clk_div);
    if (strcmp(key, "morse_spd") == 0) return snprintf(buf, buf_len, "%.4f", inst->morse_spd);
    if (strcmp(key, "swing")     == 0) return snprintf(buf, buf_len, "%.4f", inst->swing);

    /* Pages 4–7 */
    for (int v = 0; v < NUM_VOICES; v++) {
        const voice_t *vp = &inst->voices[v];
        snprintf(k, sizeof(k), "v%d_preset", v + 1);
        if (strcmp(key, k) == 0) return snprintf(buf, buf_len, "%d", vp->syn_preset);
        snprintf(k, sizeof(k), "v%d_vol", v + 1);
        if (strcmp(key, k) == 0) return snprintf(buf, buf_len, "%.4f", vp->vol);
        snprintf(k, sizeof(k), "v%d_vfreq", v + 1);
        if (strcmp(key, k) == 0) return snprintf(buf, buf_len, "%.2f", vp->freq);
        snprintf(k, sizeof(k), "v%d_wave", v + 1);
        if (strcmp(key, k) == 0) return snprintf(buf, buf_len, "%s", WAVE_NAMES[vp->wave]);
        snprintf(k, sizeof(k), "v%d_tone", v + 1);
        if (strcmp(key, k) == 0) return snprintf(buf, buf_len, "%.4f", vp->tone);
        snprintf(k, sizeof(k), "v%d_attack", v + 1);
        if (strcmp(key, k) == 0) return snprintf(buf, buf_len, "%.4f", vp->attack);
        snprintf(k, sizeof(k), "v%d_decay", v + 1);
        if (strcmp(key, k) == 0) return snprintf(buf, buf_len, "%.4f", vp->decay);
        snprintf(k, sizeof(k), "v%d_pan", v + 1);
        if (strcmp(key, k) == 0) return snprintf(buf, buf_len, "%.4f", vp->pan);
        snprintf(k, sizeof(k), "v%d_sweep", v + 1);
        if (strcmp(key, k) == 0) return snprintf(buf, buf_len, "%.4f", vp->sweep);
        snprintf(k, sizeof(k), "v%d_detune", v + 1);
        if (strcmp(key, k) == 0) return snprintf(buf, buf_len, "%.4f", vp->detune);
        snprintf(k, sizeof(k), "v%d_sub_div", v + 1);
        if (strcmp(key, k) == 0) return snprintf(buf, buf_len, "%d", vp->sub_div);
        snprintf(k, sizeof(k), "v%d_tone_rnd", v + 1);
        if (strcmp(key, k) == 0) return snprintf(buf, buf_len, "%.0f%%", vp->tone_rnd * 100.0f);
    }

    /* Patch page — momentary action knobs always read back "0" (the wide-range
     * int chain_params is just the mechanism for getting a fresh set_param
     * event on every right-turn; the user-facing value is always idle/0). */
    if (strcmp(key, "patch")      == 0) return snprintf(buf, buf_len, "%s", PATCH_NAMES[inst->patch_idx]);
    if (strcmp(key, "rnd_patch")  == 0) return snprintf(buf, buf_len, "0");
    if (strcmp(key, "rnd_rytm")   == 0) return snprintf(buf, buf_len, "0");
    if (strcmp(key, "rnd_voices") == 0) return snprintf(buf, buf_len, "0");
    if (strcmp(key, "same_voice") == 0) return snprintf(buf, buf_len, "0");
    if (strcmp(key, "rnd_mod")    == 0) return snprintf(buf, buf_len, "0");
    if (strcmp(key, "rnd_pitch")  == 0) return snprintf(buf, buf_len, "0");
    if (strcmp(key, "rnd_pan")    == 0) return snprintf(buf, buf_len, "0");

    /* Modulation */
    if (strcmp(key, "all_decay")   == 0) return snprintf(buf, buf_len, "%.4f", inst->all_decay);
    if (strcmp(key, "mod_amount")  == 0) return snprintf(buf, buf_len, "%.4f", inst->mod_amount);
    if (strcmp(key, "mod_speed")   == 0) return snprintf(buf, buf_len, "%.4f", inst->mod_speed);
    if (strcmp(key, "mod_offset")  == 0) return snprintf(buf, buf_len, "%.4f", inst->mod_offset);
    if (strcmp(key, "mod_freq")    == 0) return snprintf(buf, buf_len, "%.4f", inst->mod_freq);
    if (strcmp(key, "mod_decay")   == 0) return snprintf(buf, buf_len, "%.4f", inst->mod_decay);
    if (strcmp(key, "mod_pan")     == 0) return snprintf(buf, buf_len, "%.4f", inst->mod_pan);
    if (strcmp(key, "mod_density") == 0) return snprintf(buf, buf_len, "%.4f", inst->mod_density);
    if (strcmp(key, "mod_shape")   == 0) return snprintf(buf, buf_len, "%s", MOD_SHAPE_NAMES[inst->mod_shape]);
    if (strcmp(key, "mod_reset")   == 0) return snprintf(buf, buf_len, "0");

    /* Page 8 */
    if (strcmp(key, "master_vol") == 0) return snprintf(buf, buf_len, "%.4f", inst->master_vol);
    if (strcmp(key, "tempo_sync") == 0) return snprintf(buf, buf_len, "%s", inst->tempo_sync ? "Sync" : "Free");
    if (strcmp(key, "bpm")        == 0) return snprintf(buf, buf_len, "%.1f", inst->bpm);
    if (strcmp(key, "stereo_w")   == 0) return snprintf(buf, buf_len, "%.4f", inst->stereo_w);
    if (strcmp(key, "bit_crush")  == 0) return snprintf(buf, buf_len, "%d", inst->bit_crush);
    if (strcmp(key, "bit_rate")   == 0) return snprintf(buf, buf_len, "%.4f", inst->bit_rate);
    if (strcmp(key, "drift")      == 0) return snprintf(buf, buf_len, "%.4f", inst->drift);
    if (strcmp(key, "jitter")     == 0) return snprintf(buf, buf_len, "%.4f", inst->jitter);
    if (strcmp(key, "dc_filter")  == 0) return snprintf(buf, buf_len, "%s", inst->dc_filter ? "On" : "Off");
    if (strcmp(key, "out_mode")   == 0) {
        static const char *modes[] = {"Stereo","Mono","Spread"};
        return snprintf(buf, buf_len, "%s", modes[inst->out_mode]);
    }

    /* ── v0.2 new params ── */
    if (strcmp(key, "scene_morph")   == 0) return snprintf(buf, buf_len, "%.4f", inst->scene_morph);
    if (strcmp(key, "save_scene_a")  == 0) return snprintf(buf, buf_len, "0");
    if (strcmp(key, "save_scene_b")  == 0) return snprintf(buf, buf_len, "0");
    if (strcmp(key, "morph_curve")   == 0) return snprintf(buf, buf_len, "%s", MORPH_CURVE_NAMES[inst->morph_curve]);
    if (strcmp(key, "morph_smooth")  == 0) return snprintf(buf, buf_len, "%.1f", inst->morph_smooth_ms);
    if (strcmp(key, "phrase_length") == 0) return snprintf(buf, buf_len, "%s", PHRASE_NAMES[inst->phrase_length]);
    if (strcmp(key, "fill_shape")    == 0) return snprintf(buf, buf_len, "%s", FILL_NAMES[inst->fill_shape]);
    if (strcmp(key, "step_grid")     == 0) return snprintf(buf, buf_len, "%s", STEP_GRID_NAMES[inst->step_grid]);
    if (strcmp(key, "retrig_rate")   == 0) return snprintf(buf, buf_len, "%s", RETRIG_NAMES[inst->retrig_rate]);
    if (strcmp(key, "drummer_brain") == 0) return snprintf(buf, buf_len, "%s", DRUMMER_BRAIN_NAMES[inst->drummer_brain]);

    /* Per-voice Holzman params */
    for (int v = 0; v < NUM_VOICES; v++) {
        const voice_t *vp = &inst->voices[v];
        snprintf(k, sizeof(k), "v%d_improv", v + 1);
        if (strcmp(key, k) == 0) return snprintf(buf, buf_len, "%.4f", vp->improv);
        snprintf(k, sizeof(k), "v%d_push_pull", v + 1);
        if (strcmp(key, k) == 0) return snprintf(buf, buf_len, "%.2f", vp->push_pull);
        snprintf(k, sizeof(k), "v%d_bank_mode", v + 1);
        if (strcmp(key, k) == 0) return snprintf(buf, buf_len, "%s", BANK_MODE_NAMES[vp->bank_mode]);
        snprintf(k, sizeof(k), "v%d_ghost_crescendo", v + 1);
        if (strcmp(key, k) == 0) return snprintf(buf, buf_len, "%s", vp->ghost_crescendo ? "On" : "Off");
        for (int b = 0; b < 5; b++) {
            snprintf(k, sizeof(k), "v%d_bank_pitch_%d", v + 1, b);
            if (strcmp(key, k) == 0) return snprintf(buf, buf_len, "%.2f", vp->bank_pitch[b]);
        }
    }

    /* State serialization (key=val;key=val;...) */
    if (strcmp(key, "state") == 0) {
        int n = 0;
        int rem;
#define STATE_W(fmt, ...) \
    do { rem = buf_len - n - 1; \
         if (rem <= 0) return n; \
         n += snprintf(buf + n, (size_t)(rem), fmt, ##__VA_ARGS__); } while(0)

        STATE_W("density=%.4f;chaos=%.4f;gravity=%.4f;", inst->density, inst->chaos, inst->gravity);
        STATE_W("clk_div=%d;morse_spd=%.4f;swing=%.4f;", inst->clk_div, inst->morse_spd, inst->swing);
        STATE_W("master_vol=%.4f;stereo_w=%.4f;bit_crush=%d;bit_rate=%.4f;bpm=%.1f;", inst->master_vol, inst->stereo_w, inst->bit_crush, inst->bit_rate, inst->bpm);
        STATE_W("drift=%.4f;jitter=%.4f;", inst->drift, inst->jitter);
        STATE_W("mod_amount=%.4f;mod_speed=%.4f;mod_offset=%.4f;mod_freq=%.4f;mod_decay=%.4f;mod_pan=%.4f;mod_density=%.4f;mod_shape=%s;",
            inst->mod_amount,
            inst->mod_speed, inst->mod_offset, inst->mod_freq, inst->mod_decay,
            inst->mod_pan, inst->mod_density, MOD_SHAPE_NAMES[inst->mod_shape]);
        STATE_W("root=%s;scale=%s;tempo_sync=%s;", ROOT_NAMES[inst->root], SCALE_NAMES[inst->scale], inst->tempo_sync ? "Sync" : "Free");
        /* v0.2 globals */
        STATE_W("scene_morph=%.4f;morph_curve=%s;morph_smooth=%.1f;",
            inst->scene_morph, MORPH_CURVE_NAMES[inst->morph_curve], inst->morph_smooth_ms);
        STATE_W("phrase_length=%s;fill_shape=%s;step_grid=%s;retrig_rate=%s;drummer_brain=%s;",
            PHRASE_NAMES[inst->phrase_length], FILL_NAMES[inst->fill_shape],
            STEP_GRID_NAMES[inst->step_grid], RETRIG_NAMES[inst->retrig_rate],
            DRUMMER_BRAIN_NAMES[inst->drummer_brain]);
        for (int v = 0; v < NUM_VOICES; v++) {
            const voice_t *vp = &inst->voices[v];
            STATE_W("seq%d=%d;syn%d=%d;v%d_vol=%.4f;v%d_vfreq=%.2f;v%d_wave=%s;",
                v+1, vp->seq_preset, v+1, vp->syn_preset,
                v+1, vp->vol, v+1, vp->freq, v+1, WAVE_NAMES[vp->wave]);
            STATE_W("v%d_tone=%.4f;v%d_attack=%.4f;v%d_decay=%.4f;v%d_pan=%.4f;v%d_sweep=%.4f;v%d_detune=%.4f;v%d_sub_div=%d;",
                v+1, vp->tone, v+1, vp->attack, v+1, vp->decay, v+1, vp->pan,
                v+1, vp->sweep, v+1, vp->detune, v+1, vp->sub_div);
            STATE_W("v%d_level=%.4f;v%d_freq=%.2f;",
                v+1, vp->level, v+1, vp->freq);
            STATE_W("v%d_improv=%.4f;v%d_push_pull=%.2f;v%d_bank_mode=%s;v%d_ghost_crescendo=%s;",
                v+1, vp->improv, v+1, vp->push_pull,
                v+1, BANK_MODE_NAMES[vp->bank_mode],
                v+1, vp->ghost_crescendo ? "On" : "Off");
            for (int b = 0; b < 5; b++)
                STATE_W("v%d_bank_pitch_%d=%.2f;", v+1, b, vp->bank_pitch[b]);
        }
#undef STATE_W
        return n;
    }

    return -1;
}

/* ── render_block ─────────────────────────────────────────────────────────── */
static void render_block(void *instance, int16_t *out_lr, int frames) {
    signal_instance_t *inst = (signal_instance_t *)instance;
    if (!inst) { memset(out_lr, 0, frames * 4); return; }

    float bpm = inst->bpm > 0 ? inst->bpm : 120.0f;
    float samples_per_beat = SAMPLE_RATE * 60.0f / bpm;
    int   sync = inst->tempo_sync;

    /* ── Scene morph: smooth knob value, apply between scene A and B ── */
    {
        float target = clampf(inst->scene_morph, 0.0f, 1.0f);
        float smooth_ms = inst->morph_smooth_ms;
        float coeff;
        if (smooth_ms <= 0.5f) {
            coeff = 1.0f; /* instant */
        } else {
            coeff = 1.0f - expf(-(float)frames / (smooth_ms * 0.001f * SAMPLE_RATE));
        }
        inst->scene_morph_smoothed += coeff * (target - inst->scene_morph_smoothed);
        if (fabsf(inst->scene_morph_smoothed - inst->last_applied_morph) > 1e-4f) {
            apply_scene_morph(inst, inst->scene_morph_smoothed);
            inst->last_applied_morph = inst->scene_morph_smoothed;
        }
    }

    /* ── Tick down momentary-action flash counters (one per render block) ── */
    for (int a = 0; a < NUM_ACTIONS; a++) {
        if (inst->action_flash[a] > 0) inst->action_flash[a]--;
    }

    for (int i = 0; i < frames; i++) {
        float mix_l = 0.0f, mix_r = 0.0f;

        /* ── Advance per-voice LFOs ── */
        if (inst->mod_speed > 0.0f) {
            float mod_hz   = inst->mod_speed * 10.0f;
            float phase_inc = mod_hz * SR_INV;
            for (int vl = 0; vl < NUM_VOICES; vl++) {
                float prev = inst->vlfo_phase[vl];
                inst->vlfo_phase[vl] += phase_inc;
                if (inst->vlfo_phase[vl] >= 1.0f) {
                    inst->vlfo_phase[vl] -= 1.0f;
                    /* S&H: new random value on zero-crossing */
                    if (inst->vlfo_shape[vl] == MOD_SHAPE_SH) {
                        uint32_t r = xorshift32(&inst->vlfo_rng[vl]);
                        inst->vlfo_sh_val[vl] = (float)(int32_t)r / 2147483648.0f;
                    }
                }
                (void)prev;
                float eff = inst->vlfo_phase[vl]
                          + (float)vl * inst->mod_offset / (float)NUM_VOICES;
                while (eff >= 1.0f) eff -= 1.0f;
                inst->vlfo_value[vl] = mod_lfo_value(eff, inst->vlfo_shape[vl],
                                           &inst->vlfo_sh_val[vl], &inst->vlfo_rng[vl]);
            }
        } else {
            for (int vl = 0; vl < NUM_VOICES; vl++) inst->vlfo_value[vl] = 0.0f;
        }

        for (int v = 0; v < NUM_VOICES; v++) {
            voice_t *vp = &inst->voices[v];
            float lfo_v = inst->vlfo_value[v] * inst->mod_amount;

            /* Skip if sequencer or synth off */
            if (vp->seq_preset == 0 || vp->syn_preset == 0) continue;

            /* ── Free-running clock (used when NOT sync'd) ── */
            if (!sync) {
                if (seq_is_breakbeat(vp->seq_preset)) {
                    /* Breakbeat path: anchor-curve stochastic step trigger */
                    int step_grid_value = STEP_GRID_VALUES[inst->step_grid];
                    int sub = step_grid_value / 4;
                    if (sub < 1) sub = 1;
                    float sps = samples_per_beat / (float)sub / (float)inst->clk_div;
                    if (sps < 1.0f) sps = 1.0f;

                    float swing_off = 0.0f;
                    if ((vp->bb_step & 1) && inst->swing > 0.0f)
                        swing_off = sps * inst->swing * 0.5f;
                    float push_off = 0.0f;
                    if ((vp->bb_step & 1) && vp->push_pull != 0.0f) {
                        push_off = vp->push_pull * SAMPLE_RATE * 0.001f;
                        if (push_off < -(sps - 1.0f)) push_off = -(sps - 1.0f);
                    }

                    vp->step_accum += 1.0f;
                    if (vp->step_accum >= sps + swing_off + push_off) {
                        vp->step_accum -= (sps + swing_off + push_off);
                        vp->bb_step++;
                        if (vp->bb_step >= step_grid_value) {
                            vp->bb_step = 0;
                            vp->bb_bar_index++;
                        }
                        int is_fill = 0;
                        breakbeat_advance(inst, v, &is_fill);
                        if (vp->sparsity_lockout > 0) vp->sparsity_lockout--;
                        if (vp->sparsity_lockout == 0) {
                            float weight = 0.0f;
                            float ghost_vel = 1.0f;
                            int fire = breakbeat_step_decide(inst, v, is_fill, &weight, &ghost_vel);
                            if (fire) {
                                float prob = inst->density;
                                if (inst->mod_density > 0.0f) {
                                    float lnorm = (lfo_v + 1.0f) * 0.5f;
                                    prob *= (1.0f - inst->mod_density + inst->mod_density * lnorm);
                                }
                                prob = clampf(prob, 0.0f, 1.0f);
                                if (randf(&inst->rng) < prob) {
                                    if (inst->gravity > 0.0f && randf(&inst->rng) < inst->gravity) {
                                        for (int ov = 0; ov < NUM_VOICES; ov++) {
                                            if (ov != v) inst->voices[ov].step_accum = 0.0f;
                                        }
                                    }
                                    if (inst->jitter > 0.0f)
                                        vp->step_accum += (randf(&inst->rng) - 0.5f) * sps * inst->jitter * 0.5f;
                                    /* Snare bank pitch offset for this hit */
                                    vp->bb_pitch_offset = bb_pick_bank_offset(vp, weight);
                                    voice_trigger(vp);
                                    /* Ghost crescendo velocity ramp */
                                    vp->vel = ghost_vel;
                                    if (vp->tone_rnd > 0.0f)
                                        vp->tone_eff = clampf(vp->tone + (randf(&inst->rng) - 0.5f) * 2.0f * vp->tone_rnd, 0.0f, 1.0f);
                                    if (inst->sparsity > 0.0f)
                                        vp->sparsity_lockout = 1 + (int)(inst->sparsity * 31.0f);
                                    /* Retrigger stutter burst */
                                    int bb_idx_l = seq_to_bb_index(vp->seq_preset);
                                    if (bb_idx_l >= 0 && bb_idx_l < NUM_BREAKBEAT_PRESETS) {
                                        float retrig_p = BREAKBEAT_PRESETS[bb_idx_l].retrigger;
                                        if (retrig_p > 0.0f && randf(&vp->bb_rng) < retrig_p) {
                                            int rate;
                                            if (inst->retrig_rate == RETRIG_RAND) {
                                                static const int rand_rates[4] = {2, 3, 4, 8};
                                                rate = rand_rates[(int)(randf(&vp->bb_rng) * 4.0f) & 3];
                                            } else if (inst->retrig_rate >= 0 && inst->retrig_rate < RETRIG_RAND) {
                                                rate = RETRIG_RATES[inst->retrig_rate];
                                            } else {
                                                rate = 2;
                                            }
                                            if (rate < 2) rate = 2;
                                            vp->bb_retrig_period = sps / (float)rate;
                                            if (vp->bb_retrig_period < 64.0f) vp->bb_retrig_period = 64.0f;
                                            vp->bb_retrig_count = rate - 1;
                                            vp->bb_retrig_accum = 0.0f;
                                        }
                                    }
                                }
                            }
                        }
                    }
                } else {
                    /* Pattern path (existing presets 1-25) */
                    int idx = vp->seq_preset - 1;
                    if (idx >= 0 && idx < NUM_PATTERN_PRESETS) {
                        const rhythm_pattern_t *pat = &PATTERNS[v][idx];

                        float spd_mult = 1.0f;
                        if (idx < 5) spd_mult = 0.25f + inst->morse_spd * 1.75f;

                        float sps = samples_per_beat / (float)pat->subdivision
                                  / (float)inst->clk_div * spd_mult;
                        if (sps < 1.0f) sps = 1.0f;

                        float swing_off = 0.0f;
                        if ((vp->step & 1) && inst->swing > 0.0f)
                            swing_off = sps * inst->swing * 0.5f;

                        vp->step_accum += 1.0f;
                        if (vp->step_accum >= sps + swing_off) {
                            vp->step_accum -= (sps + swing_off);
                            vp->step = (vp->step + 1) % pat->length;

                            if (vp->sparsity_lockout > 0) vp->sparsity_lockout--;

                            if (pat->hits[vp->step] && vp->sparsity_lockout == 0) {
                                float prob = inst->density;
                                if (inst->mod_density > 0.0f) {
                                    float lnorm = (lfo_v + 1.0f) * 0.5f;
                                    prob *= (1.0f - inst->mod_density + inst->mod_density * lnorm);
                                }
                                if (inst->chaos > 0.0f)
                                    prob += (randf(&inst->rng) - 0.5f) * inst->chaos;
                                prob = clampf(prob, 0.0f, 1.0f);
                                if (randf(&inst->rng) < prob) {
                                    if (inst->gravity > 0.0f && randf(&inst->rng) < inst->gravity) {
                                        for (int ov = 0; ov < NUM_VOICES; ov++) {
                                            if (ov != v) inst->voices[ov].step_accum = 0.0f;
                                        }
                                    }
                                    if (inst->jitter > 0.0f)
                                        vp->step_accum += (randf(&inst->rng) - 0.5f) * sps * inst->jitter * 0.5f;
                                    voice_trigger(vp);
                                    if (vp->tone_rnd > 0.0f)
                                        vp->tone_eff = clampf(vp->tone + (randf(&inst->rng) - 0.5f) * 2.0f * vp->tone_rnd, 0.0f, 1.0f);
                                    if (inst->sparsity > 0.0f)
                                        vp->sparsity_lockout = 1 + (int)(inst->sparsity * 31.0f);
                                }
                            }
                        }
                    }
                }
            }

            /* ── Retrigger continuation (breakbeat stutter bursts) ── */
            if (vp->bb_retrig_count > 0) {
                vp->bb_retrig_accum += 1.0f;
                if (vp->bb_retrig_accum >= vp->bb_retrig_period) {
                    vp->bb_retrig_accum -= vp->bb_retrig_period;
                    voice_trigger(vp);
                    vp->vel = 0.7f; /* softer than initial hit */
                    vp->bb_retrig_count--;
                    if (vp->tone_rnd > 0.0f)
                        vp->tone_eff = clampf(vp->tone + (randf(&inst->rng) - 0.5f) * 2.0f * vp->tone_rnd, 0.0f, 1.0f);
                }
            }

            /* ── Envelope (with LFO decay mod) ── */
            /* Decay mod: LFO scales decay from voice value up to max (0.5s).
             * all_decay extends base toward max; mod_decay LFO-modulates around that.
             * lfo_v=0 (mod_amount=0) → no modulation, only all_decay offset. */
            float base_decay = vp->decay + inst->all_decay * (0.5f - vp->decay);
            float decay_t = base_decay;
            if (inst->mod_decay > 0.0f) {
                /* Direct: lfo_v -1..+1 scales ±depth around base_decay */
                float delta = lfo_v * inst->mod_decay * (0.5f - base_decay);
                decay_t = clampf(base_decay + delta, 0.0001f, 0.5f);
            }

            if (vp->env_stage == 0) {
                float rate = 1.0f / (vp->attack * SAMPLE_RATE);
                vp->env += rate;
                if (vp->env >= 1.0f) { vp->env = 1.0f; vp->env_stage = 1; }
            } else if (vp->env_stage == 1) {
                float rate = 1.0f / (decay_t * SAMPLE_RATE);
                vp->env -= vp->env * rate * 4.0f;
                if (vp->env < 0.0001f) {
                    vp->env = 0.0f; vp->env_stage = -1;
                    vp->hit_sub_div = 0; /* clear row override when note ends */
                }
            }

            if (vp->env <= 0.0f) continue;

            /* ── Frequency: sub_div (row override > knob) → bank pitch → sweep → LFO mod → drift → quantize ── */
            int eff_sub_div = (vp->hit_sub_div > 0) ? vp->hit_sub_div : vp->sub_div;
            float freq = vp->freq / (float)eff_sub_div;

            /* Snare bank pitch offset (semitones), applied for the duration of this hit */
            if (vp->bb_pitch_offset != 0.0f)
                freq *= powf(2.0f, vp->bb_pitch_offset / 12.0f);

            /* Sweep: pitch arc peaks at 2^sweep octaves up when env=1 */
            if (vp->sweep > 0.0f)
                freq *= powf(2.0f, vp->sweep * vp->env);

            /* LFO frequency modulation (±2 octaves at mod_freq=1) */
            if (inst->mod_freq > 0.0f)
                freq *= powf(2.0f, lfo_v * inst->mod_freq * 2.0f);

            if (inst->drift > 0.0f)
                freq *= 1.0f + (randf(&inst->rng) - 0.5f) * inst->drift * 0.005f;

            freq = quantize_freq(freq, inst->root, inst->scale);
            freq = clampf(freq, 20.0f, 20000.0f);

            /* ── Generate primary sample ── */
            float sample = voice_generate(vp, freq);

            /* ── Heterodyne: blend second detuned sine for beating ── */
            if (vp->detune > 0.0f) {
                float freq2 = freq + vp->detune;
                float p2inc = freq2 * SR_INV;
                float s2 = sinf(vp->phase2 * TWO_PI);
                vp->phase2 += p2inc;
                if (vp->phase2 >= 1.0f) vp->phase2 -= 1.0f;
                sample = (sample + s2) * 0.5f;
            }

            /* ── Tone LP filter: smooth toward tone_eff (includes per-event randomization) ── */
            vp->tone_smooth += 0.0226f * (vp->tone_eff - vp->tone_smooth);
            float lp_coeff = vp->tone_smooth * 0.999f + 0.001f;
            vp->lp_state += lp_coeff * (sample - vp->lp_state) + 1e-20f;
            sample = vp->lp_state;

            /* ── Sub-threshold saturation (Emptyset harmonic onset) ── */
            if (vp->sat > 0.0f) {
                float k = 1.0f + vp->sat * 5.0f;
                sample = tanhf(sample * k) / k;
            }

            /* ── Apply envelope + volume + pad velocity ── */
            sample *= vp->env * vp->vol * vp->level * vp->vel;

            /* ── Constant-power pan (with LFO mod) ── */
            float pan = clampf(vp->pan + lfo_v * inst->mod_pan, -1.0f, 1.0f);
            float angle = (pan + 1.0f) * 0.25f * 3.14159265f;
            mix_l += sample * cosf(angle);
            mix_r += sample * sinf(angle);
        }

        /* Scale for 4 voices */
        mix_l *= 0.35f;
        mix_r *= 0.35f;

        /* Master volume */
        mix_l *= inst->master_vol;
        mix_r *= inst->master_vol;

        /* Output mode */
        if (inst->out_mode == 1) { /* Mono */
            float m = (mix_l + mix_r) * 0.5f;
            mix_l = mix_r = m;
        } else if (inst->out_mode == 2) { /* Spread: M/S widen */
            float mid  = (mix_l + mix_r) * 0.5f;
            float side = (mix_l - mix_r) * 0.5f * 2.0f; /* amplify sides */
            mix_l = mid + side;
            mix_r = mid - side;
        }

        /* Stereo width (M/S) */
        if (inst->stereo_w != 0.5f) {
            float w    = inst->stereo_w * 2.0f;          /* 0=mono, 2=full wide */
            float mid  = (mix_l + mix_r) * 0.5f;
            float side = (mix_l - mix_r) * 0.5f;
            mix_l = mid + side * w;
            mix_r = mid - side * w;
        }

        /* Bit crusher (bit depth) */
        if (inst->bit_crush < 16) {
            float levels = (float)(1 << inst->bit_crush);
            mix_l = floorf(mix_l * levels + 0.5f) / levels;
            mix_r = floorf(mix_r * levels + 0.5f) / levels;
        }

        /* Bit rate (sample-rate decimation: hold 1–44 samples → 44100–1000 Hz) */
        if (inst->bit_rate > 0.0f) {
            float hold = 1.0f + inst->bit_rate * 43.0f;
            inst->bit_rate_accum += 1.0f;
            if (inst->bit_rate_accum >= hold) {
                inst->bit_rate_accum -= hold;
                inst->bit_rate_hold_l = mix_l;
                inst->bit_rate_hold_r = mix_r;
            }
            mix_l = inst->bit_rate_hold_l;
            mix_r = inst->bit_rate_hold_r;
        }

        /* DC blocker (1-pole highpass ~20 Hz) */
        if (inst->dc_filter) {
            static const float DC_COEFF = 0.99956f; /* ~20 Hz at 44100 */
            static const float DC_NORM  = 0.5f * (1.0f + DC_COEFF);
            float xl = mix_l, xr = mix_r;
            float nl = xl + DC_COEFF * inst->dc_x[0];
            float nr = xr + DC_COEFF * inst->dc_x[1];
            mix_l = DC_NORM * (nl - inst->dc_x[0]);
            mix_r = DC_NORM * (nr - inst->dc_x[1]);
            inst->dc_x[0] = nl; inst->dc_x[1] = nr;
            /* Denormal protection */
            if (fabsf(inst->dc_x[0]) < 1e-20f) inst->dc_x[0] = 0.0f;
            if (fabsf(inst->dc_x[1]) < 1e-20f) inst->dc_x[1] = 0.0f;
        }

        /* Soft limiter: transparent below ±0.85, prevents harsh digital overload */
        if (mix_l >  0.85f) mix_l =  0.85f + (mix_l - 0.85f) / (1.0f + 10.0f * (mix_l - 0.85f));
        if (mix_l < -0.85f) mix_l = -0.85f + (mix_l + 0.85f) / (1.0f - 10.0f * (mix_l + 0.85f));
        if (mix_r >  0.85f) mix_r =  0.85f + (mix_r - 0.85f) / (1.0f + 10.0f * (mix_r - 0.85f));
        if (mix_r < -0.85f) mix_r = -0.85f + (mix_r + 0.85f) / (1.0f - 10.0f * (mix_r + 0.85f));

        /* Write */
        out_lr[i * 2]     = (int16_t)(clampf(mix_l, -1.0f, 1.0f) * 32767.0f);
        out_lr[i * 2 + 1] = (int16_t)(clampf(mix_r, -1.0f, 1.0f) * 32767.0f);
    }
}

/* ── Plugin API export ────────────────────────────────────────────────────── */
plugin_api_v2_t *move_plugin_init_v2(const void *host) {
    (void)host;
    static plugin_api_v2_t api = {
        .api_version      = 2,
        .create_instance  = create_instance,
        .destroy_instance = destroy_instance,
        .on_midi          = on_midi,
        .set_param        = set_param,
        .get_param        = get_param,
        .get_error        = NULL,
        .render_block     = render_block,
    };
    return &api;
}
