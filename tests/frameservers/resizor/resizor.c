#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <stdbool.h>

#include <arcan_shmif.h>

static void draw_frame(struct arcan_shmif_cont* cont)
{
	float step_x = (float)cont->w / 255.0;
	float step_y = (float)cont->h / 255.0;

	for (size_t y = 0; y < cont->h; y++){
		for (size_t x = 0; x < cont->w; x++){
			cont->vidp[y * cont->pitch + x] = SHMIF_RGBA(0, step_x * x, step_y * y, 0xff);
		}
	}

	arcan_shmif_signal(cont, SHMIF_SIGVID);
}

#ifdef ENABLE_FSRV_AVFEED
void arcan_frameserver_avfeed_run(const char* resource, const char* keyfile)
#else
int main(int argc, char** argv)
#endif
{
	struct arg_arr* aarr;
	struct arcan_shmif_cont cont = arcan_shmif_open(
		SEGID_APPLICATION, SHMIF_ACQUIRE_FATALFAIL, &aarr);

	arcan_event ev;
	draw_frame(&cont);

	while (arcan_shmif_wait(&cont, &ev)){
		if (ev.category != EVENT_TARGET)
			continue;

		if (ev.tgt.kind == TARGET_COMMAND_DISPLAYHINT){
			if (ev.tgt.ioevs[0].iv && ev.tgt.ioevs[1].iv){
				arcan_shmif_resize(&cont, ev.tgt.ioevs[0].iv, ev.tgt.ioevs[1].iv);
				draw_frame(&cont);
			}
		}
	}

	arcan_shmif_drop(&cont);

#ifndef ENABLE_FSRV_AVFEED
	return EXIT_SUCCESS;
#endif
}
