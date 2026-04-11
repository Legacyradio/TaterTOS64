#!/usr/bin/env bash
set -euo pipefail

tool_name=${1:?tool name is required}
wrapper_dir=$(cd "$(dirname "$0")/../bin" && pwd)

path_find_excluding_wrapper() {
  local name="$1"
  local old_ifs="$IFS"
  IFS=:
  for dir in $PATH; do
    [ -n "$dir" ] || continue
    [ "$dir" = "$wrapper_dir" ] && continue
    if [ -x "$dir/$name" ]; then
      printf '%s\n' "$dir/$name"
      IFS="$old_ifs"
      return 0
    fi
  done
  IFS="$old_ifs"
  return 1
}

candidate=""
if [ -x "/opt/cross/bin/$tool_name" ]; then
  candidate="/opt/cross/bin/$tool_name"
elif candidate=$(path_find_excluding_wrapper "$tool_name"); then
  :
else
  echo "Missing required tool: $tool_name" >&2
  echo "Checked /opt/cross/bin and PATH outside $wrapper_dir" >&2
  exit 127
fi

exec "$candidate" "${@:2}"
