// SPDX-License-Identifier: MIT
/*
 * touch-demo: full-screen Wayland touch test for the BSD101WX1-300 panel.
 *
 * - Five targets (four corners and the centre) turn green when touched and
 *   show how far the touch landed from the target centre (DX/DY in pixels).
 * - Every finger draws a coloured trail and gets a crosshair with its
 *   ID and X/Y coordinates.
 * - The top bar shows whether the compositor offers a touch device, the
 *   number of active touches and the number of targets hit.
 * - CLEAR wipes the trails and targets; EXIT quits.
 *
 * Plain wl_shm rendering, no toolkit; only libwayland-client is needed.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"

#define MAX_TOUCH	10
#define NUM_TARGETS	5
#define BAR_H		64
#define TARGET_R	32
#define HIT_R		60
#define TRAIL_R		4

#define COL_BG		0xff101418
#define COL_GRID	0xff1c232a
#define COL_BAR		0xff26303a
#define COL_TEXT	0xffe8e8e8
#define COL_DIM		0xff8a96a0
#define COL_BTN		0xff3d4b58
#define COL_EXIT	0xff8a2a2a
#define COL_TARGET	0xffd0d0d0
#define COL_HIT		0xff30c050

static const uint32_t touch_colors[MAX_TOUCH] = {
	0xffff5050, 0xff50a0ff, 0xffffd040, 0xff50e0a0, 0xffd070ff,
	0xffff9040, 0xff40e0e0, 0xffff70b0, 0xffa0e040, 0xffffffff,
};

struct buffer {
	struct wl_buffer *wl;
	uint32_t *data;
	size_t size;
	bool busy;
};

struct touch_point {
	bool active;
	int32_t id;
	double x, y;
	uint32_t color;
};

struct target {
	int x, y;
	bool hit;
	int dx, dy;
};

struct rect {
	int x, y, w, h;
};

struct app {
	struct wl_display *display;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct xdg_wm_base *wm_base;
	struct wl_seat *seat;
	struct wl_touch *touch;

	struct wl_surface *surface;
	struct xdg_surface *xdg_surface;
	struct xdg_toplevel *toplevel;
	struct wl_callback *frame_cb;

	struct buffer buffers[2];
	uint32_t *canvas;		/* persistent trails */
	int width, height;
	int pending_w, pending_h;
	bool configured;
	bool dirty;
	bool running;

	struct touch_point touches[MAX_TOUCH];
	struct target targets[NUM_TARGETS];
	struct rect btn_clear, btn_exit;
	unsigned int total_downs;
};

/* ---------------------------------------------------------------- font */

/* 5x7 bitmap font, one byte per row, bit 4 = leftmost column */
static const uint8_t *glyph(char c)
{
	static const uint8_t digits[10][7] = {
		{0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},
		{0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}, {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E},
		{0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},
		{0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, {0x1F,0x01,0x02,0x04,0x08,0x08,0x08},
		{0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},
	};
	static const uint8_t letters[26][7] = {
		{0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}, {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},
		{0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}, {0x1C,0x12,0x11,0x11,0x11,0x12,0x1C},
		{0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}, {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},
		{0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}, {0x11,0x11,0x11,0x1F,0x11,0x11,0x11},
		{0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}, {0x07,0x02,0x02,0x02,0x02,0x12,0x0C},
		{0x11,0x12,0x14,0x18,0x14,0x12,0x11}, {0x10,0x10,0x10,0x10,0x10,0x10,0x1F},
		{0x11,0x1B,0x15,0x15,0x11,0x11,0x11}, {0x11,0x11,0x19,0x15,0x13,0x11,0x11},
		{0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}, {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},
		{0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}, {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},
		{0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}, {0x1F,0x04,0x04,0x04,0x04,0x04,0x04},
		{0x11,0x11,0x11,0x11,0x11,0x11,0x0E}, {0x11,0x11,0x11,0x11,0x11,0x0A,0x04},
		{0x11,0x11,0x11,0x15,0x15,0x15,0x0A}, {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11},
		{0x11,0x11,0x11,0x0A,0x04,0x04,0x04}, {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F},
	};
	static const uint8_t colon[7] = {0x00,0x0C,0x0C,0x00,0x0C,0x0C,0x00};
	static const uint8_t slash[7] = {0x00,0x01,0x02,0x04,0x08,0x10,0x00};
	static const uint8_t minus[7] = {0x00,0x00,0x00,0x1F,0x00,0x00,0x00};
	static const uint8_t dot[7]   = {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C};

	if (c >= '0' && c <= '9')
		return digits[c - '0'];
	if (c >= 'a' && c <= 'z')
		c -= 'a' - 'A';
	if (c >= 'A' && c <= 'Z')
		return letters[c - 'A'];
	switch (c) {
	case ':': return colon;
	case '/': return slash;
	case '-': return minus;
	case '.': return dot;
	default:  return NULL;	/* space and anything unknown */
	}
}

/* ------------------------------------------------------------- drawing */

static void fill_rect(uint32_t *buf, int w, int h, int x, int y, int rw, int rh,
		      uint32_t c)
{
	int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
	int x1 = x + rw > w ? w : x + rw, y1 = y + rh > h ? h : y + rh;

	for (int j = y0; j < y1; j++)
		for (int i = x0; i < x1; i++)
			buf[j * w + i] = c;
}

static void fill_circle(uint32_t *buf, int w, int h, int cx, int cy, int r,
			uint32_t c)
{
	for (int j = -r; j <= r; j++) {
		int y = cy + j;

		if (y < 0 || y >= h)
			continue;
		for (int i = -r; i <= r; i++) {
			int x = cx + i;

			if (x >= 0 && x < w && i * i + j * j <= r * r)
				buf[y * w + x] = c;
		}
	}
}

static void ring(uint32_t *buf, int w, int h, int cx, int cy, int r, int t,
		 uint32_t c)
{
	int ro = r * r, ri = (r - t) * (r - t);

	for (int j = -r; j <= r; j++) {
		int y = cy + j;

		if (y < 0 || y >= h)
			continue;
		for (int i = -r; i <= r; i++) {
			int x = cx + i, d = i * i + j * j;

			if (x >= 0 && x < w && d <= ro && d >= ri)
				buf[y * w + x] = c;
		}
	}
}

static void thick_line(uint32_t *buf, int w, int h, double x0, double y0,
		       double x1, double y1, int r, uint32_t c)
{
	double dx = x1 - x0, dy = y1 - y0;
	int steps = (int)(abs((int)dx) > abs((int)dy) ? abs((int)dx) : abs((int)dy));

	if (steps == 0)
		steps = 1;
	for (int s = 0; s <= steps; s++)
		fill_circle(buf, w, h, (int)(x0 + dx * s / steps),
			    (int)(y0 + dy * s / steps), r, c);
}

static int draw_text(uint32_t *buf, int w, int h, int x, int y, int scale,
		     uint32_t c, const char *s)
{
	for (; *s; s++, x += 6 * scale) {
		const uint8_t *g = glyph(*s);

		if (!g)
			continue;
		for (int row = 0; row < 7; row++)
			for (int col = 0; col < 5; col++)
				if (g[row] & (0x10 >> col))
					fill_rect(buf, w, h, x + col * scale,
						  y + row * scale, scale, scale, c);
	}
	return x;
}

static int text_width(const char *s, int scale)
{
	return (int)strlen(s) * 6 * scale - scale;
}

static void draw_button(uint32_t *buf, int w, int h, const struct rect *r,
			uint32_t bg, const char *label)
{
	fill_rect(buf, w, h, r->x, r->y, r->w, r->h, bg);
	draw_text(buf, w, h, r->x + (r->w - text_width(label, 3)) / 2,
		  r->y + (r->h - 21) / 2, 3, COL_TEXT, label);
}

/* -------------------------------------------------------------- layout */

static void layout(struct app *a)
{
	int m = 80, top = BAR_H + m;

	a->targets[0] = (struct target){ .x = m, .y = top };
	a->targets[1] = (struct target){ .x = a->width - m, .y = top };
	a->targets[2] = (struct target){ .x = a->width / 2, .y = (a->height + BAR_H) / 2 };
	a->targets[3] = (struct target){ .x = m, .y = a->height - m };
	a->targets[4] = (struct target){ .x = a->width - m, .y = a->height - m };

	a->btn_exit = (struct rect){ a->width - 140, 8, 128, BAR_H - 16 };
	a->btn_clear = (struct rect){ a->width - 290, 8, 140, BAR_H - 16 };
}

static void clear_canvas(struct app *a)
{
	for (int j = 0; j < a->height; j++)
		for (int i = 0; i < a->width; i++)
			a->canvas[j * a->width + i] =
				(i % 80 == 0 || j % 80 == 0) ? COL_GRID : COL_BG;
	for (int t = 0; t < NUM_TARGETS; t++)
		a->targets[t].hit = false;
	a->total_downs = 0;
}

static bool in_rect(const struct rect *r, double x, double y)
{
	return x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h;
}

/* -------------------------------------------------------------- buffers */

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
	fd = memfd_create("touch-demo", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, b->size) < 0) {
		perror("touch-demo: shm");
		if (fd >= 0)
			close(fd);
		return -1;
	}
	b->data = mmap(NULL, b->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (b->data == MAP_FAILED) {
		perror("touch-demo: mmap");
		close(fd);
		b->data = NULL;
		return -1;
	}
	pool = wl_shm_create_pool(a->shm, fd, b->size);
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

/* -------------------------------------------------------------- render */

static void render(struct app *a, uint32_t *buf)
{
	int w = a->width, h = a->height, active = 0, hits = 0;
	char s[64];

	memcpy(buf, a->canvas, (size_t)w * h * 4);

	for (int t = 0; t < NUM_TARGETS; t++) {
		struct target *tg = &a->targets[t];
		uint32_t c = tg->hit ? COL_HIT : COL_TARGET;

		if (tg->hit) {
			hits++;
			fill_circle(buf, w, h, tg->x, tg->y, TARGET_R, COL_HIT);
			snprintf(s, sizeof(s), "DX:%d DY:%d", tg->dx, tg->dy);
			draw_text(buf, w, h,
				  tg->x < w / 2 ? tg->x - TARGET_R : tg->x + TARGET_R - text_width(s, 2),
				  tg->y + TARGET_R + 10, 2, COL_HIT, s);
		}
		ring(buf, w, h, tg->x, tg->y, TARGET_R, 3, c);
		fill_rect(buf, w, h, tg->x - TARGET_R - 12, tg->y - 1, 2 * TARGET_R + 24, 3, c);
		fill_rect(buf, w, h, tg->x - 1, tg->y - TARGET_R - 12, 3, 2 * TARGET_R + 24, c);
	}

	for (int i = 0; i < MAX_TOUCH; i++) {
		struct touch_point *tp = &a->touches[i];
		int x = (int)tp->x, y = (int)tp->y, lx;

		if (!tp->active)
			continue;
		active++;
		fill_rect(buf, w, h, 0, y, w, 1, tp->color);
		fill_rect(buf, w, h, x, BAR_H, 1, h - BAR_H, tp->color);
		ring(buf, w, h, x, y, 40, 4, tp->color);
		fill_circle(buf, w, h, x, y, 8, tp->color);
		snprintf(s, sizeof(s), "ID%d X:%d Y:%d", tp->id, x, y);
		lx = x + 48 + text_width(s, 2) < w ? x + 48 : x - 48 - text_width(s, 2);
		draw_text(buf, w, h, lx, y - 52 < BAR_H ? y + 48 : y - 52, 2,
			  tp->color, s);
	}

	fill_rect(buf, w, h, 0, 0, w, BAR_H, COL_BAR);
	draw_text(buf, w, h, 16, 12, 3, COL_TEXT, "BSD101 TOUCH TEST");
	snprintf(s, sizeof(s), "TOUCH DEV:%s  ACTIVE:%d  TARGETS:%d/%d  TAPS:%u",
		 a->touch ? "YES" : "NO", active, hits, NUM_TARGETS, a->total_downs);
	draw_text(buf, w, h, 16, 44, 2, a->touch ? COL_DIM : 0xffff5050, s);
	draw_button(buf, w, h, &a->btn_clear, COL_BTN, "CLEAR");
	draw_button(buf, w, h, &a->btn_exit, COL_EXIT, "EXIT");

	if (!a->touch) {
		const char *msg = "NO TOUCH DEVICE IN THIS WAYLAND SEAT";

		draw_text(buf, w, h, (w - text_width(msg, 3)) / 2, h / 2 + 60, 3,
			  0xffff5050, msg);
	}
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

/* --------------------------------------------------------------- touch */

static struct touch_point *find_touch(struct app *a, int32_t id, bool create)
{
	for (int i = 0; i < MAX_TOUCH; i++)
		if (a->touches[i].active && a->touches[i].id == id)
			return &a->touches[i];
	if (!create)
		return NULL;
	for (int i = 0; i < MAX_TOUCH; i++) {
		if (!a->touches[i].active) {
			a->touches[i] = (struct touch_point){
				.active = true, .id = id,
				.color = touch_colors[i],
			};
			return &a->touches[i];
		}
	}
	return NULL;
}

static void touch_down(void *data, struct wl_touch *t, uint32_t serial,
		       uint32_t time, struct wl_surface *surface, int32_t id,
		       wl_fixed_t fx, wl_fixed_t fy)
{
	struct app *a = data;
	double x = wl_fixed_to_double(fx), y = wl_fixed_to_double(fy);
	struct touch_point *tp;

	(void)t; (void)serial; (void)time; (void)surface;
	if (in_rect(&a->btn_exit, x, y)) {
		a->running = false;
		return;
	}
	if (in_rect(&a->btn_clear, x, y)) {
		clear_canvas(a);
		return;
	}

	a->total_downs++;
	for (int i = 0; i < NUM_TARGETS; i++) {
		struct target *tg = &a->targets[i];
		int dx = (int)x - tg->x, dy = (int)y - tg->y;

		if (dx * dx + dy * dy <= HIT_R * HIT_R) {
			tg->hit = true;
			tg->dx = dx;
			tg->dy = dy;
		}
	}

	tp = find_touch(a, id, true);
	if (!tp)
		return;
	tp->x = x;
	tp->y = y;
	fill_circle(a->canvas, a->width, a->height, (int)x, (int)y, TRAIL_R,
		    tp->color);
	printf("down   id=%d x=%.1f y=%.1f\n", id, x, y);
}

static void touch_up(void *data, struct wl_touch *t, uint32_t serial,
		     uint32_t time, int32_t id)
{
	struct touch_point *tp = find_touch(data, id, false);

	(void)t; (void)serial; (void)time;
	if (tp) {
		printf("up     id=%d x=%.1f y=%.1f\n", id, tp->x, tp->y);
		tp->active = false;
	}
}

static void touch_motion(void *data, struct wl_touch *t, uint32_t time,
			 int32_t id, wl_fixed_t fx, wl_fixed_t fy)
{
	struct app *a = data;
	struct touch_point *tp = find_touch(a, id, false);
	double x = wl_fixed_to_double(fx), y = wl_fixed_to_double(fy);

	(void)t; (void)time;
	if (!tp)
		return;
	thick_line(a->canvas, a->width, a->height, tp->x, tp->y, x, y, TRAIL_R,
		   tp->color);
	tp->x = x;
	tp->y = y;
}

static void touch_frame(void *data, struct wl_touch *t)
{
	(void)t;
	redraw(data);
}

static void touch_cancel(void *data, struct wl_touch *t)
{
	struct app *a = data;

	(void)t;
	for (int i = 0; i < MAX_TOUCH; i++)
		a->touches[i].active = false;
	redraw(a);
}

static void touch_shape(void *data, struct wl_touch *t, int32_t id,
			wl_fixed_t major, wl_fixed_t minor)
{
	(void)data; (void)t; (void)id; (void)major; (void)minor;
}

static void touch_orientation(void *data, struct wl_touch *t, int32_t id,
			      wl_fixed_t orientation)
{
	(void)data; (void)t; (void)id; (void)orientation;
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

/* ---------------------------------------------------------------- seat */

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps)
{
	struct app *a = data;
	bool has_touch = caps & WL_SEAT_CAPABILITY_TOUCH;

	if (has_touch && !a->touch) {
		a->touch = wl_seat_get_touch(seat);
		wl_touch_add_listener(a->touch, &touch_listener, a);
		fprintf(stderr, "touch-demo: seat has a touch device\n");
	} else if (!has_touch && a->touch) {
		wl_touch_release(a->touch);
		a->touch = NULL;
		fprintf(stderr, "touch-demo: touch device removed from seat\n");
	}
	if (!has_touch)
		fprintf(stderr, "touch-demo: seat has NO touch device\n");
	redraw(a);
}

static void seat_name(void *data, struct wl_seat *seat, const char *name)
{
	(void)data; (void)seat; (void)name;
}

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_capabilities,
	.name = seat_name,
};

/* ----------------------------------------------------------- xdg-shell */

static void wm_base_ping(void *data, struct xdg_wm_base *wm_base,
			 uint32_t serial)
{
	(void)data;
	xdg_wm_base_pong(wm_base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
	.ping = wm_base_ping,
};

static void resize(struct app *a, int w, int h)
{
	if (w == a->width && h == a->height && a->canvas)
		return;
	for (int i = 0; i < 2; i++)
		destroy_buffer(&a->buffers[i]);
	free(a->canvas);
	a->width = w;
	a->height = h;
	a->canvas = malloc((size_t)w * h * 4);
	if (!a->canvas) {
		fprintf(stderr, "touch-demo: out of memory\n");
		exit(1);
	}
	layout(a);
	clear_canvas(a);
}

static void xdg_surface_configure(void *data, struct xdg_surface *s,
				  uint32_t serial)
{
	struct app *a = data;

	xdg_surface_ack_configure(s, serial);
	resize(a, a->pending_w > 0 ? a->pending_w : 1280,
	       a->pending_h > 0 ? a->pending_h : 800);
	a->configured = true;
	redraw(a);
}

static const struct xdg_surface_listener xdg_surface_listener = {
	.configure = xdg_surface_configure,
};

static void toplevel_configure(void *data, struct xdg_toplevel *tl,
			       int32_t w, int32_t h, struct wl_array *states)
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

static void toplevel_configure_bounds(void *data, struct xdg_toplevel *tl,
				      int32_t w, int32_t h)
{
	(void)data; (void)tl; (void)w; (void)h;
}

static void toplevel_wm_capabilities(void *data, struct xdg_toplevel *tl,
				     struct wl_array *caps)
{
	(void)data; (void)tl; (void)caps;
}

static const struct xdg_toplevel_listener toplevel_listener = {
	.configure = toplevel_configure,
	.close = toplevel_close,
	.configure_bounds = toplevel_configure_bounds,
	.wm_capabilities = toplevel_wm_capabilities,
};

/* ------------------------------------------------------------ registry */

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
		a->seat = wl_registry_bind(reg, name, &wl_seat_interface,
					   min_u32(version, 5));
		wl_seat_add_listener(a->seat, &seat_listener, a);
	}
}

static void registry_global_remove(void *data, struct wl_registry *reg,
				   uint32_t name)
{
	(void)data; (void)reg; (void)name;
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

/* ---------------------------------------------------------------- main */

int main(void)
{
	struct app a = { .running = true };
	struct wl_registry *registry;

	setvbuf(stdout, NULL, _IOLBF, 0);

	a.display = wl_display_connect(NULL);
	if (!a.display) {
		fprintf(stderr, "touch-demo: cannot connect to a Wayland display "
			"(is WAYLAND_DISPLAY / XDG_RUNTIME_DIR set?)\n");
		return 1;
	}
	registry = wl_display_get_registry(a.display);
	wl_registry_add_listener(registry, &registry_listener, &a);
	wl_display_roundtrip(a.display);
	if (!a.compositor || !a.shm || !a.wm_base) {
		fprintf(stderr, "touch-demo: compositor lacks wl_compositor, "
			"wl_shm or xdg_wm_base\n");
		return 1;
	}

	a.surface = wl_compositor_create_surface(a.compositor);
	a.xdg_surface = xdg_wm_base_get_xdg_surface(a.wm_base, a.surface);
	xdg_surface_add_listener(a.xdg_surface, &xdg_surface_listener, &a);
	a.toplevel = xdg_surface_get_toplevel(a.xdg_surface);
	xdg_toplevel_add_listener(a.toplevel, &toplevel_listener, &a);
	xdg_toplevel_set_title(a.toplevel, "BSD101 touch test");
	xdg_toplevel_set_app_id(a.toplevel, "touch-demo");
	xdg_toplevel_set_fullscreen(a.toplevel, NULL);
	wl_surface_commit(a.surface);

	while (a.running && wl_display_dispatch(a.display) != -1)
		;

	if (a.touch)
		wl_touch_release(a.touch);
	for (int i = 0; i < 2; i++)
		destroy_buffer(&a.buffers[i]);
	free(a.canvas);
	xdg_toplevel_destroy(a.toplevel);
	xdg_surface_destroy(a.xdg_surface);
	wl_surface_destroy(a.surface);
	wl_display_disconnect(a.display);
	return 0;
}
