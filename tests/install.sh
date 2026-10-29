#!/bin/sh
# Prove `make install` ships libresend.a, the soname chain
# libresend.so -> libresend.so.0 -> libresend.so.$version, include/resend.h,
# and libresend.pc. LIBDIR selects the library directory for the files and
# the .pc libdir. uninstall removes those files and leaves the directories.
# libcurl, OpenSSL, and a CA bundle stay off the install tree.
set -eu

root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cd "$root"
# shellcheck disable=SC1091
. "$root/tests/cflags.sh"
drop_caller_cflags
make_bin=${1:-make}

version=$(sed -n 's/^#define LIBRESEND_VERSION "\([^"]*\)".*/\1/p' include/resend.h)
if [ -z "$version" ]; then
    echo "install: LIBRESEND_VERSION missing from include/resend.h" >&2
    exit 1
fi

stage=$(mktemp -d)
stage64=$(mktemp -d)
work=$(mktemp -d)
trap 'rm -rf "$stage" "$stage64" "$work"' EXIT INT TERM

fail() {
    echo "install: $*" >&2
    exit 1
}

expect_tree() {
    root=$1
    want=$2
    found=$(cd "$root" && find . ! -name . | sed 's|^\./||' | LC_ALL=C sort)
    if [ "$found" != "$want" ]; then
        printf 'install: tree mismatch\nexpected:\n%s\nfound:\n%s\n' "$want" "$found" >&2
        exit 1
    fi
}

# Make will not rebuild objects when CFLAGS changes. A caller override
# (the sanitizer job) has already compiled the archive; rebuild so the
# files installed here are the default-flag library.
if [ "$caller_cflags" = 1 ]; then
    "$make_bin" -B install DESTDIR="$stage" PREFIX=/usr
else
    "$make_bin" install DESTDIR="$stage" PREFIX=/usr
fi

lib="$stage/usr/lib"
expected="usr
usr/include
usr/include/resend.h
usr/lib
usr/lib/libresend.a
usr/lib/libresend.so
usr/lib/libresend.so.0
usr/lib/libresend.so.$version
usr/lib/pkgconfig
usr/lib/pkgconfig/libresend.pc"
expect_tree "$stage" "$expected"

cmp -s include/resend.h "$stage/usr/include/resend.h" || fail "installed resend.h differs"

archive="$lib/libresend.a"
[ -f "$archive" ] && [ ! -L "$archive" ] || fail "libresend.a is not a regular file"
ar t "$archive" >/dev/null || fail "libresend.a is not an archive"
nm "$archive" | awk '{print $NF}' | grep -q -x 'resend_init' || fail "libresend.a is missing resend_init"

so="$lib/libresend.so.$version"
[ -f "$so" ] && [ ! -L "$so" ] || fail "libresend.so.$version is not a regular file"
[ -L "$lib/libresend.so.0" ] || fail "libresend.so.0 is not a symlink"
[ "$(readlink "$lib/libresend.so.0")" = "libresend.so.$version" ] || fail "libresend.so.0 symlink"
[ "$(readlink "$lib/libresend.so")" = "libresend.so.0" ] || fail "libresend.so symlink"

dyn=$(readelf -d "$so")
printf '%s\n' "$dyn" | grep -q 'SONAME.*\[libresend\.so\.0\]' || fail "SONAME"
printf '%s\n' "$dyn" | grep -q 'NEEDED.*\[libcurl\.so' || fail "libcurl is not a dynamic dependency"
# HMAC and EVP are libcrypto. libssl is unused, and --as-needed omits it.
printf '%s\n' "$dyn" | grep -q 'NEEDED.*\[libcrypto\.so' || fail "libcrypto is not a dynamic dependency"
if printf '%s\n' "$dyn" | grep -q 'NEEDED.*\[libssl\.so'; then
    fail "libssl is an unused dynamic dependency"
fi
if printf '%s\n' "$dyn" | grep -E -q 'RPATH|RUNPATH'; then
    fail "RPATH would pin a shipped library"
fi

# Only the public API is exported. Internals stay hidden.
syms=$(nm -D --defined-only "$so" | awk '{print $NF}')
printf '%s\n' "$syms" | grep -q -x 'resend_init' || fail "resend_init is not exported"
printf '%s\n' "$syms" | grep -q -x 'resend_version' || fail "resend_version is not exported"
if printf '%s\n' "$syms" | grep -E -q '^(json_|resend_json_|resend_buf_|resend_http_|resend_error_|resend_path_|resend_ascii_|resend_attachment_get|resend_attachments_list)'; then
    fail "internal symbols are exported"
fi

pc="$lib/pkgconfig/libresend.pc"
grep -q '^prefix=/usr$' "$pc" || fail "prefix is not /usr"
grep -q -x -F 'libdir=${exec_prefix}/lib' "$pc" || fail "libdir is not \${exec_prefix}/lib"
grep -q -x -F "Version: $version" "$pc" || fail "pkg-config version is not $version"
grep -q -x -F "Requires.private: libcurl openssl" "$pc" || fail "Requires.private"
if grep -q "$stage" "$pc"; then
    fail "pkg-config file contains the staging directory"
fi

export PKG_CONFIG_PATH="$lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
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
#include <string.h>
int main(void) {
    if (!resend_version() || strcmp(resend_version(), LIBRESEND_VERSION) != 0)
        return 1;
    resend_free(NULL);
    return 0;
}
EOF
# shellcheck disable=SC2086
${CC:-cc} ${CFLAGS:-} $(pkg-config --define-variable=prefix="$stage/usr" --cflags libresend) \
    -o "$work/smoke" "$work/smoke.c" $libs
LD_LIBRARY_PATH="$lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" "$work/smoke"

"$make_bin" uninstall DESTDIR="$stage" PREFIX=/usr
expect_tree "$stage" "usr
usr/include
usr/lib
usr/lib/pkgconfig"

"$make_bin" install DESTDIR="$stage64" PREFIX=/usr LIBDIR=/usr/lib64
expect_tree "$stage64" "usr
usr/include
usr/include/resend.h
usr/lib64
usr/lib64/libresend.a
usr/lib64/libresend.so
usr/lib64/libresend.so.0
usr/lib64/libresend.so.$version
usr/lib64/pkgconfig
usr/lib64/pkgconfig/libresend.pc"
pc64="$stage64/usr/lib64/pkgconfig/libresend.pc"
grep -q -x -F 'libdir=${exec_prefix}/lib64' "$pc64" || fail "libdir does not follow LIBDIR"
if grep -q "$stage64" "$pc64"; then
    fail "pkg-config file contains the staging directory"
fi
libs64=$(PKG_CONFIG_PATH="$stage64/usr/lib64/pkgconfig" pkg-config --define-variable=prefix="$stage64/usr" --libs libresend) || fail "pkg-config LIBDIR"
printf '%s\n' "$libs64" | grep -q -F -- "-L$stage64/usr/lib64" || fail "pkg-config libdir is not LIBDIR"
[ "$(readlink "$stage64/usr/lib64/libresend.so")" = "libresend.so.0" ] || fail "LIBDIR soname link"
[ "$(readlink "$stage64/usr/lib64/libresend.so.0")" = "libresend.so.$version" ] || fail "LIBDIR real link"
[ -f "$stage64/usr/lib64/libresend.a" ] && [ ! -L "$stage64/usr/lib64/libresend.a" ] || fail "LIBDIR static archive"

"$make_bin" uninstall DESTDIR="$stage64" PREFIX=/usr LIBDIR=/usr/lib64
expect_tree "$stage64" "usr
usr/include
usr/lib64
usr/lib64/pkgconfig"
