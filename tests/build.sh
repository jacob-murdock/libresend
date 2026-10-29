#!/bin/sh
# Prove the default build uses -O2, keeps -fstack-protector-strong when
# CFLAGS is replaced, and puts _FORTIFY_SOURCE in CPPFLAGS. relro/now is
# only on the shared-library link, so the static objects MinGW builds do
# not see -Wl. The sanitizer job uses -O1; the c99 job uses -O2.
set -eu

root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cd "$root"
# shellcheck disable=SC1091
. "$root/tests/cflags.sh"
drop_caller_cflags
make_bin=${1:-make}

version=$(sed -n 's/^#define LIBRESEND_VERSION "\([^"]*\)".*/\1/p' include/resend.h)
if [ -z "$version" ]; then
    echo "build: LIBRESEND_VERSION missing from include/resend.h" >&2
    exit 1
fi

fail() {
    echo "build: $*" >&2
    exit 1
}

need() {
    printf '%s\n' "$1" | grep -q -F -- "$2" || fail "$3: $1"
}

lack() {
    if printf '%s\n' "$1" | grep -q -F -- "$2"; then
        fail "$3: $1"
    fi
}

obj=$("$make_bin" -n -B build/buf.o) || fail "dry-run object"
obj_line=$(printf '%s\n' "$obj" | grep -F ' -c src/buf.c -o build/buf.o')
[ -n "$obj_line" ] || fail "no object compile line"
need "$obj_line" "-U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=2 -std=c99 -Wall -Wextra -Werror -pthread -O2 -fstack-protector-strong" \
    "default object flags"
lack "$obj_line" "relro" "relro on an object compile"

replaced=$("$make_bin" -n -B build/buf.o CFLAGS=-g) || fail "dry-run CFLAGS=-g"
replaced_line=$(printf '%s\n' "$replaced" | grep -F ' -c src/buf.c -o build/buf.o')
[ -n "$replaced_line" ] || fail "no CFLAGS=-g compile line"
need "$replaced_line" "-U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=2 -g -fstack-protector-strong" \
    "CFLAGS=-g dropped stack protector or fortify"
lack "$replaced_line" "-O2" "CFLAGS=-g still forces -O2"
lack "$replaced_line" "relro" "relro on a CFLAGS=-g compile"

static=$("$make_bin" -n -B build/libresend.a) || fail "dry-run static library"
lack "$static" "relro" "relro on the static library build"
lack "$static" "-z,now" "now on the static library build"

shared=$("$make_bin" -n -B "build/libresend.so.$version") || fail "dry-run shared library"
link=$(printf '%s\n' "$shared" | grep -F ' -shared ')
[ -n "$link" ] || fail "no shared link line"
need "$link" "-Wl,-z,defs -Wl,-z,relro,-z,now" "shared link lacks relro next to defs"
need "$link" "-Wl,--as-needed" "shared link keeps unused -lssl"
compiles=$(printf '%s\n' "$shared" | grep -F ' -c ' || true)
lack "$compiles" "relro" "relro on a shared-library object compile"

ci=.github/workflows/ci.yml
san=$(grep -F 'fsanitize=address,undefined' "$ci")
[ -n "$san" ] || fail "sanitize CFLAGS line missing"
need "$san" " -O1" "sanitize CFLAGS lacks -O1"
lack "$san" "-O2" "sanitize CFLAGS uses -O2"
need "$(cat "$ci")" 'CFLAGS="-std=c99 -pedantic -Wall -Wextra -pthread -O2"' \
    "c99 job lacks -O2"
