#!/usr/bin/env bash

host_env_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

prepend_path_once() {
  case ":$PATH:" in
    *:"$1":*) ;;
    *) PATH="$1:$PATH" ;;
  esac
}

prepend_path_once "$host_env_dir/bin"
prepend_path_once /usr/sbin
prepend_path_once /sbin
prepend_path_once /opt/cross/bin

export PATH
