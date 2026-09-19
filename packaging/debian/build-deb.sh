#!/bin/sh
# build-deb.sh -- create a .deb package from a pre-built lumi binary
# Copyright (c) 2026 Jon Mayo
# Licensed under MIT-0 OR PUBLIC DOMAIN
#
# Usage: packaging/debian/build-deb.sh [binary-path]
#
# Defaults to the musl static release binary if available,
# otherwise falls back to the native build.

set -e

PKGNAME="lumimux"
ARCH="amd64"
MAINTAINER="Jon Mayo <jon.mayo@gmail.com>"
DESCRIPTION="Terminal multiplexer with GNU Screen keybindings"

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
TRIPLET="$(cc -dumpmachine 2>/dev/null || echo x86_64-linux-gnu)"

# Package version tracks the C header so it matches the release, with an env
# override for a caller that knows better (e.g. a CI tag).
VERSION="${VERSION:-$(sed -n 's/.*LUMI_VERSION "\([^"]*\)".*/\1/p' "$REPO/src/version.h")}"

# Find binary
if [ -n "$1" ]; then
	BINARY="$1"
elif [ -f "$REPO/_out/x86_64-linux-musl/release/bin/lumi" ]; then
	BINARY="$REPO/_out/x86_64-linux-musl/release/bin/lumi"
elif [ -f "$REPO/_out/x86_64-linux-musl/bin/lumi" ]; then
	BINARY="$REPO/_out/x86_64-linux-musl/bin/lumi"
elif [ -f "$REPO/_out/$TRIPLET/release/bin/lumi" ]; then
	BINARY="$REPO/_out/$TRIPLET/release/bin/lumi"
elif [ -f "$REPO/_out/$TRIPLET/bin/lumi" ]; then
	BINARY="$REPO/_out/$TRIPLET/bin/lumi"
else
	printf 'error: no lumi binary found; build first\n' >&2
	exit 1
fi

# Detect architecture from binary
case "$(file "$BINARY")" in
*x86-64*|*x86_64*) ARCH="amd64" ;;
*aarch64*|*ARM\ aarch64*) ARCH="arm64" ;;
esac

PKGDIR="$(mktemp -d)"
trap 'rm -rf "$PKGDIR"' EXIT

# Stage the install tree via the makefile so the binary, its lumi-* symlinks,
# and the manual page match a normal install. Derive the build configuration
# from the binary's _out/<triplet>[/release]/ path so the install target
# resolves to the same output the binary came from.
case "$BINARY" in
*_out/*/bin/lumi)
	sub="${BINARY##*_out/}"
	sub="${sub%/bin/lumi}"
	MK_TRIPLET="${sub%%/*}"
	case "$sub" in */release) MK_RELEASE="RELEASE=1" ;; *) MK_RELEASE="" ;; esac
	case "$MK_TRIPLET" in
	*musl*) MK_TC="CC=musl-gcc LDFLAGS=-static" ;;
	*) MK_TC="" ;;
	esac
	;;
*)
	printf 'error: %s is not under _out/<triplet>/; cannot derive build config\n' \
		"$BINARY" >&2
	exit 1
	;;
esac

make -C "$REPO" install PREFIX=/usr DESTDIR="$PKGDIR" \
	TARGET_TRIPLET="$MK_TRIPLET" $MK_RELEASE $MK_TC

# Control file
mkdir -p "$PKGDIR/DEBIAN"
cat > "$PKGDIR/DEBIAN/control" <<CTRL
Package: $PKGNAME
Version: $VERSION
Architecture: $ARCH
Maintainer: $MAINTAINER
Description: $DESCRIPTION
 lumiMUX is a rewrite of GNU Screen with git-style sub-commands,
 tiled pane splits, configurable themes, and a built-in splash screen.
 Single static binary with no runtime dependencies.
Section: utils
Priority: optional
CTRL

# Build .deb
OUTFILE="${PKGNAME}_${VERSION}_${ARCH}.deb"
dpkg-deb --build --root-owner-group "$PKGDIR" "$REPO/$OUTFILE"

printf 'created %s\n' "$OUTFILE"
