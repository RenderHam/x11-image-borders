/* xib configuration -- rebuild after editing */

#ifndef XIB_CONFIG_H
#define XIB_CONFIG_H

#include <stdint.h>

/* Border images. PNG only. Relative paths are tried against the working
 * directory first, then the executable's directory. */
static const char *focused_image   = "/home/kuuki/Project/x11-image-borders/assets/hud-512-focused.png";
static const char *unfocused_image = "/home/kuuki/Project/x11-image-borders/assets/hud-512-unfocused.png";

/* Required square size of both border images, in pixels. xib refuses to
 * start if a loaded PNG has another size. Set it to the template you use:
 * 40, 128, 256, 512, ... (0 disables the check). Templates are generated
 * with `./xib --gen-assets`. */
static const int image_size = 512;

/* Thickness of the ring drawn outside every client, in pixels. Used only for
 * sides below that are left at 0. */
static const int border_thickness = 10;

/* Space outside the client on each side, in pixels. Set to the width of the
 * corresponding band in the artwork. 0 means "use border_thickness". */
static const int border_top    = 21;
static const int border_right  = 21;
static const int border_bottom = 21;
static const int border_left   = 21;

/* Source pixels used for each 9-slice corner when a side is left at 0.
 * With all four sides set above, each side uses its own width as the source
 * slice too, so the artwork renders 1:1. */
static const int corner_slice = 10;

/* How the middle strips are drawn:
 *   0 = stretch
 *   1 = tile */
static const int edge_mode = 1;

/* Used when an image is missing or fails to load. 0xAARRGGBB. */
static const uint32_t focused_fallback   = 0xffffffff;
static const uint32_t unfocused_fallback = 0xff6e7681;

/* WM_CLASS names that never get a border. */
static const char *exclude_classes[] = {
    "Polybar",
    "slop",
    "Bspwm",
};

/* Signal-poll timeout, milliseconds. */
static const int poll_interval_ms = 250;

#endif /* XIB_CONFIG_H */
