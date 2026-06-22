#!/usr/bin/env bash
#
# Configure the Raspberry Pi as a WiFi hotspot at 192.168.4.1.
# Tested on Raspberry Pi OS Bookworm (NetworkManager / nmcli).
#
# Usage: sudo bash scripts/setup_hotspot.sh [SSID] [PASSWORD]
# Defaults: SSID="TableFootball", PASSWORD="football2026"
#
set -euo pipefail

SSID="${1:-TableFootball}"
PSK="${2:-football2026}"
CON_NAME="TableFootball-AP"
IFACE="wlan0"

if [[ $EUID -ne 0 ]]; then
    echo "This script must be run as root (sudo)." >&2
    exit 1
fi

if ! command -v nmcli >/dev/null 2>&1; then
    echo "nmcli not found. Install NetworkManager (Bookworm has it by default)." >&2
    exit 1
fi

if (( ${#PSK} < 8 )); then
    echo "Password must be at least 8 characters (WPA2 requirement)." >&2
    exit 1
fi

echo "Creating hotspot '$SSID' on $IFACE ..."

# Drop any old AP profile with the same name so this script is idempotent.
nmcli con delete "$CON_NAME" >/dev/null 2>&1 || true

nmcli con add type wifi ifname "$IFACE" con-name "$CON_NAME" \
    autoconnect yes ssid "$SSID"

nmcli con modify "$CON_NAME" \
    802-11-wireless.mode ap \
    802-11-wireless.band bg \
    802-11-wireless-security.key-mgmt wpa-psk \
    802-11-wireless-security.proto rsn \
    802-11-wireless-security.pairwise ccmp \
    802-11-wireless-security.group ccmp \
    802-11-wireless-security.psk "$PSK" \
    ipv4.method shared \
    ipv4.address 192.168.4.1/24 \
    ipv6.method ignore

nmcli con up "$CON_NAME"

echo
echo "Hotspot is up."
echo "  SSID:     $SSID"
echo "  Password: $PSK"
echo "  IP:       192.168.4.1"
echo "  DHCP:     192.168.4.10 – 192.168.4.100 (via ipv4.method=shared)"
echo
echo "Run \`nmcli con show $CON_NAME\` to inspect, or \`nmcli con up $CON_NAME\` to re-enable later."
