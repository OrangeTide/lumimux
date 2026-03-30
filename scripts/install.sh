#!/bin/sh
# install.sh -- install lumi from pre-built binary or source
# Copyright (c) 2026 Jon Mayo
# Licensed under MIT-0 OR PUBLIC DOMAIN
#
# Usage:
#   curl -fsSL https://raw.githubusercontent.com/jonesMUX/lumimux/main/scripts/install.sh | sh
#   sh scripts/install.sh                     # install to ~/.local/bin
#   PREFIX=/usr/local sh scripts/install.sh   # install to /usr/local/bin
#   sh scripts/install.sh --from-source       # build from source tree
#
# Environment variables:
#   PREFIX    installation prefix (default: ~/.local)
#   GITHUB_REPO   GitHub owner/repo for release downloads
#                  (default: jonesMUX/lumimux)

set -e

GITHUB_REPO="${GITHUB_REPO:-jonesMUX/lumimux}"
PREFIX="${PREFIX:-$HOME/.local}"
BINDIR="$PREFIX/bin"

LUMI_CMDS="attach mserver new list version kill detach new-window \
reload send-input send-keys splash"

die() {
	printf 'error: %s\n' "$1" >&2
	exit 1
}

detect_arch() {
	arch="$(uname -m)"
	case "$arch" in
	x86_64|amd64) arch="x86_64" ;;
	aarch64|arm64) arch="aarch64" ;;
	*) die "unsupported architecture: $arch" ;;
	esac
	printf '%s' "$arch"
}

detect_os() {
	os="$(uname -s)"
	case "$os" in
	Linux)  os="linux" ;;
	Darwin) os="darwin" ;;
	*)      die "unsupported OS: $os" ;;
	esac
	printf '%s' "$os"
}

create_symlinks() {
	for cmd in $LUMI_CMDS; do
		ln -sf lumi "$BINDIR/lumi-$cmd"
	done
}

install_binary() {
	os="$(detect_os)"
	arch="$(detect_arch)"

	tag="$(curl -fsSL "https://api.github.com/repos/$GITHUB_REPO/releases/latest" \
		| grep '"tag_name"' | head -1 | sed 's/.*: *"//;s/".*//')" \
		|| die "failed to fetch latest release tag"

	asset="lumi-${os}-${arch}"
	url="https://github.com/$GITHUB_REPO/releases/download/$tag/$asset"

	printf 'downloading %s %s ...\n' "$tag" "$asset"

	mkdir -p "$BINDIR"

	if command -v curl >/dev/null 2>&1; then
		curl -fsSL -o "$BINDIR/lumi" "$url" \
			|| die "download failed: $url"
	elif command -v wget >/dev/null 2>&1; then
		wget -qO "$BINDIR/lumi" "$url" \
			|| die "download failed: $url"
	else
		die "curl or wget required"
	fi

	chmod +x "$BINDIR/lumi"
	create_symlinks

	printf 'installed lumi %s to %s\n' "$tag" "$BINDIR"
}

install_from_source() {
	# Must be run from the repo root
	if [ ! -f GNUmakefile ] || [ ! -f src/module.mk ]; then
		die "--from-source must be run from the lumimux repo root"
	fi

	printf 'building from source ...\n'

	if command -v musl-gcc >/dev/null 2>&1; then
		make CC=musl-gcc LDFLAGS=-static \
			TARGET_TRIPLET=x86_64-linux-musl RELEASE=1 lumi
		src="_out/x86_64-linux-musl/bin/lumi"
	else
		make RELEASE=1 lumi
		triplet="$(cc -dumpmachine 2>/dev/null || echo x86_64-linux-gnu)"
		src="_out/$triplet/bin/lumi"
	fi

	mkdir -p "$BINDIR"
	cp "$src" "$BINDIR/lumi"
	chmod +x "$BINDIR/lumi"
	create_symlinks

	printf 'installed lumi to %s\n' "$BINDIR"
}

check_path() {
	case ":$PATH:" in
	*":$BINDIR:"*) ;;
	*)
		printf '\nnote: %s is not in PATH\n' "$BINDIR"
		printf 'add this to your shell profile:\n'
		printf '  export PATH="%s:$PATH"\n' "$BINDIR"
		;;
	esac
}

# --- main ---

from_source=0
for arg in "$@"; do
	case "$arg" in
	--from-source) from_source=1 ;;
	--help|-h)
		printf 'usage: %s [--from-source]\n' "$0"
		printf '\nInstall lumi binary and symlinks.\n'
		printf '\nOptions:\n'
		printf '  --from-source   build from local source tree\n'
		printf '\nEnvironment:\n'
		printf '  PREFIX          installation prefix (default: ~/.local)\n'
		exit 0
		;;
	*) die "unknown option: $arg" ;;
	esac
done

if [ "$from_source" = 1 ]; then
	install_from_source
else
	install_binary
fi

check_path
