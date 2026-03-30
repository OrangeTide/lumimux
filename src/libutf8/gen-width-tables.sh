#!/bin/sh
# gen-width-tables.sh : generate width_tables.c from Unicode data files
#
# Downloads (or reads cached) Unicode Character Database files and produces
# the C source for zero-width and wide character range tables used by
# rune_width.c.
#
# Usage:
#     src/libutf8/gen-width-tables.sh [VERSIONS]
#     make gen-width-tables
#
# VERSIONS is a space-separated list of Unicode versions (default: 9.0.0
# 12.1.0 15.1.0). Downloaded files are cached under the script's directory.
# Output is written next to the script as width_tables.c.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

UNICODE_BASE_URL="https://www.unicode.org/Public"
CACHE_DIR="$SCRIPT_DIR/.unicode-cache"
OUTPUT="$SCRIPT_DIR/width_tables.c"
VERSIONS="${*:-9.0.0 12.1.0 15.1.0}"

# Map Unicode version to emoji data version.
# Emoji data lives at /Public/emoji/<ver>/emoji-data.txt.
emoji_version() {
	case "$1" in
	9.0.0)   echo "4.0" ;;
	10.0.0)  echo "5.0" ;;
	11.0.0)  echo "11.0" ;;
	12.0.0)  echo "12.0" ;;
	12.1.0)  echo "12.1" ;;
	13.0.0)  echo "13.0" ;;
	14.0.0)  echo "14.0" ;;
	15.0.0)  echo "15.0" ;;
	15.1.0)  echo "15.1" ;;
	16.0.0)  echo "16.0" ;;
	*)       echo "" ;;
	esac
}

# fetch_url URL CACHE_PATH -- download URL if not cached
fetch_url() {
	_url="$1"
	_cache="$2"
	if [ -f "$_cache" ]; then
		return 0
	fi
	echo "  Fetching $_url" >&2
	mkdir -p "$(dirname "$_cache")"
	if curl -fsSL -o "$_cache" "$_url" 2>/dev/null; then
		return 0
	else
		rm -f "$_cache"
		return 1
	fi
}

# fetch_ucd VERSION FILENAME -- download from UCD
fetch_ucd() {
	fetch_url "$UNICODE_BASE_URL/$1/ucd/$2" "$CACHE_DIR/$1/$2"
}

# fetch_emoji VERSION -- download emoji-data.txt (tries emoji/ then ucd/)
fetch_emoji() {
	_emver=$(emoji_version "$1")
	if [ -n "$_emver" ]; then
		if fetch_url "$UNICODE_BASE_URL/emoji/$_emver/emoji-data.txt" \
			"$CACHE_DIR/emoji-$_emver/emoji-data.txt"; then
			echo "$CACHE_DIR/emoji-$_emver/emoji-data.txt"
			return 0
		fi
	fi
	# Fall back to UCD emoji subdirectory
	if fetch_ucd "$1" "emoji/emoji-data.txt"; then
		echo "$CACHE_DIR/$1/emoji/emoji-data.txt"
		return 0
	fi
	echo ""
}

# extract_zero_width UNICODE_DATA_FILE
# Outputs sorted "FIRST LAST" hex pairs for zero-width codepoints.
# Includes: Mn (nonspacing mark), Me (enclosing mark), Cf (format),
# soft hyphen (U+00AD), hangul jungseong filler (U+1160..U+11FF).
extract_zero_width() {
	awk -F';' '
	/^[[:space:]]*#/ || /^[[:space:]]*$/ { next }
	{
		cp = strtonum("0x" $1)
		name = $2
		cat = $3
	}
	name ~ /, First>/ {
		range_start = cp
		range_cat = cat
		next
	}
	name ~ /, Last>/ {
		if (range_cat == "Mn" || range_cat == "Me" || range_cat == "Cf")
			printf "%05X %05X\n", range_start, cp
		range_start = 0
		next
	}
	cat == "Mn" || cat == "Me" || cat == "Cf" {
		printf "%05X %05X\n", cp, cp
	}
	END {
		# Soft hyphen
		printf "%05X %05X\n", 0x00AD, 0x00AD
		# Hangul jungseong/jongseong filler
		printf "%05X %05X\n", 0x1160, 0x11FF
	}
	' "$1" | sort -t' ' -k1,1
}

# extract_wide EAW_FILE [EMOJI_FILE]
# Outputs sorted "FIRST LAST" hex pairs for wide codepoints.
# Includes: East Asian Width W and F, plus Emoji_Presentation.
extract_wide() {
	{
		# East Asian Width: W (wide) and F (fullwidth)
		awk '
		/^[[:space:]]*#/ || /^[[:space:]]*$/ { next }
		{
			# Strip comment
			sub(/#.*/, "")
			n = split($0, parts, ";")
			if (n < 2) next
			# Trim whitespace
			gsub(/[[:space:]]/, "", parts[1])
			gsub(/[[:space:]]/, "", parts[2])
			prop = parts[2]
			if (prop != "W" && prop != "F") next
			cp_str = parts[1]
			if (index(cp_str, "..") > 0) {
				split(cp_str, r, "\\.\\.")
				printf "%05X %05X\n", strtonum("0x" r[1]), strtonum("0x" r[2])
			} else {
				v = strtonum("0x" cp_str)
				printf "%05X %05X\n", v, v
			}
		}
		' "$1"

		# Emoji_Presentation
		if [ -n "$2" ] && [ -f "$2" ]; then
			awk '
			/^[[:space:]]*#/ || /^[[:space:]]*$/ { next }
			{
				sub(/#.*/, "")
				n = split($0, parts, ";")
				if (n < 2) next
				gsub(/[[:space:]]/, "", parts[1])
				gsub(/[[:space:]]/, "", parts[2])
				if (parts[2] != "Emoji_Presentation") next
				cp_str = parts[1]
				if (index(cp_str, "..") > 0) {
					split(cp_str, r, "\\.\\.")
					printf "%05X %05X\n", strtonum("0x" r[1]), strtonum("0x" r[2])
				} else {
					v = strtonum("0x" cp_str)
					printf "%05X %05X\n", v, v
				}
			}
			' "$2"
		fi
	} | sort -t' ' -k1,1
}

# merge_ranges -- reads "FIRST LAST" hex pairs from stdin, merges
# overlapping/adjacent ranges, outputs merged pairs.
merge_ranges() {
	awk '
	NR == 1 {
		pfirst = strtonum("0x" $1)
		plast  = strtonum("0x" $2)
		next
	}
	{
		first = strtonum("0x" $1)
		last  = strtonum("0x" $2)
		if (first <= plast + 1) {
			if (last > plast) plast = last
		} else {
			printf "%05X %05X\n", pfirst, plast
			pfirst = first
			plast  = last
		}
	}
	END {
		if (NR > 0)
			printf "%05X %05X\n", pfirst, plast
	}
	'
}

# format_table NAME -- reads "FIRST LAST" hex pairs from stdin,
# outputs a C static array definition.
format_table() {
	_name="$1"
	echo "static const struct rune_range ${_name}[] = {"
	awk '{
		f = strtonum("0x" $1)
		l = strtonum("0x" $2)
		printf "\t{0x%04X, 0x%04X},\n", f, l
	}'
	echo "};"
}

# --- Main ---

tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT

{
# File header
cat <<'HEADER'
/* width_tables.c : zero-width and wide character range tables */
/* Copyright (c) 2026 Jon Mayo
 * Licensed under MIT-0 OR PUBLIC DOMAIN */
/*
 * These tables are derived from the Unicode Character Database.
 * Regenerate with: tools/gen-width-tables.sh
 *
 * Zero-width ranges include: combining marks (Mn, Mc, Me),
 * default-ignorable format characters (Cf), hangul jungseong/
 * jongseong fillers, and variation selectors.
 *
 * Wide ranges include: East Asian Fullwidth (F) and Wide (W)
 * characters as defined by UAX #11 (East Asian Width).
 */

#include "rune_width.h"
HEADER

registry=""

for ver in $VERSIONS; do
	ident=$(echo "$ver" | tr '.' '_')
	echo "Processing Unicode $ver..." >&2

	# Download data files
	fetch_ucd "$ver" "UnicodeData.txt"
	fetch_ucd "$ver" "EastAsianWidth.txt"
	emoji_file=$(fetch_emoji "$ver")

	ud_file="$CACHE_DIR/$ver/UnicodeData.txt"
	eaw_file="$CACHE_DIR/$ver/EastAsianWidth.txt"

	# Build zero-width ranges
	extract_zero_width "$ud_file" | merge_ranges > "$tmpdir/zero_$ident"

	# Build wide ranges
	extract_wide "$eaw_file" "$emoji_file" | merge_ranges > "$tmpdir/wide_$ident"

	# Emit C arrays
	cat <<-EOF

	/****************************************************************
	 * Unicode $ver
	 ****************************************************************/

	EOF
	format_table "zero_$ident" < "$tmpdir/zero_$ident"
	echo ""
	format_table "wide_$ident" < "$tmpdir/wide_$ident"

	if [ -z "$registry" ]; then
		registry="	TABLE_ENTRY(\"$ver\",  zero_$ident,  wide_$ident),"
	else
		registry="$registry
	TABLE_ENTRY(\"$ver\",  zero_$ident,  wide_$ident),"
	fi
done

# Table registry
printf '\n'
cat <<'REGISTRY'
/****************************************************************
 * Table registry -- ordered by version (oldest first)
 ****************************************************************/

#define TABLE_ENTRY(ver, z, w) { \
	.version = ver, \
	.zero = z, .zero_count = sizeof(z) / sizeof(z[0]), \
	.wide = w, .wide_count = sizeof(w) / sizeof(w[0]), \
}

const struct rune_width_table rune_width_builtin[] = {
REGISTRY
echo "$registry"
cat <<'FOOTER'
};

const size_t rune_width_builtin_count =
	sizeof(rune_width_builtin) / sizeof(rune_width_builtin[0]);
FOOTER

} > "$OUTPUT"

echo "Wrote $OUTPUT" >&2
