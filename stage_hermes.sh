#!/usr/bin/env bash
# fry1631: Native Python 3.14 + Hermes Agent staging script
# Sourced by build_iso.sh before ISO creation.

stage_tree_totfs() {
  local src="$1"
  local dest="$2"
  local rel target item

  [ -d "$src" ] || return 0

  mkdir_p_totfs "$dest"

  find "$src" -type d | while IFS= read -r item; do
    rel="${item#$src}"
    target="$dest$rel"
    mkdir_p_totfs "$target"
  done

  find "$src" -type f | while IFS= read -r item; do
    rel="${item#$src}"
    target="$dest$rel"
    "$ROOT_DIR/tools/totcopy" "$NVME_IMG" "$NVME_PART_OFFSET" \
      "$item" "$target" 2>/dev/null || true
  done
}

mkdir_p_totfs() {
  local path="$1"
  local old_ifs="$IFS"
  local cur=""
  local part

  IFS="/"
  for part in $path; do
    [ -n "$part" ] || continue
    cur="$cur/$part"
    "$ROOT_DIR/tools/totcopy" "$NVME_IMG" "$NVME_PART_OFFSET" \
      --mkdir "$cur" 2>/dev/null || true
  done
  IFS="$old_ifs"
}

# Copy shell to /apps/ so init finds it first (before ISO ramdisk version)
if [ -f "$ROOT_DIR/shell.tot" ]; then
  "$ROOT_DIR/tools/totcopy" "$NVME_IMG" "$NVME_PART_OFFSET" --mkdir "/apps" 2>/dev/null || true
  "$ROOT_DIR/tools/totcopy" "$NVME_IMG" "$NVME_PART_OFFSET" \
    "$ROOT_DIR/shell.tot" "/apps/SHELL.TOT"
fi

# Native Python binary + Hermes sysroot
STAGE_HERMES="${STAGE_HERMES:-1}"
if [ "$STAGE_HERMES" = "1" ] && [ -f "$ROOT_DIR/python3.fry" ]; then
  echo "=== Staging TaterTOS-native Python 3.14 + Hermes Agent ==="
  HERMES_SYSROOT="${HERMES_SYSROOT:-$ROOT_DIR/sysroot}"
  
  echo "  python3.fry -> /apps/python3.fry"
  "$ROOT_DIR/tools/totcopy" "$NVME_IMG" "$NVME_PART_OFFSET" \
    "$ROOT_DIR/python3.fry" "/apps/python3.fry"

  if [ -d "$HERMES_SYSROOT/usr/local/lib/python3.14" ]; then
    echo "  Staging Python stdlib + site-packages..."
    stage_tree_totfs "$HERMES_SYSROOT/usr/local/lib/python3.14" "/usr/local/lib/python3.14"
  fi

  # fry1826: rewrite the launcher's interpreter line for THIS platform.
  # Upstream ships "#!/usr/bin/env python3". With kernel "#!" support (fry1822)
  # that resolves /usr/bin/env on the ROOT filesystem — but ToTFS is mounted at
  # /nvme (main.c:446, vfs_mount_secondary), root is the ISO ramdisk, and there
  # is no env binary there. Point the shebang at the real interpreter instead.
  # This is ordinary packaging: a distro rewrites script shebangs at install
  # time for exactly this reason. Idempotent, and only touches the env form so
  # a hand-edited shebang is never clobbered.
  HERMES_LAUNCHER="$HERMES_SYSROOT/opt/hermes-agent/hermes"
  if [ -f "$HERMES_LAUNCHER" ] && \
     head -1 "$HERMES_LAUNCHER" | grep -q '^#!/usr/bin/env[[:space:]]\+python3'; then
    sed -i '1s|^#!/usr/bin/env[[:space:]]\+python3|#!/nvme/apps/python3.fry|' \
      "$HERMES_LAUNCHER"
    echo "  Rewrote hermes shebang -> #!/nvme/apps/python3.fry"
  fi

  if [ -d "$HERMES_SYSROOT/opt/hermes-agent" ]; then
    echo "  Staging Hermes Agent (669 .py files)..."
    stage_tree_totfs "$HERMES_SYSROOT/opt/hermes-agent" "/opt/hermes-agent"
  fi

  # fry1810: package METADATA. site-packages was populated by copying package
  # directories WITHOUT their *.dist-info, so importlib.metadata.version() failed
  # ("No package metadata was found for prompt_toolkit") the moment anything
  # asked for its own version — prompt_toolkit/__init__.py:32 does exactly that.
  # The venv the packages came from has all 211 .dist-info dirs; sync any that
  # are missing so a sysroot rebuild cannot silently drop them again (fry1809
  # fixed this by hand; this makes it durable).
  HERMES_VENV_SP="${HERMES_VENV_SP:-$HOME/hermes-agent/venv/lib/python3.11/site-packages}"
  HERMES_SP="$HERMES_SYSROOT/usr/local/lib/python3.14/site-packages"
  if [ -d "$HERMES_VENV_SP" ] && [ -d "$HERMES_SP" ]; then
    _di=0
    for _d in "$HERMES_VENV_SP"/*.dist-info; do
      [ -d "$_d" ] || continue
      _b="$(basename "$_d")"
      [ -e "$HERMES_SP/$_b" ] && continue
      cp -r "$_d" "$HERMES_SP/$_b" && _di=$((_di+1))
    done
    [ "$_di" -gt 0 ] && echo "  Synced $_di missing .dist-info dirs into sysroot"
  fi

  # fry1810: user config + credentials. hermes reads $HOME/.hermes, and HOME=/nvme
  # in TaterTOS (shell.c native_python_env), so these must land at /.hermes on
  # ToTFS. ONLY these three files — ~/.hermes is 656MB of caches/history that has
  # no business on the image. auth.json carries provider credentials, config.yaml
  # selects the model (default: deepseek-v4-pro), .env carries API keys.
  # Staged with Zack's explicit go-ahead (2026-08-11).
  if [ -d "$HOME/.hermes" ]; then
    echo "  Staging hermes user config -> /.hermes (auth.json, config.yaml, .env)"
    "$ROOT_DIR/tools/totcopy" "$NVME_IMG" "$NVME_PART_OFFSET" --mkdir "/.hermes" 2>/dev/null || true
    for _f in auth.json config.yaml .env; do
      if [ -f "$HOME/.hermes/$_f" ]; then
        "$ROOT_DIR/tools/totcopy" "$NVME_IMG" "$NVME_PART_OFFSET" \
          "$HOME/.hermes/$_f" "/.hermes/$_f" || true
      fi
    done
  fi

  echo "  Done. Shell commands: python3, hermes"
fi
