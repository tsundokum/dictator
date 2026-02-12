/*
 * dictator — hold Shift+F1 to dictate, release to transcribe and paste
 * Build: gcc -O2 -Wall -Wextra -o dictator dictator.c -lX11 -lasound -lcurl -lpthread
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <alsa/asoundlib.h>
#include <curl/curl.h>

/* Audio config: 16kHz mono 16-bit — Whisper sweet spot */
#define SAMPLE_RATE  16000
#define CHANNELS     1
#define FRAME_SIZE   2              /* 16-bit = 2 bytes */
#define MAX_SECONDS  60
#define BUF_SAMPLES  (SAMPLE_RATE * MAX_SECONDS)
#define BUF_BYTES    (BUF_SAMPLES * FRAME_SIZE)
#define PERIOD_FRAMES 1024

static int16_t  pcm_buf[BUF_SAMPLES];
static size_t   pcm_pos;           /* samples written */
static volatile int recording;     /* flag: 1 = keep recording */

static char api_key[1024];

/* ── Config ─────────────────────────────────────────────────────────── */

static struct {
    char     key_name[64];    /* X11 keysym name, e.g. "F1" */
    unsigned mod_mask;        /* modifier mask, e.g. ShiftMask */
    int      notify;          /* 1 = show desktop notifications */
    int      autopaste;       /* 1 = ctrl+v after clipboard copy */
} cfg = {
    .key_name  = "F1",
    .mod_mask  = 0,
    .notify    = 1,
    .autopaste = 1,
};

/* ── Config file loader ─────────────────────────────────────────────── */

static int load_config_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        /* strip trailing whitespace */
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'
                           || line[len-1] == ' '  || line[len-1] == '\t'))
            line[--len] = '\0';

        /* skip blank lines and comments */
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0' || *p == '#') continue;

        /* split on '=' */
        char *eq = strchr(p, '=');
        if (!eq) continue;

        /* key: trim trailing spaces before '=' */
        char *kend = eq - 1;
        while (kend > p && (*kend == ' ' || *kend == '\t')) kend--;
        *(kend + 1) = '\0';
        char *key = p;

        /* value: trim leading spaces after '=' */
        char *val = eq + 1;
        while (*val == ' ' || *val == '\t') val++;

        if (strcmp(key, "key") == 0) {
            cfg.mod_mask = 0;
            /* parse modifier prefixes */
            char tmp[64];
            snprintf(tmp, sizeof(tmp), "%s", val);
            char *rest = tmp;
            for (;;) {
                if (strncasecmp(rest, "shift+", 6) == 0) {
                    cfg.mod_mask |= ShiftMask; rest += 6;
                } else if (strncasecmp(rest, "ctrl+", 5) == 0) {
                    cfg.mod_mask |= ControlMask; rest += 5;
                } else if (strncasecmp(rest, "control+", 8) == 0) {
                    cfg.mod_mask |= ControlMask; rest += 8;
                } else if (strncasecmp(rest, "alt+", 4) == 0) {
                    cfg.mod_mask |= Mod1Mask; rest += 4;
                } else if (strncasecmp(rest, "super+", 6) == 0) {
                    cfg.mod_mask |= Mod4Mask; rest += 6;
                } else {
                    break;
                }
            }
            snprintf(cfg.key_name, sizeof(cfg.key_name), "%s", rest);
        } else if (strcmp(key, "notify") == 0) {
            cfg.notify = (strcmp(val, "true") == 0);
        } else if (strcmp(key, "autopaste") == 0) {
            cfg.autopaste = (strcmp(val, "true") == 0);
        }
    }
    fclose(f);
    return 0;
}

static void load_config(void) {
    load_config_file("/etc/dictator.conf"); /* missing is fine — defaults apply */
}

/* ── .env loader ────────────────────────────────────────────────────── */

static int load_env(void) {
    FILE *f = fopen(".env", "r");
    if (!f) { fprintf(stderr, "dictator: cannot open .env\n"); return -1; }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "GROQ=", 5) == 0) {
            char *val = line + 5;
            val[strcspn(val, "\r\n")] = '\0';
            snprintf(api_key, sizeof(api_key), "Authorization: Bearer %s", val);
            fclose(f);
            return 0;
        }
    }
    fclose(f);
    fprintf(stderr, "dictator: GROQ= not found in .env\n");
    return -1;
}

/* ── Notify helper ──────────────────────────────────────────────────── */

__attribute__((warn_unused_result))
static int run(const char *cmd) { return system(cmd); }

static void notify(const char *msg) {
    if (!cfg.notify) return;
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "notify-send -t 2000 'Dictator' '%s' 2>/dev/null &", msg);
    if (run(cmd)) { /* best effort */ }
}

/* ── ALSA recording thread ──────────────────────────────────────────── */

static void *record_thread(void *arg) {
    (void)arg;
    snd_pcm_t *pcm;
    int err;

    if ((err = snd_pcm_open(&pcm, "default", SND_PCM_STREAM_CAPTURE, 0)) < 0) {
        fprintf(stderr, "dictator: ALSA open: %s\n", snd_strerror(err));
        return NULL;
    }

    snd_pcm_hw_params_t *params;
    snd_pcm_hw_params_alloca(&params);
    snd_pcm_hw_params_any(pcm, params);
    snd_pcm_hw_params_set_access(pcm, params, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(pcm, params, SND_PCM_FORMAT_S16_LE);
    snd_pcm_hw_params_set_channels(pcm, params, CHANNELS);
    unsigned int rate = SAMPLE_RATE;
    snd_pcm_hw_params_set_rate_near(pcm, params, &rate, NULL);
    snd_pcm_uframes_t period = PERIOD_FRAMES;
    snd_pcm_hw_params_set_period_size_near(pcm, params, &period, NULL);

    if ((err = snd_pcm_hw_params(pcm, params)) < 0) {
        fprintf(stderr, "dictator: ALSA params: %s\n", snd_strerror(err));
        snd_pcm_close(pcm);
        return NULL;
    }

    pcm_pos = 0;
    while (recording && pcm_pos + PERIOD_FRAMES <= BUF_SAMPLES) {
        snd_pcm_sframes_t n = snd_pcm_readi(pcm, pcm_buf + pcm_pos, PERIOD_FRAMES);
        if (n == -EPIPE) {
            snd_pcm_prepare(pcm);
            continue;
        }
        if (n < 0) {
            fprintf(stderr, "dictator: ALSA read: %s\n", snd_strerror((int)n));
            break;
        }
        pcm_pos += (size_t)n;
    }

    snd_pcm_close(pcm);
    return NULL;
}

/* ── WAV builder (in-memory) ────────────────────────────────────────── */

static size_t build_wav(uint8_t **out) {
    size_t data_bytes = pcm_pos * FRAME_SIZE;
    size_t total = 44 + data_bytes;
    uint8_t *wav = malloc(total);
    if (!wav) return 0;

    uint32_t u32;
    uint16_t u16;

    memcpy(wav, "RIFF", 4);
    u32 = (uint32_t)(total - 8);    memcpy(wav + 4, &u32, 4);
    memcpy(wav + 8, "WAVE", 4);
    memcpy(wav + 12, "fmt ", 4);
    u32 = 16;                        memcpy(wav + 16, &u32, 4);
    u16 = 1;                         memcpy(wav + 20, &u16, 2); /* PCM */
    u16 = CHANNELS;                  memcpy(wav + 22, &u16, 2);
    u32 = SAMPLE_RATE;               memcpy(wav + 24, &u32, 4);
    u32 = SAMPLE_RATE * CHANNELS * FRAME_SIZE;
                                     memcpy(wav + 28, &u32, 4); /* byte rate */
    u16 = CHANNELS * FRAME_SIZE;     memcpy(wav + 32, &u16, 2); /* block align */
    u16 = 16;                        memcpy(wav + 34, &u16, 2); /* bits/sample */
    memcpy(wav + 36, "data", 4);
    u32 = (uint32_t)data_bytes;      memcpy(wav + 40, &u32, 4);
    memcpy(wav + 44, pcm_buf, data_bytes);

    *out = wav;
    return total;
}

/* ── curl write callback ────────────────────────────────────────────── */

struct response { char *data; size_t len; };

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *userp) {
    size_t bytes = size * nmemb;
    struct response *r = userp;
    char *tmp = realloc(r->data, r->len + bytes + 1);
    if (!tmp) return 0;
    r->data = tmp;
    memcpy(r->data + r->len, ptr, bytes);
    r->len += bytes;
    r->data[r->len] = '\0';
    return bytes;
}

/* ── Groq Whisper API call ──────────────────────────────────────────── */

static char *transcribe(uint8_t *wav, size_t wav_len) {
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, api_key);

    curl_mime *mime = curl_mime_init(curl);

    curl_mimepart *part = curl_mime_addpart(mime);
    curl_mime_name(part, "file");
    curl_mime_data(part, (char *)wav, wav_len);
    curl_mime_filename(part, "audio.wav");
    curl_mime_type(part, "audio/wav");

    part = curl_mime_addpart(mime);
    curl_mime_name(part, "model");
    curl_mime_data(part, "whisper-large-v3-turbo", CURL_ZERO_TERMINATED);

    part = curl_mime_addpart(mime);
    curl_mime_name(part, "response_format");
    curl_mime_data(part, "text", CURL_ZERO_TERMINATED);

    struct response resp = {0};

    curl_easy_setopt(curl, CURLOPT_URL,
                     "https://api.groq.com/openai/v1/audio/transcriptions");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    CURLcode res = curl_easy_perform(curl);

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_mime_free(mime);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        fprintf(stderr, "dictator: curl: %s\n", curl_easy_strerror(res));
        free(resp.data);
        return NULL;
    }
    if (http_code != 200) {
        char msg[256];
        snprintf(msg, sizeof(msg), "API error %ld", http_code);
        notify(msg);
        fprintf(stderr, "dictator: %s: %s\n", msg, resp.data ? resp.data : "");
        free(resp.data);
        return NULL;
    }
    /* Trim trailing whitespace */
    if (resp.data) {
        size_t len = strlen(resp.data);
        while (len > 0 && (resp.data[len-1] == '\n' || resp.data[len-1] == '\r'
                           || resp.data[len-1] == ' '))
            resp.data[--len] = '\0';
    }
    return resp.data;
}

/* ── Clipboard + paste ──────────────────────────────────────────────── */

static void paste_text(const char *text) {
    /* Copy to clipboard via xclip */
    FILE *p = popen("xclip -selection clipboard", "w");
    if (p) {
        fwrite(text, 1, strlen(text), p);
        pclose(p);
    }
    if (!cfg.autopaste) return;
    /* Small delay to ensure clipboard is set */
    usleep(50000);
    /* Simulate Ctrl+V */
    if (run("xdotool key --clearmodifiers ctrl+v")) { /* best effort */ }
}

/* ── Main ───────────────────────────────────────────────────────────── */

int main(void) {
    load_config();
    if (load_env() < 0) return 1;
    curl_global_init(CURL_GLOBAL_ALL);

    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) { fprintf(stderr, "dictator: cannot open display\n"); return 1; }

    KeySym ks = XStringToKeysym(cfg.key_name);
    if (ks == NoSymbol) {
        fprintf(stderr, "dictator: unknown key name '%s'\n", cfg.key_name);
        return 1;
    }

    Window root = DefaultRootWindow(dpy);
    KeyCode kc = XKeysymToKeycode(dpy, ks);

    /* Grab hotkey with lock-key combos (NumLock=Mod2, CapsLock=Lock) */
    unsigned int locks[] = {0, Mod2Mask, LockMask, Mod2Mask | LockMask};
    for (size_t i = 0; i < sizeof(locks)/sizeof(locks[0]); i++)
        XGrabKey(dpy, kc, cfg.mod_mask | locks[i], root,
                 False, GrabModeAsync, GrabModeAsync);

    printf("dictator: ready — hold %s%s%s%s%s to record\n",
           (cfg.mod_mask & ShiftMask)   ? "Shift+" : "",
           (cfg.mod_mask & ControlMask) ? "Ctrl+"  : "",
           (cfg.mod_mask & Mod1Mask)    ? "Alt+"   : "",
           (cfg.mod_mask & Mod4Mask)    ? "Super+" : "",
           cfg.key_name);

    pthread_t tid;
    int is_recording = 0;

    for (;;) {
        XEvent ev;
        XNextEvent(dpy, &ev);

        if (ev.type == KeyPress && !is_recording) {
            is_recording = 1;
            recording = 1;
            notify("Recording...");
            pthread_create(&tid, NULL, record_thread, NULL);
        }
        else if (ev.type == KeyRelease && is_recording) {
            /* Filter auto-repeat: if next event is KeyPress of same key, skip */
            if (XPending(dpy)) {
                XEvent next;
                XPeekEvent(dpy, &next);
                if (next.type == KeyPress && next.xkey.keycode == ev.xkey.keycode
                    && next.xkey.time == ev.xkey.time) {
                    XNextEvent(dpy, &next); /* consume the repeat press */
                    continue;
                }
            }

            recording = 0;
            pthread_join(tid, NULL);
            is_recording = 0;

            if (pcm_pos == 0) {
                notify("No audio captured");
                continue;
            }

            printf("dictator: captured %zu samples (%.1fs)\n",
                   pcm_pos, (double)pcm_pos / SAMPLE_RATE);

            uint8_t *wav;
            size_t wav_len = build_wav(&wav);
            if (!wav_len) { notify("WAV build failed"); continue; }

            char *text = transcribe(wav, wav_len);
            free(wav);

            if (text && strlen(text) > 0) {
                paste_text(text);
                notify("Done — copied to clipboard");
                printf("dictator: %s\n", text);
                free(text);
            } else {
                notify("No text returned");
                free(text);
            }
        }
    }

    /* unreachable, but for completeness */
    curl_global_cleanup();
    XCloseDisplay(dpy);
    return 0;
}
