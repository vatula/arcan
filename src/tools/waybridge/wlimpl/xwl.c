/*
 * There are a number of oddities with dealing with XWayland, and
 * its behavior change depending on if you are using rootless mode
 * or not.
 *
 * With 'normal' mode it behaves as a dumb (and buggy) wl_shell
 * client that basically ignored everything.
 *
 * With 'rootless' mode, it creates compositor surfaces and uses
 * them directly - being basically the only client to do so. The
 * job then is to pair these surfaces based on a window property
 * and just treat them as something altogether special by adding
 * a custom window-manager.
 *
 * The process we're using is that whenever a compositor surface
 * tries to commit, we check if we are running with xwayland and
 * then fires up a window manager whi
 */

#include <sys/types.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <unistd.h>

static FILE* wmfd_output = NULL;
static pid_t xwl_wm_pid = -1;

static int wmfd_input = -1;
static char wmfd_inbuf[256];
static size_t wmfd_ofs = 0;

static void process_input(const char* msg)
{
	trace(TRACE_XWL, "%s", msg);
}

/*
 * Process / update the incoming pipe, or spawn/respawn the WM if it doesn't
 * exist. This will synch the map- table of known surface IDs that we want to
 * pair with surfaces.
 */
static void xwl_check_wm()
{
	if (xwl_wm_pid == -1){
		trace(TRACE_XWL, "spawning 'arcan-xwayland-wm'");
		int p2c_pipe[2];
		int c2p_pipe[2];
		if (-1 == pipe(p2c_pipe))
			return;

		if (-1 == pipe(c2p_pipe)){
			close(p2c_pipe[0]);
			close(p2c_pipe[1]);
			return;
		}

		wmfd_input = c2p_pipe[0];
		wmfd_output = fdopen(p2c_pipe[1], "w");

		xwl_wm_pid = fork();

		if (-1 == xwl_wm_pid){
			fprintf(stderr, "Couldn't spawn wm- process (fork failed)\n");
			exit(EXIT_FAILURE);
		}

/* child, close, dup spawn */
		if (!xwl_wm_pid){
			char* const argv[] = {"arcan-xwayland-wm", NULL};
			close(p2c_pipe[1]);
			close(c2p_pipe[0]);
			dup2(STDIN_FILENO, p2c_pipe[0]);
			dup2(STDOUT_FILENO, c2p_pipe[1]);
			close(p2c_pipe[1]);
			close(c2p_pipe[0]);
			execvp("arcan-xwayland-wm", argv);
			execv("arcan-xwayland-wm", argv);
			exit(EXIT_FAILURE);
		}

/* want the input-pipe to work non-blocking here */
		int flags = fcntl(wmfd_input, F_GETFL);
			if (-1 != flags)
				fcntl(wmfd_input, F_SETFL, flags | O_NONBLOCK);

/* drop child write end, parent read end as the wm process owns these now */
		close(c2p_pipe[1]);
		close(p2c_pipe[0]);
	}

/* populate inbuffer, look for linefeed */
	char inbuf[256];
	ssize_t nr = read(wmfd_input, inbuf, 256);
	if (-1 == nr){
		if (errno != EAGAIN && errno != EINTR){
			fclose(wmfd_output);
			close(wmfd_input);
			wmfd_ofs = 0;
			kill(xwl_wm_pid, SIGKILL);
			waitpid(xwl_wm_pid, NULL, 0);
			xwl_wm_pid = -1;
			wmfd_input = -1;
			trace(TRACE_XWL, "arcan-xwayland-wm died");
		}
		return;
	}

/* check the new input for a linefeed character, or flush to the buffer */
	for (size_t i = 0; i < nr; i++){
		if (inbuf[i] == '\n'){
			wmfd_inbuf[wmfd_ofs] = '\0';
			process_input(wmfd_inbuf);
			wmfd_ofs = 0;
		}
/* accept crop on overflow (though no command should be this long) */
		else {
			wmfd_ofs = (wmfd_ofs + 1) % sizeof(wmfd_input);
			wmfd_inbuf[wmfd_ofs] = inbuf[i];
		}
	}
}

static bool xwlsurf_shmifev_handler(
	struct comp_surf* surf, struct arcan_event* ev)
{
	if (ev->category != EVENT_TARGET)
		return false;

	switch (ev->tgt.kind){
	case TARGET_COMMAND_DISPLAYHINT:
/* write to the xwl_wm_fd:
 * configure:id=%d:...
 * focus:id=%d
 */
	break;
	default:
	break;
	}

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

static bool lookup_surface(struct comp_surf* surf, struct wl_resource* res)
{
	if (!wl.use_xwayland)
		return false;

/* FIXME: need to sweep / poll the list of known surfaces in order
 * to pair and return correctly */

	return true;
}

static bool xwl_pair_surface(struct comp_surf* surf, struct wl_resource* res)
{
/* do we know of a matching xwayland- provided surface? */
	if (!lookup_surface(surf, res))
		return false;

/* if so, allocate the corresponding arcan- side resource */
	return request_surface(surf->client, &(struct surface_request){
/* SEGID should be X11, but need to patch durden as well */
			.segid = SEGID_APPLICATION,
			.target = res,
			.trace = "xwl",
			.dispatch = xwl_defer_handler,
			.client = surf->client,
			.source = surf,
			.tag = NULL
	}, 'X');
}
