/* See LICENSE file for copyright and license details.
 *
 * uuwm is designed like any other X client as well. It is driven through
 * handling X events. In contrast to other X clients, a window manager selects
 * for SubstructureRedirectMask on the root window, to receive events about
 * window (dis-)appearance.  Only one X connection at a time is allowed to
 * select for this event mask.
 *
 * Each child of the root window is called a client, except windows which have
 * set the override_redirect flag.  Clients are organized in a global
 * linked client list, the focus history is remembered through a global
 * stack list.
 *
 * To understand everything else, start reading main().
 */

/*
 * TODO:
 * - NetWM support for docks
 */

#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <xcb/xcb.h>
#include <xcb/xcb_aux.h>
#include <xcb/xcb_atom.h>
#include <xcb/xcb_icccm.h>
#include <xcb/xcb_event.h>

/* macros */
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))

typedef struct client_t client_t;
struct client_t
{
	float mina, maxa;
	int x, y, w, h;
	int basew, baseh, incw, inch, maxw, maxh, minw, minh;
	int oldbw;
    bool isfixed, isfloating, isurgent, ispanel;
	client_t *next;
	client_t *snext;
    xcb_window_t win;
};

/* variables */
static int sx, sy, sw, sh; /* X display screen geometry x, y, w, h */
static int wx, wy, ww, wh; /* window area geometry x, y, w, h, docks excluded */

enum { WMProtocols, WMDelete, WMState, NetSupported, NetWMName,
       AtomLast,
       NetFirst=NetSupported,
       NetLast=AtomLast };

static xcb_atom_t atom[AtomLast];
static const char* atom_names[AtomLast] = {
    "WM_PROTOCOLS",
    "WM_DELETE_WINDOW",
    "WM_STATE",
    "_NET_SUPPORTED",
    "_NET_WM_NAME"
};

static bool stop_wm = false;
static client_t *clients = NULL;
static client_t *sel = NULL;
static client_t *stack = NULL;
//static Cursor cursor;

static xcb_connection_t* conn;
static int default_screen;
static xcb_screen_t* screen;

static void (*do_arrange)();

/* Omission from xcb-aux */
static void pack_list(uint32_t mask, const uint32_t *src, uint32_t *dest)
{
	for ( ; mask; mask >>= 1, src++)
		if (mask & 1)
			*dest++ = *src;
}

static xcb_void_cookie_t
xcb_aux_change_window_attributes_checked (xcb_connection_t      *c,
                                          xcb_window_t           window,
                                          uint32_t               mask,
                                          const xcb_params_cw_t *params)
{
	uint32_t value_list[16];
	pack_list(mask, (const uint32_t *)params, value_list);
	return xcb_change_window_attributes_checked( c, window, mask, value_list );
}

/* Code */

static void die(const char* errstr, ...)
{
	va_list ap;
	va_start(ap, errstr);
	vfprintf(stderr, errstr, ap);
	va_end(ap);
	exit(EXIT_FAILURE);
}

static void checkotherwm()
{
    uint32_t mask = 0;
    xcb_params_cw_t params;
    XCB_AUX_ADD_PARAM(&mask, &params, event_mask,
                      XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT);

    xcb_void_cookie_t c
        = xcb_aux_change_window_attributes_checked(conn, screen->root,
                                                   mask, &params);

    xcb_generic_error_t* err = xcb_request_check(conn, c);
    if(err)
		die("another window manager is already running\n");
}

static void intern_atoms(int count, xcb_atom_t atoms[], const char* atom_names[])
{
    xcb_intern_atom_cookie_t* c=malloc(sizeof(xcb_intern_atom_cookie_t)*count);

    int i;
    for(i = 0; i < count; ++i)
        c[i]=xcb_intern_atom(conn, false, strlen(atom_names[i]), atom_names[i]);

    for(i = 0; i < count; ++i)
    {
        xcb_generic_error_t* err;
        xcb_intern_atom_reply_t* r
            = xcb_intern_atom_reply(conn, c[i], &err);
        if(!r)
            die("Unable to intern atom %s\n", atom_names[i]);
        atoms[i] = r->atom;
        free(r);
    }

    free(c);
}

static client_t* nexttiled(client_t* c)
{
    while(c && c->isfloating)
        c = c->next;
	return c;
}

static bool applysizehints(client_t* c, int* x, int* y, int* w, int* h)
{
	bool baseismin;

	/* set minimum possible */
	*w = MAX(1, *w);
	*h = MAX(1, *h);

	if(*x > sx + sw)
		*x = sw - c->w;
	if(*y > sy + sh)
		*y = sh - c->h;
	if(*x + *w < sx)
		*x = sx;
	if(*y + *h < sy)
		*y = sy;

	if(c->isfloating) {
		/* see last two sentences in ICCCM 4.1.2.3 */
		baseismin = c->basew == c->minw && c->baseh == c->minh;

		if(!baseismin) { /* temporarily remove base dimensions */
			*w -= c->basew;
			*h -= c->baseh;
		}

		/* adjust for aspect limits */
		if(c->mina > 0 && c->maxa > 0) {
			if(c->maxa < (float)*w / *h)
				*w = *h * c->maxa;
			else if(c->mina < (float)*h / *w)
				*h = *w * c->mina;
		}

		if(baseismin) { /* increment calculation requires this */
			*w -= c->basew;
			*h -= c->baseh;
		}

		/* adjust for increment value */
		if(c->incw)
			*w -= *w % c->incw;
		if(c->inch)
			*h -= *h % c->inch;

		/* restore base dimensions */
		*w += c->basew;
		*h += c->baseh;

		*w = MAX(*w, c->minw);
		*h = MAX(*h, c->minh);

		if(c->maxw)
			*w = MIN(*w, c->maxw);

		if(c->maxh)
			*h = MIN(*h, c->maxh);
	}
	return *x != c->x || *y != c->y || *w != c->w || *h != c->h;
}

static void configure(xcb_window_t win, uint16_t mask,
                      xcb_params_configure_window_t* params)
{
    xcb_void_cookie_t c
        = xcb_aux_configure_window(conn, win, mask, params);

    xcb_generic_error_t* err = xcb_request_check(conn, c);
    if(err)
    {
        printf("Warning: unable to configure\n");
        if(err->error_code != XCB_WINDOW)
            die("Unable to configure window %x (%d)\n", win, err->error_code);
        /* BadWindow is ignored as windows may disappear at any time */
        free(err);
    }
}

static void configure_event(client_t *c)
{
    xcb_configure_notify_event_t e;

    e.response_type = XCB_CONFIGURE_NOTIFY;
    e.event = c->win;
    e.window = c->win;
    e.x = c->x;
    e.y = c->y;
    e.width = c->w;
    e.height = c->h;
    e.border_width = 0;
    e.above_sibling = XCB_NONE;
    e.override_redirect = false;

    xcb_void_cookie_t cookie
        = xcb_send_event(conn, false, c->win,
                         XCB_EVENT_MASK_STRUCTURE_NOTIFY, (const char*)&e);
    xcb_generic_error_t* err = xcb_request_check(conn, cookie);
    if(err)
    {
        die("Unable to send configure event to %x (%d)\n",
            c->win, err->error_code);
    }
}

static void set_focus(uint8_t revert_to, xcb_window_t focus)
{
    xcb_void_cookie_t c
        = xcb_set_input_focus_checked(conn, revert_to, focus, XCB_CURRENT_TIME);
    xcb_generic_error_t* err = xcb_request_check(conn, c);
    if(err)
    {
        if(err->error_code != XCB_WINDOW)
            die("Unable to set input focus (%d) on %x (%d)\n",
                revert_to, focus, err->error_code);
        /* BadWindow is ignored, as windows may disappear at any time */
        free(err);
    }
}

static void resize(client_t *c, int x, int y, int w, int h)
{
	if(applysizehints(c, &x, &y, &w, &h))
    {
		c->x = x;
		c->y = y;
		c->w = w;
		c->h = h;

        uint16_t mask = 0;
        xcb_params_configure_window_t params;
        XCB_AUX_ADD_PARAM(&mask, &params, x, c->x);
        XCB_AUX_ADD_PARAM(&mask, &params, y, c->y);
        XCB_AUX_ADD_PARAM(&mask, &params, width, c->w);
        XCB_AUX_ADD_PARAM(&mask, &params, height, c->h);
        XCB_AUX_ADD_PARAM(&mask, &params, border_width, 0);

        configure(c->win, mask, &params);
		configure_event(c);
	}
}

static void monocle()
{
	client_t *c;

	for(c = nexttiled(clients); c; c = nexttiled(c->next)) {
		resize(c, wx, wy, ww, wh);
	}
}

static void updategeom()
{
    wx = sx;
    wy = sy;
    ww = sw;
    wh = sh;
}

static void setup()
{
	/* init screen */
	sx = 0;
	sy = 0;
	sw = screen->width_in_pixels;
	sh = screen->height_in_pixels;

    do_arrange = &monocle;

	updategeom();

    intern_atoms(sizeof(atom)/sizeof(atom[0]), atom, atom_names);

    /* FIXME
	wa.cursor = cursor = XCreateFontCursor(dpy, XC_watch);
    XCB_AUX_ADD_PARAM(&masp, &params, cursor, ...)
    */

    /* expose NetWM support */
    xcb_void_cookie_t c
        = xcb_change_property_checked(conn, XCB_PROP_MODE_REPLACE,
                                      screen->root, atom[NetSupported],
                                      ATOM, 32, NetLast - NetFirst,
                                      atom + NetFirst);

    if(xcb_request_check(conn, c))
		die("Unable to register myself as NetWM-compliant WM.\n");

	/* select for events */
    uint32_t mask = 0;
    xcb_params_cw_t params;
    XCB_AUX_ADD_PARAM(&mask, &params, event_mask,
                      XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT |
                      XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY |
                      XCB_EVENT_MASK_ENTER_WINDOW |
                      XCB_EVENT_MASK_LEAVE_WINDOW |
                      XCB_EVENT_MASK_STRUCTURE_NOTIFY |
                      XCB_EVENT_MASK_PROPERTY_CHANGE);

    xcb_void_cookie_t c2
        = xcb_aux_change_window_attributes_checked(conn, screen->root,
                                                   mask, (const void*)&params);

    xcb_generic_error_t* e = xcb_request_check(conn, c2);
    if(e)
        die("Unable to register event listener for root window: %d.\n", e->error_code);
}

static void attach(client_t *c)
{
	c->next = clients;
	clients = c;
}

static void attachstack(client_t *c)
{
	c->snext = stack;
	stack = c;
}

static void detach(client_t *c)
{
	client_t **tc = &clients;

    while(*tc && *tc != c)
        tc = &(*tc)->next;

    *tc = c->next;
}

static void detachstack(client_t *c)
{
	client_t **tc = &stack;

    while(*tc && *tc != c)
        tc = &(*tc)->snext;

    *tc = c->snext;
}

static void clearurgent(client_t *c)
{
	c->isurgent = false;

    xcb_get_property_cookie_t cookie = xcb_get_wm_hints(conn, c->win);
    xcb_get_property_reply_t* hints_reply
        = xcb_get_property_reply(conn, cookie, NULL);

    xcb_wm_hints_t hints;
    if(xcb_get_wm_hints_from_reply(&hints, hints_reply))
    {
        hints.flags &= ~XCB_WM_HINT_X_URGENCY;
        xcb_set_wm_hints(conn, c->win, &hints);
    }

    free(hints_reply);
}

static void focus(client_t *c)
{
    xcb_window_t win;

	if(!c)
        c = stack;
	if(c)
    {
		if(c->isurgent)
			clearurgent(c);
		detachstack(c);
		attachstack(c);

        win = c->win;
	}
	else
        win = screen->root;

    set_focus(XCB_INPUT_FOCUS_POINTER_ROOT, win);

	sel = c;
}

static void setclientstate(client_t *c, long state, bool ignore_no_window)
{
	long data[] = {state, XCB_NONE};

    xcb_void_cookie_t cookie =
        xcb_change_property_checked(
            conn, XCB_PROP_MODE_REPLACE, c->win, atom[WMState],
            atom[WMState], 32, 2, (const void*)data);

    xcb_generic_error_t* err = xcb_request_check(conn, cookie);
    if(err)
    {
        if(ignore_no_window && err->error_code == XCB_WINDOW)
        {
            free(err);
            return;
        }

        die("Unable to set client state.\n");
    }
}

static void showhide(client_t *c)
{
	if(!c)
		return;

    uint16_t mask = 0;
    xcb_params_configure_window_t params;
    XCB_AUX_ADD_PARAM(&mask, &params, x, c->x);
    XCB_AUX_ADD_PARAM(&mask, &params, y, c->y);

    configure(c->win, mask, &params);

    if(c->isfloating)
        resize(c, c->x, c->y, c->w, c->h);
    showhide(c->snext);
}

static void restack()
{
	client_t *c;

	if(!sel)
		return;
	if(sel->isfloating)
    {
        uint16_t mask = 0;
        xcb_params_configure_window_t params;
        XCB_AUX_ADD_PARAM(&mask, &params, stack_mode, XCB_STACK_MODE_ABOVE);

        configure(sel->win, mask, &params);
    }

    uint16_t mask = 0;
    xcb_params_configure_window_t params;

    XCB_AUX_ADD_PARAM(&mask, &params, stack_mode, XCB_STACK_MODE_BELOW);
    XCB_AUX_ADD_PARAM(&mask, &params, sibling, XCB_NONE);

    for(c = stack; c; c = c->snext)
        if(!c->isfloating)
        {
            configure(c->win, mask, &params);
            XCB_AUX_ADD_PARAM(&mask, &params, sibling, c->win);
        }

	//while(XCheckMaskEvent(dpy, EnterWindowMask, &ev));
}

static void arrange()
{
	showhide(stack);
	focus(NULL);
    (*do_arrange)();
	restack();
}

static void unmanage(client_t *c)
{
    xcb_request_check(conn, xcb_grab_server_checked(conn));

    uint16_t mask = 0;
    xcb_params_configure_window_t params;
    XCB_AUX_ADD_PARAM(&mask, &params, border_width, c->oldbw);
    configure(c->win, mask, &params);

	detach(c);
	detachstack(c);
	if(sel == c)
		focus(NULL);
	setclientstate(c, XCB_WM_STATE_WITHDRAWN, true);
	free(c);

    xcb_request_check(conn, xcb_ungrab_server(conn));

	arrange();
}

static void nothing()
{
}

static void cleanup()
{
	do_arrange = &nothing;
	while(stack)
		unmanage(stack);
    /* FIXME */
	//XFreeCursor(dpy, cursor);

    set_focus(XCB_INPUT_FOCUS_POINTER_ROOT, XCB_INPUT_FOCUS_POINTER_ROOT);
}

static client_t *getclient(xcb_window_t w)
{
	client_t *c = clients;

    while(c && c->win != w)
        c = c->next;

	return c;
}

static int configurerequest(void* p, xcb_connection_t* conn, xcb_configure_request_event_t* e)
{
	client_t *c = getclient(e->window);

	if(c)
    {
		if(e->value_mask & XCB_CONFIG_WINDOW_BORDER_WIDTH) {
			if(e->border_width)
            {
                uint16_t mask = 0;
                xcb_params_configure_window_t params;
                XCB_AUX_ADD_PARAM(&mask, &params, border_width, 0);
                configure(c->win, mask, &params);
            }
		} else if(c->isfloating) {
			if(e->value_mask & XCB_CONFIG_WINDOW_X)
				c->x = sx + e->x;
			if(e->value_mask & XCB_CONFIG_WINDOW_Y)
				c->y = sy + e->y;
			if(e->value_mask & XCB_CONFIG_WINDOW_WIDTH)
				c->w = e->width;
			if(e->value_mask & XCB_CONFIG_WINDOW_HEIGHT)
				c->h = e->height;
			if((c->x - sx + c->w) > sw)
				c->x = sx + (sw / 2 - c->w / 2); /* center in x direction */
			if((c->y - sy + c->h) > sh)
				c->y = sy + (sh / 2 - c->h / 2); /* center in y direction */
            if((e->value_mask & (XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y))
               & !(e->value_mask & (XCB_CONFIG_WINDOW_WIDTH
                                    | XCB_CONFIG_WINDOW_HEIGHT)))
                configure_event(c);

            uint16_t mask = 0;
            xcb_params_configure_window_t params;
            XCB_AUX_ADD_PARAM(&mask, &params, x, c->x);
            XCB_AUX_ADD_PARAM(&mask, &params, y, c->y);
            XCB_AUX_ADD_PARAM(&mask, &params, width, c->w);
            XCB_AUX_ADD_PARAM(&mask, &params, height, c->h);

            configure(c->win, mask, &params);
		}
		else
        {
			configure_event(c);
        }
	}
	else
    {
        /* Not our business, just pass it through */

        /* Note: e->value_mask is passed as is to request */
        xcb_params_configure_window_t params;
        params.x = e->x;
        params.y = e->y;
        params.width = e->width;
        params.height = e->height;
        params.border_width = e->border_width;
        params.sibling = e->sibling;
        params.stack_mode = e->stack_mode;

        configure(e->window, e->value_mask, &params);
	}
    return 0;
}

static int destroynotify(void* p, xcb_connection_t* conn, xcb_destroy_notify_event_t* e)
{
	client_t *c = getclient(e->window);
    if(c)
        unmanage(c);
    return 0;
}

static int enternotify(void* p, xcb_connection_t* conn, xcb_enter_notify_event_t* e)
{
    if((e->mode != XCB_NOTIFY_MODE_NORMAL
        || e->detail == XCB_NOTIFY_DETAIL_INFERIOR) && e->event != screen->root)
        return 0;

	client_t* c = getclient(e->event);
	if(c)
		focus(c);
	else
		focus(NULL);
    return 0;
}

static int focusin(void* p, xcb_connection_t* conn, xcb_focus_in_event_t* e)
{
    /* there are some broken focus acquiring clients */
    if(sel && e->event != sel->win)
        set_focus(XCB_INPUT_FOCUS_POINTER_ROOT, sel->win);
    return 0;
}

static void updatesizehints(client_t *c)
{
    xcb_size_hints_t hints;

    if(!xcb_get_wm_normal_hints_reply(
           conn, xcb_get_wm_normal_hints(conn, c->win), &hints, NULL))
        hints.flags = XCB_SIZE_HINT_P_SIZE;

    if(hints.flags & XCB_SIZE_HINT_BASE_SIZE) {
        c->basew = hints.base_width;
        c->baseh = hints.base_height;
    } else if(hints.flags & XCB_SIZE_HINT_P_MIN_SIZE) {
        c->basew = hints.min_width;
        c->baseh = hints.min_height;
    } else
        c->basew = c->baseh = 0;

    if(hints.flags & XCB_SIZE_HINT_P_RESIZE_INC) {
        c->incw = hints.width_inc;
        c->inch = hints.height_inc;
    } else
        c->incw = c->inch = 0;

    if(hints.flags & XCB_SIZE_HINT_P_MAX_SIZE) {
        c->maxw = hints.max_width;
        c->maxh = hints.max_height;
    } else
        c->maxw = c->maxh = 0;

    if(hints.flags & XCB_SIZE_HINT_P_MIN_SIZE) {
        c->minw = hints.min_width;
        c->minh = hints.min_height;
    } else if (hints.flags & XCB_SIZE_HINT_BASE_SIZE) {
        c->minw = hints.base_width;
        c->minh = hints.base_height;
    } else
        c->minw = c->minh = 0;

    if(hints.flags & XCB_SIZE_HINT_P_ASPECT) {
        c->mina = (float)hints.min_aspect_num / (float)hints.min_aspect_den;
        c->maxa = (float)hints.max_aspect_num / (float)hints.max_aspect_den;
    } else
        c->mina = c->maxa = 0;

	c->isfixed = (c->maxw && c->minw && c->maxh && c->minh
	             && c->maxw == c->minw && c->maxh == c->minh);
}

static void manage(xcb_window_t w)
{
	client_t *c;

    printf("New client %d\n", w);

	if(!(c = calloc(1, sizeof(client_t))))
		die("fatal: could not malloc() %u bytes\n", sizeof(client_t));
	c->win = w;

    xcb_get_geometry_reply_t* geom
        = xcb_get_geometry_reply(conn, xcb_get_geometry(conn, w), NULL);
    if(!geom)
        return;

	/* geometry */
	c->x = geom->x;
	c->y = geom->y;
	c->w = geom->width;
	c->h = geom->height;
	c->oldbw = geom->border_width;

    free(geom);

    /* FIXME */
	if(c->w == sw && c->h == sh) {
		c->x = sx;
		c->y = sy;
	} else {
		if(c->x + c->w > sx + sw)
			c->x = sx + sw - c->w;
		if(c->y + c->h > sy + sh)
			c->y = sy + sh - c->h;
		c->x = MAX(c->x, sx);
		/* only fix client y-offset, if the client center might cover the bar */
		c->y = MAX(c->y, sy);
	}

    uint16_t mask = 0;
    xcb_params_configure_window_t params;
    XCB_AUX_ADD_PARAM(&mask, &params, border_width, 0);
    configure(w, mask, &params);

	// configure_event(c); /* propagates border_width, if size doesn't change */

	updatesizehints(c);

    {
        uint32_t mask = 0;
        xcb_params_cw_t params;
        XCB_AUX_ADD_PARAM(&mask, &params, event_mask,
                          XCB_EVENT_MASK_ENTER_WINDOW |
                          XCB_EVENT_MASK_FOCUS_CHANGE |
                          XCB_EVENT_MASK_PROPERTY_CHANGE |
                          XCB_EVENT_MASK_STRUCTURE_NOTIFY);

        xcb_void_cookie_t c
            = xcb_aux_change_window_attributes_checked(conn, w, mask, &params);

        if(xcb_request_check(conn, c))
            die("Unable to select events for window.\n");
    }


    c->isfloating = c->isfloating || c->isfixed;

    /* Ugly */
    xcb_get_property_cookie_t cookie = xcb_get_wm_transient_for(conn, w);

    xcb_get_property_reply_t* transient_reply
        = xcb_get_property_reply(conn, cookie, NULL);

    xcb_window_t transient_for;
    if(xcb_get_wm_transient_for_from_reply(&transient_for, transient_reply))
        c->isfloating = c->isfloating || transient_for != XCB_NONE;

    free(transient_reply);

	if(c->isfloating)
    {
        uint16_t mask = 0;
        xcb_params_configure_window_t param;
        XCB_AUX_ADD_PARAM(&mask, &param, stack_mode, XCB_STACK_MODE_ABOVE);
        configure(c->win, mask, &params);
    }

	attach(c);
	attachstack(c);

    printf("configuring: %dx%d - %dx%d\n", c->x, c->y, c->w, c->h);

	/* some windows require this */
    {
        uint16_t mask = 0;
        xcb_params_configure_window_t param;
        XCB_AUX_ADD_PARAM(&mask, &param, x, c->x + 2*sw);
        XCB_AUX_ADD_PARAM(&mask, &param, y, c->y);
        XCB_AUX_ADD_PARAM(&mask, &param, width, c->w);
        XCB_AUX_ADD_PARAM(&mask, &param, height, c->h);
        configure(c->win, mask, &params);
    }

    if(xcb_request_check(conn, xcb_map_window_checked(conn, w)))
        die("Unable to map window.\n");

	setclientstate(c, XCB_WM_STATE_NORMAL, false);
	arrange();
}

static int mappingnotify(void* p, xcb_connection_t* conn, xcb_mapping_notify_event_t* e)
{
    return 0;
}

static int maprequest(void* p, xcb_connection_t* conn, xcb_map_request_event_t* e)
{
    xcb_get_window_attributes_cookie_t c
        = xcb_get_window_attributes(conn, e->window);
    xcb_get_window_attributes_reply_t* i
        = xcb_get_window_attributes_reply(conn, c, NULL);

    if(i && !i->override_redirect)
        if(!getclient(e->window))
            manage(e->window);

    free(i);
    return 0;
}

static void updatewmhints(client_t *c)
{
    xcb_get_property_cookie_t cookie = xcb_get_wm_hints(conn, c->win);
    xcb_get_property_reply_t* hints_reply
        = xcb_get_property_reply(conn, cookie, NULL);

    xcb_wm_hints_t hints;
    if(xcb_get_wm_hints_from_reply(&hints, hints_reply))
    {
        if(c == sel && hints.flags & XCB_WM_HINT_X_URGENCY)
        {
            hints.flags &= ~XCB_WM_HINT_X_URGENCY;
            xcb_set_wm_hints(conn, c->win, &hints);
        }
    }
    else
        c->isurgent = !!(hints.flags & XCB_WM_HINT_X_URGENCY);

    free(hints_reply);
}

static void check_refloat(client_t* c)
{
    xcb_get_property_cookie_t cookie = xcb_get_wm_transient_for(conn, c->win);

    xcb_get_property_reply_t* transient_reply
        = xcb_get_property_reply(conn, cookie, NULL);

    xcb_window_t transient_for;
    if(xcb_get_wm_transient_for_from_reply(&transient_for, transient_reply))
    {
        bool oldisfloating = c->isfloating;
        c->isfloating = getclient(transient_for) != NULL;
        if(c->isfloating != oldisfloating)
            arrange();
    }
}

static int propertynotify(void* p, xcb_connection_t* conn, xcb_property_notify_event_t* e)
{
	client_t *c;

	if((e->window == screen->root) && (e->atom == WM_NAME))
        return 0; /* ignore */
	if(e->state == XCB_PROPERTY_DELETE)
		return 0; /* ignore */
	if((c = getclient(e->window)))
    {
        if(e->atom == WM_TRANSIENT_FOR)
            check_refloat(c);
        else if(e->atom == WM_NORMAL_HINTS)
            updatesizehints(c);
        else if(e->atom == WM_HINTS)
            updatewmhints(c);
    }
    return 0;
}

static int configurenotify(void* p, xcb_connection_t* conn, xcb_configure_notify_event_t *e)
{
	if(e->window == screen->root && (e->width != sw || e->height != sh)) {
		sw = e->width;
		sh = e->height;
		updategeom();
		arrange();
	}
    return 0;
}

static int unmapnotify(void* p, xcb_connection_t* conn, xcb_unmap_notify_event_t* e)
{
	client_t *c = getclient(e->window);
    if(c)
		unmanage(c);
    return 0;
}

static void run()
{
    xcb_generic_event_t* e;
    xcb_event_handlers_t eh;
    memset(&eh, 0, sizeof(eh));
    xcb_event_handlers_init(conn, &eh);

    xcb_event_set_configure_request_handler(&eh, configurerequest, NULL);
    xcb_event_set_configure_notify_handler(&eh, configurenotify, NULL);
    xcb_event_set_destroy_notify_handler(&eh, destroynotify, NULL);
    xcb_event_set_enter_notify_handler(&eh, enternotify, NULL);
    xcb_event_set_focus_in_handler(&eh, focusin, NULL);
    xcb_event_set_mapping_notify_handler(&eh, mappingnotify, NULL);
    xcb_event_set_map_request_handler(&eh, maprequest, NULL);
    xcb_event_set_property_notify_handler(&eh, propertynotify, NULL);
    xcb_event_set_unmap_notify_handler(&eh, unmapnotify, NULL);

    while(!stop_wm && (e = xcb_wait_for_event(conn)))
    {
        xcb_event_handle(&eh, e);
        free(e);
    }
}

static void scan()
{
    xcb_query_tree_cookie_t c = xcb_query_tree(conn, screen->root);

    xcb_generic_error_t* err;
    xcb_query_tree_reply_t* tree = xcb_query_tree_reply(conn, c, &err);
    if(!tree)
        die("Unable to query windows hierarchy.\n");

    int len = xcb_query_tree_children_length(tree);
    xcb_window_t* children = xcb_query_tree_children(tree);

    xcb_get_window_attributes_cookie_t* cookies
        = malloc(sizeof(xcb_get_window_attributes_cookie_t) * len);
    xcb_get_property_cookie_t* transient_cookies
        = malloc(sizeof(xcb_get_property_cookie_t) * len);
    xcb_get_property_cookie_t* hints_cookies
        = malloc(sizeof(xcb_get_property_cookie_t) * len);

    int i;
    for(i = 0; i < len; ++i) {
        cookies[i] = xcb_get_window_attributes(conn, children[i]);
        transient_cookies[i] = xcb_get_wm_transient_for(conn, children[i]);
        hints_cookies[i] = xcb_get_wm_hints(conn, children[i]);
    }

    int ntransients = 0;
    xcb_window_t* transients = malloc(len * sizeof(xcb_window_t));

    /* Non-transient */
    for(i = 0; i < len; ++i)
    {
        xcb_get_window_attributes_reply_t* info
            = xcb_get_window_attributes_reply(conn, cookies[i], NULL);
        xcb_get_property_reply_t* transient_reply
            = xcb_get_property_reply(conn, transient_cookies[i], NULL);
        xcb_get_property_reply_t* hints_reply
            = xcb_get_property_reply(conn, hints_cookies[i], NULL);

        /* Skip windows which can't be queried about */
        if(!info)
        {
            free(transient_reply);
            free(hints_reply);
            continue;
        }

        /* Skip override-redirect windows */
        if(info->override_redirect)
        {
            free(info);
            free(transient_reply);
            free(hints_reply);
            continue;
        }

        xcb_wm_hints_t hints;

        /* Skip non-viewable and non-iconic windows */
        if(info->map_state != XCB_MAP_STATE_VIEWABLE
           || !xcb_get_wm_hints_from_reply(&hints, hints_reply)
           || hints.initial_state == XCB_WM_STATE_ICONIC)
        {
            free(info);
            free(transient_reply);
            free(hints_reply);
            continue;
        }

        /* Delay transient-for windows for a second loop */
        xcb_window_t transient_for;
        if(xcb_get_wm_transient_for_from_reply(&transient_for, transient_reply))
        {
            transients[ntransients++] = children[i];
            free(info);
            free(transient_reply);
            free(hints_reply);
            continue;
        }

        manage(children[i]);

        free(info);
        free(transient_reply);
        free(hints_reply);
    }

    /* transient */
    for(i = 0; i < ntransients; ++i)
        manage(transients[i]);

    free(tree);
    free(transients);
    free(cookies);
    free(transient_cookies);
    free(hints_cookies);
}

int main(int argc, char *argv[])
{
	if(argc == 2 && !strcmp("-v", argv[1]))
		die("uuwm-"VERSION", © 2006-2009 uuwm engineers, see LICENSE for details\n");
	else if(argc != 1)
		die("usage: uuwm [-v]\n");

    if(!(conn = xcb_connect(NULL, &default_screen)))
		die("cannot open display\n");
    if(!(screen = xcb_aux_get_screen(conn, default_screen)))
        die("cannot obtain default screen\n");

	checkotherwm();
	setup();
	scan();
	run();
	cleanup();

    xcb_disconnect(conn);
	return 0;
}
