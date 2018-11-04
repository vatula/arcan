/*
 * Pipe-based implementation of the A12 protocol,
 * relying on pre-established secure channels and low
 * bandwidth demands.
 */
#include <arcan_shmif.h>
#include <arcan_shmif_server.h>
#include <errno.h>
#include <unistd.h>
#include <poll.h>
#include <fcntl.h>
#include "a12.h"

static const short c_pollev = POLLIN | POLLERR | POLLNVAL | POLLHUP;

static inline void trace(const char* msg, ...)
{
#ifdef _DEBUG
	va_list args;
	va_start( args, msg );
		vfprintf(stderr,  msg, args );
		fprintf(stderr, "\n");
	va_end( args);
	fflush(stderr);
#endif
}

static void on_srv_event(int chid, struct arcan_event* ev, void* tag)
{
	struct shmifsrv_client* cs = tag;
	trace("client event: %s on ch %d", arcan_shmif_eventstr(ev, NULL, 0), chid);
	if (chid != 0){
		fprintf(stderr, "Multi-channel support not yet finished\n");
		return;
	}

/* note, this needs to be able to buffer etc. to handle a client that has
 * a saturated event queue ... */
	shmifsrv_enqueue_event(cs, ev, -1);
}

static void server_mode(
	struct shmifsrv_client* a, struct a12_state* ast, int fdin, int fdout)
{
/* 1. setup a12 in connect mode, _open */
	struct pollfd fds[3] = {
		{ .fd = shmifsrv_client_handle(a), .events = c_pollev },
		{	.fd = fdin, .events = c_pollev },
		{ .fd = fdout, .events = fdout }
	};

#ifdef DUMP_OUT
	FILE* fpek_out = fopen("netpipe.srv.out", "w+");
	FILE* fpek_in = fopen("netpipe.srv.in", "w+");
#endif

	bool alive = true;

	uint8_t* outbuf;
	size_t outbuf_sz = 0;

	while (alive){
/* first, flush current outgoing and/or swap buffers */
		int np = 2;
		if (outbuf_sz || (outbuf_sz = a12_channel_flush(ast, &outbuf))){
			ssize_t nw = write(fdout, outbuf, outbuf_sz);
#ifdef DUMP_OUT
			fwrite(outbuf, 1, outbuf_sz, fpek_out);
			fprintf(fpek_out, "\n--end of block\n");
			fflush(fpek_out);
#endif
			if (nw > 0)
				outbuf_sz -= nw;
			if (outbuf_sz)
				np = 3;
		}

/* pollset is extended to cover STDOUT if we have an ongoing buffer,
 * note that we right now have no real handle to poll client events
 * and that needs to be communicated via an OOB thread with futexes */
		int sv = poll(fds, np, 16);
		if (sv < 0){
			if (sv == -1 && errno != EAGAIN && errno != EINTR)
				alive = false;
			continue;
		}

/* STDIN - update a12 state machine */
		if (sv && fds[1].revents){
			uint8_t inbuf[9000];
			ssize_t nr = 0;
			while ((nr = read(fds[1].fd, inbuf, 9000)) > 0){
				trace("(srv) unpack %zd bytes", nr);
#ifdef DUMP_OUT
				fwrite(inbuf, 1, nr, fpek_in);
				fprintf(fpek_in, "\n--end of block\n");
				fflush(fpek_in);
#endif
				a12_channel_unpack(ast, inbuf, nr, a, on_srv_event);
			}
		}

/* SHMIF-client - poll event queue, check/dispatch buffers */
		if (sv && fds[0].revents){
			if (fds[0].revents != POLLIN){
				alive = false;
				continue;
			}

		}

		struct arcan_event newev;
		while (shmifsrv_dequeue_events(a, &newev, 1)){
			trace("(srv) forward event: %s", arcan_shmif_eventstr(&newev, NULL, 0));
			a12_channel_enqueue(ast, &newev);
		}

		switch(shmifsrv_poll(a)){
			case CLIENT_DEAD:
				trace("(srv) client died");
/* the descriptor will be gone so next poll will fail */
			break;
			case CLIENT_NOT_READY:
/* do nothing */
			break;
			case CLIENT_VBUFFER_READY:{
				trace("(srv) video-buffer");
/* copy + release if possible,
	 arcan_event ev = {
	     .category = EVENT_TARGET,
			 .tgt.kind = TARGET_COMMAND_BUFFER_FAIL
	 };

 */
				struct shmifsrv_vbuffer vb = shmifsrv_video(a, false);
				a12_channel_vframe(ast, &vb);
				shmifsrv_video(a, true);
			}
			break;
			case CLIENT_ABUFFER_READY:
				trace("(srv) audio-buffer");
/* copy + release if possible */
				shmifsrv_audio(a, NULL, 0);
			break;
/* do nothing */
			break;
			}
		}

#ifdef DUMP_OUT
	fclose(fpek_out);
	fclose(fpek_in);
#endif
	trace("(srv) shutting down connection");
	shmifsrv_free(a);
}

static int run_shmif_server(
	uint8_t* authk, size_t auth_sz, const char* cp, int fdin, int fdout)
{
	int fd = -1, sc = 0;

/* set to non-blocking */
	int flags = fcntl(fdout, F_GETFL);
	fcntl(fdout, F_SETFL, flags | O_NONBLOCK);
	flags = fcntl(fdin, F_GETFL);
	fcntl(fdin, F_SETFL, flags | O_NONBLOCK);

/* repeatedly open the same connection point */
	while(true){
		struct shmifsrv_client* cl =
			shmifsrv_allocate_connpoint(cp, NULL, S_IRWXU, &fd, &sc, 0);

		if (!cl){
			fprintf(stderr, "couldn't allocate connection point\n");
			return EXIT_FAILURE;
		}

		if (-1 == fd)
			fd = shmifsrv_client_handle(cl);

		if (-1 == fd){
			fprintf(stderr, "descriptor allocator failed, couldn't open connection point\n");
			return EXIT_FAILURE;
		}

		struct pollfd pfd = { .fd = fd, .events = POLLIN | POLLERR | POLLHUP };
		trace("(srv) configured, polling");
		if (poll(&pfd, 1, -1) == 1){
			trace("(srv) got connection");

/* go through the accept step, now we can hand the connection over
 * and repeat the listening stage in some other execution context,
 * here's the point to thread or multiprocess */
			if (pfd.revents == POLLIN){
				shmifsrv_poll(cl);

/* build the a12 state and hand it over to the main loop */
				server_mode(cl, a12_channel_open(authk, auth_sz), fdin, fdout);
			}
			else{
				trace("(srv) poll failed, rebuilding");
				shmifsrv_free(cl);
			}
		}
/* SIGINTR */
		else
			break;

/* wait until something happens */
	}
	return EXIT_SUCCESS;
}

struct client_state {
	struct arcan_shmif_cont* wnd;
	struct a12_state* state;
};

static void on_cl_event(int chid, struct arcan_event* ev, void* tag)
{
	struct client_state* cs = tag;
	trace("client event: %s on ch %d", arcan_shmif_eventstr(ev, NULL, 0), chid);
	if (chid != 0){
		fprintf(stderr, "Multi-channel support not yet finished\n");
		return;
	}

	arcan_shmif_enqueue(cs->wnd, ev);
}

static int run_shmif_client(
	uint8_t* authk, size_t authk_sz, int fdin, int fdout)
{
	struct arcan_shmif_cont wnd =
		arcan_shmif_open(SEGID_UNKNOWN, SHMIF_NOACTIVATE, NULL);

#ifdef DUMP_OUT
	FILE* fpek_out = fopen("netpipe.cl.out", "w+");
	FILE* fpek_in = fopen("netpipe.cl.in", "w+");
#endif

	struct a12_state* ast = a12_channel_build(authk, authk_sz);
	if (!ast){
		fprintf(stderr, "Couldn't allocate Client state machine\n");
		return EXIT_FAILURE;
	}

	a12_set_destination(ast, &wnd, 0);

	struct client_state cs = {
		.wnd = &wnd,
		.state = ast
	};

/* set to non-blocking */
	int flags = fcntl(fdin, F_GETFL);
	fcntl(fdin, F_SETFL, flags | O_NONBLOCK);

	struct pollfd fds[] = {
		{ .fd = wnd.epipe, .events = c_pollev },
		{	.fd = fdin, .events = c_pollev }
	};

	uint8_t* outbuf;
	size_t outbuf_sz = 0;
	trace("(cl) got proxy connection, waiting for source");

	bool alive = true;
	while (alive){
/* first, flush current outgoing and/or swap buffers */
		int np = 2;
		if (outbuf_sz || (outbuf_sz = a12_channel_flush(ast, &outbuf))){
			ssize_t nw = write(fdout, outbuf, outbuf_sz);
#ifdef DUMP_OUT
			fwrite(outbuf, 1, outbuf_sz, fpek_out);
			fflush(fpek_out);
#endif
			if (nw > 0)
				outbuf_sz -= nw;
			if (outbuf_sz)
				np = 3;
			continue;
		}

/* events from parent, nothing special - unless the carry a descriptor */
		int sv = poll(fds, np, -1);

		if (sv < 0){
			if (sv == -1 && errno != EAGAIN && errno != EINTR)
				alive = false;
			continue;
		}

		if (sv && fds[0].revents){
			struct arcan_event newev;
			int sc;
			while (( sc = arcan_shmif_poll(&wnd, &newev)) > 0){
				trace("(cl) incoming event: %s", arcan_shmif_eventstr(&newev, NULL, 0));
/* FIXME: special consideration for subsegment channels */
				a12_channel_enqueue(ast, &newev);
			}
/* FIXME: send disconnect packet */
			if (-1 == sc){
				alive = false;
			}
		}

/* flush data-in and feed to state machine */
		if (sv && fds[1].revents){
			uint8_t buf[9000];
			ssize_t nr;
			if ((nr = read(fds[1].fd, buf, 9000)) > 0){
#ifdef DUMP_OUT
				fwrite(buf, 1, nr, fpek_in);
				fprintf(fpek_in, "\n--end of block\n");
				fflush(fpek_in);
#endif
				a12_channel_unpack(ast, buf, nr, &cs, on_cl_event);
			}
		}
	}

#ifdef DUMP_OUT
	fclose(fpek_in);
	fclose(fpek_out);
#endif
	return EXIT_SUCCESS;
}

static int run_shmif_test(uint8_t* authk, size_t auth_sz, bool sp)
{
	int clpipe[2];
	int srvpipe[2];

	pipe(clpipe);
	pipe(srvpipe);

/* just ugly- sleep and assume that the server has been setup */
	if (fork() > 0){
		sleep(1);
		return sp ?
			run_shmif_client(authk, auth_sz, clpipe[0], srvpipe[1]) :
			run_shmif_server(authk, auth_sz, "test", srvpipe[0], clpipe[1]);
	}

	close(STDERR_FILENO);
	return sp ?
		run_shmif_server(authk, auth_sz, "test", srvpipe[0], clpipe[1]) :
		run_shmif_client(authk, auth_sz, clpipe[0], srvpipe[1]);
}

static int show_usage(const char* n, const char* msg)
{
	fprintf(stderr, "%s\nUsage:\n\t%s client mode: arcan-net [-k authkfile(0<n<64b)] -c"
	"\n\t%s server mode: arcan-net [-k authfile(0<n<64b)] -s connpoint\n"
	"\t%s testing mode: arcan-net [-k authfile(0<n<64b)] -t(server main) or -T (client main)\n", msg, n, n, n);
	return EXIT_FAILURE;
}

int main(int argc, char** argv)
{
	uint8_t authk[64] = {0};
	size_t authk_sz = 64;

	const char* cp = NULL;
	int mode = 0;

	for (size_t i = 1; i < argc; i++){
		if (strcmp(argv[i], "-k") == 0){
			i++;
			if (i == argc)
				return show_usage(argv[i], "missing keyfile argument");
			else {
				FILE* fpek = fopen(argv[i], "r");
				if (!fpek)
					return show_usage(argv[i], "keyfile couldn't be read");
				authk_sz = fread(authk, 1, 64, fpek);
				fclose(fpek);
			}
		}
		else if (strcmp(argv[i], "-s") == 0){
			i++;
			if (i == argc)
				return show_usage(argv[i], "missing connection point argument");
			mode = 1;
			cp = argv[i];
			break;
		}
		else if (strcmp(argv[i], "-c") == 0){
			mode = 2;
			break;
		}
		else if (strcmp(argv[i], "--test") == 0 || strcmp(argv[i], "-t") == 0){
			mode = 3;
			break;
		}
		else if (strcmp(argv[i], "--TEST") == 0 || strcmp(argv[i], "-T") == 0){
			mode = 4;
			break;
		}
	}

	if (mode == 3 || mode == 4){
		if (!getenv("ARCAN_CONNPATH")){
			fprintf(stderr, "Test mode: No ARCAN_CONNPATH env\n");
			return EXIT_FAILURE;
		}
		return run_shmif_test(authk, authk_sz, mode == 3);
	}

	if (isatty(STDIN_FILENO) || isatty(STDOUT_FILENO))
		return show_usage(argv[0], "[stdin] / [stdout] should not be TTYs\n");

	if (mode == 0)
		return show_usage(argv[0], "missing connection mode (-c or -s)");

	if (mode == 1)
		return run_shmif_server(authk, authk_sz, cp, STDIN_FILENO, STDOUT_FILENO);

	if (mode == 2)
		return run_shmif_client(authk, authk_sz, STDIN_FILENO, STDOUT_FILENO);

	return EXIT_SUCCESS;
}
