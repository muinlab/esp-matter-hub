#!/bin/bash

set -uo pipefail

PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"
ENV_FILE="$PROJECT_DIR/.hub_api_test_env.sh"
NODE_ID="${1:-1}"
ENDPOINT_ID="${2:-10}"

if [[ ! -f "$ENV_FILE" ]]; then
  echo "Missing env file: $ENV_FILE"
  exit 1
fi

export NODE_ID ENDPOINT_ID
source "$ENV_FILE"

print_banner() {
  echo ""
  echo "hub_api_test (chip-tool)"
  echo "Node: $NODE_ID  Endpoint: $ENDPOINT_ID  Cluster: $CLUSTER_ID"
  echo "Type /help for commands, /exit to quit."
  echo ""
}

run_cmd() {
  local input="$1"
  input="${input#${input%%[![:space:]]*}}"
  input="${input%${input##*[![:space:]]}}"
  if [[ -z "$input" ]]; then
    return 0
  fi

  case "$input" in
    /help|help)        api_help ;;
    /node)             echo "node=$NODE_ID  endpoint=$ENDPOINT_ID" ;;
    /exit|exit|quit)   return 99 ;;
    /learn_state)      learn_state ;;
    /learned_payload)  learned_payload ;;
    /buffer_snapshot)  buffer_snapshot ;;
    /sync_buffer)      sync_buffer ;;
    /factory_reset)    factory_reset ;;
    /smoke)            smoke ;;
    /pair)             pair ;;
    /unpair)           unpair ;;
    *)
      if [[ "$input" == /* ]]; then
        local raw="${input#/}"
        eval "$raw"
      else
        eval "$input"
      fi
      ;;
  esac
}

print_banner
while true; do
  read -r -e -p "hub_api_test> " line || break
  run_cmd "$line"
  rc=$?
  if [[ $rc -eq 99 ]]; then
    break
  fi
done
