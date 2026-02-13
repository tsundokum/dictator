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
    snprintf(cfg.copy_key.key_name, sizeof(cfg.copy_key.key_name), "F1");
    cfg.copy_key.mod_mask = 0;
    snprintf(cfg.paste_key.key_name, sizeof(cfg.paste_key.key_name), "F1");
    cfg.paste_key.mod_mask = MOD_SHIFT;
    cfg.notify = 1;
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
    ASSERT(strcmp(cfg.copy_key.key_name, "F1") == 0, "default copy_key is F1");
    ASSERT(cfg.copy_key.mod_mask == 0, "default copy_key mod_mask is 0");
    ASSERT(strcmp(cfg.paste_key.key_name, "F1") == 0, "default paste_key is F1");
    ASSERT(cfg.paste_key.mod_mask == MOD_SHIFT, "default paste_key mod_mask is MOD_SHIFT");
    ASSERT(cfg.notify == 1, "default notify is 1");
}

static void test_simple_copy_key(void) {
    printf("test_simple_copy_key\n");
    load_from_string("copy_key = F5\n");
    ASSERT(strcmp(cfg.copy_key.key_name, "F5") == 0, "copy_key is F5");
    ASSERT(cfg.copy_key.mod_mask == 0, "no modifiers");
}

static void test_shift_copy_key(void) {
    printf("test_shift_copy_key\n");
    load_from_string("copy_key = shift+F1\n");
    ASSERT(strcmp(cfg.copy_key.key_name, "F1") == 0, "copy_key name is F1");
    ASSERT(cfg.copy_key.mod_mask == MOD_SHIFT, "mod_mask is MOD_SHIFT");
}

static void test_ctrl_copy_key(void) {
    printf("test_ctrl_copy_key\n");
    load_from_string("copy_key = ctrl+space\n");
    ASSERT(strcmp(cfg.copy_key.key_name, "space") == 0, "copy_key is space");
    ASSERT(cfg.copy_key.mod_mask == MOD_CTRL, "mod_mask is MOD_CTRL");
}

static void test_control_alias(void) {
    printf("test_control_alias\n");
    load_from_string("copy_key = control+F1\n");
    ASSERT(strcmp(cfg.copy_key.key_name, "F1") == 0, "copy_key is F1");
    ASSERT(cfg.copy_key.mod_mask == MOD_CTRL, "control+ maps to MOD_CTRL");
}

static void test_alt_copy_key(void) {
    printf("test_alt_copy_key\n");
    load_from_string("copy_key = alt+a\n");
    ASSERT(strcmp(cfg.copy_key.key_name, "a") == 0, "copy_key is a");
    ASSERT(cfg.copy_key.mod_mask == MOD_ALT, "mod_mask is MOD_ALT");
}

static void test_super_copy_key(void) {
    printf("test_super_copy_key\n");
    load_from_string("copy_key = super+F1\n");
    ASSERT(strcmp(cfg.copy_key.key_name, "F1") == 0, "copy_key is F1");
    ASSERT(cfg.copy_key.mod_mask == MOD_SUPER, "mod_mask is MOD_SUPER");
}

static void test_multiple_modifiers(void) {
    printf("test_multiple_modifiers\n");
    load_from_string("copy_key = ctrl+shift+F2\n");
    ASSERT(strcmp(cfg.copy_key.key_name, "F2") == 0, "copy_key is F2");
    ASSERT(cfg.copy_key.mod_mask == (MOD_CTRL | MOD_SHIFT), "ctrl+shift");
}

static void test_case_insensitive_modifiers(void) {
    printf("test_case_insensitive_modifiers\n");
    load_from_string("copy_key = SHIFT+CTRL+F3\n");
    ASSERT(strcmp(cfg.copy_key.key_name, "F3") == 0, "copy_key is F3");
    ASSERT(cfg.copy_key.mod_mask == (MOD_SHIFT | MOD_CTRL), "SHIFT+CTRL parsed");
}

static void test_simple_paste_key(void) {
    printf("test_simple_paste_key\n");
    load_from_string("paste_key = F6\n");
    ASSERT(strcmp(cfg.paste_key.key_name, "F6") == 0, "paste_key is F6");
    ASSERT(cfg.paste_key.mod_mask == 0, "no modifiers");
}

static void test_shift_paste_key(void) {
    printf("test_shift_paste_key\n");
    load_from_string("paste_key = shift+F2\n");
    ASSERT(strcmp(cfg.paste_key.key_name, "F2") == 0, "paste_key name is F2");
    ASSERT(cfg.paste_key.mod_mask == MOD_SHIFT, "mod_mask is MOD_SHIFT");
}

static void test_paste_key_multiple_modifiers(void) {
    printf("test_paste_key_multiple_modifiers\n");
    load_from_string("paste_key = ctrl+alt+F4\n");
    ASSERT(strcmp(cfg.paste_key.key_name, "F4") == 0, "paste_key is F4");
    ASSERT(cfg.paste_key.mod_mask == (MOD_CTRL | MOD_ALT), "ctrl+alt");
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
        "copy_key = F7\n"
    );
    ASSERT(strcmp(cfg.copy_key.key_name, "F7") == 0, "key parsed after comments");
}

static void test_whitespace_handling(void) {
    printf("test_whitespace_handling\n");
    load_from_string("  copy_key   =   shift+F9  \n");
    ASSERT(strcmp(cfg.copy_key.key_name, "F9") == 0, "copy_key with whitespace");
    ASSERT(cfg.copy_key.mod_mask == MOD_SHIFT, "modifier with whitespace");
}

static void test_all_options(void) {
    printf("test_all_options\n");
    load_from_string(
        "copy_key = alt+F4\n"
        "paste_key = super+F4\n"
        "notify = false\n"
    );
    ASSERT(strcmp(cfg.copy_key.key_name, "F4") == 0, "copy_key is F4");
    ASSERT(cfg.copy_key.mod_mask == MOD_ALT, "copy_key alt modifier");
    ASSERT(strcmp(cfg.paste_key.key_name, "F4") == 0, "paste_key is F4");
    ASSERT(cfg.paste_key.mod_mask == MOD_SUPER, "paste_key super modifier");
    ASSERT(cfg.notify == 0, "notify off");
}

static void test_both_keys_with_modifiers(void) {
    printf("test_both_keys_with_modifiers\n");
    load_from_string(
        "copy_key = ctrl+F1\n"
        "paste_key = ctrl+shift+F1\n"
    );
    ASSERT(strcmp(cfg.copy_key.key_name, "F1") == 0, "copy_key is F1");
    ASSERT(cfg.copy_key.mod_mask == MOD_CTRL, "copy_key ctrl");
    ASSERT(strcmp(cfg.paste_key.key_name, "F1") == 0, "paste_key is F1");
    ASSERT(cfg.paste_key.mod_mask == (MOD_CTRL | MOD_SHIFT), "paste_key ctrl+shift");
}

static void test_old_config_ignored(void) {
    printf("test_old_config_ignored\n");
    load_from_string(
        "key = F5\n"
        "autopaste = false\n"
        "copy_key = F3\n"
    );
    /* old "key" and "autopaste" silently ignored; copy_key still works */
    ASSERT(strcmp(cfg.copy_key.key_name, "F3") == 0, "copy_key parsed despite old entries");
    /* paste_key unchanged from defaults */
    ASSERT(strcmp(cfg.paste_key.key_name, "F1") == 0, "paste_key stays default");
    ASSERT(cfg.paste_key.mod_mask == MOD_SHIFT, "paste_key mod stays default");
}

static void test_unknown_keys_ignored(void) {
    printf("test_unknown_keys_ignored\n");
    load_from_string(
        "foo = bar\n"
        "copy_key = F1\n"
    );
    ASSERT(strcmp(cfg.copy_key.key_name, "F1") == 0, "known key still parsed");
}

static void test_no_equals_ignored(void) {
    printf("test_no_equals_ignored\n");
    load_from_string("this line has no equals sign\n");
    ASSERT(strcmp(cfg.copy_key.key_name, "F1") == 0, "defaults preserved");
}

int main(void) {
    test_defaults();
    test_simple_copy_key();
    test_shift_copy_key();
    test_ctrl_copy_key();
    test_control_alias();
    test_alt_copy_key();
    test_super_copy_key();
    test_multiple_modifiers();
    test_case_insensitive_modifiers();
    test_simple_paste_key();
    test_shift_paste_key();
    test_paste_key_multiple_modifiers();
    test_notify_false();
    test_notify_true();
    test_comments_and_blanks();
    test_whitespace_handling();
    test_all_options();
    test_both_keys_with_modifiers();
    test_old_config_ignored();
    test_unknown_keys_ignored();
    test_no_equals_ignored();

    printf("\n%d tests, %d failed\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
