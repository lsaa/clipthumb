#!/usr/bin/env bash
set -euo pipefail

# --- Configuration ---
XVFB_DISPLAY="${XVFB_DISPLAY:-:8842}"
XVFB_RESOLUTION="1024x1024x24"
DEFAULT_THUMB_SIZE="256"

# Files (no "clients file")
SESSION_ID="${XVFB_DISPLAY//:/}"
XVFB_PIDFILE="/tmp/xvfb_${SESSION_ID}.pid"
XVFB_LOCKDIR="/tmp/xvfb_${SESSION_ID}.lock"

# --- Logging helpers ---
log_error() { printf '[ERROR]  (%s) %s\n' "$(date +%T)" "$1" >&2; }
log_info()  { printf '[INFO]   (%s) %s\n' "$(date +%T)" "$1" >&2; }

# --- Utility to check whether an Xvfb pid is valid and matches the display ---
_xvfb_pid_valid() {
    local pid="$1"
    if [[ -z "$pid" ]] || ! kill -0 "$pid" 2>/dev/null; then
        return 1
    fi
    # Ensure the process command line looks like Xvfb and mentions the display
    local args
    args=$(ps -p "$pid" -o args= 2>/dev/null || true)
    if [[ "$args" == *"Xvfb"* && "$args" == *"$XVFB_DISPLAY"* ]]; then
        return 0
    fi
    return 1
}

# --- Start Xvfb if not running; use an atomic lockdir to avoid races ---
start_session_if_needed() {
    # fast check: if pidfile exists and points at a valid Xvfb, we're done
    if [[ -r "$XVFB_PIDFILE" ]]; then
        local pid
        pid=$(<"$XVFB_PIDFILE")
        if _xvfb_pid_valid "$pid"; then
            export DISPLAY="$XVFB_DISPLAY"
            return 0
        else
            # stale pidfile
            rm -f "$XVFB_PIDFILE" || true
        fi
    fi

    # try to create lockdir atomically; if we fail, another process is starting Xvfb
    local wait=0
    while ! mkdir "$XVFB_LOCKDIR" 2>/dev/null; do
        sleep 0.1
        wait=$((wait + 1))
        # If lockdir persists for a while, check whether pidfile was created
        if (( wait >= 100 )); then
            if [[ -r "$XVFB_PIDFILE" ]]; then
                local pid
                pid=$(<"$XVFB_PIDFILE")
                if _xvfb_pid_valid "$pid"; then
                    export DISPLAY="$XVFB_DISPLAY"
                    return 0
                fi
            fi
            # give up the lock wait and attempt to remove stale lockdir (safe only if it's old)
            if [[ -d "$XVFB_LOCKDIR" ]]; then
                local age
                age=$(expr "$(date +%s)" - "$(stat -c %Y "$XVFB_LOCKDIR")")
                if (( age > 10 )); then
                    rmdir "$XVFB_LOCKDIR" 2>/dev/null || true
                fi
            fi
            wait=0
        fi
    done

    # we own the lockdir now
    trap 'rmdir "$XVFB_LOCKDIR" 2>/dev/null || true' RETURN

    # double-check another process didn't already start Xvfb while we acquired lockdir
    if [[ -r "$XVFB_PIDFILE" ]]; then
        local pid
        pid=$(<"$XVFB_PIDFILE")
        if _xvfb_pid_valid "$pid"; then
            export DISPLAY="$XVFB_DISPLAY"
            return 0
        else
            rm -f "$XVFB_PIDFILE" || true
        fi
    fi

    log_info "Starting Xvfb on display ${XVFB_DISPLAY}."
    # Start Xvfb in background and write a pidfile
    nohup Xvfb "$XVFB_DISPLAY" -screen 0 "$XVFB_RESOLUTION" -ac +extension GLX +render -noreset \
        >/dev/null 2>&1 &
    local xvfb_pid=$!
    # give it a moment to come up
    for i in {1..10}; do
        if _xvfb_pid_valid "$xvfb_pid"; then
            echo "$xvfb_pid" > "$XVFB_PIDFILE"
            export DISPLAY="$XVFB_DISPLAY"
            log_info "Xvfb started with pid $xvfb_pid"
            return 0
        fi
        sleep 0.1
    done

    # if we reach here, starting Xvfb failed
    rmdir "$XVFB_LOCKDIR" 2>/dev/null || true
    trap - RETURN
    log_error "Failed to start Xvfb on ${XVFB_DISPLAY}."
    return 1
}

# --- Detect whether there are any X clients connected to the display ---
_has_connected_clients() {
    # prefer xlsclients if available
    if command -v xlsclients >/dev/null 2>&1; then
        xlsclients -display "$XVFB_DISPLAY" 2>/dev/null | grep -q .
        return $?
    fi
    # fallback: use xwininfo to count child windows (exclude the root itself)
    if command -v xwininfo >/dev/null 2>&1; then
        local count
        count=$(xwininfo -root -tree -display "$XVFB_DISPLAY" 2>/dev/null | awk 'NR>1{print}' | grep -c '0x' || true)
        # If more than zero client windows exist, consider there are clients
        (( count > 0 ))
        return $?
    fi
    # if neither tool exists, be conservative and assume clients exist so we don't kill Xvfb prematurely
    return 0
}

# --- Cleanup: stop Xvfb only if there are no connected clients ---
cleanup_session_if_empty() {
    if [[ ! -r "$XVFB_PIDFILE" ]]; then
        return 0
    fi
    local pid
    pid=$(<"$XVFB_PIDFILE")
    if ! _xvfb_pid_valid "$pid"; then
        rm -f "$XVFB_PIDFILE" 2>/dev/null || true
        return 0
    fi

    # if any clients are connected, leave Xvfb running
    if _has_connected_clients; then
        log_info "Clients still connected to ${XVFB_DISPLAY}; leaving Xvfb running (pid $pid)."
        return 0
    fi

    log_info "No clients detected on ${XVFB_DISPLAY}; shutting down Xvfb (pid $pid)."
    kill "$pid" 2>/dev/null || true
    # wait gracefully a short while
    for i in {1..10}; do
        if ! kill -0 "$pid" 2>/dev/null; then
            break
        fi
        sleep 0.1
    done
    # force kill if still alive
    if kill -0 "$pid" 2>/dev/null; then
        kill -9 "$pid" 2>/dev/null || true
    fi
    rm -f "$XVFB_PIDFILE" 2>/dev/null || true
}

# --- Ensure required binary dependencies exist ---
require_deps() {
    local miss=()
    for cmd in xdotool magick uuidgen Xvfb; do
        if ! command -v "$cmd" >/dev/null 2>&1; then
            miss+=("$cmd")
        fi
    done
    if (( ${#miss[@]} )); then
        log_error "Missing dependency: ${miss[*]}. Install them and retry."
        exit 1
    fi
}

# --- Main thumbnailer ---
main() {
    require_deps

    if [[ $# -lt 2 ]]; then
        echo "Usage: $0 <input_file> <output_png> [size]"
        exit 1
    fi

    local input_file="$1"
    local output_file="$2"
    local thumb_size="${3:-$DEFAULT_THUMB_SIZE}"

    start_session_if_needed || exit 1

    local unique_id="clipthumb-$(uuidgen -r)"
    local wine_pid=""
    trap 'kill "${wine_pid:-}" 2>/dev/null || true; cleanup_session_if_empty' EXIT

    export DISPLAY="$XVFB_DISPLAY"
    WINEPREFIX="/home/user/.local/share/wineprefixes/csp" wine /usr/lib/clipthumb/clipthumb.exe "$input_file" "$unique_id" &
    wine_pid=$!

    # wait for the window to appear (timeout 10s)
    local x11_win_id=""
    for i in {1..10}; do
        # xdotool prints nothing on failure; use || true to avoid set -e killing the script
        x11_win_id=$(xdotool search --onlyvisible --name "$unique_id" 2>/dev/null || true)
        if [[ -n "$x11_win_id" ]]; then
            break
        fi
        sleep 1
    done

    if [[ -z "$x11_win_id" ]]; then
        log_error "Could not find X11 window for ${input_file##*/}"
        # wine process will be cleaned up by trap
        exit 1
    fi

    log_info "Found X11 ID: $x11_win_id for ${input_file##*/}"
    xdotool windowmove "$x11_win_id" 0 0 2>/dev/null || true

    local temp_bmp
    temp_bmp=$(mktemp --suffix=.bmp)
    grabwindow "$x11_win_id" "$temp_bmp" || true

    if [[ ! -s "$temp_bmp" ]]; then
        log_error "grabwindow failed to create a valid BMP file."
        rm -f "$temp_bmp"
        exit 1
    fi

    mkdir -p "$(dirname "$output_file")"
    magick "$temp_bmp" -fuzz 15% -trim +repage -resize "${thumb_size}x${thumb_size}>" "$output_file"
    rm -f "$temp_bmp"

    log_info "Thumbnail for ${input_file##*/} created at ${output_file} (size: ${thumb_size}px)"

    # cleanup and exit (trap will handle stopping Xvfb if appropriate)
    trap - EXIT
    kill "$wine_pid" 2>/dev/null || true
    cleanup_session_if_empty
}

main "$@"
