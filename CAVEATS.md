# Caveats

Limits of mtk that are known and left as they are.


## Windows

Popups open at a point of their parent, below and right of it, flipped or slid by
the compositor to stay on the screen; other placements are not offered. Their size
is fixed when they open.

Buffers are XRGB8888, so a window is opaque.

It needs wl_compositor 4. It draws at the integer scale the compositor asks for with
wl_compositor 6. A fractional scale is drawn at the next integer and scaled down by
the compositor; a compositor with wl_compositor 4 or 5 never asks, and there the
window stays at scale 1. A scale above 16 is drawn at 16. A window too large for
Cairo's 32767 pixels at its scale is drawn at a lower one and enlarged by the
compositor; one past 32767 surface pixels stays black.

A lost connection, a failed buffer allocation and a failed poll print why and exit
with 1: the clients have nothing to save.


## Input

One seat at a time: the oldest announced, the next one when it goes.

When the keyboard leaves, the modifiers held are forgotten, and when the pointer
goes, its window hears a leave. mwc sends the keyboard's state with every enter and
releases held buttons first, so its tests cannot show either.

The cursor is loaded from the theme at its size times the scale, the theme's nearest
size. When that size is no multiple of the scale, a viewport shows it at its size
over the scale; without wp_viewporter it is shown at the largest scale below that
divides it, and so a little larger. `XCURSOR_SIZE` above 256 counts as 256.

Scrolling comes as wl_pointer 7 sends it; high resolution wheels (`axis_value120`)
are not asked for. A touchpad's distance makes a step every 15 pixels, at most 20 a
frame; a flood beyond that is dropped, not carried over.


## Colours

Colours are 8 bits a channel. A colour given in 16 bits (`rgb:ffff/8000/0`) is
rounded to 8 before its shadows are made; mwc keeps all 16, so such a colour's shadow
can differ by one from mwc's frame.


## The loop

At most 16 watched fds and 32 timers. Timers count milliseconds on
`CLOCK_MONOTONIC`.
