/* keys.h : key binding table and prefix key state machine */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#ifndef KEYS_H
#define KEYS_H

#include <stddef.h>
#include <stdint.h>

struct keys;

/* maximum number of binding layers (default + named) */
#define KEYS_MAX_LAYERS		16

/* actions that bindings can trigger */
enum keys_action {
	KEYS_ACTION_NONE = 0,		/* forward byte as normal input */
	KEYS_ACTION_CONSUMED,		/* byte consumed, do not forward */
	KEYS_ACTION_SEND_PREFIX,	/* send literal prefix char */
	KEYS_ACTION_NEW_WINDOW,
	KEYS_ACTION_NEXT_WINDOW,
	KEYS_ACTION_PREV_WINDOW,
	KEYS_ACTION_SELECT_0,
	KEYS_ACTION_SELECT_1,
	KEYS_ACTION_SELECT_2,
	KEYS_ACTION_SELECT_3,
	KEYS_ACTION_SELECT_4,
	KEYS_ACTION_SELECT_5,
	KEYS_ACTION_SELECT_6,
	KEYS_ACTION_SELECT_7,
	KEYS_ACTION_SELECT_8,
	KEYS_ACTION_SELECT_9,
	KEYS_ACTION_KILL_WINDOW,
	KEYS_ACTION_DETACH,
	KEYS_ACTION_WINDOW_LIST,
	KEYS_ACTION_STATUS_TOGGLE,
	KEYS_ACTION_APPS_MENU,
	KEYS_ACTION_SPLIT_H,
	KEYS_ACTION_SPLIT_V,
	KEYS_ACTION_NEXT_PANE,
	KEYS_ACTION_PREV_PANE,
	KEYS_ACTION_CLOSE_PANE,
	KEYS_ACTION_RESIZE_PANE,
	KEYS_ACTION_SCROLLBACK,
	KEYS_ACTION_WINDOW_COLORS,
	KEYS_ACTION_MINIMIZE,
	KEYS_ACTION_MAXIMIZE,
	KEYS_ACTION_SCROLL_LOCK,
	KEYS_ACTION_INPUT_LOCK,
	KEYS_ACTION_SESSION_LIST,
	KEYS_ACTION_PASTE,
	KEYS_ACTION_CLIPBOARD_SYNC,
	KEYS_ACTION_TOGGLE_MODE,
};

/* state of the prefix key state machine */
enum keys_state {
	KEYS_STATE_NORMAL,	/* waiting for prefix key */
	KEYS_STATE_PREFIX,	/* prefix received, waiting for command */
};

/* how long to wait after prefix key before showing the menu (ms).
 * keeps the menu from flashing on rapid Ctrl-A n / Ctrl-A space. */
#define KEYS_PREFIX_TIMEOUT_MS 50

/* callback invoked when the prefix timeout expires (state still PREFIX) */
typedef void (*keys_timeout_cb)(struct keys *k, void *arg);

struct keys *keys_new(uint8_t prefix);
void keys_free(struct keys *k);

/* load GNU Screen-compatible default bindings */
void keys_default(struct keys *k);

/* bind a key (after prefix) to an action */
void keys_bind(struct keys *k, uint8_t key, enum keys_action action);

/* unbind a key */
void keys_unbind(struct keys *k, uint8_t key);

/* feed one byte of input through the state machine.
 * returns:
 *   KEYS_ACTION_NONE     -- forward this byte as normal input
 *   KEYS_ACTION_CONSUMED -- byte consumed by state machine, do not forward
 *   other action          -- execute this action, do not forward */
enum keys_action keys_feed(struct keys *k, uint8_t byte);

/* reset state machine to NORMAL (e.g. after timeout) */
void keys_reset(struct keys *k);

/* register a callback for prefix timeout expiry */
void keys_set_timeout_cb(struct keys *k, keys_timeout_cb cb, void *arg);

/* poll timeout for the event loop: KEYS_PREFIX_TIMEOUT_MS while the prefix
 * key is pending and the timeout hasn't fired yet, -1 otherwise. */
int keys_get_timeout(const struct keys *k);

/* notify libkeys that the poll timeout expired.  invokes the registered
 * callback if state is still PREFIX, then clears the pending flag so
 * keys_get_timeout() returns -1 until the next prefix key press. */
void keys_timeout_expired(struct keys *k);

/* query current state */
enum keys_state keys_get_state(const struct keys *k);

/* get the prefix key */
uint8_t keys_get_prefix(const struct keys *k);

/* query the action bound to a key (without changing state).
 * walks layers top-to-bottom using current title context. */
enum keys_action keys_get_binding(const struct keys *k, uint8_t key);

/* convert action name string to enum, returns KEYS_ACTION_NONE on unknown */
enum keys_action keys_action_from_name(const char *name);

/* convert enum to name string, returns NULL for NONE/CONSUMED */
const char *keys_action_to_name(enum keys_action action);

/* parse a key name like "C-a", "space", "c" into a byte value.
 * returns -1 on error */
int keys_parse_keyname(const char *name);

/* ---- state-dependent binding context ---- */

/* set the current window title for layer matching.
 * title is copied internally.  pass NULL to clear. */
void keys_set_title(struct keys *k, const char *title);

/* flip a named toggle on/off.  returns the new state (1=on, 0=off).
 * creates the toggle (initially on) if it doesn't exist yet. */
int keys_toggle(struct keys *k, const char *name);

/* query a toggle state.  returns 0 if the toggle doesn't exist. */
int keys_toggle_get(const struct keys *k, const char *name);

/* add a named binding layer.  returns 0 on success, -1 if full.
 * title_re (POSIX extended regex on window title) and toggle_name
 * are optional predicates -- pass NULL to omit.  a layer with
 * neither predicate is always active (like the default layer). */
int keys_layer_add(struct keys *k, const char *name,
    const char *title_re, const char *toggle_name);

/* bind a key within a named layer.  returns 0 on success. */
int keys_layer_bind(struct keys *k, const char *layer_name,
    uint8_t key, enum keys_action action);

/* return the number of layers. */
int keys_layer_count(const struct keys *k);

struct cfg;

/* load bindings from a cfg under the [bind] section.
 * also reads [keys] prefix if present.
 * supports named layers via [bind "name"] sections with
 * optional match-title and toggle directives.
 * applies on top of current bindings. */
int keys_load_cfg(struct keys *k, const struct cfg *c);

#endif /* KEYS_H */
