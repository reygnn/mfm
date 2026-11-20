#!/bin/bash
# Black-box test of mfm under a headless mwc: simulated mouse and keyboard, checked
# through the file system (a command that worked leaves files behind) and through
# pixels in screenshots. See common.sh for what it needs. Output goes to build/ui.log,
# and mfm's own to build/ui-mfm.log; MFM_DEBUG=1 adds its Wayland protocol there.
LOG=$(cd "$(dirname "$0")/.." && pwd)/build/ui.log
MFM_LOG=${LOG%.log}-mfm.log
. "$(dirname "$0")/common.sh"

# The cursor runs need the Adwaita cursors: without them wayland-cursor falls back
# to its built-in one and those checks fail with no word why. The places are
# wayland-cursor's own, or XCURSOR_PATH.
have_cursor_theme() {
	local d IFS=:
	for d in ${XCURSOR_PATH:-$HOME/.local/share/icons:$HOME/.icons:/usr/share/icons:/usr/share/pixmaps}; do
		[ -d "$d/$1/cursors" ] && return 0
	done
	return 1
}
have_cursor_theme Adwaita || fail "no Adwaita cursor theme (adwaita-icon-theme)"

# The socket comes before the test hooks, the startup command after them.
start_mwc 'touch "$SMOKE_DIR/up"; exec sleep 600'
wait_for "mwc is up" test -e "$SMOKE_DIR/up"
start_vinput
refresh
B=$(global theme border)
T=$(global theme title)
echo "theme: border=$B title=$T"

LIST=c6c9d5

# ---- command line ----------------------------------------------------------

# -h alone asks for the usage: stdout, status 0. --help, any other option and a
# second argument are refused: the usage on stderr, status 1.
out=$("$MFM" -h 2>/dev/null) && [ "${out#usage: mfm}" != "$out" ] ||
	fail "-h: status $?, stdout '$out'"
for args in --help -x "a b"; do
	# shellcheck disable=SC2086
	out=$("$MFM" $args 2>"$SMOKE_DIR/err")
	status=$?
	[ "$status" = 1 ] && [ -z "$out" ] && grep -q '^usage: mfm' "$SMOKE_DIR/err" ||
		fail "$args: status $status, stdout '$out'"
done
echo "ok: -h on stdout, --help and the rest refused on stderr"
want=$(sed -n 's/^#define MFM_VERSION "\(.*\)"$/mfm \1/p' "$MFM_DIR/version.h")
out=$("$MFM" -v) && [ "$out" = "$want" ] || fail "-v: status $?, '$out', not '$want'"
echo "ok: -v prints $want"
# A write error on stdout, a full disk or a closed stdout, is status 1 with a
# message, not 0 with nothing written.
for opt in -v -h; do
	"$MFM" $opt >/dev/full 2>"$SMOKE_DIR/err"
	status=$?
	[ "$status" = 1 ] && grep -q '^mfm: stdout: ' "$SMOKE_DIR/err" ||
		fail "$opt >/dev/full: status $status, stderr '$(cat "$SMOKE_DIR/err")'"
	"$MFM" $opt >&- 2>"$SMOKE_DIR/err"
	status=$?
	[ "$status" = 1 ] && grep -q '^mfm: stdout: ' "$SMOKE_DIR/err" ||
		fail "$opt >&-: status $status, stderr '$(cat "$SMOKE_DIR/err")'"
done
echo "ok: -v and -h report a stdout that cannot be written"

# ---- rendering and frame ---------------------------------------------------

fresh_dir basics
touch "$D/alpha.txt" "$D/beta.txt" "$D/run.sh"
mkdir "$D/folder"
mfm_start "$D"
check "mwc gave it a frame" '[ "$(get mfm frame)" = 1 ]'
check "it has the default size" '[ "$(get mfm w)" = 680 ] && [ "$(get mfm h)" = 480 ]'
ROWOFF=$((ROW0 - CY))   # for the scaled output at the end

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
end_search
do_input hold shift
do_input key a
do_input release shift
do_input key Return
file_appears "Shift+a and Enter open something" "$SMOKE_DIR/opened"
[ "$(cat "$SMOKE_DIR/opened")" = "$D/alpha.txt" ] || fail "opened $(cat "$SMOKE_DIR/opened")"
echo "ok: and that is alpha.txt"
rm -f "$SMOKE_DIR/opened"

mfm_quit

# A new selection changes a few rows of pixels, and only they go to the compositor
# as damage, not the whole window.
fresh_dir damage
touch "$D/a" "$D/b"
: >"$MFM_LOG"
MFM_DEBUG=client mfm_start "$D"
last_damage_h() {
	grep -o 'damage_buffer(0, [0-9]*, 680, [0-9]*)' "$MFM_LOG" | tail -1 |
		sed 's/.*, \([0-9]*\))/\1/'
}
# How many rectangles the last commit with damage of the window had.
damage_rects() {
	awk '/damage_buffer\(0, [0-9]+, 680, [0-9]+\)/ { n++ }
	/wl_surface#[0-9]+\.commit\(\)/ { if (n) last = n; n = 0 }
	END { print last + 0 }' "$MFM_LOG"
}
do_input key Down
check "a new selection damages only the rows that changed" \
	'h=$(last_damage_h); [ "${h:-480}" -lt 480 ]'
check "... the rows near the top and the status line as two rectangles" \
	'[ "$(damage_rects)" -ge 2 ]'
mfm_quit

# ---- context menu ----------------------------------------------------------

fresh_dir menu
touch "$D/one.txt"
mfm_start "$D"
Y=$(row_y 1)
X=$((LISTX + 40))

# menu_up <x> <y>: the menu opened at x, y is drawn, with its first item a bit below
# and right of the corner.
menu_up() { wait_shot_pixel "the menu is up" $(($1 + 60)) $(($2 + 14)) "$BG"; }
# menu_still_up <what>: a key sent now comes after the release before it, so once
# Down has lit the first item the release is known to have left the menu up. With
# the menu gone Down would go to the list, which has no row below one.txt.
menu_still_up() { do_input key Down; wait_shot_pixel "$1" $((X + 60)) $((Y + 14)) "$SEL"; }

# Press, drag, release. Open is the first item, whatever the font's size. mwc keeps
# the button to the popup's grab, so the release reaches the menu (where it does
# not, a click still runs the item: CAVEATS).
do_input move "$X" "$Y"
do_input down right
menu_up "$X" "$Y"
sleep 0.3       # past MENU_CLICK_MS: a quicker release counts as a click
do_input move $((X + 30)) $((Y + 12))
do_input up right
file_appears "press, drag and release runs Open" "$SMOKE_DIR/opened"
rm -f "$SMOKE_DIR/opened"

# A menu that is still up would take these keys: the menu must be closed now.
do_input key F2
do_input key q
do_input key Return
file_appears "the menu is closed after it ran Open" "$D/q.txt"
do_input key F2
for k in o n e; do do_input key "$k"; done
do_input key Return
file_appears "... and the name is back" "$D/one.txt"

# A quick click leaves it open for a second click.
do_input move "$X" "$Y"
do_input click right
menu_up "$X" "$Y"
menu_still_up "a quick click leaves the menu up"
[ ! -e "$SMOKE_DIR/opened" ] || fail "the opening click ran an item"
do_input move $((X + 30)) $((Y + 12))
do_input click left
file_appears "a click on an item runs it" "$SMOKE_DIR/opened"
rm -f "$SMOKE_DIR/opened"

# Slow press and release without moving: the menu stays and nothing runs.
do_input move "$X" "$Y"
do_input down right
menu_up "$X" "$Y"
sleep 0.3       # past MENU_CLICK_MS
do_input up right
menu_still_up "a slow release on the corner leaves the menu up"
[ ! -e "$SMOKE_DIR/opened" ] || fail "a release on the corner ran an item"
do_input key Return
file_appears "... and Return runs the item Down lit" "$SMOKE_DIR/opened"
rm -f "$SMOKE_DIR/opened"

# Esc closes it: afterwards the key goes to the list again.
do_input move "$X" "$Y"
do_input click right
menu_up "$X" "$Y"
do_input key Escape
do_input key F7
for k in a b; do do_input key "$k"; done
do_input key Return
file_appears "Esc closes the menu" "$D/ab"

# A click on our own window closes it and does nothing else there: the row above
# the menu does not get selected.
do_input move "$X" "$Y"
do_input click right
menu_up "$X" "$Y"
do_input move "$LISTX" "$(row_y 0)"
do_input click left
do_input key F2
for k in z z; do do_input key "$k"; done
do_input key Return
file_appears "a click beside the menu closes it, ab is still selected" "$D/zz"
do_input key F7
for k in c d; do do_input key "$k"; done
do_input key Return
file_appears "... and the keys go to the list" "$D/cd"

# The whole way through the keys: Down to Move to trash (Open Cut Copy
# "Copy to clipboard" Rename Trash; Paste is off with nothing to paste).
do_input key F5
do_input move "$X" "$(row_y 3)"
do_input click right
menu_up "$X" "$(row_y 3)"
for _ in 1 2 3 4 5 6; do do_input key Down; done
do_input key Return
wait_for "the menu moves a file to the trash" trashed_from /menu/one.txt
[ ! -e "$D/one.txt" ] || fail "one.txt still there"

mfm_quit

# Near the bottom of the screen the compositor flips the menu above the pointer.
fresh_dir flip
touch "$D/f.txt"
mfm_start "$D"
X=$((LISTX + 40))
Y=$((CY + 420))    # the list ends a bit below, then comes the status line
[ $((Y + 300)) -gt 720 ] || fail "the test expects a window near the bottom"
menu_above() {
	local y
	shoot
	for ((y = Y - 100; y < Y - 80; y++)); do
		[ "$(pixel $((X + 6)) "$y")" = "$BG" ] && return 0
	done
	return 1
}
do_input move "$X" "$Y"
do_input click right
check "the menu is up, above the pointer" menu_above
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

# Leave the window with the button down and let go outside: the drag must end,
# whether the release arrives (mwc keeps the pointer with the window while a button
# is down) or not (ptr_leave). Coming back in is an enter, and the move after it a
# motion that would take the thumb to the bottom if the drag went on. F7's prompt
# shows once that motion is handled.
do_input move $((CX + W + 60)) $((SBY + 150))
do_input up left
do_input move "$SBX" $((CY + H - 70))
do_input move "$SBX" $((CY + H - 80))
do_input key F7
wait_shot_pixel "the prompt is up" $((CX + W - 40)) $((CY + H - 19)) f6f6f6
shoot
at=$(pixel "$SBX" $((SBY + 150)))
bottom=$(pixel "$SBX" $((CY + H - 80)))
[ "$at" = "$BG" ] && [ "$bottom" = "$TROUGH" ] ||
	fail "still dragging after the button came up outside: thumb $at, bottom $bottom"
echo "ok: a drag ends when the pointer leaves the window"
do_input key Escape
mfm_quit

# ---- refusals, and what is left as it was ----------------------------------

# A move within a file system is a rename that never replaces: what has the name in
# the target stays, the moved one arrives as "name (2)".
fresh_dir samefs
mkdir -p "$D/src/d" "$D/dst"
echo new >"$D/src/a.txt"
echo f >"$D/src/d/f"
echo old >"$D/dst/a.txt"
mfm_start "$D/src"
do_input key ctrl+a
do_input key ctrl+x
do_input key BackSpace
do_input key d
do_input key Return
do_input key ctrl+v
file_appears "a move within a file system" "$D/dst/a (2).txt"
file_appears "... of a folder too" "$D/dst/d/f"
[ "$(cat "$D/dst/a.txt")" = old ] || fail "the move replaced a.txt"
[ "$(cat "$D/dst/a (2).txt")" = new ] || fail "a (2).txt is not the moved file"
[ -z "$(ls -A "$D/src")" ] || fail "left in the source: $(ls -A "$D/src")"
echo "ok: ... keeps what was there and leaves nothing behind"
mfm_quit

# F2 onto a taken name, and onto another name of the same file, is refused.
fresh_dir clash
echo a >"$D/a.txt"
echo b >"$D/b.txt"
ln "$D/a.txt" "$D/c.txt"
mfm_start "$D"
do_input key Down
do_input key F2
do_input key b
do_input key Return
wait_err "F2 onto a taken name is refused"
[ "$(cat "$D/a.txt")" = a ] && [ "$(cat "$D/b.txt")" = b ] || fail "the rename replaced b.txt"
do_input move $((LISTX + 40)) "$(row_y 1)"
do_input click left
no_err "a click clears the error"
do_input key F2
do_input key c
do_input key Return
wait_err "F2 onto a hard link of the same file is refused"
[ -e "$D/a.txt" ] && [ -e "$D/c.txt" ] || fail "a name of the hard-linked file is gone"
mfm_quit

# Shift+Delete asks; any key but y leaves the file.
fresh_dir ask
touch "$D/a.txt"
mfm_start "$D"
do_input key Down
do_input key shift+Delete
do_input key n
mfm_alive "the answer is taken"
[ -e "$D/a.txt" ] || fail "n deleted the file"
echo "ok: Shift+Delete, then n, deletes nothing"
mfm_quit

# A folder cannot be pasted into itself or below itself.
fresh_dir self
mkdir -p "$D/d/sub"
mfm_start "$D"
do_input key Down
do_input key ctrl+c
do_input key Return
do_input key Down
do_input key Return
do_input key ctrl+v
wait_err "pasting a folder into its own subfolder is refused"
mfm_alive "after the refusal"
[ -z "$(ls -A "$D/d/sub")" ] || fail "made in the subfolder: $(ls -A "$D/d/sub")"
echo "ok: ... and nothing is made there"
mfm_quit

# F7 and F2 in a folder that cannot be written, and F7 onto a taken name.
fresh_dir ro
mkdir "$D/ro"
touch "$D/ro/f" "$D/taken"
chmod 555 "$D/ro"
mfm_start "$D/ro"
do_input key F7
do_input key x
do_input key Return
wait_err "F7 in a read-only folder fails"
[ ! -e "$D/ro/x" ] || fail "F7 made a folder in a read-only one"
do_input move $((LISTX + 40)) "$(row_y 1)"
do_input click left
no_err "a click clears the error"
do_input key F2
do_input key y
do_input key Return
wait_err "F2 in a read-only folder fails"
mfm_alive "... and mfm goes on"
[ -e "$D/ro/f" ] || fail "f is gone"
chmod 755 "$D/ro"
do_input key BackSpace
no_err "going up clears the error"
do_input key F7
for k in t a k e n; do do_input key "$k"; done
do_input key Return
wait_err "F7 onto a taken name fails"
[ -f "$D/taken" ] || fail "taken is no longer the file"
mfm_quit

# Two files of one name from two folders: both go to the trash, under two names, each
# with its own path in its .trashinfo.
fresh_dir trash2
mkdir "$D/p" "$D/q"
echo p >"$D/p/same"
echo q >"$D/q/same"
mfm_start "$D/p"
do_input key Down
do_input key Delete
wait_for "the first goes to the trash" test ! -e "$D/p/same"
do_input key BackSpace
do_input key q
do_input key Return
do_input key Down
do_input key Delete
wait_for "the second goes to the trash" test ! -e "$D/q/same"
TR=$XDG_DATA_HOME/Trash
for w in p q; do
	info=$(grep -l "^Path=.*/$w/same$" "$TR"/info/same*.trashinfo) ||
		fail "no .trashinfo for $w/same"
	[ "$(echo "$info" | wc -l)" = 1 ] || fail "two .trashinfo for $w/same: $info"
	f=$(basename "$info" .trashinfo)
	[ "$(cat "$TR/files/$f")" = "$w" ] || fail "$f in the trash is not $w/same"
done
echo "ok: two files of one name are both in the trash, each with its path"
mfm_quit

# Given a file, mfm opens its folder with the file selected: F2 renames it.
fresh_dir argfile
touch "$D/a.txt" "$D/b.txt"
mfm_start "$D/b.txt"
do_input key F2
do_input key z
do_input key Return
file_appears "a file as argument is selected" "$D/z.txt"
[ -e "$D/a.txt" ] || fail "a.txt was renamed"
mfm_quit

# A path that does not exist: mfm starts at home and says why.
fresh_dir argmissing
mkdir "$D/home"
HOME=$D/home mfm_start "$D/nope"
wait_err "a missing path is an error"
do_input key F7
do_input key h
do_input key Return
file_appears "... and mfm is at home" "$D/home/h"
mfm_quit

# A program for Return or Ctrl+S that cannot be run is an error, not the end of mfm.
fresh_dir noprog
touch "$D/a.txt"
OPEN=/nonexistent/open MFM_SEND=/nonexistent/send mfm_start "$D"
do_input key Down
do_input key Return
wait_err "an MFM_OPEN that cannot run is an error"
do_input move $((LISTX + 40)) "$(row_y 1)"
do_input click left
no_err "a click clears the error"
do_input key ctrl+s
wait_err "an MFM_SEND that cannot run is an error"
mfm_alive "... and mfm goes on"
mfm_quit

# The path field, Ctrl+L: Return goes where it says; a file there is opened, a path
# that does not exist is an error. Before a key that moves the cursor, "/" starts over.
fresh_dir path
mkdir -p "$D/a/b"
touch "$D/a/f"
mfm_start "$D/a/b"
do_input key ctrl+l
do_input key BackSpace
do_input key BackSpace
do_input key Return
do_input key F7
do_input key n
do_input key Return
file_appears "the path field takes a path" "$D/a/n"
do_input key ctrl+l
do_input key End
for k in slash z z; do do_input key "$k"; done
do_input key Return
wait_err "a path that does not exist is an error"
do_input key ctrl+l
do_input key End
for k in slash f; do do_input key "$k"; done
do_input key Return
file_appears "a file in the path field is opened" "$SMOKE_DIR/opened"
[ "$(cat "$SMOKE_DIR/opened")" = "$D/a/f" ] || fail "opened $(cat "$SMOKE_DIR/opened")"
rm -f "$SMOKE_DIR/opened"
mfm_quit

# A name that is not UTF-8 is copied and moved byte for byte, "(copy)" after it.
# The keys after a job wait for its end: then it selects what it made.
fresh_dir latin
mkdir "$D/to"
L=$(printf 'caf\xe9')
echo x >"$D/$L"
mfm_start "$D"
do_input key End
do_input key ctrl+c
do_input key ctrl+v
file_appears "a Latin-1 name is copied" "$D/$L (copy)"
job_settled
mfm_alive "the copy is over"
do_input key Home
do_input key Down
do_input key Down
do_input key ctrl+x
do_input key Home
do_input key Down
do_input key Return
do_input key ctrl+v
file_appears "... and moved" "$D/to/$L"
[ ! -e "$D/$L" ] || fail "the original is still there"
mfm_quit

# Shift+click marks the range from the selection; Ctrl+A marks all, and again none.
fresh_dir range
touch "$D/a" "$D/b" "$D/c" "$D/d"
mfm_start "$D"
do_input move $((LISTX + 40)) "$(row_y 1)"
do_input click left
do_input hold shift
do_input move $((LISTX + 40)) "$(row_y 3)"
do_input click left
do_input release shift
do_input key Delete
wait_for "Shift+click marks the range" test ! -e "$D/c"
[ ! -e "$D/a" ] && [ ! -e "$D/b" ] || fail "the range missed its start"
[ -e "$D/d" ] || fail "the range went past its end"
# With nothing marked, Delete on ".." has nothing to take.
do_input key ctrl+a
do_input key ctrl+a
do_input key Home
do_input key Delete
mfm_alive "Ctrl+A twice, then Delete on .."
[ -e "$D/d" ] || fail "Ctrl+A twice left d marked"
echo "ok: Ctrl+A again unmarks"
mfm_quit

# ---- keys and settings -------------------------------------------------------

# Ctrl+T without MFM_TERMINAL takes mwc's terminal, a command for sh as mwc runs it:
# MWC_TERMINAL, else Mwm*terminal from mwmrc.
fresh_dir term
MFMT_TERMINAL= MWC_TERMINAL="X=1 touch '$SMOKE_DIR/mwc term'" mfm_start "$D"
do_input key ctrl+t
file_appears "MWC_TERMINAL runs with sh -c" "$SMOKE_DIR/mwc term"
mfm_quit
mkdir -p "$XDG_CONFIG_HOME/mwc"
printf 'Mwm*terminal: touch %s/mwmt\n' "$SMOKE_DIR" >"$XDG_CONFIG_HOME/mwc/mwmrc"
MFMT_TERMINAL= mfm_start "$D"
do_input key ctrl+t
file_appears "... and Mwm*terminal after it" "$SMOKE_DIR/mwmt"
mfm_quit
rm "$XDG_CONFIG_HOME/mwc/mwmrc"

# That sh looks its command up only in the absolute entries of $PATH: a program of
# that name in the folder shown is not it, though $PATH has "." and an empty entry.
fresh_dir termpath
printf '#!/bin/sh\ntouch "%s/wrong-term"\n' "$SMOKE_DIR" >"$D/mfmt-term"
chmod +x "$D/mfmt-term"
PATH=".::$PATH" MFMT_TERMINAL= \
	MWC_TERMINAL="mfmt-term; touch '$SMOKE_DIR/sh-done'" mfm_start "$D"
do_input key ctrl+t
file_appears "the shell ran" "$SMOKE_DIR/sh-done"
[ ! -e "$SMOKE_DIR/wrong-term" ] || fail "the terminal was taken from the folder shown"
echo "ok: ... not from the folder shown"
mfm_quit

# Up and Down repeat when held.
fresh_dir repeat
for i in $(seq 10 39); do touch "$D/f$i"; done
mfm_start "$D"
do_input press Down
sleep 1.2       # past the repeat delay
do_input lift Down
do_input key F2
do_input key z
do_input key Return
file_appears "Down held moves on" "$D/z"
[ -e "$D/f10" ] && [ -e "$D/f11" ] || fail "Down held moved one row"
echo "ok: ... several rows"
mfm_quit

# PgDn goes a page down, PgUp a page up, back to "..".
fresh_dir pages
for i in $(seq 10 59); do touch "$D/f$i"; done
mfm_start "$D"
do_input key Next
do_input key F2
do_input key z
do_input key Return
file_appears "PgDn goes a page down" "$D/z"
[ -e "$D/f10" ] && [ -e "$D/f12" ] || fail "PgDn moved too little"
for _ in 1 2 3 4; do do_input key Prior; done
do_input key F2
wait_err "PgUp goes back up to .., which F2 cannot rename"
mfm_quit

# Esc takes back one thing at a time: the marks, the search, the message.
fresh_dir esc
touch "$D/a" "$D/b"
mfm_start "$D"
do_input hold ctrl
do_input move $((LISTX + 40)) "$(row_y 2)"
do_input click left
do_input release ctrl
do_input key Escape
do_input key Up
do_input key Delete
wait_for "Esc unmarks: Delete takes the selection" test ! -e "$D/a"
[ -e "$D/b" ] || fail "Esc left b marked"
do_input key b
do_input key Escape
do_input key BackSpace
do_input key F7
for k in e s c n; do do_input key "$k"; done
do_input key Return
file_appears "Esc ends the search: BackSpace goes up" "$(dirname "$D")/escn"
do_input key Home
do_input key F2
wait_err "F2 on .. is an error"
do_input key Escape
no_err "Esc clears the message"
mfm_quit

# Shift+Delete takes j as well as y.
fresh_dir jdel
touch "$D/a"
mfm_start "$D"
do_input key Down
do_input key shift+Delete
do_input key j
wait_for "Shift+Delete, then j, deletes" test ! -e "$D/a"
mfm_quit

# Going up skips a folder that can be passed but not listed, and says so.
fresh_dir skip
mkdir -p "$D/a/b"
chmod 111 "$D/a"
mfm_start "$D/a/b"
do_input key BackSpace
wait_err "going up past a folder that cannot be listed says so"
chmod 755 "$D/a"
do_input key F7
for k in s k; do do_input key "$k"; done
do_input key Return
file_appears "... and lands above it" "$D/sk"
mfm_quit

# Alt+Up, Ctrl+Shift+N, Right on a folder, Alt+Home and the parent button.
fresh_dir nav
mkdir -p "$D/home" "$D/a/b"
HOME=$D/home mfm_start "$D/a/b"
do_input key alt+Up
do_input key ctrl+shift+n
do_input key x
do_input key Return
file_appears "Alt+Up goes up, Ctrl+Shift+N makes a folder" "$D/a/x"
do_input key Home
do_input key Down
do_input key Right
do_input key F7
do_input key r
do_input key Return
file_appears "Right opens a folder" "$D/a/b/r"
do_input key alt+Home
do_input key F7
do_input key h
do_input key Return
file_appears "Alt+Home goes home" "$D/home/h"
do_input move $((CX + 6 + (ROWH + 6) / 2)) $((CY + 6 + (ROWH + 6) / 2))
do_input click left
do_input key F7
do_input key p
do_input key Return
file_appears "the parent button goes up" "$D/p"
mfm_quit

# A double click opens; Ctrl+W quits.
fresh_dir dclick
touch "$D/f.txt"
mfm_start "$D"
rm -f "$SMOKE_DIR/opened"
do_input move $((LISTX + 40)) "$(row_y 1)"
do_input click left
do_input click left
file_appears "a double click opens" "$SMOKE_DIR/opened"
[ "$(cat "$SMOKE_DIR/opened")" = "$D/f.txt" ] || fail "opened $(cat "$SMOKE_DIR/opened")"
rm -f "$SMOKE_DIR/opened"
do_input key ctrl+w
wait "$MFM_PID"
status=$?
[ "$status" = 0 ] || fail "Ctrl+W: mfm exited with status $status"
check "Ctrl+W quits" '[ -z "$(get mfm w)" ]'
MFM_PID=

# Ctrl+R reads the folder again, also when its time says it did not change.
fresh_dir reread
touch "$D/a"
touch -d 2020-01-01 "$D"
mfm_start "$D"
touch "$D/zz"
touch -d 2020-01-01 "$D"
do_input key ctrl+r
do_input key z
do_input key z
do_input key F2
do_input key q
do_input key Return
file_appears "Ctrl+R shows what came" "$D/q"
mfm_quit

# Esc in F2 renames nothing; Ctrl+U empties the field.
fresh_dir prompt
touch "$D/a.txt"
mfm_start "$D"
do_input key Down
do_input key F2
do_input key x
do_input key Escape
mfm_alive "Esc closes the prompt"
[ -e "$D/a.txt" ] && [ ! -e "$D/x.txt" ] || fail "Esc renamed"
do_input key F2
do_input key End
do_input key ctrl+u
for k in n e w; do do_input key "$k"; done
do_input key Return
file_appears "Ctrl+U empties the field" "$D/new"
mfm_quit

# Mfm*sortReverse in mfmrc, and words for values that mean nothing.
fresh_dir rcsort
touch "$D/a" "$D/b"
mkdir -p "$XDG_CONFIG_HOME/mfm"
printf 'Mfm*sortReverse: true\nMfm*showHidden: ture\n' >"$XDG_CONFIG_HOME/mfm/mfmrc"
mfm_start "$D"
do_input key Down
do_input key F2
do_input key z
do_input key Return
file_appears "Mfm*sortReverse puts b first" "$D/z"
[ -e "$D/a" ] || fail "a came first"
grep -qF 'ignoring Mfm*showHidden "ture"' "$MFM_LOG" || fail "no word about showHidden: ture"
echo "ok: ... and a boolean that means nothing is said so"
mfm_quit
printf 'Mfm*sort: sideways\nMfm*sortReverse:\n' >"$XDG_CONFIG_HOME/mfm/mfmrc"
mfm_start "$D"
grep -qF 'ignoring Mfm*sort "sideways"' "$MFM_LOG" || fail "no word about sort: sideways"
! grep -qF 'ignoring Mfm*sortReverse' "$MFM_LOG" || fail "a word about an empty value"
do_input key Down
do_input key F2
do_input key y
do_input key Return
file_appears "a bad Mfm*sort sorts by name" "$D/y"
[ -e "$D/z" ] || fail "z came first"
mfm_quit
rm "$XDG_CONFIG_HOME/mfm/mfmrc"

# A folder whose name starts with "-" as ./-name.
fresh_dir dash
mkdir "$D/-x"
(cd "$D" && exec env WAYLAND_DISPLAY=wayland-0 MFM_OPEN="$OPENER" \
	MFM_TERMINAL="touch $SMOKE_DIR/term-ran" "$MFM" ./-x) >>"$MFM_LOG" 2>&1 &
MFM_PID=$!
mfm_up
do_input key F7
do_input key n
do_input key Return
file_appears "mfm ./-x opens it" "$D/-x/n"
mfm_quit

# What is in the trash is not trashed again.
fresh_dir intrash
touch "$D/trashme"
mfm_start "$D"
do_input key Down
do_input key Delete
wait_for "trashme goes to the trash" test ! -e "$D/trashme"
mfm_quit
mfm_start "$XDG_DATA_HOME/Trash/files"
for k in t r a s h m e; do do_input key "$k"; done
rm -f "$SMOKE_DIR/opened"
do_input key Return
file_appears "trashme is selected in the trash" "$SMOKE_DIR/opened"
[ "$(cat "$SMOKE_DIR/opened")" = "$XDG_DATA_HOME/Trash/files/trashme" ] ||
	fail "opened $(cat "$SMOKE_DIR/opened")"
rm -f "$SMOKE_DIR/opened"
do_input key Delete
wait_err "Delete in the trash is refused"
[ -e "$XDG_DATA_HOME/Trash/files/trashme" ] || fail "trashme left the trash"
mfm_quit

# ---- names that hide or run together ----------------------------------------

# A soft hyphen does not show: it shows as U+FFFD, so a<SHY>b does not look like ab.
fresh_dir invisible
touch "$D/ab" "$D/$(printf 'a\xc2\xadb')" "$D/$(printf 'a\xe2\x80\x8db')"
MFM_FONT="DejaVu Sans Bold 24" mfm_start "$D"
ink() {
	build/pngtool count "$SHOT" $((CX + 20)) $((ROW0 + $1 * ROWH + 1)) 300 $((ROWH - 2)) \
		000000
}
[ "$(ink 2)" -gt "$(ink 1)" ] ||
	fail "a soft hyphen hides: the rows have $(ink 1) and $(ink 2) dark pixels"
echo "ok: a soft hyphen shows"
[ "$(ink 3)" -gt "$(ink 1)" ] ||
	fail "a joiner between letters hides: the rows have $(ink 1) and $(ink 3) dark pixels"
echo "ok: ... and a joiner that joins nothing"
mfm_quit

# A flag is two code points and one character: Backspace takes it whole.
fresh_dir flag
mkdir "$D/x$(printf '\xf0\x9f\x87\xa9\xf0\x9f\x87\xaa')"
mfm_start "$D"
do_input key Down
do_input key F2
do_input key End
do_input key BackSpace
do_input key Return
file_appears "Backspace takes a flag whole" "$D/x"
mfm_quit

# ... also when a byte elsewhere in the name is not UTF-8.
fresh_dir flag2
mkdir "$D/$(printf '\xe9x\xf0\x9f\x87\xa9\xf0\x9f\x87\xaa')"
mfm_start "$D"
do_input key Down
do_input key F2
do_input key End
do_input key BackSpace
do_input key Return
file_appears "... next to a Latin-1 byte too" "$D/$(printf '\xe9x')"
mfm_quit

# A long name in Latin-1: bytes from 0x80 to 0xbf are letters there, and the stem
# of a copy keeps as many as fit.
fresh_dir latinlong
L=$(printf '\xb0%.0s' $(seq 1 255))
K=$(printf '\xb0%.0s' $(seq 1 248))
echo x >"$D/$L"
mfm_start "$D"
do_input key Down
do_input key ctrl+c
do_input key ctrl+v
file_appears "a long Latin-1 name keeps its stem in a copy" "$D/$K (copy)"
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
mfm_alive "a path of ${#P} bytes as a title"
mfm_quit

# A newline in a name must not make a second line, which would show on the empty
# rows below it.
fresh_dir odd
touch "$D/$(printf 'first\nsecond\nthird')"
mfm_start "$D"
do_input key F5
mfm_alive "mfm survives a newline in a name"
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
mfm_alive "odd names"
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

# Delete and Backspace take a letter with its accent, as Left and Right step over it.
fresh_dir comb2
mkdir "$D/$(printf 'e\xcc\x81x')" "$D/$(printf 'ye\xcc\x81')"
mfm_start "$D"
do_input key Down
do_input key F2
do_input key Home
do_input key Delete
do_input key Return
file_appears "Delete takes the e with its accent" "$D/x"
do_input key Down
do_input key F2
do_input key End
do_input key BackSpace
do_input key Return
file_appears "Backspace takes the e with its accent" "$D/y"
mfm_quit

# The same for the marks of other scripts: a Devanagari ka with its virama.
fresh_dir comb3
mkdir "$D/$(printf 'z\xe0\xa4\x95\xe0\xa5\x8d')"
mfm_start "$D"
do_input key Down
do_input key F2
do_input key End
do_input key BackSpace
do_input key Return
file_appears "Backspace takes a letter with a mark of another script" "$D/z"
mfm_quit

# A new name that is hidden shows hidden files, and stays selected: F2 again renames
# it, not a neighbour.
fresh_dir dotname
touch "$D/a.txt" "$D/b.txt"
mfm_start "$D"
do_input key Down
do_input key F2
do_input key period
do_input key Return
file_appears "F2 to a hidden name" "$D/..txt"
do_input key F2
do_input key x
do_input key Return
file_appears "the hidden name stays selected" "$D/x.txt"
[ -e "$D/b.txt" ] || fail "the neighbour was renamed"
do_input key ctrl+h
do_input key F7
do_input key period
do_input key d
do_input key Return
file_appears "F7 makes a hidden folder" "$D/.d"
do_input key F2
do_input key e
do_input key Return
file_appears "the hidden folder stays selected" "$D/e"
[ -d "$D/e" ] || fail "e is not the folder"
mfm_quit

# The wheel only scrolls: a name being typed and the question before deleting stay.
fresh_dir wheel
touch "$D/a.txt" "$D/b.txt"
mfm_start "$D"
do_input move $((LISTX + 40)) "$(row_y 1)"
do_input key Down
do_input key F2
do_input key 1
do_input wheel 1
do_input key Return
file_appears "F2 survives the wheel" "$D/1.txt"
[ ! -e "$SMOKE_DIR/opened" ] || fail "Return opened the file"
do_input key Down
do_input key shift+Delete
do_input wheel 1
do_input key y
wait_for "Shift+Del survives the wheel" test ! -e "$D/b.txt"
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

# Folders stay A to Z when sorting by size, reversed too; files of one size by name.
fresh_dir sort2
mkdir "$D/fa" "$D/fb"
head -c 100 /dev/zero >"$D/x.txt"
head -c 100 /dev/zero >"$D/y.txt"
mfm_start "$D"
do_input key ctrl+2
do_input key ctrl+2
do_input key Down
do_input key F2
do_input key g
do_input key Return
file_appears "by size reversed, the folders by name" "$D/g"
[ -d "$D/fb" ] || fail "fb came first"
do_input key Down
do_input key F2
do_input key h
do_input key Return
file_appears "... and files of one size too" "$D/h.txt"
[ -e "$D/y.txt" ] || fail "y.txt came first"
mfm_quit

# Holding a key runs a command once: Backspace goes up one folder, Space marks one
# entry.
fresh_dir hold
mkdir -p "$D/a/b/c/d"
touch "$D/a/b/c/d/f1" "$D/a/b/c/d/f2" "$D/a/b/c/d/f3"
mfm_start "$D/a/b/c/d"
do_input key Down
do_input press space
sleep 1.2
do_input lift space
do_input key Delete
wait_for "Space held marks f1" test ! -e "$D/a/b/c/d/f1"
[ -e "$D/a/b/c/d/f2" ] && [ -e "$D/a/b/c/d/f3" ] || fail "holding Space marked more"
do_input press BackSpace
sleep 1.2
do_input lift BackSpace
do_input key F7
do_input key n
do_input key Return
file_appears "Backspace held goes up once" "$D/a/b/c/n"
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
# Through the menu: Open Cut Copy "Copy to clipboard" Send (Paste is off with
# nothing to paste).
do_input move $((LISTX + 40)) "$(row_y 1)"
do_input click right
menu_up $((LISTX + 40)) "$(row_y 1)"
for _ in 1 2 3 4 5; do do_input key Down; done
do_input key Return
file_appears "the menu sends the clicked entry" "$SENT"
[ "$(cat "$SENT")" = "$D/a.txt" ] || fail "MFM_SEND got: $(cat "$SENT")"
mfm_quit

# The end of MFM_SEND ends the job, also when it leaves something running that keeps
# its stderr open.
BGPID=$SMOKE_DIR/bg.pid
MFM_SEND="sh -c 'sleep 300 & echo \$! > \"\$0\"' $BGPID" mfm_start "$D"
do_input key Down
do_input key ctrl+s
file_appears "MFM_SEND ran" "$BGPID"
do_input key F7
do_input key q
do_input key Return
file_appears "the job is over when MFM_SEND is" "$D/q"
kill "$(cat "$BGPID")"
mfm_quit

# ~/.config/mfm/mfmrc: Mfm*send for Ctrl+S, and where sorting and hidden files
# begin. By size with hidden files shown, the hidden one-byte file is last.
fresh_dir mfmrc
head -c 300 /dev/zero >"$D/big.txt"
head -c 100 /dev/zero >"$D/small.txt"
head -c 1 /dev/zero >"$D/.hid"
mkdir -p "$XDG_CONFIG_HOME/mfm"
{
	printf '! comment\n'
	printf '%s\n' "Mfm*send: sh -c 'printf \"%s\\n\" \"\$@\" > \"\$0.tmp\" && mv \"\$0.tmp\" \"\$0\"' $SENT"
	printf 'mfm.sort:  size\n'
	printf 'Mfm*showHidden: true\n'
} >"$XDG_CONFIG_HOME/mfm/mfmrc"
rm -f "$SENT"
mfm_start "$D"
for _ in 1 2 3; do do_input key Down; done
do_input key ctrl+s
file_appears "mfmrc: send, sort and hidden files" "$SENT"
[ "$(cat "$SENT")" = "$D/.hid" ] || fail "Mfm*send got: $(cat "$SENT")"
rm "$SENT" "$XDG_CONFIG_HOME/mfm/mfmrc"
mfm_quit

# ---- settings --------------------------------------------------------------

# A font name without a size has no metrics of its own: find_rows checks the height.
fresh_dir font
MFM_FONT=Monospace mfm_start "$D"
mfm_quit
echo "ok: a font without a size"

# The selected row has mwc's activeBackground, a mid-tone: black reads better on it
# than white. A big bold font has strokes wider than the edge smoothing.
fresh_dir ink
MFM_FONT="Sans Bold 24" mfm_start "$D"
shoot
ink=$(build/pngtool count "$SHOT" $((CX + 20)) $((ROW0 + 1)) 400 $((ROWH - 2)) 000000)
white=$(build/pngtool count "$SHOT" $((CX + 20)) $((ROW0 + 1)) 400 $((ROWH - 2)) ffffff)
[ "$ink" -gt 0 ] && [ "$white" = 0 ] ||
	fail "the selected row has $ink black and $white white pixels, not black text"
echo "ok: black text on the selected row"
mfm_quit

# As mwc reads it: a line however long counts, and the last line wins, so a value
# too long to keep gives the default, not a cut one nor the one before.
fresh_dir rclong
mkdir -p "$XDG_CONFIG_HOME/mfm"
printf 'Mfm*background:%1100s#336699\n' '' >"$XDG_CONFIG_HOME/mfm/mfmrc"
BG=336699
mfm_start "$D"
BG=aeb2c3
echo "ok: a line of 1100 blanks and a colour counts"
mfm_quit
{
	printf 'Mfm*background: #ff0000\n'
	printf 'Mfm*background: #%0300d\n' 0
} >"$XDG_CONFIG_HOME/mfm/mfmrc"
mfm_start "$D"
echo "ok: a value too long to keep gives the default"
mfm_quit
rm "$XDG_CONFIG_HOME/mfm/mfmrc"

# rgb: takes hex digits only, as mwc: no sign, blank or 0x. Each gives the default.
for c in rgb:-1/0/0 rgb:-f/0/0 'rgb: f/0/0' rgb:0xf/0/0; do
	MWC_INACTIVE=$c mfm_start "$D"
	grep -qF "ignoring MWC_INACTIVE \"$c\": not a colour" "$MFM_LOG" ||
		fail "$c: no word about it on stderr"
	echo "ok: $c is no colour, and mfm says so"
	mfm_quit
done

# The text colours of mwc's titles: Mwm*foreground and Mwm*activeForeground, else
# black or white. A big bold font has strokes wider than the edge smoothing; the
# tolerance is small, as the smoothing passes through many colours. On its own
# face a colour stays as set, the red on the status line; on the list, where
# c00000 has 3.9:1, it goes as far towards black as 4.5:1 needs, to ad0000.
fresh_dir fgres
touch "$D/a"
mkdir -p "$XDG_CONFIG_HOME/mwc"
printf 'Mwm*foreground: #c00000\nMwm*activeForeground: #00c000\n' \
	>"$XDG_CONFIG_HOME/mwc/mwmrc"
MFM_FONT="Sans Bold 24" mfm_start "$D"
shoot
H=$(get mfm h)
red=$(build/pngtool count "$SHOT" "$CX" $((CY + H - 60)) 400 60 c00000 6)
moved=$(build/pngtool count "$SHOT" $((CX + 20)) $((ROW0 + ROWH + 1)) 400 $((ROWH - 2)) ad0000 6)
unmoved=$(build/pngtool count "$SHOT" $((CX + 20)) $((ROW0 + ROWH + 1)) 400 $((ROWH - 2)) c00000 6)
green=$(build/pngtool count "$SHOT" $((CX + 20)) $((ROW0 + 1)) 400 $((ROWH - 2)) 00c000 6)
[ "$red" -gt 0 ] && [ "$moved" -gt 0 ] && [ "$unmoved" = 0 ] && [ "$green" -gt 0 ] ||
	fail "foreground: $red red on the status line, $moved ad0000 and $unmoved c00000 on a row, $green green on the selected one"
echo "ok: Mwm*foreground and Mwm*activeForeground, moved on the list"
mfm_quit
rm "$XDG_CONFIG_HOME/mwc/mwmrc"

# The date shows where the name keeps at least as much room as the date takes: a big
# font in the default window leaves it out. A folder has no size, so the right end
# of its row holds only the date.
fresh_dir dim
mkdir "$D/a"
touch -d 2024-01-01 "$D/a"
dim_px() {
	build/pngtool count "$SHOT" $((CX + $(get mfm w) - 220)) $((ROW0 + ROWH + 1)) 180 \
		$((ROWH - 2)) "$1" 6
}
MFM_FONT="DejaVu Sans Bold 24" mfm_start "$D"
name=$(build/pngtool count "$SHOT" $((CX + 20)) $((ROW0 + ROWH + 1)) 200 $((ROWH - 2)) 000000)
[ "$name" -gt 0 ] || fail "no name in the row the date is looked for in"
[ "$(dim_px 4f5055)" = 0 ] || fail "a big font in a small window shows the date"
echo "ok: a big font in a small window leaves the date out"
# Dimmed text, the date here, as far as 4.5:1 allows on the list it is drawn on:
# 4f5055 on c6c9d5. 3d3e44, dimmed for the window's background, is darker than
# the smoothing of 4f5055 ever gets.
do_input key super+Up
check "maximized" '[ "$(get mfm max)" = 1 ]'
content_origin
find_rows
check "maximized it shows the date, and dimmed text has 4.5:1" 'shoot; [ "$(dim_px 4f5055)" -gt 0 ]'
[ "$(dim_px 3d3e44)" = 0 ] || fail "the date is dimmed for the background, not the list"
mfm_quit

# The error colour follows the background: light on a dark one. F2 on ".." is one.
fresh_dir err
BG=202020
MWC_INACTIVE=#202020 MFM_FONT="Sans Bold 24" mfm_start "$D"
BG=aeb2c3
H=$(get mfm h)
do_input key F2
check "an error on a dark background is light" \
	'shoot; [ "$(build/pngtool count "$SHOT" $CX $((CY + H - 60)) 600 60 ffa0a0 6)" -gt 0 ]'
mfm_quit

# Text goes by the face it is on. On #008080 the window's text is white, the
# lighter list's black; on a dark one the input field, light whatever the theme,
# gets dark text, 666666 there.
fresh_dir faces
touch "$D/a" "$D/b"
BG=008080
MWC_INACTIVE=#008080 MFM_FONT="Sans Bold 24" mfm_start "$D"
BG=aeb2c3
shoot
black=$(build/pngtool count "$SHOT" $((CX + 20)) $((ROW0 + ROWH + 1)) 200 $((ROWH - 2)) 000000 6)
[ "$black" -gt 0 ] || fail "#008080: no black text on the list"
echo "ok: text on the list goes by the list, not the background"
mfm_quit
BG=202020
MWC_INACTIVE=#202020 MFM_FONT="Sans Bold 24" mfm_start "$D"
BG=aeb2c3
H=$(get mfm h)
# The label's white text smooths through 666666 too: only the field right of it
# counts, filled with w's.
do_input key F7
for _ in 1 2 3 4 5 6 7 8 9 10 11 12; do do_input key w; done
check "an input field on a dark theme has dark text" \
	'shoot; [ "$(build/pngtool count "$SHOT" $((CX + 260)) $((CY + H - 60)) 300 60 666666 0)" -gt 0 ]'
do_input key Escape
mfm_quit

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

# ---- the clipboard of the session -----------------------------------------

CLIPGET=$MFM_DIR/build/clipget
[ -x "$CLIPGET" ] || fail "$CLIPGET not built (make build/clipget)"
clipget() { WAYLAND_DISPLAY=wayland-0 "$CLIPGET" "$@" 2>>"$LOG"; }
# Only this test's own: the box may share the host's processes.
clip_procs() {
	local p
	for p in $(pgrep -x mfm-clip); do
		[ "$p" = "$MFM_PID" ] && continue
		{ tr '\0' '\n' <"/proc/$p/environ"; } 2>/dev/null | grep -qx "SMOKE_DIR=$SMOKE_DIR" &&
			echo "$p"
	done
}

fresh_dir clip
touch "$D/a b.txt" "$D/zz.txt"
mfm_start "$D"
do_input key a
do_input key Down
do_input key ctrl+c
do_input key ctrl+v
file_appears "Ctrl+C, Ctrl+V" "$D/zz (copy).txt"
check "plain Ctrl+C leaves the clipboard alone" 'clipget types >/dev/null; [ $? = 3 ]'
do_input key Home
do_input key Down
do_input key ctrl+shift+c
check "Ctrl+Shift+C offers text/uri-list" 'clipget types | grep -qx text/uri-list'
check "... as a file URL, percent-encoded, CRLF-terminated" \
	'[ "$(clipget text/uri-list | od -An -c | tr -d " \n")" = "$(printf "file://%s/a%%20b.txt\r\n" "${D// /%20}" | od -An -c | tr -d " \n")" ]'
check "... and the path as text" '[ "$(clipget text/plain)" = "$D/a b.txt" ]'
check "... and for GNOME's file managers" \
	'[ "$(clipget x-special/gnome-copied-files)" = "copy
file://${D// /%20}/a%20b.txt" ]'
do_input key ctrl+v
file_appears "Ctrl+V in mfm still pastes what Ctrl+C remembered" "$D/zz (copy 2).txt"
check "... and not what went into the clipboard" '[ ! -e "$D/a b (copy).txt" ]'
do_input key ctrl+a
do_input key ctrl+shift+c
check "Ctrl+Shift+C: all the marked entries" \
	'[ "$(clipget x-special/gnome-copied-files | wc -l)" = 4 ]'
check "... with the special characters encoded" \
	'clipget text/uri-list | grep -qx "file://${D// /%20}/zz%20%28copy%29.txt"$'"'"'\r'"'"''
check "one helper holds the clipboard" '[ "$(clip_procs | wc -l)" = 1 ]'
mfm_quit
check "it outlives the window" 'clipget text/plain | grep -qx "$D/zz.txt"'
check "... still just one" '[ "$(clip_procs | wc -l)" = 1 ]'

# A name that is not UTF-8 goes as a URL, not as text that says it is UTF-8.
fresh_dir clip2
touch "$D/$(printf 'caf\xe9')"
mfm_start "$D"
do_input key Down
do_input key ctrl+shift+c
check "a Latin-1 name goes as a URL" 'clipget text/uri-list | grep -q "caf%E9"'
check "... and not as text" '! clipget types | grep -q "^text/plain"'
mfm_quit

# A newline would make two paths of one, and the second a command in a terminal.
fresh_dir clip3
touch "$D/$(printf 'a\nrm -rf x')"
mfm_start "$D"
do_input key Down
do_input key ctrl+shift+c
check "a name with a newline goes as a URL" 'clipget text/uri-list | grep -q "a%0Arm"'
check "... and not as text" '! clipget types | grep -q "^text/plain"'
mfm_quit

# ---- file descriptors --------------------------------------------------------

# Started with stdin, stdout and stderr closed, mfm must not hand its Wayland
# connection to a job: quitting during one would leave the window up.
fresh_dir fds
touch "$D/a.txt"
SHIM=$SMOKE_DIR/shim
mkdir -p "$SHIM"
REAL_CP=$(command -v cp)
cat >"$SHIM/cp" <<EOS
#!/bin/sh
[ -e "$SHIM/hang" ] && { echo \$\$ >"$SHIM/hung.tmp"; mv "$SHIM/hung.tmp" "$SHIM/hung"; exec sleep 300; }
exec "$REAL_CP" "\$@"
EOS
chmod +x "$SHIM/cp"
touch "$SHIM/hang"
PATH=$SHIM:$PATH WAYLAND_DISPLAY=wayland-0 "$MFM" "$D" <&- >&- 2>&- &
MFM_PID=$!
mfm_up
do_input key Down
do_input key ctrl+c
do_input key ctrl+v
file_appears "the copy hangs in cp" "$SHIM/hung"
do_input key ctrl+q
wait "$MFM_PID"
MFM_PID=
check "quit during a job, the window goes" '[ -z "$(get mfm w)" ]'
kill "$(cat "$SHIM/hung")"
rm -f "$SHIM/hang" "$SHIM/hung"

# The clipboard's helper outlives mfm, but not as a writer of mfm's output: what
# reads that gets its end when mfm quits.
fresh_dir helperout
touch "$D/a.txt"
mkfifo "$SMOKE_DIR/out.fifo"
cat "$SMOKE_DIR/out.fifo" >/dev/null &
READER=$!
WAYLAND_DISPLAY=wayland-0 "$MFM" "$D" >"$SMOKE_DIR/out.fifo" 2>&1 &
MFM_PID=$!
mfm_up
rm "$SMOKE_DIR/out.fifo"        # both ends are open
do_input key Down
do_input key ctrl+shift+c
check "the helper has the clipboard" 'clipget types | grep -qx text/uri-list'
do_input key ctrl+q
wait "$MFM_PID"
MFM_PID=
check "the window is gone" '[ -z "$(get mfm w)" ]'
wait_for "what reads mfm's output gets its end" eval '! kill -0 "$READER" 2>/dev/null'

# ---- a scaled output ------------------------------------------------------

# At scale 2 it draws twice the pixels: the first one of the selected row is the
# row's colour, not a blend with the one above from a buffer scaled up. The
# screenshot has the output's pixels, twice the layout's.
mkdir -p "$XDG_CONFIG_HOME/mwc"
echo 'Mwm*HEADLESS-1*outputScale: 2' >"$XDG_CONFIG_HOME/mwc/mwmrc"
reload_mwc
check "the output is scaled" '[ "$(global output_HEADLESS-1 scale)" = 200 ]'
fresh_dir scaled
touch "$D/a" "$D/b"
WAYLAND_DISPLAY=wayland-0 "$MFM" "$D" >>"$MFM_LOG" 2>&1 &
MFM_PID=$!
check "mfm has a focused window" '[ "$(get mfm kbfocus)" = 1 ]'
content_origin
sharp=
for ((i = 0; i < 30; i++)); do
	WAYLAND_DISPLAY=wayland-0 grim "$SHOT" 2>/dev/null &&
		sharp=$(pixel $((2 * (CX + 80))) $((2 * (CY + ROWOFF))))
	[ "$sharp" = "$SEL" ] && break
	sleep 0.1
done
[ "$sharp" = "$SEL" ] ||
	fail "at scale 2 the selected row's first pixel is $sharp, not $SEL"
echo "ok: sharp at scale 2"

# The cursor follows the scale, also while the pointer stays on the window. Its
# buffer scale must divide its size, or compositors that check kill the client.
# vinput counts in the output's pixels.
cursor_ok() { # scale
	local w h s
	w=$(global cursor cw) h=$(global cursor ch) s=$(global cursor cscale)
	[ "$s" = "$1" ] && [ "$w" -gt 0 ] && [ $((w % s)) = 0 ] && [ $((h % s)) = 0 ]
}
do_input move $((2 * (CX + 80))) $((2 * (CY + ROWOFF + 30)))
check "the cursor is at scale 2" 'cursor_ok 2'
echo 'Mwm*HEADLESS-1*outputScale: 1' >"$XDG_CONFIG_HOME/mwc/mwmrc"
reload_mwc
check "the output is not scaled" '[ "$(global output_HEADLESS-1 scale)" = 100 ]'
check "the cursor is set again at scale 1 without the pointer moving" 'cursor_ok 1'
mfm_quit
rm "$XDG_CONFIG_HOME/mwc/mwmrc"

# A cursor theme need not have the size asked for: Adwaita has 24, 30, 36, 48, 72 and
# 96, so 8 at scale 4 asks for 32 and gets 30, which 4 doesn't divide. A viewport then
# shows the buffer at 30 / 4, about 8, as large as mwc shows its own cursor.
# The last size the cursor's viewport was given, "w h"; -1 -1 is none.
cursor_dest() {
	grep -o 'wp_viewport#[0-9]*\.set_destination(-\?[0-9]*, -\?[0-9]*)' "$MFM_LOG" |
		tail -1 | sed 's/.*(\(.*\), \(.*\))/\1 \2/'
}
# Buffers destroyed while the cursor surface still showed them: a theme gone too early.
cursor_kept() {
	awk '
	match($0, /wl_pointer#[0-9]+\.set_cursor\([0-9]+, wl_surface#[0-9]+/) {
		split(substr($0, RSTART, RLENGTH), f, /[#(, ]+/); cur = f[5]
	}
	match($0, /wl_surface#[0-9]+\.attach\((wl_buffer#[0-9]+|nil)/) {
		split(substr($0, RSTART, RLENGTH), f, /[#.()]+/)
		att[f[2]] = f[4] == "nil" ? "" : f[5]
	}
	match($0, /wl_buffer#[0-9]+\.destroy\(\)/) {
		split(substr($0, RSTART, RLENGTH), f, /[#.()]+/)
		if (cur != "" && att[cur] == f[2]) print f[2]
	}' "$MFM_LOG"
}
# How often the cursor surface was left empty since line $1 of the log: with the
# pointer on the window it never should be. The whole log is read, for which surface
# is the cursor's.
cursor_blank() {
	awk -v from="$1" '
	match($0, /wl_pointer#[0-9]+\.set_cursor\([0-9]+, wl_surface#[0-9]+/) {
		split(substr($0, RSTART, RLENGTH), f, /[#(, ]+/); cur = f[5]
	}
	match($0, /wl_surface#[0-9]+\.attach\(nil/) {
		split(substr($0, RSTART, RLENGTH), f, /[#.()]+/)
		if (NR >= from && f[2] == cur) n++
	}
	END { print n + 0 }' "$MFM_LOG"
}
# mfm_raw [env...]: start mfm without the pixel checks of mfm_start, which count in
# unscaled pixels.
mfm_raw() {
	env "$@" WAYLAND_DISPLAY=wayland-0 "$MFM" "$D" >>"$MFM_LOG" 2>&1 &
	MFM_PID=$!
	check "mfm has a focused window" '[ "$(get mfm kbfocus)" = 1 ]'
	content_origin
}
echo 'Mwm*HEADLESS-1*outputScale: 4' >"$XDG_CONFIG_HOME/mwc/mwmrc"
reload_mwc
check "the output is scaled by 4" '[ "$(global output_HEADLESS-1 scale)" = 400 ]'
fresh_dir cursor
: >"$MFM_LOG"
mfm_raw XCURSOR_THEME=Adwaita XCURSOR_SIZE=8 WAYLAND_DEBUG=client
do_input move $((4 * (CX + 20))) $((4 * (CY + 20)))
check "a cursor of 30 at scale 4 is set at scale 1" \
	'cursor_ok 1 && [ "$(global cursor cw)" = 30 ]'
check "and the viewport shows it at 8" '[ "$(cursor_dest)" = "8 8" ]'
# Back to 2 with the pointer kept on the window: the cursor follows without an enter.
FROM=$(($(wc -l <"$MFM_LOG") + 1))
echo 'Mwm*HEADLESS-1*outputScale: 2' >"$XDG_CONFIG_HOME/mwc/mwmrc"
reload_mwc
check "a new scale loads the cursor again at once" \
	'cursor_ok 2 && [ "$(global cursor cw)" = 24 ]'
check "there the scale divides it, and the viewport is unset" \
	'[ "$(cursor_dest)" = "-1 -1" ]'
[ -z "$(cursor_kept)" ] || fail "with the pointer on it, the cursor showed destroyed buffers"
echo "ok: with the pointer on the window no cursor buffer goes while shown"
[ "$(cursor_blank "$FROM")" = 0 ] || fail "with the pointer on it, the cursor was left empty"
echo "ok: and the cursor is never empty"
# The pointer goes to mwc's title bar, and the scale changes: the surface lets go of
# the old theme's buffer before that goes.
refresh
do_input move $((2 * (CX + 20))) $((2 * ($(get mfm y) + B + T / 2)))
check "the pointer leaves the window" "grep -q 'wl_pointer#[0-9]*\.leave(' \"\$MFM_LOG\""
GONE=$(grep -c 'wl_buffer#[0-9]*\.destroy()' "$MFM_LOG")
echo 'Mwm*HEADLESS-1*outputScale: 4' >"$XDG_CONFIG_HOME/mwc/mwmrc"
reload_mwc
check "the output is scaled by 4 again" '[ "$(global output_HEADLESS-1 scale)" = 400 ]'
check "the old theme is gone" \
	"[ \"\$(grep -c 'wl_buffer#[0-9]*\.destroy()' \"\$MFM_LOG\")\" -gt $GONE ]"
[ -z "$(cursor_kept)" ] || fail "with the pointer away, the cursor showed destroyed buffers"
echo "ok: with the pointer away no cursor buffer goes while shown"
mfm_quit
echo 'Mwm*HEADLESS-1*outputScale: 2' >"$XDG_CONFIG_HOME/mwc/mwmrc"
reload_mwc
check "the output is scaled by 2 again" '[ "$(global output_HEADLESS-1 scale)" = 200 ]'
# Only a whole number is a size: "32x" is none and gives the default 24, 48 at scale 2.
mfm_raw XCURSOR_THEME=Adwaita XCURSOR_SIZE=32x
do_input move $((2 * (CX + 20))) $((2 * (CY + 20)))
check "XCURSOR_SIZE=32x gives the default" 'cursor_ok 2 && [ "$(global cursor cw)" = 48 ]'
mfm_quit
# A size past any theme is the largest there is, not an overflow.
mfm_raw XCURSOR_THEME=Adwaita XCURSOR_SIZE=1000000000
do_input move $((2 * (CX + 20))) $((2 * (CY + 20)))
check "a huge XCURSOR_SIZE gives the largest cursor" '[ "$(global cursor cw)" = 96 ]'
mfm_quit
rm "$XDG_CONFIG_HOME/mwc/mwmrc"
reload_mwc
check "the output is not scaled" '[ "$(global output_HEADLESS-1 scale)" = 100 ]'

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
