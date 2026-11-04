#!/bin/bash
# Black-box test of mfm under a headless mwc: simulated mouse and keyboard from
# mwc's tests/vinput, checked through the file system (a command that worked
# leaves files behind) and through pixels in screenshots.
#
# Needs a built mwc with build/vinput and build/pngtool in MWC_DIR (default
# ../mwm), and grim. It has its own runtime directory and Wayland socket, so it
# does not touch a running session. Output goes to build/ui.log, and mfm's own
# to build/ui-mfm.log; MFM_DEBUG=1 adds its Wayland protocol there.
MFM_DIR=$(cd "$(dirname "$0")/.." && pwd)
MFM=$MFM_DIR/mfm
MWC_DIR=${MWC_DIR:-$MFM_DIR/../mwm}
LOG=$MFM_DIR/build/ui.log
MFM_LOG=$MFM_DIR/build/ui-mfm.log

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

start_mwc 'exec sleep 600'
start_vinput
refresh
B=$(global theme border)
T=$(global theme title)
echo "theme: border=$B title=$T"

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
	WAYLAND_DEBUG=${MFM_DEBUG:-} WAYLAND_DISPLAY=wayland-0 MFM_OPEN="touch $SMOKE_DIR/opened" \
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

shoot() { WAYLAND_DISPLAY=wayland-0 grim "$SHOT" 2>/dev/null; }

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

# ---- rendering and frame ---------------------------------------------------

fresh_dir basics
touch "$D/alpha.txt" "$D/beta.txt" "$D/run.sh"
mkdir "$D/folder"
mfm_start "$D"
check "mwc gave it a frame" '[ "$(get mfm frame)" = 1 ]'
check "it has the default size" '[ "$(get mfm w)" = 680 ] && [ "$(get mfm h)" = 480 ]'

# A new size means new buffers, while the compositor may still hold the old ones.
for size in 1 2 3; do
	do_input key super+Up
	check "maximized" '[ "$(get mfm max)" = 1 ]'
	content_origin
	wait_shot_pixel "drawn at the new size" $((CX + $(get mfm w) - 2)) $((CY + $(get mfm h) - 2)) "$BG"
	do_input key super+Down
	check "back to the old size" '[ "$(get mfm w)" = 680 ]'
done
content_origin
wait_shot_pixel "drawn at the old size" $((CX + 678)) $((CY + 478)) "$BG"

# ---- keyboard --------------------------------------------------------------

do_input key F7
for k in n e u; do do_input key "$k"; done
do_input key Return
file_appears "F7 and typing make a folder" "$D/neu"
[ -d "$D/neu" ] || fail "neu is not a directory"

do_input key b
do_input key e
do_input key F2
do_input key x
do_input key Return
file_appears "type-ahead and F2 rename, keeping the extension" "$D/x.txt"
[ ! -e "$D/beta.txt" ] || fail "beta.txt still there"

do_input key ctrl+c
do_input key ctrl+v
file_appears "Ctrl+C, Ctrl+V duplicates in a child process" "$D/x (copy).txt"

do_input key ctrl+t
file_appears "Ctrl+T runs the terminal" "$SMOKE_DIR/term-ran"

# Shift and a letter: type-ahead folds case, the keys must still reach it.
sleep 1.1
do_input hold shift
do_input key a
do_input release shift
do_input key Return
file_appears "Shift+a finds alpha.txt, Enter opens it" "$SMOKE_DIR/opened"
rm -f "$SMOKE_DIR/opened"

mfm_quit

# ---- context menu ----------------------------------------------------------

fresh_dir menu
touch "$D/one.txt"
mfm_start "$D"
Y=$(row_y 1)
X=$((LISTX + 40))

# Press, drag, release. Open is the first item, whatever the font's size.
# A compositor that lets the pointer change surfaces while a button is down
# loses the release (wlroots forgets the button): the menu then stays up and a
# click does it.
do_input move "$X" "$Y"
do_input down right
sleep 0.2       # the popup needs its time, and a quicker release is a click
do_input move $((X + 30)) $((Y + 12))
sleep 0.2
do_input up right
sleep 0.3
if [ -e "$SMOKE_DIR/opened" ]; then
	echo "ok: press, drag and release runs Open"
else
	echo "note: the compositor lost the release, the menu stayed up"
	do_input click left
	file_appears "after a lost release a click runs Open" "$SMOKE_DIR/opened"
fi
rm -f "$SMOKE_DIR/opened"

# A menu that is still up would take this key: the menu must be closed now.
do_input key F7
do_input key Escape

# A quick click leaves it open for a second click.
do_input move "$X" "$Y"
do_input click right
sleep 0.4
[ ! -e "$SMOKE_DIR/opened" ] || fail "the opening click ran an item"
do_input move $((X + 30)) $((Y + 12))
do_input click left
file_appears "click, then click on an item runs it" "$SMOKE_DIR/opened"
rm -f "$SMOKE_DIR/opened"

# Slow press and release without moving: the menu stays and nothing runs.
do_input move "$X" "$Y"
do_input down right
sleep 0.5
do_input up right
sleep 0.2
[ ! -e "$SMOKE_DIR/opened" ] || fail "a release on the corner ran an item"
do_input key Down
do_input key Return
file_appears "a slow click leaves it open, Down and Return run Open" "$SMOKE_DIR/opened"
rm -f "$SMOKE_DIR/opened"

# Esc closes it: afterwards the key goes to the list again.
do_input move "$X" "$Y"
do_input click right
sleep 0.4
do_input key Escape
do_input key F7
for k in a b; do do_input key "$k"; done
do_input key Return
file_appears "Esc closes the menu" "$D/ab"

# A click on our own window closes it and does nothing else there.
do_input move "$X" "$Y"
do_input click right
sleep 0.4
do_input move $((CX + 20)) $((CY + 300))
do_input click left
sleep 0.2
do_input key F7
for k in c d; do do_input key "$k"; done
do_input key Return
file_appears "a click beside the menu closes it" "$D/cd"

# The whole way through the keys: Down to Move to trash (Open Cut Copy Rename
# Trash; Paste is off with nothing to paste).
do_input key F5
do_input move "$X" "$(row_y 3)"
do_input click right
sleep 0.4
for _ in 1 2 3 4 5; do do_input key Down; done
do_input key Return
file_appears "the menu moves a file to the trash" "$XDG_DATA_HOME/Trash/files/one.txt"
[ ! -e "$D/one.txt" ] || fail "one.txt still there"

mfm_quit

# Near the bottom of the screen the compositor flips the menu above the pointer.
fresh_dir flip
touch "$D/f.txt"
mfm_start "$D"
X=$((LISTX + 40))
Y=$((CY + 420))    # the list ends a bit below, then comes the status line
[ $((Y + 300)) -gt 720 ] || fail "the test expects a window near the bottom"
do_input move "$X" "$Y"
do_input click right
sleep 0.4
shoot
for ((y = Y - 100; y < Y - 80; y++)); do
	[ "$(pixel $((X + 6)) "$y")" = "$BG" ] && break
done
[ "$y" -lt $((Y - 80)) ] || fail "no menu above the pointer"
[ "$(pixel $((X + 6)) $((Y + 10)))" != "$BG" ] || fail "the menu is below the pointer"
echo "ok: the menu flips at the bottom of the screen"
do_input key Escape
mfm_quit

# ---- scrollbar -------------------------------------------------------------

fresh_dir many
for i in $(seq 100 299); do touch "$D/file$i"; done
mfm_start "$D"
W=$(get mfm w)
H=$(get mfm h)
SBX=$((CX + W - 6 - 15 / 2))    # PAD, SBW
SBY=$((ROW0 + 6))               # inside the thumb at the top
shoot
[ "$(pixel "$SBX" $((CY + H - 80)))" = "$TROUGH" ] || fail "no trough at the bottom"

# Drag the thumb down: where it was, there is trough now.
do_input move "$SBX" "$SBY"
do_input down left
do_input move "$SBX" $((SBY + 150))
wait_shot_pixel "dragging moves the thumb" "$SBX" "$SBY" "$TROUGH"

# Leave the window with the button down and let go outside: mwc has no implicit
# grab, so mfm never sees that release and must not stay in the drag.
do_input move $((CX + W + 60)) $((SBY + 150))
do_input up left
do_input move "$SBX" $((CY + H - 80))
sleep 0.3
shoot
cp "$SHOT" "$SHOT.1"
do_input move "$SBX" "$SBY"
sleep 0.3
shoot
cmp -s "$SHOT" "$SHOT.1" || fail "still dragging after the button came up outside"
echo "ok: a drag ends when the pointer leaves the window"
mfm_quit

# ---- names the protocol or Pango could trip over ---------------------------

# Latin-1 doubles in length for display, which would make a title longer than
# a Wayland message may be and kill the connection.
fresh_dir long
P=$D
comp=$(printf '\xe9%.0s' $(seq 1 200))
for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18; do P=$P/$comp; done
mkdir -p "$P" || fail "cannot make the long path"
touch "$P/inside"
mfm_start "$P"
do_input key F5
sleep 0.3
kill -0 "$MFM_PID" 2>/dev/null || fail "mfm died on a long path"
echo "ok: a path of ${#P} bytes as a title"
mfm_quit

fresh_dir odd
touch "$D/$(printf 'line\nbreak')" "$D/$(printf 'latin\xe9')" "$D/ütf ☃ 日本語.txt"
mfm_start "$D"
do_input key F5
sleep 0.3
kill -0 "$MFM_PID" 2>/dev/null || fail "mfm died on odd names"
shoot
[ "$(pixel "$LISTX" "$ROW0")" = "$SEL" ] || fail "rows moved: a newline made a second line"
echo "ok: odd names"
mfm_quit

# ---- the end ---------------------------------------------------------------

# Compositor gone: an error message and status 1, not a hang.
fresh_dir gone
mfm_start "$D"
kill "$pid"
wait "$pid" 2>/dev/null
pid=
for ((i = 0; i < 50; i++)); do
	kill -0 "$MFM_PID" 2>/dev/null || break
	sleep 0.1
done
kill -0 "$MFM_PID" 2>/dev/null && fail "mfm still runs without a compositor"
wait "$MFM_PID"
[ $? = 1 ] || fail "mfm did not exit with status 1"
grep -q 'lost the connection' "$MFM_LOG" || fail "no message about the lost connection"
echo "ok: exits when the compositor goes away"

echo "PASS"
