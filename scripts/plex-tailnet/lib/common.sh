# SPDX-License-Identifier: AGPL-3.0-or-later
# shellcheck shell=bash
#
# Shared helpers for the plex-tailnet scripts. SOURCE this file; do not run it.
# Keeping the generic concerns (logging, dry-run execution, prompts, guards,
# secret redaction) here removes duplication between the setup scripts and keeps
# each script focused on its own orchestration.

# --- colour-aware logging (colours only on a TTY) ---------------------------
_c()   { [[ -t 1 ]] && printf '%s' "$1" || true; }
log()  { printf '%s[*]%s %s\n' "$(_c $'\033[1;34m')" "$(_c $'\033[0m')" "$*"; }
ok()   { printf '%s[+]%s %s\n' "$(_c $'\033[1;32m')" "$(_c $'\033[0m')" "$*"; }
warn() { printf '%s[!]%s %s\n' "$(_c $'\033[1;33m')" "$(_c $'\033[0m')" "$*" >&2; }
die()  { printf '%s[x]%s %s\n' "$(_c $'\033[1;31m')" "$(_c $'\033[0m')" "$*" >&2; exit 1; }

# --- command execution that honours DRY_RUN ---------------------------------
: "${DRY_RUN:=0}"
run() {
  if [[ $DRY_RUN -eq 1 ]]; then
    printf '   +'; printf ' %q' "$@"; echo
  else
    "$@"
  fi
}

# --- fail fast with a located diagnostic ------------------------------------
# Usage: enable_error_trap   (after sourcing). Tolerated failures must be
# guarded with `|| true` / `|| warn ...` as usual.
__err_trap() { warn "aborted (exit $1) near line $2"; exit "$1"; }
enable_error_trap() { trap '__err_trap "$?" "$LINENO"' ERR; }

# --- guards / predicates ----------------------------------------------------
require_root() { [[ "${EUID:-$(id -u)}" -eq 0 ]] || die "must run as root (use sudo)"; }
need_cmd()     { command -v "$1" >/dev/null 2>&1 || die "required command not found: $1"; }
have_cmd()     { command -v "$1" >/dev/null 2>&1; }
is_port()      { [[ "$1" =~ ^[0-9]+$ ]] && (( 10#$1 >= 1 && 10#$1 <= 65535 )); }

# --- interactive prompts (read the controlling terminal directly) -----------
ask_yes_no() { # question [default Y|N] -> 0 = yes, 1 = no
  local q="$1" def="${2:-Y}" ans prompt
  [[ "$def" == "Y" ]] && prompt="[Y/n]" || prompt="[y/N]"
  read -r -p "$(printf '%s[?]%s %s %s ' "$(_c $'\033[1;36m')" "$(_c $'\033[0m')" "$q" "$prompt")" ans </dev/tty || ans=""
  ans="${ans:-$def}"
  [[ "$ans" =~ ^[Yy] ]]
}

ask_choice() { # question default opt...  -> echoes the chosen value (prompt on stderr)
  local q="$1" def="$2"; shift 2
  local opts=("$@") i ans o
  {
    printf '%s[?]%s %s\n' "$(_c $'\033[1;36m')" "$(_c $'\033[0m')" "$q"
    for i in "${!opts[@]}"; do
      printf '      %d) %s%s\n' "$((i + 1))" "${opts[$i]}" "$([[ ${opts[$i]} == "$def" ]] && echo ' (default)')"
    done
    printf '    choice [%s]: ' "$def"
  } >&2
  read -r ans </dev/tty || ans=""
  [[ -z "$ans" ]] && { printf '%s' "$def"; return; }
  if [[ "$ans" =~ ^[0-9]+$ ]] && (( ans >= 1 && ans <= ${#opts[@]} )); then
    printf '%s' "${opts[$((ans - 1))]}"; return
  fi
  for o in "${opts[@]}"; do [[ "$ans" == "$o" ]] && { printf '%s' "$o"; return; }; done
  printf '%s' "$def"
}

# --- secret redaction for logging -------------------------------------------
# redact_after FLAG ARG... -> echoes ARGs with the value following FLAG masked.
redact_after() {
  local flag="$1"; shift
  local out=() mask=0 a
  for a in "$@"; do
    if [[ $mask -eq 1 ]]; then out+=("***"); mask=0
    else out+=("$a"); [[ "$a" == "$flag" ]] && mask=1; fi
  done
  printf '%s' "${out[*]}"
}
