#!/bin/bash
# What file operations do around mounts, full disks and read-only directories, with
# real mounts in a private namespace, driven through mfm under a headless mwc. Needs
# unprivileged user namespaces, and fails without them. See common.sh for the rest of
# what it needs. Output goes to build/files.log and build/files-mfm.log.
#
# It runs in three stages: this script, again as root of a user namespace to set up
# the mounts, and again as the normal user in a namespace inside that one.
SELF=$(cd "$(dirname "$0")" && pwd)/$(basename "$0")

if [ -z "${MFMT_STAGE:-}" ]; then
	# Not a skip: a green run must have tried the file operations.
	unshare -Urm true 2>/dev/null ||
		{ echo "FAIL: no user namespaces here (make test-docker has them)"; exit 1; }
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
	echo u >"$R/src/umask640"
	chmod 640 "$R/src/umask640"
	# File systems of their own with a shared trash an administrator made, sticky as
	# it has to be, and one that is not sticky and so must not be used.
	mkdir -p "$R/shared" "$R/nosticky"
	mount -t tmpfs none "$R/shared" || exit 1
	mount -t tmpfs none "$R/nosticky" || exit 1
	mkdir "$R/shared/.Trash" "$R/nosticky/.Trash"
	chmod 1777 "$R/shared/.Trash"
	chmod 777 "$R/nosticky/.Trash"
	echo s >"$R/shared/s1"
	echo n >"$R/nosticky/n1"
	chmod 777 "$R/shared" "$R/nosticky"
	# An overlay with its lower layer on another file system than its upper one:
	# files from below show the lower one's device, folders the overlay's.
	mkdir -p "$R/ovll/sub" "$R/ovlu" "$R/ovl"
	echo f >"$R/ovll/f1"
	echo g >"$R/ovll/sub/g1"
	chmod 777 "$R/ovll" "$R/ovll/sub"
	mount -t tmpfs none "$R/ovlu" || exit 1
	mkdir "$R/ovlu/up" "$R/ovlu/work"
	chmod 777 "$R/ovlu/up"
	mount -t overlay none -o "lowerdir=$R/ovll,upperdir=$R/ovlu/up,workdir=$R/ovlu/work" \
		"$R/ovl" || exit 1
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
	end_search
	for ((i = 0; i < ${#1}; i++)); do do_input key "${1:i:1}"; done
}

# to_dir <name>: from a directory into a sibling of it.
to_dir() {
	end_search      # during a search, BackSpace is part of it
	do_input key BackSpace
	type_name "$1"
	do_input key Return
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
		end_search
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
# Only what failed stays to paste again: a new zm2 is not taken along. The
# status line turns red when the paste is over.
touch "$R/src/zm2"
do_input move $((LISTX + 40)) "$(row_y 0)"
do_input click left
no_err "the error is cleared"
do_input key ctrl+v
wait_err "the second paste fails on the mount point"
job_settled
[ -e "$R/src/zm2" ] || fail "a new entry under a moved name was moved too"
echo "ok: the clipboard keeps only what failed"
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
[ -e "$SHIM/hang" ] && { touch "$SHIM/hung"; exec sleep 300; }
exec "$REAL_CP" "\$@"
EOS
chmod +x "$SHIM/cp"
touch "$SHIM/hang"
PATH=$SHIM:$PATH mfm_start "$R/src"
type_name slow
do_input key ctrl+x
to_dir wide
do_input key ctrl+v
file_appears "the worker hangs in cp" "$SHIM/hung"
W=$(pgrep -P "$MFM_PID" | head -1)
[ -n "$W" ] || fail "no worker"
kill -9 -- "-$W"
rm "$SHIM/hang"
wait_err "mfm sees the worker die"
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

# A umask that takes the owner's bits must not look like FAT: the temporary folder
# is made usable, and the copy keeps its mode.
old_umask=$(umask)
umask 0277
mfm_start "$R/src"
umask "$old_umask"
type_name uma
do_input key ctrl+c
to_dir wide
do_input key ctrl+v
wait_for "a copy under umask 0277" test -e "$R/wide/umask640"
job_settled
[ "$(stat -c %a "$R/wide/umask640")" = 640 ] || fail "the copy lost its mode"
echo "ok: ... keeps its mode"
mfm_quit

# ---- trash at the top of a file system ---------------------------------------

mfm_start "$R/shared"
do_input key Down
do_input key Delete
wait_for "a file goes to the trash on its file system" test ! -e "$R/shared/s1"
[ -e "$R/shared/.Trash/$(id -u)/files/s1" ] || fail "not in the shared .Trash/\$uid"
echo "ok: ... into the shared .Trash/\$uid"
mfm_quit

mfm_start "$R/nosticky"
do_input key Down
do_input key Delete
wait_for "a file goes to the trash on its file system" test ! -e "$R/nosticky/n1"
[ -e "$R/nosticky/.Trash-$(id -u)/files/n1" ] || fail "not in .Trash-\$uid"
[ ! -e "$R/nosticky/.Trash/$(id -u)" ] || fail "a .Trash that is not sticky was used"
echo "ok: ... into .Trash-\$uid when .Trash is not sticky"
mfm_quit

# On the overlay, files from the lower layer are trashed like any, to the top of the
# overlay, also from a folder below it.
mfm_start "$R/ovl"
type_name f1
do_input key Delete
wait_for "a file from an overlay's lower layer goes to the trash" test ! -e "$R/ovl/f1"
[ -e "$R/ovl/.Trash-$(id -u)/files/f1" ] || fail "f1 is not in the overlay's trash"
type_name sub
do_input key Return
type_name g1
do_input key Delete
wait_for "... from a folder below the top too" test ! -e "$R/ovl/sub/g1"
[ -e "$R/ovl/.Trash-$(id -u)/files/g1" ] || fail "g1 is not in the trash at the top"
[ ! -e "$R/ovl/sub/.Trash-$(id -u)" ] || fail "a trash was made in the folder"
echo "ok: ... into the trash at the overlay's top"
mfm_quit

# ---- the directory on screen ------------------------------------------------

# The next poll takes mfm up; until then F7 fails in the folder it cannot write.
mfm_start "$R/vanish/sub"
chmod 000 "$R/vanish/sub"
for ((i = 0; i < 50; i++)); do
	do_input key F7
	do_input key x
	do_input key Return
	[ -e "$R/vanish/x" ] && break
	sleep 0.1
done
file_appears "chmod 000 on the folder sends mfm to its parent" "$R/vanish/x"
chmod 755 "$R/vanish/sub"
mfm_quit

echo "PASS"
