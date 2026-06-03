#!/usr/bin/env bash
set -u

stamp="${1:-$(date +%Y%m%d_%H%M%S)}"
repo="/home/legacyindieradio/chromium-tatertos/src"
ninja="${repo}/third_party/ninja/ninja"
out="${repo}/out/tatertos"
log_dir="/home/legacyindieradio/TaterTOS64/logs"
build_log="${log_dir}/chrome_build_${stamp}.log"
monitor_log="${log_dir}/chrome_build_${stamp}_monitor.log"
pid_file="${log_dir}/chrome_build_${stamp}.pid"

mkdir -p "${log_dir}"
: > "${build_log}"
: > "${monitor_log}"

(
  echo "START $(date -Iseconds)"
  "${ninja}" -C "${out}" chrome
  code=$?
  echo "EXIT ${code} $(date -Iseconds)"
  exit "${code}"
) > "${build_log}" 2>&1 &
build_pid=$!

cat > "${pid_file}" <<EOF
BUILD_PID=${build_pid}
BUILD_LOG=${build_log}
MONITOR_LOG=${monitor_log}
START=$(date -Iseconds)
EOF

last_size=-1
last_change_epoch=$(date +%s)

{
  echo "MONITOR START $(date -Iseconds)"
  echo "build_pid=${build_pid}"
  echo "build_log=${build_log}"

  while kill -0 "${build_pid}" 2>/dev/null; do
    now_epoch=$(date +%s)
    size=$(stat -c %s "${build_log}" 2>/dev/null || echo 0)
    if [[ "${size}" != "${last_size}" ]]; then
      last_size="${size}"
      last_change_epoch="${now_epoch}"
    fi

    idle_seconds=$((now_epoch - last_change_epoch))
    echo "--- $(date -Iseconds) pid=${build_pid} alive log_bytes=${size} idle_seconds=${idle_seconds} ---"
    ps -p "${build_pid}" -o pid,ppid,stat,etime,cmd || true
    tail -n 50 "${build_log}" || true
    sleep 60
  done

  wait "${build_pid}"
  code=$?
  echo "MONITOR EXIT ${code} $(date -Iseconds)"
  echo "--- final build log tail ---"
  tail -n 160 "${build_log}" || true
  exit 0
} >> "${monitor_log}" 2>&1
