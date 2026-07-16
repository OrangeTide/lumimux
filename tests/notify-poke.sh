#!/bin/sh
# notify-poke.sh : emit desktop-notification OSC sequences for manual
# verification of lumi's notification passthrough (see attach.c:osc_passthru)
# Copyright (c) 2026 Jon Mayo
# Licensed under MIT-0 OR PUBLIC DOMAIN
#
# Run this inside a lumi window (focused or in the background) and watch
# the outer terminal for a pop-up / notification. Try it under different
# lumi configurations: screen mode, tiled with multiple panes, turbo mode,
# and from a window that is not currently focused.
#
# Usage: tests/notify-poke.sh [osc9|osc99|osc777|all]

kind="${1:-all}"
title="lumi notify-poke"
body="hello from $(hostname 2>/dev/null || echo lumi) at $$"

osc9() {
	printf '\033]9;%s\033\\' "$body"
}

osc99() {
	# kitty desktop notification: OSC 99 ; metadata ; payload
	printf '\033]99;i=1:d=0:p=title;%s\033\\' "$title"
	printf '\033]99;i=1:d=1:p=body;%s\033\\' "$body"
}

osc777() {
	printf '\033]777;notify;%s;%s\033\\' "$title" "$body"
}

case "$kind" in
	osc9)
		osc9
		;;
	osc99)
		osc99
		;;
	osc777)
		osc777
		;;
	all)
		osc9
		osc99
		osc777
		;;
	*)
		echo "usage: $0 [osc9|osc99|osc777|all]" >&2
		exit 1
		;;
esac
