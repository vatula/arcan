/*
 * Copyright 2016-2018, Björn Ståhl
 * License: 3-Clause BSD, see COPYING file in arcan source repository.
 * Reference: https://github.com/letoram/arcan/wiki/wayland.md
 * Description: XWayland specific 'wnddow Manager' that deals with the
 * special considerations needed for pairing XWayland redirected windows
 * with wayland surfaces etc.
 */
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <inttypes.h>
#include <string.h>
/* #include <X11/XCursor/XCursor.h> */
#include <xcb/xcb.h>
#include <xcb/composite.h>
#include <xcb/xfixes.h>
#include <xcb/xcb_event.h>

static xcb_connection_t* dpy;
static xcb_screen_t* screen;
static xcb_drawable_t root;
static xcb_drawable_t wnd;
static xcb_colormap_t colormap;
static xcb_visualid_t visual;

#include "atoms.h"

static inline void trace(const char* msg, ...)
{
	va_list args;
	va_start( args, msg );
		vfprintf(stderr,  msg, args );
		fprintf(stderr, "\n");
	va_end( args);
	fflush(stderr);
}

static void scan_atoms()
{
	for (size_t i = 0; i < ATOM_LAST; i++){
		xcb_intern_atom_cookie_t cookie =
			xcb_intern_atom(dpy, 0, strlen(atom_map[i]), atom_map[i]);

		xcb_generic_error_t* error;
		xcb_intern_atom_reply_t* reply =
			xcb_intern_atom_reply(dpy, cookie, &error);
		if (reply && !error){

		}
		if (error){
			fprintf(stderr,
				"atom (%s) failed with code (%d)\n", atom_map[i], error->error_code);
			free(error);
		}
		free(reply);
	}

/* do we need to add xfixes here? */
}

static bool setup_visuals()
{
	xcb_depth_iterator_t depth =
		xcb_screen_allowed_depths_iterator(screen);

	while(depth.rem > 0){
		if (depth.data->depth == 32){
			visual = (xcb_depth_visuals_iterator(depth.data).data)->visual_id;
			colormap = xcb_generate_id(dpy);
			xcb_create_colormap(dpy, XCB_COLORMAP_ALLOC_NONE, colormap, root, visual);
			return true;
		}
		xcb_depth_next(&depth);
	}

	return false;
}

static void create_window()
{
	wnd = xcb_generate_id(dpy);
	xcb_create_window(dpy,
		XCB_COPY_FROM_PARENT, wnd, root,
		0, 0, 10, 10, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
		visual, 0, NULL
	);
	xcb_change_property(dpy,
		XCB_PROP_MODE_REPLACE, wnd,
		atoms[NET_SUPPORTING_WM_CHECK], XCB_ATOM_WINDOW, 32, 1, &wnd);
/* wm name, utf8 string
 * supporting wm, selection_owner, ... */
}

static void configure_request(xcb_configure_request_event_t* ev)
{
	/* ev->window carries the ID for us to lookup */
}

static void xcb_map_request(xcb_map_request_event_t* ev)
{
/* from map_request->window, we should be able to maintain
 * a mapping to a higher-level external object */

/* extract window properties and translate to higher
 * level notes, like decorations, grab etc. */
	xcb_map_window(dpy, ev->window);
}

static void xcb_unmap_notify(xcb_unmap_notify_event_t* ev)
{
/* pair ev->window to our tracking structure
 * and extract the paired identifer so that we can unmap
	xcb_unmap_window(dpy, frame_id);
	*/
}

static void xcb_configure_request(xcb_configure_request_event_t* ev)
{
/* send the window configure */
}

int main (int argc, char **argv)
{
	int code;
	uint32_t values[3];

	xcb_generic_event_t *ev;
/*	xcb_get_geometry_reply_t *geom; */

	dpy = xcb_connect(NULL, NULL);
	if ((code = xcb_connection_has_error(dpy))){
		fprintf(stderr, "Couldn't open display (%d)\n", code);
		return EXIT_FAILURE;
	}

	screen = xcb_setup_roots_iterator(xcb_get_setup(dpy)).data;
	root = screen->root;
	if (!setup_visuals()){
		fprintf(stderr, "Couldn't setup visuals/colormap\n");
		return EXIT_FAILURE;
	}

	scan_atoms();

/*
 * enable structure and redirection notifications so that we can forward
 * the related events onward to the active arcan window manager
 */
	values[0] =
		XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY |
		XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT |
		XCB_EVENT_MASK_PROPERTY_CHANGE;

	xcb_change_window_attributes(dpy, root, XCB_CW_EVENT_MASK, values);
	xcb_composite_redirect_subwindows(dpy, root, XCB_COMPOSITE_REDIRECT_MANUAL);
	xcb_flush(dpy);

	create_window();

/* atom lookup:
 * moveresize, state, fullscreen, maximized vert, maximized horiz, active window
 */

	while( (ev = xcb_wait_for_event(dpy)) ){
		switch (ev->response_type & ~0x80) {
		case XCB_BUTTON_PRESS:
			trace("motion-notify");
		break;
		case XCB_MOTION_NOTIFY:
			trace("motion-notify");
		break;
		case XCB_BUTTON_RELEASE:
			trace("button-release");
		break;
		case XCB_ENTER_NOTIFY:
			trace("enter-notify");
		break;
		case XCB_CREATE_NOTIFY:
			trace("create-notify");
		break;
		case XCB_MAP_REQUEST:
			trace("map-request");
			xcb_map_request((xcb_map_request_event_t*) ev);
		break;
    case XCB_MAP_NOTIFY:
			trace("map-notify");
		break;
    case XCB_UNMAP_NOTIFY:
			trace("unmap-notify");
			xcb_unmap_notify((xcb_unmap_notify_event_t*) ev);
		break;
    case XCB_REPARENT_NOTIFY:
			trace("reparent-notify");
		break;
    case XCB_CONFIGURE_REQUEST:
			trace("configure-request");
			xcb_configure_request((xcb_configure_request_event_t*) ev);
		break;
    case XCB_CONFIGURE_NOTIFY:
			trace("configure-notify");
		break;
		case XCB_DESTROY_NOTIFY:
			trace("destroy-notify");
		break;
		case XCB_MAPPING_NOTIFY:
			trace("mapping-notify");
		break;
		case XCB_PROPERTY_NOTIFY:
			trace("property-notify");
		break;
		case XCB_CLIENT_MESSAGE:
			trace("client-message");
		break;
		case XCB_FOCUS_IN:
			trace("focus-in");
		break;
		default:
			trace("unhandled");
		break;
		}
		xcb_flush(dpy);
	}

return 0;
}
