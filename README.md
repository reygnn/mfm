# mtk

mtk is the Motif toolkit of the clients made for [mwc](https://github.com/reygnn/mwc):
mfm, mrun, deskControl, digitalClock, pulseControl and osDisplay. It holds what each of
them had in a copy of its own: the Wayland window on wl_shm buffers, the seat with
keyboard, compose, key repeat, pointer and cursor, an event loop, mwc's colours from
the environment and the mwmrc, and mwm's bevels and borders on Cairo. C on libwayland,
xkbcommon, Cairo and Pango.

It is no widget set. A client draws its window itself and handles its own events;
mtk tells it when, at which scale and with which colours.


## Colours

mtk takes mwc's colours as mwc does: `MWC_INACTIVE` and `MWC_ACTIVE`, else
`Mwm*background` and `Mwm*activeBackground` from `~/.config/mwc/mwmrc`, else Motif's
`#aeb2c3` and `#7387ad`. The shadows and the default text follow Motif 2.3.8's
`CalculateColorsRGB()`, as mwc's own frames do, so a client and its frame match.
`mtk_best_contrast()`, `mtk_dimmed()` and `mtk_readable()` are there for text that
must reach WCAG's 4.5:1.


## Use

mtk is vendored, not installed: a client has it in `mtk/` with git subtree and
builds it in.

    git subtree add --prefix=mtk https://github.com/reygnn/mtk main --squash
    git subtree pull --prefix=mtk https://github.com/reygnn/mtk main --squash

In the client's Makefile:

    MTK_DIR = mtk
    include $(MTK_DIR)/mtk.mk

    app: app.c $(MTK_DEPS)
    	$(CC) $(MTK_CFLAGS) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ app.c \
    		$(MTK_SRC) $(MTK_LIBS) $(LDLIBS)

`MTK_HEADLESS` are the modules without Wayland, for a client's own unit tests. The
API is in `mtk.h`; `demo/mtkdemo.c` uses all of it.

A client needs:

    sudo apt install build-essential pkg-config libwayland-dev wayland-protocols \
        libxkbcommon-dev libcairo2-dev libpango1.0-dev


## Tests

| Target | |
|---|---|
| `make` | `build/mtkdemo` |
| `make unit` | `tests/unit`: a program per module without Wayland, under ASan and UBSan |
| `make check` | the units, and the demo built under ASan and UBSan with `-Werror` |
| `make test` | `tests/ui.sh`: the demo in a headless mwc, under ASan, checked in screenshots |
| `make analyze` | `gcc -fanalyzer`, findings as errors |
| `make lint` | C lines over 100 columns |

`make test` needs a built mwc in `MWC_DIR` (default `../mwc`) with `build/vinput`,
`build/pngtool` and `build/xdgc`, grim, and the Adwaita cursor theme.


## License

MIT, see `LICENSE`.
