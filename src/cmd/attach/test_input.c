/* test_input.c : simulate attach input dispatch to find timing bugs */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "keys.h"
#include "tkbd.h"

#include <stdio.h>
#include <string.h>

#define TKBD_CH_NONE	0x7FFFFFFFU

static int test_count, fail_count;

#define TEST(name) do { test_count++; printf("  %s ... ", (name)); } while (0)
#define PASS() do { printf("ok\n"); } while (0)
#define FAIL(msg) do { printf("FAIL: %s\n", (msg)); fail_count++; } while (0)
#define ASSERT(cond, msg) do { if (!(cond)) { FAIL(msg); return; } } while (0)

/* ---- simulated attach state ---- */

static struct keys *keybinds;
static int menu_visible;

/* log of dispatched actions */
#define MAX_LOG 64
static enum keys_action action_log[MAX_LOG];
static int action_count;

static int menu_show_count;
static int menu_hide_count;
static int forward_count;	/* bytes forwarded to PTY */

static void
reset_state(void)
{
	keys_default(keybinds);
	keys_reset(keybinds);
	menu_visible = 0;
	action_count = 0;
	menu_show_count = 0;
	menu_hide_count = 0;
	forward_count = 0;
}

/* simulated menu_show */
static void
sim_menu_show(void)
{
	menu_visible = 1;
	menu_show_count++;
}

/* simulated menu_hide */
static void
sim_menu_hide(void)
{
	menu_visible = 0;
	menu_hide_count++;
}

/* simulated dispatch_action */
static void
sim_dispatch_action(enum keys_action action)
{
	if (action_count < MAX_LOG)
		action_log[action_count++] = action;
}

/* simulated menu_input -- replicates prefix_menu.c logic */
static void
sim_menu_input(const struct tkbd_seq *seq)
{
	enum keys_action action;

	if (seq->type == TKBD_MOUSE)
		return;

	switch (seq->key) {
	case TKBD_KEY_UP:
	case TKBD_KEY_DOWN:
	case TKBD_KEY_RIGHT:
	case TKBD_KEY_ENTER:
	case TKBD_KEY_LEFT:
	case TKBD_KEY_ESC:
		keys_reset(keybinds);
		sim_menu_hide();
		return;
	default:
		break;
	}

	if (seq->ch < 256) {
		action = keys_get_binding(keybinds, (uint8_t)seq->ch);
		if (action == KEYS_ACTION_SEND_PREFIX) {
			menu_visible = 0;
			keys_reset(keybinds);
			keys_feed(keybinds, (uint8_t)seq->ch);
			return;
		}
		if (action != KEYS_ACTION_NONE) {
			keys_reset(keybinds);
			sim_menu_hide();
			sim_dispatch_action(action);
			return;
		}
	}

	keys_reset(keybinds);
	sim_menu_hide();
}

/* simulated dispatch_input -- replicates attach.c logic */
static void
sim_dispatch_input(const struct tkbd_seq *seq)
{
	enum keys_action action;

	if (seq->type == TKBD_MOUSE)
		return;

	/* menu layer */
	if (menu_visible) {
		sim_menu_input(seq);
		return;
	}

	/* prefix key state machine -- synthesize C0 byte from key+modifier */
	uint32_t feed_ch = seq->ch;

	if (feed_ch >= 256 && (seq->mod & TKBD_MOD_CTRL)) {
		if (seq->key >= TKBD_KEY_A && seq->key <= TKBD_KEY_Z)
			feed_ch = seq->key & 0x1F;
		else if (seq->key >= TKBD_KEY_BACKSLASH &&
		    seq->key <= TKBD_KEY_UNDERSCORE)
			feed_ch = seq->key & 0x1F;
		else if (seq->key == TKBD_KEY_AT ||
		    seq->key == TKBD_KEY_SPACE)
			feed_ch = 0x00;
	} else if (feed_ch >= 256) {
		if (seq->key >= TKBD_KEY_A && seq->key <= TKBD_KEY_Z)
			feed_ch = seq->key + 0x20;
		else if (seq->key >= 0x20 && seq->key <= 0x7E)
			feed_ch = seq->key;
	}

	if (feed_ch < 256) {
		action = keys_feed(keybinds, (uint8_t)feed_ch);

		if (action == KEYS_ACTION_CONSUMED)
			return;
		if (action != KEYS_ACTION_NONE) {
			sim_dispatch_action(action);
			return;
		}
	}

	/* forward to PTY */
	forward_count++;
}

/* simulate one read() producing a batch of bytes.
 * after processing, check keys state for deferred menu show
 * (mirrors the main event loop prefix timeout logic). */
static void
sim_read_batch(const char *bytes, int len)
{
	int off = 0;

	while (off < len) {
		struct tkbd_seq seq;
		int consumed;

		memset(&seq, 0, sizeof(seq));
		seq.ch = TKBD_CH_NONE;
		consumed = tkbd_parse(&seq, bytes + off, (size_t)(len - off));
		if (consumed == 0)
			break;
		sim_dispatch_input(&seq);
		off += consumed;
	}

	/* deferred menu show -- mirrors main loop prefix timeout */
	if (keys_get_state(keybinds) == KEYS_STATE_PREFIX)
		sim_menu_show();
}

/* ---- tests ---- */

static void
test_fast_ctrl_a_space(void)
{
	TEST("Ctrl-A space in one read -> NEXT_WINDOW");
	reset_state();
	sim_read_batch("\x01 ", 2);
	ASSERT(action_count == 1, "expected 1 action");
	ASSERT(action_log[0] == KEYS_ACTION_NEXT_WINDOW, "expected NEXT_WINDOW");
	ASSERT(menu_show_count == 0, "menu should not appear");
	ASSERT(forward_count == 0, "no bytes forwarded");
	PASS();
}

static void
test_fast_ctrl_a_n(void)
{
	TEST("Ctrl-A n in one read -> NEXT_WINDOW");
	reset_state();
	sim_read_batch("\x01n", 2);
	ASSERT(action_count == 1, "expected 1 action");
	ASSERT(action_log[0] == KEYS_ACTION_NEXT_WINDOW, "expected NEXT_WINDOW");
	ASSERT(menu_show_count == 0, "menu should not appear");
	PASS();
}

static void
test_slow_ctrl_a_then_space(void)
{
	TEST("Ctrl-A alone, then space -> menu + NEXT_WINDOW");
	reset_state();
	sim_read_batch("\x01", 1);
	ASSERT(menu_visible == 1, "menu should appear");
	ASSERT(menu_show_count == 1, "one menu_show");
	sim_read_batch(" ", 1);
	ASSERT(action_count == 1, "expected 1 action");
	ASSERT(action_log[0] == KEYS_ACTION_NEXT_WINDOW, "expected NEXT_WINDOW");
	ASSERT(menu_visible == 0, "menu should be hidden");
	PASS();
}

static void
test_rapid_two_next(void)
{
	TEST("Ctrl-A space Ctrl-A space in one read -> 2x NEXT_WINDOW");
	reset_state();
	sim_read_batch("\x01 \x01 ", 4);
	ASSERT(action_count == 2, "expected 2 actions");
	ASSERT(action_log[0] == KEYS_ACTION_NEXT_WINDOW, "first NEXT_WINDOW");
	ASSERT(action_log[1] == KEYS_ACTION_NEXT_WINDOW, "second NEXT_WINDOW");
	ASSERT(menu_show_count == 0, "no menu flash");
	ASSERT(forward_count == 0, "no bytes forwarded");
	PASS();
}

static void
test_split_ctrl_a_then_ctrl_a_space(void)
{
	TEST("Ctrl-A alone, then Ctrl-A+space -> NEXT_WINDOW");
	reset_state();
	sim_read_batch("\x01", 1);
	ASSERT(menu_visible == 1, "menu should appear from first Ctrl-A");
	ASSERT(menu_show_count == 1, "one menu_show");

	sim_read_batch("\x01 ", 2);
	ASSERT(action_count == 1, "expected 1 NEXT_WINDOW action");
	ASSERT(action_log[0] == KEYS_ACTION_NEXT_WINDOW, "expected NEXT_WINDOW");
	ASSERT(forward_count == 0, "no bytes forwarded to PTY");
	PASS();
}

static void
test_split_ctrl_a_then_ctrl_a_alone_then_space(void)
{
	TEST("Ctrl-A, Ctrl-A, space in 3 reads -> NEXT_WINDOW");
	reset_state();
	sim_read_batch("\x01", 1);
	ASSERT(menu_visible == 1, "menu from first Ctrl-A");

	sim_read_batch("\x01", 1);
	ASSERT(keys_get_state(keybinds) == KEYS_STATE_PREFIX,
	    "should be in PREFIX state");

	sim_read_batch(" ", 1);
	ASSERT(action_count == 1, "expected 1 action");
	ASSERT(action_log[0] == KEYS_ACTION_NEXT_WINDOW, "expected NEXT_WINDOW");
	ASSERT(forward_count == 0, "no bytes forwarded");
	PASS();
}

static void
test_split_rapid_alternating(void)
{
	TEST("alternating single-byte reads: C-a sp C-a sp -> 2x NEXT");
	reset_state();
	sim_read_batch("\x01", 1);	/* menu appears */
	sim_read_batch(" ", 1);		/* NEXT_WINDOW, menu hides */
	ASSERT(action_count == 1, "first NEXT");

	sim_read_batch("\x01", 1);	/* menu appears again */
	sim_read_batch(" ", 1);		/* NEXT_WINDOW, menu hides */
	ASSERT(action_count == 2, "second NEXT");
	ASSERT(action_log[0] == KEYS_ACTION_NEXT_WINDOW, "first");
	ASSERT(action_log[1] == KEYS_ACTION_NEXT_WINDOW, "second");
	ASSERT(forward_count == 0, "no bytes forwarded");
	PASS();
}

static void
test_split_ctrl_a_then_space_ctrl_a_space(void)
{
	TEST("Ctrl-A, then sp+C-a+sp -> 2x NEXT_WINDOW");
	reset_state();
	sim_read_batch("\x01", 1);
	ASSERT(menu_visible == 1, "menu appears");

	sim_read_batch(" \x01 ", 3);
	ASSERT(action_count == 2, "expected 2 actions");
	ASSERT(action_log[0] == KEYS_ACTION_NEXT_WINDOW, "first NEXT");
	ASSERT(action_log[1] == KEYS_ACTION_NEXT_WINDOW, "second NEXT");
	ASSERT(forward_count == 0, "no bytes forwarded");
	PASS();
}

static void
test_literal_prefix_fast(void)
{
	TEST("fast Ctrl-A Ctrl-A -> SEND_PREFIX");
	reset_state();
	sim_read_batch("\x01\x01", 2);
	ASSERT(action_count == 1, "expected 1 action");
	ASSERT(action_log[0] == KEYS_ACTION_SEND_PREFIX, "expected SEND_PREFIX");
	ASSERT(forward_count == 0, "no bytes forwarded");
	PASS();
}

static void
test_no_forward_on_prefix_space(void)
{
	TEST("prefix+space never forwards raw bytes");
	reset_state();

	/* worst case: every byte in a separate read */
	sim_read_batch("\x01", 1);	/* prefix, menu shows */
	sim_read_batch("\x01", 1);	/* prefix while menu showing */
	sim_read_batch(" ", 1);		/* command key */

	ASSERT(forward_count == 0, "space must not be forwarded to PTY");
	ASSERT(action_count == 1, "expected 1 NEXT_WINDOW");
	ASSERT(action_log[0] == KEYS_ACTION_NEXT_WINDOW, "expected NEXT_WINDOW");
	PASS();
}

static void
test_fast_ctrl_a_ctrl_space(void)
{
	TEST("fast Ctrl-A Ctrl-Space -> NEXT_WINDOW");
	reset_state();
	sim_read_batch("\x01\x00", 2);
	ASSERT(action_count == 1, "expected 1 action");
	ASSERT(action_log[0] == KEYS_ACTION_NEXT_WINDOW, "expected NEXT_WINDOW");
	ASSERT(forward_count == 0, "no bytes forwarded");
	PASS();
}

static void
test_fast_ctrl_a_ctrl_n(void)
{
	TEST("fast Ctrl-A Ctrl-N -> NEXT_WINDOW");
	reset_state();
	sim_read_batch("\x01\x0E", 2);
	ASSERT(action_count == 1, "expected 1 action");
	ASSERT(action_log[0] == KEYS_ACTION_NEXT_WINDOW, "expected NEXT_WINDOW");
	ASSERT(forward_count == 0, "no bytes forwarded");
	PASS();
}

static void
test_kitty_ctrl_a_space(void)
{
	TEST("kitty CSI 97;5u + space -> NEXT_WINDOW");
	reset_state();
	/* CSI 97;5u = Ctrl-A in kitty keyboard protocol */
	sim_read_batch("\x1b[97;5u ", 8);
	ASSERT(action_count == 1, "expected 1 action");
	ASSERT(action_log[0] == KEYS_ACTION_NEXT_WINDOW, "expected NEXT_WINDOW");
	ASSERT(forward_count == 0, "no bytes forwarded");
	PASS();
}

static void
test_kitty_ctrl_a_ctrl_a(void)
{
	TEST("kitty CSI 97;5u twice -> SEND_PREFIX");
	reset_state();
	sim_read_batch("\x1b[97;5u\x1b[97;5u", 14);
	ASSERT(action_count == 1, "expected 1 action");
	ASSERT(action_log[0] == KEYS_ACTION_SEND_PREFIX, "expected SEND_PREFIX");
	ASSERT(forward_count == 0, "no bytes forwarded");
	PASS();
}

static void
test_kitty_ctrl_a_n(void)
{
	TEST("kitty CSI 97;5u + n -> NEXT_WINDOW");
	reset_state();
	sim_read_batch("\x1b[97;5un", 8);
	ASSERT(action_count == 1, "expected 1 action");
	ASSERT(action_log[0] == KEYS_ACTION_NEXT_WINDOW, "expected NEXT_WINDOW");
	ASSERT(forward_count == 0, "no bytes forwarded");
	PASS();
}

int
main(void)
{
	keybinds = keys_new(0x01); /* Ctrl-A prefix */
	if (!keybinds) {
		fprintf(stderr, "keys_new failed\n");
		return 1;
	}

	printf("attach input dispatch tests:\n");
	test_fast_ctrl_a_space();
	test_fast_ctrl_a_n();
	test_slow_ctrl_a_then_space();
	test_rapid_two_next();
	test_split_ctrl_a_then_ctrl_a_space();
	test_split_ctrl_a_then_ctrl_a_alone_then_space();
	test_split_rapid_alternating();
	test_split_ctrl_a_then_space_ctrl_a_space();
	test_literal_prefix_fast();
	test_no_forward_on_prefix_space();
	test_fast_ctrl_a_ctrl_space();
	test_fast_ctrl_a_ctrl_n();
	test_kitty_ctrl_a_space();
	test_kitty_ctrl_a_ctrl_a();
	test_kitty_ctrl_a_n();

	keys_free(keybinds);

	printf("\n%d tests, %d failures\n", test_count, fail_count);
	return fail_count > 0 ? 1 : 0;
}
