/*
 * rnnoise_gate.c - RNNoise-based LADSPA noise suppressor and VAD gate plugin
 *
 * Part of Clean Mic.
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <rnnoise.h>

#include "ladspa.h"

#define PLUGIN_UNIQUE_ID 0x4A474D01
#define PLUGIN_LABEL     "rnnoise_gate"
#define PLUGIN_NAME      "Clean Mic RNNoise Gate"
#define PLUGIN_MAKER     "Clean Mic"
#define PLUGIN_COPYRIGHT "GPL"

#define RNNOISE_FRAME_SIZE 480

/*
 * Energy check for the gate. RNNoise's VAD alone is not reliable on loud,
 * steady background noise (e.g. a high-gain USB mic): it can report voice
 * probability > 0.9 on pure noise, which kept the gate open most of the time
 * so the floor never applied. The gate therefore also requires the input frame
 * energy to be clearly above a tracked noise floor.
 */
#define SPEECH_MARGIN_DB        9.0f   /* frame must exceed noise floor by this */
#define NOISE_FALL_COEF         0.3f   /* fast tracking when energy drops */
#define NOISE_RISE_DB_PER_SEC   1.0f   /* slow tracking when energy rises */
#define NOISE_FLOOR_MIN_DB    -100.0f
#define NOISE_SILENCE_DB       -90.0f  /* frames below this (digital silence, dropouts) do not update the tracker */
#define NOISE_WINDOW_SEC         0.6f  /* the tracker never stays below the minimum of the last two windows */
#define NOISE_WINDOW_EMPTY_DB 1000.0f  /* window minimum before any frame was seen */
#define NOISE_FLOOR_INIT_DB      0.0f  /* start high; falls to the real floor in a few frames */
#define FLOOR_DB_MIN           -80.0f  /* floor at this value means true mute (gain 0) */

enum {
    PORT_INPUT           = 0,
    PORT_OUTPUT          = 1,
    PORT_SUPPRESSION     = 2,
    PORT_GATE_ENABLE     = 3,
    PORT_VAD_THRESHOLD   = 4,
    PORT_GATE_RELEASE_MS = 5,
    PORT_FLOOR_DB        = 6,
    PORT_COUNT           = 7
};

static const LADSPA_PortDescriptor PORT_DESCRIPTORS[PORT_COUNT] = {
    LADSPA_PORT_INPUT  | LADSPA_PORT_AUDIO,
    LADSPA_PORT_OUTPUT | LADSPA_PORT_AUDIO,
    LADSPA_PORT_INPUT  | LADSPA_PORT_CONTROL,
    LADSPA_PORT_INPUT  | LADSPA_PORT_CONTROL,
    LADSPA_PORT_INPUT  | LADSPA_PORT_CONTROL,
    LADSPA_PORT_INPUT  | LADSPA_PORT_CONTROL,
    LADSPA_PORT_INPUT  | LADSPA_PORT_CONTROL
};

static const char * const PORT_NAMES[PORT_COUNT] = {
    "Input",
    "Output",
    "Suppression",
    "Gate Enable",
    "VAD Threshold",
    "Gate Release ms",
    "Floor dB"
};

static const LADSPA_PortRangeHint PORT_RANGE_HINTS[PORT_COUNT] = {
    /* 0: Input */
    { 0, 0.0f, 0.0f },
    /* 1: Output */
    { 0, 0.0f, 0.0f },
    /* 2: Suppression 0..1, default 1.0 */
    { LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_DEFAULT_1, 0.0f, 1.0f },
    /* 3: Gate Enable 0/1, default 1 */
    { LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_TOGGLED | LADSPA_HINT_DEFAULT_1, 0.0f, 1.0f },
    /* 4: VAD Threshold 0..1, default 0.6 */
    { LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_DEFAULT_MIDDLE, 0.0f, 1.0f },
    /* 5: Gate Release ms 20..1000, default 200 */
    { LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_DEFAULT_LOW, 20.0f, 1000.0f },
    /* 6: Floor dB -80..0, default -60 */
    { LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_DEFAULT_LOW, -80.0f, 0.0f }
};

typedef struct {
    unsigned long sample_rate;
    DenoiseState *rnn_state;

    /* Port pointers */
    const LADSPA_Data *port_input;
    LADSPA_Data       *port_output;
    const LADSPA_Data *port_suppression;
    const LADSPA_Data *port_gate_enable;
    const LADSPA_Data *port_vad_threshold;
    const LADSPA_Data *port_gate_release_ms;
    const LADSPA_Data *port_floor_db;

    /* Default storage for control fallback */
    LADSPA_Data default_suppression;
    LADSPA_Data default_gate_enable;
    LADSPA_Data default_vad_threshold;
    LADSPA_Data default_gate_release_ms;
    LADSPA_Data default_floor_db;

    /* Ring / FIFO delay buffers */
    float in_buf[RNNOISE_FRAME_SIZE];
    float out_buf[RNNOISE_FRAME_SIZE];
    int   buf_pos;

    /* Gate dynamics */
    float         current_gain;
    unsigned long hold_samples;
    float         noise_floor_db;
    float         window_min_db;      /* minimum frame energy in the current window */
    float         prev_window_min_db; /* minimum frame energy in the previous window */
    float         window_elapsed_sec;
} RNNoiseGatePlugin;

static float frame_energy_db(const float *frame) {
    double sum = 0.0;
    for (int i = 0; i < RNNOISE_FRAME_SIZE; i++) {
        float s = frame[i];
        if (isnan(s) || isinf(s)) s = 0.0f;
        sum += (double)s * (double)s;
    }
    float db = (float)(10.0 * log10(sum / RNNOISE_FRAME_SIZE + 1e-20));
    return db < NOISE_FLOOR_MIN_DB ? NOISE_FLOOR_MIN_DB : db;
}

static void reset_noise_floor(RNNoiseGatePlugin *plugin) {
    plugin->noise_floor_db = NOISE_FLOOR_INIT_DB;
    plugin->window_min_db = NOISE_WINDOW_EMPTY_DB;
    plugin->prev_window_min_db = NOISE_WINDOW_EMPTY_DB;
    plugin->window_elapsed_sec = 0.0f;
}

/*
 * Lift the tracker to the minimum energy seen over the last two windows
 * (0.6-1.2 s). Speech has short pauses, so this minimum stays near the
 * background, while a tracker pulled down by a dropout recovers in under 2 s.
 */
static void update_noise_window(RNNoiseGatePlugin *plugin, float energy_db, float frame_sec) {
    int is_silent = (energy_db < NOISE_SILENCE_DB);
    if (!is_silent && energy_db < plugin->window_min_db) {
        plugin->window_min_db = energy_db;
    }
    plugin->window_elapsed_sec += frame_sec;
    if (plugin->window_elapsed_sec < NOISE_WINDOW_SEC) {
        return;
    }

    float recent_min_db = plugin->window_min_db;
    if (plugin->prev_window_min_db < recent_min_db) {
        recent_min_db = plugin->prev_window_min_db;
    }
    int seen_audio = (recent_min_db < NOISE_WINDOW_EMPTY_DB);
    if (seen_audio && recent_min_db > plugin->noise_floor_db) {
        plugin->noise_floor_db = recent_min_db;
    }
    plugin->prev_window_min_db = plugin->window_min_db;
    plugin->window_min_db = NOISE_WINDOW_EMPTY_DB;
    plugin->window_elapsed_sec = 0.0f;
}

/* Minimum-style tracker: follows drops quickly, rises slowly (so speech does not lift it). */
static void update_noise_floor(RNNoiseGatePlugin *plugin, float energy_db, float frame_sec) {
    update_noise_window(plugin, energy_db, frame_sec);

    /* Digital silence says nothing about the background; skip it so the tracker never sinks to the clamp. */
    if (energy_db < NOISE_SILENCE_DB) {
        return;
    }

    float diff = energy_db - plugin->noise_floor_db;
    if (diff < 0.0f) {
        plugin->noise_floor_db += NOISE_FALL_COEF * diff;
    } else {
        float max_rise = NOISE_RISE_DB_PER_SEC * frame_sec;
        plugin->noise_floor_db += diff < max_rise ? diff : max_rise;
    }
}

static float floor_db_to_gain(float floor_db) {
    if (floor_db <= FLOOR_DB_MIN) {
        return 0.0f;
    }
    return powf(10.0f, floor_db / 20.0f);
}

static void process_rnnoise_frame(RNNoiseGatePlugin *plugin) {
    /* Read and clamp control inputs */
    float suppression = plugin->port_suppression ? *plugin->port_suppression : plugin->default_suppression;
    if (suppression < 0.0f) suppression = 0.0f;
    if (suppression > 1.0f) suppression = 1.0f;

    float gate_enable = plugin->port_gate_enable ? *plugin->port_gate_enable : plugin->default_gate_enable;
    int gate_enabled = (gate_enable > 0.5f);

    float vad_thresh = plugin->port_vad_threshold ? *plugin->port_vad_threshold : plugin->default_vad_threshold;
    if (vad_thresh < 0.0f) vad_thresh = 0.0f;
    if (vad_thresh > 1.0f) vad_thresh = 1.0f;

    float release_ms = plugin->port_gate_release_ms ? *plugin->port_gate_release_ms : plugin->default_gate_release_ms;
    if (release_ms < 20.0f) release_ms = 20.0f;
    if (release_ms > 1000.0f) release_ms = 1000.0f;

    float floor_db = plugin->port_floor_db ? *plugin->port_floor_db : plugin->default_floor_db;
    if (floor_db < -80.0f) floor_db = -80.0f;
    if (floor_db > 0.0f) floor_db = 0.0f;
    float floor_gain = floor_db_to_gain(floor_db);

    float sr = plugin->sample_rate > 0 ? (float)plugin->sample_rate : 48000.0f;
    float attack_rate = 1.0f / (0.005f * sr);  /* ~5 ms attack ramp */
    float release_rate = 1.0f / (0.020f * sr); /* ~20 ms release ramp to avoid clicks */
    unsigned long full_hold = (unsigned long)((release_ms / 1000.0f) * sr);

    /* Scale input samples by 32768 for RNNoise */
    float rnn_in[RNNOISE_FRAME_SIZE];
    for (int i = 0; i < RNNOISE_FRAME_SIZE; i++) {
        float s = plugin->in_buf[i];
        if (isnan(s) || isinf(s)) s = 0.0f;
        if (fabsf(s) < 1e-15f) s = 0.0f;
        rnn_in[i] = s * 32768.0f;
    }

    float rnn_out[RNNOISE_FRAME_SIZE];
    float vad_prob = rnnoise_process_frame(plugin->rnn_state, rnn_out, rnn_in);
    if (isnan(vad_prob) || isinf(vad_prob)) vad_prob = 0.0f;

    /* Scale back by 1/32768 */
    for (int i = 0; i < RNNOISE_FRAME_SIZE; i++) {
        rnn_out[i] /= 32768.0f;
        if (isnan(rnn_out[i]) || isinf(rnn_out[i])) rnn_out[i] = 0.0f;
    }

    float energy_db = frame_energy_db(plugin->in_buf);
    update_noise_floor(plugin, energy_db, RNNOISE_FRAME_SIZE / sr);
    int above_noise = (energy_db >= plugin->noise_floor_db + SPEECH_MARGIN_DB);
    int voice_present = (vad_prob >= vad_thresh) && above_noise;
    if (gate_enabled && voice_present) {
        plugin->hold_samples = full_hold;
    }

    for (int i = 0; i < RNNOISE_FRAME_SIZE; i++) {
        float dry = plugin->in_buf[i];
        if (isnan(dry) || isinf(dry)) dry = 0.0f;
        float wet = rnn_out[i];
        float mixed = (1.0f - suppression) * dry + suppression * wet;

        float target_gain;
        if (!gate_enabled) {
            target_gain = 1.0f;
        } else if (voice_present) {
            target_gain = 1.0f;
        } else {
            if (plugin->hold_samples > 0) {
                plugin->hold_samples--;
                target_gain = 1.0f;
            } else {
                target_gain = floor_gain;
            }
        }

        /* Smooth ramp towards target gain */
        if (target_gain > plugin->current_gain) {
            plugin->current_gain += attack_rate;
            if (plugin->current_gain > target_gain) {
                plugin->current_gain = target_gain;
            }
        } else if (target_gain < plugin->current_gain) {
            plugin->current_gain -= release_rate;
            if (plugin->current_gain < target_gain) {
                plugin->current_gain = target_gain;
            }
        }

        float out_sample = mixed * plugin->current_gain;
        if (fabsf(out_sample) < 1e-15f) out_sample = 0.0f;
        if (isnan(out_sample) || isinf(out_sample)) out_sample = 0.0f;
        plugin->out_buf[i] = out_sample;
    }
}

static LADSPA_Handle instantiate_rnnoise_gate(const LADSPA_Descriptor *descriptor,
                                             unsigned long sample_rate) {
    (void)descriptor;
    RNNoiseGatePlugin *plugin = (RNNoiseGatePlugin *)calloc(1, sizeof(RNNoiseGatePlugin));
    if (!plugin) {
        return NULL;
    }

    plugin->rnn_state = rnnoise_create(NULL);
    if (!plugin->rnn_state) {
        free(plugin);
        return NULL;
    }

    plugin->sample_rate = sample_rate > 0 ? sample_rate : 48000;

    /* Initialize control fallbacks */
    plugin->default_suppression = 1.0f;
    plugin->default_gate_enable = 1.0f;
    plugin->default_vad_threshold = 0.6f;
    plugin->default_gate_release_ms = 200.0f;
    plugin->default_floor_db = -60.0f;

    plugin->port_suppression = &plugin->default_suppression;
    plugin->port_gate_enable = &plugin->default_gate_enable;
    plugin->port_vad_threshold = &plugin->default_vad_threshold;
    plugin->port_gate_release_ms = &plugin->default_gate_release_ms;
    plugin->port_floor_db = &plugin->default_floor_db;

    plugin->current_gain = powf(10.0f, -60.0f / 20.0f);
    plugin->hold_samples = 0;
    reset_noise_floor(plugin);
    plugin->buf_pos = 0;

    return (LADSPA_Handle)plugin;
}

static void connect_port_rnnoise_gate(LADSPA_Handle instance,
                                      unsigned long port,
                                      LADSPA_Data *data_location) {
    RNNoiseGatePlugin *plugin = (RNNoiseGatePlugin *)instance;
    if (!plugin) return;

    switch (port) {
        case PORT_INPUT:
            plugin->port_input = data_location;
            break;
        case PORT_OUTPUT:
            plugin->port_output = data_location;
            break;
        case PORT_SUPPRESSION:
            plugin->port_suppression = data_location ? data_location : &plugin->default_suppression;
            break;
        case PORT_GATE_ENABLE:
            plugin->port_gate_enable = data_location ? data_location : &plugin->default_gate_enable;
            break;
        case PORT_VAD_THRESHOLD:
            plugin->port_vad_threshold = data_location ? data_location : &plugin->default_vad_threshold;
            break;
        case PORT_GATE_RELEASE_MS:
            plugin->port_gate_release_ms = data_location ? data_location : &plugin->default_gate_release_ms;
            break;
        case PORT_FLOOR_DB:
            plugin->port_floor_db = data_location ? data_location : &plugin->default_floor_db;
            break;
        default:
            break;
    }
}

static void activate_rnnoise_gate(LADSPA_Handle instance) {
    RNNoiseGatePlugin *plugin = (RNNoiseGatePlugin *)instance;
    if (!plugin) return;

    if (plugin->rnn_state) {
        rnnoise_init(plugin->rnn_state, NULL);
    }

    plugin->buf_pos = 0;
    for (int i = 0; i < RNNOISE_FRAME_SIZE; i++) {
        plugin->in_buf[i] = 0.0f;
        plugin->out_buf[i] = 0.0f;
    }

    float floor_db = plugin->port_floor_db ? *plugin->port_floor_db : plugin->default_floor_db;
    if (floor_db < -80.0f) floor_db = -80.0f;
    if (floor_db > 0.0f) floor_db = 0.0f;

    float gate_enable = plugin->port_gate_enable ? *plugin->port_gate_enable : plugin->default_gate_enable;
    if (gate_enable > 0.5f) {
        plugin->current_gain = floor_db_to_gain(floor_db);
    } else {
        plugin->current_gain = 1.0f;
    }
    plugin->hold_samples = 0;
    reset_noise_floor(plugin);
}

static void run_rnnoise_gate(LADSPA_Handle instance, unsigned long sample_count) {
    RNNoiseGatePlugin *plugin = (RNNoiseGatePlugin *)instance;
    if (!plugin || sample_count == 0) return;

    const LADSPA_Data *in = plugin->port_input;
    LADSPA_Data *out = plugin->port_output;
    if (!in || !out) return;

    int pos = plugin->buf_pos;
    for (unsigned long i = 0; i < sample_count; i++) {
        LADSPA_Data in_sample = in[i];
        out[i] = plugin->out_buf[pos];
        plugin->in_buf[pos] = in_sample;
        pos++;
        if (pos == RNNOISE_FRAME_SIZE) {
            plugin->buf_pos = pos;
            process_rnnoise_frame(plugin);
            pos = 0;
        }
    }
    plugin->buf_pos = pos;
}

static void deactivate_rnnoise_gate(LADSPA_Handle instance) {
    (void)instance;
}

static void cleanup_rnnoise_gate(LADSPA_Handle instance) {
    RNNoiseGatePlugin *plugin = (RNNoiseGatePlugin *)instance;
    if (!plugin) return;

    if (plugin->rnn_state) {
        rnnoise_destroy(plugin->rnn_state);
        plugin->rnn_state = NULL;
    }
    free(plugin);
}

static const LADSPA_Descriptor DESCRIPTOR = {
    PLUGIN_UNIQUE_ID,
    PLUGIN_LABEL,
    LADSPA_PROPERTY_HARD_RT_CAPABLE,
    PLUGIN_NAME,
    PLUGIN_MAKER,
    PLUGIN_COPYRIGHT,
    PORT_COUNT,
    PORT_DESCRIPTORS,
    PORT_NAMES,
    PORT_RANGE_HINTS,
    NULL,
    instantiate_rnnoise_gate,
    connect_port_rnnoise_gate,
    activate_rnnoise_gate,
    run_rnnoise_gate,
    NULL,
    NULL,
    deactivate_rnnoise_gate,
    cleanup_rnnoise_gate
};

__attribute__((visibility("default")))
const LADSPA_Descriptor * ladspa_descriptor(unsigned long Index) {
    if (Index == 0) {
        return &DESCRIPTOR;
    }
    return NULL;
}
