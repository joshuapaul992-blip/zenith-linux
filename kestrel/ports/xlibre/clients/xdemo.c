/* =============================================================================
 *  xdemo.c -- first X11 client for Kestrel
 *
 *  Connects to $DISPLAY (default :0) over the AF_UNIX socket, creates a
 *  window, draws a few filled rectangles and a line of text with the server's
 *  built-in "fixed" font, and logs every event it receives. With -t N it exits
 *  after N seconds (for automated runs); otherwise it runs until the window
 *  is destroyed or the server goes away.
 *
 *  Uses only libxcb, linked statically against the musl sysroot built by
 *  ports/xlibre/build-sysroot.sh.
 * ============================================================================= */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <time.h>
#include <xcb/xcb.h>

static uint32_t rgb(const xcb_screen_t *s, unsigned r, unsigned g, unsigned b)
{
    (void)s;                    /* the Kestrel server only offers 24-bit TrueColor */
    return (r << 16) | (g << 8) | b;
}

static void draw(xcb_connection_t *c, xcb_window_t win, xcb_gcontext_t gc,
                 const xcb_screen_t *s, int frame)
{
    static const unsigned colours[][3] = {
        { 0xd0, 0x30, 0x30 }, { 0x30, 0xb0, 0x40 }, { 0x30, 0x60, 0xd0 }, { 0xe0, 0xc0, 0x20 },
    };
    uint32_t v;

    v = rgb(s, 0x20, 0x24, 0x30);
    xcb_change_gc(c, gc, XCB_GC_FOREGROUND, &v);
    xcb_rectangle_t bg = { 0, 0, 480, 300 };
    xcb_poly_fill_rectangle(c, win, gc, 1, &bg);

    for (int i = 0; i < 4; i++) {
        const unsigned *col = colours[(i + frame) % 4];
        v = rgb(s, col[0], col[1], col[2]);
        xcb_change_gc(c, gc, XCB_GC_FOREGROUND, &v);
        xcb_rectangle_t r = { (int16_t)(30 + i * 110), 40, 90, 150 };
        xcb_poly_fill_rectangle(c, win, gc, 1, &r);
    }

    v = rgb(s, 0xff, 0xff, 0xff);
    xcb_change_gc(c, gc, XCB_GC_FOREGROUND, &v);
    xcb_point_t line[] = { { 30, 220 }, { 450, 220 } };
    xcb_poly_line(c, XCB_COORD_MODE_ORIGIN, win, gc, 2, line);

    const char *msg = "Hello from X11 on Kestrel";
    xcb_image_text_8(c, (uint8_t)strlen(msg), win, gc, 30, 250, msg);

    char buf[64];
    snprintf(buf, sizeof buf, "frame %d", frame);
    xcb_image_text_8(c, (uint8_t)strlen(buf), win, gc, 30, 270, buf);
    xcb_flush(c);
}

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

int main(int argc, char **argv)
{
    int timeout_s = 0;
    setvbuf(stdout, NULL, _IOLBF, 0);     /* the log is not a terminal */
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "-t") && i + 1 < argc)
            timeout_s = atoi(argv[++i]);

    int screen_no;
    xcb_connection_t *c = NULL;
    /* The server may still be starting: retry for a few seconds. */
    for (int tries = 0; tries < 50; tries++) {
        c = xcb_connect(getenv("DISPLAY") ? NULL : ":0", &screen_no);
        if (!xcb_connection_has_error(c))
            break;
        xcb_disconnect(c);
        c = NULL;
        struct timespec ts = { 0, 200 * 1000000L };
        nanosleep(&ts, NULL);
    }
    if (!c) {
        printf("xdemo: cannot connect to the X server\n");
        return 1;
    }

    const xcb_setup_t *setup = xcb_get_setup(c);
    printf("xdemo: connected, vendor \"%.*s\", release %u, %d screen(s)\n",
           xcb_setup_vendor_length(setup), xcb_setup_vendor(setup),
           (unsigned)setup->release_number, xcb_setup_roots_length(setup));

    xcb_screen_iterator_t it = xcb_setup_roots_iterator(setup);
    for (int i = 0; i < screen_no && it.rem; i++)
        xcb_screen_next(&it);
    xcb_screen_t *s = it.data;
    printf("xdemo: screen %d is %ux%u, depth %u, root 0x%x\n", screen_no,
           s->width_in_pixels, s->height_in_pixels, s->root_depth, s->root);

    xcb_window_t win = xcb_generate_id(c);
    uint32_t wvals[] = {
        rgb(s, 0x20, 0x24, 0x30),
        XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_BUTTON_PRESS |
            XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_STRUCTURE_NOTIFY,
    };
    xcb_create_window(c, XCB_COPY_FROM_PARENT, win, s->root,
                      (int16_t)((s->width_in_pixels - 480) / 2),
                      (int16_t)((s->height_in_pixels - 300) / 2), 480, 300, 2,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, s->root_visual,
                      XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, wvals);
    const char *title = "xdemo";
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, win, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8,
                        (uint32_t)strlen(title), title);

    xcb_font_t font = xcb_generate_id(c);
    xcb_void_cookie_t fck = xcb_open_font_checked(c, font, 5, "fixed");
    xcb_generic_error_t *err = xcb_request_check(c, fck);
    if (err) {
        printf("xdemo: OpenFont(fixed) failed, error %u\n", err->error_code);
        free(err);
        font = XCB_NONE;
    }

    xcb_gcontext_t gc = xcb_generate_id(c);
    uint32_t gvals[] = { s->white_pixel, rgb(s, 0x20, 0x24, 0x30), font };
    xcb_create_gc(c, gc, win, XCB_GC_FOREGROUND | XCB_GC_BACKGROUND | (font ? XCB_GC_FONT : 0),
                  gvals);
    /* The server keeps the pointer hidden until a client defines a cursor
     * (normally the window manager): use the cursor font's left_ptr. */
    xcb_font_t cfont = xcb_generate_id(c);
    xcb_open_font(c, cfont, 6, "cursor");
    xcb_cursor_t cursor = xcb_generate_id(c);
    xcb_create_glyph_cursor(c, cursor, cfont, cfont, 68, 69, 0, 0, 0, 0xffff, 0xffff, 0xffff);
    xcb_change_window_attributes(c, s->root, XCB_CW_CURSOR, &cursor);

    xcb_map_window(c, win);
    xcb_flush(c);

    long deadline = timeout_s ? now_ms() + timeout_s * 1000L : 0;
    long next_frame = now_ms() + 1000;
    int frame = 0, exposed = 0;
    struct pollfd pfd = { xcb_get_file_descriptor(c), POLLIN, 0 };

    for (;;) {
        xcb_generic_event_t *ev;
        while ((ev = xcb_poll_for_event(c))) {
            switch (ev->response_type & 0x7f) {
            case XCB_EXPOSE:
                if (((xcb_expose_event_t *)ev)->count == 0) {
                    if (!exposed)
                        printf("xdemo: window mapped and exposed, drawing\n");
                    exposed = 1;
                    draw(c, win, gc, s, frame);
                }
                break;
            case XCB_KEY_PRESS: {
                xcb_key_press_event_t *k = (xcb_key_press_event_t *)ev;
                printf("xdemo: key press, keycode %u state 0x%x\n", k->detail, k->state);
                break;
            }
            case XCB_BUTTON_PRESS: {
                xcb_button_press_event_t *b = (xcb_button_press_event_t *)ev;
                printf("xdemo: button %u at %d,%d\n", b->detail, b->event_x, b->event_y);
                break;
            }
            case XCB_MOTION_NOTIFY:
                break;
            case XCB_DESTROY_NOTIFY:
                free(ev);
                goto out;
            case 0: {
                xcb_generic_error_t *e = (xcb_generic_error_t *)ev;
                printf("xdemo: X error %u (major %u)\n", e->error_code, e->major_code);
                break;
            }
            }
            free(ev);
        }
        if (xcb_connection_has_error(c)) {
            printf("xdemo: connection closed by server\n");
            break;
        }
        long t = now_ms();
        if (deadline && t >= deadline)
            break;
        if (exposed && t >= next_frame) {
            draw(c, win, gc, s, ++frame);
            next_frame = t + 1000;
        }
        poll(&pfd, 1, 100);
    }
out:
    printf("xdemo: done after %d frame(s)\n", frame);
    xcb_disconnect(c);
    return 0;
}
