#!/usr/bin/env bash
#
# push_update.sh — Upload app / RCP / web binaries to a running s20-otbr device
# and trigger a reboot via the local-update REST API.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)/build"

# Defaults
APP_BIN="${BUILD_DIR}/s20_otbr.bin"
RCP_BIN="${BUILD_DIR}/esp-idf/esp_rcp_update/spiffs_image/ot_rcp_0/rcp_image"
WEB_BIN="${BUILD_DIR}/web_storage.bin"
SKIP_APP=false
SKIP_RCP=false
SKIP_WEB=false
NO_REBOOT=false
DEVICE_IP=""

usage() {
    cat <<'EOF'
push_update.sh — Upload app / RCP / web binaries to a running s20-otbr device
and trigger a reboot via the local-update REST API.

Usage:
  ./push_update.sh <device-ip> [options]

Options:
  --app <path>   App firmware binary  (default: build/s20_otbr.bin)
  --rcp <path>   RCP firmware binary  (default: build/esp-idf/esp_rcp_update/spiffs_image/ot_rcp_0/rcp_image)
  --web <path>   Web storage binary   (default: build/web_storage.bin)
  --skip-app     Skip app firmware upload
  --skip-rcp     Skip RCP firmware upload
  --skip-web     Skip web storage upload
  --no-reboot    Upload files but do not trigger device reboot
  -h, --help     Show this help message
EOF
    exit 0
}

die() {
    echo "ERROR: $*" >&2
    exit 1
}

# ── Argument parsing ──────────────────────────────────────────────────────────

if [[ $# -eq 0 ]]; then
    usage
fi

POSITIONAL=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help)   usage ;;
        --app)       APP_BIN="$2";  shift 2 ;;
        --rcp)       RCP_BIN="$2";  shift 2 ;;
        --web)       WEB_BIN="$2";  shift 2 ;;
        --skip-app)  SKIP_APP=true; shift ;;
        --skip-rcp)  SKIP_RCP=true; shift ;;
        --skip-web)  SKIP_WEB=true; shift ;;
        --no-reboot) NO_REBOOT=true; shift ;;
        -*)          die "Unknown option: $1" ;;
        *)           POSITIONAL+=("$1"); shift ;;
    esac
done

[[ ${#POSITIONAL[@]} -eq 0 ]] && die "Device IP address is required."
[[ ${#POSITIONAL[@]} -gt 1 ]] && die "Too many positional arguments."
DEVICE_IP="${POSITIONAL[0]}"

# Basic IP / hostname sanity check (no shell execution, pure regex)
if ! [[ "$DEVICE_IP" =~ ^[a-zA-Z0-9._\-]+$ ]]; then
    die "Invalid device address: '${DEVICE_IP}'"
fi

# ── Validate binaries before uploading ───────────────────────────────────────

check_bin() {
    local label="$1" path="$2"
    [[ -f "$path" ]] || die "${label} binary not found: ${path}"
    [[ -r "$path" ]] || die "${label} binary is not readable: ${path}"
}

$SKIP_APP || check_bin "App" "$APP_BIN"
$SKIP_RCP || check_bin "RCP" "$RCP_BIN"
$SKIP_WEB || check_bin "Web" "$WEB_BIN"

if $SKIP_APP && $SKIP_RCP && $SKIP_WEB; then
    die "All uploads skipped — nothing to do."
fi

# ── Helper: POST a binary and check the JSON response ────────────────────────

upload_bin() {
    local label="$1" url="$2" path="$3"

    echo "  Uploading ${label}: ${path} → ${url}"
    local size
    size=$(wc -c < "$path")
    echo "  Size: ${size} bytes"

    local http_code response body
    response=$(curl --progress-bar --show-error --write-out "\n%{http_code}" \
        --max-time 300 \
        --header "Content-Type: application/octet-stream" \
        --data-binary "@${path}" \
        --request POST \
        "${url}") || die "${label} upload failed (curl error)"
    http_code=$(echo "$response" | tail -n1)
    body=$(echo "$response" | head -n -1)

    # Expect {"error":0,...}
    local err_code
    err_code=$(echo "$body" | grep -oP '"error"\s*:\s*\K[0-9]+' || echo "")
    if [[ "$err_code" != "0" ]]; then
        local msg
        msg=$(echo "$body" | grep -oP '"message"\s*:\s*"\K[^"]+' || echo "$body")
        die "${label} upload failed (HTTP ${http_code}): ${msg}"
    fi

    echo "  ${label}: OK"
}

# ── Main ──────────────────────────────────────────────────────────────────────

BASE_URL="http://${DEVICE_IP}"

echo "==> s20-otbr local update → ${DEVICE_IP}"
echo ""

$SKIP_APP || upload_bin "App"  "${BASE_URL}/ota/upload/app" "$APP_BIN"
$SKIP_RCP || upload_bin "RCP"  "${BASE_URL}/ota/upload/rcp" "$RCP_BIN"
$SKIP_WEB || upload_bin "Web"  "${BASE_URL}/ota/upload/web" "$WEB_BIN"

echo ""

if $NO_REBOOT; then
    echo "==> Uploads complete. Reboot skipped (--no-reboot)."
else
    echo "==> Triggering device reboot..."
    # The device may close the connection immediately on reboot — ignore curl errors here.
    curl --silent --max-time 10 --request POST "${BASE_URL}/ota/restart" || true
    echo
    echo "==> Done. The device is rebooting; the web UI will be unavailable briefly."
fi
