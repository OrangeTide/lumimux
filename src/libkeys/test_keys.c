/* test_keys.c : tests for libkeys */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "keys.h"
#include "cfg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int test_count;
static int fail_count;

#define TEST(name) \
	do { \
		test_count++; \
		printf("  %s ... ", name); \
	} while (0)

#define PASS() \
	do { \
		printf("ok\n"); \
	} while (0)

#define FAIL(msg) \
	do { \
		printf("FAIL: %s\n", msg); \
		fail_count++; \
	} while (0)

#define ASSERT(cond, msg) \
	do { \
		if (!(cond)) { \
			FAIL(msg); \
			return; \
		} \
	} while (0)

/* ---- tests ---- */

static void
test_new_free(void)
{
	struct keys *k;

	TEST("new/free");

	k = keys_new(0x01);
	ASSERT(k != NULL, "keys_new returned NULL");
	ASSERT(keys_get_prefix(k) == 0x01, "wrong prefix");
	ASSERT(keys_get_state(k) == KEYS_STATE_NORMAL, "initial state wrong");
	keys_free(k);
	PASS();
}

static void
test_normal_passthrough(void)
{
	struct keys *k;
	enum keys_action a;

	TEST("normal bytes pass through");

	k = keys_new(0x01);
	keys_default(k);

	a = keys_feed(k, 'a');
	ASSERT(a == KEYS_ACTION_NONE, "'a' should pass through");
	a = keys_feed(k, 'z');
	ASSERT(a == KEYS_ACTION_NONE, "'z' should pass through");
	a = keys_feed(k, 0x03); /* Ctrl-C */
	ASSERT(a == KEYS_ACTION_NONE, "Ctrl-C should pass through");

	keys_free(k);
	PASS();
}

static void
test_prefix_consumed(void)
{
	struct keys *k;
	enum keys_action a;

	TEST("prefix key consumed");

	k = keys_new(0x01);
	keys_default(k);

	a = keys_feed(k, 0x01);
	ASSERT(a == KEYS_ACTION_CONSUMED, "prefix should be consumed");
	ASSERT(keys_get_state(k) == KEYS_STATE_PREFIX, "should be in PREFIX");

	keys_free(k);
	PASS();
}

static void
test_prefix_then_action(void)
{
	struct keys *k;
	enum keys_action a;

	TEST("prefix + c = new window");

	k = keys_new(0x01);
	keys_default(k);

	keys_feed(k, 0x01); /* prefix */
	a = keys_feed(k, 'c');
	ASSERT(a == KEYS_ACTION_NEW_WINDOW, "Ctrl-A c should be NEW_WINDOW");
	ASSERT(keys_get_state(k) == KEYS_STATE_NORMAL, "back to NORMAL");

	keys_free(k);
	PASS();
}

static void
test_send_prefix(void)
{
	struct keys *k;
	enum keys_action a;

	TEST("prefix + prefix = send literal");

	k = keys_new(0x01);
	keys_default(k);

	keys_feed(k, 0x01);
	a = keys_feed(k, 0x01);
	ASSERT(a == KEYS_ACTION_SEND_PREFIX, "Ctrl-A Ctrl-A = SEND_PREFIX");

	keys_free(k);
	PASS();
}

static void
test_navigation(void)
{
	struct keys *k;
	enum keys_action a;

	TEST("window navigation keys");

	k = keys_new(0x01);
	keys_default(k);

	keys_feed(k, 0x01);
	a = keys_feed(k, 'n');
	ASSERT(a == KEYS_ACTION_NEXT_WINDOW, "Ctrl-A n = NEXT");

	keys_feed(k, 0x01);
	a = keys_feed(k, 'p');
	ASSERT(a == KEYS_ACTION_PREV_WINDOW, "Ctrl-A p = PREV");

	keys_feed(k, 0x01);
	a = keys_feed(k, ' ');
	ASSERT(a == KEYS_ACTION_NEXT_WINDOW, "Ctrl-A space = NEXT");

	keys_free(k);
	PASS();
}

static void
test_select_by_number(void)
{
	struct keys *k;
	enum keys_action a;
	int i;

	TEST("select window by number");

	k = keys_new(0x01);
	keys_default(k);

	for (i = 0; i <= 9; i++) {
		keys_feed(k, 0x01);
		a = keys_feed(k, (uint8_t)('0' + i));
		ASSERT(a == KEYS_ACTION_SELECT_0 + i,
		    "Ctrl-A <digit> should select");
	}

	keys_free(k);
	PASS();
}

static void
test_detach(void)
{
	struct keys *k;
	enum keys_action a;

	TEST("detach binding");

	k = keys_new(0x01);
	keys_default(k);

	keys_feed(k, 0x01);
	a = keys_feed(k, 'd');
	ASSERT(a == KEYS_ACTION_DETACH, "Ctrl-A d = DETACH");

	keys_free(k);
	PASS();
}

static void
test_unknown_after_prefix(void)
{
	struct keys *k;
	enum keys_action a;

	TEST("unknown key after prefix consumed");

	k = keys_new(0x01);
	keys_default(k);

	keys_feed(k, 0x01);
	a = keys_feed(k, 0x7F); /* DEL -- not bound */
	ASSERT(a == KEYS_ACTION_CONSUMED, "unbound should be CONSUMED");
	ASSERT(keys_get_state(k) == KEYS_STATE_NORMAL, "back to NORMAL");

	keys_free(k);
	PASS();
}

static void
test_bind_unbind(void)
{
	struct keys *k;
	enum keys_action a;

	TEST("bind/unbind custom key");

	k = keys_new(0x01);
	keys_default(k);

	/* unbind 'c' */
	keys_unbind(k, 'c');
	keys_feed(k, 0x01);
	a = keys_feed(k, 'c');
	ASSERT(a == KEYS_ACTION_CONSUMED, "unbound 'c' should be consumed");

	/* bind 'x' to detach */
	keys_bind(k, 'x', KEYS_ACTION_DETACH);
	keys_feed(k, 0x01);
	a = keys_feed(k, 'x');
	ASSERT(a == KEYS_ACTION_DETACH, "custom 'x' = DETACH");

	keys_free(k);
	PASS();
}

static void
test_reset(void)
{
	struct keys *k;

	TEST("reset clears PREFIX state");

	k = keys_new(0x01);
	keys_default(k);

	keys_feed(k, 0x01);
	ASSERT(keys_get_state(k) == KEYS_STATE_PREFIX, "in PREFIX");
	keys_reset(k);
	ASSERT(keys_get_state(k) == KEYS_STATE_NORMAL, "back to NORMAL");

	keys_free(k);
	PASS();
}

static void
test_action_names(void)
{
	enum keys_action a;

	TEST("action name round-trip");

	/* known actions round-trip */
	a = keys_action_from_name("new-window");
	ASSERT(a == KEYS_ACTION_NEW_WINDOW, "new-window");
	ASSERT(strcmp(keys_action_to_name(a), "new-window") == 0, "to_name");

	a = keys_action_from_name("detach");
	ASSERT(a == KEYS_ACTION_DETACH, "detach");

	a = keys_action_from_name("select-5");
	ASSERT(a == KEYS_ACTION_SELECT_5, "select-5");

	/* unknown returns NONE */
	a = keys_action_from_name("bogus");
	ASSERT(a == KEYS_ACTION_NONE, "unknown -> NONE");

	/* NONE/CONSUMED have no name */
	ASSERT(keys_action_to_name(KEYS_ACTION_NONE) == NULL, "NONE -> NULL");
	ASSERT(keys_action_to_name(KEYS_ACTION_CONSUMED) == NULL,
	    "CONSUMED -> NULL");

	PASS();
}

static void
test_parse_keyname(void)
{
	TEST("parse key names");

	ASSERT(keys_parse_keyname("C-a") == 1, "C-a -> 1");
	ASSERT(keys_parse_keyname("c-a") == 1, "c-a -> 1");
	ASSERT(keys_parse_keyname("C-z") == 26, "C-z -> 26");
	ASSERT(keys_parse_keyname("space") == ' ', "space -> 32");
	ASSERT(keys_parse_keyname("quote") == '"', "quote -> '\"'");
	ASSERT(keys_parse_keyname("tab") == '\t', "tab -> 9");
	ASSERT(keys_parse_keyname("esc") == 0x1b, "esc -> 27");
	ASSERT(keys_parse_keyname("escape") == 0x1b, "escape -> 27");
	ASSERT(keys_parse_keyname("backspace") == 0x7f, "backspace -> 127");
	ASSERT(keys_parse_keyname("c") == 'c', "c -> 'c'");
	ASSERT(keys_parse_keyname("0") == '0', "0 -> '0'");
	ASSERT(keys_parse_keyname("") == -1, "empty -> -1");
	ASSERT(keys_parse_keyname(NULL) == -1, "NULL -> -1");

	PASS();
}

/* helper: write a string to a temp file and return the path */
static char *
write_tmpfile(const char *content)
{
	char path[] = "/tmp/test_keys_XXXXXX";
	int fd;
	size_t len;

	fd = mkstemp(path);
	if (fd < 0)
		return NULL;
	len = strlen(content);
	if (write(fd, content, len) != (ssize_t)len) {
		close(fd);
		unlink(path);
		return NULL;
	}
	close(fd);
	return strdup(path);
}

static void
test_load_cfg(void)
{
	struct keys *k;
	struct cfg *c;
	char *path;
	enum keys_action a;
	static const char config[] =
		"[keys]\n"
		"prefix = C-b\n"
		"\n"
		"[bind]\n"
		"x = detach\n"
		"c = none\n"
		"space = prev-window\n";

	TEST("load config bindings");

	path = write_tmpfile(config);
	ASSERT(path != NULL, "write tmpfile");

	c = cfg_new();
	ASSERT(cfg_load(c, path) == 0, "cfg_load");
	unlink(path);
	free(path);

	k = keys_new(0x01);
	keys_default(k);
	keys_load_cfg(k, c);
	cfg_free(c);

	/* prefix changed to Ctrl-B */
	ASSERT(keys_get_prefix(k) == 0x02, "prefix changed to C-b");

	/* Ctrl-B Ctrl-B sends prefix */
	keys_feed(k, 0x02);
	a = keys_feed(k, 0x02);
	ASSERT(a == KEYS_ACTION_SEND_PREFIX, "C-b C-b = SEND_PREFIX");

	/* old prefix (Ctrl-A) passes through */
	a = keys_feed(k, 0x01);
	ASSERT(a == KEYS_ACTION_NONE, "old prefix passes through");

	/* x = detach */
	keys_feed(k, 0x02);
	a = keys_feed(k, 'x');
	ASSERT(a == KEYS_ACTION_DETACH, "x = detach");

	/* c = none (unbound) */
	keys_feed(k, 0x02);
	a = keys_feed(k, 'c');
	ASSERT(a == KEYS_ACTION_CONSUMED, "c = none (unbound)");

	/* space = prev-window (overridden from default next-window) */
	keys_feed(k, 0x02);
	a = keys_feed(k, ' ');
	ASSERT(a == KEYS_ACTION_PREV_WINDOW, "space = prev-window");

	/* n still has default (not overridden) */
	keys_feed(k, 0x02);
	a = keys_feed(k, 'n');
	ASSERT(a == KEYS_ACTION_NEXT_WINDOW, "n = default next-window");

	keys_free(k);
	PASS();
}

/* ---- timeout API tests ---- */

static void
test_get_timeout_normal(void)
{
	struct keys *k;

	TEST("get_timeout returns -1 in NORMAL");

	k = keys_new(0x01);
	keys_default(k);

	ASSERT(keys_get_timeout(k) == -1, "should be -1 in NORMAL");

	keys_free(k);
	PASS();
}

static void
test_get_timeout_prefix(void)
{
	struct keys *k;

	TEST("get_timeout returns timeout after prefix");

	k = keys_new(0x01);
	keys_default(k);

	keys_feed(k, 0x01);
	ASSERT(keys_get_timeout(k) == KEYS_PREFIX_TIMEOUT_MS,
	    "should be PREFIX_TIMEOUT_MS");

	keys_free(k);
	PASS();
}

static int timeout_cb_called;

static void
test_timeout_cb(struct keys *k, void *arg)
{
	(void)k;
	(void)arg;
	timeout_cb_called++;
}

static void
test_timeout_expired_callback(void)
{
	struct keys *k;

	TEST("timeout_expired invokes callback");

	k = keys_new(0x01);
	keys_default(k);
	keys_set_timeout_cb(k, test_timeout_cb, NULL);

	timeout_cb_called = 0;
	keys_feed(k, 0x01);
	keys_timeout_expired(k);
	ASSERT(timeout_cb_called == 1, "callback should fire once");

	keys_free(k);
	PASS();
}

static void
test_timeout_consumed(void)
{
	struct keys *k;

	TEST("get_timeout returns -1 after expired");

	k = keys_new(0x01);
	keys_default(k);
	keys_set_timeout_cb(k, test_timeout_cb, NULL);

	keys_feed(k, 0x01);
	keys_timeout_expired(k);
	ASSERT(keys_get_timeout(k) == -1,
	    "should be -1 after timeout fired");

	keys_free(k);
	PASS();
}

static void
test_timeout_cleared_by_action(void)
{
	struct keys *k;

	TEST("action clears timeout_pending");

	k = keys_new(0x01);
	keys_default(k);

	keys_feed(k, 0x01);
	ASSERT(keys_get_timeout(k) == KEYS_PREFIX_TIMEOUT_MS,
	    "pending after prefix");

	keys_feed(k, 'c'); /* action clears prefix state */
	ASSERT(keys_get_timeout(k) == -1,
	    "should be -1 after action");

	keys_free(k);
	PASS();
}

static void
test_timeout_cleared_by_reset(void)
{
	struct keys *k;

	TEST("reset clears timeout_pending");

	k = keys_new(0x01);
	keys_default(k);

	keys_feed(k, 0x01);
	ASSERT(keys_get_timeout(k) == KEYS_PREFIX_TIMEOUT_MS,
	    "pending after prefix");

	keys_reset(k);
	ASSERT(keys_get_timeout(k) == -1,
	    "should be -1 after reset");

	keys_free(k);
	PASS();
}

/* ---- layer tests ---- */

static void
test_layer_count(void)
{
	struct keys *k;

	TEST("initial layer count is 1");

	k = keys_new(0x01);
	ASSERT(keys_layer_count(k) == 1, "should have 1 default layer");
	keys_free(k);
	PASS();
}

static void
test_layer_add(void)
{
	struct keys *k;
	int rc;

	TEST("add named layer");

	k = keys_new(0x01);
	keys_default(k);
	rc = keys_layer_add(k, "vi", "^vim.*", NULL);
	ASSERT(rc == 0, "layer_add should succeed");
	ASSERT(keys_layer_count(k) == 2, "should have 2 layers");
	keys_free(k);
	PASS();
}

static void
test_layer_title_match(void)
{
	struct keys *k;
	enum keys_action a;

	TEST("layer activates on title match");

	k = keys_new(0x01);
	keys_default(k);

	keys_layer_add(k, "vi", "^vim", NULL);
	keys_layer_bind(k, "vi", 'j', KEYS_ACTION_PREV_WINDOW);

	/* no title set -- layer inactive, 'j' unbound in default */
	keys_feed(k, 0x01);
	a = keys_feed(k, 'j');
	ASSERT(a == KEYS_ACTION_CONSUMED, "no title -> layer inactive");

	/* title doesn't match */
	keys_set_title(k, "bash");
	keys_feed(k, 0x01);
	a = keys_feed(k, 'j');
	ASSERT(a == KEYS_ACTION_CONSUMED, "non-matching title -> inactive");

	/* title matches */
	keys_set_title(k, "vim foo.c");
	keys_feed(k, 0x01);
	a = keys_feed(k, 'j');
	ASSERT(a == KEYS_ACTION_PREV_WINDOW, "matching title -> active");

	keys_free(k);
	PASS();
}

static void
test_layer_fallthrough(void)
{
	struct keys *k;
	enum keys_action a;

	TEST("layer falls through to default");

	k = keys_new(0x01);
	keys_default(k);

	/* add a layer that only binds 'j', not 'c' */
	keys_layer_add(k, "extra", NULL, NULL); /* always active */
	keys_layer_bind(k, "extra", 'j', KEYS_ACTION_PREV_WINDOW);

	/* 'j' resolved from extra layer */
	keys_feed(k, 0x01);
	a = keys_feed(k, 'j');
	ASSERT(a == KEYS_ACTION_PREV_WINDOW, "'j' from extra layer");

	/* 'c' falls through to default */
	keys_feed(k, 0x01);
	a = keys_feed(k, 'c');
	ASSERT(a == KEYS_ACTION_NEW_WINDOW, "'c' from default layer");

	keys_free(k);
	PASS();
}

static void
test_layer_override(void)
{
	struct keys *k;
	enum keys_action a;

	TEST("higher layer overrides default");

	k = keys_new(0x01);
	keys_default(k);

	/* override 'n' in a higher layer */
	keys_layer_add(k, "custom", NULL, NULL);
	keys_layer_bind(k, "custom", 'n', KEYS_ACTION_DETACH);

	keys_feed(k, 0x01);
	a = keys_feed(k, 'n');
	ASSERT(a == KEYS_ACTION_DETACH, "'n' overridden by custom layer");

	keys_free(k);
	PASS();
}

static void
test_toggle_basic(void)
{
	struct keys *k;
	int state;

	TEST("toggle on/off");

	k = keys_new(0x01);

	/* toggle doesn't exist yet -> off */
	ASSERT(keys_toggle_get(k, "log") == 0, "nonexistent toggle = 0");

	/* first toggle creates it as on */
	state = keys_toggle(k, "log");
	ASSERT(state == 1, "first toggle -> on");
	ASSERT(keys_toggle_get(k, "log") == 1, "get = 1");

	/* toggle again -> off */
	state = keys_toggle(k, "log");
	ASSERT(state == 0, "second toggle -> off");
	ASSERT(keys_toggle_get(k, "log") == 0, "get = 0");

	/* toggle again -> on */
	state = keys_toggle(k, "log");
	ASSERT(state == 1, "third toggle -> on");

	keys_free(k);
	PASS();
}

static void
test_layer_toggle(void)
{
	struct keys *k;
	enum keys_action a;

	TEST("layer activates on toggle");

	k = keys_new(0x01);
	keys_default(k);

	keys_layer_add(k, "debug", NULL, "debug-mode");
	keys_layer_bind(k, "debug", 'j', KEYS_ACTION_KILL_WINDOW);

	/* toggle off -> layer inactive */
	keys_feed(k, 0x01);
	a = keys_feed(k, 'j');
	ASSERT(a == KEYS_ACTION_CONSUMED, "toggle off -> layer inactive");

	/* turn on toggle */
	keys_toggle(k, "debug-mode");

	keys_feed(k, 0x01);
	a = keys_feed(k, 'j');
	ASSERT(a == KEYS_ACTION_KILL_WINDOW, "toggle on -> layer active");

	/* turn off toggle */
	keys_toggle(k, "debug-mode");

	keys_feed(k, 0x01);
	a = keys_feed(k, 'j');
	ASSERT(a == KEYS_ACTION_CONSUMED, "toggle off again -> inactive");

	keys_free(k);
	PASS();
}

static void
test_layer_title_and_toggle(void)
{
	struct keys *k;
	enum keys_action a;

	TEST("layer with both title and toggle predicates");

	k = keys_new(0x01);
	keys_default(k);

	keys_layer_add(k, "special", "^vim", "edit-mode");
	keys_layer_bind(k, "special", 'j', KEYS_ACTION_SPLIT_V);

	/* neither predicate met */
	keys_feed(k, 0x01);
	a = keys_feed(k, 'j');
	ASSERT(a == KEYS_ACTION_CONSUMED, "neither -> inactive");

	/* title matches but toggle off */
	keys_set_title(k, "vim");
	keys_feed(k, 0x01);
	a = keys_feed(k, 'j');
	ASSERT(a == KEYS_ACTION_CONSUMED, "title only -> inactive");

	/* toggle on but title wrong */
	keys_toggle(k, "edit-mode");
	keys_set_title(k, "bash");
	keys_feed(k, 0x01);
	a = keys_feed(k, 'j');
	ASSERT(a == KEYS_ACTION_CONSUMED, "toggle only -> inactive");

	/* both predicates met */
	keys_set_title(k, "vim foo.c");
	keys_feed(k, 0x01);
	a = keys_feed(k, 'j');
	ASSERT(a == KEYS_ACTION_SPLIT_V, "both -> active");

	keys_free(k);
	PASS();
}

static void
test_get_binding_layered(void)
{
	struct keys *k;
	enum keys_action a;

	TEST("get_binding respects layers");

	k = keys_new(0x01);
	keys_default(k);

	keys_layer_add(k, "over", "^zsh", NULL);
	keys_layer_bind(k, "over", 'n', KEYS_ACTION_DETACH);

	/* no title -> default */
	a = keys_get_binding(k, 'n');
	ASSERT(a == KEYS_ACTION_NEXT_WINDOW, "default layer");

	/* matching title -> override */
	keys_set_title(k, "zsh");
	a = keys_get_binding(k, 'n');
	ASSERT(a == KEYS_ACTION_DETACH, "override layer");

	keys_free(k);
	PASS();
}

static void
test_load_cfg_named_layer(void)
{
	struct keys *k;
	struct cfg *c;
	char *path;
	enum keys_action a;
	static const char config[] =
		"[bind]\n"
		"c = new-window\n"
		"\n"
		"[bind \"vi\"]\n"
		"match-title = ^vim\n"
		"j = prev-window\n"
		"k = next-window\n";

	TEST("load config with named layer");

	path = write_tmpfile(config);
	ASSERT(path != NULL, "write tmpfile");

	c = cfg_new();
	ASSERT(cfg_load(c, path) == 0, "cfg_load");
	unlink(path);
	free(path);

	k = keys_new(0x01);
	keys_default(k);
	keys_load_cfg(k, c);
	cfg_free(c);

	/* should have 2 layers */
	ASSERT(keys_layer_count(k) == 2, "2 layers");

	/* default binding works */
	keys_feed(k, 0x01);
	a = keys_feed(k, 'c');
	ASSERT(a == KEYS_ACTION_NEW_WINDOW, "default 'c'");

	/* vi layer inactive without title */
	keys_feed(k, 0x01);
	a = keys_feed(k, 'j');
	ASSERT(a == KEYS_ACTION_CONSUMED, "'j' inactive without title");

	/* vi layer active with matching title */
	keys_set_title(k, "vim test.c");
	keys_feed(k, 0x01);
	a = keys_feed(k, 'j');
	ASSERT(a == KEYS_ACTION_PREV_WINDOW, "'j' active with vim title");

	keys_feed(k, 0x01);
	a = keys_feed(k, 'k');
	ASSERT(a == KEYS_ACTION_NEXT_WINDOW, "'k' active with vim title");

	keys_free(k);
	PASS();
}

static void
test_load_cfg_toggle_layer(void)
{
	struct keys *k;
	struct cfg *c;
	char *path;
	enum keys_action a;
	static const char config[] =
		"[bind \"debug\"]\n"
		"toggle = dbg\n"
		"j = kill-window\n";

	TEST("load config with toggle layer");

	path = write_tmpfile(config);
	ASSERT(path != NULL, "write tmpfile");

	c = cfg_new();
	ASSERT(cfg_load(c, path) == 0, "cfg_load");
	unlink(path);
	free(path);

	k = keys_new(0x01);
	keys_default(k);
	keys_load_cfg(k, c);
	cfg_free(c);

	ASSERT(keys_layer_count(k) == 2, "2 layers");

	/* toggle off -> inactive */
	keys_feed(k, 0x01);
	a = keys_feed(k, 'j');
	ASSERT(a == KEYS_ACTION_CONSUMED, "toggle off -> inactive");

	/* toggle on -> active */
	keys_toggle(k, "dbg");
	keys_feed(k, 0x01);
	a = keys_feed(k, 'j');
	ASSERT(a == KEYS_ACTION_KILL_WINDOW, "toggle on -> active");

	keys_free(k);
	PASS();
}

/* ---- main ---- */

int
main(void)
{
	printf("libkeys tests:\n");

	test_new_free();
	test_normal_passthrough();
	test_prefix_consumed();
	test_prefix_then_action();
	test_send_prefix();
	test_navigation();
	test_select_by_number();
	test_detach();
	test_unknown_after_prefix();
	test_bind_unbind();
	test_reset();
	test_action_names();
	test_parse_keyname();
	test_load_cfg();
	test_get_timeout_normal();
	test_get_timeout_prefix();
	test_timeout_expired_callback();
	test_timeout_consumed();
	test_timeout_cleared_by_action();
	test_timeout_cleared_by_reset();

	/* layer tests */
	test_layer_count();
	test_layer_add();
	test_layer_title_match();
	test_layer_fallthrough();
	test_layer_override();
	test_toggle_basic();
	test_layer_toggle();
	test_layer_title_and_toggle();
	test_get_binding_layered();
	test_load_cfg_named_layer();
	test_load_cfg_toggle_layer();

	printf("\n%d tests, %d failures\n", test_count, fail_count);
	return fail_count > 0 ? 1 : 0;
}
