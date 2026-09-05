#!/usr/bin/env bash
#
# Bash equivalent of generate_dev_ble_mqtt_secrets_header.ps1, for benches
# where the PowerShell cask is installed but `pwsh` itself is unavailable
# (observed on an Intel Mac). Both scripts must keep generating exactly the
# same ignored header, include/interbridge_dev_ble_mqtt_secrets.h - see
# docs/dev-ble-mqtt.md > "Local credentials and build". Uses only tools
# that ship standard on macOS/Bash (bash, cat, printf, sed, awk, grep, tr,
# git, mktemp, dirname, basename) - no installable dependency, and
# deliberately no `realpath` (not guaranteed present on every macOS
# install); physical path resolution below uses only `cd`/`pwd -P`.
#
# Deliberately never touches, generates, or accepts a Wi-Fi SSID or
# password: this environment gets Wi-Fi exclusively from the official BLE
# Unified Provisioning session and whatever the ESP-IDF Wi-Fi driver then
# persists in NVS. Never accepts certificate, private key, PoP, SSID, or
# password material as a command-line argument - all of it is read only
# from fixed filenames inside an explicitly selected directory outside
# this repository.
set -euo pipefail

error() {
  echo "Error: $*" >&2
  exit 1
}

usage() {
  cat >&2 <<'EOF'
Usage: generate_dev_ble_mqtt_secrets_header.sh <credentials_dir> <device_id> [destination]

  credentials_dir  Directory outside this repository containing exactly:
                    endpoint.txt, AmazonRootCA1.pem, device-certificate.pem.crt,
                    private.pem.key, pop.txt
  device_id        Must match ^ib-[0-9a-f]{32}$
  destination      Optional; must resolve to the ignored header path
                    include/interbridge_dev_ble_mqtt_secrets.h (the default)
EOF
  exit 1
}

if [ "$#" -lt 2 ] || [ "$#" -gt 3 ]; then
  usage
fi

credentials_dir=$1
device_id=$2
destination=${3:-include/interbridge_dev_ble_mqtt_secrets.h}

if ! [[ "$device_id" =~ ^ib-[0-9a-f]{32}$ ]]; then
  error "device_id must match ^ib-[0-9a-f]{32}\$."
fi

# Resolves $1 to its physical (symlink-free) absolute path, mirroring
# PowerShell's Resolve-Path, without depending on the external `realpath`
# utility. Requires the target to already exist, exactly like
# Resolve-Path -LiteralPath.
physical_path() {
  local target=$1
  if [ -d "$target" ]; then
    (cd "$target" && pwd -P)
  elif [ -e "$target" ]; then
    local dir base
    dir=$(dirname "$target")
    base=$(basename "$target")
    printf '%s/%s\n' "$(cd "$dir" && pwd -P)" "$base"
  else
    error "Path does not exist: $target"
  fi
}

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)
repo_root=$(cd "$script_dir/.." && pwd -P)

# The destination file itself is not expected to exist yet - only resolve
# its containing directory physically (mirrors [IO.Path]::GetFullPath,
# which normalizes lexically rather than requiring existence).
destination_dir=$(dirname "$destination")
destination_base=$(basename "$destination")
if [ -d "$repo_root/$destination_dir" ]; then
  destination_path="$(cd "$repo_root/$destination_dir" && pwd -P)/$destination_base"
else
  error "Destination directory does not exist: $destination_dir"
fi
expected_destination="$repo_root/include/interbridge_dev_ble_mqtt_secrets.h"
if [ "$destination_path" != "$expected_destination" ]; then
  error "Destination must be the ignored DEV BLE+MQTT header path."
fi

if ! git -C "$repo_root" check-ignore --quiet -- "$destination_path"; then
  error "Destination is not ignored by Git; refusing to write."
fi

credential_path=$(physical_path "$credentials_dir")

# Case-insensitive prefix compare, matching the .ps1's
# StringComparison.OrdinalIgnoreCase - relevant on the default
# case-insensitive-preserving macOS filesystem.
lower() { printf '%s' "$1" | tr '[:upper:]' '[:lower:]'; }
case "$(lower "$credential_path")" in
  "$(lower "$repo_root")"/*)
    error "Credentials directory must be outside the repository."
    ;;
esac

# Exactly these five external files, nothing else - see docs/dev-ble-
# mqtt.md's runbook for how pop.txt is expected to be produced
# (openssl rand -hex 32).
required_files="endpoint.txt AmazonRootCA1.pem device-certificate.pem.crt private.pem.key pop.txt"
for name in $required_files; do
  if [ ! -f "$credential_path/$name" ]; then
    error "A required credential file is missing: $name"
  fi
done

# Reads the whole file, preserving any trailing newline exactly (plain
# command substitution would otherwise strip it) - matches PowerShell's
# Get-Content -Raw.
read_raw() {
  local content
  content=$(cat "$1"; printf 'X')
  printf '%s' "${content%X}"
}

trim() {
  printf '%s' "$1" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//'
}

is_blank() {
  local stripped
  stripped=$(printf '%s' "$1" | tr -d '[:space:]')
  [ -z "$stripped" ]
}

# Must match include/interbridge_dev_ble_mqtt_secrets.example.h exactly -
# never let a real generated header silently ship with the example
# placeholder still in place.
pop_placeholder="REPLACE_WITH_LOCAL_DEV_POP"

root_ca=""
certificate=""
private_key=""
pop=""
tmp_file=""
cleanup() {
  # Best-effort defense in depth, same spirit as the .ps1's finally block
  # nulling these out - bash has no secure-memory-zeroing equivalent, but
  # this at least drops the references as soon as this script exits, on
  # every exit path (success, error, or interrupt). Also removes the
  # staging temp file on any exit before a successful mv (e.g. one of the
  # macro checks below failing) so a partial/rejected header never lingers
  # outside the destination it was validated for.
  unset root_ca certificate private_key pop root_ca_escaped certificate_escaped private_key_escaped pop_escaped device_id_escaped 2>/dev/null || true
  [ -n "$tmp_file" ] && rm -f "$tmp_file"
  true
}
trap cleanup EXIT

endpoint=$(trim "$(read_raw "$credential_path/endpoint.txt")")
if ! [[ "$endpoint" =~ ^[a-z0-9][a-z0-9-]{8,}-ats\.iot\.[a-z0-9-]+\.amazonaws\.com$ ]]; then
  error "Endpoint format is invalid."
fi

root_ca=$(read_raw "$credential_path/AmazonRootCA1.pem")
is_blank "$root_ca" && error "Root CA file is empty."

certificate=$(read_raw "$credential_path/device-certificate.pem.crt")
is_blank "$certificate" && error "Device certificate file is empty."

private_key=$(read_raw "$credential_path/private.pem.key")
is_blank "$private_key" && error "Private key file is empty."

pop=$(trim "$(read_raw "$credential_path/pop.txt")")
if [ "$pop" = "$pop_placeholder" ]; then
  error "PoP must not be the example header's placeholder value."
fi
# Exactly 64 lowercase hex characters - the format `openssl rand -hex 32`
# produces. Rejects anything else (wrong length, uppercase, non-hex,
# accidental whitespace/newlines beyond the single trim above) rather
# than trying to normalize it.
if ! [[ "$pop" =~ ^[0-9a-f]{64}$ ]]; then
  error "PoP must be exactly 64 lowercase hexadecimal characters (e.g. from 'openssl rand -hex 32')."
fi

# Escapes a value for a C++ string literal: backslashes first, then
# quotes, then every line ending (CRLF/LF/CR) becomes a literal two-
# character \n - same order and result as the .ps1's ConvertTo-CppString,
# implemented line-by-line so multi-line PEM content is handled without
# any external dependency. Reads from stdin so it works whether the
# original file used CRLF or LF line endings.
cpp_escape() {
  awk '
    NR > 1 { printf "\\n" }
    {
      sub(/\r$/, "")
      gsub(/\\/, "\\\\")
      gsub(/"/, "\\\"")
      printf "%s", $0
    }
  '
}

device_id_escaped=$(printf '%s' "$device_id" | cpp_escape)
endpoint_escaped=$(printf '%s' "$endpoint" | cpp_escape)
root_ca_escaped=$(printf '%s' "$root_ca" | cpp_escape)
certificate_escaped=$(printf '%s' "$certificate" | cpp_escape)
private_key_escaped=$(printf '%s' "$private_key" | cpp_escape)
pop_escaped=$(printf '%s' "$pop" | cpp_escape)

tmp_file=$(mktemp)
{
  printf '#pragma once\n'
  printf '\n'
  printf '// Generated local DEV configuration for esp32-c3-dev-ble-mqtt. This file\n'
  printf '// must remain ignored. Deliberately has no Wi-Fi SSID/password - Wi-Fi\n'
  printf '// comes exclusively from BLE Unified Provisioning/NVS.\n'
  printf '#define INTERBRIDGE_DEV_AWS_ENDPOINT "%s"\n' "$endpoint_escaped"
  printf '#define INTERBRIDGE_DEV_DEVICE_ID "%s"\n' "$device_id_escaped"
  printf '#define INTERBRIDGE_DEV_ROOT_CA_PEM "%s"\n' "$root_ca_escaped"
  printf '#define INTERBRIDGE_DEV_CERTIFICATE_PEM "%s"\n' "$certificate_escaped"
  printf '#define INTERBRIDGE_DEV_PRIVATE_KEY_PEM "%s"\n' "$private_key_escaped"
  printf '#define INTERBRIDGE_DEV_BLE_POP "%s"\n' "$pop_escaped"
} > "$tmp_file"

# Every macro this header is supposed to define must appear exactly once,
# and neither Wi-Fi macro must ever appear - guards against a future edit
# to this script accidentally duplicating/dropping one, or reintroducing
# a static Wi-Fi credential this environment must never have.
for macro in INTERBRIDGE_DEV_AWS_ENDPOINT INTERBRIDGE_DEV_DEVICE_ID INTERBRIDGE_DEV_ROOT_CA_PEM \
             INTERBRIDGE_DEV_CERTIFICATE_PEM INTERBRIDGE_DEV_PRIVATE_KEY_PEM INTERBRIDGE_DEV_BLE_POP; do
  match_count=$(grep -c "^#define $macro " "$tmp_file")
  if [ "$match_count" -ne 1 ]; then
    error "Internal error: $macro must be defined exactly once (found $match_count) - refusing to write."
  fi
done
for macro in INTERBRIDGE_DEV_WIFI_SSID INTERBRIDGE_DEV_WIFI_PASSWORD; do
  if grep -q "^#define $macro " "$tmp_file"; then
    error "Internal error: $macro must never appear in this header - refusing to write."
  fi
done

mv "$tmp_file" "$destination_path"

echo "DEV BLE+MQTT secrets header generated safely at the ignored destination."
echo "device_id: $device_id (certificate/private key/PoP values, sizes, and hashes are never shown)."
