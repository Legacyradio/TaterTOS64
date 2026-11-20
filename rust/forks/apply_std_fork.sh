#!/usr/bin/env bash
# Re-applies the TaterTOS std fork onto the active nightly rust-src.
set -e
STDSRC=~/.rustup/toolchains/nightly-x86_64-unknown-linux-gnu/lib/rustlib/src/rust/library
HERE="$(cd "$(dirname "$0")" && pwd)"
rsync -a "$HERE/rust-std-edits/" "$STDSRC/"
echo "TaterTOS std fork applied to $STDSRC"
