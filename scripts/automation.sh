#!/bin/bash
# automation.sh -- helper commands for development and testing
# Usage: scripts/automation.sh <command> [args...]
#
# Paths and environment are set up automatically based on the
# build output directory.

set -e

REPO="$(cd "$(dirname "$0")/.." && pwd)"
TRIPLET="$(cc -dumpmachine 2>/dev/null || echo x86_64-linux-gnu)"
BIN="$REPO/_out/$TRIPLET/bin"
SOCK_DIR="/run/user/$(id -u)/lumi"

export LUMI_LIBEXEC_PATH="$BIN"
export PATH="$BIN:$PATH"

SESSION="${LUMI_SESSION:-test}"

usage() {
	cat <<-EOF
	Usage: scripts/automation.sh <command> [args...]

	Session management:
	  start [name]       Start a detached server (default: "$SESSION")
	  stop [name]        Kill a running server
	  new-window [name]  Add a window to a session
	  attach [name]      Attach to a session (interactive)
	  attach-turbo [name]  Attach in turbo mode (interactive)
	  list               List session sockets

	Testing:
	  trace-attach [name]  Attach in turbo mode with INPUT_TRACE
	  show-trace         Show the input trace log
	  run-pty-test       Run the PTY-based rapid input test
	  run-tests          Build and run all unit tests

	Build:
	  build              Build all targets
	  build-trace        Build lumi-attach with INPUT_TRACE enabled
	  clean              Clean and rebuild

	Environment:
	  env                Print key paths and variables
	EOF
}

cmd_env() {
	echo "REPO=$REPO"
	echo "BIN=$BIN"
	echo "SOCK_DIR=$SOCK_DIR"
	echo "LUMI_LIBEXEC_PATH=$LUMI_LIBEXEC_PATH"
	echo "SESSION=$SESSION"
	echo "TRIPLET=$TRIPLET"
}

cmd_start() {
	local name="${1:-$SESSION}"

	if [ -d "$SOCK_DIR/$name" ] && ls "$SOCK_DIR/$name"/*/socket >/dev/null 2>&1; then
		echo "Session '$name' already running"
		return 0
	fi
	"$BIN/lumi-new" -d -n "$name"
	sleep 0.5
	echo "Started session '$name'"
}

cmd_stop() {
	local name="${1:-$SESSION}"

	pkill -f "lumi-mserver.*$name" 2>/dev/null || true
	sleep 0.3
	rm -rf "$SOCK_DIR/$name"
	echo "Stopped session '$name'"
}

cmd_new_window() {
	local name="${1:-$SESSION}"
	"$BIN/lumi-new-window" -s "$name"
}

cmd_attach() {
	local name="${1:-$SESSION}"
	"$BIN/lumi-attach" -s "$name"
}

cmd_attach_turbo() {
	local name="${1:-$SESSION}"
	"$BIN/lumi-attach" -s "$name" -m turbo
}

cmd_list() {
	ls -la "$SOCK_DIR/" 2>/dev/null || echo "No sessions"
}

cmd_trace_attach() {
	local name="${1:-$SESSION}"
	rm -f /tmp/lumi-input-trace.log
	"$BIN/lumi-attach" -s "$name" -m turbo
	echo "--- trace log ---"
	cat /tmp/lumi-input-trace.log 2>/dev/null || echo "No trace (build with INPUT_TRACE?)"
}

cmd_show_trace() {
	cat /tmp/lumi-input-trace.log 2>/dev/null || echo "No trace log"
}

cmd_run_pty_test() {
	local name="test-pty-$$"

	echo "Building..."
	make -C "$REPO" -j lumi-attach lumi-new lumi-new-window lumi-mserver >/dev/null 2>&1

	if [ ! -f /tmp/test_pty_input ]; then
		echo "Compiling PTY test harness..."
		cc -o /tmp/test_pty_input /tmp/test_pty_input.c
	fi

	# Clean any old session
	pkill -f "lumi-mserver.*$name" 2>/dev/null || true
	rm -f "$SOCK_DIR/$name.sock" /tmp/lumi-input-trace.log

	LUMI_BIN="$BIN" LUMI_SESSION="$name" /tmp/test_pty_input
}

cmd_run_tests() {
	make -C "$REPO" run-tests
}

cmd_build() {
	make -C "$REPO" -j
}

cmd_build_trace() {
	echo "Note: INPUT_TRACE must be #defined in attach.c (hardcoded)"
	make -C "$REPO" -j lumi-attach
}

cmd_clean() {
	make -C "$REPO" clean
	make -C "$REPO" -j
}

# dispatch
case "${1:-}" in
	env)           cmd_env ;;
	start)         cmd_start "${2:-}" ;;
	stop)          cmd_stop "${2:-}" ;;
	new-window)    cmd_new_window "${2:-}" ;;
	attach)        cmd_attach "${2:-}" ;;
	attach-turbo)  cmd_attach_turbo "${2:-}" ;;
	list)          cmd_list ;;
	trace-attach)  cmd_trace_attach "${2:-}" ;;
	show-trace)    cmd_show_trace ;;
	run-pty-test)  cmd_run_pty_test ;;
	run-tests)     cmd_run_tests ;;
	build)         cmd_build ;;
	build-trace)   cmd_build_trace ;;
	clean)         cmd_clean ;;
	help|-h|--help) usage ;;
	"")            usage; exit 1 ;;
	*)             echo "Unknown command: $1"; usage; exit 1 ;;
esac
