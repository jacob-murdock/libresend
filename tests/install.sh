#!/bin/sh
# Prove `make install` ships libresend.so.0, include/resend.h, and libresend.pc.
# The pkg-config version is the client's LIBRESEND_VERSION. libcurl, OpenSSL,
# and a CA bundle stay off the install tree.
set -eu

root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cd "$root"
make_bin=${1:-make}

version=$(sed -n 's/^#define LIBRESEND_VERSION "\([^"]*\)".*/\1/p' src/internal.h)
if [ -z "$version" ]; then
    echo "install: LIBRESEND_VERSION missing from src/internal.h" >&2
    exit 1
fi

stage=$(mktemp -d)
work=$(mktemp -d)
trap 'rm -rf "$stage" "$work"' EXIT INT TERM

"$make_bin" install DESTDIR="$stage" PREFIX=/usr

fail() {
    echo "install: $*" >&2
    exit 1
}

expected='usr
usr/include
usr/include/resend.h
usr/lib
usr/lib/libresend.so
usr/lib/libresend.so.0
usr/lib/pkgconfig
usr/lib/pkgconfig/libresend.pc'

found=$(cd "$stage" && find . ! -name . | sed 's|^\./||' | LC_ALL=C sort)
if [ "$found" != "$expected" ]; then
    printf 'install: tree mismatch\nexpected:\n%s\nfound:\n%s\n' "$expected" "$found" >&2
    exit 1
fi

cmp -s include/resend.h "$stage/usr/include/resend.h" || fail "installed resend.h differs"

so="$stage/usr/lib/libresend.so.0"
[ -f "$so" ] && [ ! -L "$so" ] || fail "libresend.so.0 is not a regular file"
[ "$(readlink "$stage/usr/lib/libresend.so")" = "libresend.so.0" ] || fail "libresend.so symlink"

dyn=$(readelf -d "$so")
printf '%s\n' "$dyn" | grep -q 'SONAME.*\[libresend\.so\.0\]' || fail "SONAME"
printf '%s\n' "$dyn" | grep -q 'NEEDED.*\[libcurl\.so' || fail "libcurl is not a dynamic dependency"
printf '%s\n' "$dyn" | grep -q 'NEEDED.*\[libssl\.so' || fail "libssl is not a dynamic dependency"
printf '%s\n' "$dyn" | grep -q 'NEEDED.*\[libcrypto\.so' || fail "libcrypto is not a dynamic dependency"
if printf '%s\n' "$dyn" | grep -E -q 'RPATH|RUNPATH'; then
    fail "RPATH would pin a shipped library"
fi

pc="$stage/usr/lib/pkgconfig/libresend.pc"
grep -q '^prefix=/usr$' "$pc" || fail "prefix is not /usr"
grep -q -x -F "Version: $version" "$pc" || fail "pkg-config version is not $version"
grep -q -x -F "Requires.private: libcurl openssl" "$pc" || fail "Requires.private"
if grep -q "$stage" "$pc"; then
    fail "pkg-config file contains the staging directory"
fi

export PKG_CONFIG_PATH="$stage/usr/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
pc_ver=$(pkg-config --define-variable=prefix="$stage/usr" --modversion libresend)
[ "$pc_ver" = "$version" ] || fail "modversion $pc_ver != $version"

libs=$(pkg-config --define-variable=prefix="$stage/usr" --libs libresend)
static_libs=$(pkg-config --define-variable=prefix="$stage/usr" --static --libs libresend)
if printf '%s\n' "$libs" | grep -q -- '-lcurl'; then
    fail "--libs leaks libcurl"
fi
if printf '%s\n' "$libs" | grep -q -- '-lssl'; then
    fail "--libs leaks libssl"
fi
printf '%s\n' "$static_libs" | grep -q -- '-lcurl' || fail "--static --libs omits libcurl"
printf '%s\n' "$static_libs" | grep -q -- '-lssl' || fail "--static --libs omits libssl"

cat > "$work/smoke.c" <<'EOF'
#include <resend.h>
int main(void) {
    resend_free(NULL);
    return 0;
}
EOF
# shellcheck disable=SC2086
${CC:-cc} ${CFLAGS:-} $(pkg-config --define-variable=prefix="$stage/usr" --cflags libresend) \
    -o "$work/smoke" "$work/smoke.c" $libs
LD_LIBRARY_PATH="$stage/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" "$work/smoke"
