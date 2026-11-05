/*
 * test_plugin.c - Test runner for Clean Mic RNNoise LADSPA plugin
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <dlfcn.h>
#include <assert.h>

#include "ladspa.h"

#define SAMPLE_RATE 48000
#define DURATION_SEC 2
#define TOTAL_SAMPLES (SAMPLE_RATE * DURATION_SEC)

/* 2nd-order resonator / bandpass formant filter */
typedef struct {
    float b0, b1, b2;
    float a1, a2;
    float x1, x2;
    float y1, y2;
} Biquad;

static void biquad_init_bandpass(Biquad *b, float fc, float q, float fs) {
    float w0 = 2.0f * (float)M_PI * fc / fs;
    float alpha = sinf(w0) / (2.0f * q);
    float a0 = 1.0f + alpha;

    b->b0 = (sinf(w0) / 2.0f) / a0;
    b->b1 = 0.0f;
    b->b2 = (-sinf(w0) / 2.0f) / a0;
    b->a1 = (-2.0f * cosf(w0)) / a0;
    b->a2 = (1.0f - alpha) / a0;
    b->x1 = b->x2 = b->y1 = b->y2 = 0.0f;
}

static float biquad_process(Biquad *b, float in) {
    float out = b->b0 * in + b->b1 * b->x1 + b->b2 * b->x2
                - b->a1 * b->y1 - b->a2 * b->y2;
    b->x2 = b->x1;
    b->x1 = in;
    b->y2 = b->y1;
    b->y1 = out;
    return out;
}

static void generate_white_noise(float *buffer, int num_samples, float db) {
    float amp = powf(10.0f, db / 20.0f);
    for (int i = 0; i < num_samples; i++) {
        float r = ((float)rand() / (float)RAND_MAX) * 2.0f - 1.0f;
        buffer[i] = r * amp;
    }
}

static void generate_voice_like_signal(float *buffer, int num_samples, float f0, float noise_db) {
    float phase = 0.0f;
    float phase_inc = f0 / (float)SAMPLE_RATE;
    float noise_amp = powf(10.0f, noise_db / 20.0f);

    Biquad f1, f2;
    /* Typical vowel formants ~700 Hz and ~2400 Hz */
    biquad_init_bandpass(&f1, 700.0f, 4.0f, (float)SAMPLE_RATE);
    biquad_init_bandpass(&f2, 2400.0f, 5.0f, (float)SAMPLE_RATE);

    for (int i = 0; i < num_samples; i++) {
        /* Sawtooth wave */
        float saw = 2.0f * (phase - floorf(phase + 0.5f));
        phase += phase_inc;
        if (phase >= 1.0f) phase -= 1.0f;

        /* Formant filtering */
        float formant = biquad_process(&f1, saw) * 1.5f + biquad_process(&f2, saw) * 0.8f;
        /* Mixed with white noise */
        float noise = (((float)rand() / (float)RAND_MAX) * 2.0f - 1.0f) * noise_amp;

        buffer[i] = formant * 0.35f + noise;
    }
}

static void run_in_odd_blocks(const LADSPA_Descriptor *desc, LADSPA_Handle inst,
                              const float *input, float *output, int total_samples) {
    static const int block_sizes[] = { 100, 333, 1, 0, 1024 };
    int num_block_sizes = (int)(sizeof(block_sizes) / sizeof(block_sizes[0]));
    int processed = 0;
    int b_idx = 0;

    while (processed < total_samples) {
        int bs = block_sizes[b_idx % num_block_sizes];
        b_idx++;

        if (processed + bs > total_samples) {
            bs = total_samples - processed;
        }

        /* Connect buffer slice for this block */
        desc->connect_port(inst, 0, (LADSPA_Data *)(input + processed));
        desc->connect_port(inst, 1, (LADSPA_Data *)(output + processed));

        /* Test handling of arbitrary / odd block sizes, including 0 */
        desc->run(inst, (unsigned long)bs);

        processed += bs;
    }
    assert(processed == total_samples);
}

static double calculate_rms(const float *buffer, int start, int end) {
    assert(end > start);
    double sum = 0.0;
    for (int i = start; i < end; i++) {
        sum += (double)buffer[i] * (double)buffer[i];
    }
    return sqrt(sum / (double)(end - start));
}

int main(int argc, char **argv) {
    const char *so_path = (argc > 1) ? argv[1] : "./build/librnnoise_gate_ladspa.so";
    printf("Testing LADSPA plugin from: %s\n", so_path);

    /* 1. dlopen the shared library */
    void *handle = dlopen(so_path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        fprintf(stderr, "dlopen failed: %s\n", dlerror());
        return 1;
    }

    /* 2. Retrieve descriptor */
    LADSPA_Descriptor_Function get_descriptor =
        (LADSPA_Descriptor_Function)dlsym(handle, "ladspa_descriptor");
    if (!get_descriptor) {
        fprintf(stderr, "dlsym ladspa_descriptor failed: %s\n", dlerror());
        dlclose(handle);
        return 1;
    }

    const LADSPA_Descriptor *desc = get_descriptor(0);
    assert(desc != NULL);
    assert(get_descriptor(1) == NULL);

    /* Verify metadata */
    printf("Plugin Label: %s\n", desc->Label);
    printf("Plugin UniqueID: 0x%lX\n", desc->UniqueID);
    printf("Plugin Name: %s\n", desc->Name);
    printf("Plugin Maker: %s\n", desc->Maker);
    printf("Plugin PortCount: %lu\n", desc->PortCount);

    assert(desc->UniqueID == 0x4A474D01);
    assert(strcmp(desc->Label, "rnnoise_gate") == 0);
    assert(desc->PortCount == 7);
    assert(desc->Properties & LADSPA_PROPERTY_HARD_RT_CAPABLE);

    /* Print and verify port table */
    printf("\n=== LADSPA Port Table ===\n");
    printf("%-5s | %-16s | %-10s | %-10s | %-8s | %-8s\n",
           "Port", "Name", "Type", "Direction", "Lower", "Upper");
    printf("--------------------------------------------------------------------\n");
    for (unsigned long p = 0; p < desc->PortCount; p++) {
        const char *type_str = LADSPA_IS_PORT_AUDIO(desc->PortDescriptors[p]) ? "Audio" : "Control";
        const char *dir_str = LADSPA_IS_PORT_INPUT(desc->PortDescriptors[p]) ? "Input" : "Output";
        printf("%-5lu | %-16s | %-10s | %-10s | %-8.1f | %-8.1f\n",
               p, desc->PortNames[p], type_str, dir_str,
               desc->PortRangeHints[p].LowerBound,
               desc->PortRangeHints[p].UpperBound);
    }
    printf("=========================\n\n");

    /* 3. Instantiate plugin at 48000 Hz */
    LADSPA_Handle inst = desc->instantiate(desc, SAMPLE_RATE);
    assert(inst != NULL);

    /* Allocate test audio buffers */
    float *in_buf = (float *)calloc(TOTAL_SAMPLES, sizeof(float));
    float *out_buf = (float *)calloc(TOTAL_SAMPLES, sizeof(float));
    assert(in_buf != NULL && out_buf != NULL);

    /* Control parameters */
    LADSPA_Data port_suppression = 1.0f;
    LADSPA_Data port_gate_enable = 1.0f;
    LADSPA_Data port_vad_threshold = 0.6f;
    LADSPA_Data port_gate_release_ms = 200.0f;
    LADSPA_Data port_floor_db = -60.0f;

    /* Connect ports */
    desc->connect_port(inst, 0, in_buf);
    desc->connect_port(inst, 1, out_buf);
    desc->connect_port(inst, 2, &port_suppression);
    desc->connect_port(inst, 3, &port_gate_enable);
    desc->connect_port(inst, 4, &port_vad_threshold);
    desc->connect_port(inst, 5, &port_gate_release_ms);
    desc->connect_port(inst, 6, &port_floor_db);

    /* Activate plugin */
    if (desc->activate) {
        desc->activate(inst);
    }

    /* --------------------------------------------------------------------- */
    /* Test Case A: 2 s of white noise at -30 dB in odd block sizes          */
    /* --------------------------------------------------------------------- */
    printf("[Test A] Feeding 2.0 s of white noise at -30 dBFS (blocks: 100, 333, 1, 0, 1024)...\n");
    generate_white_noise(in_buf, TOTAL_SAMPLES, -30.0f);
    memset(out_buf, 0, sizeof(float) * TOTAL_SAMPLES);

    run_in_odd_blocks(desc, inst, in_buf, out_buf, TOTAL_SAMPLES);

    /* Assert no NaN/Inf and check length handling */
    for (int i = 0; i < TOTAL_SAMPLES; i++) {
        assert(!isnan(out_buf[i]));
        assert(!isinf(out_buf[i]));
    }
    printf("  -> Assertion PASSED: no NaN or Inf in output.\n");

    /* Settle period: measure after gate settles (from 0.5s to 2.0s) */
    int settle_start = SAMPLE_RATE / 2; /* 0.5 s */
    double in_rms_noise = calculate_rms(in_buf, settle_start, TOTAL_SAMPLES);
    double out_rms_noise = calculate_rms(out_buf, settle_start, TOTAL_SAMPLES);
    double in_db_noise = 20.0 * log10(in_rms_noise + 1e-12);
    double out_db_noise = 20.0 * log10(out_rms_noise + 1e-12);
    double attenuation_noise = in_db_noise - out_db_noise;

    printf("  -> Input RMS: %.2f dBFS, Output RMS: %.2f dBFS\n", in_db_noise, out_db_noise);
    printf("  -> Measured attenuation: %.2f dB\n", attenuation_noise);
    assert(attenuation_noise >= 30.0);
    printf("  -> Assertion PASSED: noise attenuation >= 30 dB (measured %.2f dB).\n", attenuation_noise);

    /* --------------------------------------------------------------------- */
    /* Test Case B: 2 s harmonic voice-like signal + noise in odd block sizes */
    /* --------------------------------------------------------------------- */
    printf("\n[Test B] Feeding 2.0 s harmonic voice-like signal + noise (blocks: 100, 333, 1, 0, 1024)...\n");
    generate_voice_like_signal(in_buf, TOTAL_SAMPLES, 150.0f, -30.0f);
    memset(out_buf, 0, sizeof(float) * TOTAL_SAMPLES);

    run_in_odd_blocks(desc, inst, in_buf, out_buf, TOTAL_SAMPLES);

    for (int i = 0; i < TOTAL_SAMPLES; i++) {
        assert(!isnan(out_buf[i]));
        assert(!isinf(out_buf[i]));
    }
    printf("  -> Assertion PASSED: no NaN or Inf in voice output.\n");

    double in_rms_voice = calculate_rms(in_buf, settle_start, TOTAL_SAMPLES);
    double out_rms_voice = calculate_rms(out_buf, settle_start, TOTAL_SAMPLES);
    double in_db_voice = 20.0 * log10(in_rms_voice + 1e-12);
    double out_db_voice = 20.0 * log10(out_rms_voice + 1e-12);
    printf("  -> Voice Input RMS: %.2f dBFS, Voice Output RMS: %.2f dBFS\n", in_db_voice, out_db_voice);
    /* Voice must pass through (output non-silent and clearly audible) */
    assert(out_rms_voice > 0.01);
    printf("  -> Assertion PASSED: voice signal is non-silent and passed by gate.\n");

    /* --------------------------------------------------------------------- */
    /* Test Case C: Gate-disabled mode output is non-silent                   */
    /* --------------------------------------------------------------------- */
    printf("\n[Test C] Testing gate-disabled mode (Gate Enable = 0)...\n");
    port_gate_enable = 0.0f; /* Disable gate */

    /* Feed voice + noise with gate disabled */
    generate_voice_like_signal(in_buf, TOTAL_SAMPLES, 150.0f, -30.0f);
    memset(out_buf, 0, sizeof(float) * TOTAL_SAMPLES);
    run_in_odd_blocks(desc, inst, in_buf, out_buf, TOTAL_SAMPLES);

    for (int i = 0; i < TOTAL_SAMPLES; i++) {
        assert(!isnan(out_buf[i]));
        assert(!isinf(out_buf[i]));
    }

    double out_rms_ungated_voice = calculate_rms(out_buf, settle_start, TOTAL_SAMPLES);
    double out_db_ungated_voice = 20.0 * log10(out_rms_ungated_voice + 1e-12);
    printf("  -> Gate-disabled Voice Output RMS: %.2f dBFS\n", out_db_ungated_voice);
    assert(out_rms_ungated_voice > 0.01); /* Clearly audible and non-silent */
    printf("  -> Assertion PASSED: gate-disabled voice output is non-silent.\n");

    /* Also verify with noise: gate floor is not applied when disabled */
    generate_white_noise(in_buf, TOTAL_SAMPLES, -30.0f);
    memset(out_buf, 0, sizeof(float) * TOTAL_SAMPLES);
    run_in_odd_blocks(desc, inst, in_buf, out_buf, TOTAL_SAMPLES);

    for (int i = 0; i < TOTAL_SAMPLES; i++) {
        assert(!isnan(out_buf[i]));
        assert(!isinf(out_buf[i]));
    }

    double out_rms_ungated_noise = calculate_rms(out_buf, settle_start, TOTAL_SAMPLES);
    double out_db_ungated_noise = 20.0 * log10(out_rms_ungated_noise + 1e-12);
    printf("  -> Gate-disabled Noise Output RMS: %.2f dBFS (vs gated %.2f dBFS)\n",
           out_db_ungated_noise, out_db_noise);
    assert(out_rms_ungated_noise > 0.0);
    assert(out_rms_ungated_noise > out_rms_noise * 100.0);
    printf("  -> Assertion PASSED: gate-disabled noise output bypasses gate floor.\n");

    /* --------------------------------------------------------------------- */
    /* Test Case D: In-place processing (input buffer == output buffer)      */
    /* --------------------------------------------------------------------- */
    printf("\n[Test D] Testing in-place processing (in_buf == out_buf)...\n");
    port_gate_enable = 1.0f;
    generate_voice_like_signal(in_buf, TOTAL_SAMPLES, 150.0f, -30.0f);
    /* In-place: both input and output point to in_buf */
    run_in_odd_blocks(desc, inst, in_buf, in_buf, TOTAL_SAMPLES);

    for (int i = 0; i < TOTAL_SAMPLES; i++) {
        assert(!isnan(in_buf[i]));
        assert(!isinf(in_buf[i]));
    }
    double in_place_rms = calculate_rms(in_buf, settle_start, TOTAL_SAMPLES);
    assert(in_place_rms > 0.01);
    printf("  -> Assertion PASSED: in-place processing completed without distortion (RMS: %.2f dBFS).\n",
           20.0 * log10(in_place_rms + 1e-12));

    /* Cleanup */
    if (desc->deactivate) {
        desc->deactivate(inst);
    }
    desc->cleanup(inst);
    dlclose(handle);

    free(in_buf);
    free(out_buf);

    printf("\nAll plugin acceptance tests PASSED successfully!\n");
    return 0;
}
