#!/bin/bash
# Black-box test of mtk's Wayland side: mtkdemo in a headless mwc, driven with mwc's
# vinput, read from screenshots and from MTKDEMO_STATE. It has its own sockets and
# touches no running session.
#
# Needs a built mwc with build/vinput, build/pngtool and build/xdgc in MWC_DIR (by
# default ../mwc), grim, and the Adwaita cursor theme. MTKDEMO picks the binary, as
# build/mtkdemo-asan. Output goes to build/ui.log, the demo's own to
# build/ui-mtkdemo.log.
MTK_DIR=$(cd "$(dirname "$0")/.." && pwd)
DEMO=${MTKDEMO:-$MTK_DIR/build/mtkdemo}
case $DEMO in /*) ;; *) DEMO=$MTK_DIR/$DEMO ;; esac
LOG=$MTK_DIR/build/ui.log
DEMO_LOG=$MTK_DIR/build/ui-mtkdemo.log
MWC_DIR=${MWC_DIR:-$MTK_DIR/../mwc}
[ -x "$DEMO" ] || { echo "FAIL: $DEMO not built"; exit 1; }
[ -d "$MWC_DIR" ] || { echo "FAIL: no mwc in $MWC_DIR (set MWC_DIR)"; exit 1; }
mkdir -p "$MTK_DIR/build"
cd "$MWC_DIR" || exit 1
. tests/lib.sh
require_tools grim
for tool in build/vinput build/pngtool build/xdgc; do
	[ -x "$tool" ] || { echo "FAIL: $MWC_DIR/$tool not built (make $tool)"; exit 1; }
done

export MWC_TEST_STATE=$SMOKE_DIR/state
export LC_ALL=C.UTF-8
unset MWC_INACTIVE MWC_ACTIVE XCURSOR_PATH MTKDEMO_FIXED MTKDEMO_DECO
export XCURSOR_THEME=Adwaita XCURSOR_SIZE=24
export ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=1}
export LSAN_OPTIONS=suppressions=$MTK_DIR/tests/unit/lsan.supp:print_suppressions=0
: >"$DEMO_LOG"
SHOT=$SMOKE_DIR/ui.png
ERR_SHOT=$MTK_DIR/build/ui-last.png
DS=$SMOKE_DIR/demo.state
# Motif 2.3.8's shades of mwc's default colours, as mwc draws its own frames.
TOP=dadada      # the light shadow of #aaaaaa, mwc's default
ACT_TOP=e7e5db  # ... of #c6c1aa, the border with the keyboard
BOT=5a5a5a
BG=aaaaaa
SEL=c6c1aa      # activeBackground: a lit menu item

mkdir -p "$XDG_CONFIG_HOME/mwc"
: >"$XDG_CONFIG_HOME/mwc/mwmrc"
start_mwc 'exec sleep 300'
start_vinput
refresh

shoot() { WAYLAND_DISPLAY=wayland-0 grim "$SHOT" 2>/dev/null; }
# ds KEY: a value from the demo's state
ds() { awk -v k="$1" '$1 == k { $1 = ""; sub(/^ /, ""); print; exit }' "$DS" 2>/dev/null; }
where() {
	X0=$(get mtkdemo x) Y0=$(get mtkdemo y) W0=$(get mtkdemo w) H0=$(get mtkdemo h)
}
# pixel_at DESCRIPTION DX DY RRGGBB: in the demo, polled; DX and DY in the pixels of
# the screenshot, SC of them to one in the layout.
SC=1
pixel_at() {
	local got= i
	for ((i = 0; i < 50; i++)); do
		shoot && got=$(build/pngtool pixel "$SHOT" $((SC * X0 + $2)) $((SC * Y0 + $3)))
		[ "$got" = "$4" ] && { echo "ok: $1"; return 0; }
		sleep 0.1
	done
	cp "$SHOT" "$ERR_SHOT" 2>/dev/null
	fail "$1: $2,$3 is ${got:-unreadable}, expected $4 (screenshot in $ERR_SHOT)"
}
start_demo() {
	rm -f "$DS"
	MTKDEMO_STATE=$DS WAYLAND_DISPLAY=wayland-0 "$DEMO" >>"$DEMO_LOG" 2>&1 &
	PID_demo=$!
	check "mtkdemo is mapped" "[ -n \"\$(get mtkdemo x)\" ]"
	refresh
	where
}
quit_demo() { # key
	local i
	do_input key "$1"
	for ((i = 0; i < 50; i++)); do
		kill -0 "$PID_demo" 2>/dev/null || break
		sleep 0.1
	done
	kill -0 "$PID_demo" 2>/dev/null && fail "$1 doesn't quit"
	wait "$PID_demo"
	[ $? = 0 ] || fail "mtkdemo didn't exit with 0 (see $DEMO_LOG)"
	check "it is gone" "[ -z \"\$(get mtkdemo x)\" ]"
}

# ---- a fixed window, as a dock tool --------------------------------------------
MTKDEMO_FIXED=1 start_demo
check "its own size" "[ \"\$(get mtkdemo w)x\$(get mtkdemo h)\" = 300x120 ]"
[ "$(get mtkdemo bw)" = 0 ] && [ "$(get mtkdemo th)" = 0 ] ||
	fail "mwc decorates it: border $(get mtkdemo bw), title $(get mtkdemo th)"
echo "ok: no server-side decoration unless asked"
check "it has the keyboard" "[ \"\$(get mtkdemo kbfocus)\" = 1 ]"
check "it knows" "[ \"\$(ds focus)\" = 1 ]"
check "mtk_window_focused() says so too" "[ \"\$(ds wfocus)\" = 1 ]"
check "the seat and the display" "[ \"\$(ds seat)\" = 1 ] && [ \"\$(ds display)\" = 1 ]"
pixel_at "its border is the active one, in Motif's light shadow" 0 60 $ACT_TOP
pixel_at "the face beside the bevel" 2 60 c6c1aa

# Another window takes the keyboard: the border goes inactive.
WAYLAND_DISPLAY=wayland-0 build/xdgc app=thief size=200x100 >"$SMOKE_DIR/thief.log" 2>&1 &
PID_thief=$!
check "another window takes the keyboard" "[ \"\$(get thief kbfocus)\" = 1 ]"
check "it knows it lost it" "[ \"\$(ds focus)\" = 0 ]"
check "mtk_window_focused() too" "[ \"\$(ds wfocus)\" = 0 ]"
pixel_at "its border is the inactive one" 0 60 $TOP
pixel_at "the dark shadow on the right" 299 60 $BOT
kill "$PID_thief"
wait "$PID_thief" 2>/dev/null
check "it has the keyboard again" "[ \"\$(ds focus)\" = 1 ]"

# settle KEY: the value once it has stopped changing for 0.2 s
settle() {
	local a b i
	b=$(ds "$1")
	for ((i = 0; i < 50; i++)); do
		sleep 0.2
		a=$(ds "$1")
		[ "$a" = "$b" ] && break
		b=$a
	done
	echo "$a"
}

# ---- keys -----------------------------------------------------------------------
for k in h i shift+1; do do_input key "$k"; done
check "typed text arrives" "[ \"\$(ds text)\" = 'hi!' ]"
check "with a serial for requests" "[ \"\$(ds serial)\" = 1 ]"
do_input key BackSpace
check "BackSpace" "[ \"\$(ds text)\" = 'hi' ]"
do_input key ctrl+a
check "Ctrl+A makes no text" "[ \"\$(ds text)\" = 'hi' ]"
# A held key repeats at mwc's rate, after its delay; a modifier pressed while it
# repeats doesn't stop it.
r0=$(ds repeats)
do_input press x
check "a held key repeats" "[ \"\$(ds repeats)\" -ge $((r0 + 3)) ]"
do_input lift x
r1=$(settle repeats)
sleep 0.5
[ "$(ds repeats)" = "$r1" ] || fail "it repeats on after the release"
echo "ok: and stops at the release"
case $(ds text) in hixxxx*) echo "ok: the repeats are typed" ;;
	*) fail "the repeats made '$(ds text)'" ;; esac
do_input press x
check "x repeats again" "[ \"\$(ds repeats)\" -ge $((r1 + 3)) ]"
do_input hold shift
r2=$(ds repeats)
check "and on with Shift pressed" "[ \"\$(ds repeats)\" -ge $((r2 + 3)) ]"
do_input lift x
do_input release shift
r1=$(settle repeats)
case $(ds text) in hixxxx*xX*) echo "ok: as X then" ;;
	*) fail "the repeats with Shift made '$(ds text)'" ;; esac
# Ctrl+x may not repeat: the client says so.
do_input hold ctrl
do_input press x
sleep 1
do_input lift x
do_input release ctrl
[ "$(ds repeats)" = "$r1" ] || fail "Ctrl+X repeats, though key_repeats says no"
echo "ok: the client decides which keys repeat"

# ---- the pointer ----------------------------------------------------------------
# The button is at 9,9, 80 wide.
do_input move $((X0 + 30)) $((Y0 + 15))
do_input down left
pixel_at "the button is sunken while pressed" 9 9 $BOT
do_input up left
check "a click" "[ \"\$(ds clicks)\" = 1 ]"
check "at the pointer, in the window's pixels" "[ \"\$(ds press)\" = '30 15' ]"
pixel_at "it is raised again" 9 9 $TOP
do_input move $((X0 + 30)) $((Y0 + 15))
do_input down left
do_input move $((X0 + 200)) $((Y0 + 15))
do_input up left
check "released elsewhere: no click" "[ \"\$(ds clicks)\" = 1 ]"
do_input wheel 2
check "the wheel, in steps, once per frame" \
	"[ \"\$(ds steps)\" = 2 ] && [ \"\$(ds scrolls)\" = 1 ]"
do_input wheel -3
check "and back" "[ \"\$(ds steps)\" = -1 ] && [ \"\$(ds scrolls)\" = 2 ]"

# ---- held buttons -----------------------------------------------
# A press sees the buttons held before it.
do_input move $((X0 + 150)) $((Y0 + 90))
do_input down left
do_input down middle
check "a press sees the left button held" "[ \"\$(ds buttons)\" = 1 ]"
do_input up middle
do_input up left
do_input click left
check "and none after their release" "[ \"\$(ds buttons)\" = 0 ]"

# ---- a popup -----------------------------------------------------------------------
# The right button opens a menu below and right of the pointer, with the grab.
PX=150 PY=50
menu_up() { pixel_at "$1" $((PX + 60)) $((PY + 14)) $BG; }
do_input move $((X0 + PX)) $((Y0 + PY))
do_input click right
check "the menu is up" "[ \"\$(ds popup)\" = 1 ]"
check "where mwc put it, at the pointer" "[ \"\$(ds popxy)\" = '$PX $PY' ]"
check "a surface and no toplevel" "[ \"\$(ds handles)\" = 1 ]"
menu_up "it is drawn below and right of the pointer"
do_input key Down
pixel_at "Down lights the first item" $((PX + 60)) $((PY + 14)) $SEL
do_input key Return
check "Return takes it, and the menu goes" "[ \"\$(ds choice)\" = 1 ] && [ \"\$(ds popup)\" = 0 ]"
pixel_at "it is gone from the screen" $((PX + 60)) $((PY + 14)) f6f6f6
do_input click right
menu_up "up again"
do_input move $((X0 + PX + 60)) $((Y0 + PY + 2 + 24 + 12))
pixel_at "the pointer lights the second item" $((PX + 60)) $((PY + 2 + 24 + 12)) $SEL
do_input click left
check "a click takes it" "[ \"\$(ds choice)\" = 2 ] && [ \"\$(ds popup)\" = 0 ]"
do_input move $((X0 + PX)) $((Y0 + PY))
do_input click right
menu_up "up again"
do_input key Escape
check "Esc closes it, nothing taken" "[ \"\$(ds popup)\" = 0 ] && [ \"\$(ds choice)\" = 2 ]"
check "Esc went to the menu, not the window" "kill -0 $PID_demo"
# Held past mwc's delay, its repeats don't go on to the window, which quits on Esc.
do_input click right
menu_up "up again"
do_input press Escape
check "the menu goes at the press" "[ \"\$(ds popup)\" = 0 ]"
sleep 1.5
do_input lift Escape
sleep 0.3
check "a held Esc doesn't repeat into the window" "kill -0 $PID_demo"
do_input click right
menu_up "up again"
do_input move $((X0 + 20)) $((Y0 + 100))
do_input click left
check "a click on the window beside it closes it" "[ \"\$(ds popup)\" = 0 ]"
WAYLAND_DISPLAY=wayland-0 build/xdgc app=thief size=200x100 >"$SMOKE_DIR/thief.log" 2>&1 &
PID_thief=$!
check "another window is up" "[ -n \"\$(get thief x)\" ]"
do_input move $((X0 + 150)) $((Y0 + 15))   # above the other window
do_input click left
check "the demo has the keyboard" "[ \"\$(ds focus)\" = 1 ]"
do_input move $((X0 + PX)) $((Y0 + PY))
do_input click right
menu_up "up again"
TX=$(get thief x) TY=$(get thief y)
do_input move $((TX + 190)) $((TY + 90))
do_input click left
check "a click on another client ends it" "[ \"\$(ds popup)\" = 0 ]"
kill "$PID_thief"
wait "$PID_thief" 2>/dev/null
check "the demo has the keyboard again" "[ \"\$(ds focus)\" = 1 ]"

# ---- the loop -------------------------------------------------------------------
t0=$(ds ticks)
check "the timer ticks" "[ \"\$(ds ticks)\" -gt $((t0 + 2)) ]"
# One at a time: two pending signals may be delivered as one.
kill -USR1 "$PID_demo"
check "the watched fd" "[ \"\$(ds signals)\" = 1 ]"
kill -USR1 "$PID_demo"
check "again" "[ \"\$(ds signals)\" = 2 ]"
# SIGUSR2: two timers due at once, each cancelling the other, and a watch dropped
# in the idle hook while the one after it has something to read.
c0=$(ds sigcalls)
kill -USR2 "$PID_demo"
check "of two timers that cancel each other one runs" "[ \"\$(ds races)\" = 1 ]"
check "a watch dropped in the idle hook isn't polled" \
	"[ \"\$(ds sigcalls)\" = $((c0 + 2)) ]"
sleep 0.3
[ "$(ds races)" = 1 ] && [ "$(ds sigcalls)" = $((c0 + 2)) ] ||
	fail "later: races $(ds races), calls $(ds sigcalls) from $c0"
echo "ok: and stay so"
p0=$(ds paints)
sleep 0.5
[ "$(ds paints)" = "$p0" ] || fail "it paints without a change: $p0, then $(ds paints)"
echo "ok: no paint without a change"
[ "$(ds dirty)" = 0 ] || fail "it says a paint is due"
echo "ok: and none is due"
check "the cursor theme was loaded once" "[ \"\$(ds cursors)\" = 1 ]"

# ---- the scale ------------------------------------------------------------------
# At scale 2 it draws twice the pixels: the first one of the face beside the bevel is
# the face's colour, not a blend from a buffer scaled up.
echo 'Mwm*HEADLESS-1*outputScale: 2' >>"$XDG_CONFIG_HOME/mwc/mwmrc"
reload_mwc
check "the output is scaled" "[ \"\$(global output_HEADLESS-1 scale)\" = 200 ]"
check "the demo at scale 2" "[ \"\$(ds scale)\" = 2 ]"
check "mtk_window_scale() says so too" "[ \"\$(ds wscale)\" = 2 ]"
check "the cursor theme loaded again, for it" "[ \"\$(ds cursors)\" = 2 ]"
refresh
where
check "the same size in the layout" "[ \"\$(get mtkdemo w)x\$(get mtkdemo h)\" = 300x120 ]"
SC=2
pixel_at "the bevel in buffer pixels" $((2 * 1 + 1)) 120 $ACT_TOP
pixel_at "the face right beside it" 4 120 c6c1aa
SC=1
sed -i '/outputScale/d' "$XDG_CONFIG_HOME/mwc/mwmrc"
reload_mwc
check "scale 1 again" "[ \"\$(ds scale)\" = 1 ]"
refresh
where
quit_demo Escape

# ---- a resizable window, with mwc's frame ------------------------------------------
DEMO_LOG_SAVED=$DEMO_LOG DEMO_LOG=$SMOKE_DIR/deco.log
WAYLAND_DEBUG=client MTKDEMO_DECO=1 start_demo
DEMO_LOG=$DEMO_LOG_SAVED
check "mwc decorates it when asked" "[ \"\$(get mtkdemo th)\" -gt 0 ]"
check "the compositor's size" "[ \"\$(ds size)\" = \"\$(get mtkdemo w)x\$(get mtkdemo h)\" ]"
# F2: a size, a minimum and a title of the client's.
do_input key F2
check "a size of its own" "[ \"\$(get mtkdemo w)x\$(get mtkdemo h)\" = 360x140 ]"
check "it paints it" "[ \"\$(ds size)\" = 360x140 ]"
check "a title of its own" "[ \"\$(get mtkdemo tlen)\" = 11 ]"
check "a minimum size" "grep -q 'xdg_toplevel#[0-9]*\.set_min_size(250, 110)' $SMOKE_DIR/deco.log"
# Maximized it takes the size it is given.
do_input key alt+F10
check "maximized, larger" "[ \"\$(get mtkdemo w)\" -gt 600 ]"
check "it paints that size" "[ \"\$(ds size)\" = \"\$(get mtkdemo w)x\$(get mtkdemo h)\" ]"
# Back from it mwc leaves the size to the client: its own from before, not the
# maximized one nor the first.
do_input key alt+F10
check "unmaximized, its size from before" "[ \"\$(ds size)\" = 360x140 ]"
check "and mwc's" "[ \"\$(get mtkdemo w)x\$(get mtkdemo h)\" = 360x140 ]"
quit_demo ctrl+q
grep -q 'ERROR: AddressSanitizer\|runtime error' "$SMOKE_DIR/deco.log" &&
	fail "the sanitizers report: $(grep -m1 'ERROR\|runtime error' "$SMOKE_DIR/deco.log")"

# Maximized before its first commit, mwc has no size of its own for it: back from
# that it leaves the size to the client, which takes its own from the start.
MTKDEMO_DECO=1 MTKDEMO_MAXIMIZED=1 start_demo
check "maximized from the start" "[ \"\$(get mtkdemo max)\" = 1 ]"
check "it paints the maximized size" "[ \"\$(ds size)\" = \"\$(get mtkdemo w)x\$(get mtkdemo h)\" ]"
do_input key alt+F10
check "unmaximized, its own size" "[ \"\$(ds size)\" = 300x120 ]"
check "and mwc's" "[ \"\$(get mtkdemo w)x\$(get mtkdemo h)\" = 300x120 ]"
quit_demo Escape

# ---- damage -------------------------------------------------------------------
# A change damages only the rows that changed, not the whole window.
DEMO_LOG_SAVED=$DEMO_LOG DEMO_LOG=$SMOKE_DIR/damage.log
WAYLAND_DEBUG=client MTKDEMO_FIXED=1 start_demo
DEMO_LOG=$DEMO_LOG_SAVED
check "it has the keyboard" "[ \"\$(ds focus)\" = 1 ]"
do_input key x
check "x typed" "[ \"\$(ds text)\" = x ]"
last_damage_h() {
	grep -o 'damage_buffer(0, [0-9]*, 300, [0-9]*)' "$SMOKE_DIR/damage.log" | tail -1 |
		sed 's/.*, \([0-9]*\))$/\1/'
}
check "a letter damages only the field's rows" 'h=$(last_damage_h); [ "${h:-120}" -lt 60 ]'
quit_demo Escape

# ---- a loop of the client's own --------------------------------------------------
MTKDEMO_OWN_LOOP=1 MTKDEMO_FIXED=1 start_demo
check "with its own loop, it has the keyboard" "[ \"\$(ds focus)\" = 1 ]"
for k in o w n; do do_input key "$k"; done
check "keys arrive" "[ \"\$(ds text)\" = own ]"
r0=$(ds repeats)
do_input press x
check "a held key repeats" "[ \"\$(ds repeats)\" -ge $((r0 + 3)) ]"
do_input lift x
t0=$(ds ticks)
check "mtk's timers run" "[ \"\$(ds ticks)\" -gt $((t0 + 2)) ]"
kill -USR1 "$PID_demo"
check "its own fd" "[ \"\$(ds signals)\" = 1 ]"
pixel_at "its border is the active one" 0 60 $ACT_TOP
# mwc's close ends it too, though mtk_quit() doesn't end its loop.
quit_demo alt+F4

# ---- a popup gone in its configure ----------------------------------------------
MTKDEMO_DROP_MENU=1 MTKDEMO_FIXED=1 start_demo
do_input move $((X0 + 150)) $((Y0 + 50))
do_input click right
check "the menu closes itself as it is configured" \
	"[ \"\$(ds menucfg)\" = 1 ] && [ \"\$(ds popup)\" = 0 ]"
sleep 0.3
check "and the demo lives on" "kill -0 $PID_demo"
do_input key z
check "with the keyboard" "[ \"\$(ds text)\" = z ]"
quit_demo Escape

# ---- shortcuts in a layout without Latin letters ---------------------------------
# Ctrl+Q in a Russian layout is the key that is Q in the US one, as Qt reads it.
stop_vinput
start_vinput 1280 720 ru,us
MTKDEMO_FIXED=1 start_demo
do_input key q
check "q types a Cyrillic letter" "[ \"\$(ds text)\" = й ]"
quit_demo ctrl+q
stop_vinput
start_vinput

# ---- past what Cairo draws -----------------------------------------------------
# 9000 wide is 36000 at scale 4, past the 32767 Cairo draws: drawn at 3 and enlarged
# by mwc. A buffer Cairo cannot draw stays black.
echo 'Mwm*HEADLESS-1*outputScale: 4' >>"$XDG_CONFIG_HOME/mwc/mwmrc"
reload_mwc
check "the output is at scale 4" "[ \"\$(global output_HEADLESS-1 scale)\" = 400 ]"
DEMO_LOG_SAVED=$DEMO_LOG DEMO_LOG=$SMOKE_DIR/wide.log
WAYLAND_DEBUG=client MTKDEMO_FIXED=1 MTKDEMO_WIDTH=9000 start_demo
DEMO_LOG=$DEMO_LOG_SAVED
check "it is 9000 wide" "[ \"\$(get mtkdemo w)\" = 9000 ]"
check "drawn at buffer scale 3" \
	"grep -q 'wl_surface#[0-9]*\.set_buffer_scale(3)' \"$SMOKE_DIR/wide.log\""
SC=4
pixel_at "and drawn, not black" 24 240 $BG
SC=1
quit_demo Escape
grep -q 'ERROR: AddressSanitizer\|runtime error' "$SMOKE_DIR/wide.log" &&
	fail "the sanitizers report: $(grep -m1 'ERROR\|runtime error' "$SMOKE_DIR/wide.log")"
sed -i '/outputScale/d' "$XDG_CONFIG_HOME/mwc/mwmrc"
reload_mwc

# ---- a poll that fails ----------------------------------------------------------
# Told as itself, with its cause, not as a lost connection. Without ASan, whose
# runtime must be loaded first.
err=$(LD_PRELOAD=$MTK_DIR/build/failpoll.so WAYLAND_DISPLAY=wayland-0 \
	"$MTK_DIR/build/mtkdemo" 2>&1 >/dev/null)
st=$?
[ $st = 1 ] && [ "$err" = "mtkdemo: poll: Cannot allocate memory" ] ||
	fail "a failing poll: status $st, \"$err\""
echo "ok: a failing poll is told as such"

stop_mwc
grep -q 'ERROR: AddressSanitizer\|runtime error' "$DEMO_LOG" &&
	fail "the sanitizers report in $DEMO_LOG"
echo "PASS: mtk ui"
