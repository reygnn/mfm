# mtk

Project rules. The rules for all C projects, mwc as their reference, are in the global
CLAUDE.md.


## Build and test

Build and test inside mwc's `mwc-dev` distrobox; the host lacks grim.
`distrobox enter mwc-dev -- bash -lc 'cd ~/data/C-Projects/mtk && make check test'`.
The targets are in README.md. `make test` drives `build/mtkdemo-asan` in a headless
mwc with its `tests/lib.sh`, `build/vinput`, `build/pngtool` and `build/xdgc`; when
those can't do what a test needs, extend them in mwc.

Logic worth testing goes to `color.c`, `rc.c`, `draw.c` or `input.c`, tested by
`tests/unit/NAME_test.c` linked with `NAME.c` (and `UNIT_WITH_NAME`). `wl.c` only
asks them and talks Wayland; what it does is tested through `demo/mtkdemo.c` and
`tests/ui.sh`. A new feature of `wl.c` gets a use in the demo and a check there.


## Rules

- Colours, shadows and text colour are mwc's: `mwc/color.c` and `mwc/color.h` are
  its `src/color.c` and `color.h`, unchanged, copied anew when mwc changes them.
  `mwc_color.h` gives them `mtk_` names, `make check` compares them with mwc's. The
  cases of mwc's `tests/unit/color_test.c` are in `tests/unit/color_test.c`.
- Every public name starts with `mtk_` or `MTK_`: the clients have their own `mix`,
  `fill` and `bevel` until they are moved over.
- mtk is vendored in the clients with git subtree (README.md). A change to `mtk.h`
  that breaks a client is made in that client too, in its repository.
- mtk has no `-v` and no gitversion: `MTK_VERSION` in `mtk.h` is raised when the API
  changes, the commit is the client's.
