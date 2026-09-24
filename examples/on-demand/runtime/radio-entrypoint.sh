#!/bin/sh
set -eu
umask 077
set -- --standby --socket /run/barista/engine.sock --interface "$GAMEPAD_INTERFACE"
if [ -n "${GAMEPAD_PAIR_CODE:-}" ]; then
    case "$GAMEPAD_PAIR_CODE" in
        *[!0-3]*) exit 64 ;;
    esac
    [ "${#GAMEPAD_PAIR_CODE}" -eq 4 ] || exit 64
    set -- "$@" --pair --pair-code "$GAMEPAD_PAIR_CODE"
else
    test -s /var/lib/drcd/credentials.conf || exit 78
    set -- "$@" --np
fi
exec /usr/local/bin/barista-engine "$@"
