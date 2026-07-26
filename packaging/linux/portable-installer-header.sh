#!/bin/sh
set -eu

marker="__UCI_ARENA_PAYLOAD_BELOW__"
line="$(awk "/^${marker}\$/ { print NR + 1; exit }" "$0")"
if [ -z "${line}" ]; then
  echo "UCI Arena portable payload marker is missing" >&2
  exit 2
fi

temporary="$(mktemp -d "${TMPDIR:-/tmp}/uci-arena-setup.XXXXXX")"
cleanup() {
  rm -rf -- "${temporary}"
}
trap cleanup EXIT HUP INT TERM

tail -n +"${line}" "$0" | tar -xzf - -C "${temporary}"
exec "${temporary}/bin/arena-setup" \
  --catalog "${temporary}/release/catalog.json" \
  --metadata-dir "${temporary}/release/tuf" \
  --offline "$@"

exit 2
__UCI_ARENA_PAYLOAD_BELOW__
