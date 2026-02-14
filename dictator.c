/*
 * dictator — hold a hotkey to dictate, release to transcribe
 * Build: gcc -O2 -Wall -Wextra -DUSE_X11 -DUSE_EVDEV -o dictator dictator.c -lX11 -lasound -lcurl -lpthread -levdev
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>
#include <signal.h>
#include <ctype.h>

#ifdef USE_X11
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/XKBlib.h>
#endif

#ifdef USE_EVDEV
#include <libevdev/libevdev.h>
#include <linux/input-event-codes.h>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#endif

#include <alsa/asoundlib.h>
#include <curl/curl.h>

/* Audio config: 16kHz mono 16-bit — Whisper sweet spot */
#define SAMPLE_RATE  16000
#define CHANNELS     1
#define FRAME_SIZE   2              /* 16-bit = 2 bytes */
#define MAX_SECONDS  300
#define BUF_SAMPLES  (SAMPLE_RATE * MAX_SECONDS)
#define BUF_BYTES    (BUF_SAMPLES * FRAME_SIZE)
#define PERIOD_FRAMES 1024
#define CHUNK_SECONDS 30
#define CHUNK_SAMPLES (SAMPLE_RATE * CHUNK_SECONDS)

static int16_t  pcm_buf[BUF_SAMPLES];
static size_t   pcm_pos;           /* samples written */
static atomic_int recording;       /* flag: 1 = keep recording */

static char api_key[1024];

/* ── Backend-agnostic modifier flags ──────────────────────────────── */

#define MOD_SHIFT  (1 << 0)
#define MOD_CTRL   (1 << 1)
#define MOD_ALT    (1 << 2)
#define MOD_SUPER  (1 << 3)

/* ── Backend detection ────────────────────────────────────────────── */

enum backend { BACKEND_X11, BACKEND_EVDEV };
static enum backend active_backend;

static enum backend detect_backend(void) {
    const char *st = getenv("XDG_SESSION_TYPE");
    if (st && strcmp(st, "wayland") == 0) {
#ifdef USE_EVDEV
        return BACKEND_EVDEV;
#else
        fprintf(stderr, "dictator: Wayland detected but built without evdev support\n");
        return BACKEND_X11;
#endif
    }
    return BACKEND_X11;
}

/* ── Config ─────────────────────────────────────────────────────────── */

struct hotkey {
    char     key_name[64];    /* key name, e.g. "F1" */
    unsigned mod_mask;        /* modifier mask using MOD_* flags */
};

static struct {
    struct hotkey copy_key;   /* transcribe + clipboard only */
    struct hotkey paste_key;  /* transcribe + clipboard + Ctrl+V */
    int           notify;     /* 1 = show desktop notifications */
    int           max_duration; /* recording limit in seconds */
} cfg = {
    .copy_key     = { .key_name = "F1", .mod_mask = 0 },
    .paste_key    = { .key_name = "F1", .mod_mask = MOD_SHIFT },
    .notify       = 1,
    .max_duration = MAX_SECONDS,
};

/* ── Config file loader ─────────────────────────────────────────────── */

static void parse_hotkey(const char *val, struct hotkey *hk) {
    hk->mod_mask = 0;
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "%s", val);
    char *rest = tmp;
    for (;;) {
        if (strncasecmp(rest, "shift+", 6) == 0) {
            hk->mod_mask |= MOD_SHIFT; rest += 6;
        } else if (strncasecmp(rest, "ctrl+", 5) == 0) {
            hk->mod_mask |= MOD_CTRL; rest += 5;
        } else if (strncasecmp(rest, "control+", 8) == 0) {
            hk->mod_mask |= MOD_CTRL; rest += 8;
        } else if (strncasecmp(rest, "alt+", 4) == 0) {
            hk->mod_mask |= MOD_ALT; rest += 4;
        } else if (strncasecmp(rest, "super+", 6) == 0) {
            hk->mod_mask |= MOD_SUPER; rest += 6;
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
        } else if (strcmp(key, "max_duration") == 0) {
            int v = atoi(val);
            if (v < 10) v = 10;
            if (v > MAX_SECONDS) v = MAX_SECONDS;
            cfg.max_duration = v;
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
    int warned = 0;
    size_t max_samples = (size_t)(SAMPLE_RATE * cfg.max_duration);
    while (recording && pcm_pos + period <= (snd_pcm_uframes_t)max_samples) {
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
        if (!warned && pcm_pos >= (size_t)(SAMPLE_RATE * (cfg.max_duration - 10))) {
            char msg[128];
            snprintf(msg, sizeof(msg),
                     "Recording limit approaching (max_duration=%ds)",
                     cfg.max_duration);
            notify(msg);
            warned = 1;
        }
    }
    if (pcm_pos + period > (snd_pcm_uframes_t)max_samples) {
        notify("Recording limit reached — set max_duration in /etc/dictator.conf to increase");
    }

    snd_pcm_close(pcm);
    return NULL;
}

/* ── WAV builder (in-memory) ────────────────────────────────────────── */

static size_t build_wav(int16_t *samples, size_t num_samples, uint8_t **out) {
    size_t data_bytes = num_samples * FRAME_SIZE;
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
    memcpy(wav + 44, samples, data_bytes);

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
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);

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
    /* Copy to both clipboard and primary selection */
    if (active_backend == BACKEND_EVDEV) {
        FILE *p = popen("wl-copy", "w");
        if (p) { fwrite(text, 1, strlen(text), p); pclose(p); }
        p = popen("wl-copy --primary", "w");
        if (p) { fwrite(text, 1, strlen(text), p); pclose(p); }
    } else {
        FILE *p = popen("xclip -selection clipboard", "w");
        if (p) { fwrite(text, 1, strlen(text), p); pclose(p); }
        p = popen("xclip -selection primary", "w");
        if (p) { fwrite(text, 1, strlen(text), p); pclose(p); }
    }
    if (!autopaste) return;
    /* Small delay to ensure clipboard is set */
    usleep(50000);
    /* Simulate Shift+Insert — works in both GUI apps and terminals */
    if (active_backend == BACKEND_EVDEV) {
        if (run("ydotool key 42:1 110:1 110:0 42:0")) { /* best effort */ }
    } else {
        if (run("xdotool key --clearmodifiers shift+Insert")) { /* best effort */ }
    }
}

/* ── Signal handling ─────────────────────────────────────────────────── */

static volatile sig_atomic_t quit;

static void handle_signal(int sig) {
    (void)sig;
    quit = 1;
}

/* ── Shared post-recording logic ─────────────────────────────────────── */

static void handle_recording_done(int autopaste) {
    if (pcm_pos == 0) {
        notify("No audio captured");
        return;
    }

    printf("dictator: captured %zu samples (%.1fs)\n",
           pcm_pos, (double)pcm_pos / SAMPLE_RATE);

    size_t nchunks = (pcm_pos + CHUNK_SAMPLES - 1) / CHUNK_SAMPLES;
    char result[16384] = "";
    size_t result_len = 0;

    for (size_t i = 0; i < nchunks; i++) {
        size_t offset = i * CHUNK_SAMPLES;
        size_t chunk_samples = pcm_pos - offset;
        if (chunk_samples > CHUNK_SAMPLES) chunk_samples = CHUNK_SAMPLES;

        uint8_t *wav;
        size_t wav_len = build_wav(pcm_buf + offset, chunk_samples, &wav);
        if (!wav_len) { notify("WAV build failed"); return; }

        char *text = transcribe(wav, wav_len);
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
        paste_text(result, autopaste);
        notify(autopaste ? "Done — pasted"
                         : "Done — copied to clipboard");
        printf("dictator: %s\n", result);
    } else {
        notify("No text returned");
    }
}

/* ── Hotkey display helper ───────────────────────────────────────────── */

static void print_hotkey(const struct hotkey *hk, char *buf, size_t len) {
    snprintf(buf, len, "%s%s%s%s%s",
             (hk->mod_mask & MOD_SHIFT) ? "Shift+" : "",
             (hk->mod_mask & MOD_CTRL)  ? "Ctrl+"  : "",
             (hk->mod_mask & MOD_ALT)   ? "Alt+"   : "",
             (hk->mod_mask & MOD_SUPER) ? "Super+" : "",
             hk->key_name);
}

/* ── X11 hotkey grab helpers ─────────────────────────────────────────── */

#ifdef USE_X11

static unsigned mod_to_x11(unsigned mod) {
    unsigned x = 0;
    if (mod & MOD_SHIFT) x |= ShiftMask;
    if (mod & MOD_CTRL)  x |= ControlMask;
    if (mod & MOD_ALT)   x |= Mod1Mask;
    if (mod & MOD_SUPER) x |= Mod4Mask;
    return x;
}

static const unsigned int lock_combos[] = {0, Mod2Mask, LockMask, Mod2Mask | LockMask};
#define N_LOCK_COMBOS (sizeof(lock_combos)/sizeof(lock_combos[0]))

static void grab_hotkey(Display *dpy, Window root, KeyCode kc, unsigned mod) {
    unsigned xmod = mod_to_x11(mod);
    for (size_t i = 0; i < N_LOCK_COMBOS; i++)
        XGrabKey(dpy, kc, xmod | lock_combos[i], root,
                 False, GrabModeAsync, GrabModeAsync);
}

static void ungrab_hotkey(Display *dpy, Window root, KeyCode kc, unsigned mod) {
    unsigned xmod = mod_to_x11(mod);
    for (size_t i = 0; i < N_LOCK_COMBOS; i++)
        XUngrabKey(dpy, kc, xmod | lock_combos[i], root);
}

static int run_x11(void) {
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) { fprintf(stderr, "dictator: cannot open display\n"); return 1; }
    XkbSetDetectableAutoRepeat(dpy, True, NULL);

    /* Resolve copy_key */
    KeySym copy_ks = XStringToKeysym(cfg.copy_key.key_name);
    if (copy_ks == NoSymbol) {
        fprintf(stderr, "dictator: unknown copy_key '%s'\n", cfg.copy_key.key_name);
        XCloseDisplay(dpy);
        return 1;
    }
    /* Resolve paste_key */
    KeySym paste_ks = XStringToKeysym(cfg.paste_key.key_name);
    if (paste_ks == NoSymbol) {
        fprintf(stderr, "dictator: unknown paste_key '%s'\n", cfg.paste_key.key_name);
        XCloseDisplay(dpy);
        return 1;
    }

    Window root = DefaultRootWindow(dpy);
    KeyCode copy_kc  = XKeysymToKeycode(dpy, copy_ks);
    KeyCode paste_kc = XKeysymToKeycode(dpy, paste_ks);

    unsigned copy_xmod  = mod_to_x11(cfg.copy_key.mod_mask);
    unsigned paste_xmod = mod_to_x11(cfg.paste_key.mod_mask);

    grab_hotkey(dpy, root, copy_kc,  cfg.copy_key.mod_mask);
    grab_hotkey(dpy, root, paste_kc, cfg.paste_key.mod_mask);

    char copy_str[128], paste_str[128];
    print_hotkey(&cfg.copy_key,  copy_str,  sizeof(copy_str));
    print_hotkey(&cfg.paste_key, paste_str, sizeof(paste_str));
    printf("dictator: ready (X11) — hold %s to copy, %s to paste\n",
           copy_str, paste_str);

    pthread_t tid;
    int is_recording = 0;
    int active_autopaste = 0;
    KeyCode active_kc = 0;

    int xfd = ConnectionNumber(dpy);
    XFlush(dpy); /* flush grab requests before entering poll loop */
    while (!quit) {
        /* Poll X fd with timeout so we can check quit flag */
        struct pollfd xpfd = { .fd = xfd, .events = POLLIN };
        int pr = poll(&xpfd, 1, 200);
        if (pr <= 0) continue;
        while (XEventsQueued(dpy, QueuedAfterReading) > 0 && !quit) {
            XEvent ev;
            XNextEvent(dpy, &ev);

            if (ev.type == KeyPress && !is_recording) {
                /* Strip lock-key bits to match our configured modifiers */
                unsigned clean = ev.xkey.state & ~(Mod2Mask | LockMask);

                if (ev.xkey.keycode == paste_kc &&
                    clean == paste_xmod) {
                    active_autopaste = 1;
                    active_kc = paste_kc;
                } else if (ev.xkey.keycode == copy_kc &&
                           clean == copy_xmod) {
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
                handle_recording_done(active_autopaste);
            }
        }
    }

    if (is_recording) {
        recording = 0;
        pthread_join(tid, NULL);
    }
    ungrab_hotkey(dpy, root, copy_kc,  cfg.copy_key.mod_mask);
    ungrab_hotkey(dpy, root, paste_kc, cfg.paste_key.mod_mask);
    XCloseDisplay(dpy);
    return 0;
}

#endif /* USE_X11 */

/* ── Evdev backend ───────────────────────────────────────────────────── */

#ifdef USE_EVDEV

/* Key name -> evdev keycode lookup table */
static const struct {
    const char *name;
    unsigned    code;
} evdev_key_table[] = {
    {"F1",  KEY_F1},  {"F2",  KEY_F2},  {"F3",  KEY_F3},  {"F4",  KEY_F4},
    {"F5",  KEY_F5},  {"F6",  KEY_F6},  {"F7",  KEY_F7},  {"F8",  KEY_F8},
    {"F9",  KEY_F9},  {"F10", KEY_F10}, {"F11", KEY_F11}, {"F12", KEY_F12},
    {"space", KEY_SPACE}, {"Space", KEY_SPACE},
    {"Return", KEY_ENTER}, {"return", KEY_ENTER},
    {"Tab", KEY_TAB}, {"tab", KEY_TAB},
    {"Escape", KEY_ESC}, {"escape", KEY_ESC},
    {"BackSpace", KEY_BACKSPACE}, {"backspace", KEY_BACKSPACE},
    {"Delete", KEY_DELETE}, {"delete", KEY_DELETE},
    {"Home", KEY_HOME}, {"End", KEY_END},
    {"Prior", KEY_PAGEUP}, {"Next", KEY_PAGEDOWN},
    {"Up", KEY_UP}, {"Down", KEY_DOWN}, {"Left", KEY_LEFT}, {"Right", KEY_RIGHT},
    {"a", KEY_A}, {"b", KEY_B}, {"c", KEY_C}, {"d", KEY_D},
    {"e", KEY_E}, {"f", KEY_F}, {"g", KEY_G}, {"h", KEY_H},
    {"i", KEY_I}, {"j", KEY_J}, {"k", KEY_K}, {"l", KEY_L},
    {"m", KEY_M}, {"n", KEY_N}, {"o", KEY_O}, {"p", KEY_P},
    {"q", KEY_Q}, {"r", KEY_R}, {"s", KEY_S}, {"t", KEY_T},
    {"u", KEY_U}, {"v", KEY_V}, {"w", KEY_W}, {"x", KEY_X},
    {"y", KEY_Y}, {"z", KEY_Z},
    {"0", KEY_0}, {"1", KEY_1}, {"2", KEY_2}, {"3", KEY_3},
    {"4", KEY_4}, {"5", KEY_5}, {"6", KEY_6}, {"7", KEY_7},
    {"8", KEY_8}, {"9", KEY_9},
    {"minus", KEY_MINUS}, {"equal", KEY_EQUAL},
    {"bracketleft", KEY_LEFTBRACE}, {"bracketright", KEY_RIGHTBRACE},
    {"semicolon", KEY_SEMICOLON}, {"apostrophe", KEY_APOSTROPHE},
    {"grave", KEY_GRAVE}, {"backslash", KEY_BACKSLASH},
    {"comma", KEY_COMMA}, {"period", KEY_DOT}, {"slash", KEY_SLASH},
    {NULL, 0}
};

static int keyname_to_evdev(const char *name) {
    for (int i = 0; evdev_key_table[i].name; i++) {
        if (strcasecmp(name, evdev_key_table[i].name) == 0)
            return (int)evdev_key_table[i].code;
    }
    return -1;
}

/* Evdev keyboard device discovery */
static struct libevdev *open_keyboard_device(void) {
    DIR *dir = opendir("/dev/input");
    if (!dir) {
        fprintf(stderr, "dictator: cannot open /dev/input\n");
        return NULL;
    }

    struct libevdev *dev = NULL;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strncmp(ent->d_name, "event", 5) != 0) continue;

        char path[280];
        snprintf(path, sizeof(path), "/dev/input/%s", ent->d_name);

        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;

        struct libevdev *candidate = NULL;
        if (libevdev_new_from_fd(fd, &candidate) < 0) {
            close(fd);
            continue;
        }

        /* Pick first device with EV_KEY + KEY_A (filters mice, power buttons) */
        if (libevdev_has_event_type(candidate, EV_KEY) &&
            libevdev_has_event_code(candidate, EV_KEY, KEY_A)) {
            dev = candidate;
            break;
        }

        libevdev_free(candidate);
        close(fd);
    }
    closedir(dir);

    if (!dev)
        fprintf(stderr, "dictator: no keyboard device found in /dev/input/\n"
                        "  Ensure you are in the 'input' group: sudo usermod -aG input $USER\n");
    return dev;
}

/* Evdev modifier state tracking */
static unsigned evdev_mod_state;

static void update_mod_state(unsigned code, int pressed) {
    unsigned bit = 0;
    switch (code) {
        case KEY_LEFTSHIFT: case KEY_RIGHTSHIFT: bit = MOD_SHIFT; break;
        case KEY_LEFTCTRL:  case KEY_RIGHTCTRL:  bit = MOD_CTRL;  break;
        case KEY_LEFTALT:   case KEY_RIGHTALT:   bit = MOD_ALT;   break;
        case KEY_LEFTMETA:  case KEY_RIGHTMETA:  bit = MOD_SUPER; break;
    }
    if (bit) {
        if (pressed) evdev_mod_state |= bit; else evdev_mod_state &= ~bit;
    }
}

static int run_evdev(void) {
    /* Resolve keycodes from config */
    int copy_code = keyname_to_evdev(cfg.copy_key.key_name);
    if (copy_code < 0) {
        fprintf(stderr, "dictator: unknown copy_key '%s' for evdev\n",
                cfg.copy_key.key_name);
        return 1;
    }
    int paste_code = keyname_to_evdev(cfg.paste_key.key_name);
    if (paste_code < 0) {
        fprintf(stderr, "dictator: unknown paste_key '%s' for evdev\n",
                cfg.paste_key.key_name);
        return 1;
    }

    struct libevdev *dev = open_keyboard_device();
    if (!dev) return 1;

    int fd = libevdev_get_fd(dev);

    char copy_str[128], paste_str[128];
    print_hotkey(&cfg.copy_key,  copy_str,  sizeof(copy_str));
    print_hotkey(&cfg.paste_key, paste_str, sizeof(paste_str));
    printf("dictator: ready (evdev/Wayland) — hold %s to copy, %s to paste\n",
           copy_str, paste_str);

    pthread_t tid;
    int is_recording = 0;
    int active_autopaste = 0;
    int active_code = 0;

    struct pollfd pfd = { .fd = fd, .events = POLLIN };

    while (!quit) {
        int ret = poll(&pfd, 1, 200); /* 200ms timeout to check quit */
        if (ret <= 0) continue;

        struct input_event ev;
        int rc;
        while ((rc = libevdev_next_event(dev, LIBEVDEV_READ_FLAG_NORMAL, &ev))
               == LIBEVDEV_READ_STATUS_SUCCESS ||
               rc == LIBEVDEV_READ_STATUS_SYNC) {
            if (ev.type != EV_KEY) continue;

            /* Update modifier state for all key events */
            update_mod_state(ev.code, ev.value != 0);

            if (ev.value == 1 && !is_recording) {
                /* Key press (not repeat) */
                int matched = 0;
                if ((int)ev.code == paste_code &&
                    evdev_mod_state == cfg.paste_key.mod_mask) {
                    active_autopaste = 1;
                    active_code = paste_code;
                    matched = 1;
                } else if ((int)ev.code == copy_code &&
                           evdev_mod_state == cfg.copy_key.mod_mask) {
                    active_autopaste = 0;
                    active_code = copy_code;
                    matched = 1;
                }
                if (!matched) continue;

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
            else if (ev.value == 0 && is_recording &&
                     (int)ev.code == active_code) {
                /* Key release */
                recording = 0;
                pthread_join(tid, NULL);
                is_recording = 0;
                handle_recording_done(active_autopaste);
            }
        }
    }

    if (is_recording) {
        recording = 0;
        pthread_join(tid, NULL);
    }
    libevdev_free(dev);
    close(fd);
    return 0;
}

#endif /* USE_EVDEV */

/* ── Main ───────────────────────────────────────────────────────────── */

int main(void) {
    load_config();
    if (load_env() < 0) return 1;
    curl_global_init(CURL_GLOBAL_ALL);

    signal(SIGINT,  handle_signal);
    signal(SIGTERM, handle_signal);

    active_backend = detect_backend();

    int rc = 1;
    switch (active_backend) {
#ifdef USE_X11
    case BACKEND_X11:
        rc = run_x11();
        break;
#endif
#ifdef USE_EVDEV
    case BACKEND_EVDEV:
        rc = run_evdev();
        break;
#endif
    default:
        fprintf(stderr, "dictator: no backend available for this session type\n");
        break;
    }

    curl_global_cleanup();
    printf("dictator: shutdown\n");
    return rc;
}
