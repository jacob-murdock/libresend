# Sourced by build.sh, install.sh, and dist.sh.
# Those scripts check the Makefile's default CFLAGS. The sanitizer job
# exports its own, and `make CFLAGS=...` copies that assignment into
# MAKEFLAGS after " -- " (spaces in the value are backslash-escaped).
# Drop both. caller_cflags=1 when either form was present, so install.sh
# can rebuild objects that were already compiled with the override.

drop_caller_cflags() {
    caller_cflags=0
    if [ -n "${CFLAGS+x}" ]; then
        caller_cflags=1
    fi
    unset CFLAGS

    if [ -n "${MAKEFLAGS-}" ]; then
        case "$MAKEFLAGS" in
            *" -- "*)
                cf_flags=${MAKEFLAGS%% -- *}
                cf_rest=${MAKEFLAGS#* -- }
                cf_kept=
                while [ -n "$cf_rest" ]; do
                    cf_tok=
                    while [ -n "$cf_rest" ]; do
                        cf_c=${cf_rest%"${cf_rest#?}"}
                        cf_rest=${cf_rest#?}
                        if [ "$cf_c" = '\' ]; then
                            if [ -z "$cf_rest" ]; then
                                cf_tok=$cf_tok$cf_c
                                break
                            fi
                            cf_n=${cf_rest%"${cf_rest#?}"}
                            cf_rest=${cf_rest#?}
                            cf_tok=$cf_tok$cf_c$cf_n
                            continue
                        fi
                        if [ "$cf_c" = ' ' ]; then
                            break
                        fi
                        cf_tok=$cf_tok$cf_c
                    done
                    case "$cf_tok" in
                        '' ) ;;
                        CFLAGS=*) caller_cflags=1 ;;
                        *) cf_kept=${cf_kept:+$cf_kept }$cf_tok ;;
                    esac
                done
                if [ -n "$cf_kept" ]; then
                    MAKEFLAGS="$cf_flags -- $cf_kept"
                else
                    MAKEFLAGS=$cf_flags
                fi
                export MAKEFLAGS
                ;;
        esac
    fi
    unset cf_flags cf_rest cf_kept cf_tok cf_c cf_n
}
