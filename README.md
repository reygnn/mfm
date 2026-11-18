# mfm

mfm is a minimal file manager for Wayland in the look of the Motif window manager,
made to go with [mwc](https://github.com/reygnn/mwc). One C file on libwayland,
xkbcommon, Cairo and Pango, no toolkit. It takes mwc's colours, browses, copies, moves,
renames, trashes and deletes, and leaves the rest to a terminal.


## Build

On Ubuntu 26.04; mfm needs glibc 2.34 or later:

    sudo apt install build-essential pkg-config libwayland-dev wayland-protocols \
        libxkbcommon-dev libcairo2-dev libpango1.0-dev
    make
    sudo make install

`make install` puts `mfm` in `/usr/local/bin`, adds a desktop entry and the manual
page `mfm.1`. Files are opened with `xdg-open` from xdg-utils unless `MFM_OPEN` names
another program.


## Run

    mfm [-h | -v] [directory | file]

Without an argument mfm starts in the current directory; given a file, it opens its
folder and selects it. `-h` prints the usage, `-v` the version, which is set in
`version.h`. To start it with mwc, in `~/.config/mwc/autostart`:

    mfm "$HOME" &


## Keys

| Keys | Action |
|---|---|
| arrows, PgUp, PgDn, Home, End | move |
| Return, Right | open |
| Backspace, Left, Alt+Up | parent folder |
| Alt+Home | home folder |
| letters | jump to a name |
| Ctrl+L | enter a path |
| F2 | rename |
| F7, Ctrl+Shift+N | new folder |
| Ctrl+H | hidden files on or off |
| Ctrl+1, 2, 3 | sort by name, size or date; again: reversed |
| Ctrl+R, F5 | reload |
| Ctrl+T | terminal in this folder |
| Space, Insert | mark and move on |
| Ctrl+A | mark or unmark all |
| Ctrl+C, Ctrl+X | remember for copying or moving |
| Ctrl+V | paste here |
| Ctrl+Shift+C | the paths into the session's clipboard |
| Ctrl+S | send with `MFM_SEND` |
| Delete | to the trash |
| Shift+Delete | delete, after asking |
| Esc | clear the search, the message or the marks |
| Ctrl+Q, Ctrl+W, Alt+F4 | quit |

Commands act on the marked entries, else on the selected one. Ctrl+C and Ctrl+X keep
the entries inside mfm; Ctrl+Shift+C offers them to other programs, for example to
paste files into a Firefox upload field.


## Mouse

| Where | Action |
|---|---|
| a row | click: select; double click: open; Ctrl+click: mark; Shift+click: mark up to the selection |
| the list | button 3: context menu; wheel: scroll |
| the scroll bar | drag, or click into the trough |
| the arrow button, button 8 | parent folder |
| the path field | click: edit |


## Configuration

`~/.config/mfm/mfmrc` uses the syntax of mwc's mwmrc. A variable in the environment wins
over its entry.

    Mfm*send:        adb-push-download
    Mfm*terminal:    mwt
    Mfm*sort:        date
    Mfm*showHidden:  true

| Resource | Variable | Default | |
|---|---|---|---|
| `open` | `MFM_OPEN` | `xdg-open` | opens files, the path as last argument |
| `send` | `MFM_SEND` | none | Ctrl+S, the paths as last arguments |
| `terminal` | `MFM_TERMINAL` | `MWC_TERMINAL`, else the first found | Ctrl+T |
| `font` | `MFM_FONT` | `Sans 10` | a Pango font name |
| `background`, `activeBackground` | `MWC_INACTIVE`, `MWC_ACTIVE` | mwc's, from its mwmrc | window and selection |
| `foreground`, `activeForeground` | | mwc's, else black or white | their text |
| `sort`, `sortReverse` | | `name`, `false` | the order at start: `name`, `size` or `date` |
| `showHidden` | | `false` | hidden files at start |

The commands are split at spaces, with quotes grouping words, and run without a shell.
The manual page has the details on keys, the context menu and how files are copied,
moved and trashed. Known limits are in [CAVEATS.md](CAVEATS.md).


## How this is built

mfm is developed with Claude (Anthropic) as a coding assistant. I set the scope and the
design and review the changes; Claude writes most of the code and the tests. The checks:

* `make test` drives mfm under a headless mwc with a simulated mouse and keyboard and
  checks what lands on disk and on screen, and runs copy, move, trash and delete
  against real mounts, read-only folders and a full disk.
* Every fix comes with a test that fails without it, checked by taking the fix out.
* What is left open is written down rather than hidden: see CAVEATS, and the reviews
  in `docs/history`.


## Testing

    make test

runs mfm under a headless mwc with a simulated mouse and keyboard and checks files and
screenshots (`tests/ui.sh`), and file operations with real mounts in a private
namespace (`tests/files.sh`, which fails without user namespaces). It needs a built mwc
with `build/vinput` and `build/pngtool` in `MWC_DIR` (by default `../mwc`), grim and
pgrep.

    make test-docker

does the same in a Podman or Docker container, for hosts without grim or mwc's
dependencies, or where AppArmor blocks user namespaces.


## License

MIT, see [LICENSE](LICENSE).
