#!/bin/sh
# bump-version.sh — update VERSION, commit, and tag.
# Usage: ./scripts/bump-version.sh [YY.MM.patch]
# CalVer style version with Year, month, and patch number.
# Omit the argument to use today's date.

set -e

cd "$(git rev-parse --show-toplevel)"

if [ -n "$1" ]; then
    version="$1"
else
    version="$(date +%y.%m.0)"
fi

# Inrement .N suffix if tag already exists
base="${version%.*}"
n=1
while git rev-parse "v$version" >/dev/null 2>&1; do
    version="${base}.${n}"
    n=$((n + 1))
done

# printf '%s\n' "$version" > VERSION
# git add VERSION
sed -i "s/^#define LUMI_VERSION.*\$/#define LUMI_VERSION \"$version\"/" src/version.h
git add src/version.h

git commit -m "Release v$version"
git tag -a "v$version" -m "v$version"

echo "Tagged v$version"

