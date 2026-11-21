/*
 * Display module
 *
 * Shows the recognition results on the touchscreen LCD as a fullscreen
 * Wayland window (Weston or labwc), drawn with Cairo into shared memory.
 *
 * Default screen: the top 5 classes now heard with their scores (watched
 * classes show their threshold), the input level in dBFS and recent events.
 * Alert screen: shown when an event is detected, with class, score,
 * threshold and time. A touch (or click) anywhere dismisses it, and it goes
 * away by itself after alert_dismiss_sec.
 */
#include <cairo/cairo.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#include "common/config.h"
#include "common/protocol.h"
#include "common/socket.h"
#include "xdg-shell-client-protocol.h"

#define DEFAULT_WIDTH		1280
#define DEFAULT_HEIGHT		800
#define NUM_RECENT		5	/* events kept on the default screen */
#define RESULT_STALE_MS		3000	/* no results: pipeline not running */
#define LEVEL_MIN_DBFS		-60.0
#define FONT			"DejaVu Sans"

/* The layout is designed on a 2000 x 1250 canvas and scaled to the window */
#define DESIGN_W		2000.0
#define DESIGN_H		1250.0

typedef enum { MODE_DEFAULT, MODE_ALERT } display_mode_t;

typedef struct {
	display_mode_t mode;
	frame_result_t top_n;		/* latest frame */
	int64_t top_n_rx_ms;		/* when it arrived, 0: none yet */
	event_t alert;			/* event on the alert screen */
	int64_t alert_since_ms;
	event_t recent[NUM_RECENT];	/* newest first */
	int num_recent;
	const app_config_t *config;
	char (*class_names)[AER_NAME_MAX];
} display_state_t;

typedef struct {
	struct wl_buffer *buffer;
	void *data;
	size_t size;
	int width, height;
	int busy;			/* held by the compositor */
} shm_buffer_t;

typedef struct {
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct xdg_wm_base *wm_base;
	struct wl_seat *seat;
	struct wl_pointer *pointer;
	struct wl_touch *touch;
	struct wl_surface *surface;
	struct xdg_surface *xdg_surface;
	struct xdg_toplevel *toplevel;
	shm_buffer_t buffers[2];
	int width, height;		/* current window size */
	int pending_width, pending_height;
	int configured;
	int touched;			/* touch/click since last check */
} ui_t;

static volatile int running = 1;

/* ---------------------------------------------------------------- Wayland */

static void buffer_release(void *data, struct wl_buffer *buffer)
{
	(void)buffer;
	((shm_buffer_t *)data)->busy = 0;
}

static const struct wl_buffer_listener buffer_listener = {
	.release = buffer_release,
};

static void destroy_buffer(shm_buffer_t *b)
{
	if (b->buffer)
		wl_buffer_destroy(b->buffer);
	if (b->data)
		munmap(b->data, b->size);
	memset(b, 0, sizeof(*b));
}

static int create_buffer(ui_t *ui, shm_buffer_t *b, int width, int height)
{
	struct wl_shm_pool *pool;
	int fd, stride = width * 4;

	b->size = (size_t)stride * height;
	fd = memfd_create("aer-display", MFD_CLOEXEC);
	if (fd < 0)
		return -1;
	if (ftruncate(fd, (off_t)b->size) < 0) {
		close(fd);
		return -1;
	}
	b->data = mmap(NULL, b->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (b->data == MAP_FAILED) {
		b->data = NULL;
		close(fd);
		return -1;
	}
	pool = wl_shm_create_pool(ui->shm, fd, (int32_t)b->size);
	b->buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride,
					      WL_SHM_FORMAT_XRGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	wl_buffer_add_listener(b->buffer, &buffer_listener, b);
	b->width = width;
	b->height = height;
	b->busy = 0;
	return 0;
}

/* A buffer of the current window size that the compositor is not using */
static shm_buffer_t *free_buffer(ui_t *ui)
{
	int i;

	for (i = 0; i < 2; i++) {
		shm_buffer_t *b = &ui->buffers[i];

		if (b->busy)
			continue;
		if (b->buffer && (b->width != ui->width ||
				  b->height != ui->height))
			destroy_buffer(b);
		if (!b->buffer && create_buffer(ui, b, ui->width, ui->height))
			return NULL;
		return b;
	}
	return NULL;
}

static void wm_base_ping(void *data, struct xdg_wm_base *wm_base,
			 uint32_t serial)
{
	(void)data;
	xdg_wm_base_pong(wm_base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
	.ping = wm_base_ping,
};

static void xdg_surface_configure(void *data, struct xdg_surface *xdg_surface,
				  uint32_t serial)
{
	ui_t *ui = data;

	xdg_surface_ack_configure(xdg_surface, serial);
	if (ui->pending_width > 0 && ui->pending_height > 0) {
		ui->width = ui->pending_width;
		ui->height = ui->pending_height;
	}
	ui->configured = 1;
}

static const struct xdg_surface_listener xdg_surface_listener = {
	.configure = xdg_surface_configure,
};

static void toplevel_configure(void *data, struct xdg_toplevel *toplevel,
			       int32_t width, int32_t height,
			       struct wl_array *states)
{
	ui_t *ui = data;

	(void)toplevel;
	(void)states;
	ui->pending_width = width;
	ui->pending_height = height;
}

static void toplevel_close(void *data, struct xdg_toplevel *toplevel)
{
	/* a kiosk screen: stays up until the service is stopped */
	(void)data;
	(void)toplevel;
}

static const struct xdg_toplevel_listener toplevel_listener = {
	.configure = toplevel_configure,
	.close = toplevel_close,
};

static void pointer_enter(void *data, struct wl_pointer *pointer,
			  uint32_t serial, struct wl_surface *surface,
			  wl_fixed_t x, wl_fixed_t y)
{
	(void)data; (void)surface; (void)x; (void)y;
	wl_pointer_set_cursor(pointer, serial, NULL, 0, 0);	/* hide */
}

static void pointer_leave(void *data, struct wl_pointer *pointer,
			  uint32_t serial, struct wl_surface *surface)
{
	(void)data; (void)pointer; (void)serial; (void)surface;
}

static void pointer_motion(void *data, struct wl_pointer *pointer,
			   uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
	(void)data; (void)pointer; (void)time; (void)x; (void)y;
}

static void pointer_button(void *data, struct wl_pointer *pointer,
			   uint32_t serial, uint32_t time, uint32_t button,
			   uint32_t state)
{
	(void)pointer; (void)serial; (void)time; (void)button;
	if (state == WL_POINTER_BUTTON_STATE_PRESSED)
		((ui_t *)data)->touched = 1;
}

static void pointer_axis(void *data, struct wl_pointer *pointer,
			 uint32_t time, uint32_t axis, wl_fixed_t value)
{
	(void)data; (void)pointer; (void)time; (void)axis; (void)value;
}

static const struct wl_pointer_listener pointer_listener = {
	.enter = pointer_enter,
	.leave = pointer_leave,
	.motion = pointer_motion,
	.button = pointer_button,
	.axis = pointer_axis,
};

static void touch_down(void *data, struct wl_touch *touch, uint32_t serial,
		       uint32_t time, struct wl_surface *surface, int32_t id,
		       wl_fixed_t x, wl_fixed_t y)
{
	(void)touch; (void)serial; (void)time; (void)surface; (void)id;
	(void)x; (void)y;
	((ui_t *)data)->touched = 1;
}

static void touch_up(void *data, struct wl_touch *touch, uint32_t serial,
		     uint32_t time, int32_t id)
{
	(void)data; (void)touch; (void)serial; (void)time; (void)id;
}

static void touch_motion(void *data, struct wl_touch *touch, uint32_t time,
			 int32_t id, wl_fixed_t x, wl_fixed_t y)
{
	(void)data; (void)touch; (void)time; (void)id; (void)x; (void)y;
}

static void touch_frame(void *data, struct wl_touch *touch)
{
	(void)data; (void)touch;
}

static void touch_cancel(void *data, struct wl_touch *touch)
{
	(void)data; (void)touch;
}

static const struct wl_touch_listener touch_listener = {
	.down = touch_down,
	.up = touch_up,
	.motion = touch_motion,
	.frame = touch_frame,
	.cancel = touch_cancel,
};

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps)
{
	ui_t *ui = data;

	if ((caps & WL_SEAT_CAPABILITY_POINTER) && !ui->pointer) {
		ui->pointer = wl_seat_get_pointer(seat);
		wl_pointer_add_listener(ui->pointer, &pointer_listener, ui);
	} else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && ui->pointer) {
		wl_pointer_destroy(ui->pointer);
		ui->pointer = NULL;
	}
	if ((caps & WL_SEAT_CAPABILITY_TOUCH) && !ui->touch) {
		ui->touch = wl_seat_get_touch(seat);
		wl_touch_add_listener(ui->touch, &touch_listener, ui);
	} else if (!(caps & WL_SEAT_CAPABILITY_TOUCH) && ui->touch) {
		wl_touch_destroy(ui->touch);
		ui->touch = NULL;
	}
}

static void seat_name(void *data, struct wl_seat *seat, const char *name)
{
	(void)data; (void)seat; (void)name;
}

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_capabilities,
	.name = seat_name,
};

static void registry_global(void *data, struct wl_registry *registry,
			    uint32_t name, const char *interface,
			    uint32_t version)
{
	ui_t *ui = data;

	if (!strcmp(interface, wl_compositor_interface.name)) {
		ui->compositor = wl_registry_bind(registry, name,
						  &wl_compositor_interface, 1);
	} else if (!strcmp(interface, wl_shm_interface.name)) {
		ui->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (!strcmp(interface, xdg_wm_base_interface.name)) {
		ui->wm_base = wl_registry_bind(registry, name,
					       &xdg_wm_base_interface, 1);
		xdg_wm_base_add_listener(ui->wm_base, &wm_base_listener, ui);
	} else if (!strcmp(interface, wl_seat_interface.name) && !ui->seat) {
		/* v1-3: the listeners above cover every event */
		ui->seat = wl_registry_bind(registry, name, &wl_seat_interface,
					    version < 3 ? version : 3);
		wl_seat_add_listener(ui->seat, &seat_listener, ui);
	}
}

static void registry_global_remove(void *data, struct wl_registry *registry,
				   uint32_t name)
{
	(void)data; (void)registry; (void)name;
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

/*
 * Connect to the Wayland compositor (WAYLAND_DISPLAY) and create a
 * fullscreen window. width/height are used until the compositor picks the
 * size. Returns NULL if no compositor is reachable.
 */
ui_t *create_window(int width, int height)
{
	ui_t *ui = calloc(1, sizeof(*ui));

	if (!ui)
		return NULL;
	ui->width = width;
	ui->height = height;
	ui->display = wl_display_connect(NULL);
	if (!ui->display) {
		fprintf(stderr, "cannot connect to Wayland display %s\n",
			getenv("WAYLAND_DISPLAY") ? getenv("WAYLAND_DISPLAY") :
			"(unset)");
		free(ui);
		return NULL;
	}
	ui->registry = wl_display_get_registry(ui->display);
	wl_registry_add_listener(ui->registry, &registry_listener, ui);
	wl_display_roundtrip(ui->display);
	if (!ui->compositor || !ui->shm || !ui->wm_base) {
		fprintf(stderr, "compositor lacks wl_shm or xdg_wm_base\n");
		wl_display_disconnect(ui->display);
		free(ui);
		return NULL;
	}

	ui->surface = wl_compositor_create_surface(ui->compositor);
	ui->xdg_surface = xdg_wm_base_get_xdg_surface(ui->wm_base, ui->surface);
	xdg_surface_add_listener(ui->xdg_surface, &xdg_surface_listener, ui);
	ui->toplevel = xdg_surface_get_toplevel(ui->xdg_surface);
	xdg_toplevel_add_listener(ui->toplevel, &toplevel_listener, ui);
	xdg_toplevel_set_title(ui->toplevel, "Audio Event Recognition");
	xdg_toplevel_set_app_id(ui->toplevel, "audio-event-recognition");
	xdg_toplevel_set_fullscreen(ui->toplevel, NULL);
	wl_surface_commit(ui->surface);

	while (!ui->configured)
		if (wl_display_dispatch(ui->display) < 0) {
			fprintf(stderr, "Wayland connection lost\n");
			return NULL;
		}
	fprintf(stderr, "window %dx%d\n", ui->width, ui->height);
	return ui;
}

/* --------------------------------------------------------------- Drawing */

typedef enum { ALIGN_LEFT, ALIGN_CENTER, ALIGN_RIGHT } align_t;

static void set_rgb(cairo_t *cr, unsigned rgb)
{
	cairo_set_source_rgb(cr, ((rgb >> 16) & 0xff) / 255.0,
			     ((rgb >> 8) & 0xff) / 255.0, (rgb & 0xff) / 255.0);
}

static void rounded_rect(cairo_t *cr, double x, double y, double w, double h,
			 double r)
{
	if (r > h / 2)
		r = h / 2;
	if (r > w / 2)
		r = w / 2;
	cairo_new_sub_path(cr);
	cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
	cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
	cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
	cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
	cairo_close_path(cr);
}

static void fill_rounded(cairo_t *cr, double x, double y, double w, double h,
			 double r, unsigned rgb)
{
	rounded_rect(cr, x, y, w, h, r);
	set_rgb(cr, rgb);
	cairo_fill(cr);
}

static void set_font(cairo_t *cr, double size, int bold)
{
	cairo_select_font_face(cr, FONT, CAIRO_FONT_SLANT_NORMAL,
			       bold ? CAIRO_FONT_WEIGHT_BOLD :
			       CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size(cr, size);
}

/* Width of text with 'spacing' added after every character */
static double text_width(cairo_t *cr, const char *text, double spacing)
{
	cairo_text_extents_t ext;
	size_t chars = 0;
	const char *p;

	cairo_text_extents(cr, text, &ext);
	for (p = text; *p; p++)
		if ((*p & 0xc0) != 0x80)
			chars++;
	return ext.x_advance + (chars ? (chars - 1) * spacing : 0);
}

/*
 * Draw text with its vertical middle (cap height) at y, aligned at x.
 * spacing > 0 draws letter-spaced text one character at a time.
 */
static void draw_text(cairo_t *cr, double x, double y, const char *text,
		      double size, int bold, align_t align, double spacing,
		      unsigned rgb)
{
	double w;

	set_font(cr, size, bold);
	w = text_width(cr, text, spacing);
	if (align == ALIGN_CENTER)
		x -= w / 2;
	else if (align == ALIGN_RIGHT)
		x -= w;
	y += size * 0.36;
	set_rgb(cr, rgb);

	if (spacing <= 0) {
		cairo_move_to(cr, x, y);
		cairo_show_text(cr, text);
		return;
	}
	while (*text) {
		char ch[8];
		size_t n = 1;
		cairo_text_extents_t ext;

		while (text[n] && (text[n] & 0xc0) == 0x80 && n < sizeof(ch) - 1)
			n++;
		memcpy(ch, text, n);
		ch[n] = '\0';
		cairo_move_to(cr, x, y);
		cairo_show_text(cr, ch);
		cairo_text_extents(cr, ch, &ext);
		x += ext.x_advance + spacing;
		text += n;
	}
}

/* Shorten text with "..." until it fits max_width at the current font */
static void fit_text(cairo_t *cr, const char *text, double size, int bold,
		     double max_width, char *out, size_t out_size)
{
	size_t len;

	set_font(cr, size, bold);
	snprintf(out, out_size, "%s", text);
	if (text_width(cr, out, 0) <= max_width)
		return;
	for (len = strlen(text); len > 0; len--) {
		if ((text[len] & 0xc0) == 0x80)
			continue;	/* not at a character boundary */
		snprintf(out, out_size, "%.*s...", (int)len, text);
		if (text_width(cr, out, 0) <= max_width)
			return;
	}
	snprintf(out, out_size, "...");
}

static void format_clock(int64_t ms, char *out, size_t size)
{
	time_t t = (time_t)(ms / 1000);
	struct tm tm;

	localtime_r(&t, &tm);
	strftime(out, size, "%H:%M:%S", &tm);
}

static void draw_header(cairo_t *cr, const display_state_t *state,
			unsigned rgb)
{
	char clock[16];

	format_clock(now_ms(), clock, sizeof(clock));
	draw_text(cr, 50, 50, state->config->board_name, 30, 1, ALIGN_LEFT, 0,
		  rgb);
	draw_text(cr, 1950, 50, clock, 36, 1, ALIGN_RIGHT, 0, rgb);
}

/* colours of the default screen */
#define C_BG		0x0e1117
#define C_PANEL		0x1a1d25
#define C_CARD		0x0e1117
#define C_TRACK		0x2b2f3a
#define C_TEXT		0xf2f4f7
#define C_LABEL		0x8b929e
#define C_GREEN		0x34d399
#define C_GREEN_DIM	0x2f8a6e
#define C_ORANGE	0xf5a524
#define C_RED		0xe0323f
#define C_RED_DARK	0xa81c2e

static void draw_class_row(cairo_t *cr, const display_state_t *state, int i,
			   double y)
{
	int index = state->top_n.class_index[i];
	float score = state->top_n.score[i];
	const watched_class_t *w = config_find_watched(state->config, index);
	double bar_x = 144, bar_w = 936, bar_y = y + 32, bar_h = 28;
	double name_max = w ? 760 : 940;
	char buf[AER_NAME_MAX + 8];

	snprintf(buf, sizeof(buf), "%d", i + 1);
	draw_text(cr, 88, y, buf, 28, 1, ALIGN_LEFT, 0, C_LABEL);
	fit_text(cr, state->class_names[index], 36, i == 0, name_max, buf,
		 sizeof(buf));
	draw_text(cr, 144, y, buf, 36, i == 0, ALIGN_LEFT, 0, C_TEXT);

	if (w) {
		double badge_x = 144 + text_width(cr, buf, 0) + 24;

		rounded_rect(cr, badge_x, y - 16, 132, 32, 16);
		set_rgb(cr, C_ORANGE);
		cairo_set_line_width(cr, 2.5);
		cairo_stroke(cr);
		draw_text(cr, badge_x + 66, y, "WATCHED", 20, 1, ALIGN_CENTER,
			  1.5, C_ORANGE);
	}

	snprintf(buf, sizeof(buf), "%.2f", score);
	draw_text(cr, 1237, y, buf, 36, 1, ALIGN_RIGHT, 0, C_TEXT);

	fill_rounded(cr, bar_x, bar_y, bar_w, bar_h, bar_h / 2, C_TRACK);
	if (score > 0.0f) {
		double fill = bar_w * (score > 1.0f ? 1.0 : score);

		if (fill < bar_h)
			fill = bar_h;
		fill_rounded(cr, bar_x, bar_y, fill, bar_h, bar_h / 2,
			     w && score >= w->threshold ? C_RED :
			     i == 0 ? C_GREEN : C_GREEN_DIM);
	}

	if (w) {
		double tx = bar_x + bar_w * w->threshold;

		set_rgb(cr, C_ORANGE);
		cairo_rectangle(cr, tx - 2, bar_y - 16, 4, bar_h + 22);
		cairo_fill(cr);
		snprintf(buf, sizeof(buf), "threshold %.2f", w->threshold);
		draw_text(cr, tx + 10, bar_y + bar_h + 22, buf, 20, 1,
			  ALIGN_LEFT, 0, C_ORANGE);
	}
}

/* Default screen: top classes now heard, input level and recent events */
void render_default(cairo_t *cairo_handle, int width, int height,
		    const display_state_t *state)
{
	cairo_t *cr = cairo_handle;
	int live = state->top_n_rx_ms &&
		   now_ms() - state->top_n_rx_ms < RESULT_STALE_MS;
	char buf[64];
	int i;

	cairo_save(cr);
	cairo_scale(cr, width / DESIGN_W, height / DESIGN_H);
	set_rgb(cr, C_BG);
	cairo_paint(cr);
	draw_header(cr, state, C_TEXT);

	/* NOW HEARING */
	fill_rounded(cr, 50, 138, 1225, 780, 18, C_PANEL);
	draw_text(cr, 88, 185, "NOW HEARING", 26, 1, ALIGN_LEFT, 4.5, C_LABEL);
	if (live) {
		for (i = 0; i < state->top_n.n; i++)
			draw_class_row(cr, state, i, 265 + i * 131);
	} else {
		draw_text(cr, 662, 530, "Waiting for audio...", 36, 0,
			  ALIGN_CENTER, 0, C_LABEL);
	}

	/* INPUT LEVEL */
	fill_rounded(cr, 50, 957, 1225, 243, 18, C_PANEL);
	draw_text(cr, 88, 1004, "INPUT LEVEL", 26, 1, ALIGN_LEFT, 4.5, C_LABEL);
	fill_rounded(cr, 88, 1085, 1149, 43, 21.5, C_TRACK);
	if (live) {
		double db = state->top_n.level_dbfs;
		double frac = (db - LEVEL_MIN_DBFS) / -LEVEL_MIN_DBFS;
		double unit_w;

		set_font(cr, 26, 0);
		unit_w = text_width(cr, "dBFS", 0);
		draw_text(cr, 1237, 1004, "dBFS", 26, 0, ALIGN_RIGHT, 0,
			  C_LABEL);
		/* U+2212 minus sign, as on the mockup */
		snprintf(buf, sizeof(buf), "%s%d", db <= -0.5 ? "\xe2\x88\x92" :
			 "", (int)lround(fabs(db)));
		draw_text(cr, 1237 - unit_w - 10, 1000, buf, 50, 1,
			  ALIGN_RIGHT, 0, C_TEXT);
		frac = frac < 0 ? 0 : frac > 1 ? 1 : frac;
		if (frac > 0)
			fill_rounded(cr, 88, 1085, frac * 1149 < 43 ? 43 :
				     frac * 1149, 43, 21.5,
				     db > -3 ? C_RED : db > -12 ? C_ORANGE :
				     C_GREEN);
	}

	/* RECENT EVENTS */
	fill_rounded(cr, 1313, 138, 637, 1062, 18, C_PANEL);
	draw_text(cr, 1350, 185, "RECENT EVENTS", 26, 1, ALIGN_LEFT, 4.5,
		  C_LABEL);
	if (state->num_recent == 0)
		draw_text(cr, 1631, 330, "No events yet", 30, 0, ALIGN_CENTER,
			  0, C_LABEL);
	for (i = 0; i < state->num_recent; i++) {
		double y = 232 + i * 187;
		char name[AER_NAME_MAX + 4];

		fill_rounded(cr, 1338, y, 587, 168, 16, C_CARD);
		fit_text(cr, state->recent[i].class_name, 36, 1, 540, name,
			 sizeof(name));
		draw_text(cr, 1631, y + 60, name, 36, 1, ALIGN_CENTER, 0,
			  C_TEXT);
		format_clock(state->recent[i].time_ms, buf, sizeof(buf));
		draw_text(cr, 1631, y + 110, buf, 28, 1, ALIGN_CENTER, 0,
			  C_TEXT);
	}
	cairo_restore(cr);
}

static void draw_info_box(cairo_t *cr, double x, const char *label,
			  const char *value)
{
	fill_rounded(cr, x, 685, 359, 156, 10, C_RED_DARK);
	draw_text(cr, x + 179.5, 724, label, 22, 1, ALIGN_CENTER, 4, 0xffffff);
	draw_text(cr, x + 179.5, 786, value, 62, 1, ALIGN_CENTER, 0, 0xffffff);
}

/* Alert screen: detected class, score, threshold and time */
void render_alert(cairo_t *cairo_handle, int width, int height,
		  const display_state_t *state)
{
	cairo_t *cr = cairo_handle;
	char name[AER_NAME_MAX * 2], buf[32];
	double size = 300;
	size_t i;

	cairo_save(cr);
	cairo_scale(cr, width / DESIGN_W, height / DESIGN_H);
	set_rgb(cr, C_RED);
	cairo_paint(cr);
	draw_header(cr, state, 0xffffff);

	draw_text(cr, 1000, 298, "EVENT DETECTED", 50, 1, ALIGN_CENTER, 14,
		  0xffffff);

	/* class name in capitals, shrunk until it fits the screen */
	for (i = 0; state->alert.class_name[i] && i < sizeof(name) - 1; i++)
		name[i] = (char)toupper((unsigned char)state->alert.class_name[i]);
	name[i] = '\0';
	set_font(cr, size, 1);
	while (size > 60 && text_width(cr, name, 0) > 1840) {
		size -= 10;
		set_font(cr, size, 1);
	}
	draw_text(cr, 1000, 495, name, size, 1, ALIGN_CENTER, 0, 0xffffff);

	snprintf(buf, sizeof(buf), "%.2f", state->alert.score);
	draw_info_box(cr, 430, "CONFIDENCE", buf);
	snprintf(buf, sizeof(buf), "%.2f", state->alert.threshold);
	draw_info_box(cr, 820, "THRESHOLD", buf);
	format_clock(state->alert.time_ms, buf, sizeof(buf));
	draw_info_box(cr, 1211, "DETECTED AT", buf);

	fill_rounded(cr, 563, 904, 874, 111, 55.5, 0xffffff);
	draw_text(cr, 1000, 959, "TOUCH ANYWHERE TO DISMISS", 38, 1,
		  ALIGN_CENTER, 2, C_RED);
	cairo_restore(cr);
}

/*
 * Apply a detector result to the display state: store the latest top-N
 * classes, and switch to the alert screen when an event fired. Either
 * argument may be NULL.
 */
void update_display_state(display_state_t *state, const event_t *event,
			  const frame_result_t *top_n_result)
{
	if (top_n_result) {
		state->top_n = *top_n_result;
		state->top_n_rx_ms = now_ms();
	}
	if (event) {
		int n = state->num_recent < NUM_RECENT ? state->num_recent + 1 :
			NUM_RECENT;

		memmove(&state->recent[1], &state->recent[0],
			(size_t)(n - 1) * sizeof(event_t));
		state->recent[0] = *event;
		state->num_recent = n;
		state->alert = *event;
		state->alert_since_ms = now_ms();
		state->mode = MODE_ALERT;
	}
}

static int redraw(ui_t *ui, const display_state_t *state)
{
	shm_buffer_t *b = free_buffer(ui);
	cairo_surface_t *surface;
	cairo_t *cr;

	if (!b)
		return -1;	/* both buffers on screen: try again later */
	surface = cairo_image_surface_create_for_data(b->data,
			CAIRO_FORMAT_RGB24, b->width, b->height, b->width * 4);
	cr = cairo_create(surface);
	if (state->mode == MODE_ALERT)
		render_alert(cr, b->width, b->height, state);
	else
		render_default(cr, b->width, b->height, state);
	cairo_destroy(cr);
	cairo_surface_flush(surface);
	cairo_surface_destroy(surface);

	wl_surface_attach(ui->surface, b->buffer, 0, 0);
	wl_surface_damage(ui->surface, 0, 0, b->width, b->height);
	wl_surface_commit(ui->surface);
	b->busy = 1;
	return 0;
}

/* ------------------------------------------------------------------ Main */

static int handle_detector(int fd, display_state_t *state, void *buf,
			   size_t size)
{
	ssize_t n = receive_on_socket(fd, buf, size);
	uint32_t type;

	if (n == 0 || (n < 0 && errno != EINTR && errno != EAGAIN &&
		       errno != EMSGSIZE))
		return -1;
	if (n < (ssize_t)sizeof(type))
		return 0;
	memcpy(&type, buf, sizeof(type));
	if (type == MSG_EVENT && (size_t)n == sizeof(msg_event_t))
		update_display_state(state, &((msg_event_t *)buf)->event, NULL);
	else if (type == MSG_RESULT && (size_t)n == sizeof(msg_result_t))
		update_display_state(state, NULL, &((msg_result_t *)buf)->top_n);
	else
		return 0;
	return 1;
}

int main(int argc, char **argv)
{
	static char class_names[AER_NUM_CLASSES][AER_NAME_MAX];
	display_state_t state;
	app_config_t config;
	msg_result_t *msg;
	ui_t *ui;
	int det_fd = -1, dirty = 1, last_second = -1;
	int64_t last_try = 0;

	setvbuf(stderr, NULL, _IOLBF, 0);
	config_load_from_args(argc, argv, &config);
	install_signal_handlers(&running);

	memset(&state, 0, sizeof(state));
	state.config = &config;
	state.class_names = class_names;
	load_class_names(config.class_map_path, class_names);
	msg = malloc(sizeof(*msg));
	if (!msg)
		return 1;

	ui = create_window(DEFAULT_WIDTH, DEFAULT_HEIGHT);
	if (!ui)
		return 1;	/* systemd retries until the compositor is up */

	while (running) {
		struct pollfd pfd[2];
		int64_t now = now_ms();
		nfds_t nfds = 1;
		int timeout;

		if (det_fd < 0 && now - last_try >= 1000) {
			last_try = now;
			det_fd = try_connect_to_socket(AER_SOCK_DETECTOR);
			if (det_fd >= 0)
				fprintf(stderr, "connected to %s\n",
					AER_SOCK_DETECTOR);
		}

		if (ui->touched) {
			ui->touched = 0;
			if (state.mode == MODE_ALERT) {
				state.mode = MODE_DEFAULT;
				dirty = 1;
			}
		}
		if (state.mode == MODE_ALERT && config.alert_dismiss_sec > 0 &&
		    now - state.alert_since_ms >=
		    config.alert_dismiss_sec * 1000LL) {
			state.mode = MODE_DEFAULT;
			dirty = 1;
		}
		if ((int)(now / 1000) != last_second) {	/* clock */
			last_second = (int)(now / 1000);
			dirty = 1;
		}
		if (dirty && redraw(ui, &state) == 0)
			dirty = 0;

		/* standard Wayland read: prepare, flush, poll, read */
		while (wl_display_prepare_read(ui->display) != 0)
			wl_display_dispatch_pending(ui->display);
		wl_display_flush(ui->display);

		pfd[0].fd = wl_display_get_fd(ui->display);
		pfd[0].events = POLLIN;
		if (det_fd >= 0) {
			pfd[1].fd = det_fd;
			pfd[1].events = POLLIN;
			nfds = 2;
		}
		timeout = 1000 - (int)(now_ms() % 1000) + 5;
		if (dirty)
			timeout = 50;	/* waiting for a free buffer */
		if (poll(pfd, nfds, timeout) < 0 && errno != EINTR) {
			wl_display_cancel_read(ui->display);
			break;
		}
		if (pfd[0].revents & POLLIN) {
			if (wl_display_read_events(ui->display) < 0)
				break;
		} else {
			wl_display_cancel_read(ui->display);
		}
		if (wl_display_dispatch_pending(ui->display) < 0) {
			fprintf(stderr, "Wayland connection lost\n");
			break;
		}
		if (pfd[0].revents & (POLLHUP | POLLERR)) {
			fprintf(stderr, "Wayland connection closed\n");
			break;
		}

		if (nfds == 2 && pfd[1].revents) {
			int r = handle_detector(det_fd, &state, msg,
						sizeof(*msg));

			if (r < 0) {
				fprintf(stderr, "detector disconnected\n");
				close(det_fd);
				det_fd = -1;
				dirty = 1;
			} else if (r > 0) {
				dirty = 1;
			}
		}
	}

	if (det_fd >= 0)
		close(det_fd);
	free(msg);
	/* exit code 1 if the compositor went away so systemd restarts us */
	return running ? 1 : 0;
}
