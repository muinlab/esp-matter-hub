#!/bin/bash

export PATH="$HOME/esp/esp-matter/connectedhomeip/connectedhomeip/.environment/gn_out:$PATH"

CLUSTER_ID="0x1337FC01"
COMMISSIONER_NAME="${COMMISSIONER_NAME:-beta}"
NODE_ID="${1:-1}"
ENDPOINT_ID="${2:-10}"
DEVICE_IP="${DEVICE_IP:-192.168.75.202}"
WEB_API_KEY="${WEB_API_KEY:-}"

# ── Output Filters ──────────────────────────────────────────

_strip_ansi() {
  sed $'s/\x1b\\[[0-9;]*m//g'
}

_filter_read() {
  local output
  output="$(_strip_ansi)"

  local err
  err="$(printf '%s\n' "$output" | grep -m1 'Run command failure:')"
  if [[ -n "$err" ]]; then
    printf '\033[31mERROR: %s\033[0m\n' "${err##*Run command failure: }"
    return 1
  fi

  local data_line
  data_line="$(printf '%s\n' "$output" | grep -m1 'Data = ')"
  if [[ -z "$data_line" ]]; then
    printf '\033[31mERROR: no data in response\033[0m\n'
    return 1
  fi

  # String value: Data = "..." (N chars),
  local str_val
  str_val="$(printf '%s\n' "$data_line" | sed -n 's/.*Data = "\(.*\)" ([0-9]* chars).*/\1/p')"
  if [[ -n "$str_val" ]]; then
    printf '%s' "$str_val" | python3 -m json.tool 2>/dev/null || printf '%s\n' "$str_val"
    return 0
  fi

  # Numeric value: Data = N (unsigned/signed),
  local num_val
  num_val="$(printf '%s\n' "$data_line" | sed -n 's/.*Data = \([0-9-]*\) .*/\1/p')"
  if [[ -n "$num_val" ]]; then
    printf '%s\n' "$num_val"
    return 0
  fi

  # Fallback: show raw Data= content
  printf '%s\n' "$data_line" | sed 's/.*Data = //'
}

_filter_cmd() {
  local output
  output="$(_strip_ansi)"

  local err
  err="$(printf '%s\n' "$output" | grep -m1 'Run command failure:')"
  if [[ -n "$err" ]]; then
    printf '\033[31mERROR: %s\033[0m\n' "${err##*Run command failure: }"
    return 1
  fi

  local status_line
  status_line="$(printf '%s\n' "$output" | grep -m1 'Received Command Response Status')"
  if [[ -n "$status_line" ]]; then
    local cmd_status
    cmd_status="$(printf '%s' "$status_line" | sed -n 's/.*Status=\(0x[0-9a-fA-F]*\).*/\1/p')"
    if [[ "$cmd_status" == "0x0" ]]; then
      printf '\033[32mOK\033[0m\n'
    else
      printf '\033[31mFAIL (Status=%s)\033[0m\n' "$cmd_status"
      return 1
    fi
    return 0
  fi

  printf 'OK\n'
}

_filter_pair() {
  local output
  output="$(_strip_ansi)"

  if printf '%s\n' "$output" | grep -q 'commissioning completed with success'; then
    printf '\033[32mPaired successfully\033[0m\n'
    return 0
  fi

  local err
  err="$(printf '%s\n' "$output" | grep -m1 'commissioning Failure:\|Run command failure:')"
  if [[ -n "$err" ]]; then
    printf '\033[31mFAILED: %s\033[0m\n' "${err##*: }"
    return 1
  fi

  printf '\033[33mUnknown result. chip-tool output:\033[0m\n'
  printf '%s\n' "$output" | tail -20
  return 1
}

# ── Core ────────────────────────────────────────────────────

read_attr() {
  chip-tool any read-by-id "$CLUSTER_ID" "$1" "$NODE_ID" "$ENDPOINT_ID" \
    --commissioner-name "$COMMISSIONER_NAME" 2>&1 | _filter_read
}

send_cmd() {
  local cmd_id="$1"
  local fields="${2:-\{\}}"
  chip-tool any command-by-id "$CLUSTER_ID" "$cmd_id" "$fields" "$NODE_ID" "$ENDPOINT_ID" \
    --commissioner-name "$COMMISSIONER_NAME" 2>&1 | _filter_cmd
}

# ── Attribute Reads ─────────────────────────────────────────

learn_state() {
  local val
  val="$(read_attr 0x0000)"
  local rc=$?
  case "$val" in
    0) printf '%s (IDLE)\n' "$val" ;;
    1) printf '%s (IN_PROGRESS)\n' "$val" ;;
    2) printf '%s (READY)\n' "$val" ;;
    3) printf '%s (FAILED)\n' "$val" ;;
    *) printf '%s\n' "$val" ;;
  esac
  return $rc
}

learned_payload()  { read_attr 0x0001; }

# ── Commands ────────────────────────────────────────────────

learn() {
  local timeout_ms="${1:-15000}"
  send_cmd 0x00 "{\"0:U32\":${timeout_ms}}"
}

cancel_learn() { send_cmd 0x01; }

send_raw() {
  if [[ -z "${1:-}" || -z "${2:-}" ]]; then
    echo "usage: send_raw <signal_id> <ticks_hex> [carrier_hz] [repeat]"
    echo "example: send_raw 100 41248A117C02 38000 1"
    return 1
  fi
  local sig="$1" ticks_hex="$2"
  local carrier="${3:-38000}" repeat="${4:-1}"

  send_cmd 0x0B "{\"0:U32\":${sig}, \"1:U32\":${carrier}, \"2:U8\":${repeat}, \"3:BYTES\":\"${ticks_hex}\"}"
}

sync_buffer()    { dump_nvs; }  # alias for dump_nvs (no RAM buffer)
factory_reset()  { send_cmd 0x0D; }
dump_nvs()       { send_cmd 0x0E; echo "Reading snapshot..."; buffer_snapshot; }
buffer_snapshot() { read_attr 0x0007; }

dump_log() {
  if [[ -z "$WEB_API_KEY" ]]; then
    printf '\033[33mAPI 키가 설정되지 않음. set_key <key> 로 설정하세요.\033[0m\n'
    return 1
  fi
  printf 'Fetching activity log from %s ...\n' "$DEVICE_IP"
  curl -s -H "X-Api-Key: $WEB_API_KEY" "http://${DEVICE_IP}/api/logs" | python3 -m json.tool 2>/dev/null || \
    curl -s -H "X-Api-Key: $WEB_API_KEY" "http://${DEVICE_IP}/api/logs"
}

set_key() {
  if [[ -z "${1:-}" ]]; then
    echo "usage: set_key <api_key>"
    return 1
  fi
  WEB_API_KEY="$1"
  printf 'API key set: %s\n' "$WEB_API_KEY"
}

set_ip() {
  if [[ -z "${1:-}" ]]; then
    echo "usage: set_ip <device_ip>"
    return 1
  fi
  DEVICE_IP="$1"
  printf 'Device IP set: %s\n' "$DEVICE_IP"
}

commission() {
  local timeout="${1:-300}"
  send_cmd 0x08 "{\"0:U16\":${timeout}}"
}

pair() {
  local pin="${1:-20202021}"
  printf 'Pairing node %s (pin: %s) ...\n' "$NODE_ID" "$pin"
  chip-tool pairing onnetwork "$NODE_ID" "$pin" \
    --commissioner-name "$COMMISSIONER_NAME" 2>&1 | _filter_pair
}

pair_wifi() {
  local ssid="${1:-}"
  local password="${2:-}"
  local pin="${3:-20202021}"
  local disc="${4:-3840}"
  if [[ -z "$ssid" || -z "$password" ]]; then
    echo "usage: pair_wifi <ssid> <password> [pin] [discriminator]"
    echo "  BLE로 WiFi 크레덴셜을 전달하여 커미셔닝합니다."
    echo "  예: pair_wifi MyWiFi MyPassword123"
    return 1
  fi
  printf 'BLE WiFi pairing node %s (ssid: %s, pin: %s, disc: %s) ...\n' "$NODE_ID" "$ssid" "$pin" "$disc"
  chip-tool pairing ble-wifi "$NODE_ID" "$ssid" "$password" "$pin" "$disc" \
    --commissioner-name "$COMMISSIONER_NAME" 2>&1 | _filter_pair
}

pair_auto() {
  local pin="${1:-20202021}"
  local disc="${2:-3840}"
  local wifi_env="$PROJECT_DIR/.wifi_credentials"

  # .wifi_credentials에서 크레덴셜 읽기
  local ssid="" password=""
  if [[ -f "$wifi_env" ]]; then
    ssid="$(grep '^WIFI_SSID=' "$wifi_env" | cut -d= -f2-)"
    password="$(grep '^WIFI_PASSWORD=' "$wifi_env" | cut -d= -f2-)"
    if [[ -n "$ssid" && -n "$password" ]]; then
      echo "저장된 크레덴셜 사용: $ssid"
    fi
  fi

  # 크레덴셜이 없으면 입력받고 저장
  if [[ -z "$ssid" || -z "$password" ]]; then
    read -r -p "WiFi SSID: " ssid
    if [[ -z "$ssid" ]]; then
      echo "SSID가 비어있습니다."
      return 1
    fi
    read -r -s -p "WiFi 비밀번호: " password
    echo ""
    if [[ -z "$password" ]]; then
      echo "비밀번호가 비어있습니다."
      return 1
    fi
    read -r -p "이 크레덴셜을 저장할까요? (y/N): " save_yn
    if [[ "$save_yn" =~ ^[yY] ]]; then
      printf 'WIFI_SSID=%s\nWIFI_PASSWORD=%s\n' "$ssid" "$password" > "$wifi_env"
      chmod 600 "$wifi_env"
      echo "저장 완료: $wifi_env (다음부터 자동 사용)"
    fi
  fi

  printf 'Auto WiFi pairing: ssid=%s node=%s pin=%s disc=%s\n' "$ssid" "$NODE_ID" "$pin" "$disc"
  chip-tool pairing ble-wifi "$NODE_ID" "$ssid" "$password" "$pin" "$disc" \
    --commissioner-name "$COMMISSIONER_NAME" 2>&1 | _filter_pair
}

unpair() {
  printf 'Unpairing node %s ...\n' "$NODE_ID"
  chip-tool pairing unpair "$NODE_ID" \
    --commissioner-name "$COMMISSIONER_NAME" 2>&1 | _filter_pair
}

smoke() {
  echo "=== Learn State ===" && learn_state
}

api_help() {
  cat <<'HELP'
── 속성 읽기 ──────────────────────────────────────────────
  learn_state                 학습 상태 (0=IDLE 1=IN_PROGRESS 2=READY 3=FAILED)
  learned_payload             현재 학습 세션 상세 (JSON)
  buffer_snapshot             버퍼 스냅샷 (SyncBuffer 후 조회)

── IR 학습 ────────────────────────────────────────────────
  learn [timeout_ms]          학습 시작 (기본 15000ms)
  cancel_learn                학습 취소

── IR 송신 (v3.2) ─────────────────────────────────────────
  send_raw <signal_id> <ticks_hex> [carrier_hz] [repeat]
                              raw 신호 송신 + 버퍼 저장
  예: send_raw 100 41248A117C02 38000 1

── NVS/시스템 ─────────────────────────────────────────────
  sync_buffer                 dump_nvs 별칭 (RAM 버퍼 없음)
  dump_nvs                    NVS 전체 신호 조회 → 스냅샷
  dump_log                    활동 로그 조회 (웹 API)
  buffer_snapshot             BufferSnapshot 속성 읽기
  factory_reset               Matter 팩토리 리셋 (전체 초기화)
  set_key <key>               웹 API 키 설정
  set_ip <ip>                 디바이스 IP 설정 (기본: 192.168.75.202)

── 커미셔닝 ───────────────────────────────────────────────
  pair [setup_pin]            온네트워크 커미셔닝 (WiFi 연결 후)
  pair_wifi <ssid> <pw>       BLE WiFi 커미셔닝 (WiFi 크레덴셜 전달)
  pair_auto                   현재 Mac WiFi로 자동 BLE 커미셔닝
  unpair                      디바이스 커미셔닝 해제
  commission [timeout_sec]    커미셔닝 창 열기
  smoke                       학습 상태 조회

── REPL ───────────────────────────────────────────────────
  /help                       이 도움말
  /node                       현재 node/endpoint 확인
  /exit                       종료
HELP
}
