#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# headscale-server-setup.sh -- OPTIONAL self-hosted coordination server.
#
# Use instead of Tailscale's control plane when you want no account limits and
# full control over who may join. Run on a PUBLIC Debian 12+/Ubuntu 22.04+ VPS
# with a DNS name pointing at it. It:
#   1. installs the official Headscale .deb (latest release, or --version)
#   2. points server_url at https://<domain> and enables built-in Let's Encrypt
#      TLS (unless --no-tls, for running behind your own reverse proxy)
#   3. starts the systemd service
#   4. creates a user and mints a reusable pre-auth key
#
# The Plex host and every client then join with:
#   sudo tailscale up --login-server https://<domain> --authkey <preauthkey>
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib/common.sh
source "${SCRIPT_DIR}/lib/common.sh" || { echo "missing ${SCRIPT_DIR}/lib/common.sh" >&2; exit 1; }
enable_error_trap

readonly CFG="/etc/headscale/config.yaml"
DOMAIN=""
USER_NAME="plex"
VERSION=""                       # auto-detect latest if empty
EXPIRY="720h"                    # preauth key lifetime (30 days)
USE_TLS=1
readonly LISTEN_PLAIN="127.0.0.1:8080"
PREAUTH_KEY=""

usage() {
  cat <<EOF
Usage: sudo $0 --domain hs.example.com [options]

  --domain NAME     Public DNS name for this Headscale server (required).
  --user NAME       Headscale user to create (default: $USER_NAME).
  --version VER     Headscale version (default: latest GitHub release).
  --expiration DUR  Pre-auth key lifetime, Go duration (default: $EXPIRY).
  --no-tls          Listen on $LISTEN_PLAIN for a reverse proxy (no built-in TLS).
  --dry-run         Print actions without changing anything.
  -h, --help        This help.
EOF
}

parse_args() {
  while [[ $# -gt 0 ]]; do
    case "$1" in
      --domain) DOMAIN="$2"; shift 2;;
      --user) USER_NAME="$2"; shift 2;;
      --version) VERSION="$2"; shift 2;;
      --expiration) EXPIRY="$2"; shift 2;;
      --no-tls) USE_TLS=0; shift;;
      --dry-run) DRY_RUN=1; shift;;
      -h|--help) usage; exit 0;;
      *) die "unknown option: $1 (see --help)";;
    esac
  done
  require_root
  [[ -n "$DOMAIN" ]] || die "--domain is required"
  [[ "$DOMAIN" =~ ^[A-Za-z0-9.-]+$ ]] || die "--domain looks invalid: $DOMAIN"
  [[ "$EXPIRY" =~ ^[0-9]+[smhd]$ ]] || die "--expiration must be a Go duration like 720h, got: $EXPIRY"
  [[ -n "$USER_NAME" ]] || die "--user must not be empty"
  need_cmd curl
  need_cmd dpkg
}

detect_version() {
  [[ -n "$VERSION" ]] && { printf '%s' "$VERSION"; return; }
  local tag
  tag="$(curl -fsSL https://api.github.com/repos/juanfont/headscale/releases/latest \
         | sed -n 's/.*"tag_name":[[:space:]]*"v\{0,1\}\([^"]*\)".*/\1/p' | head -n1)"
  [[ -n "$tag" ]] || die "could not detect latest Headscale version; pass --version X.Y.Z"
  printf '%s' "$tag"
}

install_headscale() {
  if have_cmd headscale; then
    ok "headscale already installed ($(headscale version 2>/dev/null | head -n1))"
    return
  fi
  local ver arch url tmp
  ver="$(detect_version)"
  arch="$(dpkg --print-architecture)"
  url="https://github.com/juanfont/headscale/releases/download/v${ver}/headscale_${ver}_linux_${arch}.deb"
  tmp="$(mktemp --suffix=.deb)"
  log "downloading Headscale v${ver} (${arch})"
  run curl -fsSL -o "$tmp" "$url"
  log "installing package"
  run apt-get install -y "$tmp"
  run rm -f "$tmp"
}

# set or append a top-level scalar key in the YAML config (other keys untouched)
set_yaml() {
  local key="$1" val="$2"
  if grep -qE "^[[:space:]]*${key}:" "$CFG"; then
    run sed -i -E "s|^([[:space:]]*)${key}:.*|\1${key}: ${val}|" "$CFG"
  elif [[ $DRY_RUN -eq 1 ]]; then
    echo "   + append ${key}: ${val} >> $CFG"
  else
    printf '%s: %s\n' "$key" "$val" >> "$CFG"
  fi
}

configure_headscale() {
  [[ -f "$CFG" ]] || die "expected config at $CFG (did the package install correctly?)"
  run cp -a "$CFG" "${CFG}.bak.$(date +%Y%m%d%H%M%S)"
  set_yaml server_url "https://${DOMAIN}"
  if [[ $USE_TLS -eq 1 ]]; then
    set_yaml listen_addr "0.0.0.0:443"
    set_yaml tls_letsencrypt_hostname "${DOMAIN}"
    set_yaml tls_letsencrypt_challenge_type "HTTP-01"
    set_yaml tls_letsencrypt_listen ":http"
    warn "built-in TLS: ports 80 (ACME challenge) and 443 must be reachable."
  else
    set_yaml listen_addr "$LISTEN_PLAIN"
    warn "--no-tls: terminate TLS at a reverse proxy in front of $LISTEN_PLAIN."
  fi
  ok "configured $CFG (server_url=https://${DOMAIN})"
}

start_headscale() {
  run systemctl enable --now headscale
  if [[ $DRY_RUN -eq 0 ]]; then
    sleep 2
    systemctl is-active --quiet headscale \
      && ok "headscale is running" \
      || warn "headscale not active; check 'journalctl -u headscale -e'"
  fi
}

provision_user() {
  if [[ $DRY_RUN -eq 1 ]]; then
    echo "   + headscale users create $USER_NAME"
    echo "   + headscale preauthkeys create --user $USER_NAME --reusable --expiration $EXPIRY"
    return
  fi
  if ! headscale users list 2>/dev/null | grep -qw "$USER_NAME"; then
    log "creating user '$USER_NAME'"
    headscale users create "$USER_NAME" || warn "users create failed (may already exist)"
  else
    ok "user '$USER_NAME' already exists"
  fi
  log "minting reusable pre-auth key (valid $EXPIRY)"
  # Newer headscale wants the user id; older accepts the name. Try name, then id.
  PREAUTH_KEY="$(headscale preauthkeys create --user "$USER_NAME" --reusable --expiration "$EXPIRY" 2>/dev/null | tail -n1 || true)"
  if [[ -z "$PREAUTH_KEY" || "$PREAUTH_KEY" == *" "* ]]; then
    local uid
    uid="$(headscale users list 2>/dev/null | awk -v u="$USER_NAME" '$0 ~ u {print $1; exit}')"
    [[ -n "$uid" ]] && PREAUTH_KEY="$(headscale preauthkeys create --user "$uid" --reusable --expiration "$EXPIRY" 2>/dev/null | tail -n1 || true)"
  fi
  if [[ -n "$PREAUTH_KEY" ]]; then
    ok "pre-auth key (treat as a secret): $PREAUTH_KEY"
  else
    warn "could not auto-mint a key; run: headscale preauthkeys create --user $USER_NAME --reusable --expiration $EXPIRY"
  fi
}

main() {
  parse_args "$@"
  install_headscale
  configure_headscale
  start_headscale
  provision_user

  cat <<EOF

$(ok "Headscale ready at https://${DOMAIN}")

Join the Plex server and every client with:
  sudo tailscale up --login-server https://${DOMAIN} --authkey ${PREAUTH_KEY:-<preauth-key>}

On the Plex host, do VPN + Plex config in one step:
  sudo ./plex-tailscale-setup.sh --login-server https://${DOMAIN} --authkey ${PREAUTH_KEY:-<preauth-key>}

Manage access:
  headscale users list
  headscale nodes list
  headscale preauthkeys create --user ${USER_NAME} --reusable --expiration ${EXPIRY}
EOF
}

main "$@"
