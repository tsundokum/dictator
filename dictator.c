/*
 * dictator — hold a hotkey to dictate, release to transcribe
 * Build: gcc -O2 -Wall -Wextra -o dictator dictator.c -lX11 -lasound -lcurl -lpthread
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>
#include <signal.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/XKBlib.h>
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
static atomic_int recording;       /* flag: 1 = keep recording */

static char api_key[1024];

/* ── Config ─────────────────────────────────────────────────────────── */

struct hotkey {
    char     key_name[64];    /* X11 keysym name, e.g. "F1" */
    unsigned mod_mask;        /* modifier mask, e.g. ShiftMask */
};

static struct {
    struct hotkey copy_key;   /* transcribe + clipboard only */
    struct hotkey paste_key;  /* transcribe + clipboard + Ctrl+V */
    int           notify;     /* 1 = show desktop notifications */
} cfg = {
    .copy_key  = { .key_name = "F1", .mod_mask = 0 },
    .paste_key = { .key_name = "F1", .mod_mask = ShiftMask },
    .notify    = 1,
};

/* ── Config file loader ─────────────────────────────────────────────── */

static void parse_hotkey(const char *val, struct hotkey *hk) {
    hk->mod_mask = 0;
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "%s", val);
    char *rest = tmp;
    for (;;) {
        if (strncasecmp(rest, "shift+", 6) == 0) {
            hk->mod_mask |= ShiftMask; rest += 6;
        } else if (strncasecmp(rest, "ctrl+", 5) == 0) {
            hk->mod_mask |= ControlMask; rest += 5;
        } else if (strncasecmp(rest, "control+", 8) == 0) {
            hk->mod_mask |= ControlMask; rest += 8;
        } else if (strncasecmp(rest, "alt+", 4) == 0) {
            hk->mod_mask |= Mod1Mask; rest += 4;
        } else if (strncasecmp(rest, "super+", 6) == 0) {
            hk->mod_mask |= Mod4Mask; rest += 6;
        } else {
            break;
        }
    }
    snprintf(hk->key_name, sizeof(hk->key_name), "%s", rest);
}

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

        if (strcmp(key, "copy_key") == 0) {
            parse_hotkey(val, &cfg.copy_key);
        } else if (strcmp(key, "paste_key") == 0) {
            parse_hotkey(val, &cfg.paste_key);
        } else if (strcmp(key, "notify") == 0) {
            cfg.notify = (strcmp(val, "true") == 0);
        }
        /* old "key" and "autopaste" entries silently ignored */
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
    /* Escape single quotes: replace ' with '\'' for safe shell interpolation */
    char safe[512];
    size_t j = 0;
    for (size_t i = 0; msg[i] && j + 4 < sizeof(safe); i++) {
        if (msg[i] == '\'') {
            memcpy(safe + j, "'\\''", 4);
            j += 4;
        } else {
            safe[j++] = msg[i];
        }
    }
    safe[j] = '\0';
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "notify-send -t 2000 'Dictator' '%s' 2>/dev/null &", safe);
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
    if (rate != SAMPLE_RATE) {
        fprintf(stderr, "dictator: ALSA rate %u != %d, aborting\n",
                rate, SAMPLE_RATE);
        snd_pcm_close(pcm);
        return NULL;
    }
    snd_pcm_uframes_t period = PERIOD_FRAMES;
    snd_pcm_hw_params_set_period_size_near(pcm, params, &period, NULL);

    if ((err = snd_pcm_hw_params(pcm, params)) < 0) {
        fprintf(stderr, "dictator: ALSA params: %s\n", snd_strerror(err));
        snd_pcm_close(pcm);
        return NULL;
    }

    pcm_pos = 0;
    while (recording && pcm_pos + period <= (snd_pcm_uframes_t)BUF_SAMPLES) {
        snd_pcm_sframes_t n = snd_pcm_readi(pcm, pcm_buf + pcm_pos, period);
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

static void paste_text(const char *text, int autopaste) {
    /* Copy to clipboard via xclip */
    FILE *p = popen("xclip -selection clipboard", "w");
    if (p) {
        fwrite(text, 1, strlen(text), p);
        pclose(p);
    }
    if (!autopaste) return;
    /* Small delay to ensure clipboard is set */
    usleep(50000);
    /* Simulate Ctrl+V */
    if (run("xdotool key --clearmodifiers ctrl+v")) { /* best effort */ }
}

/* ── Signal handling ─────────────────────────────────────────────────── */

static volatile sig_atomic_t quit;

static void handle_signal(int sig) {
    (void)sig;
    quit = 1;
}

/* ── X11 hotkey grab helpers ─────────────────────────────────────────── */

static const unsigned int lock_combos[] = {0, Mod2Mask, LockMask, Mod2Mask | LockMask};
#define N_LOCK_COMBOS (sizeof(lock_combos)/sizeof(lock_combos[0]))

static void grab_hotkey(Display *dpy, Window root, KeyCode kc, unsigned mod) {
    for (size_t i = 0; i < N_LOCK_COMBOS; i++)
        XGrabKey(dpy, kc, mod | lock_combos[i], root,
                 False, GrabModeAsync, GrabModeAsync);
}

static void ungrab_hotkey(Display *dpy, Window root, KeyCode kc, unsigned mod) {
    for (size_t i = 0; i < N_LOCK_COMBOS; i++)
        XUngrabKey(dpy, kc, mod | lock_combos[i], root);
}

static void print_hotkey(const struct hotkey *hk, char *buf, size_t len) {
    snprintf(buf, len, "%s%s%s%s%s",
             (hk->mod_mask & ShiftMask)   ? "Shift+" : "",
             (hk->mod_mask & ControlMask) ? "Ctrl+"  : "",
             (hk->mod_mask & Mod1Mask)    ? "Alt+"   : "",
             (hk->mod_mask & Mod4Mask)    ? "Super+" : "",
             hk->key_name);
}

/* ── Main ───────────────────────────────────────────────────────────── */

int main(void) {
    load_config();
    if (load_env() < 0) return 1;
    curl_global_init(CURL_GLOBAL_ALL);

    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) { fprintf(stderr, "dictator: cannot open display\n"); return 1; }
    XkbSetDetectableAutoRepeat(dpy, True, NULL);

    /* Resolve copy_key */
    KeySym copy_ks = XStringToKeysym(cfg.copy_key.key_name);
    if (copy_ks == NoSymbol) {
        fprintf(stderr, "dictator: unknown copy_key '%s'\n", cfg.copy_key.key_name);
        return 1;
    }
    /* Resolve paste_key */
    KeySym paste_ks = XStringToKeysym(cfg.paste_key.key_name);
    if (paste_ks == NoSymbol) {
        fprintf(stderr, "dictator: unknown paste_key '%s'\n", cfg.paste_key.key_name);
        return 1;
    }

    Window root = DefaultRootWindow(dpy);
    KeyCode copy_kc  = XKeysymToKeycode(dpy, copy_ks);
    KeyCode paste_kc = XKeysymToKeycode(dpy, paste_ks);

    grab_hotkey(dpy, root, copy_kc,  cfg.copy_key.mod_mask);
    grab_hotkey(dpy, root, paste_kc, cfg.paste_key.mod_mask);

    signal(SIGINT,  handle_signal);
    signal(SIGTERM, handle_signal);

    char copy_str[128], paste_str[128];
    print_hotkey(&cfg.copy_key,  copy_str,  sizeof(copy_str));
    print_hotkey(&cfg.paste_key, paste_str, sizeof(paste_str));
    printf("dictator: ready — hold %s to copy, %s to paste\n",
           copy_str, paste_str);

    pthread_t tid;
    int is_recording = 0;
    int active_autopaste = 0;  /* whether current recording should autopaste */
    KeyCode active_kc = 0;    /* keycode that started recording */

    while (!quit) {
        XEvent ev;
        XNextEvent(dpy, &ev);
        if (quit) break;

        if (ev.type == KeyPress && !is_recording) {
            /* Strip lock-key bits to match our configured modifiers */
            unsigned clean = ev.xkey.state & ~(Mod2Mask | LockMask);

            if (ev.xkey.keycode == paste_kc &&
                clean == cfg.paste_key.mod_mask) {
                active_autopaste = 1;
                active_kc = paste_kc;
            } else if (ev.xkey.keycode == copy_kc &&
                       clean == cfg.copy_key.mod_mask) {
                active_autopaste = 0;
                active_kc = copy_kc;
            } else {
                continue;
            }

            is_recording = 1;
            recording = 1;
            notify("Recording...");
            if (pthread_create(&tid, NULL, record_thread, NULL) != 0) {
                perror("dictator: pthread_create");
                notify("Failed to start recording");
                is_recording = 0;
                recording = 0;
                continue;
            }
        }
        else if (ev.type == KeyRelease && is_recording &&
                 ev.xkey.keycode == active_kc) {
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
                paste_text(text, active_autopaste);
                notify(active_autopaste ? "Done — pasted"
                                        : "Done — copied to clipboard");
                printf("dictator: %s\n", text);
                free(text);
            } else {
                notify("No text returned");
                free(text);
            }
        }
    }

    if (is_recording) {
        recording = 0;
        pthread_join(tid, NULL);
    }
    ungrab_hotkey(dpy, root, copy_kc,  cfg.copy_key.mod_mask);
    ungrab_hotkey(dpy, root, paste_kc, cfg.paste_key.mod_mask);
    curl_global_cleanup();
    XCloseDisplay(dpy);
    printf("dictator: shutdown\n");
    return 0;
}
