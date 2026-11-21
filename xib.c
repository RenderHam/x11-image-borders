/* xib - image based window borders for X11
 *
 * Draws an override-redirect ARGB frame window directly below every managed
 * client and paints a 9-slice PNG ring around it. Needs a compositor.
 */

#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <cairo/cairo-xcb.h>
#include <cairo/cairo.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <xcb/render.h>
#include <xcb/shape.h>
#include <xcb/xcb.h>

#include "config.h"

typedef struct {
    int left, top, right, bottom;
} edges_t;

/* Effective space outside each client, resolved from config.h at startup. */
static edges_t insets;          /* destination widths */
static edges_t src_insets;      /* source slice sizes in the PNG */

typedef struct {
    xcb_window_t win;   /* client */
    xcb_window_t frame; /* our override-redirect window */
    int16_t x, y;
    uint16_t w, h;
    uint16_t cw, ch;
    uint16_t bw;
    int mapped;
    int focused;
    xcb_pixmap_t pixmap;
    cairo_surface_t *surface;
    uint16_t rendered_w, rendered_h;
    int rendered_focused;
    int cache_valid;
} client_t;

enum {
    A_NET_CLIENT_LIST,
    A_NET_ACTIVE_WINDOW,
    A_NET_WM_STATE,
    A_NET_WM_STATE_FULLSCREEN,
    A_NET_WM_STATE_HIDDEN,
    A_NET_WM_WINDOW_TYPE,
    A_NET_WM_WINDOW_TYPE_NORMAL,
    A_NET_WM_WINDOW_TYPE_DIALOG,
    A_NET_WM_WINDOW_TYPE_UTILITY,
    A_NET_WM_WINDOW_OPACITY,
    A_UTF8_STRING,
    A_NET_WM_NAME,
    A_XIB_PID,
    A_COUNT
};

static const char *atom_names[A_COUNT] = {
    "_NET_CLIENT_LIST",
    "_NET_ACTIVE_WINDOW",
    "_NET_WM_STATE",
    "_NET_WM_STATE_FULLSCREEN",
    "_NET_WM_STATE_HIDDEN",
    "_NET_WM_WINDOW_TYPE",
    "_NET_WM_WINDOW_TYPE_NORMAL",
    "_NET_WM_WINDOW_TYPE_DIALOG",
    "_NET_WM_WINDOW_TYPE_UTILITY",
    "_NET_WM_WINDOW_OPACITY",
    "UTF8_STRING",
    "_NET_WM_NAME",
    "_XIB_PID",
};

static xcb_connection_t *conn;
static xcb_screen_t *screen;
static int screen_number;
static xcb_visualtype_t *argb_visual;
static xcb_colormap_t colormap;
static xcb_gcontext_t gc;
static xcb_atom_t atoms[A_COUNT];

static client_t *clients;
static size_t nclients, client_cap;

static cairo_surface_t *img_focused, *img_unfocused;
static xcb_window_t focused_win = XCB_NONE;

static volatile sig_atomic_t running = 1;
static volatile sig_atomic_t reload_requested = 0;
static int debug_enabled;

#define DBG(...) do { if (debug_enabled) fprintf(stderr, "xib: " __VA_ARGS__); } while (0)

static void present(client_t *c);
static void update_client(client_t *c);
static void remove_client_at(size_t i);
static void sync_client_list(void);

static void fail(const char *msg)
{
    fprintf(stderr, "xib: %s\n", msg);
    exit(1);
}

/* ------------------------------------------------------------------ */
/* Border template generation
 *
 * Templates keep the original 40x40 HUD line work at every size: a 3px rule
 * and a 7px corner cut. Only the canvas gets bigger, giving you room to draw
 * decorations on the corners and sides. They are written with Cairo, so
 * generating them needs neither Python nor ImageMagick at runtime.
 */

/* Fixed at hud-40 scale -- do not scale these with the canvas. */
#define HUD_LINE_WIDTH 3
#define HUD_CORNER_CUT 7

static const int hud_template_sizes[] = { 40, 128, 256, 512 };
#define N_HUD_TEMPLATE_SIZES \
    (sizeof hud_template_sizes / sizeof hud_template_sizes[0])

static char *dup_dirname(const char *path)
{
    const char *slash = strrchr(path, '/');
    if (!slash)
        return strdup(".");
    if (slash == path)
        return strdup("/");
    size_t n = (size_t)(slash - path);
    char *dir = malloc(n + 1);
    if (!dir)
        return NULL;
    memcpy(dir, path, n);
    dir[n] = '\0';
    return dir;
}

/* Directory that the configured images live in. Mirrors load_image():
 * relative paths resolve against the working directory first, then the
 * executable's directory. */
static char *resolve_asset_dir(void)
{
    char *dir = dup_dirname(focused_image);
    if (!dir || focused_image[0] == '/')
        return dir;

    struct stat st;
    if (stat(dir, &st) == 0 && S_ISDIR(st.st_mode))
        return dir;

    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n > 0) {
        exe[n] = '\0';
        char *slash = strrchr(exe, '/');
        if (slash) {
            *slash = '\0';
            size_t need = strlen(exe) + strlen(dir) + 2;
            char *full = malloc(need);
            if (full) {
                snprintf(full, need, "%s/%s", exe, dir);
                if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) {
                    free(dir);
                    return full;
                }
                free(full);
            }
        }
    }
    return dir;
}

static int mkdir_p(const char *path)
{
    char tmp[PATH_MAX];
    if (path == NULL)
        return -1;
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof tmp)
        return -1;
    memcpy(tmp, path, len + 1);

    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
            return -1;
        *p = '/';
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

static int dir_has_png(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d)
        return 0;
    int found = 0;
    struct dirent *e;
    while (!found && (e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        size_t n = strlen(e->d_name);
        if (n > 4 && strcasecmp(e->d_name + n - 4, ".png") == 0)
            found = 1;
    }
    closedir(d);
    return found;
}

static int join_path(char *out, size_t n, const char *dir, const char *name)
{
    int r = (dir[0] == '\0' || strcmp(dir, ".") == 0)
                ? snprintf(out, n, "%s", name)
                : snprintf(out, n, "%s/%s", dir, name);
    return r > 0 && (size_t)r < n;
}

static int write_hud_png(const char *path, int size, uint32_t color)
{
    if (size < 8 || size > 4096)
        return 0;

    int thick = HUD_LINE_WIDTH;
    int cut = HUD_CORNER_CUT;
    if (cut > size / 2)
        cut = size / 2;

    uint32_t *data = calloc((size_t)size * (size_t)size, sizeof *data);
    if (!data) {
        fprintf(stderr, "xib: out of memory generating %s\n", path);
        return 0;
    }

    const double r2 = 1.4142135623730951;
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            /* Outside the chamfered square? */
            if (x + y < cut ||
                (size - 1 - x) + y < cut ||
                x + (size - 1 - y) < cut ||
                (size - 1 - x) + (size - 1 - y) < cut)
                continue;

            double cx = x + 0.5, cy = y + 0.5;
            double d = cx;
            if (cy < d)
                d = cy;
            if (size - cx < d)
                d = size - cx;
            if (size - cy < d)
                d = size - cy;
            double diag;
            diag = (cx + cy - cut) / r2;
            if (diag < d)
                d = diag;
            diag = ((size - cx) + cy - cut) / r2;
            if (diag < d)
                d = diag;
            diag = (cx + (size - cy) - cut) / r2;
            if (diag < d)
                d = diag;
            diag = ((size - cx) + (size - cy) - cut) / r2;
            if (diag < d)
                d = diag;

            if (d < thick)
                data[(size_t)y * (size_t)size + (size_t)x] = color;
        }
    }

    cairo_surface_t *s = cairo_image_surface_create_for_data(
        (unsigned char *)data, CAIRO_FORMAT_ARGB32, size, size, size * 4);
    if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "xib: cannot create surface for %s: %s\n", path,
                cairo_status_to_string(cairo_surface_status(s)));
        cairo_surface_destroy(s);
        free(data);
        return 0;
    }
    cairo_surface_mark_dirty(s);
    cairo_status_t st = cairo_surface_write_to_png(s, path);
    cairo_surface_destroy(s);
    free(data);
    if (st != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "xib: cannot write %s: %s\n", path,
                cairo_status_to_string(st));
        return 0;
    }
    return 1;
}

static int write_hud_pair(const char *dir, const char *focused_name,
                          const char *unfocused_name, int size)
{
    char path[PATH_MAX];
    if (!join_path(path, sizeof path, dir, focused_name) ||
        !write_hud_png(path, size, focused_fallback))
        return 0;
    if (!join_path(path, sizeof path, dir, unfocused_name) ||
        !write_hud_png(path, size, unfocused_fallback))
        return 0;
    return 1;
}

static void ensure_assets(void)
{
    char *dir = resolve_asset_dir();
    if (!dir)
        return;
    if (dir_has_png(dir)) {
        free(dir);
        return;
    }
    if (mkdir_p(dir) != 0) {
        fprintf(stderr, "xib: cannot create %s: %s (using fallback colors)\n",
                dir, strerror(errno));
        free(dir);
        return;
    }

    int size = image_size > 0 ? image_size : 40;
    fprintf(stderr, "xib: no border images in %s, generating templates\n", dir);

    int configured_done = 0;
    for (size_t i = 0; i < N_HUD_TEMPLATE_SIZES; i++) {
        int s = hud_template_sizes[i];
        char nf[64], nu[64];
        snprintf(nf, sizeof nf, "hud-%d-focused.png", s);
        snprintf(nu, sizeof nu, "hud-%d-unfocused.png", s);
        if (!write_hud_pair(dir, nf, nu, s))
            fprintf(stderr, "xib: failed to generate %s\n", nf);
        if (s == size) {
            const char *bf = strrchr(focused_image, '/');
            const char *bu = strrchr(unfocused_image, '/');
            bf = bf ? bf + 1 : focused_image;
            bu = bu ? bu + 1 : unfocused_image;
            if (strcmp(nf, bf) == 0 && strcmp(nu, bu) == 0)
                configured_done = 1;
        }
    }

    if (!configured_done) {
        const char *bf = strrchr(focused_image, '/');
        const char *bu = strrchr(unfocused_image, '/');
        bf = bf ? bf + 1 : focused_image;
        bu = bu ? bu + 1 : unfocused_image;
        if (!write_hud_pair(dir, bf, bu, size))
            fprintf(stderr, "xib: failed to generate %s\n", bf);
    }
    free(dir);
}

static int parse_template_size(const char *s, int *out)
{
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno || *end || v < 8 || v > 4096)
        return 0;
    *out = (int)v;
    return 1;
}

static int gen_assets(int n, char **args)
{
    int sizes[32];
    int count = 0;

    if (n == 0) {
        for (size_t i = 0; i < N_HUD_TEMPLATE_SIZES; i++)
            sizes[count++] = hud_template_sizes[i];
    } else {
        if ((size_t)n > sizeof sizes / sizeof sizes[0]) {
            fprintf(stderr, "xib: too many sizes (max %zu)\n",
                    sizeof sizes / sizeof sizes[0]);
            return 1;
        }
        for (int i = 0; i < n; i++) {
            if (!parse_template_size(args[i], &sizes[count])) {
                fprintf(stderr, "xib: invalid template size '%s' (8..4096)\n",
                        args[i]);
                return 1;
            }
            count++;
        }
    }

    char *dir = resolve_asset_dir();
    if (!dir) {
        fprintf(stderr, "xib: out of memory\n");
        return 1;
    }
    if (mkdir_p(dir) != 0) {
        fprintf(stderr, "xib: cannot create %s: %s\n", dir, strerror(errno));
        free(dir);
        return 1;
    }

    for (int i = 0; i < count; i++) {
        char nf[64], nu[64], path[PATH_MAX];
        snprintf(nf, sizeof nf, "hud-%d-focused.png", sizes[i]);
        snprintf(nu, sizeof nu, "hud-%d-unfocused.png", sizes[i]);
        if (!write_hud_pair(dir, nf, nu, sizes[i])) {
            free(dir);
            return 1;
        }
        join_path(path, sizeof path, dir, nf);
        printf("wrote %s (%dx%d)\n", path, sizes[i], sizes[i]);
        join_path(path, sizeof path, dir, nu);
        printf("wrote %s (%dx%d)\n", path, sizes[i], sizes[i]);
    }

    if (count == 1) {
        int size = sizes[0];
        char nf[64], nu[64];
        snprintf(nf, sizeof nf, "hud-%d-focused.png", size);
        snprintf(nu, sizeof nu, "hud-%d-unfocused.png", size);
        printf("xib: use it with:\n"
               "  focused_image   = \"%s/%s\";\n"
               "  unfocused_image = \"%s/%s\";\n"
               "  image_size      = %d;\n",
               dir, nf, dir, nu, size);
        printf("xib: the rule stays %dpx with a %dpx corner cut at every size\n",
               HUD_LINE_WIDTH, HUD_CORNER_CUT);
        printf("xib: keep border_thickness = corner_slice = 10 and bspwm window_gap = 10\n");
    } else {
        printf("xib: edit these files freely; the line stays %dpx and the corner\n"
               "xib: cut %dpx at every size, so keep border_thickness = corner_slice = 10\n",
               HUD_LINE_WIDTH, HUD_CORNER_CUT);
    }

    free(dir);
    return 0;
}

static void on_signal(int sig)
{
    if (sig == SIGHUP)
        reload_requested = 1;
    else
        running = 0;
}

static xcb_atom_t intern_atom(const char *name)
{
    xcb_intern_atom_reply_t *r =
        xcb_intern_atom_reply(conn, xcb_intern_atom(conn, 0, (uint16_t)strlen(name), name), NULL);
    if (!r)
        return XCB_NONE;
    xcb_atom_t a = r->atom;
    free(r);
    return a;
}

static void intern_atoms(void)
{
    for (int i = 0; i < A_COUNT; i++)
        atoms[i] = intern_atom(atom_names[i]);
}

static xcb_visualtype_t *find_argb_visual(void)
{
    xcb_depth_iterator_t di = xcb_screen_allowed_depths_iterator(screen);
    for (; di.rem; xcb_depth_next(&di)) {
        if (di.data->depth != 32)
            continue;
        xcb_visualtype_iterator_t vi = xcb_depth_visuals_iterator(di.data);
        for (; vi.rem; xcb_visualtype_next(&vi)) {
            if (vi.data->_class == XCB_VISUAL_CLASS_TRUE_COLOR)
                return vi.data;
        }
    }
    return NULL;
}

static int compositor_running(void)
{
    char name[32];
    snprintf(name, sizeof name, "_NET_WM_CM_S%d", screen_number);
    xcb_atom_t a = intern_atom(name);
    if (a == XCB_NONE)
        return 0;
    xcb_get_selection_owner_reply_t *r =
        xcb_get_selection_owner_reply(conn, xcb_get_selection_owner(conn, a), NULL);
    int yes = r && r->owner != XCB_NONE;
    free(r);
    return yes;
}

static int is_our_frame(xcb_window_t win)
{
    for (size_t i = 0; i < nclients; i++)
        if (clients[i].frame == win)
            return 1;
    return 0;
}

static client_t *find_client(xcb_window_t win)
{
    for (size_t i = 0; i < nclients; i++)
        if (clients[i].win == win)
            return &clients[i];
    return NULL;
}

static client_t *find_frame(xcb_window_t win)
{
    for (size_t i = 0; i < nclients; i++)
        if (clients[i].frame == win)
            return &clients[i];
    return NULL;
}

static client_t *client_new(void)
{
    if (nclients == client_cap) {
        size_t cap = client_cap ? client_cap * 2 : 8;
        client_t *p = realloc(clients, cap * sizeof *p);
        if (!p)
            fail("out of memory");
        clients = p;
        client_cap = cap;
    }
    client_t *c = &clients[nclients++];
    memset(c, 0, sizeof *c);
    return c;
}

static int get_atoms(xcb_window_t win, xcb_atom_t prop, xcb_atom_t **out, int *count)
{
    *out = NULL;
    *count = 0;
    xcb_get_property_reply_t *r =
        xcb_get_property_reply(conn, xcb_get_property(conn, 0, win, prop, XCB_ATOM_ATOM, 0, 1024), NULL);
    if (!r)
        return 0;
    if (r->format == 32) {
        int n = xcb_get_property_value_length(r) / 4;
        if (n > 0) {
            xcb_atom_t *v = malloc((size_t)n * sizeof *v);
            if (v) {
                memcpy(v, xcb_get_property_value(r), (size_t)n * sizeof *v);
                *out = v;
                *count = n;
            }
        }
    }
    free(r);
    return 1;
}

static int get_windows(xcb_window_t win, xcb_atom_t prop, xcb_window_t **out, int *count)
{
    *out = NULL;
    *count = 0;
    xcb_get_property_reply_t *r =
        xcb_get_property_reply(conn, xcb_get_property(conn, 0, win, prop, XCB_ATOM_WINDOW, 0, 1024), NULL);
    if (!r)
        return 0;
    if (r->format == 32) {
        int n = xcb_get_property_value_length(r) / 4;
        if (n > 0) {
            xcb_window_t *v = malloc((size_t)n * sizeof *v);
            if (v) {
                memcpy(v, xcb_get_property_value(r), (size_t)n * sizeof *v);
                *out = v;
                *count = n;
            }
        }
    }
    free(r);
    return 1;
}

static int in_client_list(xcb_window_t win)
{
    xcb_window_t *wins;
    int n, found = 0;
    if (!get_windows(screen->root, atoms[A_NET_CLIENT_LIST], &wins, &n))
        return 0;
    for (int i = 0; i < n; i++)
        if (wins[i] == win)
            found = 1;
    free(wins);
    return found;
}

static xcb_window_t read_active_window(void)
{
    xcb_get_property_reply_t *r = xcb_get_property_reply(
        conn,
        xcb_get_property(conn, 0, screen->root, atoms[A_NET_ACTIVE_WINDOW], XCB_ATOM_WINDOW, 0, 1),
        NULL);
    if (!r)
        return XCB_NONE;
    xcb_window_t w = XCB_NONE;
    if (r->format == 32 && xcb_get_property_value_length(r) >= 4)
        memcpy(&w, xcb_get_property_value(r), sizeof w);
    free(r);
    return w;
}

static int has_state(xcb_window_t win, xcb_atom_t state)
{
    xcb_atom_t *list;
    int n;
    if (!get_atoms(win, atoms[A_NET_WM_STATE], &list, &n))
        return 0;
    int found = 0;
    for (int i = 0; i < n; i++)
        if (list[i] == state)
            found = 1;
    free(list);
    return found;
}

static int class_excluded(xcb_window_t win)
{
    xcb_get_property_reply_t *r = xcb_get_property_reply(
        conn, xcb_get_property(conn, 0, win, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 0, 256), NULL);
    if (!r)
        return 0;
    int len = xcb_get_property_value_length(r);
    const char *p = xcb_get_property_value(r);
    int off = 0, excluded = 0;
    while (off < len) {
        size_t sl = strnlen(p + off, (size_t)(len - off));
        for (int i = 0; exclude_classes[i]; i++) {
            if (strcmp(p + off, exclude_classes[i]) == 0)
                excluded = 1;
        }
        if (sl == 0)
            break;
        off += (int)sl + 1;
    }
    free(r);
    return excluded;
}

static int type_allowed(xcb_window_t win)
{
    xcb_atom_t *list;
    int n;
    if (!get_atoms(win, atoms[A_NET_WM_WINDOW_TYPE], &list, &n))
        return 1;
    if (n == 0) {
        free(list);
        return 1;
    }
    int allowed = 0;
    for (int i = 0; i < n; i++) {
        if (list[i] == atoms[A_NET_WM_WINDOW_TYPE_NORMAL] ||
            list[i] == atoms[A_NET_WM_WINDOW_TYPE_DIALOG] ||
            list[i] == atoms[A_NET_WM_WINDOW_TYPE_UTILITY])
            allowed = 1;
    }
    free(list);
    return allowed;
}

static cairo_surface_t *try_png(const char *path)
{
    cairo_surface_t *s = cairo_image_surface_create_from_png(path);
    if (cairo_surface_status(s) == CAIRO_STATUS_SUCCESS)
        return s;
    cairo_surface_destroy(s);
    return NULL;
}

static cairo_surface_t *load_image(const char *path)
{
    cairo_surface_t *s = try_png(path);
    if (!s && path[0] != '/') {
        char exe[PATH_MAX];
        ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
        if (n > 0) {
            exe[n] = '\0';
            char *slash = strrchr(exe, '/');
            if (slash) {
                *slash = '\0';
                size_t need = strlen(exe) + strlen(path) + 2;
                char *full = malloc(need);
                if (full) {
                    snprintf(full, need, "%s/%s", exe, path);
                    s = try_png(full);
                    free(full);
                }
            }
        }
    }
    if (!s) {
        fprintf(stderr, "xib: cannot load %s (using fallback color)\n", path);
        return NULL;
    }
    if (image_size > 0) {
        int w = cairo_image_surface_get_width(s);
        int h = cairo_image_surface_get_height(s);
        if (w != image_size || h != image_size) {
            fprintf(stderr,
                    "xib: %s is %dx%d, expected %dx%d (image_size in config.h)\n",
                    path, w, h, image_size, image_size);
            if (w == h && w >= 8 && w <= 4096)
                fprintf(stderr, "xib: set image_size = %d to accept this image\n", w);
            fprintf(stderr,
                    "xib: or regenerate the configured size with './xib --gen-assets %d'\n",
                    image_size);
            exit(1);
        }
    }
    if (cairo_image_surface_get_format(s) != CAIRO_FORMAT_ARGB32) {
        int w = cairo_image_surface_get_width(s);
        int h = cairo_image_surface_get_height(s);
        cairo_surface_t *conv = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
        cairo_t *cr = cairo_create(conv);
        cairo_set_source_surface(cr, s, 0, 0);
        cairo_paint(cr);
        cairo_destroy(cr);
        cairo_surface_destroy(s);
        s = conv;
    }
    return s;
}

static void set_rgba(cairo_t *cr, uint32_t argb)
{
    cairo_set_source_rgba(cr,
                          ((argb >> 16) & 0xff) / 255.0,
                          ((argb >> 8) & 0xff) / 255.0,
                          (argb & 0xff) / 255.0,
                          ((argb >> 24) & 0xff) / 255.0);
}

static void draw_patch(cairo_t *cr, cairo_surface_t *src, int sx, int sy, int sw, int sh,
                       int dx, int dy, int dw, int dh, int tile)
{
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0)
        return;

    cairo_save(cr);
    cairo_rectangle(cr, dx, dy, dw, dh);
    cairo_clip(cr);

    cairo_surface_t *sub = NULL;
    cairo_pattern_t *pat;
    cairo_matrix_t m;

    if (tile) {
        sub = cairo_surface_create_for_rectangle(src, sx, sy, sw, sh);
        pat = cairo_pattern_create_for_surface(sub);
        cairo_pattern_set_extend(pat, CAIRO_EXTEND_REPEAT);
        cairo_matrix_init_translate(&m, -dx, -dy);
    } else {
        pat = cairo_pattern_create_for_surface(src);
        double xx = (double)sw / dw;
        double yy = (double)sh / dh;
        cairo_matrix_init(&m, xx, 0, 0, yy, sx - dx * xx, sy - dy * yy);
    }
    cairo_pattern_set_filter(pat, CAIRO_FILTER_BILINEAR);
    cairo_pattern_set_matrix(pat, &m);
    cairo_set_source(cr, pat);
    cairo_paint(cr);
    cairo_pattern_destroy(pat);
    if (sub)
        cairo_surface_destroy(sub);
    cairo_restore(cr);
}

static void draw_nine_slice(cairo_t *cr, cairo_surface_t *img, edges_t e,
                            int fw, int fh)
{
    int iw = cairo_image_surface_get_width(img);
    int ih = cairo_image_surface_get_height(img);
    int sl = src_insets.left, st = src_insets.top;
    int sr = src_insets.right, sb = src_insets.bottom;

    if (sl <= 0 || st <= 0 || sr <= 0 || sb <= 0 || iw < 2 || ih < 2)
        return;
    if (sl + sr > iw)
        sl = sr = iw / 2;
    if (st + sb > ih)
        st = sb = ih / 2;

    int inner_w = fw - e.left - e.right;
    int inner_h = fh - e.top - e.bottom;
    if (inner_w < 0 || inner_h < 0)
        return;

    /* Each source corner matches its own side's width/height. */
    draw_patch(cr, img, 0, 0, sl, st, 0, 0, e.left, e.top, 0);
    draw_patch(cr, img, iw - sr, 0, sr, st, fw - e.right, 0, e.right, e.top, 0);
    draw_patch(cr, img, 0, ih - sb, sl, sb, 0, fh - e.bottom, e.left, e.bottom, 0);
    draw_patch(cr, img, iw - sr, ih - sb, sr, sb,
               fw - e.right, fh - e.bottom, e.right, e.bottom, 0);

    /* Edge strips between the corners. */
    draw_patch(cr, img, sl, 0, iw - sl - sr, st,
               e.left, 0, inner_w, e.top, edge_mode);
    draw_patch(cr, img, sl, ih - sb, iw - sl - sr, sb,
               e.left, fh - e.bottom, inner_w, e.bottom, edge_mode);
    draw_patch(cr, img, 0, st, sl, ih - st - sb,
               0, e.top, e.left, inner_h, edge_mode);
    draw_patch(cr, img, iw - sr, st, sr, ih - st - sb,
               fw - e.right, e.top, e.right, inner_h, edge_mode);
}

static void draw_fallback(cairo_t *cr, edges_t e, int fw, int fh, uint32_t argb)
{
    set_rgba(cr, argb);
    cairo_rectangle(cr, 0, 0, fw, e.top);
    cairo_rectangle(cr, 0, fh - e.bottom, fw, e.bottom);
    if (fh > e.top && fh > e.bottom) {
        if (fh - e.top - e.bottom > 0) {
            cairo_rectangle(cr, 0, e.top, e.left, fh - e.top - e.bottom);
            cairo_rectangle(cr, fw - e.right, e.top, e.right, fh - e.top - e.bottom);
        }
    }
    cairo_fill(cr);
}

static void ensure_pixmap(client_t *c)
{
    if (c->surface && c->pixmap && c->rendered_w == c->w && c->rendered_h == c->h)
        return;

    if (c->surface) {
        cairo_surface_destroy(c->surface);
        c->surface = NULL;
    }
    if (c->pixmap) {
        xcb_free_pixmap(conn, c->pixmap);
        c->pixmap = XCB_NONE;
    }
    if (c->w == 0 || c->h == 0)
        return;

    c->pixmap = xcb_generate_id(conn);
    xcb_create_pixmap(conn, 32, c->pixmap, screen->root, c->w, c->h);
    c->surface = cairo_xcb_surface_create(conn, c->pixmap, argb_visual, c->w, c->h);
    if (cairo_surface_status(c->surface) != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "xib: cannot create cairo surface: %s\n",
                cairo_status_to_string(cairo_surface_status(c->surface)));
        cairo_surface_destroy(c->surface);
        c->surface = NULL;
        xcb_free_pixmap(conn, c->pixmap);
        c->pixmap = XCB_NONE;
        return;
    }
    c->rendered_w = c->w;
    c->rendered_h = c->h;
    c->cache_valid = 0;
}

static void render_frame(client_t *c)
{
    if (!c->frame)
        return;
    ensure_pixmap(c);
    if (!c->surface)
        return;

    cairo_t *cr = cairo_create(c->surface);
    if (cairo_status(cr) != CAIRO_STATUS_SUCCESS) {
        cairo_destroy(cr);
        return;
    }

    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    cairo_surface_t *img = c->focused ? img_focused : img_unfocused;
    if (img)
        draw_nine_slice(cr, img, insets, c->w, c->h);
    else
        draw_fallback(cr, insets, c->w, c->h,
                      c->focused ? focused_fallback : unfocused_fallback);

    /* Keep the client area transparent so translucent clients are not tinted. */
    int inner_w = c->w - insets.left - insets.right;
    int inner_h = c->h - insets.top - insets.bottom;
    if (inner_w > 0 && inner_h > 0) {
        cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
        cairo_rectangle(cr, insets.left, insets.top, inner_w, inner_h);
        cairo_fill(cr);
    }
    cairo_destroy(cr);
    cairo_surface_flush(c->surface);

    c->rendered_focused = c->focused;
    c->cache_valid = 1;
}

static void present(client_t *c)
{
    if (!c->mapped || !c->frame)
        return;
    if (!c->cache_valid || c->rendered_focused != c->focused ||
        c->rendered_w != c->w || c->rendered_h != c->h)
        render_frame(c);
    if (!c->pixmap)
        return;
    xcb_void_cookie_t cookie =
        xcb_copy_area_checked(conn, c->pixmap, c->frame, gc, 0, 0, 0, 0, c->w, c->h);
    if (debug_enabled) {
        xcb_generic_error_t *err = xcb_request_check(conn, cookie);
        if (err) {
            DBG("copy_area failed: X error %d\n", err->error_code);
            free(err);
        }
    }
}

static void set_frame_props(xcb_window_t win)
{
    uint32_t opacity = 0xffffffffu;
    static const char wm_class[] = "xib\0Xib";

    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, win, XCB_ATOM_WM_CLASS,
                        XCB_ATOM_STRING, 8, sizeof wm_class, wm_class);
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, win, atoms[A_NET_WM_NAME],
                        atoms[A_UTF8_STRING], 8, 3, "xib");
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, win, atoms[A_NET_WM_WINDOW_OPACITY],
                        XCB_ATOM_CARDINAL, 32, 1, &opacity);
}

static xcb_window_t create_frame(void)
{
    xcb_window_t win = xcb_generate_id(conn);
    uint32_t mask = XCB_CW_BACK_PIXMAP | XCB_CW_BORDER_PIXEL | XCB_CW_OVERRIDE_REDIRECT |
                    XCB_CW_EVENT_MASK | XCB_CW_COLORMAP;
    uint32_t values[5] = { XCB_NONE, 0, 1, XCB_EVENT_MASK_EXPOSURE, colormap };

    xcb_create_window(conn, 32, win, screen->root, 0, 0, 1, 1, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, argb_visual->visual_id, mask, values);
    set_frame_props(win);
    /* Keep the frame's input shape empty so the client receives pointer
     * events while the border stays visible above it. */
    xcb_shape_mask(conn, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_INPUT, win, 0, 0, XCB_NONE);
    return win;
}

static void apply_geometry(client_t *c)
{
    uint32_t geo[4] = { (uint32_t)c->x, (uint32_t)c->y, c->w, c->h };
    xcb_configure_window(conn, c->frame,
                         XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                         XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, geo);
}

static void stack_above_client(client_t *c)
{
    uint32_t sv[2] = { c->win, XCB_STACK_MODE_ABOVE };
    xcb_configure_window(conn, c->frame,
                         XCB_CONFIG_WINDOW_SIBLING | XCB_CONFIG_WINDOW_STACK_MODE, sv);
    DBG("stack frame 0x%x above client 0x%x\n", c->frame, c->win);
}

/* Set the frame's input shape to empty so the client receives pointer events
 * even though the frame is mapped and above it. */
static void set_frame_shape(client_t *c)
{
    xcb_shape_rectangles(conn, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_INPUT, 0,
                         c->frame, 0, 0, 0, NULL);
}

static void remove_client_at(size_t i)
{
    client_t *c = &clients[i];
    DBG("remove client 0x%x (frame 0x%x)\n", c->win, c->frame);
    if (c->surface)
        cairo_surface_destroy(c->surface);
    if (c->pixmap)
        xcb_free_pixmap(conn, c->pixmap);
    if (c->frame)
        xcb_destroy_window(conn, c->frame);
    clients[i] = clients[--nclients];
}

static void add_client(xcb_window_t win)
{
    if (find_client(win) || is_our_frame(win))
        return;

    xcb_get_window_attributes_reply_t *a = xcb_get_window_attributes_reply(
        conn, xcb_get_window_attributes(conn, win), NULL);
    if (!a)
        return;
    int viewable = !a->override_redirect && a->map_state == XCB_MAP_STATE_VIEWABLE;
    free(a);
    if (!viewable)
        return;
    if (class_excluded(win) || !type_allowed(win))
        return;

    xcb_get_geometry_reply_t *g = xcb_get_geometry_reply(conn, xcb_get_geometry(conn, win), NULL);
    if (!g)
        return;
    xcb_translate_coordinates_reply_t *tr = xcb_translate_coordinates_reply(
        conn, xcb_translate_coordinates(conn, win, screen->root, 0, 0), NULL);
    if (!tr) {
        free(g);
        return;
    }

    client_t *c = client_new();
    c->win = win;
    c->focused = (win == focused_win);
    c->frame = create_frame();
    DBG("add client 0x%x -> frame 0x%x at %d,%d %ux%u\n",
        win, c->frame, tr->dst_x - g->border_width - insets.left,
        tr->dst_y - g->border_width - insets.top,
        (unsigned)(g->width + 2 * g->border_width + insets.left + insets.right),
        (unsigned)(g->height + 2 * g->border_width + insets.top + insets.bottom));
    c->bw = g->border_width;
    c->cw = g->width;
    c->ch = g->height;
    c->x = (int16_t)(tr->dst_x - c->bw - insets.left);
    c->y = (int16_t)(tr->dst_y - c->bw - insets.top);
    c->w = (uint16_t)(c->cw + 2 * c->bw + insets.left + insets.right);
    c->h = (uint16_t)(c->ch + 2 * c->bw + insets.top + insets.bottom);

    /* Get _NET_WM_STATE changes so fullscreen toggles are seen. */
    uint32_t propmask = XCB_EVENT_MASK_PROPERTY_CHANGE;
    xcb_change_window_attributes(conn, win, XCB_CW_EVENT_MASK, &propmask);

    apply_geometry(c);
    if (!has_state(win, atoms[A_NET_WM_STATE_FULLSCREEN]) &&
        !has_state(win, atoms[A_NET_WM_STATE_HIDDEN])) {
        xcb_map_window(conn, c->frame);
        c->mapped = 1;
        stack_above_client(c);
        set_frame_shape(c);
        present(c);
    }

    free(g);
    free(tr);
}

static void update_client(client_t *c)
{
    xcb_get_geometry_reply_t *g = xcb_get_geometry_reply(conn, xcb_get_geometry(conn, c->win), NULL);
    if (!g) {
        remove_client_at((size_t)(c - clients));
        return;
    }
    xcb_translate_coordinates_reply_t *tr = xcb_translate_coordinates_reply(
        conn, xcb_translate_coordinates(conn, c->win, screen->root, 0, 0), NULL);
    if (!tr) {
        free(g);
        return;
    }
    xcb_get_window_attributes_reply_t *a = xcb_get_window_attributes_reply(
        conn, xcb_get_window_attributes(conn, c->win), NULL);
    if (!a) {
        free(g);
        free(tr);
        return;
    }
    int viewable = a->map_state == XCB_MAP_STATE_VIEWABLE;
    free(a);

    c->bw = g->border_width;
    c->cw = g->width;
    c->ch = g->height;
    int16_t x = (int16_t)(tr->dst_x - c->bw - insets.left);
    int16_t y = (int16_t)(tr->dst_y - c->bw - insets.top);
    uint16_t w = (uint16_t)(c->cw + 2 * c->bw + insets.left + insets.right);
    uint16_t h = (uint16_t)(c->ch + 2 * c->bw + insets.top + insets.bottom);
    free(g);
    free(tr);

    int geom_changed = (x != c->x || y != c->y || w != c->w || h != c->h);
    c->x = x;
    c->y = y;
    c->w = w;
    c->h = h;
    if (geom_changed)
        apply_geometry(c);

    int hidden = has_state(c->win, atoms[A_NET_WM_STATE_FULLSCREEN]) ||
                 has_state(c->win, atoms[A_NET_WM_STATE_HIDDEN]);
    int visible = viewable && !hidden;

    if (visible) {
        c->focused = (c->win == focused_win);
        if (!c->mapped) {
            xcb_map_window(conn, c->frame);
            c->mapped = 1;
            stack_above_client(c);
            set_frame_shape(c);
        }
        present(c);
    } else if (c->mapped) {
        xcb_unmap_window(conn, c->frame);
        c->mapped = 0;
    }
}

static void update_focus(void)
{
    xcb_window_t aw = read_active_window();
    if (aw == focused_win)
        return;
    focused_win = aw;
    for (size_t i = 0; i < nclients; i++) {
        client_t *c = &clients[i];
        int f = (c->win == aw);
        if (c->focused == f)
            continue;
        c->focused = f;
        present(c);
    }
}

static void mark_all_stale(void)
{
    for (size_t i = 0; i < nclients; i++) {
        clients[i].cache_valid = 0;
        if (clients[i].mapped)
            present(&clients[i]);
    }
}

static void reload_images(void)
{
    ensure_assets();
    cairo_surface_t *f = load_image(focused_image);
    cairo_surface_t *u = load_image(unfocused_image);
    if (f) {
        if (img_focused)
            cairo_surface_destroy(img_focused);
        img_focused = f;
    }
    if (u) {
        if (img_unfocused)
            cairo_surface_destroy(img_unfocused);
        img_unfocused = u;
    }
    mark_all_stale();
}

static void sync_client_list(void)
{
    xcb_window_t *wins;
    int n;
    if (!get_windows(screen->root, atoms[A_NET_CLIENT_LIST], &wins, &n))
        return;

    for (int i = 0; i < n; i++) {
        client_t *c = find_client(wins[i]);
        if (c)
            update_client(c);
        else
            add_client(wins[i]);
    }
    for (size_t i = nclients; i-- > 0;) {
        int found = 0;
        for (int j = 0; j < n; j++) {
            if (wins[j] == clients[i].win) {
                found = 1;
                break;
            }
        }
        if (!found)
            remove_client_at(i);
    }
    free(wins);
}

static void handle_event(xcb_generic_event_t *ev)
{
    uint8_t type = ev->response_type & ~0x80u;
    switch (type) {
    case XCB_EXPOSE: {
        xcb_expose_event_t *e = (void *)ev;
        client_t *c = find_frame(e->window);
        if (c)
            present(c);
        break;
    }
    case XCB_CONFIGURE_NOTIFY: {
        xcb_configure_notify_event_t *e = (void *)ev;
        if (e->window == screen->root)
            break;
        client_t *c = find_client(e->window);
        if (c)
            update_client(c);
        break;
    }
    case XCB_MAP_NOTIFY: {
        xcb_map_notify_event_t *e = (void *)ev;
        if (is_our_frame(e->window))
            break;
        client_t *c = find_client(e->window);
        if (c)
            update_client(c);
        else if (in_client_list(e->window))
            add_client(e->window);
        break;
    }
    case XCB_UNMAP_NOTIFY: {
        xcb_unmap_notify_event_t *e = (void *)ev;
        client_t *c = find_client(e->window);
        if (c && c->mapped) {
            xcb_unmap_window(conn, c->frame);
            c->mapped = 0;
        }
        break;
    }
    case XCB_DESTROY_NOTIFY: {
        xcb_destroy_notify_event_t *e = (void *)ev;
        client_t *c = find_client(e->window);
        if (c)
            remove_client_at((size_t)(c - clients));
        break;
    }
    case XCB_PROPERTY_NOTIFY: {
        xcb_property_notify_event_t *e = (void *)ev;
        if (e->window == screen->root) {
            if (e->atom == atoms[A_NET_ACTIVE_WINDOW])
                update_focus();
            else if (e->atom == atoms[A_NET_CLIENT_LIST])
                sync_client_list();
        } else if (e->atom == atoms[A_NET_WM_STATE]) {
            client_t *c = find_client(e->window);
            if (c)
                update_client(c);
        }
        break;
    }
    default:
        break;
    }
}

static int instance_ok(void)
{
    xcb_get_property_reply_t *r = xcb_get_property_reply(
        conn,
        xcb_get_property(conn, 0, screen->root, atoms[A_XIB_PID], XCB_ATOM_CARDINAL, 0, 1),
        NULL);
    if (r) {
        if (r->format == 32 && xcb_get_property_value_length(r) >= 4) {
            uint32_t pid = 0;
            memcpy(&pid, xcb_get_property_value(r), sizeof pid);
            if (pid && pid != (uint32_t)getpid() &&
                (kill((pid_t)pid, 0) == 0 || errno == EPERM)) {
                fprintf(stderr, "xib: already running (pid %u)\n", pid);
                free(r);
                return 0;
            }
        }
        free(r);
    }
    uint32_t pid = (uint32_t)getpid();
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, screen->root, atoms[A_XIB_PID],
                        XCB_ATOM_CARDINAL, 32, 1, &pid);
    return 1;
}

static void cleanup(void)
{
    while (nclients > 0)
        remove_client_at(nclients - 1);
    if (img_focused)
        cairo_surface_destroy(img_focused);
    if (img_unfocused)
        cairo_surface_destroy(img_unfocused);
    DBG("cleanup: conn error=%d\n", conn ? xcb_connection_has_error(conn) : -1);
    if (conn && screen) {
        xcb_delete_property(conn, screen->root, atoms[A_XIB_PID]);
        /* Round-trip so the delete is really on the wire before we close. */
        free(xcb_get_input_focus_reply(conn, xcb_get_input_focus(conn), NULL));
    }
    if (conn) {
        xcb_flush(conn);
        xcb_disconnect(conn);
    }
}

static void validate_config(void)
{
    if (border_thickness <= 0)
        fail("border_thickness must be > 0");
    if (border_top < 0 || border_right < 0 || border_bottom < 0 || border_left < 0)
        fail("border_top/right/bottom/left must be >= 0 (0 uses border_thickness)");
    if (corner_slice <= 0)
        fail("corner_slice must be > 0");
    if (image_size < 0)
        fail("image_size must be >= 0");
    if (image_size > 0) {
        if (image_size < 8 || image_size > 4096)
            fail("image_size must be 8..4096 (0 disables the size check)");
        if (corner_slice * 2 > image_size)
            fail("corner_slice must be <= image_size / 2");
    }

    insets.left = border_left > 0 ? border_left : border_thickness;
    insets.top = border_top > 0 ? border_top : border_thickness;
    insets.right = border_right > 0 ? border_right : border_thickness;
    insets.bottom = border_bottom > 0 ? border_bottom : border_thickness;

    src_insets.left = border_left > 0 ? border_left : corner_slice;
    src_insets.top = border_top > 0 ? border_top : corner_slice;
    src_insets.right = border_right > 0 ? border_right : corner_slice;
    src_insets.bottom = border_bottom > 0 ? border_bottom : corner_slice;

    if (image_size > 0 &&
        (insets.left + insets.right > image_size ||
         insets.top + insets.bottom > image_size))
        fail("per-side frame widths must fit inside image_size");
    if (image_size > 0 &&
        (src_insets.left + src_insets.right > image_size ||
         src_insets.top + src_insets.bottom > image_size))
        fail("per-side border widths must fit inside image_size");

    DBG("insets: L=%d T=%d R=%d B=%d\n",
        insets.left, insets.top, insets.right, insets.bottom);
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s [--render-test | --gen-assets [SIZE...]]\n"
            "  Draws image borders around X11 windows. Options are compiled in config.h.\n"
            "  --render-test        render the 9-slice to /tmp/xib-render-*.png and exit\n"
            "  --gen-assets [SIZE]  write editable HUD templates (default: 40 128 256 512)\n",
            argv0);
}

static int render_test(void)
{
    cairo_surface_t *imgs[2] = { load_image(focused_image), load_image(unfocused_image) };
    const char *names[2] = { "focused", "unfocused" };
    uint32_t fallbacks[2] = { focused_fallback, unfocused_fallback };

    for (int i = 0; i < 2; i++) {
        cairo_surface_t *img = imgs[i];
        if (img)
            printf("%s: %dx%d\n", names[i], cairo_image_surface_get_width(img),
                   cairo_image_surface_get_height(img));
        cairo_surface_t *out = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 400, 300);
        cairo_t *cr = cairo_create(out);
        cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
        cairo_paint(cr);
        cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
        if (img)
            draw_nine_slice(cr, img, insets, 400, 300);
        else
            draw_fallback(cr, insets, 400, 300, fallbacks[i]);
        int inner_w = 400 - insets.left - insets.right;
        int inner_h = 300 - insets.top - insets.bottom;
        if (inner_w > 0 && inner_h > 0) {
            cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
            cairo_rectangle(cr, insets.left, insets.top, inner_w, inner_h);
            cairo_fill(cr);
        }
        cairo_destroy(cr);
        cairo_surface_flush(out);
        char path[64];
        snprintf(path, sizeof path, "/tmp/xib-render-%s.png", names[i]);
        cairo_surface_write_to_png(out, path);
        printf("wrote %s (status %s)\n", path, cairo_status_to_string(cairo_surface_status(out)));
        cairo_surface_destroy(out);
        if (img)
            cairo_surface_destroy(img);
    }
    return 0;
}

int main(int argc, char **argv)
{
    debug_enabled = getenv("XIB_DEBUG") != NULL;
    int render = 0;
    if (argc > 1) {
        if (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
            usage(argv[0]);
            return 0;
        }
        if (!strcmp(argv[1], "--gen-assets"))
            return gen_assets(argc - 2, argv + 2);
        if (!strcmp(argv[1], "--render-test"))
            render = 1;
        else {
            usage(argv[0]);
            return 1;
        }
    }

    validate_config();
    ensure_assets();
    if (render)
        return render_test();

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGHUP, on_signal);

    conn = xcb_connect(NULL, &screen_number);
    if (!conn || xcb_connection_has_error(conn))
        fail("cannot open display");
    screen = xcb_setup_roots_iterator(xcb_get_setup(conn)).data;
    if (!screen)
        fail("no screen");

    argb_visual = find_argb_visual();
    if (!argb_visual)
        fail("no 32-bit TrueColor visual; a compositor such as picom is required");

    colormap = xcb_generate_id(conn);
    xcb_create_colormap(conn, XCB_COLORMAP_ALLOC_NONE, colormap, screen->root,
                        argb_visual->visual_id);

    /* A GC is bound to the depth of the drawable it was created with, so make
     * ours from a depth-32 drawable rather than the depth-24 root window. */
    xcb_pixmap_t scratch = xcb_generate_id(conn);
    xcb_create_pixmap(conn, 32, scratch, screen->root, 1, 1);
    gc = xcb_generate_id(conn);
    xcb_create_gc(conn, gc, scratch, 0, NULL);
    xcb_free_pixmap(conn, scratch);

    intern_atoms();
    if (!compositor_running())
        fprintf(stderr, "xib: warning: no compositor detected; borders may render incorrectly\n");
    if (!instance_ok()) {
        xcb_disconnect(conn);
        return 1;
    }

    uint32_t root_mask = XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY | XCB_EVENT_MASK_PROPERTY_CHANGE;
    xcb_change_window_attributes(conn, screen->root, XCB_CW_EVENT_MASK, &root_mask);

    img_focused = load_image(focused_image);
    img_unfocused = load_image(unfocused_image);

    focused_win = read_active_window();
    debug_enabled = getenv("XIB_DEBUG") != NULL;
    DBG("focused 0x%x\n", focused_win);
    sync_client_list();
    xcb_flush(conn);

    while (running) {
        if (reload_requested) {
            reload_requested = 0;
            reload_images();
            xcb_flush(conn);
        }

        xcb_generic_event_t *ev;
        while ((ev = xcb_poll_for_event(conn))) {
            handle_event(ev);
            free(ev);
        }
        if (xcb_connection_has_error(conn))
            break;
        xcb_flush(conn);

        struct pollfd pfd = { .fd = xcb_get_file_descriptor(conn), .events = POLLIN };
        int r = poll(&pfd, 1, poll_interval_ms);
        if (r < 0 && errno != EINTR)
            break;
    }

    cleanup();
    return 0;
}
