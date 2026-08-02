#!/bin/sh
# smoke.sh : basic smoke tests for lumi binaries
# Copyright (c) 2026 Jon Mayo
# Licensed under MIT-0 OR PUBLIC DOMAIN
#
# Verifies that built binaries exist and produce expected output.
# Usage: tests/smoke.sh [bindir]
#   bindir defaults to _out/$(cc -dumpmachine)/bin

set -e

BINDIR="${1:-_out/$(${CC:-cc} -dumpmachine)/bin}"

fail=0
total=0

pass() {
	total=$((total + 1))
	printf "  %s ... ok\n" "$1"
}

fail() {
	total=$((total + 1))
	fail=$((fail + 1))
	printf "  %s ... FAIL: %s\n" "$1" "$2"
}

check_exists() {
	if [ -x "$BINDIR/$1" ]; then
		pass "$1 exists"
	else
		fail "$1 exists" "not found or not executable"
	fi
}

printf "smoke tests (bindir=%s):\n" "$BINDIR"

# all sub-commands should be present
for cmd in lumi lumi-attach lumi-detach lumi-kill lumi-list \
           lumi-new lumi-new-window lumi-mserver lumi-version; do
	check_exists "$cmd"
done

# lumi version should print a version string
export LUMI_LIBEXEC_PATH="$BINDIR"
out=$("$BINDIR/lumi" version 2>&1) || true
case "$out" in
	lumi\ version\ *)
		pass "lumi version output"
		;;
	*)
		fail "lumi version output" "unexpected: $out"
		;;
esac

# lumi with no args should exit non-zero and print usage
if "$BINDIR/lumi" 2>/dev/null; then
	fail "lumi no-args exits non-zero" "exited 0"
else
	pass "lumi no-args exits non-zero"
fi

# lumi-attach inside LUMI_SESSION should refuse to run
export LUMI_SESSION="test"
out=$("$BINDIR/lumi-attach" 2>&1) || true
case "$out" in
	*"already inside session"*)
		pass "lumi-attach LUMI_SESSION guard"
		;;
	*)
		fail "lumi-attach LUMI_SESSION guard" "unexpected: $out"
		;;
esac
unset LUMI_SESSION

# "lumi new" must pass the session name through to the mserver even when no
# lumi-mserver symlink exists, so the sub-command runs in-process.  Rewriting
# the process title used to blank the argument strings on that path, and the
# window then registered at the root of the runtime directory instead of
# inside its session.  A lone copy of the binary in a private directory, with
# a PATH that has no lumi in it, is what forces the in-process path.
tmp=$(mktemp -d "${TMPDIR:-/tmp}/lumi-smoke-XXXXXX")
cp "$BINDIR/lumi" "$tmp/lumi"
mkdir -p "$tmp/run"
(
	unset LUMI_SESSION LUMI_LIBEXEC_PATH
	XDG_RUNTIME_DIR="$tmp/run" PATH=/usr/bin:/bin \
	    "$tmp/lumi" new -d -s smokesess >/dev/null 2>&1
)
sleep 1
if [ -d "$tmp/run/lumi/smokesess" ] &&
   [ -n "$(find "$tmp/run/lumi/smokesess" -name socket -print -quit)" ] &&
   [ -z "$(find "$tmp/run/lumi" -maxdepth 1 -name '[0-9]*' -print -quit)" ]; then
	pass "lumi new registers the window inside its session"
else
	fail "lumi new registers the window inside its session" \
	    "window missing from the session directory"
fi
for p in $(ls "$tmp/run/lumi/smokesess" 2>/dev/null) \
         $(ls "$tmp/run/lumi" 2>/dev/null); do
	case "$p" in
		[0-9]*) kill -TERM "$p" 2>/dev/null || true ;;
	esac
done
sleep 0.5
rm -rf "$tmp"

printf "smoke: %d tests, %d failures\n" "$total" "$fail"
[ "$fail" -eq 0 ]
