#!/bin/bash
# What file operations do around mounts, full disks and read-only directories, with
# real mounts in a private namespace, driven through mfm under a headless mwc. Skipped
# where unprivileged user namespaces are not available. See common.sh for the rest of
# what it needs. Output goes to build/files.log and build/files-mfm.log.
#
# It runs in three stages: this script, again as root of a user namespace to set up
# the mounts, and again as the normal user in a namespace inside that one.
SELF=$(cd "$(dirname "$0")" && pwd)/$(basename "$0")

if [ -z "${MFMT_STAGE:-}" ]; then
	unshare -Urm true 2>/dev/null || { echo "SKIP: no user namespaces here"; exit 0; }
	MFMT_ROOT=$(mktemp -d "${XDG_RUNTIME_DIR:-/tmp}/mfmt.XXXXXX") || exit 1
	MFMT_STAGE=mounts MFMT_ROOT=$MFMT_ROOT MFMT_UID=$(id -u) MFMT_GID=$(id -g) \
		unshare -Urm bash "$SELF"
	status=$?
	rmdir "$MFMT_ROOT"
	exit $status
fi

if [ "$MFMT_STAGE" = mounts ]; then
	R=$MFMT_ROOT
	mount -t tmpfs -o size=64m none "$R" || exit 1
	mkdir -p "$R/del/outer/inner" "$R/del/precious" "$R/del2/box/m" "$R/src/mnt" \
		"$R/dst" "$R/wide" "$R/vanish/sub" "$R/rolink" "$R/victim"
	echo thesis >"$R/del/precious/thesis.txt"
	mount --bind "$R/del/precious" "$R/del/outer/inner" || exit 1
	mount -t tmpfs none "$R/del2/box/m" || exit 1
	echo keep >"$R/del2/box/m/keep"
	mount -t tmpfs none "$R/src/mnt" || exit 1
	echo keep >"$R/src/mnt/keep"
	mount -t tmpfs -o size=1m none "$R/dst" || exit 1
	mount -t tmpfs -o size=32m none "$R/wide" || exit 1
	head -c 4194304 /dev/urandom >"$R/src/big"
	echo slow >"$R/src/slow"
	mkdir -p "$R/src/rofull" "$R/src/roempty" "$R/src/rotrash" "$R/src/tree/sub"
	echo f >"$R/src/rofull/f"
	echo f >"$R/src/rotrash/f"
	echo f >"$R/src/tree/sub/f"
	echo f >"$R/src/tree/secret"
	# Sentinels: each refusal shares its job with one of these, which must arrive
	# or go, so the refusal is known to have been tried.
	touch "$R/del/zzb" "$R/del/zzc" "$R/del2/zzb" "$R/src/zm1" "$R/src/zm2" \
		"$R/src/zc1"
	chmod 555 "$R/src/rofull" "$R/src/roempty" "$R/src/rotrash" "$R/src/tree/sub"
	chmod 000 "$R/src/tree/secret"
	# A link in a folder it cannot be removed from, to a read-only folder.
	ln -s "$R/victim" "$R/rolink/ln"
	chmod 555 "$R/rolink" "$R/victim"
	# Inside a namespace of its own the mounts cannot be undone, as for a user.
	MFMT_STAGE=test exec unshare -U --map-user="$MFMT_UID" --map-group="$MFMT_GID" \
		bash "$SELF"
fi

R=$MFMT_ROOT
LOG=$(cd "$(dirname "$SELF")/.." && pwd)/build/files.log
MFM_LOG=${LOG%.log}-mfm.log
. "$(dirname "$SELF")/common.sh"

require_tools pgrep
# The socket comes before the test hooks, the startup command after them.
start_mwc 'touch "$SMOKE_DIR/up"; exec sleep 600'
wait_for "mwc is up" test -e "$SMOKE_DIR/up"
start_vinput
refresh
B=$(global theme border)
T=$(global theme title)

# type_name <letters>: type-ahead to a name; a pause first ends an earlier search.
type_name() {
	local i
	sleep 1.1
	for ((i = 0; i < ${#1}; i++)); do do_input key "${1:i:1}"; done
}

# to_dir <name>: from a directory into a sibling of it.
to_dir() {
	sleep 1.1       # during a search, BackSpace is part of it
	do_input key BackSpace
	type_name "$1"
	do_input key Return
	sleep 0.2
}

# job_settled: wait until no worker runs below mfm any more. That alone says nothing
# about a job that has not started yet: wait for what it does first.
job_settled() {
	local i
	for ((i = 0; i < 100; i++)); do
		pgrep -P "$MFM_PID" >/dev/null || return 0
		sleep 0.1
	done
	fail "the job did not end"
}

no_hidden_left() {
	local left
	left=$(find "$1" -maxdepth 1 -name '.mfm-*' 2>/dev/null)
	[ -z "$left" ] || fail "$2: left behind: $left"
}

# mark_rows <row>...: Ctrl+click them.
mark_rows() {
	local r
	do_input hold ctrl
	for r; do
		do_input move $((LISTX + 40)) "$(row_y "$r")"
		do_input click left
	done
	do_input release ctrl
}

# delete_rows <sentinel> <row>...: mark the rows, Shift+Delete and say yes; done once
# the sentinel among them is gone.
delete_rows() {
	local sentinel=$1
	shift
	mark_rows "$@"
	do_input key shift+Delete
	do_input key y
	wait_for "the delete ran" test ! -e "$sentinel"
	job_settled
}

# mark_names <name>...: type-ahead to each and mark it, with Space once the search is
# over (vinput has no Insert).
mark_names() {
	local n
	for n; do
		type_name "$n"
		sleep 1.1
		do_input key space
	done
}

# ---- delete -----------------------------------------------------------------

# .. outer precious zzb zzc
mfm_start "$R/del"
mark_rows 1 4
do_input key Delete
wait_for "the trash ran" test ! -e "$R/del/zzc"
[ -d "$R/del/outer/inner" ] || fail "outer went to the trash with its mount"
[ -e "$R/del/precious/thesis.txt" ] || fail "the bind mount went with it"
echo "ok: the trash keeps out of a folder with a mount in it"
do_input key Escape
do_input key Escape
delete_rows "$R/del/zzb" 1 3
[ -e "$R/del/precious/thesis.txt" ] || fail "a bind mount of the same file system was emptied"
[ -d "$R/del/outer/inner" ] || fail "outer was deleted around its mount"
echo "ok: delete keeps out of a folder with a bind mount in it"
mfm_quit

# .. box zzb
mfm_start "$R/del2"
delete_rows "$R/del2/zzb" 1 2
[ -e "$R/del2/box/m/keep" ] || fail "a mounted file system was emptied"
echo "ok: delete keeps out of a folder with another file system mounted in it"
mfm_quit

# Renaming a folder into another one changes its "..": a read-only one is opened for
# that and gets its mode back. The trash is the one of the tmpfs, at its top.
mfm_start "$R/src"
type_name rot
do_input key Delete
wait_for "a read-only folder goes to the trash" test ! -e "$R/src/rotrash"
[ "$(stat -c %a "$R/.Trash-$(id -u)/files/rotrash")" = 555 ] ||
	fail "the trashed folder lost its mode"
echo "ok: and keeps its mode there"
mfm_quit

# ---- move -------------------------------------------------------------------

# The full disk frees up again once the failed copy is cleaned up: then zm1 fits.
mfm_start "$R/src"
mark_names big zm1
do_input key ctrl+x
to_dir dst
do_input key ctrl+v
wait_for "the move ran" test -e "$R/dst/zm1"
job_settled
[ ! -e "$R/dst/big" ] || fail "half a file under its real name"
[ "$(stat -c %s "$R/src/big")" = 4194304 ] || fail "the source was damaged"
no_hidden_left "$R/dst" "a move to a full disk"
echo "ok: a move to a full disk leaves the source and no half copy"
mfm_quit

mfm_start "$R/src"
mark_names mnt zm2
do_input key ctrl+x
to_dir wide
do_input key ctrl+v
wait_for "the move ran" test -e "$R/wide/zm2"
job_settled
[ -e "$R/src/mnt/keep" ] || fail "the mounted file system was emptied"
[ ! -e "$R/wide/mnt" ] || fail "the mount point was moved"
no_hidden_left "$R/wide" "moving a mount point"
echo "ok: a mount point is not moved"
mfm_quit

mfm_start "$R/src"
type_name roe
do_input key ctrl+x
to_dir wide
do_input key ctrl+v
wait_for "the read-only folder arrives" test -e "$R/wide/roempty"
wait_for "... and leaves the source" test ! -e "$R/src/roempty"
job_settled
[ "$(stat -c %a "$R/wide/roempty")" = 555 ] || fail "the moved folder lost its mode"
no_hidden_left "$R/wide" "moving a read-only folder"
echo "ok: a move of a read-only folder arrives under its name"
mfm_quit

# A worker that dies must not look like success: the clipboard stays for another try.
SHIM=$SMOKE_DIR/shim
mkdir -p "$SHIM"
REAL_CP=$(command -v cp)
cat >"$SHIM/cp" <<EOS
#!/bin/sh
[ -e "$SHIM/hang" ] && exec sleep 300
exec "$REAL_CP" "\$@"
EOS
chmod +x "$SHIM/cp"
touch "$SHIM/hang"
PATH=$SHIM:$PATH mfm_start "$R/src"
type_name slow
do_input key ctrl+x
to_dir wide
do_input key ctrl+v
sleep 0.5
W=$(pgrep -P "$MFM_PID" | head -1)
[ -n "$W" ] || fail "no worker"
kill -9 -- "-$W"
rm "$SHIM/hang"
sleep 0.5
do_input key ctrl+v
job_settled
wait_for "the clipboard survives a worker that died" test -e "$R/wide/slow"
rm -rf "$R"/wide/.mfm-*
mfm_quit

# A link that cannot be removed after its move stays a link: what it points to
# keeps its mode.
mfm_start "$R/rolink"
do_input key Down
do_input key ctrl+x
to_dir wide
do_input key ctrl+v
wait_for "the link is moved" test -L "$R/wide/ln"
job_settled
[ "$(stat -c %a "$R/victim")" = 555 ] || fail "the link's target was made writable"
echo "ok: and what it points to keeps its mode"
mfm_quit

# ---- copy -------------------------------------------------------------------

mfm_start "$R/src"
type_name rof
do_input key ctrl+c
to_dir wide
do_input key ctrl+v
wait_for "the read-only folder is copied" test -e "$R/wide/rofull/f"
job_settled
[ "$(stat -c %a "$R/wide/rofull")" = 555 ] || fail "the copy lost its mode"
no_hidden_left "$R/wide" "copying a read-only folder"
echo "ok: a copy of a read-only folder arrives under its name, still read-only"
mfm_quit

mfm_start "$R/src"
mark_names tree zc1
do_input key ctrl+c
to_dir wide
do_input key ctrl+v
wait_for "the copy ran" test -e "$R/wide/zc1"
job_settled
[ ! -e "$R/wide/tree" ] || fail "a failed copy under its real name"
no_hidden_left "$R/wide" "a failed copy with a read-only folder in it"
echo "ok: a failed copy is cleaned up, read-only folders included"
mfm_quit

# A read-only folder with something in it, to another file system: it arrives
# complete, under its name, and the original goes.
mfm_start "$R/src"
type_name rof
do_input key ctrl+x
to_dir dst
do_input key ctrl+v
wait_for "the read-only folder arrives with its file" test -e "$R/dst/rofull/f"
wait_for "... and the original goes" test ! -e "$R/src/rofull"
job_settled
[ "$(stat -c %a "$R/dst/rofull")" = 555 ] || fail "the moved folder lost its mode"
no_hidden_left "$R/dst" "moving a read-only folder with a file in it"
mfm_quit

# ---- the directory on screen ------------------------------------------------

mfm_start "$R/vanish/sub"
chmod 000 "$R/vanish/sub"
sleep 1.5
do_input key F7
do_input key x
do_input key Return
file_appears "chmod 000 on the folder sends mfm to its parent" "$R/vanish/x"
chmod 755 "$R/vanish/sub"
mfm_quit

echo "PASS"
