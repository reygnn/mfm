#!/bin/bash
# Black-box test of mfm under a headless mwc: simulated mouse and keyboard, checked
# through the file system (a command that worked leaves files behind) and through
# pixels in screenshots. See common.sh for what it needs. Output goes to build/ui.log,
# and mfm's own to build/ui-mfm.log; MFM_DEBUG=1 adds its Wayland protocol there.
LOG=$(cd "$(dirname "$0")/.." && pwd)/build/ui.log
MFM_LOG=${LOG%.log}-mfm.log
. "$(dirname "$0")/common.sh"

# The socket comes before the test hooks, the startup command after them.
start_mwc 'touch "$SMOKE_DIR/up"; exec sleep 600'
wait_for "mwc is up" test -e "$SMOKE_DIR/up"
start_vinput
refresh
B=$(global theme border)
T=$(global theme title)
echo "theme: border=$B title=$T"

LIST=c6c9d5

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
file_appears "Shift+a and Enter open something" "$SMOKE_DIR/opened"
[ "$(cat "$SMOKE_DIR/opened")" = "$D/alpha.txt" ] || fail "opened $(cat "$SMOKE_DIR/opened")"
echo "ok: and that is alpha.txt"
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

# A click on our own window closes it and does nothing else there: the row above
# the menu does not get selected.
do_input move "$X" "$Y"
do_input click right
sleep 0.4
do_input move "$LISTX" "$(row_y 0)"
do_input click left
sleep 0.3
shoot
[ "$(pixel "$LISTX" "$ROW0")" != "$SEL" ] || fail "the click selected a row"
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

# A newline in a name must not make a second line, which would show on the empty
# rows below it.
fresh_dir odd
touch "$D/$(printf 'first\nsecond\nthird')"
mfm_start "$D"
do_input key F5
sleep 0.3
kill -0 "$MFM_PID" 2>/dev/null || fail "mfm died on odd names"
shoot
for ((x = LISTX - 60; x < LISTX + 100; x += 3)); do
	[ "$(pixel "$x" "$(row_y 2)")" = "$LIST" ] || fail "text on the empty row at x=$x"
done
echo "ok: a newline stays on its line"
mfm_quit

fresh_dir odd2
touch "$D/$(printf 'latin\xe9')" "$D/ütf ☃ 日本語.txt" "$D/$(printf 'zz\xe2\x80\xaefdp.exe')"
mfm_start "$D"
do_input key F5
sleep 0.3
kill -0 "$MFM_PID" 2>/dev/null || fail "mfm died on odd names"
echo "ok: odd names"
mfm_quit

# The cursor does not stop between a letter and its accent: F2 puts it behind the
# stem, Left in front of the e with its accent, so Backspace takes the f.
fresh_dir comb
touch "$D/$(printf 'cafe\xcc\x81.txt')"
mfm_start "$D"
do_input key Down
do_input key F2
do_input key Left
do_input key BackSpace
do_input key Return
file_appears "Left steps over a combining accent" "$D/$(printf 'cae\xcc\x81.txt')"
mfm_quit

# Ctrl+1/2/3 sort by name, size and date, the same key again reverses; folders
# stay on top (the last row would be the folder in the reversed size order
# otherwise). vinput has no End: up to "..", then down to the fifth row.
fresh_dir sort
mkdir "$D/folder"
head -c 300 /dev/zero >"$D/a.txt"; touch -d 2020-01-01 "$D/a.txt"
head -c 100 /dev/zero >"$D/b.txt"; touch -d 2024-01-01 "$D/b.txt"
head -c 200 /dev/zero >"$D/c.txt"; touch -d 2022-01-01 "$D/c.txt"
mfm_start "$D"
sort_rename() {
	do_input key "$1"
	for _ in 1 2 3 4; do do_input key Up; done
	for _ in 1 2 3 4; do do_input key Down; done
	do_input key F2
	do_input key "$2"
	do_input key Return
	file_appears "$3" "$D/$2.txt"
}
sort_rename ctrl+2 s "by size, the smallest last"
sort_rename ctrl+2 l "by size reversed, the largest last"
sort_rename ctrl+3 o "by date, the oldest last"
sort_rename ctrl+3 n "by date reversed, the newest last"
sort_rename ctrl+1 z "back by name"
[ -d "$D/folder" ] || fail "the folder was renamed"
mfm_quit

# Ctrl+S runs MFM_SEND once, with the full paths of the marked entries, else of
# the selected one. (Without MFM_SEND the menu leaves Send out: the trash test
# above counts on that.)
fresh_dir send
touch "$D/a.txt" "$D/b c.txt" "$D/d.txt"
SENT=$SMOKE_DIR/sent
MFM_SEND="sh -c 'printf \"%s\\n\" \"\$@\" > \"\$0.tmp\" && mv \"\$0.tmp\" \"\$0\"' $SENT" \
	mfm_start "$D"
do_input key Down
do_input key space
do_input key space
do_input key ctrl+s
file_appears "Ctrl+S sends the marked entries" "$SENT"
[ "$(cat "$SENT")" = "$(printf '%s\n' "$D/a.txt" "$D/b c.txt")" ] ||
	fail "MFM_SEND got: $(cat "$SENT")"
rm "$SENT"
# The marks are gone after sending, so Ctrl+S takes the selection: d.txt.
do_input key ctrl+s
file_appears "then the selected one" "$SENT"
[ "$(cat "$SENT")" = "$D/d.txt" ] || fail "MFM_SEND got: $(cat "$SENT")"
rm "$SENT"
# Through the menu: Open Cut Copy Send (Paste is off with nothing to paste).
do_input move $((LISTX + 40)) "$(row_y 1)"
do_input click right
sleep 0.4
for _ in 1 2 3 4; do do_input key Down; done
do_input key Return
file_appears "the menu sends the clicked entry" "$SENT"
[ "$(cat "$SENT")" = "$D/a.txt" ] || fail "MFM_SEND got: $(cat "$SENT")"
mfm_quit

# ---- settings --------------------------------------------------------------

# A font name without a size has no metrics of its own: find_rows checks the height.
fresh_dir font
MFM_FONT=Monospace mfm_start "$D"
mfm_quit
echo "ok: a font without a size"

# mfm has to read mwmrc as mwc does.
fresh_dir rc
mkdir -p "$XDG_CONFIG_HOME/mwc"
printf 'mwm.Background : #336699\n' >"$XDG_CONFIG_HOME/mwc/mwmrc"
BG=336699
mfm_start "$D"
BG=aeb2c3
rm "$XDG_CONFIG_HOME/mwc/mwmrc"
echo "ok: mwmrc in another spelling"
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
