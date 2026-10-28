#!/bin/sh
# Prove `make dist` writes libresend-$(VERSION).tar.gz from git archive,
# plus a SHA-256 file of that tarball.
set -eu

root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cd "$root"
make_bin=${1:-make}

if ! git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    echo "dist: skip (not a git checkout)"
    exit 0
fi

version=$(sed -n 's/^#define LIBRESEND_VERSION "\([^"]*\)".*/\1/p' src/internal.h)
if [ -z "$version" ]; then
    echo "dist: LIBRESEND_VERSION missing from src/internal.h" >&2
    exit 1
fi

fail() {
    echo "dist: $*" >&2
    exit 1
}

"$make_bin" dist

tarball="build/libresend-$version.tar.gz"
sumfile="build/libresend-$version.tar.gz.sha256"
prefix="libresend-$version/"

[ -f "$tarball" ] || fail "missing $tarball"
[ -f "$sumfile" ] || fail "missing $sumfile"

(cd build && sha256sum -c "libresend-$version.tar.gz.sha256") || fail "sha256 mismatch"

members=$(tar -tzf "$tarball")
[ -n "$members" ] || fail "empty archive"
bad=$(printf '%s\n' "$members" | grep -v "^$prefix" || true)
if [ -n "$bad" ]; then
    printf 'dist: member outside %s\n%s\n' "$prefix" "$bad" >&2
    exit 1
fi

need() {
    printf '%s\n' "$members" | grep -qxF "$prefix$1" || fail "missing $1"
}

need Makefile
need include/resend.h
need src/internal.h
need LICENSE
need README.md
need tests/check.c
need examples/send_email.c

archived=$(tar -xOf "$tarball" "${prefix}src/internal.h" |
    sed -n 's/^#define LIBRESEND_VERSION "\([^"]*\)".*/\1/p')
[ "$archived" = "$version" ] || fail "archived version $archived != $version"
