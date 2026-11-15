# Caveats

Known limits of mfm that are left as they are, with what they mean in use.


## Renaming without replacing

mfm never overwrites: moves within a file system are a rename that fails on an existing
name, and copies land under a hidden name first. On file systems without an atomic
rename that refuses to replace, such as NFS, this does not hold for folders, nor for
files where hard links fail too.


## Mount points

Permanent deletion is `rm -rf --one-file-system`. mfm refuses to delete a mount point
or a folder with something mounted in it, bind mounts from the same file system
included, since `rm` would empty what is mounted. A mount point cannot be moved, nor
can a folder with something mounted in it be moved to another file system. Copying
works.


## Input methods

Compose and dead keys work, following the locale. Input methods such as ibus or fcitx
do not: they need text-input-v3, which mwc does not offer.


## Compositors

mfm needs xdg-shell. It asks for a frame through xdg-decoration; compositors without
it, such as Mutter, show mfm without one. Wayland has no window icon; the compositor
sees the app_id `mfm`.

It draws at the integer scale the compositor asks for with wl_compositor 6. A
fractional scale is drawn at the next integer and scaled down by the compositor; an
older compositor never asks, and there the window stays at scale 1 and is blurred.

Dragging with a button held needs the compositor's implicit grab. Without it mfm does
not see a release outside its window: the scroll bar stops at the edge, and in the
context menu press, drag and release does not run the command, a click still does.

Ctrl+Shift+C needs ext-data-control-v1, which mwc, sway and other wlroots compositors
offer. A small helper process of mfm holds the clipboard, since on Wayland the program
that copied hands the data out. It stays after mfm quits, until something else is
copied or the compositor ends.
