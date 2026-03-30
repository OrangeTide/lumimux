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

printf "smoke: %d tests, %d failures\n" "$total" "$fail"
[ "$fail" -eq 0 ]
