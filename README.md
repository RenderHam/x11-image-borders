# xib — image borders for X11 windows

`xib` draws image-based (9-slice PNG) borders around managed windows. It is a
standalone X11 daemon, not a picom fork. Frames are mapped above clients, and
their input shape is empty so the border is visible but the client still
receives pointer events.

## Requirements

- X11, single screen
- a compositor with a color management window: `picom --config ~/.config/picom/picom.conf`
- `cairo`, `libxcb` (`xcb-render`, `xcb-shape`)
- nothing else: `xib` writes its own editable template PNGs

Arch:

```sh
sudo pacman -S cairo libxcb
```

## Build and run

```sh
make
./xib            # run in the foreground
```

Autostart next to `picom` (for bspwm, in `~/.config/bspwm/bspwmrc`):

```sh
picom &
xib &
```

Commands:

```sh
./xib --render-test     # render to /tmp/xib-render-{focused,unfocused}.png
./xib --gen-assets      # generate templates: 40, 128, 256, 512
./xib --help
```

Signals:

```sh
pkill xib        # stop
pkill -HUP xib   # reload PNGs without restarting
```

## Configuration (`config.h`)

Everything is compiled in; rebuild after editing.

| Option | Default | Meaning |
| --- | --- | --- |
| `focused_image` | `assets/hud-512-focused.png` | focused client border |
| `unfocused_image` | `assets/hud-512-unfocused.png` | other clients |
| `image_size` | `512` | expected square PNG size; `0` disables the check |
| `border_thickness` | `10` | fallback ring width outside the client |
| `border_top` / `border_right` / `border_bottom` / `border_left` | `21` each | per-side outer width |
| `corner_slice` | `10` | source corner size for sides left at `0` |
| `edge_mode` | `1` | `0` stretch, `1` tile the edge strips |
| `focused_fallback` | `0xffffffff` | used if the focused PNG fails to load |
| `unfocused_fallback` | `0xff6e7681` | same for the unfocused PNG |
| `exclude_classes[]` | `Polybar`, `slop`, `Bspwm` | never get a border |

Images must be square and exactly `image_size` pixels. If not, `xib` exits at
startup and prints the size it found plus the value to use.

Per-side values override `border_thickness`. They are also the source slice
widths for that side, so artwork with asymmetric bands renders 1:1. A side
left at `0` falls back to `border_thickness` for the frame and `corner_slice`
for the source.

## Images and templates

9-slice PNG contract:

- square source image, PNG with alpha
- corners use a source patch the size of the side
- edge strips stretch or tile to fill the window edge
- the centre is cleared to transparent for real windows

Built-in templates are editable, no Python/ImageMagick needed:

```sh
./xib --gen-assets            # 40, 128, 256, 512
./xib --gen-assets 128        # one size
./xib --gen-assets 40 96 256  # custom sizes
```

Files are `assets/hud-<size>-focused.png` / `hud-<size>-unfocused.png`. On
first run, if the asset directory has no PNGs, `xib` generates the default
template set.

The default `hud-512` pair uses a 3px outer line with a 7px cut and the rest
of the canvas as editable space. Keep `border_thickness = corner_slice = 10`
for the normal 10px gap look; larger borders require larger bspwm gaps.

## Picom

`xib` frames need the compositor to ignore them. Add to `picom.conf`:

```conf
opacity-rule = ["100:class_g = 'xib'"];
focus-exclude = ["class_g = 'xib'"];
shadow-exclude = ["class_g = 'xib'"];
rounded-corners-exclude = ["class_g = 'xib'"];
corner-radius = 0;
```

Also note: `active-opacity` must be `<= 1`; a value like `1.5` is invalid and
picom clamps or misbehaves.

## Behaviour

- tracked: `_NET_CLIENT_LIST`, root `SubstructureNotify`, `_NET_ACTIVE_WINDOW`
- frames follow move/resize, including floating windows
- fullscreen/hidden clients hide their frame
- frames are stacked above clients with an empty input shape
- override-redirect windows, non-normal/dialog/utility types and
  `exclude_classes[]` are skipped

## Files

```
xib.c                     daemon
config.h                  compiled configuration
Makefile                  build/install
assets/hud-512-focused.png    default focused template
assets/hud-512-unfocused.png  default unfocused template
assets/hud-{40,128,256,512}-*.png  other templates
assets/test.png               original full-screen HUD artwork
tools/make-hud-assets.py      optional Python generator
tools/make-assets.sh          sample image generator
```

## Development

```sh
make            # build
./xib --render-test
```

`make clean && make` rebuilds from scratch. `xib` is warning-free with
`-Wall -Wextra`.
