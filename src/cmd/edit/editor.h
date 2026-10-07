/* editor.h : the editor core shared between edit.c and vi.c.
 *
 * edit.c holds the buffer, rendering, chrome, menus, dialogs, and the
 * modeless personality; vi.c holds the vi personality layered on top. This
 * header carries the state both share (struct editor and its enums), the core
 * helpers vi.c calls, and the vi entry points edit.c calls. */

#ifndef LUMI_EDIT_EDITOR_H
#define LUMI_EDIT_EDITOR_H

#include <limits.h>
#include <stddef.h>
#include <stdint.h>

/* A yank/delete register: owned bytes, their length, and whether the content
 * is whole lines (put restores it as new lines). */
struct vi_reg {
	char	*bytes;
	size_t	len;
	int	linewise;
};

/* A recorded key sequence, used by the '.' repeat to replay the last change. */
struct vi_keylog {
	struct tkbd_seq	*ev;
	int		len;
	int		cap;
};

/* One open file. The editor keeps a list of these; the active buffer's fields
 * are mirrored into the flat struct editor for editing and copied back here on
 * a switch. Only genuinely per-file state lives here -- the draw surface,
 * chrome, and global vi state (registers, the dot register, the clipboard, the
 * last search) stay in struct editor and are shared across all buffers. */
struct ebuf {
	struct text	*t;
	char		path[PATH_MAX];
	int		has_name;
	size_t		cy, cx, top, left;
	int		sel_active;
	size_t		ay, ax;
	const struct syntax *syn;
	uint16_t	*line_state;
	size_t		line_state_cap;
	size_t		hl_valid;
	int		hex_view;
	size_t		hex_top;
	size_t		vi_mark_y[26];
	size_t		vi_mark_x[26];
	uint32_t	vi_marks_set;
};

/* Referenced only by pointer here; the users include the real headers. */
struct text;
struct draw;
struct draw_term;
struct cfg;
struct syntax;
struct build_err;
struct tkbd_seq;

#define TAB_WIDTH 8

/* Editing personality. The default is a modeless (nano-style) editor;
 * MODE_NORMAL/MODE_INSERT are the vi personality, toggled with F2. */
enum edit_mode {
	MODE_MODELESS,		/* value 0, so a zeroed editor starts modeless */
	MODE_NORMAL,		/* vi command mode */
	MODE_INSERT,		/* vi insert mode */
};

/* What the main loop must do after a command; save and quit may prompt, so
 * they are carried out there where the input stream is available. */
enum req {
	REQ_CONTINUE,
	REQ_FIND,
	REQ_GOTO,
	REQ_HELP,
	REQ_SAVE,
	REQ_QUIT,		/* modeless quit: prompts if the buffer is dirty */
	REQ_VI_COLON,		/* vi ':' ex command line */
	REQ_VI_SEARCH,		/* vi '/' search prompt */
	REQ_FORCE_QUIT,		/* vi decided quitting is allowed: leave now */
	REQ_QUIT_ERR,		/* vi ':cq' -- leave with a nonzero exit code */
};

struct editor {
	struct ebuf	*bufs;		/* open buffers; active mirrors into flat */
	int		nbuf;		/* number of open buffers */
	int		bufs_cap;	/* allocated slots in bufs */
	int		cur;		/* index of the active buffer */
	struct text	*t;
	char		path[PATH_MAX];
	int		has_name;
	size_t		cy;		/* cursor line */
	size_t		cx;		/* cursor byte offset within the line */
	size_t		top;		/* first visible line */
	size_t		left;		/* horizontal scroll, display columns */
	int		rows;
	int		cols;
	int		in_session;
	struct draw	*d;		/* drawing surface and input source */
	struct draw_term *term;		/* terminal driver, for mouse control */
	int		mouse_on;	/* editor mouse reporting is enabled */
	int		sel_active;	/* a selection is being extended */
	size_t		ay;		/* selection anchor line */
	size_t		ax;		/* selection anchor byte column */
	char		*clip;		/* internal clipboard bytes */
	size_t		clip_len;	/* length of clip in bytes */
	int		clip_linewise;	/* clip holds whole lines (vi p/P) */
	char		last_find[256];	/* last search string, for repeat */
	int		vi_search_dir;	/* last search direction: 1 fwd, -1 back */
	int		vi_off_kind;	/* search offset: 0 none, l lines, e end, s start */
	long		vi_off_n;	/* the offset's signed count */
	int		vi_match_valid;	/* the match and landing fields are current */
	size_t		vi_match_cy;	/* where the last match starts */
	size_t		vi_match_cx;
	size_t		vi_placed_cy;	/* where the offset then put the cursor */
	size_t		vi_placed_cx;
	int		vi_want_col;	/* display column j/k aim for (INT_MAX=EOL) */
	int		vi_vert_run;	/* this command was a vertical j/k/$ move */
	int		vi_vert_prev;	/* the previous command was one */
	char		status[160];
	int		dos_chrome;	/* DOS EDIT chrome + palette on */
	int		hex_view;	/* render the buffer as a hex dump */
	size_t		hex_top;	/* first visible hex row (byte offset >> 4) */
	int		hex_ascii;	/* editing the ascii column, not the hex */
	int		hex_pending;	/* a typed high nibble 0-15, or -1 */
	int		hex_insert;	/* insert bytes instead of overwriting */
	unsigned char	hex_pat[64];	/* last searched byte pattern */
	size_t		hex_pat_len;	/* its length, 0 when none searched yet */
	int		hex_pat_dir;	/* last search direction: 1 fwd, -1 back */
	int		hex_cols;	/* dump bytes per row: 8, 16, or 32 */
	int		hex_inspect;	/* show the data-inspector footer line */
	int		hex_sel;	/* a byte selection is being extended */
	size_t		hex_anchor;	/* byte offset the selection anchors at */
	struct cfg	*cfg;		/* lumi.conf, for [build] commands */

	/* Build diagnostics from the last compile/make. This is one global
	 * quickfix list shared by every buffer, not per-file state, so
	 * navigation can cross files. */
	struct build_err *errs;		/* parsed file:line[:col] diagnostics */
	int		n_errs;
	int		errs_cap;
	int		err_cur;	/* current diagnostic, or -1 */
	char		build_dir[PATH_MAX];	/* cwd of the last build, for
						 * resolving relative paths */

	/* syntax highlighting */
	const struct syntax *syn;	/* language, or NULL when none */
	int		hl_on;		/* highlighting enabled */
	uint16_t	*line_state;	/* tokenizer state at each line's start */
	size_t		line_state_cap;
	size_t		hl_valid;	/* line_state[0..hl_valid) are current */
	uint8_t		*hl_buf;	/* scratch styles for one rendered line */
	size_t		hl_buf_cap;

	/* vi personality state */
	enum edit_mode	mode;
	char		vi_visual;	/* 0, 'v' charwise, or 'V' linewise */
	int		vi_count;	/* pending motion count, 0 = none */
	char		vi_op;		/* pending operator: 0, 'd', 'c', 'y' */
	int		vi_op_count;	/* count typed before the operator */
	int		vi_gpending;	/* a leading 'g' is awaiting its pair */
	char		vi_charsearch;	/* f/F/t/T awaiting its target char */
	char		vi_textobj;	/* i/a awaiting a text-object char */
	char		vi_markcmd;	/* m/`/' awaiting its mark letter */
	char		vi_rpending;	/* r typed, awaiting the new character */
	int		vi_overtype;	/* R Replace mode: typing overwrites */
	size_t		vi_mark_y[26];	/* line of mark 'a'..'z' */
	size_t		vi_mark_x[26];	/* byte column of the mark */
	uint32_t	vi_marks_set;	/* bit i set: mark 'a'+i is defined */
	struct vi_reg	vi_regs[26];	/* named registers "a..z */
	char		vi_reg;		/* selected register a-z/A-Z, 0 = none */
	char		vi_reg_fresh;	/* vi_reg was just armed by " */
	char		vi_regpending;	/* " typed, awaiting the register name */
	struct vi_keylog vi_dot;	/* keys of the last change, for '.' */
	struct vi_keylog vi_rec;	/* the command being recorded now */
	int		vi_replaying;	/* replaying '.': do not record */
	int		vi_cmd_open;	/* a command is mid-record */
	int		vi_suppress_dot;/* this command must not become '.' */
	size_t		vi_cmd_rev;	/* text revision when it started */
	char		vi_last_fT;	/* last f/F/t/T, for ; and , */
	uint32_t	vi_last_fT_ch;	/* the target char it searched for */
	int		vi_zpending;	/* a leading 'Z' is awaiting its pair */
};

/* Core helpers the vi personality relies on, defined in edit.c. */
int text_height(const struct editor *e);
int disp_cols(const char *s, size_t nbytes);
size_t rune_len_at(const char *s, size_t len, size_t cx);
size_t prev_rune_len(const char *s, size_t cx);
void clamp_col(struct editor *e);
void move_left(struct editor *e);
void move_right(struct editor *e);
void hl_touch(struct editor *e, size_t line);
void do_insert(struct editor *e, const char *bytes, size_t n);
void do_newline(struct editor *e);
void do_delete(struct editor *e);
void do_backspace(struct editor *e);
void do_find(struct editor *e, const char *q);
int do_find_dir(struct editor *e, const char *q, int dir);

/* Multi-buffer management (edit.c). The active buffer's per-file state lives
 * in the flat struct editor; these swap it with the saved buffers. */
int buf_open(struct editor *e, const char *path);	/* open/switch; index or -1 */
void buf_switch(struct editor *e, int i);
int buf_cycle(struct editor *e, int dir);		/* next/prev; new index */
int buf_close(struct editor *e, int i);			/* 0 ok, -1 refused */
void buf_list(struct editor *e);			/* summarize into status */
void insert_clip(struct editor *e);
void insert_bytes(struct editor *e, const char *bytes, size_t len);
char *region_text(struct editor *e, size_t y1, size_t x1, size_t y2, size_t x2,
    size_t *outlen);
void delete_region(struct editor *e, size_t y1, size_t x1, size_t y2,
    size_t x2);
void clip_set(struct editor *e, char *bytes, size_t len);
int prompt_line(struct editor *e, const char *q, char *buf, size_t bufsz);

/* Hex view helpers, defined in hex.c. The byte stream is the buffer's line
 * bytes joined by an implied newline (a trailing one only when the content
 * ends with a newline), so these operate over the same struct text as the
 * text view. */
size_t hex_total(const struct text *t);
size_t hex_offset_of(const struct text *t, size_t cy, size_t cx);
void hex_pos_at(const struct text *t, size_t off, size_t *cy, size_t *cx);
size_t hex_gather(const struct text *t, size_t start, unsigned char *buf,
    size_t count);
void hex_format_row(char *out, size_t out_sz, size_t offset,
    const unsigned char *buf, size_t n, int cols);
int hex_hexcol(int j);		/* row column of byte j's hex pair */
int hex_ascii_start(int cols);	/* row column where the ascii gutter begins */
int hex_asciicol(int j, int cols);	/* row column of byte j's ascii cell */
int hex_row_width(int cols);	/* display width of a cols-wide dump row */
int hex_parse_bytes(const char *s, unsigned char *out, size_t max,
    size_t *outlen);		/* parse "de ad be ef" hex pairs; 0 ok, -1 bad */
int hex_find(const struct text *t, const unsigned char *pat, size_t plen,
    size_t from, int dir, size_t *found);	/* wrapping byte search; 1 = hit */
void hex_inspect_line(char *out, size_t out_sz, const unsigned char *b,
    size_t n);				/* decode up to 4 bytes at the cursor */

/* The vi personality, defined in vi.c. */
enum req vi_dispatch(struct editor *e, const struct tkbd_seq *seq);
enum req vi_colon(struct editor *e);
void vi_search(struct editor *e);
int vi_search_cmd(struct editor *e, const char *typed, int dir);
void vi_search_run(struct editor *e, const char *q, int dir);
void vi_clamp(struct editor *e);
void vi_reset_pending(struct editor *e);
size_t vi_col_to_byte(struct editor *e, size_t y, int target_col);

#endif /* LUMI_EDIT_EDITOR_H */
