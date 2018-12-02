/*
 * Copyright: 2017-2018, Björn Ståhl
 * Description: A12 protocol state machine, substream decoding routines
 * License: 3-Clause BSD, see COPYING file in arcan source repository.
 * Reference: https://arcan-fe.com
 */
#include <arcan_shmif.h>
#include "a12_int.h"
#include "a12.h"

#ifdef LOG_FRAME_OUTPUT
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#include "../../engine/external/stb_image_write.h"
#endif

bool a12int_buffer_format(int method)
{
	return
		method == POSTPROCESS_VIDEO_H264 ||
		method == POSTPROCESS_VIDEO_MINIZ ||
		method == POSTPROCESS_VIDEO_DMINIZ;
}

/*
 * performance wise we should check if the extra branch in miniz vs. dminiz
 * should be handled here or by inlining and copying
 */
static int video_miniz(const void* buf, int len, void* user)
{
	struct a12_state* S = user;
	struct video_frame* cvf = &S->channels[S->in_channel].unpack_state.vframe;
	struct arcan_shmif_cont* cont = S->channels[S->in_channel].cont;
	const uint8_t* inbuf = buf;

	if (!cont || len > cvf->expanded_sz){
		debug_print(1, "decompression resulted in data overcommit");
		return 0;
	}

/* we have a 1..4 byte spill from a previous call so we need to have
 * a 1-px buffer that we populate before packing */
	if (cvf->carry){
		while (cvf->carry < 3){
			cvf->pxbuf[cvf->carry++] = *inbuf++;
			len--;

/* and this spill can also be short */
			if (!len)
				return 1;
		}

/* and commit */
		if (cvf->postprocess == POSTPROCESS_VIDEO_DMINIZ)
			cont->vidp[cvf->out_pos++] ^=
				SHMIF_RGBA(cvf->pxbuf[0], cvf->pxbuf[1], cvf->pxbuf[2], 0xff);
		else
			cont->vidp[cvf->out_pos++] =
				SHMIF_RGBA(cvf->pxbuf[0], cvf->pxbuf[1], cvf->pxbuf[2], 0xff);

/* which can happen on a row boundary */
		cvf->row_left--;
		if (cvf->row_left == 0){
			cvf->out_pos -= cvf->w;
			cvf->out_pos += cont->pitch;
			cvf->row_left = cvf->w;
		}
		cvf->carry = 0;
	}

/* pixel-aligned fill/unpack, same as everywhere else */
	size_t npx = (len / 3) * 3;
	for (size_t i = 0; i < npx; i += 3){
		if (cvf->postprocess == POSTPROCESS_VIDEO_DMINIZ)
			cont->vidp[cvf->out_pos++] ^=
				SHMIF_RGBA(inbuf[i], inbuf[i+1], inbuf[i+2], 0xff);
		else
			cont->vidp[cvf->out_pos++] =
				SHMIF_RGBA(inbuf[i], inbuf[i+1], inbuf[i+2], 0xff);

			cvf->row_left--;
			if (cvf->row_left == 0){
				cvf->out_pos -= cvf->w;
				cvf->out_pos += cont->pitch;
				cvf->row_left = cvf->w;
			}
	}

/* we need to account for len bytes not aligning */
	if (len - npx){
		cvf->carry = 0;
		for (size_t i = 0; i < len - npx; i++){
			cvf->pxbuf[cvf->carry++] = inbuf[npx + i];
		}
	}

	cvf->expanded_sz -= len;
	return 1;
}

void a12int_decode_vbuffer(
	struct a12_state* S, struct video_frame* cvf, struct arcan_shmif_cont* cont)
{
	if (cvf->postprocess == POSTPROCESS_VIDEO_MINIZ ||
			cvf->postprocess == POSTPROCESS_VIDEO_DMINIZ){
		size_t inbuf_pos = cvf->inbuf_pos;
		tinfl_decompress_mem_to_callback(cvf->inbuf, &inbuf_pos, video_miniz, S, 0);
		free(cvf->inbuf);
		cvf->carry = 0;
		if (cvf->commit && cvf->commit != 255){
			arcan_shmif_signal(cont, SHMIF_SIGVID);
		}
		return;
	}

	debug_print(1, "unhandled unpack method %d", cvf->postprocess);
}

void a12int_unpack_vbuffer(struct a12_state* S,
	struct video_frame* cvf, struct arcan_shmif_cont* cont)
{
/* raw frame types, the implementations and variations are so small that
 * we can just do it here - no need for the more complex stages like for
 * 264, ... */
	if (cvf->postprocess == POSTPROCESS_VIDEO_RGBA){
		for (size_t i = 0; i < S->decode_pos; i += 4){
			cont->vidp[cvf->out_pos++] = SHMIF_RGBA(
				S->decode[i+0], S->decode[i+1], S->decode[i+2], S->decode[i+3]);
			cvf->row_left--;
			if (cvf->row_left == 0){
				cvf->out_pos -= cvf->w;
				cvf->out_pos += cont->pitch;
				cvf->row_left = cvf->w;
			}
		}
	}
	else if (cvf->postprocess == POSTPROCESS_VIDEO_RGB){
		for (size_t i = 0; i < S->decode_pos; i += 3){
			cont->vidp[cvf->out_pos++] = SHMIF_RGBA(
				S->decode[i+0], S->decode[i+1], S->decode[i+2], 0xff);
			cvf->row_left--;
			if (cvf->row_left == 0){
				cvf->out_pos -= cvf->w;
				cvf->out_pos += cont->pitch;
				cvf->row_left = cvf->w;
			}
		}
	}
	else if (cvf->postprocess == POSTPROCESS_VIDEO_RGB565){
		static const uint8_t rgb565_lut5[] = {
			0,     8,  16,  25,  33,  41,  49,  58,  66,   74,  82,  90,  99, 107,
			115, 123, 132, 140, 148, 156, 165, 173, 181, 189,  197, 206, 214, 222,
			230, 239, 247, 255
		};

		static const uint8_t rgb565_lut6[] = {
			0,     4,   8,  12,  16,  20,  24,  28,  32,  36,  40,  45,  49,  53,  57,
			61,   65,  69,  73,  77,  81,  85,  89,  93,  97, 101, 105, 109, 113, 117,
			121, 125, 130, 134, 138, 142, 146, 150, 154, 158, 162, 166, 170, 174,
			178, 182, 186, 190, 194, 198, 202, 206, 210, 215, 219, 223, 227, 231,
			235, 239, 243, 247, 251, 255
		};

		for (size_t i = 0; i < S->decode_pos; i += 2){
			uint16_t px;
			unpack_u16(&px, &S->decode[i]);
			cont->vidp[cvf->out_pos++] =
				SHMIF_RGBA(
					rgb565_lut5[ (px & 0xf800) >> 11],
					rgb565_lut6[ (px & 0x07e0) >>  5],
					rgb565_lut5[ (px & 0x001f)      ],
					0xff
				);
			cvf->row_left--;
			if (cvf->row_left == 0){
				cvf->out_pos -= cvf->w;
				cvf->out_pos += cont->pitch;
				cvf->row_left = cvf->w;
			}
		}
	}

	cvf->inbuf_sz -= S->decode_pos;
	if (cvf->inbuf_sz == 0){
		fprintf(stderr, "done, commit? %d\n", cvf->commit);
		debug_print(2, "video frame completed, commit:%"PRIu8, cvf->commit);
		if (cvf->commit){
			arcan_shmif_signal(cont, SHMIF_SIGVID);
		}
	}
	else {
		debug_print(3, "video buffer left: %"PRIu32, cvf->inbuf_sz);
	}
}
