/**
 * PocketDAW Plugin: Drum Machine
 * 
 * Synthesized drum sounds mapped to MIDI notes:
 *   C2 (36) = Kick
 *   D2 (38) = Snare
 *   F#2 (42) = Closed Hi-Hat
 *   A#2 (46) = Open Hi-Hat
 *   C#2 (37) = Rimshot
 *   D#2 (39) = Clap
 *   G#2 (44) = Pedal Hi-Hat
 *   A2 (45) = Tom
 * 
 * Each drum is a synthesis recipe (sine + noise + pitch envelope).
 * 8 simultaneous sounds, no polyphony needed per drum.
 */
#include "pdsynth_api.h"
#include <SDL2/SDL.h>
#include "pd_text.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define MAX_HITS 8
#define TWO_PI 6.28318530718f

enum {
    P_KICK_TUNE = 0,
    P_KICK_DECAY,
    P_SNARE_TONE,
    P_SNARE_SNAP,
    P_HAT_TONE,
    P_HAT_DECAY,
    P_MASTER_DRIVE,
    P_ROOM,
    PARAM_COUNT
};

typedef enum {
    DRUM_KICK = 0,
    DRUM_SNARE,
    DRUM_CLOSED_HAT,
    DRUM_OPEN_HAT,
    DRUM_RIM,
    DRUM_CLAP,
    DRUM_PEDAL_HAT,
    DRUM_TOM,
    DRUM_COUNT
} DrumType;

typedef struct {
    DrumType type;
    float phase;
    float time;      // Time since trigger (seconds)
    float velocity;
    int active;
    float pitchEnv;
    uint32_t seed;
} DrumHit;

/* Pads on Page 0 are drawn in a 4×2 grid, indexed 0..7, in draw order.
 * Index → DrumType — kept in sync with the draw grid below. */
static const DrumType DM_PAD_ORDER[8] = {
    DRUM_KICK, DRUM_SNARE, DRUM_CLOSED_HAT, DRUM_OPEN_HAT,
    DRUM_RIM,  DRUM_CLAP,  DRUM_PEDAL_HAT,  DRUM_TOM
};
static const char* DM_PAD_LABEL[8] = {
    "KICK", "SNARE", "C.HAT", "O.HAT",
    "RIM",  "CLAP",  "P.HAT", "TOM"
};

/* Pads register as pseudo-params with idx PARAM_COUNT..PARAM_COUNT+7 so they
 * flow through the same SDK v4.5 spatial-nav + click hit-test path used by
 * every other plugin. set_param on these indices is a safe no-op. */
#define DM_PAD_PARAM_BASE   PARAM_COUNT   /* = 8 */
#define DM_PAD_COUNT        8

typedef struct {
    float sampleRate;
    float params[PARAM_COUNT];
    DrumHit hits[MAX_HITS];
    uint32_t seed;
    float roomBuf[4410]; // ~100ms reverb buffer
    int roomPos;

    /* Pad navigation (Page 0) */
    int   focusedPad;          /* 0..7 */
    int   lastSelectedParam;   /* edge detection */
    int   lastEditMode;

    /* Preset bookkeeping */
    int   currentPreset;
} DrumMachine;

/* ── Factory Kit Bank ────────────────────────────────────────────
 * 16 starter kits spanning the common drum-machine genres. Values
 * are 0..1 manifest ranges matching the 8 synthesis params. */
typedef struct {
    const char* name;
    float kickTune;
    float kickDecay;
    float snareTone;
    float snareSnap;
    float hatTone;
    float hatDecay;
    float drive;
    float room;
} KitPreset;

static const KitPreset DM_FACTORY_PRESETS[] = {
    /*                          kTun   kDec   sTon   sSnp   hTon   hDec   drv    rm */
    {"808 Trap",               0.10f, 0.85f, 0.30f, 0.60f, 0.35f, 0.25f, 0.35f, 0.15f},
    {"Deep 808",               0.05f, 0.95f, 0.25f, 0.55f, 0.30f, 0.20f, 0.40f, 0.10f},
    {"House",                  0.55f, 0.35f, 0.55f, 0.55f, 0.50f, 0.30f, 0.30f, 0.25f},
    {"Techno",                 0.45f, 0.30f, 0.50f, 0.70f, 0.60f, 0.20f, 0.50f, 0.15f},
    {"Minimal Techno",         0.48f, 0.25f, 0.45f, 0.65f, 0.55f, 0.18f, 0.30f, 0.35f},
    {"Classic 909",            0.55f, 0.35f, 0.60f, 0.60f, 0.65f, 0.25f, 0.25f, 0.20f},
    {"Classic 808",            0.30f, 0.70f, 0.40f, 0.50f, 0.45f, 0.30f, 0.20f, 0.20f},
    {"Lo-Fi",                  0.40f, 0.50f, 0.45f, 0.35f, 0.40f, 0.35f, 0.45f, 0.40f},
    {"Warm Lo-Fi",             0.35f, 0.60f, 0.40f, 0.30f, 0.35f, 0.40f, 0.55f, 0.50f},
    {"Electronic",             0.55f, 0.40f, 0.55f, 0.65f, 0.60f, 0.25f, 0.35f, 0.25f},
    {"Electro-Funk",           0.45f, 0.50f, 0.55f, 0.70f, 0.70f, 0.30f, 0.40f, 0.20f},
    {"Heavy",                  0.30f, 0.70f, 0.65f, 0.80f, 0.55f, 0.30f, 0.70f, 0.20f},
    {"Industrial",             0.35f, 0.60f, 0.70f, 0.85f, 0.60f, 0.25f, 0.80f, 0.30f},
    {"Trap Hybrid",            0.15f, 0.80f, 0.45f, 0.70f, 0.50f, 0.20f, 0.50f, 0.18f},
    {"Boom Bap",               0.38f, 0.55f, 0.55f, 0.55f, 0.45f, 0.30f, 0.40f, 0.25f},
    {"Bright Pop",             0.55f, 0.40f, 0.60f, 0.65f, 0.75f, 0.35f, 0.30f, 0.20f},
};

#define DM_NUM_PRESETS (int)(sizeof(DM_FACTORY_PRESETS) / sizeof(DM_FACTORY_PRESETS[0]))

static float frand(uint32_t* s) {
    *s = *s * 1664525u + 1013904223u;
    return (float)(*s >> 8) / 16777216.0f;
}

static DrumType noteToType(int8_t note) {
    switch (note) {
        case 36: return DRUM_KICK;
        case 38: return DRUM_SNARE;
        case 42: return DRUM_CLOSED_HAT;
        case 46: return DRUM_OPEN_HAT;
        case 37: return DRUM_RIM;
        case 39: return DRUM_CLAP;
        case 44: return DRUM_PEDAL_HAT;
        case 45: return DRUM_TOM;
        default: // Map any other note to nearest drum
            if (note < 37) return DRUM_KICK;
            if (note < 39) return DRUM_SNARE;
            if (note < 43) return DRUM_CLOSED_HAT;
            if (note < 46) return DRUM_OPEN_HAT;
            return DRUM_TOM;
    }
}

static float synthDrum(DrumHit* h, float* params, float sr) {
    float t = h->time;
    float vel = h->velocity;
    float out = 0;
    
    switch (h->type) {
        case DRUM_KICK: {
            float tune = params[P_KICK_TUNE] * 60.0f + 40.0f; // 40-100 Hz
            float decay = params[P_KICK_DECAY] * 0.5f + 0.1f;
            float pitchDrop = tune * 4.0f * expf(-t * 30.0f) + tune;
            float body = sinf(h->phase * TWO_PI) * expf(-t / decay);
            float click = (frand(&h->seed) * 2 - 1) * expf(-t * 200.0f) * 0.3f;
            h->phase += pitchDrop / sr;
            out = (body + click) * vel;
            if (t > decay * 4) h->active = 0;
            break;
        }
        case DRUM_SNARE: {
            float tone = params[P_SNARE_TONE] * 150.0f + 150.0f; // 150-300 Hz
            float snap = params[P_SNARE_SNAP] * 0.8f + 0.2f;
            float body = sinf(h->phase * TWO_PI) * expf(-t * 15.0f);
            float noise = (frand(&h->seed) * 2 - 1) * expf(-t / (snap * 0.2f));
            h->phase += tone / sr;
            out = (body * 0.5f + noise * 0.6f) * vel;
            if (t > 0.5f) h->active = 0;
            break;
        }
        case DRUM_CLOSED_HAT: {
            float tone = params[P_HAT_TONE];
            float decay = params[P_HAT_DECAY] * 0.05f + 0.02f;
            float n1 = sinf(h->phase * TWO_PI * (4000 + tone * 4000));
            float n2 = sinf(h->phase * TWO_PI * (6000 + tone * 3000) * 1.414f);
            float env = expf(-t / decay);
            h->phase += 1.0f / sr;
            out = (n1 * 0.5f + n2 * 0.5f) * env * vel * 0.5f;
            if (t > decay * 5) h->active = 0;
            break;
        }
        case DRUM_OPEN_HAT: {
            float tone = params[P_HAT_TONE];
            float decay = params[P_HAT_DECAY] * 0.3f + 0.1f;
            float n1 = sinf(h->phase * TWO_PI * (4000 + tone * 4000));
            float n2 = sinf(h->phase * TWO_PI * (7500 + tone * 2000));
            float env = expf(-t / decay);
            h->phase += 1.0f / sr;
            out = (n1 * 0.4f + n2 * 0.4f + (frand(&h->seed) - 0.5f) * 0.2f) * env * vel * 0.5f;
            if (t > decay * 4) h->active = 0;
            break;
        }
        case DRUM_RIM: {
            float freq = 800.0f * expf(-t * 60.0f) + 400.0f;
            float body = sinf(h->phase * TWO_PI) * expf(-t * 40.0f);
            h->phase += freq / sr;
            out = body * vel * 0.7f;
            if (t > 0.1f) h->active = 0;
            break;
        }
        case DRUM_CLAP: {
            // Multiple noise bursts
            float env1 = expf(-fmodf(t, 0.02f) * 200.0f);
            float env2 = expf(-t / 0.15f);
            float burst = (t < 0.03f) ? env1 : 0;
            float tail = (frand(&h->seed) * 2 - 1) * env2;
            out = (burst * 0.5f + tail * 0.6f) * vel;
            if (t > 0.4f) h->active = 0;
            break;
        }
        case DRUM_PEDAL_HAT: {
            float env = expf(-t * 80.0f);
            float n = sinf(h->phase * TWO_PI * 5000);
            h->phase += 1.0f / sr;
            out = n * env * vel * 0.3f;
            if (t > 0.05f) h->active = 0;
            break;
        }
        case DRUM_TOM: {
            float freq = 120.0f + 200.0f * expf(-t * 20.0f);
            float body = sinf(h->phase * TWO_PI) * expf(-t / 0.3f);
            h->phase += freq / sr;
            out = body * vel * 0.8f;
            if (t > 0.8f) h->active = 0;
            break;
        }
        default: break;
    }
    return out;
}

// ── API ──

int pdsynth_api_version(void) { return PDSYNTH_API_VERSION; }
const char* pdsynth_name(void) { return "Drum Machine"; }
int pdsynth_param_count(void) { return PARAM_COUNT; }

const char* pdsynth_param_name(int idx) {
    static const char* names[] = {
        "Kick Tune", "Kick Decay", "Snare Tone", "Snare Snap",
        "Hat Tone", "Hat Decay", "Drive", "Room"
    };
    if (idx >= 0 && idx < PARAM_COUNT) return names[idx];
    return "?";
}

PdSynthInstance pdsynth_create(float sampleRate) {
    DrumMachine* s = (DrumMachine*)calloc(1, sizeof(DrumMachine));
    s->sampleRate = sampleRate;
    s->seed = 12345;
    s->focusedPad = 0;
    s->lastSelectedParam = -1;
    s->lastEditMode = 0;
    /* Default kit: House (idx 2) — a balanced starter kit. */
    s->currentPreset = 2;
    const KitPreset* p = &DM_FACTORY_PRESETS[s->currentPreset];
    s->params[P_KICK_TUNE]   = p->kickTune;
    s->params[P_KICK_DECAY]  = p->kickDecay;
    s->params[P_SNARE_TONE]  = p->snareTone;
    s->params[P_SNARE_SNAP]  = p->snareSnap;
    s->params[P_HAT_TONE]    = p->hatTone;
    s->params[P_HAT_DECAY]   = p->hatDecay;
    s->params[P_MASTER_DRIVE]= p->drive;
    s->params[P_ROOM]        = p->room;
    return (PdSynthInstance)s;
}

/* Shared drum-trigger helper. Called from note-on AND from the pad preview
 * path when the user activates a pad on Page 0. Same choke + voice-steal
 * rules as pdsynth_note. */
static void dm_trigger(DrumMachine* s, DrumType dt, float velocity) {
    /* Choke: close hat kills open hat (same as note-on path). */
    if (dt == DRUM_CLOSED_HAT || dt == DRUM_PEDAL_HAT) {
        for (int i = 0; i < MAX_HITS; i++) {
            if (s->hits[i].active && s->hits[i].type == DRUM_OPEN_HAT) {
                s->hits[i].active = 0;
            }
        }
    }
    int slot = -1;
    for (int i = 0; i < MAX_HITS; i++) {
        if (!s->hits[i].active) { slot = i; break; }
    }
    if (slot < 0) {
        for (int i = 0; i < MAX_HITS; i++) {
            if (s->hits[i].type == dt) { slot = i; break; }
        }
        if (slot < 0) slot = 0;
    }
    DrumHit* h = &s->hits[slot];
    memset(h, 0, sizeof(DrumHit));
    h->type = dt;
    h->velocity = (velocity < 0.0f) ? 0.0f : (velocity > 1.0f ? 1.0f : velocity);
    h->active = 1;
    h->seed = s->seed++;
}

void pdsynth_destroy(PdSynthInstance inst) { free(inst); }

void pdsynth_note(PdSynthInstance inst, PdSynthNote* note) {
    DrumMachine* s = (DrumMachine*)inst;
    if (note->type != 1) return; // Only note-on (drums don't need note-off)
    dm_trigger(s, noteToType(note->note), note->velocity / 127.0f);
}

void pdsynth_process(PdSynthInstance inst, PdSynthAudio* audio) {
    DrumMachine* s = (DrumMachine*)inst;
    float drive = 1.0f + s->params[P_MASTER_DRIVE] * 3.0f;
    float room = s->params[P_ROOM] * 0.3f;
    float dt = 1.0f / s->sampleRate;
    
    for (int i = 0; i < audio->bufferSize; i++) {
        float mix = 0;
        
        for (int h = 0; h < MAX_HITS; h++) {
            if (!s->hits[h].active) continue;
            mix += synthDrum(&s->hits[h], s->params, s->sampleRate);
            s->hits[h].time += dt;
        }
        
        // Soft clip (drive)
        mix *= drive;
        if (mix > 1.0f) mix = 1.0f - 1.0f / (mix + 1.0f);
        else if (mix < -1.0f) mix = -1.0f + 1.0f / (-mix + 1.0f);
        
        // Simple room (comb filter)
        int roomDelay = (int)(s->sampleRate * 0.023f); // ~23ms
        float roomSample = s->roomBuf[s->roomPos];
        s->roomBuf[s->roomPos] = mix + roomSample * room * 0.6f;
        s->roomPos = (s->roomPos + 1) % roomDelay;
        
        float wet = mix + roomSample * room;
        
        audio->outputL[i] += wet;
        audio->outputR[i] += wet;
    }
}

float pdsynth_get_param(PdSynthInstance inst, int idx) {
    DrumMachine* s = (DrumMachine*)inst;
    if (idx >= 0 && idx < PARAM_COUNT) return s->params[idx];
    return 0;
}

void pdsynth_set_param(PdSynthInstance inst, int idx, float val) {
    DrumMachine* s = (DrumMachine*)inst;
    if (idx >= 0 && idx < PARAM_COUNT) s->params[idx] = val;
}

void pdsynth_reset(PdSynthInstance inst) {
    DrumMachine* s = (DrumMachine*)inst;
    for (int i = 0; i < MAX_HITS; i++) s->hits[i].active = 0;
    memset(s->roomBuf, 0, sizeof(s->roomBuf));
    s->roomPos = 0;
}

/* ── Presets ────────────────────────────────────────────────── */

int pdsynth_preset_count(void) { return DM_NUM_PRESETS; }

const char* pdsynth_preset_name(int index) {
    if (index < 0 || index >= DM_NUM_PRESETS) return NULL;
    return DM_FACTORY_PRESETS[index].name;
}

void pdsynth_load_preset(PdSynthInstance inst, int index) {
    DrumMachine* s = (DrumMachine*)inst;
    if (index < 0 || index >= DM_NUM_PRESETS) return;
    const KitPreset* p = &DM_FACTORY_PRESETS[index];
    s->params[P_KICK_TUNE]    = p->kickTune;
    s->params[P_KICK_DECAY]   = p->kickDecay;
    s->params[P_SNARE_TONE]   = p->snareTone;
    s->params[P_SNARE_SNAP]   = p->snareSnap;
    s->params[P_HAT_TONE]     = p->hatTone;
    s->params[P_HAT_DECAY]    = p->hatDecay;
    s->params[P_MASTER_DRIVE] = p->drive;
    s->params[P_ROOM]         = p->room;
    s->currentPreset = index;
}

int pdsynth_get_preset(PdSynthInstance inst) {
    DrumMachine* s = (DrumMachine*)inst;
    return s->currentPreset;
}

/* ══════════════════════════════════════════════════════════════
 * Page 0 custom draw — 4x2 drum pad grid that flashes per hit.
 * ══════════════════════════════════════════════════════════════ */

static void dm_fillRect(SDL_Renderer* r, int x, int y, int w, int h) {
    SDL_Rect rc = {x, y, w, h}; SDL_RenderFillRect(r, &rc);
}
static void dm_drawRect(SDL_Renderer* r, int x, int y, int w, int h) {
    SDL_Rect rc = {x, y, w, h}; SDL_RenderDrawRect(r, &rc);
}

int pdsynth_draw(PdSynthInstance inst, const PdDrawContext* ctx) {
    if (ctx->page != 0) return 0;

    DrumMachine* s = (DrumMachine*)inst;
    SDL_Renderer* r = (SDL_Renderer*)ctx->renderer;
    int X = ctx->x, Y = ctx->y, W = ctx->w, H = ctx->h;
    int sel = ctx->selectedParam;
    int editing = ctx->editMode;

    /* Per-drum baseline colours — each pad keeps its own identity tint */
    static const uint8_t DRUM_RGB[DRUM_COUNT][3] = {
        {255,  60,  40},  /* KICK       — deep red   */
        {255, 140,  40},  /* SNARE      — orange     */
        {255, 220,  60},  /* CLOSED_HAT — yellow     */
        {255, 255, 120},  /* OPEN_HAT   — bright yel */
        {255, 100, 140},  /* RIM        — pink       */
        {255,  80, 120},  /* CLAP       — pink-red   */
        {200, 180,  60},  /* PEDAL_HAT  — dim yel    */
        {200,  80,  60},  /* TOM        — brown-red  */
    };
    const uint8_t AR = 255, AG = 50, AB = 80;

    /* ── Flash amount per drum type (0..1). Scans active hits;
     * the most recently triggered hit for a given type wins. ── */
    float flash[DRUM_COUNT] = {0};
    for (int i = 0; i < MAX_HITS; i++) {
        if (!s->hits[i].active) continue;
        int t = (int)s->hits[i].type;
        if (t < 0 || t >= DRUM_COUNT) continue;
        /* Roughly: 300 ms visual decay. time is in seconds since trigger. */
        float f = 1.0f - s->hits[i].time * 3.0f;
        if (f < 0.0f) f = 0.0f;
        if (f > 1.0f) f = 1.0f;
        f *= s->hits[i].velocity;
        if (f > flash[t]) flash[t] = f;
    }

    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);

    /* ── Background ── */
    SDL_SetRenderDrawColor(r, 10, 4, 8, 255);
    dm_fillRect(r, X, Y, W, H);

    /* ── Accent band (Y=0..14) with "DRUM MACHINE" title ── */
    {
        int bandH = 14;
        SDL_SetRenderDrawColor(r, 18, 4, 8, 255);
        dm_fillRect(r, X, Y, W, bandH);
        pdt_drawStrC(r, X + 6, Y + 4, 255, 180, 200, 255, "DRUM MACHINE");
        SDL_SetRenderDrawColor(r, AR, AG, AB, 220);
        SDL_RenderDrawLine(r, X, Y + bandH, X + W - 1, Y + bandH);
        /* 4 short stripes on the right — a tiny beat visualisation */
        for (int i = 0; i < 4; i++) {
            int sx = X + W - 12 - i * 6;
            uint8_t a = 80 + (uint8_t)(flash[i] * 175);
            SDL_SetRenderDrawColor(r, AR, AG, AB, a);
            dm_fillRect(r, sx, Y + 3, 3, bandH - 6);
        }
    }

    /* ── Focus + preview edge detection ── SDK v4.5 spatial nav routes the
     * focus through ctx->selectedParam; we reserve paramIdx values 8..15
     * for the 8 pads (set_param on those indices is a safe no-op). When
     * the user presses A on a focused pad the host flips editMode to 1
     * for one frame; we detect that rising edge and trigger a preview hit. */
    {
        int selPadParam = ctx->selectedParam;
        int pendingPreviewPad = -1;
        if (selPadParam >= DM_PAD_PARAM_BASE &&
            selPadParam <  DM_PAD_PARAM_BASE + DM_PAD_COUNT) {
            int pad = selPadParam - DM_PAD_PARAM_BASE;
            s->focusedPad = pad;
            /* Edit-mode rising edge on this pad → preview. */
            if (ctx->editMode && !s->lastEditMode &&
                s->lastSelectedParam == selPadParam) {
                pendingPreviewPad = pad;
            }
        }
        s->lastSelectedParam = selPadParam;
        s->lastEditMode = ctx->editMode;
        if (pendingPreviewPad >= 0) {
            dm_trigger(s, DM_PAD_ORDER[pendingPreviewPad], 0.85f);
        }
    }

    /* ── 4x2 pad grid (Y=18..110, 92 px, compressed from 130) ── */
    {
        int gx = X + 8, gy = Y + 18, gw = W - 16, gh = 92;
        int cols = 4, rows = 2;
        int gap = 6;
        int cellW = (gw - gap * (cols - 1)) / cols;
        int cellH = (gh - gap * (rows - 1)) / rows;
        /* Mouse path: the host's spatial-nav hit-test already routes clicks
         * on registered controls (our pd_register_control calls below) to
         * setting selectedParam, which makes A-to-preview work the same
         * way as gamepad via the edge detector above. No extra code here. */
        for (int row = 0; row < rows; row++) {
            for (int col = 0; col < cols; col++) {
                int idx = row * cols + col;
                if (idx >= DRUM_COUNT) break;
                int px = gx + col * (cellW + gap);
                int py = gy + row * (cellH + gap);
                uint8_t dr = DRUM_RGB[idx][0];
                uint8_t dg = DRUM_RGB[idx][1];
                uint8_t db = DRUM_RGB[idx][2];
                float f = flash[idx];

                /* Register as a spatial-nav target so D-pad can focus it
                 * and mouse hit-test can select it via the host NavGroup. */
                pd_register_control(ctx, DM_PAD_PARAM_BASE + idx,
                                    px, py, cellW, cellH);

                /* Pad body — dim base colour, brightens with flash */
                uint8_t bg = (uint8_t)(18 + f * 60);
                SDL_SetRenderDrawColor(r, bg, bg / 2, bg / 3, 255);
                dm_fillRect(r, px, py, cellW, cellH);

                /* Flash overlay */
                if (f > 0.01f) {
                    SDL_SetRenderDrawColor(r, dr, dg, db, (uint8_t)(f * 200));
                    dm_fillRect(r, px + 2, py + 2, cellW - 4, cellH - 4);
                }

                /* Inner highlight ring scales with velocity */
                uint8_t ringA = (uint8_t)(80 + f * 175);
                SDL_SetRenderDrawColor(r, dr, dg, db, ringA);
                dm_drawRect(r, px, py, cellW, cellH);
                dm_drawRect(r, px + 1, py + 1, cellW - 2, cellH - 2);

                /* Focus ring when pad is the active spatial-nav target. */
                int isFocused = (ctx->selectedParam == DM_PAD_PARAM_BASE + idx);
                if (isFocused) {
                    SDL_SetRenderDrawColor(r, 255, 255, 255, 220);
                    dm_drawRect(r, px - 1, py - 1, cellW + 2, cellH + 2);
                    dm_drawRect(r, px - 2, py - 2, cellW + 4, cellH + 4);
                }

                /* Pad label along the top-left corner — readable on device. */
                SDL_SetRenderDrawColor(r, 255, 255, 255, (uint8_t)(140 + f * 115));
                pdt_drawStrC(r, px + 3, py + 3, 255, 255, 255,
                             (uint8_t)(160 + f * 95), DM_PAD_LABEL[idx]);

                /* Tiny glyph in the centre — a shape hint per drum type.
                 * KICK: filled circle; SNARE: horizontal bar; HATS: vertical
                 * tick; RIM: small square; CLAP: dot cluster; TOM: tall bar.
                 * These are 6-10 px shapes drawn with SDL rects/lines. */
                int mx = px + cellW / 2;
                int my = py + cellH / 2;
                SDL_SetRenderDrawColor(r, 255, 255, 255, (uint8_t)(120 + f * 135));
                switch (idx) {
                    case DRUM_KICK: {
                        /* big filled diamond-ish blob */
                        for (int dy = -6; dy <= 6; dy++) {
                            int span = 6 - (dy < 0 ? -dy : dy);
                            SDL_RenderDrawLine(r, mx - span, my + dy, mx + span, my + dy);
                        }
                        break;
                    }
                    case DRUM_SNARE: {
                        dm_fillRect(r, mx - 10, my - 2, 20, 4);
                        break;
                    }
                    case DRUM_CLOSED_HAT: {
                        dm_fillRect(r, mx - 1, my - 8, 2, 16);
                        break;
                    }
                    case DRUM_OPEN_HAT: {
                        dm_fillRect(r, mx - 1, my - 10, 2, 8);
                        dm_fillRect(r, mx - 1, my + 2,  2, 8);
                        break;
                    }
                    case DRUM_RIM: {
                        dm_drawRect(r, mx - 4, my - 4, 8, 8);
                        dm_drawRect(r, mx - 5, my - 5, 10, 10);
                        break;
                    }
                    case DRUM_CLAP: {
                        for (int d = 0; d < 5; d++) {
                            int dx2 = (d - 2) * 3;
                            dm_fillRect(r, mx + dx2 - 1, my - 1, 2, 2);
                        }
                        break;
                    }
                    case DRUM_PEDAL_HAT: {
                        dm_fillRect(r, mx - 8, my - 1, 16, 2);
                        dm_fillRect(r, mx - 1, my,     2, 5);
                        break;
                    }
                    case DRUM_TOM: {
                        dm_fillRect(r, mx - 2, my - 8, 4, 16);
                        break;
                    }
                }
            }
        }
    }

    /* ── Live oscilloscope (Y=114..144, 30 px) ── */
    {
        int ox = X + 8, oy = Y + 114, ow = W - 16, oh = 30;
        SDL_SetRenderDrawColor(r, 8, 4, 6, 255);
        dm_fillRect(r, ox, oy, ow, oh);
        SDL_SetRenderDrawColor(r, AR, AG, AB, ctx->peak > 0.01f ? 220 : 80);
        dm_drawRect(r, ox, oy, ow, oh);

        int mid = oy + oh / 2;
        SDL_SetRenderDrawColor(r, AR / 5, AG / 5, AB / 5, 160);
        SDL_RenderDrawLine(r, ox + 3, mid, ox + ow - 4, mid);

        if (ctx->scopeLen > 0 && ctx->scopeBufL) {
            SDL_SetRenderDrawColor(r, 255, 220, 200, 220);
            int px = ox + 2, py = mid;
            for (int i = 1; i < ow - 4; i++) {
                int si = i * ctx->scopeLen / (ow - 4);
                if (si >= ctx->scopeLen) break;
                int nx = ox + 2 + i;
                int ny = mid - (int)(ctx->scopeBufL[si] * (oh / 2 - 4));
                if (ny < oy + 2) ny = oy + 2;
                if (ny > oy + oh - 2) ny = oy + oh - 2;
                SDL_RenderDrawLine(r, px, py, nx, ny);
                px = nx; py = ny;
            }
        }
    }

    /* ── Peak meters L/R (Y=148..160, 12 px) ── */
    {
        int mx = X + 8, my = Y + 148, mh = 12;
        int mw = (W - 20) / 2;
        for (int ch = 0; ch < 2; ch++) {
            float pk = (ch == 0) ? ctx->peakL : ctx->peakR;
            if (pk < 0.0f) pk = 0.0f; if (pk > 1.0f) pk = 1.0f;
            int x0 = mx + ch * (mw + 4);
            SDL_SetRenderDrawColor(r, 14, 6, 8, 255);
            dm_fillRect(r, x0, my, mw, mh);
            int fill = (int)(pk * (mw - 2));
            uint8_t mcR = pk > 0.7f ? 255 : (uint8_t)(180 + pk * 80);
            uint8_t mcG = pk < 0.7f ? 100 + (uint8_t)(pk * 100) : (uint8_t)((1.0f - pk) * 200);
            uint8_t mcB = 40;
            SDL_SetRenderDrawColor(r, mcR, mcG, mcB, 255);
            dm_fillRect(r, x0 + 1, my + 1, fill, mh - 2);
            SDL_SetRenderDrawColor(r, AR, AG, AB, 120);
            dm_drawRect(r, x0, my, mw, mh);
        }
    }

    /* ── Interactive param strip (Y=164..238, 74 px) — 8 params ── */
    {
        int stripY = Y + 164;
        int stripH = H - 164 - 2;
        int pCount = 8;
        int stripPadX = 6;
        int cellW = (W - stripPadX * 2) / pCount;

        float vals[8] = {
            s->params[P_KICK_TUNE], s->params[P_KICK_DECAY],
            s->params[P_SNARE_TONE], s->params[P_SNARE_SNAP],
            s->params[P_HAT_TONE], s->params[P_HAT_DECAY],
            s->params[P_MASTER_DRIVE], s->params[P_ROOM]
        };
        static const char* labels[8] = {
            "KTN", "KDC", "STN", "SNP", "HTN", "HDC", "DRV", "RM"
        };

        SDL_SetRenderDrawColor(r, AR / 8, AG / 8, AB / 8, 200);
        SDL_RenderDrawLine(r, X + 4, stripY, X + W - 4, stripY);

        for (int p = 0; p < pCount; p++) {
            int cx = X + stripPadX + p * cellW;
            int cy = stripY + 4;
            int cw = cellW - 1;
            int ch = stripH - 6;
            int isSel = (sel == p);
            int isEdit = isSel && editing;
            pdt_drawParamCell(r, ctx, p, cx, cy, cw, ch, labels[p], vals[p],
                              isSel, isEdit, AR, AG, AB);
        }
    }

    SDL_SetRenderDrawColor(r, AR, AG, AB, 80);
    dm_drawRect(r, X, Y, W, H);
    return 1;
}
