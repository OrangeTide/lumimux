#!/bin/sh
# kbd_leak.sh : regression test for the kitty keyboard-protocol flag leak.
# PUBLIC DOMAIN (CC0-1.0)
#
# The attach client mirrors the focused window's kitty keyboard flags onto
# the outer terminal. It must do so with a set (CSI = flags u), which leaves
# the outer terminal's flag stack at a fixed depth. An earlier version used a
# stack push (CSI > flags u) and pop (CSI < u); switching between windows that
# requested different nonzero flag sets grew the outer stack, and a single pop
# on the way to a plain window left a stale enhanced-keyboard entry active.
# That made Enter report as CSI 13 u, so plain programs never saw a newline.
#
# This test drives a window through flags 0 -> 1 -> 15 -> 0 while a real
# attach client runs on a pty, captures what the client sends outward, and
# asserts it used set ops and never push/pop ops.
#
# Usage: tests/kbd_leak.sh [bindir]
#   bindir defaults to _out/$(cc -dumpmachine)/bin

BINDIR="${1:-_out/$(${CC:-cc} -dumpmachine)/bin}"
BIN="$BINDIR/lumi"

fail=0
total=0

pass() { total=$((total + 1)); printf "  %s ... ok\n" "$1"; }
fail() { total=$((total + 1)); fail=$((fail + 1)); printf "  %s ... FAIL: %s\n" "$1" "$2"; }

printf "kbd_leak test (bindir=%s):\n" "$BINDIR"

if [ ! -x "$BIN" ]; then
	fail "lumi present" "$BIN not found"
	printf "kbd_leak: %d tests, %d failures\n" "$total" "$fail"
	exit 1
fi

# needs a pty logger and a timeout; skip cleanly where unavailable rather
# than reporting a spurious failure.
if ! command -v script >/dev/null 2>&1 || ! command -v timeout >/dev/null 2>&1; then
	printf "  kbd_leak ... skipped (need script and timeout)\n"
	printf "kbd_leak: 0 tests, 0 failures (skipped)\n"
	exit 0
fi

# Unix socket paths are capped near 108 bytes, so the runtime dir must be
# short. Keep it isolated (fresh, private) but under a short /tmp path.
RUN=$(mktemp -d "${TMPDIR:-/tmp}/lumikbd.XXXXXX") || exit 1
EMIT="$RUN/emit.sh"
CAP="$RUN/cap.raw"

cleanup() {
	for d in "$RUN"/lumi/kbd/*/; do
		[ -d "$d" ] || continue
		p=$(basename "$d")
		case "$p" in [0-9]*) kill -TERM "$p" 2>/dev/null || true ;; esac
	done
	rm -rf "$RUN"
}
trap cleanup EXIT INT TERM

# window program: push flags=1, then a different nonzero set, then pop both,
# ending back at plain (flags 0) the way a shell tab would.
cat > "$EMIT" <<'EOF'
#!/bin/sh
sleep 2
printf '\033[>1u'
sleep 0.7
printf '\033[>15u'
sleep 0.7
printf '\033[<2u'
sleep 4
EOF
chmod +x "$EMIT"

# isolated: private runtime dir, no lumi on PATH, no libexec override.
unset LUMI_LIBEXEC_PATH LUMI_SESSION
export XDG_RUNTIME_DIR="$RUN"
export PATH=/usr/bin:/bin
export TERM=xterm-256color

"$BIN" new -d -s kbd "$EMIT" >/dev/null 2>&1
sleep 0.5
if ! find "$RUN/lumi/kbd" -name socket -print 2>/dev/null | grep -q .; then
	fail "session starts" "no window socket under $RUN/lumi/kbd"
	printf "kbd_leak: %d tests, %d failures\n" "$total" "$fail"
	exit 1
fi
pass "session starts"

# attach on a pty and capture the outward byte stream. SIGKILL on timeout so
# the capture reflects the live steady state, without exit-time cleanup.
timeout -s KILL 7 script -q -e -c "$BIN attach -s kbd" "$CAP" </dev/null \
    >/dev/null 2>&1 || true

# grep for kitty keyboard-protocol ops: private marker (< = >) then digits,
# ending in u. modifyOtherKeys (... m) and DECKPAM (ESC = / ESC >) do not
# match, so these patterns isolate the keyboard-flag ops.
# grep does not read \033 in the pattern as an ESC byte, so build a real one.
# grep -c prints a count (0 on no match) and is the value we want; its exit
# status is not meaningful here.
ESC=$(printf '\033')
pushops=$(LC_ALL=C grep -acE "$ESC\[>[0-9]*u" "$CAP" 2>/dev/null); pushops=${pushops:-0}
popops=$(LC_ALL=C  grep -acE "$ESC\[<[0-9;]*u" "$CAP" 2>/dev/null); popops=${popops:-0}
setops=$(LC_ALL=C  grep -acE "$ESC\[=[0-9;]*u" "$CAP" 2>/dev/null); setops=${setops:-0}

if [ "$setops" -gt 0 ]; then
	pass "forwards kitty flags with set ops (CSI = u)"
else
	fail "forwards kitty flags with set ops (CSI = u)" \
	    "no set op captured; forwarding did not engage"
fi

if [ "$pushops" -eq 0 ] && [ "$popops" -eq 0 ]; then
	pass "no stack push/pop leak (CSI > u / CSI < u)"
else
	fail "no stack push/pop leak (CSI > u / CSI < u)" \
	    "captured $pushops push and $popops pop ops"
fi

printf "kbd_leak: %d tests, %d failures\n" "$total" "$fail"
[ "$fail" -eq 0 ]
