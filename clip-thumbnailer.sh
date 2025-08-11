#!/bin/bash

# --- Configuration ---
XVFB_DISPLAY="${XVFB_DISPLAY:-:8842}"
XVFB_RESOLUTION="1024x1024x24"
DEFAULT_THUMB_SIZE="256"
WINEPREFIX="/home/user/.local/share/wineprefixes/csp"

# --- Session Management ---
SESSION_ID="${XVFB_DISPLAY//:/}"
XVFB_SESSION_LOCK_FILE="/tmp/xvfb_${SESSION_ID}.session.lock"

# --- Helper Functions ---
log_error() { echo "[ERROR] ($(date +%T)) $1" >&2; }
log_info() { echo "[INFO]  ($(date +%T)) $1" >&2; }

start_session_if_needed() {
    (
        flock -xn 200 || exit 1
        if ! xset -q -display "$XVFB_DISPLAY" > /dev/null 2>&1; then
            log_info "Starting Xvfb on display ${XVFB_DISPLAY}."
            Xvfb "$XVFB_DISPLAY" -screen 0 "$XVFB_RESOLUTION" -ac +extension GLX +render -noreset &
            sleep 1
        fi
    ) 200>"$XVFB_SESSION_LOCK_FILE"
}

cleanup_session_if_empty() {
    (
        flock -x 200
        local window_count
        window_count=$(xwininfo -root -children -display "$XVFB_DISPLAY" 2>/dev/null | grep -c '0x')
        if [[ $window_count -le 1 ]]; then
            log_info "No more client windows. Shutting down Xvfb session."
            pkill -f "Xvfb ${XVFB_DISPLAY}"
            rm -f "$XVFB_SESSION_LOCK_FILE"
        fi
    ) 200>"$XVFB_SESSION_LOCK_FILE"
}

# --- Main Thumbnailing Logic (Final Version) ---
main() {
    # The thumbnailer spec provides 3 arguments: input, output, and size.
    if [[ $# -lt 2 ]]; then
        echo "Usage: $0 <input_file> <output_png> [size]"
        echo "This script is intended to be called by a FreeDesktop.org thumbnailer service."
        exit 1
    fi

    # Check for dependencies
    if ! command -v xdotool &> /dev/null || ! command -v magick &> /dev/null; then
        log_error "Missing dependency. Please install xdotool and imagemagick."
        exit 1
    fi

    local input_file="$1"
    local output_file="$2"
    local thumb_size="${3:-$DEFAULT_THUMB_SIZE}"

    start_session_if_needed

    local unique_id="clipthumb-$(uuidgen)"
    local wine_pid
    trap 'kill "$wine_pid" 2>/dev/null; cleanup_session_if_empty' EXIT

    export DISPLAY="$XVFB_DISPLAY"
    /usr/lib/clipthumb/clipthumb.exe "$input_file" "$unique_id" &
    wine_pid=$!

    local x11_win_id=""
    for ((i=0; i<10; i++)); do # Timeout after 10 seconds
        x11_win_id=$(xdotool search --onlyvisible --name "$unique_id")
        if [[ -n "$x11_win_id" ]]; then
            break
        fi
        sleep 1
    done

    if [[ -z "$x11_win_id" ]]; then
        log_error "Could not find X11 window for ${input_file##*/}"
        exit 1
    fi

    log_info "Found X11 ID: $x11_win_id for ${input_file##*/}"

    local temp_bmp
    temp_bmp=$(mktemp --suffix=.bmp)

    grabwindow "$x11_win_id" "$temp_bmp"

    if [[ ! -s "$temp_bmp" ]]; then
        log_error "grabwindow failed to create a valid BMP file."
        rm -f "$temp_bmp"
        exit 1
    fi

    # Ensure the output directory exists, then save the thumbnail to the path provided.
    mkdir -p "$(dirname "$output_file")"
    magick "$temp_bmp" -fuzz 15% -trim +repage -resize "${thumb_size}x${thumb_size}>" "$output_file"
    rm -f "$temp_bmp"

    log_info "Thumbnail for ${input_file##*/} created at ${output_file} (size: ${thumb_size}px)"

    # Cleanup is handled by the trap.
    trap - EXIT
    kill "$wine_pid" 2>/dev/null
    cleanup_session_if_empty
}

main "$@"
