/*
 * test_config — unit tests for dictator config parsing
 * Build: make test
 * Run:   ./test_config
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <X11/Xlib.h>

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
    snprintf(cfg.key_name, sizeof(cfg.key_name), "F1");
    cfg.mod_mask  = 0;
    cfg.notify    = 1;
    cfg.autopaste = 1;
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
    ASSERT(strcmp(cfg.key_name, "F1") == 0, "default key_name is F1");
    ASSERT(cfg.mod_mask == 0, "default mod_mask is 0");
    ASSERT(cfg.notify == 1, "default notify is 1");
    ASSERT(cfg.autopaste == 1, "default autopaste is 1");
}

static void test_simple_key(void) {
    printf("test_simple_key\n");
    load_from_string("key = F5\n");
    ASSERT(strcmp(cfg.key_name, "F5") == 0, "key_name is F5");
    ASSERT(cfg.mod_mask == 0, "no modifiers");
}

static void test_shift_key(void) {
    printf("test_shift_key\n");
    load_from_string("key = shift+F1\n");
    ASSERT(strcmp(cfg.key_name, "F1") == 0, "key_name is F1");
    ASSERT(cfg.mod_mask == ShiftMask, "mod_mask is ShiftMask");
}

static void test_ctrl_key(void) {
    printf("test_ctrl_key\n");
    load_from_string("key = ctrl+space\n");
    ASSERT(strcmp(cfg.key_name, "space") == 0, "key_name is space");
    ASSERT(cfg.mod_mask == ControlMask, "mod_mask is ControlMask");
}

static void test_control_alias(void) {
    printf("test_control_alias\n");
    load_from_string("key = control+F1\n");
    ASSERT(strcmp(cfg.key_name, "F1") == 0, "key_name is F1");
    ASSERT(cfg.mod_mask == ControlMask, "control+ maps to ControlMask");
}

static void test_alt_key(void) {
    printf("test_alt_key\n");
    load_from_string("key = alt+a\n");
    ASSERT(strcmp(cfg.key_name, "a") == 0, "key_name is a");
    ASSERT(cfg.mod_mask == Mod1Mask, "mod_mask is Mod1Mask");
}

static void test_super_key(void) {
    printf("test_super_key\n");
    load_from_string("key = super+F1\n");
    ASSERT(strcmp(cfg.key_name, "F1") == 0, "key_name is F1");
    ASSERT(cfg.mod_mask == Mod4Mask, "mod_mask is Mod4Mask");
}

static void test_multiple_modifiers(void) {
    printf("test_multiple_modifiers\n");
    load_from_string("key = ctrl+shift+F2\n");
    ASSERT(strcmp(cfg.key_name, "F2") == 0, "key_name is F2");
    ASSERT(cfg.mod_mask == (ControlMask | ShiftMask), "ctrl+shift");
}

static void test_case_insensitive_modifiers(void) {
    printf("test_case_insensitive_modifiers\n");
    load_from_string("key = SHIFT+CTRL+F3\n");
    ASSERT(strcmp(cfg.key_name, "F3") == 0, "key_name is F3");
    ASSERT(cfg.mod_mask == (ShiftMask | ControlMask), "SHIFT+CTRL parsed");
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

static void test_autopaste_false(void) {
    printf("test_autopaste_false\n");
    load_from_string("autopaste = false\n");
    ASSERT(cfg.autopaste == 0, "autopaste is 0");
}

static void test_comments_and_blanks(void) {
    printf("test_comments_and_blanks\n");
    load_from_string(
        "# this is a comment\n"
        "\n"
        "  # indented comment\n"
        "key = F7\n"
    );
    ASSERT(strcmp(cfg.key_name, "F7") == 0, "key parsed after comments");
}

static void test_whitespace_handling(void) {
    printf("test_whitespace_handling\n");
    load_from_string("  key   =   shift+F9  \n");
    ASSERT(strcmp(cfg.key_name, "F9") == 0, "key_name with whitespace");
    ASSERT(cfg.mod_mask == ShiftMask, "modifier with whitespace");
}

static void test_all_options(void) {
    printf("test_all_options\n");
    load_from_string(
        "key = alt+F4\n"
        "notify = false\n"
        "autopaste = false\n"
    );
    ASSERT(strcmp(cfg.key_name, "F4") == 0, "key_name is F4");
    ASSERT(cfg.mod_mask == Mod1Mask, "alt modifier");
    ASSERT(cfg.notify == 0, "notify off");
    ASSERT(cfg.autopaste == 0, "autopaste off");
}

static void test_unknown_keys_ignored(void) {
    printf("test_unknown_keys_ignored\n");
    load_from_string(
        "foo = bar\n"
        "key = F1\n"
    );
    ASSERT(strcmp(cfg.key_name, "F1") == 0, "known key still parsed");
}

static void test_no_equals_ignored(void) {
    printf("test_no_equals_ignored\n");
    load_from_string("this line has no equals sign\n");
    ASSERT(strcmp(cfg.key_name, "F1") == 0, "defaults preserved");
}

int main(void) {
    test_defaults();
    test_simple_key();
    test_shift_key();
    test_ctrl_key();
    test_control_alias();
    test_alt_key();
    test_super_key();
    test_multiple_modifiers();
    test_case_insensitive_modifiers();
    test_notify_false();
    test_notify_true();
    test_autopaste_false();
    test_comments_and_blanks();
    test_whitespace_handling();
    test_all_options();
    test_unknown_keys_ignored();
    test_no_equals_ignored();

    printf("\n%d tests, %d failed\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
