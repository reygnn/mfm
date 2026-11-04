# What the black-box tests of mfm share: a headless mwc, simulated input from its
# tests/vinput, and helpers on top. Set LOG and MFM_LOG, then source this.
#
# Needs a built mwc with build/vinput and build/pngtool in MWC_DIR (by default
# ../mwc or ../mwm), and grim. It has its own runtime directory and Wayland socket,
# so it does not touch a running session.
MFM_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
MFM=${MFM:-$MFM_DIR/mfm}
if [ -z "${MWC_DIR:-}" ]; then
	MWC_DIR=$MFM_DIR/../mwc
	[ -d "$MWC_DIR" ] || MWC_DIR=$MFM_DIR/../mwm
fi

[ -x "$MFM" ] || { echo "FAIL: $MFM not built"; exit 1; }
[ -d "$MWC_DIR" ] || { echo "FAIL: no mwc in $MWC_DIR (set MWC_DIR)"; exit 1; }
cd "$MWC_DIR" || exit 1
. tests/lib.sh

require_tools grim
for tool in build/vinput build/pngtool; do
	[ -x "$tool" ] || { echo "FAIL: $MWC_DIR/$tool not built (make $tool)"; exit 1; }
done

export MWC_TEST_STATE=$SMOKE_DIR/state
export XDG_DATA_HOME=$SMOKE_DIR/data
export LC_ALL=C.UTF-8
unset MFM_OPEN MFM_TERMINAL MFM_FONT
SHOT=$SMOKE_DIR/ui.png
MFM_PID=
: >"$MFM_LOG"

BG=aeb2c3
SEL=7387ad
TROUGH=999dac

# ---- helpers ---------------------------------------------------------------

# file_appears <description> <path>
file_appears() { wait_for "$1" test -e "$2"; }

# Top-left corner of the window's content, on the screen, in CX and CY.
content_origin() {
	refresh
	CX=$(($(get mfm x) + B))
	CY=$(($(get mfm y) + T + B))
}

# mfm_start <directory>: run mfm there and wait until its window is up.
mfm_start() {
	# The marker holds the path of what was opened.
	WAYLAND_DEBUG=${MFM_DEBUG:-} WAYLAND_DISPLAY=wayland-0 \
		MFM_OPEN="sh -c 'printf %s \"\$1\" > \"\$0\"' $SMOKE_DIR/opened" \
		MFM_TERMINAL="touch $SMOKE_DIR/term-ran" "$MFM" "$1" >>"$MFM_LOG" 2>&1 &
	MFM_PID=$!
	check "mfm has a focused window" '[ "$(get mfm kbfocus)" = 1 ]'
	content_origin
	wait_shot_pixel "the window is drawn" $((CX + 1)) $((CY + 1)) "$BG"
	find_rows
}

# mfm_quit: Ctrl+Q must end it with status 0.
mfm_quit() {
	do_input key ctrl+q
	wait "$MFM_PID"
	local status=$?
	[ "$status" = 0 ] || fail "mfm exited with status $status"
	check "the window is gone" '[ -z "$(get mfm w)" ]'
	MFM_PID=
}

shoot() { rm -f "$SHOT"; WAYLAND_DISPLAY=wayland-0 grim "$SHOT" 2>/dev/null || fail "no screenshot"; }

pixel() { build/pngtool pixel "$SHOT" "$1" "$2"; }

# wait_shot_pixel <description> <x> <y> <rrggbb>: screen coordinates.
wait_shot_pixel() { pixel_is "$1" "$2" "$3" "$4"; }

# Find the selected row, which starts as the first one: ROW0 is its top on the
# screen, ROWH its height, LISTX a column inside the list.
find_rows() {
	local y end
	LISTX=$((CX + 80))
	shoot
	for ((y = CY; y < CY + 200; y++)); do
		[ "$(pixel "$LISTX" "$y")" = "$SEL" ] && break
	done
	[ "$y" -lt $((CY + 200)) ] || fail "no selected row found"
	ROW0=$y
	for ((end = y; end < y + 60; end++)); do
		[ "$(pixel "$LISTX" "$end")" = "$SEL" ] || break
	done
	ROWH=$((end - y))
	[ "$ROWH" -ge 10 ] && [ "$ROWH" -lt 50 ] || fail "odd row height $ROWH"
	echo "ok: rows start at $ROW0, $ROWH high"
}

row_y() { echo $((ROW0 + $1 * ROWH + ROWH / 2)); }

# Make a directory with entries for one scenario.
fresh_dir() {
	D=$SMOKE_DIR/tree/$1
	mkdir -p "$D"
}

