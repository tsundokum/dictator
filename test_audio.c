/*
 * test_audio — unit tests for WAV building and audio chunking
 * Build: make test_audio
 * Run:   ./test_audio
 *
 * Strategy: #define transcribe/paste_text before including dictator.c
 * so the real implementations get compiled under mock names. We then
 * provide wrapper functions that the rest of dictator.c calls via the
 * #define'd names — but since dictator.c's own definitions already
 * became our mock names, we instead just redefine the function bodies
 * by not providing separate definitions and letting the #define redirect
 * dictator.c's code. Then we override by using linker-level tricks.
 *
 * Actually: we use a simpler approach. We #define TESTING_AUDIO to
 * guard out the real transcribe/paste_text in dictator.c, then provide
 * our own mock versions.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ── Mock tracking state ─────────────────────────────────────────────── */

static int    mock_transcribe_calls;
static int    mock_paste_calls;
static char   mock_paste_buf[16384];
static int    mock_paste_autopaste;

/* Include dictator.c with main renamed */
#define main dictator_main
#include "dictator.c"
#undef main

/*
 * The above includes the real transcribe() and paste_text() but that's
 * fine — we won't call them directly. Instead, we'll test build_wav()
 * directly and test the chunking logic by reimplementing the core loop
 * from handle_recording_done() with our mocks.
 */

/* ── Mock implementations ────────────────────────────────────────────── */

static char *test_transcribe(uint8_t *wav, size_t wav_len) {
    mock_transcribe_calls++;

    /* Validate WAV header */
    if (wav_len < 44) return NULL;
    if (memcmp(wav, "RIFF", 4) != 0) return NULL;
    if (memcmp(wav + 8, "WAVE", 4) != 0) return NULL;
    if (memcmp(wav + 12, "fmt ", 4) != 0) return NULL;
    if (memcmp(wav + 36, "data", 4) != 0) return NULL;

    char *result = malloc(32);
    if (!result) return NULL;
    snprintf(result, 32, "chunk%d", mock_transcribe_calls);
    return result;
}

static void test_paste_text(const char *text, int autopaste) {
    mock_paste_calls++;
    snprintf(mock_paste_buf, sizeof(mock_paste_buf), "%s", text);
    mock_paste_autopaste = autopaste;
}

/*
 * Reimplementation of handle_recording_done() chunking logic using mocks.
 * This mirrors the real function but calls test_transcribe/test_paste_text.
 */
static void mock_handle_recording_done(int autopaste) {
    if (pcm_pos == 0) return;

    size_t nchunks = (pcm_pos + CHUNK_SAMPLES - 1) / CHUNK_SAMPLES;
    char result[16384] = "";
    size_t result_len = 0;

    for (size_t i = 0; i < nchunks; i++) {
        size_t offset = i * CHUNK_SAMPLES;
        size_t chunk_samples = pcm_pos - offset;
        if (chunk_samples > CHUNK_SAMPLES) chunk_samples = CHUNK_SAMPLES;

        uint8_t *wav;
        size_t wav_len = build_wav(pcm_buf + offset, chunk_samples, &wav);
        if (!wav_len) return;

        char *text = test_transcribe(wav, wav_len);
        free(wav);

        if (text && strlen(text) > 0) {
            if (result_len > 0 && result_len + 1 < sizeof(result)) {
                result[result_len++] = ' ';
            }
            size_t tlen = strlen(text);
            if (result_len + tlen < sizeof(result)) {
                memcpy(result + result_len, text, tlen);
                result_len += tlen;
                result[result_len] = '\0';
            }
        }
        free(text);
    }

    if (result_len > 0) {
        test_paste_text(result, autopaste);
    }
}

/* ── Test harness ────────────────────────────────────────────────────── */

static int tests_run, tests_failed;

#define ASSERT(cond, msg) do { \
    tests_run++; \
    if (!(cond)) { \
        fprintf(stderr, "  FAIL: %s (line %d)\n", msg, __LINE__); \
        tests_failed++; \
    } \
} while (0)

static void reset_mocks(void) {
    mock_transcribe_calls = 0;
    mock_paste_calls = 0;
    mock_paste_buf[0] = '\0';
    mock_paste_autopaste = -1;
    pcm_pos = 0;
    memset(pcm_buf, 0, sizeof(pcm_buf));
}

/* ── build_wav tests ─────────────────────────────────────────────────── */

static void test_build_wav_1s(void) {
    printf("test_build_wav_1s\n");
    reset_mocks();

    size_t num_samples = SAMPLE_RATE; /* 1 second */
    int16_t *samples = calloc(num_samples, sizeof(int16_t));
    uint8_t *wav = NULL;
    size_t wav_len = build_wav(samples, num_samples, &wav);

    size_t expected = 44 + num_samples * FRAME_SIZE;
    ASSERT(wav_len == expected, "total length = 44 + data");
    ASSERT(wav != NULL, "wav not NULL");
    ASSERT(memcmp(wav, "RIFF", 4) == 0, "RIFF magic");
    ASSERT(memcmp(wav + 8, "WAVE", 4) == 0, "WAVE magic");
    ASSERT(memcmp(wav + 12, "fmt ", 4) == 0, "fmt chunk");
    ASSERT(memcmp(wav + 36, "data", 4) == 0, "data chunk");

    /* Check sample rate field at offset 24 */
    uint32_t sr;
    memcpy(&sr, wav + 24, 4);
    ASSERT(sr == SAMPLE_RATE, "sample rate matches");

    /* Check RIFF size = total - 8 */
    uint32_t riff_size;
    memcpy(&riff_size, wav + 4, 4);
    ASSERT(riff_size == wav_len - 8, "RIFF size correct");

    /* Check data size */
    uint32_t data_size;
    memcpy(&data_size, wav + 40, 4);
    ASSERT(data_size == num_samples * FRAME_SIZE, "data size correct");

    free(wav);
    free(samples);
}

static void test_build_wav_chunk_size(void) {
    printf("test_build_wav_chunk_size\n");
    reset_mocks();

    size_t num_samples = CHUNK_SAMPLES; /* 30 seconds */
    int16_t *samples = calloc(num_samples, sizeof(int16_t));
    uint8_t *wav = NULL;
    size_t wav_len = build_wav(samples, num_samples, &wav);

    size_t expected = 44 + CHUNK_SAMPLES * FRAME_SIZE;
    ASSERT(wav_len == expected, "30s chunk WAV size correct");

    uint32_t data_size;
    memcpy(&data_size, wav + 40, 4);
    ASSERT(data_size == CHUNK_SAMPLES * FRAME_SIZE, "30s data size");

    free(wav);
    free(samples);
}

static void test_build_wav_single_sample(void) {
    printf("test_build_wav_single_sample\n");
    reset_mocks();

    int16_t sample = 12345;
    uint8_t *wav = NULL;
    size_t wav_len = build_wav(&sample, 1, &wav);

    ASSERT(wav_len == 44 + 2, "single sample WAV = 46 bytes");
    ASSERT(memcmp(wav, "RIFF", 4) == 0, "RIFF magic");

    /* Verify sample data is preserved */
    int16_t out_sample;
    memcpy(&out_sample, wav + 44, 2);
    ASSERT(out_sample == 12345, "sample data preserved");

    free(wav);
}

/* ── Chunking tests ──────────────────────────────────────────────────── */

static void test_chunk_short_recording(void) {
    printf("test_chunk_short_recording\n");
    reset_mocks();

    pcm_pos = SAMPLE_RATE * 10; /* 10 seconds */
    mock_handle_recording_done(0);

    ASSERT(mock_transcribe_calls == 1, "short recording: 1 transcribe call");
    ASSERT(mock_paste_calls == 1, "short recording: 1 paste call");
    ASSERT(strcmp(mock_paste_buf, "chunk1") == 0, "short recording: correct text");
    ASSERT(mock_paste_autopaste == 0, "short recording: no autopaste");
}

static void test_chunk_exactly_30s(void) {
    printf("test_chunk_exactly_30s\n");
    reset_mocks();

    pcm_pos = CHUNK_SAMPLES; /* exactly 30s */
    mock_handle_recording_done(1);

    ASSERT(mock_transcribe_calls == 1, "30s: 1 transcribe call");
    ASSERT(mock_paste_calls == 1, "30s: 1 paste call");
    ASSERT(strcmp(mock_paste_buf, "chunk1") == 0, "30s: correct text");
    ASSERT(mock_paste_autopaste == 1, "30s: autopaste set");
}

static void test_chunk_45s(void) {
    printf("test_chunk_45s\n");
    reset_mocks();

    pcm_pos = SAMPLE_RATE * 45; /* 2 chunks: 30s + 15s */
    mock_handle_recording_done(0);

    ASSERT(mock_transcribe_calls == 2, "45s: 2 transcribe calls");
    ASSERT(mock_paste_calls == 1, "45s: 1 paste call");
    ASSERT(strcmp(mock_paste_buf, "chunk1 chunk2") == 0, "45s: space-concatenated");
}

static void test_chunk_60s(void) {
    printf("test_chunk_60s\n");
    reset_mocks();

    pcm_pos = SAMPLE_RATE * 60; /* 60s = 2 chunks of 30s */
    mock_handle_recording_done(1);

    ASSERT(mock_transcribe_calls == 2, "60s: 2 transcribe calls");
    ASSERT(mock_paste_calls == 1, "60s: 1 paste call");
    ASSERT(strcmp(mock_paste_buf, "chunk1 chunk2") == 0, "60s: space-concatenated");
    ASSERT(mock_paste_autopaste == 1, "60s: autopaste set");
}

static void test_chunk_300s(void) {
    printf("test_chunk_300s\n");
    reset_mocks();

    cfg.max_duration = 300;
    pcm_pos = BUF_SAMPLES; /* 300s = 10 chunks of 30s */
    mock_handle_recording_done(1);

    ASSERT(mock_transcribe_calls == 10, "300s: 10 transcribe calls");
    ASSERT(mock_paste_calls == 1, "300s: 1 paste call");
    ASSERT(mock_paste_autopaste == 1, "300s: autopaste set");
}

static void test_chunk_zero_samples(void) {
    printf("test_chunk_zero_samples\n");
    reset_mocks();

    pcm_pos = 0;
    mock_handle_recording_done(0);

    ASSERT(mock_transcribe_calls == 0, "zero: no transcribe calls");
    ASSERT(mock_paste_calls == 0, "zero: no paste calls");
}

static void test_chunk_autopaste_flag(void) {
    printf("test_chunk_autopaste_flag\n");

    /* autopaste=0 */
    reset_mocks();
    pcm_pos = SAMPLE_RATE;
    mock_handle_recording_done(0);
    ASSERT(mock_paste_autopaste == 0, "autopaste 0 passed through");

    /* autopaste=1 */
    reset_mocks();
    pcm_pos = SAMPLE_RATE;
    mock_handle_recording_done(1);
    ASSERT(mock_paste_autopaste == 1, "autopaste 1 passed through");
}

/* ── Main ───────────────────────────────────────────────────────────── */

int main(void) {
    /* build_wav tests */
    test_build_wav_1s();
    test_build_wav_chunk_size();
    test_build_wav_single_sample();

    /* chunking tests */
    test_chunk_short_recording();
    test_chunk_exactly_30s();
    test_chunk_45s();
    test_chunk_60s();
    test_chunk_300s();
    test_chunk_zero_samples();
    test_chunk_autopaste_flag();

    printf("\n%d tests, %d failed\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
