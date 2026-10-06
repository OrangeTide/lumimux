/* syntax.c : the syntax highlighter state machine */

#include "syntax.h"

#include <string.h>

/* Match a byte against a joe-style character class: literal bytes and lo-hi
 * ranges. A literal '-' at the end or start is matched as itself. */
static int
class_match(const char *cls, unsigned char c)
{
	size_t i = 0;

	while (cls[i]) {
		unsigned char lo = (unsigned char)cls[i];

		if (cls[i + 1] == '-' && cls[i + 2]) {
			unsigned char hi = (unsigned char)cls[i + 2];

			if (c >= lo && c <= hi)
				return 1;
			i += 3;
		} else {
			if (c == lo)
				return 1;
			i++;
		}
	}
	return 0;
}

static int
kw_eq(const char *word, size_t n, const char *kw, int nocase)
{
	size_t i;

	for (i = 0; i < n && kw[i]; i++) {
		unsigned char a = (unsigned char)word[i];
		unsigned char b = (unsigned char)kw[i];

		if (nocase) {
			if (a >= 'A' && a <= 'Z')
				a += 32;
			if (b >= 'A' && b <= 'Z')
				b += 32;
		}
		if (a != b)
			return (int)a - (int)b;
	}
	if (i < n)
		return 1;		/* word longer than kw */
	return kw[i] ? -1 : 0;		/* kw longer than word, or equal */
}

/* Look up bytes[0,n) in a keyword array. A linear scan keeps the tables free
 * of any sort-order requirement; the lists are short and consulted only when
 * an identifier ends. Returns its style, or SYN_TEXT when it is not a
 * keyword. */
static enum syn_style
kw_lookup(const struct syn_kw *kws, uint16_t nkw, const char *word, size_t n,
    int nocase)
{
	uint16_t i;

	for (i = 0; i < nkw; i++)
		if (kw_eq(word, n, kws[i].word, nocase) == 0)
			return kws[i].style;
	return SYN_TEXT;
}

uint16_t
syn_line(const struct syntax *sy, uint16_t state, const char *bytes, size_t n,
    uint8_t *out)
{
	size_t pos = 0;
	long buf_start = -1;		/* start of an identifier buffer */
	long guard = 0;
	long guard_max = (long)(n + 1) * (sy->nstates + 4) + 32;

	for (;;) {
		int at_nl = (pos >= n);
		unsigned char ch = at_nl ? '\n' : (unsigned char)bytes[pos];
		const struct syn_state *st = &sy->states[state];
		const struct syn_rule *rule = NULL;
		uint16_t r;

		if (++guard > guard_max)
			break;			/* a malformed table would loop */

		for (r = 0; r < st->nrules; r++) {
			if (class_match(st->rules[r].match, ch)) {
				rule = &st->rules[r];
				break;
			}
		}

		if (rule) {
			enum syn_style ts = sy->states[rule->next].style;

			if (rule->buffer)
				buf_start = (long)pos;
			if (out && !at_nl)
				out[pos] = (uint8_t)st->style;
			if (rule->recolor && out) {
				long k = (long)pos - (long)rule->recolor + 1;

				if (k < 0)
					k = 0;
				for (; k <= (long)pos; k++)
					if (k < (long)n)
						out[k] = (uint8_t)ts;
			}
			if (rule->recolor_buf && out && buf_start >= 0) {
				long k;

				for (k = buf_start; k < (long)pos; k++)
					if (k < (long)n)
						out[k] = (uint8_t)ts;
				buf_start = -1;
			}
			state = rule->next;
			if (!rule->noeat)
				pos++;
		} else {
			if (out && !at_nl)
				out[pos] = (uint8_t)st->style;
			if (st->def_recolor && out) {
				enum syn_style ds = sy->states[st->def].style;
				long k = (long)pos - (long)st->def_recolor + 1;

				if (k < 0)
					k = 0;
				for (; k <= (long)pos; k++)
					if (k < (long)n)
						out[k] = (uint8_t)ds;
			}
			if (st->def_noeat) {
				if (st->keywords && buf_start >= 0) {
					enum syn_style ks = kw_lookup(
					    st->keywords, st->nkeywords,
					    bytes + buf_start,
					    pos - (size_t)buf_start,
					    sy->nocase);

					if (out)
						for (long k = buf_start;
						    k < (long)pos; k++)
							out[k] = (uint8_t)ks;
					buf_start = -1;
				}
				state = st->def;	/* keep the byte */
			} else {
				state = st->def;
				pos++;
			}
		}

		if (pos > n)
			break;			/* consumed the synthetic newline */
	}
	return state;
}

/****************************************************************
 * Built-in language registry
 ****************************************************************/

extern const struct syntax syntax_c;
extern const struct syntax syntax_sh;
extern const struct syntax syntax_lua;
extern const struct syntax syntax_py;
extern const struct syntax syntax_rust;
extern const struct syntax syntax_go;
extern const struct syntax syntax_bas;
extern const struct syntax syntax_fth;
extern const struct syntax syntax_js;
extern const struct syntax syntax_html;
extern const struct syntax syntax_nasm;
extern const struct syntax syntax_gas;
extern const struct syntax syntax_pas;

static const struct syntax *const registry[] = {
	&syntax_c,
	&syntax_sh,
	&syntax_lua,
	&syntax_py,
	&syntax_rust,
	&syntax_go,
	&syntax_bas,
	&syntax_fth,
	&syntax_js,
	&syntax_html,
	&syntax_nasm,
	&syntax_gas,
	&syntax_pas,
};

const struct syntax *
syn_for_ext(const char *ext)
{
	size_t i, j;

	if (!ext || !ext[0])
		return NULL;
	for (i = 0; i < sizeof(registry) / sizeof(registry[0]); i++) {
		const char *const *e = registry[i]->exts;

		for (j = 0; e && e[j]; j++)
			if (strcmp(e[j], ext) == 0)
				return registry[i];
	}
	return NULL;
}
