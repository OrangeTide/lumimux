/* test_utf8.c : tests for libutf8 */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */

#include "utf8.h"
#include "rune_width.h"

#include <stdio.h>
#include <string.h>

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

/* ---- utf8_decode tests ---- */

static void
test_decode_ascii(void)
{
	uint32_t rune;
	int n;

	TEST("decode ASCII");
	n = utf8_decode(&rune, (const unsigned char *)"A", 1);
	ASSERT(n == 1, "expected 1 byte consumed");
	ASSERT(rune == 'A', "expected 'A'");
	PASS();
}

static void
test_decode_null(void)
{
	uint32_t rune;
	int n;

	TEST("decode NUL byte");
	n = utf8_decode(&rune, (const unsigned char *)"\0", 1);
	ASSERT(n == 1, "expected 1 byte consumed");
	ASSERT(rune == 0, "expected U+0000");
	PASS();
}

static void
test_decode_2byte(void)
{
	uint32_t rune;
	int n;

	TEST("decode 2-byte (U+00E9 e-acute)");
	n = utf8_decode(&rune, (const unsigned char *)"\xC3\xA9", 2);
	ASSERT(n == 2, "expected 2 bytes consumed");
	ASSERT(rune == 0x00E9, "expected U+00E9");
	PASS();
}

static void
test_decode_3byte(void)
{
	uint32_t rune;
	int n;

	TEST("decode 3-byte (U+4E16)");
	n = utf8_decode(&rune, (const unsigned char *)"\xE4\xB8\x96", 3);
	ASSERT(n == 3, "expected 3 bytes consumed");
	ASSERT(rune == 0x4E16, "expected U+4E16");
	PASS();
}

static void
test_decode_4byte(void)
{
	uint32_t rune;
	int n;

	TEST("decode 4-byte (U+1F600)");
	n = utf8_decode(&rune,
	    (const unsigned char *)"\xF0\x9F\x98\x80", 4);
	ASSERT(n == 4, "expected 4 bytes consumed");
	ASSERT(rune == 0x1F600, "expected U+1F600");
	PASS();
}

static void
test_decode_max_codepoint(void)
{
	uint32_t rune;
	int n;

	TEST("decode U+10FFFF (max valid)");
	n = utf8_decode(&rune,
	    (const unsigned char *)"\xF4\x8F\xBF\xBF", 4);
	ASSERT(n == 4, "expected 4 bytes consumed");
	ASSERT(rune == 0x10FFFF, "expected U+10FFFF");
	PASS();
}

static void
test_decode_empty(void)
{
	uint32_t rune;
	int n;

	TEST("decode empty input");
	n = utf8_decode(&rune, (const unsigned char *)"", 0);
	ASSERT(n == 0, "expected 0 bytes consumed");
	ASSERT(rune == UTF8_RUNE_ERROR, "expected error rune");
	PASS();
}

static void
test_decode_invalid_lead(void)
{
	uint32_t rune;
	int n;

	TEST("decode invalid lead byte 0xFF");
	n = utf8_decode(&rune, (const unsigned char *)"\xFF", 1);
	ASSERT(n == 1, "expected 1 byte consumed");
	ASSERT(rune == UTF8_RUNE_ERROR, "expected error rune");
	PASS();
}

static void
test_decode_continuation_only(void)
{
	uint32_t rune;
	int n;

	TEST("decode bare continuation byte 0x80");
	n = utf8_decode(&rune, (const unsigned char *)"\x80", 1);
	ASSERT(n == 1, "expected 1 byte consumed");
	ASSERT(rune == UTF8_RUNE_ERROR, "expected error rune");
	PASS();
}

static void
test_decode_truncated_2byte(void)
{
	uint32_t rune;
	int n;

	TEST("decode truncated 2-byte sequence");
	n = utf8_decode(&rune, (const unsigned char *)"\xC3", 1);
	ASSERT(n == 1, "expected 1 byte consumed");
	ASSERT(rune == UTF8_RUNE_ERROR, "expected error rune");
	PASS();
}

static void
test_decode_truncated_3byte(void)
{
	uint32_t rune;
	int n;

	TEST("decode truncated 3-byte sequence");
	n = utf8_decode(&rune, (const unsigned char *)"\xE4\xB8", 2);
	ASSERT(n == 1, "expected 1 byte consumed");
	ASSERT(rune == UTF8_RUNE_ERROR, "expected error rune");
	PASS();
}

static void
test_decode_bad_continuation(void)
{
	uint32_t rune;
	int n;

	TEST("decode bad continuation byte");
	/* lead says 2-byte, but second byte is not 10xxxxxx */
	n = utf8_decode(&rune, (const unsigned char *)"\xC3\x00", 2);
	ASSERT(n == 1, "expected 1 byte consumed");
	ASSERT(rune == UTF8_RUNE_ERROR, "expected error rune");
	PASS();
}

static void
test_decode_overlong_2byte(void)
{
	uint32_t rune;
	int n;

	TEST("decode overlong 2-byte (NUL as C0 80)");
	n = utf8_decode(&rune, (const unsigned char *)"\xC0\x80", 2);
	ASSERT(n == 1, "expected 1 byte consumed (reject)");
	ASSERT(rune == UTF8_RUNE_ERROR, "expected error rune");
	PASS();
}

static void
test_decode_overlong_3byte(void)
{
	uint32_t rune;
	int n;

	TEST("decode overlong 3-byte (U+007F as E0 81 BF)");
	n = utf8_decode(&rune,
	    (const unsigned char *)"\xE0\x81\xBF", 3);
	ASSERT(n == 1, "expected 1 byte consumed (reject)");
	ASSERT(rune == UTF8_RUNE_ERROR, "expected error rune");
	PASS();
}

static void
test_decode_surrogate(void)
{
	uint32_t rune;
	int n;

	TEST("decode surrogate U+D800");
	/* U+D800 = ED A0 80 */
	n = utf8_decode(&rune,
	    (const unsigned char *)"\xED\xA0\x80", 3);
	ASSERT(n == 1, "expected 1 byte consumed (reject)");
	ASSERT(rune == UTF8_RUNE_ERROR, "expected error rune");
	PASS();
}

static void
test_decode_surrogate_high(void)
{
	uint32_t rune;
	int n;

	TEST("decode surrogate U+DFFF");
	/* U+DFFF = ED BF BF */
	n = utf8_decode(&rune,
	    (const unsigned char *)"\xED\xBF\xBF", 3);
	ASSERT(n == 1, "expected 1 byte consumed (reject)");
	ASSERT(rune == UTF8_RUNE_ERROR, "expected error rune");
	PASS();
}

static void
test_decode_above_max(void)
{
	uint32_t rune;
	int n;

	TEST("decode above U+10FFFF (F4 90 80 80)");
	n = utf8_decode(&rune,
	    (const unsigned char *)"\xF4\x90\x80\x80", 4);
	ASSERT(n == 1, "expected 1 byte consumed (reject)");
	ASSERT(rune == UTF8_RUNE_ERROR, "expected error rune");
	PASS();
}

static void
test_decode_from_longer_buffer(void)
{
	uint32_t rune;
	int n;

	TEST("decode first codepoint from longer buffer");
	n = utf8_decode(&rune,
	    (const unsigned char *)"\xC3\xA9hello", 7);
	ASSERT(n == 2, "expected 2 bytes consumed");
	ASSERT(rune == 0x00E9, "expected U+00E9");
	PASS();
}

/* ---- utf8_encode tests ---- */

static void
test_encode_ascii(void)
{
	unsigned char buf[UTF8_MAX];
	int n;

	TEST("encode ASCII 'Z'");
	n = utf8_encode(buf, 'Z');
	ASSERT(n == 1, "expected 1 byte");
	ASSERT(buf[0] == 'Z', "expected 'Z'");
	PASS();
}

static void
test_encode_null(void)
{
	unsigned char buf[UTF8_MAX];
	int n;

	TEST("encode U+0000");
	n = utf8_encode(buf, 0);
	ASSERT(n == 1, "expected 1 byte");
	ASSERT(buf[0] == 0, "expected 0x00");
	PASS();
}

static void
test_encode_2byte(void)
{
	unsigned char buf[UTF8_MAX];
	int n;

	TEST("encode U+00E9 (2-byte)");
	n = utf8_encode(buf, 0x00E9);
	ASSERT(n == 2, "expected 2 bytes");
	ASSERT(buf[0] == 0xC3, "bad lead byte");
	ASSERT(buf[1] == 0xA9, "bad continuation byte");
	PASS();
}

static void
test_encode_3byte(void)
{
	unsigned char buf[UTF8_MAX];
	int n;

	TEST("encode U+4E16 (3-byte)");
	n = utf8_encode(buf, 0x4E16);
	ASSERT(n == 3, "expected 3 bytes");
	ASSERT(buf[0] == 0xE4, "bad byte 0");
	ASSERT(buf[1] == 0xB8, "bad byte 1");
	ASSERT(buf[2] == 0x96, "bad byte 2");
	PASS();
}

static void
test_encode_4byte(void)
{
	unsigned char buf[UTF8_MAX];
	int n;

	TEST("encode U+1F600 (4-byte)");
	n = utf8_encode(buf, 0x1F600);
	ASSERT(n == 4, "expected 4 bytes");
	ASSERT(buf[0] == 0xF0, "bad byte 0");
	ASSERT(buf[1] == 0x9F, "bad byte 1");
	ASSERT(buf[2] == 0x98, "bad byte 2");
	ASSERT(buf[3] == 0x80, "bad byte 3");
	PASS();
}

static void
test_encode_max(void)
{
	unsigned char buf[UTF8_MAX];
	int n;

	TEST("encode U+10FFFF (max)");
	n = utf8_encode(buf, 0x10FFFF);
	ASSERT(n == 4, "expected 4 bytes");
	ASSERT(buf[0] == 0xF4, "bad byte 0");
	ASSERT(buf[1] == 0x8F, "bad byte 1");
	ASSERT(buf[2] == 0xBF, "bad byte 2");
	ASSERT(buf[3] == 0xBF, "bad byte 3");
	PASS();
}

static void
test_encode_surrogate(void)
{
	unsigned char buf[UTF8_MAX];
	int n;

	TEST("encode surrogate U+D800 rejected");
	n = utf8_encode(buf, 0xD800);
	ASSERT(n == 0, "expected 0 (rejected)");
	PASS();
}

static void
test_encode_surrogate_end(void)
{
	unsigned char buf[UTF8_MAX];
	int n;

	TEST("encode surrogate U+DFFF rejected");
	n = utf8_encode(buf, 0xDFFF);
	ASSERT(n == 0, "expected 0 (rejected)");
	PASS();
}

static void
test_encode_above_max(void)
{
	unsigned char buf[UTF8_MAX];
	int n;

	TEST("encode above U+10FFFF rejected");
	n = utf8_encode(buf, 0x110000);
	ASSERT(n == 0, "expected 0 (rejected)");
	PASS();
}

static void
test_encode_boundary_0x80(void)
{
	unsigned char buf[UTF8_MAX];
	int n;

	TEST("encode U+0080 (first 2-byte)");
	n = utf8_encode(buf, 0x80);
	ASSERT(n == 2, "expected 2 bytes");
	ASSERT(buf[0] == 0xC2, "bad lead byte");
	ASSERT(buf[1] == 0x80, "bad continuation byte");
	PASS();
}

static void
test_encode_boundary_0x800(void)
{
	unsigned char buf[UTF8_MAX];
	int n;

	TEST("encode U+0800 (first 3-byte)");
	n = utf8_encode(buf, 0x800);
	ASSERT(n == 3, "expected 3 bytes");
	ASSERT(buf[0] == 0xE0, "bad byte 0");
	ASSERT(buf[1] == 0xA0, "bad byte 1");
	ASSERT(buf[2] == 0x80, "bad byte 2");
	PASS();
}

static void
test_encode_boundary_0x10000(void)
{
	unsigned char buf[UTF8_MAX];
	int n;

	TEST("encode U+10000 (first 4-byte)");
	n = utf8_encode(buf, 0x10000);
	ASSERT(n == 4, "expected 4 bytes");
	ASSERT(buf[0] == 0xF0, "bad byte 0");
	ASSERT(buf[1] == 0x90, "bad byte 1");
	ASSERT(buf[2] == 0x80, "bad byte 2");
	ASSERT(buf[3] == 0x80, "bad byte 3");
	PASS();
}

/* ---- utf8_runelen tests ---- */

static void
test_runelen_ascii(void)
{
	TEST("runelen ASCII");
	ASSERT(utf8_runelen('A') == 1, "expected 1");
	PASS();
}

static void
test_runelen_2byte(void)
{
	TEST("runelen U+00E9 (2-byte)");
	ASSERT(utf8_runelen(0x00E9) == 2, "expected 2");
	PASS();
}

static void
test_runelen_3byte(void)
{
	TEST("runelen U+4E16 (3-byte)");
	ASSERT(utf8_runelen(0x4E16) == 3, "expected 3");
	PASS();
}

static void
test_runelen_4byte(void)
{
	TEST("runelen U+1F600 (4-byte)");
	ASSERT(utf8_runelen(0x1F600) == 4, "expected 4");
	PASS();
}

static void
test_runelen_surrogate(void)
{
	TEST("runelen surrogate rejected");
	ASSERT(utf8_runelen(0xD800) == 0, "expected 0");
	ASSERT(utf8_runelen(0xDFFF) == 0, "expected 0");
	PASS();
}

static void
test_runelen_above_max(void)
{
	TEST("runelen above max rejected");
	ASSERT(utf8_runelen(0x110000) == 0, "expected 0");
	PASS();
}

/* ---- roundtrip tests ---- */

static void
test_roundtrip(void)
{
	/* encode then decode several codepoints */
	static const uint32_t cps[] = {
		0, 0x7F, 0x80, 0x7FF, 0x800, 0xFFFF, 0x10000, 0x10FFFF,
	};
	unsigned char buf[UTF8_MAX];
	uint32_t rune;
	int enc_n, dec_n;
	int i;

	TEST("encode/decode roundtrip");
	for (i = 0; i < (int)(sizeof(cps) / sizeof(cps[0])); i++) {
		/* skip surrogates */
		if (cps[i] >= 0xD800 && cps[i] <= 0xDFFF)
			continue;
		enc_n = utf8_encode(buf, cps[i]);
		ASSERT(enc_n > 0, "encode failed");
		dec_n = utf8_decode(&rune, buf, (size_t)enc_n);
		ASSERT(dec_n == enc_n, "decode length mismatch");
		ASSERT(rune == cps[i], "roundtrip value mismatch");
	}
	PASS();
}

/* ---- rune_width tests ---- */

static void
test_width_ascii(void)
{
	TEST("width ASCII printable");
	ASSERT(rune_width('A') == 1, "expected 1");
	ASSERT(rune_width('~') == 1, "expected 1");
	ASSERT(rune_width(' ') == 1, "expected 1");
	PASS();
}

static void
test_width_null(void)
{
	TEST("width NUL");
	ASSERT(rune_width(0) == 0, "expected 0");
	PASS();
}

static void
test_width_c0_control(void)
{
	TEST("width C0 control chars");
	ASSERT(rune_width(0x01) == -1, "expected -1 for SOH");
	ASSERT(rune_width(0x1F) == -1, "expected -1 for US");
	ASSERT(rune_width(0x7F) == -1, "expected -1 for DEL");
	PASS();
}

static void
test_width_c1_control(void)
{
	TEST("width C1 control chars");
	ASSERT(rune_width(0x80) == -1, "expected -1 for 0x80");
	ASSERT(rune_width(0x9F) == -1, "expected -1 for 0x9F");
	PASS();
}

static void
test_width_combining(void)
{
	TEST("width combining mark (U+0300)");
	/* U+0300 = combining grave accent -- zero-width in all versions */
	ASSERT(rune_width(0x0300) == 0, "expected 0");
	PASS();
}

static void
test_width_cjk(void)
{
	TEST("width CJK wide (U+4E16)");
	/* CJK Unified Ideograph -- wide in all versions */
	ASSERT(rune_width(0x4E16) == 2, "expected 2");
	PASS();
}

static void
test_width_fullwidth(void)
{
	TEST("width fullwidth form (U+FF21)");
	/* Fullwidth Latin Capital Letter A */
	ASSERT(rune_width(0xFF21) == 2, "expected 2");
	PASS();
}

static void
test_width_latin_extended(void)
{
	TEST("width Latin Extended (U+00E9)");
	ASSERT(rune_width(0x00E9) == 1, "expected 1");
	PASS();
}

static void
test_width_replacement(void)
{
	TEST("width replacement char (U+FFFD)");
	ASSERT(rune_width(UTF8_RUNE_ERROR) == 1, "expected 1");
	PASS();
}

static void
test_width_version_switch(void)
{
	const char *v;
	size_t count;
	const struct rune_width_table *tables;

	TEST("width version switching");
	tables = rune_width_tables(&count);
	ASSERT(count >= 1, "need at least one built-in table");

	/* switch to the first table */
	ASSERT(rune_width_set(tables[0].version) == 0,
	    "set first version failed");
	v = rune_width_version();
	ASSERT(strcmp(v, tables[0].version) == 0, "version mismatch");

	/* switch to last (latest) */
	ASSERT(rune_width_set(tables[count - 1].version) == 0,
	    "set latest version failed");

	/* non-existent version should fail */
	ASSERT(rune_width_set("0.0.0") == -1,
	    "expected -1 for unknown version");

	PASS();
}

static void
test_width_init(void)
{
	TEST("width init");
	ASSERT(rune_width_init() == 0, "init failed");
	ASSERT(rune_width_version() != NULL, "version is NULL");
	PASS();
}

/* ---- main ---- */

int
main(void)
{
	rune_width_init();

	printf("libutf8 tests:\n");

	/* utf8_decode */
	test_decode_ascii();
	test_decode_null();
	test_decode_2byte();
	test_decode_3byte();
	test_decode_4byte();
	test_decode_max_codepoint();
	test_decode_empty();
	test_decode_invalid_lead();
	test_decode_continuation_only();
	test_decode_truncated_2byte();
	test_decode_truncated_3byte();
	test_decode_bad_continuation();
	test_decode_overlong_2byte();
	test_decode_overlong_3byte();
	test_decode_surrogate();
	test_decode_surrogate_high();
	test_decode_above_max();
	test_decode_from_longer_buffer();

	/* utf8_encode */
	test_encode_ascii();
	test_encode_null();
	test_encode_2byte();
	test_encode_3byte();
	test_encode_4byte();
	test_encode_max();
	test_encode_surrogate();
	test_encode_surrogate_end();
	test_encode_above_max();
	test_encode_boundary_0x80();
	test_encode_boundary_0x800();
	test_encode_boundary_0x10000();

	/* utf8_runelen */
	test_runelen_ascii();
	test_runelen_2byte();
	test_runelen_3byte();
	test_runelen_4byte();
	test_runelen_surrogate();
	test_runelen_above_max();

	/* roundtrip */
	test_roundtrip();

	/* rune_width */
	test_width_ascii();
	test_width_null();
	test_width_c0_control();
	test_width_c1_control();
	test_width_combining();
	test_width_cjk();
	test_width_fullwidth();
	test_width_latin_extended();
	test_width_replacement();
	test_width_version_switch();
	test_width_init();

	printf("test_utf8: %d tests, %d failures\n",
	    test_count, fail_count);
	return fail_count > 0 ? 1 : 0;
}
