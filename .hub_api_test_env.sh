#!/bin/bash

export PATH="$HOME/esp/esp-matter/connectedhomeip/connectedhomeip/.environment/gn_out:$PATH"

CLUSTER_ID="0x1337FC01"
COMMISSIONER_NAME="${COMMISSIONER_NAME:-beta}"
NODE_ID="${1:-1}"
ENDPOINT_ID="${2:-10}"

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

  printf 'Unknown result\n'
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
signals()          { read_attr 0x0002; }
cache_status()     { read_attr 0x0006; }
signal_payload()   { read_attr 0x0005; }

# ── Commands ────────────────────────────────────────────────

learn() {
  local timeout_ms="${1:-15000}"
  send_cmd 0x00 "{\"0:U32\":${timeout_ms}}"
}

cancel_learn() { send_cmd 0x01; }

save() {
  local name="${1:-}"
  local device_type="${2:-light}"
  if [[ -z "$name" ]]; then
    echo "usage: save <name> [device_type]"
    return 1
  fi
  send_cmd 0x02 "{\"0:STR\":\"${name}\", \"1:STR\":\"${device_type}\"}"
}

delete_signal() {
  if [[ -z "${1:-}" ]]; then
    echo "usage: delete_signal <signal_id>"
    return 1
  fi
  send_cmd 0x03 "{\"0:U32\":$1}"
}

get_signal_payload() {
  if [[ -z "${1:-}" ]]; then
    echo "usage: get_signal_payload <signal_id>"
    return 1
  fi
  send_cmd 0x0A "{\"0:U32\":$1}"
  echo "Reading payload..."
  signal_payload
}

send_raw() {
  if [[ -z "${1:-}" || -z "${2:-}" || -z "${3:-}" ]]; then
    echo "usage: send_raw <device_id> <signal_id> <is_level:0|1> <high_low:0|1> <carrier_hz> <repeat> <ticks_hex>"
    echo "example: send_raw 1 100 0 1 38000 1 41248A117C02"
    return 1
  fi
  local dev="$1" sig="$2" is_level="$3" high_low="$4"
  local carrier="${5:-38000}" repeat="${6:-1}" ticks_hex="${7:-}"

  local is_level_str="false"
  local high_low_str="false"
  [[ "$is_level" == "1" ]] && is_level_str="true"
  [[ "$high_low" == "1" ]] && high_low_str="true"

  if [[ -z "$ticks_hex" ]]; then
    echo "error: ticks_hex is required"
    return 1
  fi

  send_cmd 0x0B "{\"0:U32\":${dev}, \"1:U32\":${sig}, \"2:BOOL\":${is_level_str}, \"3:BOOL\":${high_low_str}, \"4:U32\":${carrier}, \"5:U8\":${repeat}, \"6:BYTES\":\"${ticks_hex}\"}"
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
  echo "" && echo "=== Signals (NVS backup) ===" && signals
  echo "" && echo "=== Cache Status ===" && cache_status
}

api_help() {
  cat <<'HELP'
── 속성 읽기 ──────────────────────────────────────────────
  learn_state                 학습 상태 (0=IDLE 1=IN_PROGRESS 2=READY 3=FAILED)
  learned_payload             현재 학습 세션 상세 (JSON)
  signals                     NVS 백업 신호 목록
  cache_status                캐시 상태 bitmap (bit=슬롯)

── IR 학습 ────────────────────────────────────────────────
  learn [timeout_ms]          학습 시작 (기본 15000ms)
  cancel_learn                학습 취소
  save <name> [type]          학습한 신호 NVS 백업 저장
  delete_signal <signal_id>   NVS 백업 신호 삭제
  get_signal_payload <id>     NVS 백업에서 raw payload 조회

── IR 송신 (v3.1) ─────────────────────────────────────────
  send_raw <dev_id> <sig_id> <is_level:0|1> <high_low:0|1> <carrier> <repeat> <ticks_hex>
                              raw 신호 송신 + 캐시 + 자동등록 + 슬롯 바인딩
  예: send_raw 1 100 0 1 38000 1 41248A117C02

── 기타 ───────────────────────────────────────────────────
  pair [setup_pin]            온네트워크 커미셔닝 (WiFi 연결 후)
  pair_wifi <ssid> <pw>       BLE WiFi 커미셔닝 (WiFi 크레덴셜 전달)
  pair_auto                   현재 Mac WiFi로 자동 BLE 커미셔닝
  unpair                      디바이스 커미셔닝 해제
  commission [timeout_sec]    커미셔닝 창 열기
  smoke                       전체 상태 일괄 조회

── REPL ───────────────────────────────────────────────────
  /help                       이 도움말
  /node                       현재 node/endpoint 확인
  /exit                       종료
HELP
}
