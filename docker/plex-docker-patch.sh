#!/usr/bin/env bash
# docker/plex-docker-patch.sh
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Patch a running Plex Media Server container in place.
#
# ── Overview ──────────────────────────────────────────────────────────────
# Same LD_PRELOAD mechanism as the Dockerfile-based approach: the s6
# `svc-plex` `run` file is rewritten to exec the in-container wrapper,
# which sets LD_PRELOAD last and execs the PMS binary. The .so's
# constructor (src/main.cpp) calls unsetenv, so glibc helper children
# (Tuner, Script Host, transcoders) are unaffected.
#
# No `docker build`, no container recreate, original image untouched.
# Revertible via `uninstall` (a `.orig` copy of the s6 `run` file is kept).
#
# ── Usage ──────────────────────────────────────────────────────────────────
#   plex-docker-patch.sh [flags] <subcommand> [container-name]
#
# Subcommands:
#   install   [name]   Apply the patch in place (default if omitted)
#   uninstall [name]   Restore the s6 run file from its .orig
#   status    [name]   Report whether the patch is active
#   help               Show usage
#
# Flags (can appear before or after the subcommand):
#   --name <name>      Container name (alternative to positional)
#   --no-build         Use existing build/plexmediaserver_crack.so; do not invoke build.sh
#   --force-rebuild    Delete build/plexmediaserver_crack.so and rebuild from scratch
#   --dry-run          Print the actions that would be taken without executing them
#   --verbose, -v      Trace every docker/build command to stderr before execution
#   --quiet, -q        Suppress non-essential output (only the final report)
#   --version          Print the script version and exit
#
# Container name defaults to "plex". Both plexinc/pms-docker and
# lscr.io/linuxserver/plex are auto-detected by reading the s6 run file:
# LSIO uses `s6-setuidgid abc`; plexinc does not.
#
# The .so is built locally via the project's build.sh, so a zig-capable
# toolchain is required on the host (or an existing build/plexmediaserver_crack.so
# is reused). See docs/DOCKER.md for the full guide.
#
# ── Requirements ──────────────────────────────────────────────────────────
#   - docker on PATH and accessible to the current user
#   - bash 4+ (or bash 3.2+ on macOS; arrays + $'...' are used)
#   - curl + xz-utils (for build.sh) if no prebuilt .so
#
# ── Idempotency ────────────────────────────────────────────────────────────
# install:    safe to re-run. The .orig is preserved across re-installs, the
#             .so and wrapper are overwritten with the latest build, the
#             run file is rewritten, the container is restarted.
# uninstall:  safe to re-run. The run file is re-copied from .orig each time.
# status:     read-only, always safe.
#
# ── Exit codes ─────────────────────────────────────────────────────────────
#   0  success
#   1  runtime error (docker missing, container not found, install failed)
#   2  usage error (unknown subcommand or flag)
#
# ── Design notes ──────────────────────────────────────────────────────────
# - `set -euo pipefail` for fail-fast. All transient failures must surface
#   as non-zero exits so callers can detect them.
# - `docker exec sh -c '...' _ "${var}"` is the only safe pattern for
#   passing host-side paths into the container's shell: single-quote the
#   command so the host bash does NOT interpolate, then pass the path as
#   a positional arg. Without this, a path containing `;` or `$()` would
#   be a command-injection vector. (Currently safe because all paths come
#   from a hardcoded candidate list, but the safe pattern is enforced
#   throughout for future-proofing.)
# - PMS PID lookup scans /proc/*/comm (not ps -ef | grep), which avoids
#   the "Plex" pattern matching the scanning command itself, and works
#   regardless of which process-listing tools the container provides.
# - All destructive operations route through `run`, which respects
#   --dry-run and --verbose. This is the single place to look to see what
#   side effects the script has.
# - Logging: stdout is for human-readable status and the final report;
#   stderr is for warnings, errors, --verbose traces, and --dry-run plans.
#   This makes the script CI-friendly (pipe stdout, capture stderr).
# - `DEBUG=1` env var enables `set -x` for full command tracing.
#
# ── Version ────────────────────────────────────────────────────────────────
SCRIPT_VERSION='1.0.0'

set -euo pipefail
[[ -n "${DEBUG:-}" ]] && set -x

# ── Paths / constants ─────────────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

# In-container paths (must match what the Dockerfiles copy to).
SO_PATH_INSIDE="/usr/lib/plexmediaserver/lib/plexmediaserver_crack.so"
WRAPPER_PATH_INSIDE="/usr/lib/plexmediaserver/plex-crack-wrapper.sh"
PMS_COMM_NAME="Plex Media Server"   # prctl(PR_SET_NAME) sets this

# s6-overlay v3 / v2 candidate paths, in preference order. If the upstream
# image's s6 layout changes, this is the single place to update.
S6_RUN_CANDIDATES=(
    "/etc/s6-overlay/s6-rc.d/svc-plex/run"
    "/etc/services.d/plex/run"
)

# Operational defaults.
DEFAULT_CONTAINER="plex"
MAX_WAIT_ITERATIONS=30
WAIT_INTERVAL_SECONDS=2
PMS_HTTP_PORT=32400

# ── Arg-parser state ──────────────────────────────────────────────────────
SUBCOMMAND=""
CONTAINER_NAME=""
DRY_RUN=false
VERBOSE=false
QUIET=false
SKIP_BUILD=false
FORCE_REBUILD=false

# ── Logging ───────────────────────────────────────────────────────────────
# stdout -> human-readable status, success messages, final report
# stderr -> warnings, errors, --verbose traces, --dry-run plans
log()  { [ "${QUIET}" != "true" ] && printf '%s\n' "$*"; }
warn() { printf 'WARNING: %s\n' "$*" >&2; }
die()  { printf 'ERROR: %s\n' "$*" >&2; exit 1; }
trace() { [ "${VERBOSE}" = "true" ] && printf '+ %s\n' "$*" >&2 || true; }

# ── Command runner: respects --dry-run and --verbose ──────────────────────
# All destructive operations go through this wrapper. In dry-run mode
# the command is echoed to stderr and skipped. In verbose mode the
# command is echoed to stderr before execution. Stdout is captured by
# the caller as usual.
run() {
    if [ "${DRY_RUN}" = "true" ]; then
        printf '[dry-run]'
        local arg
        for arg in "$@"; do
            printf ' %s' "${arg}"
        done
        printf '\n' >&2
    else
        trace "$*"
        "$@"
    fi
}

# ── Usage ─────────────────────────────────────────────────────────────────
print_usage() {
    cat <<EOF
Usage: $(basename "$0") [flags] <subcommand> [container-name]

Subcommands:
  install   [name]   Apply the patch in place (default)
  uninstall [name]   Revert the s6 run file from its .orig
  status    [name]   Report whether the patch is active
  help               Show this message

Flags (can appear before or after the subcommand):
  --name <name>      Container name (alternative to positional)
  --no-build         Use existing build/plexmediaserver_crack.so; do not invoke build.sh
  --force-rebuild    Delete build/plexmediaserver_crack.so and rebuild from scratch
  --dry-run          Print the actions that would be taken without executing them
  --verbose, -v      Trace every docker/build command to stderr
  --quiet, -q        Suppress non-essential output (only the final report)
  --version          Print the script version and exit

Container name defaults to "${DEFAULT_CONTAINER}".

Examples:
  $(basename "$0") install                      # default container
  $(basename "$0") install my-plex              # custom name
  $(basename "$0") --name my-plex install       # flag form
  $(basename "$0") install --dry-run            # preview only
  $(basename "$0") --verbose status my-plex     # trace every docker call
  $(basename "$0") uninstall my-plex

The script auto-detects the base image (plexinc vs linuxserver) by
reading the s6 svc-plex run file inside the container. The same .so and
docker/wrapper.sh are used in both cases; only the run file content
differs (LSIO preserves s6-setuidgid abc).
EOF
}

# ── Arg parsing ───────────────────────────────────────────────────────────
parse_args() {
    while [ "$#" -gt 0 ]; do
        case "$1" in
            install|uninstall|status)
                if [ -n "${SUBCOMMAND}" ]; then
                    die "subcommand already specified: ${SUBCOMMAND}"
                fi
                SUBCOMMAND="$1"
                shift
                ;;
            help|-h|--help)            print_usage; exit 0 ;;
            --name)                    [ "$#" -ge 2 ] || die "--name requires an argument"
                                        CONTAINER_NAME="$2"; shift 2 ;;
            --name=*)                  CONTAINER_NAME="${1#--name=}"; shift ;;
            --no-build)                SKIP_BUILD=true;     shift ;;
            --force-rebuild)           FORCE_REBUILD=true;  shift ;;
            --dry-run)                 DRY_RUN=true;        shift ;;
            --verbose|-v)              VERBOSE=true;        shift ;;
            --quiet|-q)                QUIET=true;          shift ;;
            --version)                 printf '%s\n' "${SCRIPT_VERSION}"; exit 0 ;;
            --)                        shift; break ;;
            -*)                        die "unknown flag: $1 (try --help)" ;;
            *)
                if [ -z "${CONTAINER_NAME}" ]; then
                    CONTAINER_NAME="$1"
                else
                    die "unexpected positional argument: $1"
                fi
                shift
                ;;
        esac
    done

    SUBCOMMAND="${SUBCOMMAND:-install}"
    CONTAINER_NAME="${CONTAINER_NAME:-${DEFAULT_CONTAINER}}"

    # Mutual-exclusion checks.
    if [ "${SKIP_BUILD}" = "true" ] && [ "${FORCE_REBUILD}" = "true" ]; then
        die "--no-build and --force-rebuild are mutually exclusive"
    fi
    if [ "${QUIET}" = "true" ] && [ "${VERBOSE}" = "true" ]; then
        die "--quiet and --verbose are mutually exclusive"
    fi

    # Make state available to subcommand functions.
    export SUBCOMMAND CONTAINER_NAME DRY_RUN VERBOSE QUIET SKIP_BUILD FORCE_REBUILD
}

# ── Pre-flight checks ────────────────────────────────────────────────────
require_docker() {
    command -v docker >/dev/null 2>&1 || die "docker not on PATH"
}

require_container_exists() {
    require_docker
    if ! docker inspect "${CONTAINER_NAME}" >/dev/null 2>&1; then
        die "container '${CONTAINER_NAME}' not found. Start one with: docker run -d --name ${CONTAINER_NAME} ..."
    fi
}

require_container_running() {
    require_container_exists
    local state
    state="$(docker inspect --format '{{.State.Running}}' "${CONTAINER_NAME}" 2>/dev/null || echo unknown)"
    if [ "${state}" != "true" ]; then
        die "container '${CONTAINER_NAME}' is not running (state: ${state}). Start it with: docker start ${CONTAINER_NAME}"
    fi
}

# ── Detection ─────────────────────────────────────────────────────────────

# Echoes the s6 svc-plex run file path on stdout, or returns 1.
# Two candidates are tried in preference order: s6-overlay v3 path, then
# the legacy v2 path.
detect_run_script() {
    local p
    for p in "${S6_RUN_CANDIDATES[@]}"; do
        if run docker exec -u root "${CONTAINER_NAME}" test -f "${p}" 2>/dev/null; then
            printf '%s\n' "${p}"
            return 0
        fi
    done
    return 1
}

# Echoes one of: plexinc, linuxserver, unknown
# Detection: LSIO's run file content includes `s6-setuidgid abc`; plexinc's
# does not. If neither marker is found, return `unknown` (the install
# flow will refuse to proceed).
detect_base_image() {
    local run_script="$1"
    local content
    content="$(run docker exec -u root "${CONTAINER_NAME}" cat "${run_script}" 2>/dev/null || true)"
    if printf '%s' "${content}" | grep -q 's6-setuidgid abc'; then
        printf 'linuxserver\n'
    elif printf '%s' "${content}" | grep -qE 'Plex Media Server|start\.sh|with-contenv'; then
        printf 'plexinc\n'
    else
        printf 'unknown\n'
    fi
}

# Echoes the PMS PID on stdout, or empty.
# Strategy: scan /proc/*/comm for the PMS comm name (set via
# prctl(PR_SET_NAME)). This avoids the "ps -ef | grep | grep -v grep"
# pattern, which has a tendency to match its own command line, and works
# regardless of which process-listing tools are in the container.
find_pms_pid() {
    run docker exec "${CONTAINER_NAME}" sh -c '
        for d in /proc/[0-9]*; do
            [ -r "$d/comm" ] || continue
            if [ "$(cat "$d/comm" 2>/dev/null)" = "$1" ]; then
                basename "$d"
                exit 0
            fi
        done
        exit 1
    ' _ "${PMS_COMM_NAME}" 2>/dev/null || true
}

# ── Build ─────────────────────────────────────────────────────────────────
build_so() {
    local so_path="${PROJECT_ROOT}/build/plexmediaserver_crack.so"

    if [ "${FORCE_REBUILD}" = "true" ] && [ -f "${so_path}" ]; then
        log "[*] --force-rebuild: removing existing .so"
        run rm -f "${so_path}"
    fi

    if [ "${SKIP_BUILD}" = "true" ]; then
        if [ ! -f "${so_path}" ]; then
            die "--no-build specified but ${so_path} does not exist. Build it first with: bash build.sh"
        fi
        log "[*] --no-build: reusing existing .so (build.sh not invoked)"
        return 0
    fi

    if [ -f "${so_path}" ]; then
        log "[*] Reusing existing .so (rm it or pass --force-rebuild to rebuild)"
        return 0
    fi

    log "[*] Building .so via build.sh (this may take 1-2 min on first run)..."
    ( cd "${PROJECT_ROOT}" && run bash build.sh )
}

# ── Filesystem ops ────────────────────────────────────────────────────────
# Build the new s6 run file content locally and docker cp it in. We do
# this locally (rather than heredoc-over-docker-exec) to avoid quoting
# hell and command-injection risk.
write_new_run_file() {
    local base="$1" out="$2"
    {
        printf '#!/usr/bin/with-contenv bash\n'
        if [ "${base}" = "linuxserver" ]; then
            printf 'exec s6-setuidgid abc %s\n' "${WRAPPER_PATH_INSIDE}"
        else
            printf 'exec %s\n' "${WRAPPER_PATH_INSIDE}"
        fi
    } > "${out}"
    chmod 0755 "${out}"
}

# Back up the s6 run file to .orig. Idempotent: if .orig exists, leave it.
# Uses the safe `sh -c '...' _ "${path}"` pattern: single-quoted command
# + positional arg, so the host bash never interpolates the path.
backup_run_file() {
    local run_script="$1"
    if run docker exec -u root "${CONTAINER_NAME}" test -f "${run_script}.orig" 2>/dev/null; then
        log "[*] ${run_script}.orig already present, leaving it"
        return 0
    fi
    run docker exec -u root "${CONTAINER_NAME}" \
        sh -c 'cp "$1" "$1".orig' _ "${run_script}"
    log "[*] Backed up ${run_script} -> ${run_script}.orig"
}

# Copy the .so and wrapper into the container and chmod the wrapper.
copy_artifacts() {
    local so_host="${PROJECT_ROOT}/build/plexmediaserver_crack.so"
    local wrapper_host="${SCRIPT_DIR}/wrapper.sh"

    if [ ! -f "${so_host}" ]; then
        die "${so_host} does not exist. Build it first with: bash build.sh"
    fi
    if [ ! -f "${wrapper_host}" ]; then
        die "${wrapper_host} does not exist. Re-clone the project or restore docker/wrapper.sh."
    fi

    log "[*] Copying .so and wrapper into the container..."
    run docker cp "${so_host}" "${CONTAINER_NAME}:${SO_PATH_INSIDE}"
    run docker cp "${wrapper_host}" "${CONTAINER_NAME}:${WRAPPER_PATH_INSIDE}"
    run docker exec -u root "${CONTAINER_NAME}" chmod 0755 "${WRAPPER_PATH_INSIDE}"
}

# ── Verification ───────────────────────────────────────────────────────────
# Polls PMS /identity until it returns 200 or the timeout expires.
wait_for_pms_ready() {
    log "[*] Waiting for PMS /identity (up to $((MAX_WAIT_ITERATIONS * WAIT_INTERVAL_SECONDS))s)..."
    local i code
    for i in $(seq 1 "${MAX_WAIT_ITERATIONS}"); do
        code="$(curl -fsS -o /dev/null -w '%{http_code}' "http://127.0.0.1:${PMS_HTTP_PORT}/identity" 2>/dev/null || echo 000)"
        if [ "${code}" = "200" ]; then
            log "[*] PMS is up (HTTP 200)."
            return 0
        fi
        sleep "${WAIT_INTERVAL_SECONDS}"
    done
    return 1
}

# Confirms the .so is mapped into the PMS process.
verify_so_mapped() {
    local pms_pid="$1"
    if [ -z "${pms_pid}" ]; then
        warn "could not find PMS pid inside container"
        return 1
    fi
    if run docker exec "${CONTAINER_NAME}" grep -F plexmediaserver_crack.so "/proc/${pms_pid}/maps" >/dev/null 2>&1; then
        log "[*] OK: plexmediaserver_crack.so is mapped into PMS (pid ${pms_pid})"
        return 0
    fi
    warn "PMS running (pid ${pms_pid}) but .so is NOT in /proc/${pms_pid}/maps"
    warn "    This usually means PMS exited 127 (loader failure). Check: docker logs ${CONTAINER_NAME} | tail -50"
    return 1
}

# ── Subcommands ──────────────────────────────────────────────────────────
do_install() {
    require_container_running
    log "[*] Container: ${CONTAINER_NAME}"

    local run_script
    if ! run_script="$(detect_run_script)"; then
        die "could not find s6 svc-plex run file in container. Tried: ${S6_RUN_CANDIDATES[*]}"
    fi
    log "[*] s6 run file: ${run_script}"

    local base
    base="$(detect_base_image "${run_script}")"
    case "${base}" in
        linuxserver) log "[*] Detected base: lscr.io/linuxserver/plex (s6-setuidgid abc will be preserved)" ;;
        plexinc)     log "[*] Detected base: plexinc/pms-docker" ;;
        *)
            die "could not detect base image from s6 run file content. Please file an issue with the file contents."
            ;;
    esac

    build_so
    copy_artifacts
    backup_run_file "${run_script}"

    # Write the new run file locally and copy it in. The mktemp + trap
    # pattern ensures we never leak the temp file, even on error paths.
    local tmp_run
    tmp_run="$(mktemp)"
    trap 'rm -f "${tmp_run}"' EXIT
    write_new_run_file "${base}" "${tmp_run}"
    run docker cp "${tmp_run}" "${CONTAINER_NAME}:${run_script}"
    rm -f "${tmp_run}"
    trap - EXIT

    log "[*] Restarting ${CONTAINER_NAME} (s6 will exec the new run file)..."
    run docker restart "${CONTAINER_NAME}" >/dev/null

    if ! wait_for_pms_ready; then
        warn "PMS did not return 200 within $((MAX_WAIT_ITERATIONS * WAIT_INTERVAL_SECONDS))s. Check: docker logs ${CONTAINER_NAME} | tail -50"
    fi

    local pms_pid
    pms_pid="$(find_pms_pid)"
    verify_so_mapped "${pms_pid}"

    cat <<EOF

Patch applied. For full verification (all 14 feature bits ON):
  docker cp ${PROJECT_ROOT}/scripts/readbitset.py ${CONTAINER_NAME}:/tmp/readbitset.py
  docker exec -u root ${CONTAINER_NAME} python3 /tmp/readbitset.py ${pms_pid:-<PMS_PID>}

To revert:  $(basename "$0") uninstall ${CONTAINER_NAME}
To re-check: $(basename "$0") status ${CONTAINER_NAME}
EOF
}

do_uninstall() {
    require_container_running

    local run_script
    if ! run_script="$(detect_run_script)"; then
        die "could not find s6 svc-plex run file. Tried: ${S6_RUN_CANDIDATES[*]}"
    fi

    if ! run docker exec -u root "${CONTAINER_NAME}" test -f "${run_script}.orig" 2>/dev/null; then
        die "no ${run_script}.orig found -- the patch may not be installed, or the .orig was deleted. Manual restore: docker cp <upstream-image>:${run_script} ${CONTAINER_NAME}:${run_script}"
    fi

    log "[*] Restoring ${run_script} from .orig..."
    run docker exec -u root "${CONTAINER_NAME}" \
        sh -c 'cp "$1" "$2"' _ "${run_script}.orig" "${run_script}"
    run docker exec -u root "${CONTAINER_NAME}" chmod 0755 "${run_script}"

    log "[*] Restarting ${CONTAINER_NAME}..."
    run docker restart "${CONTAINER_NAME}" >/dev/null

    cat <<EOF

Patch removed. PMS is back to upstream behavior. Optional cleanup:
  docker exec -u root ${CONTAINER_NAME} rm -f ${SO_PATH_INSIDE} ${WRAPPER_PATH_INSIDE}
  docker exec -u root ${CONTAINER_NAME} rm -f ${run_script}.orig
EOF
}

do_status() {
    require_container_exists

    local run_script
    if ! run_script="$(detect_run_script)"; then
        warn "s6 svc-plex run file not found (tried: ${S6_RUN_CANDIDATES[*]})"
        return 1
    fi

    local run_content has_orig has_so has_wrapper pms_pid
    run_content="$(run docker exec -u root "${CONTAINER_NAME}" cat "${run_script}" 2>/dev/null || true)"
    if run docker exec -u root "${CONTAINER_NAME}" test -f "${run_script}.orig" 2>/dev/null; then
        has_orig="yes"
    else
        has_orig="no"
    fi
    if run docker exec -u root "${CONTAINER_NAME}" test -f "${SO_PATH_INSIDE}" 2>/dev/null; then
        has_so="yes"
    else
        has_so="no"
    fi
    if run docker exec -u root "${CONTAINER_NAME}" test -x "${WRAPPER_PATH_INSIDE}" 2>/dev/null; then
        has_wrapper="yes"
    else
        has_wrapper="no"
    fi
    pms_pid="$(find_pms_pid)"

    log "Container: ${CONTAINER_NAME}"
    log "  s6 run file:   ${run_script}"
    log "  .orig present: ${has_orig}"
    log "  .so present:   ${has_so}  (${SO_PATH_INSIDE})"
    log "  wrapper:       ${has_wrapper}  (${WRAPPER_PATH_INSIDE})"
    log "  PMS pid:       ${pms_pid:-<not running>}"
    log "  run file content:"
    printf '%s\n' "${run_content}" | sed 's/^/    /'
    log ""

    if printf '%s' "${run_content}" | grep -q 'plex-crack-wrapper.sh'; then
        log "[*] PATCH IS ACTIVE (s6 run file points to the wrapper)."
        if [ -n "${pms_pid}" ] \
            && run docker exec "${CONTAINER_NAME}" grep -F plexmediaserver_crack.so "/proc/${pms_pid}/maps" >/dev/null 2>&1; then
            log "[*] .so is mapped into PMS (pid ${pms_pid})."
        else
            warn "PMS running (pid ${pms_pid:-?}) but .so is NOT in its maps; check docker logs."
        fi
    else
        log "[*] PATCH IS NOT ACTIVE (s6 run file does not point to the wrapper)."
    fi
}

# ── Main ──────────────────────────────────────────────────────────────────
parse_args "$@"
case "${SUBCOMMAND}" in
    install)   do_install ;;
    uninstall) do_uninstall ;;
    status)    do_status ;;
esac
