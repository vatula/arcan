static bool xwlsurf_shmifev_handler(
	struct comp_surf* surf, struct arcan_event* ev)
{
	return false;
}

static bool xwl_defer_handler(
	struct surface_request* req, struct arcan_shmif_cont* con)
{
	if (!req || !con){
		return false;
	}

	struct comp_surf* surf = wl_resource_get_user_data(req->target);
	surf->acon = *con;
	surf->cookie = 0xfeedface;
	surf->shell_res = req->target;
	surf->dispatch = xwlsurf_shmifev_handler;

	return true;
}

static bool xwl_pair_surface(struct comp_surf* surf, struct wl_resource* res)
{
/* missing:
 * launch / check the xwayland / xwayland-wm setup,
 * check valid IDs and match against the resource - get decoration
 * parameters etc.
 */

	request_surface(surf->client, &(struct surface_request){
/* SEGID should be X11, but need to patch durden as well */
			.segid = SEGID_APPLICATION,
			.target = res,
			.trace = "xwl",
			.dispatch = xwl_defer_handler,
			.client = surf->client,
			.source = surf,
			.tag = NULL
	}, 'X');

	return true;
}
