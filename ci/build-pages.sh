#!/bin/sh
# Build the GitHub Pages site from doc/ sources.
# Requires: pandoc
# Environment: REPO_URL (optional, for footer links)
set -e

REPO_URL="${REPO_URL:-https://github.com/OrangeTide/lumi}"

mkdir -p _site

# Append footer links to a temp copy
tmp=$(mktemp)
cat doc/index.md > "$tmp"
printf '\n---\n\n[Source Code](%s) | [Releases](%s/releases) | [Issues](%s/issues)\n' \
  "$REPO_URL" "$REPO_URL" "$REPO_URL" >> "$tmp"

pandoc "$tmp" \
  -f markdown -t html \
  --standalone \
  --metadata title="lumimux(1)" \
  --css style.css \
  -o _site/index.html

rm -f "$tmp"
cp doc/style.css _site/style.css
