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
VERSION="0.1.0"
ARCH="amd64"
MAINTAINER="Jon Mayo <jon@example.com>"
DESCRIPTION="Terminal multiplexer with GNU Screen keybindings"

LUMI_CMDS="attach mserver new list version kill detach new-window \
reload send-input send-keys splash"

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
TRIPLET="$(cc -dumpmachine 2>/dev/null || echo x86_64-linux-gnu)"

# Find binary
if [ -n "$1" ]; then
	BINARY="$1"
elif [ -f "$REPO/_out/x86_64-linux-musl/bin/lumi" ]; then
	BINARY="$REPO/_out/x86_64-linux-musl/bin/lumi"
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

# Install tree
mkdir -p "$PKGDIR/usr/bin"
install -m 755 "$BINARY" "$PKGDIR/usr/bin/lumi"
for cmd in $LUMI_CMDS; do
	ln -sf lumi "$PKGDIR/usr/bin/lumi-$cmd"
done

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
