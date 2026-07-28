#!/bin/sh
set -eu

if [ "$#" -ne 3 ]; then
  echo "usage: $0 <payload-directory> <header-script> <output.run>" >&2
  exit 2
fi

payload="$1"
header="$2"
output="$3"

test -x "${payload}/bin/arena-setup"
test -f "${payload}/release/catalog.json"
test -f "${payload}/release/tuf/root.json"
test -f "${header}"

temporary="${output}.partial"
rm -f -- "${temporary}"
cp -- "${header}" "${temporary}"
tar --sort=name --mtime="@${SOURCE_DATE_EPOCH:-0}" \
  --owner=0 --group=0 --numeric-owner \
  -czf - -C "${payload}" . >> "${temporary}"
chmod 0755 "${temporary}"
mv -f -- "${temporary}" "${output}"
