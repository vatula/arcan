/*
 * Copyright 2016-2018, Björn Ståhl
 * License: 3-Clause BSD, see COPYING file in arcan source repository.
 * Reference: https://github.com/letoram/arcan/wiki/wayland.md
 * Description: XWayland specific 'wnddow Manager' that deals with the
 * special considerations needed for pairing XWayland redirected windows
 * with wayland surfaces etc.
 */
#include <arcan_shmif.h>
#include <inttypes.h>
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
			atoms[i] = reply->atom;
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

static void xcb_map_request(xcb_map_request_event_t* ev)
{
	fprintf(stdout, "kind=map:id=%"PRIu32"\n", ev->window);
/* while the above could've made a round-trip to make sure we don't
 * race with the wayland channel, the approach of detecting surface-
 * type and checking seems to work ok (xwl.c) */
	xcb_map_window(dpy, ev->window);
}

static void xcb_unmap_notify(xcb_unmap_notify_event_t* ev)
{
	fprintf(stdout, "kind=unmap:id=%"PRIu32"\n", ev->window);
}

static void xcb_configure_request(xcb_configure_request_event_t* ev)
{
/* this needs to translate to _resize calls and to VIEWPORT hint events */
	fprintf(stdout,
		"kind=configure:id=%"PRIu32":x=%d:y=%d:w=%d:h=%d\n",
		ev->window, ev->x, ev->y, ev->width, ev->height
	);

/* just ack the configure request for now, this should really be deferred
 * until we receive the corresponding command from our parent but we lack
 * that setup right now */
	xcb_configure_window(dpy, ev->window,
		XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
		XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT |
		XCB_CONFIG_WINDOW_BORDER_WIDTH,
		(uint32_t[]){ev->x, ev->y, ev->width, ev->height, 0}
	);

	xcb_set_input_focus(dpy,
		XCB_INPUT_FOCUS_POINTER_ROOT, ev->window, XCB_CURRENT_TIME);
/*
 * weston does a bit more here,
 *  see _read_properties (protocols, normal hints, wm state,
 *  window type, name, pid, motif_wm_hints, wm_client_machine)
 */
}

/* use stdin/popen/line based format to make debugging easier */
static void process_wm_command()
{
	const char* arg = "";
	struct arg_arr* args = arg_unpack(arg);
	if (!args)
		return;

	const char* dst;
	if (!arg_lookup(args, "kind", 0, &dst)){
		fprintf(stderr, "malformed argument: %s, missing kind\n", arg);
	}
	if (strcmp(dst, "query") == 0){
	}
	else if (strcmp(dst, "maximized") == 0){
	}
	else if (strcmp(dst, "fullscreen") == 0){
	}
	else if (strcmp(dst, "configure") == 0){
	}
	else if (strcmp(dst, "destroy") == 0){
	}
	else if (strcmp(dst, "focus") == 0){
	}

cleanup:
	arg_cleanup(args);
}

int main (int argc, char **argv)
{
	int code;
	uint32_t values[3];

	xcb_generic_event_t *ev;

/* FIXME: this isn't right, we should be responsible for spawning
 * XWayland with -rootless and pass the wm descriptors etc. here,
 * so when that is added, remove the sleep etc.
 */
	if (!getenv("DISPLAY")){
		putenv("DISPLAY=:0");
	}

	int counter = 10;
	while (counter--){
		dpy = xcb_connect(NULL, NULL);
		if ((code = xcb_connection_has_error(dpy))){
			fprintf(stderr, "Couldn't open display (%d)\n", code);
			sleep(1);
			continue;
		}
		break;
	}
	if (!counter)
		return EXIT_FAILURE;

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
