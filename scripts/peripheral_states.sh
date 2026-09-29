#!/usr/bin/env bash
# peripheral_states.sh
#
# Drives bin/analyze_embedded once per peripheral configuration so the
# report can compare platform idle/active power with the desktop GUI and
# Bluetooth on vs off. These are SYSTEM-LEVEL toggles (they affect the
# whole board, not one process), which is why they live in a script and
# not inside the C harness.
#
# Each invocation tags its output rows via CRITTER_PERIPH_LABEL, and the
# per-state CSVs are concatenated into embedded_results_all.csv at the
# end so every state sits in one table for the report.
#
# Run from the repo root:   sudo ./scripts/peripheral_states.sh
#
# NOTE: toggling the GUI will drop you to a console if you are running
# this FROM the desktop. Run it over SSH so you do not kill your own
# session. The script restores both services to their original state on
# exit (including Ctrl-C) via the trap below.

set -u
BIN=./bin/analyze_embedded
OUT_ALL=embedded_results_all.csv

if [[ ! -x "$BIN" ]]; then
    echo "ERROR: $BIN not found. Run 'make' first." >&2
    exit 1
fi

# --- capture original states so we can restore them ---------------------
gui_was_active=0
bt_was_active=0
systemctl is-active --quiet display-manager 2>/dev/null && gui_was_active=1
systemctl is-active --quiet bluetooth 2>/dev/null && bt_was_active=1

restore() {
    echo ">>> Restoring original peripheral states..."
    if [[ $gui_was_active -eq 1 ]]; then sudo systemctl start display-manager 2>/dev/null; fi
    if [[ $bt_was_active -eq 1 ]]; then
        sudo systemctl start bluetooth 2>/dev/null
        sudo rfkill unblock bluetooth 2>/dev/null
    fi
    echo ">>> Done."
}
trap restore EXIT INT TERM

gui_off() { sudo systemctl stop display-manager 2>/dev/null; sleep 3; }
gui_on()  { sudo systemctl start display-manager 2>/dev/null; sleep 3; }
bt_off()  { sudo rfkill block bluetooth 2>/dev/null; sudo systemctl stop bluetooth 2>/dev/null; sleep 2; }
bt_on()   { sudo systemctl start bluetooth 2>/dev/null; sudo rfkill unblock bluetooth 2>/dev/null; sleep 2; }

run_state() {
    local label="$1"
    echo ""
    echo "############################################################"
    echo "# PERIPHERAL STATE: $label"
    echo "############################################################"
    CRITTER_PERIPH_LABEL="$label" "$BIN"
    # append this state's row (skip header after the first) to the combined csv
    if [[ -f embedded_results.csv ]]; then
        if [[ ! -f "$OUT_ALL" ]]; then
            cp embedded_results.csv "$OUT_ALL"
        else
            tail -n +2 embedded_results.csv >> "$OUT_ALL"
        fi
    fi
}

rm -f "$OUT_ALL"

# --- sweep the four combinations ----------------------------------------
# Order chosen to end back at "everything on" being restored by the trap.
echo ">>> Baseline: whatever the system is at now (labelled as-found)"
run_state "as-found_gui=${gui_was_active}_bt=${bt_was_active}"

echo ">>> GUI off, Bluetooth on"
gui_off; bt_on
run_state "gui-off_bt-on"

echo ">>> GUI off, Bluetooth off"
gui_off; bt_off
run_state "gui-off_bt-off"

echo ">>> GUI on, Bluetooth off"
gui_on; bt_off
run_state "gui-on_bt-off"

echo ""
echo ">>> All states measured. Combined table: $OUT_ALL"
echo ">>> (trap will now restore your original GUI/Bluetooth state)"
