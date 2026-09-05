# Notice

## Source code

The source code in this repository is licensed under the MIT License (see `LICENSE`),
except where a file states otherwise.

This project is built on top of two open-source reference repositories:

- `folotoy/ai-passport` — hardware baseline and BSP for the FoloToy AI Passport.
- `PhoenixZHC/FoloToy-EVA-musicplayer` — EVA-style LVGL application example for the
  same hardware. The `components/bsp` board-support package and several project
  conventions (partitions, sdkconfig defaults, build layout) are derived from it.

## Third-party content

The MIT License does not grant rights to third-party content. This includes, but
is not limited to:

- Evangelion / NERV names, logos, characters, and trade dress
- commercial fonts
- any copyrighted sample task text or images you add yourself

This is an unofficial fan-made firmware project. It is not affiliated with,
endorsed by, or sponsored by the owners of Evangelion, NERV, FoloToy, or any
other rights holders.

If you publish a fork, only include assets you have the right to redistribute.
The UI text bitmaps committed in `main/todo_text_assets.c` are generated from
host fonts at build time by `tools/prepare_todo_text.py`; regenerate or replace
them with fonts you are licensed to redistribute.
