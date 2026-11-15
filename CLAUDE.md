# mfm

Project rules. The rules for all C projects, mwc as their reference, are in the global
CLAUDE.md.


## Build and test

| Target | |
|---|---|
| `make` | mfm |
| `make test` | `tests/ui.sh` and `tests/files.sh` under a headless mwc; logs in `build/` |
| `make test-docker` | the same in a Podman container, image `mfm-test` |

Use `make test-docker`: the host lacks grim, and Ubuntu's AppArmor blocks the user
namespaces `tests/files.sh` needs, which skips it. `make test` needs a built mwc with
`build/vinput` and `build/pngtool` in `MWC_DIR` (default `../mwc`).

Polling helpers: `check`, `wait_for`, `pixel_is`, `reload_mwc` from mwc's
`tests/lib.sh`; `mfm_start`, `mfm_quit`, `find_rows`, `wait_shot_pixel` in
`tests/common.sh`.


## Rules

- mfm-x11 is outdated and not maintained; nothing is carried over to it.
- Keys and settings are documented in `mfm.1`; the README keeps one line each.
