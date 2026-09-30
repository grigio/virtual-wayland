/*
 * vwinput - inject pointer/keyboard events into a Wayland compositor through
 * the zwlr_virtual_pointer_manager_v1 + zwp_virtual_keyboard_manager_v1
 * protocols.  Nothing touches /dev/uinput, so the host session that owns the
 * real mouse/keyboard never sees these events.
 *
 * The CLI deliberately mirrors ydotool, so scripts written against ydotool
 * translate 1:1:
 *
 *   ydotool mousemove -a -x 600 -y 500   ->  vwinput mousemove -a -x 600 -y 500
 *   ydotool key 125:1 103:1 103:0 125:0  ->  vwinput key 125:1 103:1 103:0 125:0
 *   ydotool click 0xC0                   ->  vwinput click 0xC0
 *
 * Absolute coordinates are native (buffer) pixels of the compositor's (first
 * enabled) output - i.e. exactly the pixels `vw shot` writes, at any output
 * scale.
 *
 * Build: see build.sh next to this file.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "virtual-keyboard-unstable-v1-client-protocol.h"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"

/* linux/input-event-codes.h without dragging the kernel header in */
#define VW_BTN_LEFT     0x110
#define VW_BTN_RIGHT    0x111
#define VW_BTN_MIDDLE   0x112
#define VW_BTN_SIDE     0x113
#define VW_BTN_EXTRA    0x114
#define VW_BTN_FORWARD  0x115
#define VW_BTN_BACK     0x116
#define VW_BTN_TASK     0x117

struct ctx {
	struct wl_display *dpy;
	struct wl_registry *registry;
	struct wl_seat *seat;
	struct wl_output *output;
	struct zwlr_virtual_pointer_manager_v1 *pointer_mgr;
	struct zwp_virtual_keyboard_manager_v1 *keyboard_mgr;
	struct zwlr_virtual_pointer_v1 *pointer;
	struct zwp_virtual_keyboard_v1 *keyboard;

	int32_t out_width;   /* physical px, from wl_output.mode */
	int32_t out_height;
	int32_t out_scale;   /* from wl_output.scale (v2) */

	struct xkb_context *xkb_ctx;
	struct xkb_keymap *keymap;
	struct xkb_state *xkb;
};

/* ---------------------------------------------------------------- helpers */

static uint32_t
now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000u + (uint32_t)(ts.tv_nsec / 1000000u));
}

static void
usage(FILE *out)
{
	fputs(
"vwinput - virtual Wayland input injector (ydotool-compatible CLI)\n"
"\n"
"Usage: vwinput [-d <wayland-display>] <command> [args]\n"
"\n"
"Commands:\n"
"  mousemove [-a] -x <n> -y <n>   -a: absolute (native px of the output)\n"
"                                 default: relative motion\n"
"  mousemove -w -y <n>            wheel, <n> notches (negative = down)\n"
"  click <hex>                    0x00..0x07 button, 0x40 down, 0x80 up\n"
"                                 0xC0 = left click, 0x41 = right down\n"
"                                 ('click 0x00' is a no-op, as in ydotool)\n"
"  key [-d <ms>] <code>:<state> [<code>:<state> ...]\n"
"                                 raw evdev keycodes, e.g.\n"
"                                 key 125:1 103:1 103:0 125:0   (Super+Up)\n"
"  scroll -x <n> -y <n>           axis units, ~15 per wheel notch\n"
"  drag -x <x0> -y <y0> -X <x1> -Y <y1> [-n steps] [-d ms]\n"
"                                 press at (x0,y0), walk to (x1,y1) and\n"
"                                 release, all on ONE device (a real drag);\n"
"                                 defaults: -n 40 -d 50\n"
"\n"
"Target compositor: $WAYLAND_DISPLAY (or -d). It must implement\n"
"zwlr_virtual_pointer_manager_v1 (labwc, sway, river, cage, ...) and\n"
"zwp_virtual_keyboard_manager_v1 for `key`.\n"
"\n"
"Notes:\n"
"  * One invocation = one virtual input device. Send a whole key sequence in\n"
"    a single `key` call so press/release land on the same device.\n"
"  * Coordinates are native (buffer) pixels - the same pixels `vw shot`\n"
"    writes, whatever the output scale (vw shot pins grim -s to the real\n"
"    scale, so a screenshot pixel and a -a coordinate are the same pixel).\n", out);
}

static int
parse_u32(const char *s, uint32_t *out)
{
	char *end = NULL;
	errno = 0;
	unsigned long v = strtoul(s, &end, 0);
	if (errno || end == s || (end && *end != '\0'))
		return -1;
	*out = (uint32_t)v;
	return 0;
}

static long
parse_long(const char *s, int *ok)
{
	char *end = NULL;
	errno = 0;
	long v = strtol(s, &end, 0);
	if (errno || end == s || (end && *end != '\0')) {
		*ok = 0;
		return 0;
	}
	*ok = 1;
	return v;
}

/* ------------------------------------------------------------- listeners */

static void output_geometry(void *data, struct wl_output *output, int32_t x,
			    int32_t y, int32_t physical_width,
			    int32_t physical_height, int32_t subpixel,
			    const char *make, const char *model,
			    int32_t transform);
static void output_mode(void *data, struct wl_output *output, uint32_t flags,
			int32_t width, int32_t height, int32_t refresh);
static void output_done(void *data, struct wl_output *output);
static void output_scale(void *data, struct wl_output *output, int32_t factor);

static const struct wl_output_listener output_listener = {
	.geometry = output_geometry,
	.mode = output_mode,
	.done = output_done,
	.scale = output_scale,
};

static void
registry_global(void *data, struct wl_registry *registry, uint32_t name,
		const char *interface, uint32_t version)
{
	struct ctx *c = data;

	if (strcmp(interface, wl_seat_interface.name) == 0) {
		c->seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
	} else if (strcmp(interface, wl_output_interface.name) == 0) {
		uint32_t v = version >= 2 ? 2 : version;
		c->output = wl_registry_bind(registry, name, &wl_output_interface, v);
		if (c->output)
			wl_output_add_listener(c->output, &output_listener, c);
	} else if (strcmp(interface,
			zwlr_virtual_pointer_manager_v1_interface.name) == 0) {
		uint32_t v = version >= 2 ? 2 : version;
		c->pointer_mgr = wl_registry_bind(registry, name,
			&zwlr_virtual_pointer_manager_v1_interface, v);
	} else if (strcmp(interface,
			zwp_virtual_keyboard_manager_v1_interface.name) == 0) {
		uint32_t v = version >= 1 ? 1 : version;
		c->keyboard_mgr = wl_registry_bind(registry, name,
			&zwp_virtual_keyboard_manager_v1_interface, v);
	}

	(void)registry;
}

static void
registry_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
	(void)data;
	(void)registry;
	(void)name;
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

static void
output_geometry(void *data, struct wl_output *output, int32_t x, int32_t y,
		int32_t physical_width, int32_t physical_height,
		int32_t subpixel, const char *make, const char *model,
		int32_t transform)
{
	(void)data; (void)output; (void)x; (void)y; (void)physical_width;
	(void)physical_height; (void)subpixel; (void)make; (void)model;
	(void)transform;
}

static void
output_mode(void *data, struct wl_output *output, uint32_t flags,
		int32_t width, int32_t height, int32_t refresh)
{
	struct ctx *c = data;
	(void)output;
	(void)refresh;
	if (flags & WL_OUTPUT_MODE_CURRENT) {
		c->out_width = width;
		c->out_height = height;
	}
}

static void
output_done(void *data, struct wl_output *output)
{
	(void)data;
	(void)output;
}

static void
output_scale(void *data, struct wl_output *output, int32_t factor)
{
	struct ctx *c = data;
	(void)output;
	if (factor > 0)
		c->out_scale = factor;
}

/* ------------------------------------------------------------ connection */

static int
ctx_init(struct ctx *c, const char *display)
{
	memset(c, 0, sizeof(*c));
	c->out_scale = 1;

	c->dpy = wl_display_connect(display);
	if (!c->dpy) {
		fprintf(stderr, "vwinput: cannot connect to Wayland display '%s' (%s)\n",
			display ? display : (getenv("WAYLAND_DISPLAY") ? getenv("WAYLAND_DISPLAY") : "default"),
			strerror(errno));
		return -1;
	}

	c->registry = wl_display_get_registry(c->dpy);
	if (!c->registry) {
		fprintf(stderr, "vwinput: wl_display_get_registry failed\n");
		return -1;
	}
	wl_registry_add_listener(c->registry, &registry_listener, c);
	wl_display_roundtrip(c->dpy);   /* globals */
	wl_display_roundtrip(c->dpy);   /* wl_output geometry/mode/scale/done */

	return 0;
}

static void
ctx_finish(struct ctx *c)
{
	if (c->xkb)
		xkb_state_unref(c->xkb);
	if (c->keymap)
		xkb_keymap_unref(c->keymap);
	if (c->xkb_ctx)
		xkb_context_unref(c->xkb_ctx);
	if (c->pointer)
		zwlr_virtual_pointer_v1_destroy(c->pointer);
	if (c->keyboard)
		zwp_virtual_keyboard_v1_destroy(c->keyboard);
	if (c->pointer_mgr)
		zwlr_virtual_pointer_manager_v1_destroy(c->pointer_mgr);
	if (c->keyboard_mgr)
		zwp_virtual_keyboard_manager_v1_destroy(c->keyboard_mgr);
	if (c->output)
		wl_output_destroy(c->output);
	if (c->seat)
		wl_seat_release(c->seat);
	if (c->registry)
		wl_registry_destroy(c->registry);
	if (c->dpy) {
		wl_display_flush(c->dpy);
		wl_display_disconnect(c->dpy);
	}
}

/* Make sure everything we queued is on the wire and processed. */
static int
ctx_flush(struct ctx *c)
{
	if (wl_display_roundtrip(c->dpy) < 0) {
		fprintf(stderr, "vwinput: wayland error: %s\n",
			wl_display_get_error(c->dpy) ? strerror(wl_display_get_error(c->dpy)) : "?");
		return -1;
	}
	return 0;
}

/* -------------------------------------------------------------- geometry */

/* Native (buffer) size of the output, with a sane fallback.
 *
 * This is the space absolute coordinates live in, and it is deliberate: the
 * compositor normalises x / x_extent over its *logical* layout, so an extent
 * in logical pixels would only be right at scale 1. wl_output.scale cannot be
 * used to convert it either - it is an integer, so the skill's default 0.5
 * arrives as 1. Using the raw mode size means x lands on pixel x of the
 * screenshot `vw shot` writes, at every scale. */
static void
output_extent(struct ctx *c, uint32_t *width, uint32_t *height)
{
	int32_t w = c->out_width > 0 ? c->out_width : 1280;
	int32_t h = c->out_height > 0 ? c->out_height : 720;
	*width = (uint32_t)w;
	*height = (uint32_t)h;
}

/* ---------------------------------------------------------------- pointer */

static int
ensure_pointer(struct ctx *c)
{
	if (c->pointer)
		return 0;
	if (!c->pointer_mgr) {
		fprintf(stderr, "vwinput: compositor does not advertise "
			"zwlr_virtual_pointer_manager_v1\n");
		return -1;
	}
	if (!c->seat) {
		fprintf(stderr, "vwinput: compositor advertises no wl_seat\n");
		return -1;
	}
	c->pointer = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(
		c->pointer_mgr, c->seat);
	if (!c->pointer) {
		fprintf(stderr, "vwinput: create_virtual_pointer failed\n");
		return -1;
	}
	return 0;
}

/* -------------------------------------------------------------- keyboard */

static int
write_keymap(char *str, size_t len)
{
	int fd;
#ifdef __linux__
	fd = memfd_create("vwinput-keymap", 0u);
#endif
	if (fd < 0) {
		int pfd[2];
		if (pipe(pfd) < 0)
			return -1;
		if (write(pfd[1], str, len) != (ssize_t)len) {
			close(pfd[0]);
			close(pfd[1]);
			return -1;
		}
		close(pfd[1]);
		return pfd[0];
	}
	if (write(fd, str, len) != (ssize_t)len) {
		close(fd);
		return -1;
	}
	if (lseek(fd, 0, SEEK_SET) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static int
ensure_keyboard(struct ctx *c)
{
	if (c->keyboard)
		return 0;
	if (!c->keyboard_mgr) {
		fprintf(stderr, "vwinput: compositor does not advertise "
			"zwp_virtual_keyboard_manager_v1\n");
		return -1;
	}
	if (!c->seat) {
		fprintf(stderr, "vwinput: compositor advertises no wl_seat\n");
		return -1;
	}

	c->keyboard = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(
		c->keyboard_mgr, c->seat);
	if (!c->keyboard) {
		fprintf(stderr, "vwinput: create_virtual_keyboard failed\n");
		return -1;
	}

	/* A virtual keyboard with no keymap has no modifier state, and the
	 * compositor's keybindings (e.g. Super+Up) would never see the
	 * modifier. Ship a plain us keymap and track modifiers locally. */
	c->xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (!c->xkb_ctx)
		return -1;
	struct xkb_rule_names names = { .layout = "us", .variant = "" };
	c->keymap = xkb_keymap_new_from_names(c->xkb_ctx, &names,
		XKB_KEYMAP_COMPILE_NO_FLAGS);
	if (!c->keymap) {
		fprintf(stderr, "vwinput: xkb: could not compile a 'us' keymap\n");
		return -1;
	}
	char *str = xkb_keymap_get_as_string(c->keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
	if (!str)
		return -1;
	size_t len = strlen(str) + 1;
	int fd = write_keymap(str, len);
	free(str);
	if (fd < 0) {
		fprintf(stderr, "vwinput: xkb: could not serialise keymap (%s)\n",
			strerror(errno));
		return -1;
	}
	zwp_virtual_keyboard_v1_keymap(c->keyboard,
		WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, (uint32_t)len);
	close(fd);

	c->xkb = xkb_state_new(c->keymap);
	return c->xkb ? 0 : -1;
}

static void
send_modifiers(struct ctx *c)
{
	uint32_t depressed = xkb_state_serialize_mods(c->xkb, XKB_STATE_MODS_DEPRESSED);
	uint32_t latched = xkb_state_serialize_mods(c->xkb, XKB_STATE_MODS_LATCHED);
	uint32_t locked = xkb_state_serialize_mods(c->xkb, XKB_STATE_MODS_LOCKED);
	uint32_t group = xkb_state_serialize_layout(c->xkb, XKB_STATE_LAYOUT_EFFECTIVE);
	zwp_virtual_keyboard_v1_modifiers(c->keyboard, depressed, latched,
		locked, group);
}

/* Send key events + the modifier state they imply (same order a real
 * keyboard produces: key first, then modifiers). */
static int
send_key_sequence(struct ctx *c, int argc, char **argv, long delay_ms)
{
	if (ensure_keyboard(c) < 0)
		return -1;

	for (int i = 0; i < argc; i++) {
		const char *tok = argv[i];
		char *end = NULL;
		errno = 0;
		unsigned long code = strtoul(tok, &end, 0);
		if (end == tok || !end || *end != ':') {
			/* ydotool: non-interpretable values only cause a delay */
			if (delay_ms > 0)
				usleep((useconds_t)delay_ms * 1000u);
			continue;
		}
		uint32_t state;
		if (parse_u32(end + 1, &state) < 0 || state > 1) {
			fprintf(stderr, "vwinput: bad key token '%s' (want <code>:0|1)\n", tok);
			return -1;
		}
		uint32_t key = (uint32_t)code;
		zwp_virtual_keyboard_v1_key(c->keyboard, now_ms(), key,
			state ? WL_KEYBOARD_KEY_STATE_PRESSED
			      : WL_KEYBOARD_KEY_STATE_RELEASED);
		xkb_state_update_key(c->xkb, key + 8,
			state ? XKB_KEY_DOWN : XKB_KEY_UP);
		send_modifiers(c);
		if (i + 1 < argc && delay_ms > 0)
			usleep((useconds_t)delay_ms * 1000u);
	}
	return 0;
}

/* -------------------------------------------------------------- commands */

static int
cmd_mousemove(struct ctx *c, int argc, char **argv)
{
	int absolute = 0, wheel = 0;
	int have_x = 0, have_y = 0;
	long x = 0, y = 0;
	int ok;

	optind = 1;
	int o;
	while ((o = getopt(argc, argv, "+""ax:y:wh")) != -1) {
		switch (o) {
		case 'a':
			absolute = 1;
			break;
		case 'w':
			wheel = 1;
			break;
		case 'x':
			x = parse_long(optarg, &ok);
			if (!ok) {
				fprintf(stderr, "vwinput: bad -x '%s'\n", optarg);
				return 2;
			}
			have_x = 1;
			break;
		case 'y':
			y = parse_long(optarg, &ok);
			if (!ok) {
				fprintf(stderr, "vwinput: bad -y '%s'\n", optarg);
				return 2;
			}
			have_y = 1;
			break;
		default:
			return 2;
		}
	}
	if (!have_x && !have_y) {
		fprintf(stderr, "vwinput: mousemove needs -x and/or -y\n");
		return 2;
	}
	if (ensure_pointer(c) < 0)
		return 1;

	if (wheel) {
		/* ydotool counts wheel notches; Wayland axis units are ~15/notch */
		if (have_y)
			zwlr_virtual_pointer_v1_axis(c->pointer, now_ms(),
				WL_POINTER_AXIS_VERTICAL_SCROLL,
				wl_fixed_from_double((double)y * 15.0));
		if (have_x)
			zwlr_virtual_pointer_v1_axis(c->pointer, now_ms(),
				WL_POINTER_AXIS_HORIZONTAL_SCROLL,
				wl_fixed_from_double((double)x * 15.0));
	} else if (absolute) {
		uint32_t ew, eh;
		output_extent(c, &ew, &eh);
		zwlr_virtual_pointer_v1_motion_absolute(c->pointer, now_ms(),
			(uint32_t)(x < 0 ? 0 : x), (uint32_t)(y < 0 ? 0 : y), ew, eh);
	} else {
		zwlr_virtual_pointer_v1_motion(c->pointer, now_ms(),
			wl_fixed_from_double((double)x), wl_fixed_from_double((double)y));
	}
	zwlr_virtual_pointer_v1_frame(c->pointer);
	return 0;
}

static int
cmd_click(struct ctx *c, int argc, char **argv)
{
	uint32_t value;
	int have_value = 0;
	long repeat = 1;
	int ok;

	optind = 1;
	int o;
	while ((o = getopt(argc, argv, "+""r:h")) != -1) {
		switch (o) {
		case 'r':
			repeat = parse_long(optarg, &ok);
			if (!ok || repeat < 1) {
				fprintf(stderr, "vwinput: bad -r '%s'\n", optarg);
				return 2;
			}
			break;
		default:
			return 2;
		}
	}
	for (int i = optind; i < argc; i++) {
		if (parse_u32(argv[i], &value) < 0) {
			fprintf(stderr, "vwinput: bad button '%s'\n", argv[i]);
			return 2;
		}
		have_value = 1;

		uint32_t button = VW_BTN_LEFT + (value & 0x0F);
		uint32_t dir = value & 0xC0;

		if (ensure_pointer(c) < 0)
			return 1;
		for (long r = 0; r < repeat; r++) {
			if (dir & 0x40) {
				zwlr_virtual_pointer_v1_button(c->pointer, now_ms(),
					button, WL_POINTER_BUTTON_STATE_PRESSED);
				zwlr_virtual_pointer_v1_frame(c->pointer);
			}
			if (dir & 0x80) {
				if (dir & 0x40)
					usleep(12000);
				zwlr_virtual_pointer_v1_button(c->pointer, now_ms(),
					button, WL_POINTER_BUTTON_STATE_RELEASED);
				zwlr_virtual_pointer_v1_frame(c->pointer);
			}
		}
	}
	if (!have_value) {
		fprintf(stderr, "vwinput: click needs a button value (e.g. 0xC0)\n");
		return 2;
	}
	return 0;
}

/* Press a button, walk the pointer from (x0,y0) to (x1,y1) in absolute
 * steps and release it — all inside ONE invocation, i.e. on one virtual
 * device, so the compositor sees a real press-drag-release instead of a
 * press whose device disappears mid-gesture. */
static int
cmd_drag(struct ctx *c, int argc, char **argv)
{
	int have_x = 0, have_y = 0, have_X = 0, have_Y = 0;
	long x0 = 0, y0 = 0, x1 = 0, y1 = 0, steps = 40, delay = 50;
	int ok;

	optind = 1;
	int o;
	while ((o = getopt(argc, argv, "+""x:y:X:Y:n:d:h")) != -1) {
		switch (o) {
		case 'x':
			x0 = parse_long(optarg, &ok);
			if (!ok) {
				fprintf(stderr, "vwinput: bad -x '%s'\n", optarg);
				return 2;
			}
			have_x = 1;
			break;
		case 'y':
			y0 = parse_long(optarg, &ok);
			if (!ok) {
				fprintf(stderr, "vwinput: bad -y '%s'\n", optarg);
				return 2;
			}
			have_y = 1;
			break;
		case 'X':
			x1 = parse_long(optarg, &ok);
			if (!ok) {
				fprintf(stderr, "vwinput: bad -X '%s'\n", optarg);
				return 2;
			}
			have_X = 1;
			break;
		case 'Y':
			y1 = parse_long(optarg, &ok);
			if (!ok) {
				fprintf(stderr, "vwinput: bad -Y '%s'\n", optarg);
				return 2;
			}
			have_Y = 1;
			break;
		case 'n':
			steps = parse_long(optarg, &ok);
			if (!ok || steps < 1) {
				fprintf(stderr, "vwinput: bad -n '%s'\n", optarg);
				return 2;
			}
			break;
		case 'd':
			delay = parse_long(optarg, &ok);
			if (!ok || delay < 0) {
				fprintf(stderr, "vwinput: bad -d '%s'\n", optarg);
				return 2;
			}
			break;
		default:
			return 2;
		}
	}
	if (!have_x || !have_y || !have_X || !have_Y) {
		fprintf(stderr, "vwinput: drag needs -x -y -X -Y\n");
		return 2;
	}
	if (ensure_pointer(c) < 0)
		return 1;

	uint32_t ew, eh;
	output_extent(c, &ew, &eh);

#define VW_ABS_MOTION(px, py)                                                  \
	do {                                                                   \
		zwlr_virtual_pointer_v1_motion_absolute(c->pointer, now_ms(),  \
			(uint32_t)((px) < 0 ? 0 : (px)),                       \
			(uint32_t)((py) < 0 ? 0 : (py)), ew, eh);              \
		zwlr_virtual_pointer_v1_frame(c->pointer);                       \
		wl_display_flush(c->dpy);                                      \
	} while (0)

	/* Go to the start of the drag first (the caller's last absolute
	 * position may be anywhere), then press. */
	VW_ABS_MOTION(x0, y0);
	usleep(20000);
	zwlr_virtual_pointer_v1_button(c->pointer, now_ms(), VW_BTN_LEFT,
		WL_POINTER_BUTTON_STATE_PRESSED);
	zwlr_virtual_pointer_v1_frame(c->pointer);
	wl_display_flush(c->dpy);

	for (long s = 1; s <= steps; s++) {
		double t = (double)s / (double)steps;
		VW_ABS_MOTION(x0 + (x1 - x0) * t, y0 + (y1 - y0) * t);
		if (delay > 0)
			usleep((useconds_t)(delay * 1000));
	}

	zwlr_virtual_pointer_v1_button(c->pointer, now_ms(), VW_BTN_LEFT,
		WL_POINTER_BUTTON_STATE_RELEASED);
	zwlr_virtual_pointer_v1_frame(c->pointer);
#undef VW_ABS_MOTION
	return 0;
}

static int
cmd_scroll(struct ctx *c, int argc, char **argv)
{
	int have_x = 0, have_y = 0;
	double x = 0, y = 0;
	int ok;

	optind = 1;
	int o;
	while ((o = getopt(argc, argv, "+""x:y:h")) != -1) {
		switch (o) {
		case 'x':
			x = strtod(optarg, NULL);
			(void)ok;
			have_x = 1;
			break;
		case 'y':
			y = strtod(optarg, NULL);
			have_y = 1;
			break;
		default:
			return 2;
		}
	}
	if (!have_x && !have_y) {
		fprintf(stderr, "vwinput: scroll needs -x and/or -y\n");
		return 2;
	}
	if (ensure_pointer(c) < 0)
		return 1;
	if (have_y) {
		zwlr_virtual_pointer_v1_axis(c->pointer, now_ms(),
			WL_POINTER_AXIS_VERTICAL_SCROLL, wl_fixed_from_double(y));
		zwlr_virtual_pointer_v1_frame(c->pointer);
	}
	if (have_x) {
		zwlr_virtual_pointer_v1_axis(c->pointer, now_ms(),
			WL_POINTER_AXIS_HORIZONTAL_SCROLL, wl_fixed_from_double(x));
		zwlr_virtual_pointer_v1_frame(c->pointer);
	}
	return 0;
}

static int
cmd_key(struct ctx *c, int argc, char **argv)
{
	long delay = 0;
	int ok;

	optind = 1;
	int o;
	while ((o = getopt(argc, argv, "+""d:h")) != -1) {
		switch (o) {
		case 'd':
			delay = parse_long(optarg, &ok);
			if (!ok || delay < 0) {
				fprintf(stderr, "vwinput: bad -d '%s'\n", optarg);
				return 2;
			}
			break;
		default:
			return 2;
		}
	}
	if (optind >= argc) {
		fprintf(stderr, "vwinput: key needs at least one <code>:<state>\n");
		return 2;
	}
	return send_key_sequence(c, argc - optind, &argv[optind], delay) < 0 ? 1 : 0;
}


/* A virtual-wayland session records its socket here.  If one is running we
 * refuse to inject into any *other* display (that would be the real desktop
 * and would move the user's actual mouse). */
static char *
virtual_session_socket(void)
{
	const char *dir = getenv("VIRTUAL_WAYLAND_DIR");
	char path[512];

	if (dir && *dir)
		snprintf(path, sizeof(path), "%s/wayland-display", dir);
	else
		snprintf(path, sizeof(path), "%s/virtual-wayland/wayland-display",
			getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");

	FILE *f = fopen(path, "r");
	if (!f)
		return NULL;
	char buf[256] = {0};
	if (!fgets(buf, sizeof(buf), f)) {
		fclose(f);
		return NULL;
	}
	fclose(f);
	buf[strcspn(buf, "\r\n")] = '\0';
	return buf[0] ? strdup(buf) : NULL;
}

/* ------------------------------------------------------------------ main */

int
main(int argc, char **argv)
{
	const char *display = NULL;
	int i = 1;

	if (i < argc && strcmp(argv[i], "-h") == 0) {
		usage(stdout);
		return 0;
	}
	if (i + 1 < argc && strcmp(argv[i], "-d") == 0) {
		display = argv[i + 1];
		i += 2;
	}
	if (i >= argc) {
		usage(stderr);
		return 2;
	}
	const char *cmd = argv[i];

	/* Safety rail: never silently drive the real desktop's input. */
	char *session = virtual_session_socket();
	if (session && !getenv("VWINPUT_ALLOW_ANY")) {
		const char *target = display ? display : getenv("WAYLAND_DISPLAY");
		if (!target || strcmp(target, session) != 0) {
			fprintf(stderr,
				"vwinput: refusing to inject into '%s' while a virtual-wayland\n"
				"         session is running on '%s'.\n"
				"         That display is (probably) the real desktop - it would move\n"
				"         the user's actual mouse.\n"
				"\n"
				"         use:   vw input %s ...\n"
				"           or:  eval $(vw env)   # then vwinput works as-is\n"
				"         bypass: VWINPUT_ALLOW_ANY=1\n",
				target ? target : "(default)", session, cmd);
			free(session);
			return 1;
		}
	}
	free(session);

	struct ctx c;
	if (ctx_init(&c, display) < 0)
		return 1;

	/* Handlers are getopt()-driven, so hand them argv with the subcommand
	 * in argv[0] (the usual convention getopt expects). */
	int sub_argc = argc - i;
	char **sub_argv = &argv[i];

	int rc;
	if (strcmp(cmd, "mousemove") == 0) {
		rc = cmd_mousemove(&c, sub_argc, sub_argv);
	} else if (strcmp(cmd, "click") == 0) {
		rc = cmd_click(&c, sub_argc, sub_argv);
	} else if (strcmp(cmd, "drag") == 0) {
		rc = cmd_drag(&c, sub_argc, sub_argv);
	} else if (strcmp(cmd, "scroll") == 0) {
		rc = cmd_scroll(&c, sub_argc, sub_argv);
	} else if (strcmp(cmd, "key") == 0) {
		rc = cmd_key(&c, sub_argc, sub_argv);
	} else if (strcmp(cmd, "help") == 0) {
		usage(stdout);
		rc = 0;
	} else {
		fprintf(stderr, "vwinput: unknown command '%s'\n", cmd);
		usage(stderr);
		rc = 2;
	}

	if (rc == 0 && ctx_flush(&c) < 0)
		rc = 1;
	ctx_finish(&c);
	return rc;
}
