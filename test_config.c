/*
 * test_config — unit tests for dictator config parsing
 * Build: make test
 * Run:   ./test_config
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Pull in the config struct and parser from dictator.c.
 * We only need the config bits — stub out everything else. */
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

/* Reset cfg to defaults before each test */
static void reset_cfg(void) {
    snprintf(cfg.speech2text_key.key_name, sizeof(cfg.speech2text_key.key_name), "F1");
    cfg.speech2text_key.mod_mask = 0;
    snprintf(cfg.speech2text_paste_key.key_name, sizeof(cfg.speech2text_paste_key.key_name), "F1");
    cfg.speech2text_paste_key.mod_mask = MOD_SHIFT;
    snprintf(cfg.speech2text_translate_paste_key.key_name, sizeof(cfg.speech2text_translate_paste_key.key_name), "F1");
    cfg.speech2text_translate_paste_key.mod_mask = MOD_CTRL;
    cfg.notify = 1;
    cfg.max_duration = MAX_SECONDS;
    snprintf(cfg.groq_model, sizeof(cfg.groq_model), "whisper-large-v3");
    cfg.proxy[0] = '\0';
}

/* Write content to a temp file, load it, then remove */
static void load_from_string(const char *content) {
    const char *path = "/tmp/dictator_test.conf";
    FILE *f = fopen(path, "w");
    fputs(content, f);
    fclose(f);
    reset_cfg();
    load_config_file(path);
    remove(path);
}

/* ── Tests ──────────────────────────────────────────────────────────── */

static void test_defaults(void) {
    printf("test_defaults\n");
    reset_cfg();
    int rc = load_config_file("/tmp/nonexistent_dictator.conf");
    ASSERT(rc == -1, "missing file returns -1");
    ASSERT(strcmp(cfg.speech2text_key.key_name, "F1") == 0, "default speech2text_key is F1");
    ASSERT(cfg.speech2text_key.mod_mask == 0, "default speech2text_key mod_mask is 0");
    ASSERT(strcmp(cfg.speech2text_paste_key.key_name, "F1") == 0, "default speech2text_paste_key is F1");
    ASSERT(cfg.speech2text_paste_key.mod_mask == MOD_SHIFT, "default speech2text_paste_key mod_mask is MOD_SHIFT");
    ASSERT(strcmp(cfg.speech2text_translate_paste_key.key_name, "F1") == 0, "default speech2text_translate_paste_key is F1");
    ASSERT(cfg.speech2text_translate_paste_key.mod_mask == MOD_CTRL, "default speech2text_translate_paste_key mod_mask is MOD_CTRL");
    ASSERT(cfg.notify == 1, "default notify is 1");
}

static void test_simple_speech2text_key(void) {
    printf("test_simple_speech2text_key\n");
    load_from_string("speech2text_key = F5\n");
    ASSERT(strcmp(cfg.speech2text_key.key_name, "F5") == 0, "speech2text_key is F5");
    ASSERT(cfg.speech2text_key.mod_mask == 0, "no modifiers");
}

static void test_shift_speech2text_key(void) {
    printf("test_shift_speech2text_key\n");
    load_from_string("speech2text_key = shift+F1\n");
    ASSERT(strcmp(cfg.speech2text_key.key_name, "F1") == 0, "speech2text_key name is F1");
    ASSERT(cfg.speech2text_key.mod_mask == MOD_SHIFT, "mod_mask is MOD_SHIFT");
}

static void test_ctrl_speech2text_key(void) {
    printf("test_ctrl_speech2text_key\n");
    load_from_string("speech2text_key = ctrl+space\n");
    ASSERT(strcmp(cfg.speech2text_key.key_name, "space") == 0, "speech2text_key is space");
    ASSERT(cfg.speech2text_key.mod_mask == MOD_CTRL, "mod_mask is MOD_CTRL");
}

static void test_control_alias(void) {
    printf("test_control_alias\n");
    load_from_string("speech2text_key = control+F1\n");
    ASSERT(strcmp(cfg.speech2text_key.key_name, "F1") == 0, "speech2text_key is F1");
    ASSERT(cfg.speech2text_key.mod_mask == MOD_CTRL, "control+ maps to MOD_CTRL");
}

static void test_alt_speech2text_key(void) {
    printf("test_alt_speech2text_key\n");
    load_from_string("speech2text_key = alt+a\n");
    ASSERT(strcmp(cfg.speech2text_key.key_name, "a") == 0, "speech2text_key is a");
    ASSERT(cfg.speech2text_key.mod_mask == MOD_ALT, "mod_mask is MOD_ALT");
}

static void test_super_speech2text_key(void) {
    printf("test_super_speech2text_key\n");
    load_from_string("speech2text_key = super+F1\n");
    ASSERT(strcmp(cfg.speech2text_key.key_name, "F1") == 0, "speech2text_key is F1");
    ASSERT(cfg.speech2text_key.mod_mask == MOD_SUPER, "mod_mask is MOD_SUPER");
}

static void test_multiple_modifiers(void) {
    printf("test_multiple_modifiers\n");
    load_from_string("speech2text_key = ctrl+shift+F2\n");
    ASSERT(strcmp(cfg.speech2text_key.key_name, "F2") == 0, "speech2text_key is F2");
    ASSERT(cfg.speech2text_key.mod_mask == (MOD_CTRL | MOD_SHIFT), "ctrl+shift");
}

static void test_case_insensitive_modifiers(void) {
    printf("test_case_insensitive_modifiers\n");
    load_from_string("speech2text_key = SHIFT+CTRL+F3\n");
    ASSERT(strcmp(cfg.speech2text_key.key_name, "F3") == 0, "speech2text_key is F3");
    ASSERT(cfg.speech2text_key.mod_mask == (MOD_SHIFT | MOD_CTRL), "SHIFT+CTRL parsed");
}

static void test_simple_speech2text_paste_key(void) {
    printf("test_simple_speech2text_paste_key\n");
    load_from_string("speech2text_paste_key = F6\n");
    ASSERT(strcmp(cfg.speech2text_paste_key.key_name, "F6") == 0, "speech2text_paste_key is F6");
    ASSERT(cfg.speech2text_paste_key.mod_mask == 0, "no modifiers");
}

static void test_shift_speech2text_paste_key(void) {
    printf("test_shift_speech2text_paste_key\n");
    load_from_string("speech2text_paste_key = shift+F2\n");
    ASSERT(strcmp(cfg.speech2text_paste_key.key_name, "F2") == 0, "speech2text_paste_key name is F2");
    ASSERT(cfg.speech2text_paste_key.mod_mask == MOD_SHIFT, "mod_mask is MOD_SHIFT");
}

static void test_speech2text_paste_key_multiple_modifiers(void) {
    printf("test_speech2text_paste_key_multiple_modifiers\n");
    load_from_string("speech2text_paste_key = ctrl+alt+F4\n");
    ASSERT(strcmp(cfg.speech2text_paste_key.key_name, "F4") == 0, "speech2text_paste_key is F4");
    ASSERT(cfg.speech2text_paste_key.mod_mask == (MOD_CTRL | MOD_ALT), "ctrl+alt");
}

static void test_notify_false(void) {
    printf("test_notify_false\n");
    load_from_string("notify = false\n");
    ASSERT(cfg.notify == 0, "notify is 0");
}

static void test_notify_true(void) {
    printf("test_notify_true\n");
    load_from_string("notify = true\n");
    ASSERT(cfg.notify == 1, "notify is 1");
}

static void test_comments_and_blanks(void) {
    printf("test_comments_and_blanks\n");
    load_from_string(
        "# this is a comment\n"
        "\n"
        "  # indented comment\n"
        "speech2text_key = F7\n"
    );
    ASSERT(strcmp(cfg.speech2text_key.key_name, "F7") == 0, "key parsed after comments");
}

static void test_whitespace_handling(void) {
    printf("test_whitespace_handling\n");
    load_from_string("  speech2text_key   =   shift+F9  \n");
    ASSERT(strcmp(cfg.speech2text_key.key_name, "F9") == 0, "speech2text_key with whitespace");
    ASSERT(cfg.speech2text_key.mod_mask == MOD_SHIFT, "modifier with whitespace");
}

static void test_all_options(void) {
    printf("test_all_options\n");
    load_from_string(
        "speech2text_key = alt+F4\n"
        "speech2text_paste_key = super+F4\n"
        "notify = false\n"
    );
    ASSERT(strcmp(cfg.speech2text_key.key_name, "F4") == 0, "speech2text_key is F4");
    ASSERT(cfg.speech2text_key.mod_mask == MOD_ALT, "speech2text_key alt modifier");
    ASSERT(strcmp(cfg.speech2text_paste_key.key_name, "F4") == 0, "speech2text_paste_key is F4");
    ASSERT(cfg.speech2text_paste_key.mod_mask == MOD_SUPER, "speech2text_paste_key super modifier");
    ASSERT(cfg.notify == 0, "notify off");
}

static void test_both_keys_with_modifiers(void) {
    printf("test_both_keys_with_modifiers\n");
    load_from_string(
        "speech2text_key = ctrl+F1\n"
        "speech2text_paste_key = ctrl+shift+F1\n"
    );
    ASSERT(strcmp(cfg.speech2text_key.key_name, "F1") == 0, "speech2text_key is F1");
    ASSERT(cfg.speech2text_key.mod_mask == MOD_CTRL, "speech2text_key ctrl");
    ASSERT(strcmp(cfg.speech2text_paste_key.key_name, "F1") == 0, "speech2text_paste_key is F1");
    ASSERT(cfg.speech2text_paste_key.mod_mask == (MOD_CTRL | MOD_SHIFT), "speech2text_paste_key ctrl+shift");
}

static void test_old_config_ignored(void) {
    printf("test_old_config_ignored\n");
    load_from_string(
        "key = F5\n"
        "autopaste = false\n"
        "speech2text_key = F3\n"
    );
    /* old "key" and "autopaste" silently ignored; speech2text_key still works */
    ASSERT(strcmp(cfg.speech2text_key.key_name, "F3") == 0, "speech2text_key parsed despite old entries");
    /* speech2text_paste_key unchanged from defaults */
    ASSERT(strcmp(cfg.speech2text_paste_key.key_name, "F1") == 0, "speech2text_paste_key stays default");
    ASSERT(cfg.speech2text_paste_key.mod_mask == MOD_SHIFT, "speech2text_paste_key mod stays default");
}

static void test_unknown_keys_ignored(void) {
    printf("test_unknown_keys_ignored\n");
    load_from_string(
        "foo = bar\n"
        "speech2text_key = F1\n"
    );
    ASSERT(strcmp(cfg.speech2text_key.key_name, "F1") == 0, "known key still parsed");
}

static void test_no_equals_ignored(void) {
    printf("test_no_equals_ignored\n");
    load_from_string("this line has no equals sign\n");
    ASSERT(strcmp(cfg.speech2text_key.key_name, "F1") == 0, "defaults preserved");
}

static void test_max_duration_default(void) {
    printf("test_max_duration_default\n");
    reset_cfg();
    int rc = load_config_file("/tmp/nonexistent_dictator.conf");
    (void)rc;
    ASSERT(cfg.max_duration == 300, "default max_duration is 300");
}

static void test_max_duration_custom(void) {
    printf("test_max_duration_custom\n");
    load_from_string("max_duration = 120\n");
    ASSERT(cfg.max_duration == 120, "max_duration set to 120");
}

static void test_max_duration_clamped(void) {
    printf("test_max_duration_clamped\n");
    load_from_string("max_duration = 5\n");
    ASSERT(cfg.max_duration == 10, "max_duration clamped to 10 (lower bound)");

    load_from_string("max_duration = 999\n");
    ASSERT(cfg.max_duration == 300, "max_duration clamped to 300 (upper bound)");
}

static void test_groq_model_default(void) {
    printf("test_groq_model_default\n");
    reset_cfg();
    ASSERT(strcmp(cfg.groq_model, "whisper-large-v3") == 0,
           "default groq_model is whisper-large-v3");
}

static void test_groq_model_custom(void) {
    printf("test_groq_model_custom\n");
    load_from_string("groq_model = distil-whisper-large-v3-en\n");
    ASSERT(strcmp(cfg.groq_model, "distil-whisper-large-v3-en") == 0,
           "groq_model set to custom value");
}

static void test_proxy_default(void) {
    printf("test_proxy_default\n");
    reset_cfg();
    ASSERT(cfg.proxy[0] == '\0', "default proxy is empty");
}

static void test_proxy_custom(void) {
    printf("test_proxy_custom\n");
    load_from_string("proxy = http://user:pass@host:port\n");
    ASSERT(strcmp(cfg.proxy, "http://user:pass@host:port") == 0,
           "proxy set to custom value");
}

static void test_simple_speech2text_translate_paste_key(void) {
    printf("test_simple_speech2text_translate_paste_key\n");
    load_from_string("speech2text_translate_paste_key = F5\n");
    ASSERT(strcmp(cfg.speech2text_translate_paste_key.key_name, "F5") == 0, "speech2text_translate_paste_key is F5");
    ASSERT(cfg.speech2text_translate_paste_key.mod_mask == 0, "no modifiers");
}

static void test_speech2text_translate_paste_key_with_modifiers(void) {
    printf("test_speech2text_translate_paste_key_with_modifiers\n");
    load_from_string("speech2text_translate_paste_key = ctrl+shift+F2\n");
    ASSERT(strcmp(cfg.speech2text_translate_paste_key.key_name, "F2") == 0, "speech2text_translate_paste_key is F2");
    ASSERT(cfg.speech2text_translate_paste_key.mod_mask == (MOD_CTRL | MOD_SHIFT), "ctrl+shift");
}

static void test_all_three_keys(void) {
    printf("test_all_three_keys\n");
    load_from_string(
        "speech2text_key = F1\n"
        "speech2text_paste_key = shift+F1\n"
        "speech2text_translate_paste_key = ctrl+F1\n"
    );
    ASSERT(strcmp(cfg.speech2text_key.key_name, "F1") == 0, "speech2text_key is F1");
    ASSERT(cfg.speech2text_key.mod_mask == 0, "speech2text_key no mods");
    ASSERT(strcmp(cfg.speech2text_paste_key.key_name, "F1") == 0, "speech2text_paste_key is F1");
    ASSERT(cfg.speech2text_paste_key.mod_mask == MOD_SHIFT, "speech2text_paste_key shift");
    ASSERT(strcmp(cfg.speech2text_translate_paste_key.key_name, "F1") == 0, "speech2text_translate_paste_key is F1");
    ASSERT(cfg.speech2text_translate_paste_key.mod_mask == MOD_CTRL, "speech2text_translate_paste_key ctrl");
}

int main(void) {
    test_defaults();
    test_simple_speech2text_key();
    test_shift_speech2text_key();
    test_ctrl_speech2text_key();
    test_control_alias();
    test_alt_speech2text_key();
    test_super_speech2text_key();
    test_multiple_modifiers();
    test_case_insensitive_modifiers();
    test_simple_speech2text_paste_key();
    test_shift_speech2text_paste_key();
    test_speech2text_paste_key_multiple_modifiers();
    test_notify_false();
    test_notify_true();
    test_comments_and_blanks();
    test_whitespace_handling();
    test_all_options();
    test_both_keys_with_modifiers();
    test_old_config_ignored();
    test_unknown_keys_ignored();
    test_no_equals_ignored();
    test_max_duration_default();
    test_max_duration_custom();
    test_max_duration_clamped();
    test_groq_model_default();
    test_groq_model_custom();
    test_proxy_default();
    test_proxy_custom();
    test_simple_speech2text_translate_paste_key();
    test_speech2text_translate_paste_key_with_modifiers();
    test_all_three_keys();

    printf("\n%d tests, %d failed\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
