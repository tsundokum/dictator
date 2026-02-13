/*
 * test_e2e — end-to-end integration test: mp3 → PCM → chunked WAV → Groq API
 *
 * Converts test.mp3 to raw PCM, runs it through the chunked transcription
 * pipeline, and compares the result against the reference text in test.txt.
 *
 * Requires: ffmpeg, GROQ key in .env, network access, test.mp3, test.txt
 * Build: make test_e2e
 * Run:   ./test_e2e       (NOT part of `make test` — use `make e2e`)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>

#define main dictator_main
#include "dictator.c"
#undef main

static int tests_run, tests_failed;

#define ASSERT(cond, msg) do { \
    tests_run++; \
    if (!(cond)) { \
        fprintf(stderr, "  FAIL: %s (line %d)\n", msg, __LINE__); \
        tests_failed++; \
    } \
} while (0)

/* ── Word tokenizer ──────────────────────────────────────────────────── */

/* Split text into lowercase alpha-only words */
static int tokenize(const char *text, char words[][64], int max_words) {
    int count = 0;
    const char *p = text;
    while (*p && count < max_words) {
        while (*p && !isalpha((unsigned char)*p)) p++;
        if (!*p) break;
        int len = 0;
        while (*p && isalpha((unsigned char)*p) && len < 63) {
            words[count][len++] = (char)tolower((unsigned char)*p);
            p++;
        }
        words[count][len] = '\0';
        count++;
    }
    return count;
}

/* Count how many unique words from `ref` appear anywhere in `result` */
static int count_word_matches(char ref[][64], int nref,
                              char result[][64], int nresult) {
    int matches = 0;
    for (int i = 0; i < nref; i++) {
        for (int j = 0; j < nresult; j++) {
            if (strcmp(ref[i], result[j]) == 0) {
                matches++;
                break;
            }
        }
    }
    return matches;
}

/* ── Load reference text from file ───────────────────────────────────── */

static char *load_text_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

/* ── Load PCM from mp3 via ffmpeg ────────────────────────────────────── */

static size_t load_pcm_from_mp3(const char *mp3_path) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "ffmpeg -y -i '%s' -ar %d -ac %d -f s16le -loglevel error pipe:1",
             mp3_path, SAMPLE_RATE, CHANNELS);

    FILE *p = popen(cmd, "r");
    if (!p) {
        fprintf(stderr, "test_e2e: failed to run ffmpeg\n");
        return 0;
    }

    size_t total = 0;
    size_t max_bytes = (size_t)BUF_SAMPLES * FRAME_SIZE;
    uint8_t *buf = (uint8_t *)pcm_buf;

    while (total < max_bytes) {
        size_t n = fread(buf + total, 1, max_bytes - total, p);
        if (n == 0) break;
        total += n;
    }
    pclose(p);

    size_t samples = total / FRAME_SIZE;
    printf("test_e2e: loaded %zu samples (%.1fs) from %s (max %ds)\n",
           samples, (double)samples / SAMPLE_RATE, mp3_path, MAX_SECONDS);
    return samples;
}

/* ── Chunked transcription (mirrors handle_recording_done logic) ─────── */

static char *chunked_transcribe(size_t num_samples) {
    static char result[16384];
    result[0] = '\0';
    size_t result_len = 0;

    size_t nchunks = (num_samples + CHUNK_SAMPLES - 1) / CHUNK_SAMPLES;
    printf("test_e2e: %zu chunk(s) to transcribe\n", nchunks);

    for (size_t i = 0; i < nchunks; i++) {
        size_t offset = i * CHUNK_SAMPLES;
        size_t chunk_samples = num_samples - offset;
        if (chunk_samples > CHUNK_SAMPLES) chunk_samples = CHUNK_SAMPLES;

        uint8_t *wav;
        size_t wav_len = build_wav(pcm_buf + offset, chunk_samples, &wav);
        if (!wav_len) {
            fprintf(stderr, "test_e2e: build_wav failed for chunk %zu\n", i);
            return NULL;
        }

        printf("test_e2e: chunk %zu/%zu (%.1fs)...",
               i + 1, nchunks, (double)chunk_samples / SAMPLE_RATE);
        fflush(stdout);

        char *text = transcribe(wav, wav_len);
        free(wav);

        if (text && strlen(text) > 0) {
            printf(" %zu chars\n", strlen(text));
            if (result_len > 0 && result_len + 1 < sizeof(result)) {
                result[result_len++] = ' ';
            }
            size_t tlen = strlen(text);
            if (result_len + tlen < sizeof(result)) {
                memcpy(result + result_len, text, tlen);
                result_len += tlen;
                result[result_len] = '\0';
            }
            free(text);
        } else {
            printf(" (empty)\n");
            free(text);
        }
    }

    return result;
}

/* ── Main ───────────────────────────────────────────────────────────── */

int main(void) {
    /* Load reference text */
    char *ref_text = load_text_file("test.txt");
    if (!ref_text) {
        fprintf(stderr, "test_e2e: cannot open test.txt\n");
        return 1;
    }

    /* Load API key */
    if (load_env() < 0) {
        fprintf(stderr, "test_e2e: cannot load .env (need GROQ= key)\n");
        free(ref_text);
        return 1;
    }
    cfg.notify = 0; /* suppress desktop notifications */
    curl_global_init(CURL_GLOBAL_ALL);

    /* Convert mp3 → PCM (capped at BUF_SAMPLES) */
    size_t samples = load_pcm_from_mp3("test.mp3");
    ASSERT(samples > 0, "loaded PCM from mp3");
    if (samples == 0) goto done;

    /* Transcribe via chunked pipeline */
    char *result = chunked_transcribe(samples);
    ASSERT(result != NULL, "transcription returned non-NULL");
    if (!result) goto done;

    ASSERT(strlen(result) > 50, "transcription has substantial text");
    printf("\ntest_e2e: result (%zu chars):\n  %.200s...\n\n",
           strlen(result), result);

    /* Tokenize reference and result */
    static char ref_words[2048][64];
    int nref = tokenize(ref_text, ref_words, 2048);

    /* Tokenize transcription result */
    static char res_words[2048][64];
    int nres = tokenize(result, res_words, 2048);

    printf("test_e2e: reference words: %d, result words: %d\n", nref, nres);
    ASSERT(nres > 20, "result has >20 words");

    /* Word overlap: check both directions */
    int forward = count_word_matches(res_words, nres, ref_words, nref);
    double fwd_pct = nres > 0 ? (double)forward / nres * 100.0 : 0;
    printf("test_e2e: result words found in reference: %d/%d (%.0f%%)\n",
           forward, nres, fwd_pct);

    /* Also check ref→result for the words we'd expect */
    int reverse = count_word_matches(ref_words, nref, res_words, nres);
    double rev_pct = nref > 0 ? (double)reverse / nref * 100.0 : 0;
    printf("test_e2e: reference words found in result: %d/%d (%.0f%%)\n",
           reverse, nref, rev_pct);

    /* Both directions should have strong overlap */
    ASSERT(fwd_pct >= 95.0,
           ">=95%% of transcribed words appear in reference text");
    ASSERT(rev_pct >= 95.0,
           ">=95%% of reference words appear in transcription");

done:
    free(ref_text);
    curl_global_cleanup();
    printf("\n%d tests, %d failed\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
