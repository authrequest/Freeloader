#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# plex-tailscale-setup.sh -- run on the LINUX host that runs Plex Media Server.
#
# Makes a local Plex server reachable by remote users over a Tailscale /
# Headscale mesh VPN: no router port-forwarding, no Plex Relay, no patching.
#
#   1. install/join Tailscale (Tailscale's control plane, or your Headscale)
#   2. ask a few security questions (skippable with flags or --yes)
#   3. edit Preferences.xml safely (Plex stopped, backed up, ownership restored)
#   4. optionally lock the firewall to the tailnet
#   5. restart Plex and run a health check (also available as `--healthcheck`)
#
# Target: Debian/Ubuntu-family with systemd. Requires: tailscale (auto-installed),
# python3, python3-defusedxml, curl. Run as root (except --healthcheck).
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib/common.sh
source "${SCRIPT_DIR}/lib/common.sh" || { echo "missing ${SCRIPT_DIR}/lib/common.sh" >&2; exit 1; }
enable_error_trap
readonly PREFS_PY="${SCRIPT_DIR}/lib/plex_prefs.py"

# ---- defaults --------------------------------------------------------------
readonly PREFS_DEFAULT='/var/lib/plexmediaserver/Library/Application Support/Plex Media Server/Preferences.xml'
PREFS="${PLEX_PREFS:-$PREFS_DEFAULT}"
SERVICE="plexmediaserver"
PLEX_PORT="32400"
URL_SCHEME="https"
SECURE=""                    # ask | required|preferred|disabled|keep
RELAY=""                     # ask | disable|keep
FIREWALL=""                  # ask | none|tailnet|lan
LOGIN_SERVER=""
AUTHKEY=""
TS_HOSTNAME=""
TS_IFACE="tailscale0"
readonly TAILNET_V4="100.64.0.0/10"
readonly TAILNET_V6="fd7a:115c:a1e0::/48"
SKIP_TAILSCALE=0
SKIP_PLEX=0
ASSUME_YES=0
HEALTHCHECK_ONLY=0
INTERACTIVE=0

usage() {
  cat <<EOF
Usage: sudo $0 [options]

Connectivity:
  --login-server URL   Use a self-hosted Headscale control server.
  --authkey KEY        Pre-auth/auth key (unattended join; never logged).
  --hostname NAME      Tailnet hostname for this node.
  --ts-iface NAME      Tailscale interface (default: $TS_IFACE).

Plex:
  --prefs PATH         Preferences.xml path (quote it -- it has spaces).
  --service NAME       systemd unit name (default: $SERVICE).
  --port N             Plex port (default: $PLEX_PORT).
  --url-scheme S       https|http for the published URL (default: https).

Security (prompted interactively unless set here or with --yes):
  --secure MODE        required|preferred|disabled|keep (default: preferred).
  --disable-relay      Set RelayEnabled=0 (recommended on a tailnet).
  --keep-relay         Leave Plex Relay untouched.
  --firewall MODE      none|tailnet|lan (default: none).

Control:
  --healthcheck        Run health checks only and exit (no changes, no root).
  --skip-tailscale     Do not touch Tailscale.
  --skip-plex          Do not touch Plex config.
  -y, --yes            Non-interactive: accept defaults.
  --dry-run            Print actions without changing anything.
  -h, --help           This help.
EOF
}

parse_args() {
  while [[ $# -gt 0 ]]; do
    case "$1" in
      --login-server) LOGIN_SERVER="$2"; shift 2;;
      --authkey)      AUTHKEY="$2"; shift 2;;
      --hostname)     TS_HOSTNAME="$2"; shift 2;;
      --ts-iface)     TS_IFACE="$2"; shift 2;;
      --prefs)        PREFS="$2"; shift 2;;
      --service)      SERVICE="$2"; shift 2;;
      --port)         PLEX_PORT="$2"; shift 2;;
      --url-scheme)   URL_SCHEME="$2"; shift 2;;
      --secure)       SECURE="$2"; shift 2;;
      --disable-relay) RELAY="disable"; shift;;
      --keep-relay)   RELAY="keep"; shift;;
      --firewall)     FIREWALL="$2"; shift 2;;
      --healthcheck)  HEALTHCHECK_ONLY=1; shift;;
      --skip-tailscale) SKIP_TAILSCALE=1; shift;;
      --skip-plex)    SKIP_PLEX=1; shift;;
      -y|--yes|--non-interactive) ASSUME_YES=1; shift;;
      --dry-run)      DRY_RUN=1; shift;;
      -h|--help)      usage; exit 0;;
      *) die "unknown option: $1 (see --help)";;
    esac
  done
  case "$URL_SCHEME" in http|https) ;; *) die "--url-scheme must be http or https";; esac
  is_port "$PLEX_PORT" || die "--port must be 1-65535, got: $PLEX_PORT"
  [[ $ASSUME_YES -eq 0 && -t 0 ]] && INTERACTIVE=1 || INTERACTIVE=0
}

# ---- security questionnaire ------------------------------------------------
resolve_security_options() {
  if [[ -z "$SECURE" ]]; then
    [[ $INTERACTIVE -eq 1 ]] \
      && SECURE="$(ask_choice 'Secure connections between clients and server:' preferred required preferred disabled keep)" \
      || SECURE="preferred"
  fi
  if [[ -z "$RELAY" ]]; then
    if [[ $INTERACTIVE -eq 1 ]]; then
      ask_yes_no 'Disable Plex Relay (recommended -- you reach the server via the tailnet)?' Y && RELAY=disable || RELAY=keep
    else RELAY=disable; fi
  fi
  if [[ -z "$FIREWALL" ]]; then
    [[ $INTERACTIVE -eq 1 ]] \
      && FIREWALL="$(ask_choice "Lock down Plex ${PLEX_PORT}/tcp? (tailnet=VPN only, lan=VPN+home LAN, none=leave)" none tailnet lan none)" \
      || FIREWALL="none"
  fi
  case "$SECURE"   in required|preferred|disabled|keep) ;; *) die "--secure must be required|preferred|disabled|keep";; esac
  case "$RELAY"    in disable|keep) ;;                     *) die "relay choice must be disable|keep";; esac
  case "$FIREWALL" in none|tailnet|lan) ;;                 *) die "--firewall must be none|tailnet|lan";; esac
  log "security: secureConnections=$SECURE, relay=$RELAY, firewall=$FIREWALL"
}

# ---- tailscale -------------------------------------------------------------
install_tailscale() {
  if have_cmd tailscale; then
    ok "tailscale already installed ($(tailscale version 2>/dev/null | head -n1))"
  else
    need_cmd curl
    log "installing Tailscale via official script"
    if [[ $DRY_RUN -eq 1 ]]; then echo "   + curl -fsSL https://tailscale.com/install.sh | sh"
    else curl -fsSL https://tailscale.com/install.sh | sh; fi
  fi
  run systemctl enable --now tailscaled
}

join_tailnet() {
  local args=(up --reset)
  [[ -n "$LOGIN_SERVER" ]] && args+=(--login-server "$LOGIN_SERVER")
  [[ -n "$AUTHKEY" ]]      && args+=(--authkey "$AUTHKEY")
  [[ -n "$TS_HOSTNAME" ]]  && args+=(--hostname "$TS_HOSTNAME")
  log "bringing up tailscale: tailscale $(redact_after --authkey "${args[@]}")"
  [[ -z "$AUTHKEY" ]] && warn "no --authkey: 'tailscale up' prints a login URL; open it to authenticate."
  # Do not route the auth key through run(): its dry-run echo would print the
  # secret. The redacted command was already logged above.
  [[ $DRY_RUN -eq 1 ]] && return 0
  tailscale "${args[@]}"
}

tailnet_ip_soft() { tailscale ip -4 2>/dev/null | head -n1 || true; }

# ---- plex ------------------------------------------------------------------
secure_value() {
  case "$1" in required) echo 0;; preferred) echo 1;; disabled) echo 2;; *) echo "";; esac
}

get_attr() { python3 "$PREFS_PY" get "$PREFS" "$1" 2>/dev/null || true; }

configure_plex() {
  local ts_ip="$1"
  [[ -f "$PREFS" ]] || die "Preferences.xml not found at: $PREFS (pass --prefs; quote the path)"
  [[ -f "$PREFS_PY" ]] || die "missing helper: $PREFS_PY"
  need_cmd python3

  local url="${URL_SCHEME}://${ts_ip}:${PLEX_PORT}"
  local owner mode sv relay_val
  owner="$(stat -c '%U:%G' "$PREFS")"
  mode="$(stat -c '%a' "$PREFS")"
  sv="$(secure_value "$SECURE")"
  [[ "$RELAY" == "disable" ]] && relay_val="0" || relay_val=""

  log "stopping $SERVICE (Plex rewrites Preferences.xml on exit; edit while stopped)"
  run systemctl stop "$SERVICE" || warn "could not stop $SERVICE; continuing"

  local bak; bak="${PREFS}.bak.$(date +%Y%m%d%H%M%S)"
  run cp -a "$PREFS" "$bak"
  ok "backup written: $bak"

  if [[ $DRY_RUN -eq 1 ]]; then
    log "[dry-run] merge customConnections += $url"
    log "[dry-run] merge LanNetworksBandwidth += $TAILNET_V4,$TAILNET_V6"
    [[ -n "$sv" ]] && log "[dry-run] set secureConnections = $sv ($SECURE)"
    [[ -n "$relay_val" ]] && log "[dry-run] set RelayEnabled = 0 (disable relay)"
  else
    python3 "$PREFS_PY" merge "$PREFS" \
      --custom-url "$url" --lan "${TAILNET_V4},${TAILNET_V6}" \
      --secure "$sv" --relay "$relay_val"
    ok "Preferences.xml updated"
  fi

  run chown "$owner" "$PREFS"
  run chmod "$mode" "$PREFS"
  log "starting $SERVICE"
  run systemctl start "$SERVICE"

  if [[ $DRY_RUN -eq 0 ]]; then
    log "waiting for Plex to answer locally..."
    local i
    for i in $(seq 1 20); do
      curl -fsS "http://127.0.0.1:${PLEX_PORT}/identity" >/dev/null 2>&1 && { ok "Plex is up locally"; return 0; }
      sleep 1
    done
    warn "Plex did not answer on :${PLEX_PORT} within 20s; check 'systemctl status $SERVICE'"
  fi
}

# ---- firewall (only ever touches ${PLEX_PORT}/tcp; SSH stays open) ----------
configure_firewall() {
  local mode="$1"
  [[ "$mode" == "none" ]] && { log "firewall: left unchanged"; return 0; }

  if have_cmd ufw && ufw status 2>/dev/null | grep -qi '^Status: active'; then
    log "firewall: ufw active -- restricting ${PLEX_PORT}/tcp"
    run ufw allow in on "$TS_IFACE" to any port "$PLEX_PORT" proto tcp || true
    if [[ "$mode" == "lan" ]]; then
      local n
      for n in 10.0.0.0/8 172.16.0.0/12 192.168.0.0/16; do
        run ufw allow from "$n" to any port "$PLEX_PORT" proto tcp || true
      done
    fi
    run ufw deny "$PLEX_PORT"/tcp || true
    ok "ufw: ${PLEX_PORT}/tcp limited to tailnet$([[ "$mode" == lan ]] && echo ' + private LAN')"
    return 0
  fi

  if have_cmd firewall-cmd && firewall-cmd --state 2>/dev/null | grep -qi running; then
    log "firewall: firewalld running -- restricting ${PLEX_PORT}/tcp"
    run firewall-cmd --permanent --zone=trusted --change-interface="$TS_IFACE" || true
    run firewall-cmd --permanent --remove-port="$PLEX_PORT"/tcp || true
    if [[ "$mode" == "lan" ]]; then
      local n
      for n in 10.0.0.0/8 172.16.0.0/12 192.168.0.0/16; do
        run firewall-cmd --permanent --add-rich-rule="rule family=ipv4 source address=$n port port=$PLEX_PORT protocol=tcp accept" || true
      done
    fi
    run firewall-cmd --reload || true
    ok "firewalld: $TS_IFACE trusted; ${PLEX_PORT}/tcp not exposed publicly"
    return 0
  fi

  warn "no ACTIVE managed firewall (ufw/firewalld) found; not touching firewall (avoiding lockout)."
  warn "Manual nftables equivalent (only filters ${PLEX_PORT}/tcp, safe for SSH):"
  cat >&2 <<EOF
    nft add table inet plexlock
    nft 'add chain inet plexlock input { type filter hook input priority -10 ; }'
    nft add rule inet plexlock input iifname "lo" accept
    nft add rule inet plexlock input iifname "$TS_IFACE" tcp dport ${PLEX_PORT} accept
$( [[ "$mode" == lan ]] && echo "    nft add rule inet plexlock input ip saddr { 10.0.0.0/8, 172.16.0.0/12, 192.168.0.0/16 } tcp dport ${PLEX_PORT} accept" )
    nft add rule inet plexlock input tcp dport ${PLEX_PORT} drop
EOF
}

# ---- health check ----------------------------------------------------------
HC_PASS=0; HC_WARN=0; HC_FAIL=0
hc() { # label status detail
  local label="$1" status="$2" detail="${3:-}" sym col
  case "$status" in
    PASS) sym="+"; col=$'\033[1;32m'; HC_PASS=$((HC_PASS + 1));;
    WARN) sym="!"; col=$'\033[1;33m'; HC_WARN=$((HC_WARN + 1));;
    FAIL) sym="x"; col=$'\033[1;31m'; HC_FAIL=$((HC_FAIL + 1));;
  esac
  printf '  %s[%s]%s %-26s %s\n' "$(_c "$col")" "$sym" "$(_c $'\033[0m')" "$label" "$detail"
}

healthcheck() {
  local ts_ip="${1:-}"
  HC_PASS=0; HC_WARN=0; HC_FAIL=0
  printf '\n%sHealth check%s\n' "$(_c $'\033[1m')" "$(_c $'\033[0m')"

  if have_cmd tailscale; then
    tailscale status >/dev/null 2>&1 && hc "Tailscale backend" PASS "running" \
      || hc "Tailscale backend" FAIL "down / logged out (run 'tailscale up')"
    local ip; ip="$(tailnet_ip_soft)"
    [[ -n "$ip" ]] && hc "Tailnet IPv4" PASS "$ip" || hc "Tailnet IPv4" FAIL "no address assigned"
    [[ -z "$ts_ip" || "$ts_ip" == "<"* ]] && ts_ip="$ip"
  else
    hc "Tailscale" FAIL "not installed"
  fi

  systemctl is-active --quiet "$SERVICE" 2>/dev/null \
    && hc "Plex service" PASS "$SERVICE active" \
    || hc "Plex service" WARN "$SERVICE not active (or no systemd)"

  curl -fsS --max-time 8 "http://127.0.0.1:${PLEX_PORT}/identity" >/dev/null 2>&1 \
    && hc "Plex local API" PASS "127.0.0.1:${PLEX_PORT}" \
    || hc "Plex local API" FAIL "no response on :${PLEX_PORT}"

  if [[ -n "$ts_ip" && "$ts_ip" != "<"* ]]; then
    curl -fsSk --max-time 8 "http://${ts_ip}:${PLEX_PORT}/identity" >/dev/null 2>&1 \
      && hc "Plex via tailnet IP" PASS "${ts_ip}:${PLEX_PORT}" \
      || hc "Plex via tailnet IP" WARN "unreachable at ${ts_ip}:${PLEX_PORT} (firewall/not joined?)"
  fi

  if [[ -f "$PREFS" ]] && have_cmd python3; then
    local cc lan rly
    cc="$(get_attr customConnections)"; lan="$(get_attr LanNetworksBandwidth)"; rly="$(get_attr RelayEnabled)"
    [[ "$cc" == *":${PLEX_PORT}"* ]] && hc "customConnections" PASS "$cc" || hc "customConnections" WARN "no tailnet URL (${cc:-empty})"
    [[ "$lan" == *"100.64.0.0/10"* ]] && hc "LAN networks" PASS "tailnet treated as LAN" || hc "LAN networks" WARN "tailnet range missing (${lan:-empty})"
    [[ "$rly" == "0" ]] && hc "Plex Relay" PASS "disabled" || hc "Plex Relay" WARN "enabled (RelayEnabled=${rly:-unset})"
  else
    hc "Preferences.xml" WARN "not readable at $PREFS"
  fi

  if have_cmd ufw && ufw status 2>/dev/null | grep -qi '^Status: active'; then
    ufw status 2>/dev/null | grep -q "$PLEX_PORT" \
      && hc "Firewall (ufw)" PASS "${PLEX_PORT}/tcp rules present" \
      || hc "Firewall (ufw)" WARN "${PLEX_PORT}/tcp open on all interfaces"
  elif have_cmd firewall-cmd && firewall-cmd --state 2>/dev/null | grep -qi running; then
    hc "Firewall (firewalld)" PASS "running"
  else
    hc "Firewall" WARN "no managed firewall active"
  fi

  printf '\n  %s%d passed%s, %s%d warnings%s, %s%d failed%s\n' \
    "$(_c $'\033[1;32m')" "$HC_PASS" "$(_c $'\033[0m')" \
    "$(_c $'\033[1;33m')" "$HC_WARN" "$(_c $'\033[0m')" \
    "$(_c $'\033[1;31m')" "$HC_FAIL" "$(_c $'\033[0m')"
  [[ $HC_FAIL -eq 0 ]]
}

# ---- main ------------------------------------------------------------------
main() {
  parse_args "$@"

  if [[ $HEALTHCHECK_ONLY -eq 1 ]]; then
    if healthcheck "$(tailnet_ip_soft)"; then exit 0; else exit 1; fi
  fi

  require_root
  resolve_security_options

  local ts_ip="<tailscale-ip>"
  if [[ $SKIP_TAILSCALE -eq 0 ]]; then
    install_tailscale
    join_tailnet
    ts_ip="$(tailnet_ip_soft)"
    [[ -n "$ts_ip" ]] || die "could not read tailscale IPv4 (authenticated? 'tailscale status')"
    ok "this node's tailnet IPv4: $ts_ip"
  else
    ts_ip="$(tailnet_ip_soft)"; ts_ip="${ts_ip:-<tailscale-ip>}"
    warn "--skip-tailscale: using existing tailnet IP $ts_ip"
  fi

  [[ $SKIP_PLEX -eq 0 ]] && configure_plex "$ts_ip" || warn "--skip-plex: not modifying Plex"
  configure_firewall "$FIREWALL"
  [[ $DRY_RUN -eq 0 ]] && healthcheck "$ts_ip" || true

  cat <<EOF

$(ok "Server setup complete.")

Published Plex connection : ${URL_SCHEME}://${ts_ip}:${PLEX_PORT}
Tailnet treated as LAN    : ${TAILNET_V4}, ${TAILNET_V6}
Security                  : secureConnections=${SECURE}, relay=${RELAY}, firewall=${FIREWALL}

For each remote user:
  1. Install Tailscale:  https://tailscale.com/download
$( [[ -n "$LOGIN_SERVER" ]] && echo "  2. Join your Headscale:  sudo tailscale up --login-server $LOGIN_SERVER --authkey <their-preauthkey>" \
                              || echo "  2. Sign in to the SAME tailnet, or invite them to it." )
  3. Open Plex, sign in; the server appears over the tailnet.
     Shared users still need a library share (Settings > Users & Sharing).

Re-run health checks any time:  sudo $0 --healthcheck
See README.md for ACLs and the 2026 Plex Pass caveat.
EOF
}

main "$@"
