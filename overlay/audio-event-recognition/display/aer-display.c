// SPDX-License-Identifier: MIT
/*
 * aer-display - module 6-1: full-screen status dashboard on the LCD (Weston).
 *
 *   AER_MSG_STATUS -> live view: top class, input level, score of every
 *                     event type against its threshold
 *   AER_MSG_EVENT  -> red alert banner (held for alert_hold_sec or until the
 *                     screen is touched) and the recent events list
 *
 * Plain wl_shm + xdg-shell client drawn with cairo. Started by systemd as the
 * Weston user; it waits for the compositor socket in $XDG_RUNTIME_DIR and
 * for the aer-detect socket, so start order does not matter.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <cairo/cairo.h>
#include <wayland-client.h>

#include "xdg-shell-client-protocol.h"

#include "aer.h"
#include "conf.h"
#include "ipc.h"

#define MAX_RECENT	6
#define STALE_MS	3000
#define FONT		"DejaVu Sans"

struct buffer {
	struct wl_buffer *wl;
	void *data;
	size_t size;
	bool busy;
};

struct recent {
	struct aer_event ev;
	int64_t t_ns;
};

struct app {
	struct wl_display *display;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct xdg_wm_base *wm_base;
	struct wl_seat *seat;
	struct wl_touch *touch;
	struct wl_pointer *pointer;

	struct wl_surface *surface;
	struct xdg_surface *xdg_surface;
	struct xdg_toplevel *toplevel;
	struct wl_callback *frame_cb;

	struct buffer buffers[2];
	int width, height;
	int pending_w, pending_h;
	bool configured, dirty, running;

	/* pipeline state */
	char device[64];
	bool have_status;
	struct aer_status st;
	int64_t status_ms;
	bool alert;
	struct aer_event alert_ev;
	int64_t alert_t_ns, alert_until_ms;
	int64_t hold_ms;
	struct recent recent[MAX_RECENT];
	int nrecent;
	time_t shown_sec;
	int64_t shown_phase;
};

/* ------------------------------------------------------------- drawing */

struct rgb {
	double r, g, b;
};

#define RGB(hex) ((struct rgb){ ((hex) >> 16 & 0xff) / 255.0, ((hex) >> 8 & 0xff) / 255.0, ((hex) & 0xff) / 255.0 })

static const struct rgb C_BG = RGB(0x101418), C_PANEL = RGB(0x1b2229), C_BAR = RGB(0x26303a),
			C_TEXT = RGB(0xe8e8e8), C_DIM = RGB(0x8a96a0), C_OK = RGB(0x2fae5f),
			C_OK_BG = RGB(0x173524), C_WARN = RGB(0xf0b429), C_ALERT = RGB(0xd93a2b),
			C_ALERT2 = RGB(0x8f1d14), C_TRACK = RGB(0x323d48);

enum align { LEFT, CENTER, RIGHT };

static void set_color(cairo_t *cr, struct rgb c)
{
	cairo_set_source_rgb(cr, c.r, c.g, c.b);
}

static void rounded_rect(cairo_t *cr, double x, double y, double w, double h, double r)
{
	/* short bars (low scores) must not get corners wider than themselves */
	if (r > w / 2)
		r = w / 2;
	if (r > h / 2)
		r = h / 2;
	cairo_new_sub_path(cr);
	cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
	cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
	cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
	cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
	cairo_close_path(cr);
}

static void fill_rect(cairo_t *cr, double x, double y, double w, double h, double r, struct rgb c)
{
	set_color(cr, c);
	rounded_rect(cr, x, y, w, h, r);
	cairo_fill(cr);
}

/* y is the text baseline; shrinks the font until the text fits max_w (0: no limit) */
static double text(cairo_t *cr, double x, double y, double size, bool bold, struct rgb c,
		   enum align al, double max_w, const char *s)
{
	cairo_text_extents_t ext;

	cairo_select_font_face(cr, FONT, CAIRO_FONT_SLANT_NORMAL,
			       bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
	for (;;) {
		cairo_set_font_size(cr, size);
		cairo_text_extents(cr, s, &ext);
		if (!max_w || ext.x_advance <= max_w || size < 8)
			break;
		size *= 0.92;
	}
	if (al == CENTER)
		x -= ext.x_advance / 2;
	else if (al == RIGHT)
		x -= ext.x_advance;
	set_color(cr, c);
	cairo_move_to(cr, x, y);
	cairo_show_text(cr, s);
	return ext.x_advance;
}

static void hms(int64_t t_ns, char *buf, size_t len)
{
	time_t sec = t_ns / 1000000000;
	struct tm tm;

	localtime_r(&sec, &tm);
	strftime(buf, len, "%H:%M:%S", &tm);
}

static void upper(char *dst, size_t len, const char *src)
{
	size_t i;

	for (i = 0; i + 1 < len && src[i]; i++)
		dst[i] = (src[i] >= 'a' && src[i] <= 'z') ? src[i] - 32 : src[i];
	dst[i] = '\0';
}

static void render(struct app *a, void *data)
{
	const double W = a->width, H = a->height;
	const double s = W / 1280.0 < H / 800.0 ? W / 1280.0 : H / 800.0;
	const double m = 28 * s;	/* outer margin */
	int64_t now_ms = aer_mono_ms();
	bool live = a->have_status && now_ms - a->status_ms < STALE_MS;
	cairo_surface_t *surf;
	cairo_t *cr;
	char buf[160], tbuf[32];
	double top, bx, by, bw, bh, px, pw, py, ph;

	surf = cairo_image_surface_create_for_data(data, CAIRO_FORMAT_RGB24, a->width, a->height,
						   a->width * 4);
	cr = cairo_create(surf);
	set_color(cr, C_BG);
	cairo_paint(cr);

	/* ---- top bar */
	top = 72 * s;
	set_color(cr, C_BAR);
	cairo_rectangle(cr, 0, 0, W, top);
	cairo_fill(cr);
	text(cr, m, top * 0.64, 30 * s, true, C_TEXT, LEFT, W * 0.45, "Audio Event Recognition");
	{
		time_t t = time(NULL);
		struct tm tm;
		double x;

		localtime_r(&t, &tm);
		strftime(tbuf, sizeof(tbuf), "%Y-%m-%d  %H:%M:%S", &tm);
		x = W - m - text(cr, W - m, top * 0.64, 22 * s, false, C_DIM, RIGHT, 0, tbuf);
		x -= 32 * s;
		x -= text(cr, x, top * 0.64, 22 * s, true, C_TEXT, RIGHT, 0, a->device);
		x -= 30 * s;
		set_color(cr, live ? C_OK : C_DIM);
		cairo_arc(cr, x, top * 0.5, 9 * s, 0, 2 * M_PI);
		cairo_fill(cr);
	}

	/* ---- banner */
	bx = m;
	by = top + m;
	bw = W - 2 * m;
	bh = 230 * s;
	if (a->alert) {
		bool flash = (now_ms / 500) % 2;
		char title[AER_EVENT_NAME_LEN + 16], when[16];

		fill_rect(cr, bx, by, bw, bh, 18 * s, flash ? C_ALERT : C_ALERT2);
		upper(title, sizeof(title), a->alert_ev.type);
		strncat(title, " DETECTED", sizeof(title) - strlen(title) - 1);
		text(cr, bx + bw / 2, by + bh * 0.50, 76 * s, true, C_TEXT, CENTER, bw - 2 * m, title);
		hms(a->alert_t_ns, when, sizeof(when));
		snprintf(buf, sizeof(buf), "score %.2f  \xc2\xb7  %s  \xc2\xb7  %s", a->alert_ev.score,
			 a->alert_ev.cls, when);
		text(cr, bx + bw / 2, by + bh * 0.74, 28 * s, false, C_TEXT, CENTER, bw - 2 * m, buf);
		text(cr, bx + bw / 2, by + bh * 0.91, 18 * s, false, RGB(0xf6c9c4), CENTER, 0,
		     "touch the screen to dismiss");
	} else {
		fill_rect(cr, bx, by, bw, bh, 18 * s, live ? C_OK_BG : C_PANEL);
		if (live) {
			text(cr, bx + bw / 2, by + bh * 0.52, 76 * s, true, C_OK, CENTER, bw - 2 * m,
			     "LISTENING");
			snprintf(buf, sizeof(buf), "now hearing: %s (%.2f)", a->st.top_name,
				 a->st.top_score);
			text(cr, bx + bw / 2, by + bh * 0.78, 28 * s, false, C_TEXT, CENTER, bw - 2 * m,
			     buf);
		} else {
			text(cr, bx + bw / 2, by + bh * 0.52, 64 * s, true, C_DIM, CENTER, bw - 2 * m,
			     "WAITING FOR AUDIO");
			text(cr, bx + bw / 2, by + bh * 0.78, 24 * s, false, C_DIM, CENTER, bw - 2 * m,
			     a->have_status ? "no data from the recognition pipeline"
					    : "recognition pipeline is starting");
		}
	}

	/* ---- event scores (left) */
	px = m;
	py = by + bh + m;
	pw = (W - 3 * m) * 0.58;
	ph = H - py - m;
	fill_rect(cr, px, py, pw, ph, 14 * s, C_PANEL);
	text(cr, px + 24 * s, py + 40 * s, 22 * s, true, C_DIM, LEFT, 0, "EVENT SCORES");
	if (a->have_status && a->st.n_events) {
		int n = (int)a->st.n_events;
		double row = (ph - 70 * s) / (n < 5 ? 5 : n);
		double lx = px + 24 * s, lw = 170 * s;
		double tx = lx + lw, tw = pw - lw - 140 * s;

		for (int i = 0; i < n && i < AER_MAX_EVENTS; i++) {
			double y = py + 64 * s + row * i, cy = y + row / 2;
			double sc = a->st.ev[i].score, th = a->st.ev[i].threshold;
			bool over = sc >= th;

			if (sc < 0)
				sc = 0;
			if (sc > 1)
				sc = 1;
			text(cr, lx, cy + 9 * s, 24 * s, true, over ? C_ALERT : C_TEXT, LEFT, lw - 10 * s,
			     a->st.ev[i].name);
			fill_rect(cr, tx, cy - 12 * s, tw, 24 * s, 6 * s, C_TRACK);
			if (sc > 0.005)
				fill_rect(cr, tx, cy - 12 * s, tw * sc, 24 * s, 6 * s,
					  over ? C_ALERT : (sc >= th * 0.6 ? C_WARN : C_OK));
			set_color(cr, C_TEXT);
			cairo_rectangle(cr, tx + tw * th - 1.5 * s, cy - 18 * s, 3 * s, 36 * s);
			cairo_fill(cr);
			snprintf(buf, sizeof(buf), "%.2f", a->st.ev[i].score);
			text(cr, px + pw - 24 * s, cy + 9 * s, 24 * s, false, C_TEXT, RIGHT, 0, buf);
		}
	} else {
		text(cr, px + pw / 2, py + ph / 2, 22 * s, false, C_DIM, CENTER, 0, "no data");
	}

	/* ---- level, inference time, recent events (right) */
	px = px + pw + m;
	pw = W - px - m;
	fill_rect(cr, px, py, pw, ph, 14 * s, C_PANEL);
	{
		double lx = px + 24 * s, lw = pw - 48 * s, y = py + 40 * s;
		double lvl = live ? a->st.level_dbfs : -120, frac = (lvl + 90) / 90;

		text(cr, lx, y, 22 * s, true, C_DIM, LEFT, 0, "INPUT LEVEL");
		snprintf(buf, sizeof(buf), live ? "%.0f dBFS" : "-", lvl);
		text(cr, lx + lw, y, 22 * s, false, C_TEXT, RIGHT, 0, buf);
		y += 18 * s;
		frac = frac < 0 ? 0 : frac > 1 ? 1 : frac;
		fill_rect(cr, lx, y, lw, 20 * s, 6 * s, C_TRACK);
		if (frac > 0.01)
			fill_rect(cr, lx, y, lw * frac, 20 * s, 6 * s,
				  lvl > -6 ? C_ALERT : lvl > -20 ? C_WARN : C_OK);

		y += 62 * s;
		text(cr, lx, y, 22 * s, true, C_DIM, LEFT, 0, "INFERENCE");
		snprintf(buf, sizeof(buf), live ? "%.0f ms / frame" : "-", a->st.infer_ms);
		text(cr, lx + lw, y, 22 * s, false, C_TEXT, RIGHT, 0, buf);

		y += 56 * s;
		text(cr, lx, y, 22 * s, true, C_DIM, LEFT, 0, "RECENT EVENTS");
		y += 12 * s;
		if (!a->nrecent)
			text(cr, lx, y + 36 * s, 22 * s, false, C_DIM, LEFT, 0, "none yet");
		for (int i = 0; i < a->nrecent; i++) {
			const struct recent *r = &a->recent[i];
			double ry = y + 36 * s + i * 38 * s;

			if (ry > py + ph - 12 * s)
				break;
			hms(r->t_ns, tbuf, sizeof(tbuf));
			text(cr, lx, ry, 22 * s, false, C_DIM, LEFT, 0, tbuf);
			text(cr, lx + 120 * s, ry, 22 * s, true, i == 0 && a->alert ? C_ALERT : C_TEXT,
			     LEFT, lw - 200 * s, r->ev.type);
			snprintf(buf, sizeof(buf), "%.2f", r->ev.score);
			text(cr, lx + lw, ry, 22 * s, false, C_TEXT, RIGHT, 0, buf);
		}
	}

	cairo_destroy(cr);
	cairo_surface_flush(surf);
	cairo_surface_destroy(surf);
}

/* ------------------------------------------------------------- buffers */

static void buffer_release(void *data, struct wl_buffer *wl_buffer)
{
	struct buffer *b = data;

	(void)wl_buffer;
	b->busy = false;
}

static const struct wl_buffer_listener buffer_listener = {
	.release = buffer_release,
};

static void destroy_buffer(struct buffer *b)
{
	if (b->wl)
		wl_buffer_destroy(b->wl);
	if (b->data)
		munmap(b->data, b->size);
	memset(b, 0, sizeof(*b));
}

static int create_buffer(struct app *a, struct buffer *b)
{
	int stride = a->width * 4;
	struct wl_shm_pool *pool;
	int fd;

	b->size = (size_t)stride * a->height;
	fd = memfd_create("aer-display", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, (off_t)b->size) < 0) {
		aer_log("shm: %s", strerror(errno));
		if (fd >= 0)
			close(fd);
		return -1;
	}
	b->data = mmap(NULL, b->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (b->data == MAP_FAILED) {
		aer_log("mmap: %s", strerror(errno));
		close(fd);
		b->data = NULL;
		return -1;
	}
	pool = wl_shm_create_pool(a->shm, fd, (int32_t)b->size);
	b->wl = wl_shm_pool_create_buffer(pool, 0, a->width, a->height, stride,
					  WL_SHM_FORMAT_XRGB8888);
	wl_buffer_add_listener(b->wl, &buffer_listener, b);
	wl_shm_pool_destroy(pool);
	close(fd);
	return 0;
}

static struct buffer *next_buffer(struct app *a)
{
	for (int i = 0; i < 2; i++) {
		struct buffer *b = &a->buffers[i];

		if (b->busy)
			continue;
		if (!b->wl && create_buffer(a, b) < 0)
			return NULL;
		return b;
	}
	return NULL;
}

static const struct wl_callback_listener frame_listener;

static void redraw(struct app *a)
{
	struct buffer *b;

	if (!a->configured || a->frame_cb) {
		a->dirty = true;
		return;
	}
	b = next_buffer(a);
	if (!b) {
		a->dirty = true;	/* retried on the next frame callback */
		return;
	}
	render(a, b->data);
	wl_surface_attach(a->surface, b->wl, 0, 0);
	wl_surface_damage_buffer(a->surface, 0, 0, a->width, a->height);
	a->frame_cb = wl_surface_frame(a->surface);
	wl_callback_add_listener(a->frame_cb, &frame_listener, a);
	wl_surface_commit(a->surface);
	b->busy = true;
	a->dirty = false;
}

static void frame_done(void *data, struct wl_callback *cb, uint32_t time)
{
	struct app *a = data;

	(void)time;
	wl_callback_destroy(cb);
	a->frame_cb = NULL;
	if (a->dirty)
		redraw(a);
}

static const struct wl_callback_listener frame_listener = {
	.done = frame_done,
};

/* ----------------------------------------------------------- input */

static void dismiss(struct app *a)
{
	if (a->alert) {
		a->alert = false;
		redraw(a);
	}
}

static void touch_down(void *data, struct wl_touch *t, uint32_t serial, uint32_t time,
		       struct wl_surface *surface, int32_t id, wl_fixed_t x, wl_fixed_t y)
{
	(void)t; (void)serial; (void)time; (void)surface; (void)id; (void)x; (void)y;
	dismiss(data);
}

static void touch_up(void *data, struct wl_touch *t, uint32_t serial, uint32_t time, int32_t id)
{
	(void)data; (void)t; (void)serial; (void)time; (void)id;
}

static void touch_motion(void *data, struct wl_touch *t, uint32_t time, int32_t id,
			 wl_fixed_t x, wl_fixed_t y)
{
	(void)data; (void)t; (void)time; (void)id; (void)x; (void)y;
}

static void touch_frame(void *data, struct wl_touch *t)
{
	(void)data; (void)t;
}

static void touch_cancel(void *data, struct wl_touch *t)
{
	(void)data; (void)t;
}

static void touch_shape(void *data, struct wl_touch *t, int32_t id, wl_fixed_t major,
			wl_fixed_t minor)
{
	(void)data; (void)t; (void)id; (void)major; (void)minor;
}

static void touch_orientation(void *data, struct wl_touch *t, int32_t id, wl_fixed_t o)
{
	(void)data; (void)t; (void)id; (void)o;
}

static const struct wl_touch_listener touch_listener = {
	.down = touch_down,
	.up = touch_up,
	.motion = touch_motion,
	.frame = touch_frame,
	.cancel = touch_cancel,
	.shape = touch_shape,
	.orientation = touch_orientation,
};

static void ptr_enter(void *data, struct wl_pointer *p, uint32_t serial, struct wl_surface *s,
		      wl_fixed_t x, wl_fixed_t y)
{
	(void)data; (void)p; (void)serial; (void)s; (void)x; (void)y;
}

static void ptr_leave(void *data, struct wl_pointer *p, uint32_t serial, struct wl_surface *s)
{
	(void)data; (void)p; (void)serial; (void)s;
}

static void ptr_motion(void *data, struct wl_pointer *p, uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
	(void)data; (void)p; (void)time; (void)x; (void)y;
}

static void ptr_button(void *data, struct wl_pointer *p, uint32_t serial, uint32_t time,
		       uint32_t button, uint32_t state)
{
	(void)p; (void)serial; (void)time; (void)button;
	if (state == WL_POINTER_BUTTON_STATE_PRESSED)
		dismiss(data);
}

static void ptr_axis(void *data, struct wl_pointer *p, uint32_t time, uint32_t axis, wl_fixed_t v)
{
	(void)data; (void)p; (void)time; (void)axis; (void)v;
}

static void ptr_frame(void *data, struct wl_pointer *p)
{
	(void)data; (void)p;
}

static void ptr_axis_source(void *data, struct wl_pointer *p, uint32_t src)
{
	(void)data; (void)p; (void)src;
}

static void ptr_axis_stop(void *data, struct wl_pointer *p, uint32_t time, uint32_t axis)
{
	(void)data; (void)p; (void)time; (void)axis;
}

static void ptr_axis_discrete(void *data, struct wl_pointer *p, uint32_t axis, int32_t d)
{
	(void)data; (void)p; (void)axis; (void)d;
}

static const struct wl_pointer_listener pointer_listener = {
	.enter = ptr_enter,
	.leave = ptr_leave,
	.motion = ptr_motion,
	.button = ptr_button,
	.axis = ptr_axis,
	.frame = ptr_frame,
	.axis_source = ptr_axis_source,
	.axis_stop = ptr_axis_stop,
	.axis_discrete = ptr_axis_discrete,
};

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps)
{
	struct app *a = data;

	if ((caps & WL_SEAT_CAPABILITY_TOUCH) && !a->touch) {
		a->touch = wl_seat_get_touch(seat);
		wl_touch_add_listener(a->touch, &touch_listener, a);
	} else if (!(caps & WL_SEAT_CAPABILITY_TOUCH) && a->touch) {
		wl_touch_release(a->touch);
		a->touch = NULL;
	}
	if ((caps & WL_SEAT_CAPABILITY_POINTER) && !a->pointer) {
		a->pointer = wl_seat_get_pointer(seat);
		wl_pointer_add_listener(a->pointer, &pointer_listener, a);
	} else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && a->pointer) {
		wl_pointer_release(a->pointer);
		a->pointer = NULL;
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

/* ------------------------------------------------------- xdg-shell */

static void wm_base_ping(void *data, struct xdg_wm_base *wm_base, uint32_t serial)
{
	(void)data;
	xdg_wm_base_pong(wm_base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
	.ping = wm_base_ping,
};

static void xdg_surface_configure(void *data, struct xdg_surface *s, uint32_t serial)
{
	struct app *a = data;
	int w = a->pending_w > 0 ? a->pending_w : 1280;
	int h = a->pending_h > 0 ? a->pending_h : 800;

	xdg_surface_ack_configure(s, serial);
	if (w != a->width || h != a->height) {
		for (int i = 0; i < 2; i++)
			destroy_buffer(&a->buffers[i]);
		a->width = w;
		a->height = h;
		aer_log("surface %dx%d", w, h);
	}
	a->configured = true;
	redraw(a);
}

static const struct xdg_surface_listener xdg_surface_listener = {
	.configure = xdg_surface_configure,
};

static void toplevel_configure(void *data, struct xdg_toplevel *tl, int32_t w, int32_t h,
			       struct wl_array *states)
{
	struct app *a = data;

	(void)tl; (void)states;
	a->pending_w = w;
	a->pending_h = h;
}

static void toplevel_close(void *data, struct xdg_toplevel *tl)
{
	(void)tl;
	((struct app *)data)->running = false;
}

static void toplevel_configure_bounds(void *data, struct xdg_toplevel *tl, int32_t w, int32_t h)
{
	(void)data; (void)tl; (void)w; (void)h;
}

static void toplevel_wm_capabilities(void *data, struct xdg_toplevel *tl, struct wl_array *caps)
{
	(void)data; (void)tl; (void)caps;
}

static const struct xdg_toplevel_listener toplevel_listener = {
	.configure = toplevel_configure,
	.close = toplevel_close,
	.configure_bounds = toplevel_configure_bounds,
	.wm_capabilities = toplevel_wm_capabilities,
};

static uint32_t min_u32(uint32_t a, uint32_t b)
{
	return a < b ? a : b;
}

static void registry_global(void *data, struct wl_registry *reg, uint32_t name,
			    const char *iface, uint32_t version)
{
	struct app *a = data;

	if (!strcmp(iface, wl_compositor_interface.name)) {
		a->compositor = wl_registry_bind(reg, name, &wl_compositor_interface,
						 min_u32(version, 4));
	} else if (!strcmp(iface, wl_shm_interface.name)) {
		a->shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
	} else if (!strcmp(iface, xdg_wm_base_interface.name)) {
		a->wm_base = wl_registry_bind(reg, name, &xdg_wm_base_interface, 1);
		xdg_wm_base_add_listener(a->wm_base, &wm_base_listener, a);
	} else if (!strcmp(iface, wl_seat_interface.name) && !a->seat) {
		a->seat = wl_registry_bind(reg, name, &wl_seat_interface, min_u32(version, 5));
		wl_seat_add_listener(a->seat, &seat_listener, a);
	}
}

static void registry_global_remove(void *data, struct wl_registry *reg, uint32_t name)
{
	(void)data; (void)reg; (void)name;
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

/* ------------------------------------------------------------ setup */

/*
 * Connects to $WAYLAND_DISPLAY, or to the first wayland-N socket found in
 * $XDG_RUNTIME_DIR (Weston 14 names its socket wayland-1). Waits until the
 * compositor is up.
 */
static struct wl_display *connect_wayland(void)
{
	char rt[64];
	bool warned = false;

	if (!getenv("XDG_RUNTIME_DIR")) {
		snprintf(rt, sizeof(rt), "/run/user/%u", (unsigned int)getuid());
		setenv("XDG_RUNTIME_DIR", rt, 1);
	}

	while (!aer_quit) {
		const char *env = getenv("WAYLAND_DISPLAY");
		struct wl_display *d = NULL;

		if (env && *env) {
			d = wl_display_connect(NULL);
		} else {
			DIR *dir = opendir(getenv("XDG_RUNTIME_DIR"));
			struct dirent *de;

			while (dir && !d && (de = readdir(dir))) {
				if (strncmp(de->d_name, "wayland-", 8) || strchr(de->d_name, '.'))
					continue;
				d = wl_display_connect(de->d_name);
				if (d)
					aer_log("connected to compositor %s/%s",
						getenv("XDG_RUNTIME_DIR"), de->d_name);
			}
			if (dir)
				closedir(dir);
		}
		if (d)
			return d;
		if (!warned)
			aer_log("waiting for the Wayland compositor in %s", getenv("XDG_RUNTIME_DIR"));
		warned = true;
		sleep(2);
	}
	return NULL;
}

static void handle_message(struct app *a, const uint8_t *msg, ssize_t len)
{
	const struct aer_hdr *h = (const struct aer_hdr *)msg;

	if (aer_msg_valid(msg, len, AER_MSG_STATUS)) {
		memcpy(&a->st, h + 1, sizeof(a->st));
		a->have_status = true;
		a->status_ms = aer_mono_ms();
		redraw(a);
	} else if (aer_msg_valid(msg, len, AER_MSG_EVENT)) {
		const struct aer_event *ev = (const struct aer_event *)(h + 1);

		memmove(&a->recent[1], &a->recent[0], (MAX_RECENT - 1) * sizeof(a->recent[0]));
		a->recent[0].ev = *ev;
		a->recent[0].t_ns = h->t_ns;
		if (a->nrecent < MAX_RECENT)
			a->nrecent++;
		a->alert = true;
		a->alert_ev = *ev;
		a->alert_t_ns = h->t_ns;
		a->alert_until_ms = aer_mono_ms() + a->hold_ms;
		redraw(a);
	}
}

int main(int argc, char **argv)
{
	static uint8_t msg[AER_MAX_MSG];
	struct app a = { .running = true };
	struct wl_registry *registry;
	struct aer_client in;
	struct aer_conf *conf;
	int ret = 0;

	conf = aer_conf_load(aer_parse_common(argc, argv, NULL, NULL, NULL));
	aer_signals();
	aer_device_name(conf, a.device, sizeof(a.device));
	a.hold_ms = (int64_t)(aer_conf_double(conf, "display", "alert_hold_sec", 10.0) * 1000);
	aer_client_init(&in, aer_run_dir(conf), "detect");

	a.display = connect_wayland();
	if (!a.display)
		return 0;
	registry = wl_display_get_registry(a.display);
	wl_registry_add_listener(registry, &registry_listener, &a);
	wl_display_roundtrip(a.display);
	if (!a.compositor || !a.shm || !a.wm_base) {
		aer_log("compositor lacks wl_compositor, wl_shm or xdg_wm_base");
		return 1;
	}

	a.surface = wl_compositor_create_surface(a.compositor);
	a.xdg_surface = xdg_wm_base_get_xdg_surface(a.wm_base, a.surface);
	xdg_surface_add_listener(a.xdg_surface, &xdg_surface_listener, &a);
	a.toplevel = xdg_surface_get_toplevel(a.xdg_surface);
	xdg_toplevel_add_listener(a.toplevel, &toplevel_listener, &a);
	xdg_toplevel_set_title(a.toplevel, "Audio Event Recognition");
	xdg_toplevel_set_app_id(a.toplevel, "aer-display");
	xdg_toplevel_set_fullscreen(a.toplevel, NULL);
	wl_surface_commit(a.surface);

	while (a.running && !aer_quit) {
		struct pollfd pfd[2];
		int n = 1;

		while (wl_display_prepare_read(a.display) != 0)
			if (wl_display_dispatch_pending(a.display) < 0)
				goto lost;
		wl_display_flush(a.display);

		pfd[0] = (struct pollfd){ .fd = wl_display_get_fd(a.display), .events = POLLIN };
		aer_client_connect(&in);
		if (in.fd >= 0)
			pfd[n++] = (struct pollfd){ .fd = in.fd, .events = POLLIN };

		if (poll(pfd, (nfds_t)n, 250) > 0 && (pfd[0].revents & POLLIN)) {
			if (wl_display_read_events(a.display) < 0)
				goto lost;
		} else {
			wl_display_cancel_read(a.display);
		}
		if ((pfd[0].revents & (POLLERR | POLLHUP)) || wl_display_dispatch_pending(a.display) < 0)
			goto lost;

		if (n > 1 && pfd[1].revents) {
			ssize_t len;

			/* drain everything queued so the screen shows the latest state */
			while ((len = aer_client_read(&in, msg, sizeof(msg))) > 0)
				handle_message(&a, msg, len);
		}

		/* clock tick, alert flashing (500 ms phases) and expiry, data going stale */
		{
			time_t now = time(NULL);
			int64_t now_ms = aer_mono_ms(), phase;

			if (a.alert && now_ms >= a.alert_until_ms)
				a.alert = false;
			phase = a.alert ? now_ms / 500 : -1;
			if (now != a.shown_sec || phase != a.shown_phase) {
				a.shown_sec = now;
				a.shown_phase = phase;
				redraw(&a);
			}
		}
	}
	goto out;

lost:
	aer_log("lost the connection to the compositor");
	ret = 1;
out:
	aer_client_close(&in);
	if (a.touch)
		wl_touch_release(a.touch);
	if (a.pointer)
		wl_pointer_release(a.pointer);
	for (int i = 0; i < 2; i++)
		destroy_buffer(&a.buffers[i]);
	if (a.toplevel)
		xdg_toplevel_destroy(a.toplevel);
	if (a.xdg_surface)
		xdg_surface_destroy(a.xdg_surface);
	if (a.surface)
		wl_surface_destroy(a.surface);
	wl_display_disconnect(a.display);
	aer_conf_free(conf);
	return ret;
}
