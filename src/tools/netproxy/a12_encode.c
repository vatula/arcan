/*
 * Copyright: 2017-2018, Björn Ståhl
 * Description: A12 protocol state machine
 * License: 3-Clause BSD, see COPYING file in arcan source repository.
 * Reference: https://arcan-fe.com
 */

#include <stdint.h>
#include <stdlib.h>
#include <inttypes.h>
#include <stdio.h>

#include "a12_int.h"
#include "a12.h"
#include "a12_encode.h"

#ifdef LOG_FRAME_OUTPUT
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#include "../../engine/external/stb_image_write.h"
#endif

/*
 * create the control packet
 */
static void a12int_vframehdr_build(uint8_t buf[CONTROL_PACKET_SIZE],
	uint64_t last_seen, uint8_t chid,
	int type, uint32_t sid,
	uint16_t sw, uint16_t sh, uint16_t w, uint16_t h, uint16_t x, uint16_t y,
	uint32_t len, uint32_t exp_len, bool commit)
{
	debug_print(2, "vframehdr: ch: %"PRIu8", type: %d, sid: %"PRIu32
		" sw*sh: %"PRIu16"x%"PRIu16", w*h: %"PRIu16"x%"PRIu16" @ %"PRIu16
		",%"PRIu16" on len: %"PRIu32" expand to %"PRIu32,
		chid, type, sid, sw, sh, w, h, x, y, len, exp_len
	);

	memset(buf, '\0', CONTROL_PACKET_SIZE);
	pack_u64(last_seen, &buf[0]);
/* uint8_t entropy[8]; */
	buf[16] = chid; /* [16] : channel-id */
	buf[17] = COMMAND_VIDEOFRAME; /* [17] : command */
	pack_u32(0, &buf[18]); /* [18..21] : stream-id */
	buf[22] = type; /* [22] : type */
	pack_u16(sw, &buf[23]); /* [23..24] : surfacew */
	pack_u16(sh, &buf[25]); /* [25..26] : surfaceh */
	pack_u16(x, &buf[27]); /* [27..28] : startx */
	pack_u16(y, &buf[29]); /* [29..30] : starty */
	pack_u16(w, &buf[31]); /* [31..32] : framew */
	pack_u16(h, &buf[33]); /* [33..34] : frameh */
	pack_u32(len, &buf[36]); /* [36..39] : length */
	pack_u32(exp_len, &buf[40]); /* [40..43] : exp-length */

/* [35] : dataflags: uint8 */
/* [40] Commit on completion, this is always set right now but will change
 * when 'chain of deltas' mode for shmif is added */
	buf[44] = commit;
}

/*
 * Need to chunk up a binary stream that do not have intermediate headers, that
 * typically comes with the compression / h264 / ...  output. To avoid yet
 * another copy, we use the prepend mechanism in a12int_append_out.
 */
static void chunk_pack(struct a12_state* S,
	int type, uint8_t* buf, size_t buf_sz, size_t chunk_sz)
{
	size_t n_chunks = buf_sz / chunk_sz;

	uint8_t outb[a12int_header_size(type)];
	outb[0] = 0; /* [0] : channel id */
	pack_u32(0xbacabaca, &outb[1]); /* [1..4] : stream */
	pack_u16(chunk_sz, &outb[5]); /* [5..6] : length */

	for (size_t i = 0; i < n_chunks; i++){
		a12int_append_out(S, type, &buf[i * chunk_sz], chunk_sz, outb, sizeof(outb));
	}

	size_t left = buf_sz - n_chunks * chunk_sz;
	pack_u16(left, &outb[5]); /* [5..6] : length */
	if (left)
		a12int_append_out(S, type, &buf[n_chunks * chunk_sz], left, outb, sizeof(outb));
}

/*
 * the rgb565, rgb and rgba function all follow the same pattern
 */
void a12int_encode_rgb565(PACK_ARGS)
{
	size_t px_sz = 2;

/* calculate chunk sizes based on a fitting amount of pixels */
	size_t hdr_sz = a12int_header_size(STATE_VIDEO_PACKET);
	size_t ppb = (chunk_sz - hdr_sz) / px_sz;
	size_t bpb = ppb * px_sz;
	size_t blocks = w * h / ppb;

	shmif_pixel* inbuf = vb->buffer;
	size_t pos = y * vb->pitch + x;

/* get the packing buffer, cancel if oom */
	uint8_t* outb = malloc(hdr_sz + bpb);
	if (!outb)
		return;

/* store the control frame that defines our video buffer */
	uint8_t hdr_buf[CONTROL_PACKET_SIZE];
	a12int_vframehdr_build(hdr_buf, S->last_seen_seqnr, chid,
		POSTPROCESS_VIDEO_RGB565, 0, vb->w, vb->h, w, h, x, y,
		w * h * px_sz, w * h * px_sz, 1
	);
	a12int_append_out(S,
		STATE_CONTROL_PACKET, hdr_buf, CONTROL_PACKET_SIZE, NULL, 0);

	outb[0] = chid; /* [0] : channel id */
	pack_u32(0xbacabaca, &outb[1]); /* [1..4] : stream */
	pack_u16(bpb, &outb[5]); /* [5..6] : length */

/* sweep the incoming frame, and pack maximum block size */
	size_t row_len = w;
	for (size_t i = 0; i < blocks; i++){
		for (size_t j = 0; j < bpb; j += px_sz){
			uint8_t r, g, b, ign;
			uint16_t px;
			SHMIF_RGBA_DECOMP(inbuf[pos++], &r, &g, &b, &ign);
			px =
				(((b >> 3) & 0x1f) << 0) |
				(((g >> 2) & 0x3f) << 5) |
				(((r >> 3) & 0x1f) << 11)
			;
			pack_u16(px, &outb[hdr_sz+j]);
			row_len--;
			if (row_len == 0){
				pos += vb->pitch - w;
				row_len = w;
			}
		}
		a12int_append_out(S, STATE_VIDEO_PACKET, outb, hdr_sz + bpb, NULL, 0);
	}

/* last chunk */
	size_t left = ((w * h) - (blocks * ppb)) * px_sz;
	if (left){
		pack_u16(left, &outb[5]);
		debug_print(2, "small block of %zu bytes", left);
		for (size_t i = 0; i < left; i+= px_sz){
			uint8_t r, g, b, ign;
			uint16_t px;
			SHMIF_RGBA_DECOMP(inbuf[pos++], &r, &g, &b, &ign);
			px =
				(((b >> 3) & 0x1f) << 0) |
				(((g >> 2) & 0x3f) << 5) |
				(((r >> 3) & 0x1f) << 11)
			;
			pack_u16(px, &outb[hdr_sz+i]);
			row_len--;
			if (row_len == 0){
				pos += vb->pitch - w;
				row_len = w;
			}
		}
		a12int_append_out(S, STATE_VIDEO_PACKET, outb, left+hdr_sz, NULL, 0);
	}

	free(outb);
}

void a12int_encode_rgba(PACK_ARGS)
{
	size_t px_sz = 4;
	debug_print(2, "encode_rgba frame");

/* calculate chunk sizes based on a fitting amount of pixels */
	size_t hdr_sz = a12int_header_size(STATE_VIDEO_PACKET);
	size_t ppb = (chunk_sz - hdr_sz) / px_sz;
	size_t bpb = ppb * px_sz;
	size_t blocks = w * h / ppb;

	shmif_pixel* inbuf = vb->buffer;
	size_t pos = y * vb->pitch + x;

/* get the packing buffer, cancel if oom */
	uint8_t* outb = malloc(hdr_sz + bpb);
	if (!outb)
		return;

/* store the control frame that defines our video buffer */
	uint8_t hdr_buf[CONTROL_PACKET_SIZE];
	a12int_vframehdr_build(hdr_buf, S->last_seen_seqnr, chid,
		POSTPROCESS_VIDEO_RGBA, 0, vb->w, vb->h, w, h, x, y,
		w * h * px_sz, w * h * px_sz, 1
	);
	a12int_append_out(S,
		STATE_CONTROL_PACKET, hdr_buf, CONTROL_PACKET_SIZE, NULL, 0);

	outb[0] = chid; /* [0] : channel id */
	pack_u32(0xbacabaca, &outb[1]); /* [1..4] : stream */
	pack_u16(bpb, &outb[5]); /* [5..6] : length */

/* sweep the incoming frame, and pack maximum block size */
	size_t row_len = w;
	for (size_t i = 0; i < blocks; i++){
		for (size_t j = 0; j < bpb; j += px_sz){
			uint8_t* dst = &outb[hdr_sz+j];
			SHMIF_RGBA_DECOMP(inbuf[pos++], &dst[0], &dst[1], &dst[2], &dst[3]);
			row_len--;
			if (row_len == 0){
				pos += vb->pitch - w;
				row_len = w;
			}
		}

/* dispatch to out-queue(s) */
		a12int_append_out(S, STATE_VIDEO_PACKET, outb, hdr_sz + bpb, NULL, 0);
	}

/* last chunk */
	size_t left = ((w * h) - (blocks * ppb)) * px_sz;
	if (left){
		pack_u16(left, &outb[5]);
		debug_print(2, "small block of %zu bytes", left);
		for (size_t i = 0; i < left; i+= px_sz){
			uint8_t* dst = &outb[hdr_sz+i];
			SHMIF_RGBA_DECOMP(inbuf[pos++], &dst[0], &dst[1], &dst[2], &dst[3]);
			row_len--;
			if (row_len == 0){
				pos += vb->pitch - w;
				row_len = w;
			}
		}
		a12int_append_out(S, STATE_VIDEO_PACKET, outb, hdr_sz + left, NULL, 0);
	}

	free(outb);
}

void a12int_encode_rgb(PACK_ARGS)
{
	size_t px_sz = 3;
	debug_print(2, "encode_rgb frame");

/* calculate chunk sizes based on a fitting amount of pixels */
	size_t hdr_sz = a12int_header_size(STATE_VIDEO_PACKET);
	size_t ppb = (chunk_sz - hdr_sz) / px_sz;
	size_t bpb = ppb * px_sz;
	size_t blocks = w * h / ppb;

	shmif_pixel* inbuf = vb->buffer;
	size_t pos = y * vb->pitch + x;

/* get the packing buffer, cancel if oom */
	uint8_t* outb = malloc(hdr_sz + bpb);
	if (!outb)
		return;

/* store the control frame that defines our video buffer */
	uint8_t hdr_buf[CONTROL_PACKET_SIZE];
	a12int_vframehdr_build(hdr_buf, S->last_seen_seqnr, chid,
		POSTPROCESS_VIDEO_RGB, 0, vb->w, vb->h, w, h, x, y,
		w * h * px_sz, w * h * px_sz, 1
	);
	a12int_append_out(S,
		STATE_CONTROL_PACKET, hdr_buf, CONTROL_PACKET_SIZE, NULL, 0);

	outb[0] = chid; /* [0] : channel id */
	pack_u32(0xbacabaca, &outb[1]); /* [1..4] : stream */
	pack_u16(bpb, &outb[5]); /* [5..6] : length */

/* sweep the incoming frame, and pack maximum block size */
	size_t row_len = w;
	for (size_t i = 0; i < blocks; i++){
		for (size_t j = 0; j < bpb; j += px_sz){
			uint8_t ign;
			uint8_t* dst = &outb[hdr_sz+j];
			SHMIF_RGBA_DECOMP(inbuf[pos++], &dst[0], &dst[1], &dst[2], &ign);
			row_len--;
			if (row_len == 0){
				pos += vb->pitch - w;
				row_len = w;
			}
		}

/* dispatch to out-queue(s) */
		debug_print(2, "flush %zu bytes", hdr_sz + bpb);
		a12int_append_out(S, STATE_VIDEO_PACKET, outb, hdr_sz + bpb, NULL, 0);
	}

/* last chunk */
	size_t left = ((w * h) - (blocks * ppb)) * px_sz;
	if (left){
		pack_u16(left, &outb[5]);
/* sweep the incoming frame, and pack maximum block size */
		size_t row_len = w;
		for (size_t i = 0; i < blocks; i++){
			for (size_t j = 0; j < bpb; j += px_sz){
				uint8_t ign;
				uint8_t* dst = &outb[hdr_sz+j];
				SHMIF_RGBA_DECOMP(inbuf[pos++], &dst[0], &dst[1], &dst[2], &ign);
				row_len--;
				if (row_len == 0){
					pos += vb->pitch - w;
					row_len = w;
				}
/* dispatch to out-queue(s) */
			}

			a12int_append_out(S, STATE_VIDEO_PACKET, outb, hdr_sz + left, NULL, 0);
		}
	}

	free(outb);
}

struct compress_res {
	bool ok;
	uint8_t type;
	size_t out_sz;
	uint8_t* out_buf;
};

static struct compress_res compress_deltaz(struct a12_state* S, uint8_t ch,
	struct shmifsrv_vbuffer* vb, size_t* x, size_t* y, size_t* w, size_t* h)
{
	int type;
	uint8_t* compress_in;
	size_t compress_in_sz = 0;
	struct shmifsrv_vbuffer* ab = &S->channels[ch].acc;
	static int count = 0;

/* reset the accumulation buffer so that we rebuild the normal frame */
	if (ab->w != vb->w || ab->h != vb->h || count++ > 5){
		free(ab->buffer);
		free(S->channels[ch].compression);
		ab->buffer = NULL;
		S->channels[ch].compression = NULL;
		count = 0;
	}

/* first or reset run, build accumulation buffer and copy */
	if (!ab->buffer){
		type = POSTPROCESS_VIDEO_MINIZ;
		*ab = *vb;
		size_t nb = vb->w * vb->h * 3;
		ab->buffer = malloc(nb);
		*w = vb->w;
		*h = vb->h;
		*x = 0;
		*y = 0;
		debug_print(1, "dpng, switch to I frame (%zu, %zu)", *w, *h);

		if (!ab->buffer)
			return (struct compress_res){};

/* the compression buffer stores a ^ b, accumulation is a packed copy of the
 * contents of the previous input frame, this should provide a better basis for
 * deflates RLE etc. stages, but also act as an option for us to provide our
 * cheaper RLE or send out a raw- frame when the RLE didn't work out */
		S->channels[ch].compression = malloc(nb);
		compress_in_sz = nb;

		if (!S->channels[ch].compression){
			free(ab->buffer);
			ab->buffer = NULL;
			return (struct compress_res){};
		}

/* so accumulation buffer might be tightly packed while the source
 * buffer do not have to be, thus we need to iterate and do this copy */
		compress_in = (uint8_t*) ab->buffer;
		uint8_t* acc = compress_in;
		size_t ofs = 0;
		for (size_t y = 0; y < vb->h; y++){
			for (size_t x = 0; x < vb->w; x++){
				uint8_t ign;
				shmif_pixel px = vb->buffer[y*vb->pitch+x];
				SHMIF_RGBA_DECOMP(px, &acc[ofs], &acc[ofs+1], &acc[ofs+2], &ign);
				ofs += 3;
			}
		}
	}
/* We have a delta frame, use accumulation buffer as a way to calculate a ^ b
 * and store ^ b. For smaller regions, we might want to do something simpler
 * like RLE only. The flags (,0) can be derived with the _zip helper */
	else {
		debug_print(2, "dpng, delta frame");
		compress_in = S->channels[ch].compression;
		uint8_t* acc = (uint8_t*) ab->buffer;
		for (size_t cy = (*y); cy < (*y)+(*h); cy++){
			size_t rs = (cy * ab->w + (*x)) * 3;

			for (size_t cx = *x; cx < (*x)+(*w); cx++){
				uint8_t r, g, b, ign;
				shmif_pixel px = vb->buffer[cy * vb->pitch + cx];
				SHMIF_RGBA_DECOMP(px, &r, &g, &b, &ign);
				compress_in[compress_in_sz++] = acc[rs+0] ^ r;
				compress_in[compress_in_sz++] = acc[rs+1] ^ g;
				compress_in[compress_in_sz++] = acc[rs+2] ^ b;
				acc[rs+0] = r; acc[rs+1] = g; acc[rs+2] = b;
				rs += 3;
			}
		}
		type = POSTPROCESS_VIDEO_DMINIZ;
	}

	size_t out_sz;
#ifdef LOG_FRAME_OUTPUT
	static int count;
	char fn[26];
	snprintf(fn, 26, "deltaz_%d.png", count++);
	FILE* fpek = fopen(fn, "w");
	void* fbuf =
		tdefl_write_image_to_png_file_in_memory(compress_in, *w, *h, 3, &out_sz);
	fwrite(fbuf, out_sz, 1, fpek);
	fclose(fpek);
	free(fbuf);
#endif

	uint8_t* buf = tdefl_compress_mem_to_heap(
			compress_in, compress_in_sz, &out_sz, 0);

	return (struct compress_res){
		.type = type,
		.ok = buf != NULL,
		.out_buf = buf,
		.out_sz = out_sz
	};
}

void a12int_encode_dpng(PACK_ARGS)
{
	struct compress_res cres = compress_deltaz(S, chid, vb, &x, &y, &w, &h);
	if (!cres.ok)
		return;

	uint8_t hdr_buf[CONTROL_PACKET_SIZE];
	a12int_vframehdr_build(hdr_buf, S->last_seen_seqnr, chid,
		cres.type, 0, vb->w, vb->h, w, h, x, y,
		cres.out_sz, w * h * 3, 1
	);

	debug_print(2, "dpng (%d), in: %zu, out: %zu",
		cres.type, w * h * 3, cres.out_sz);

	a12int_append_out(S,
		STATE_CONTROL_PACKET, hdr_buf, CONTROL_PACKET_SIZE, NULL, 0);
	chunk_pack(S, STATE_VIDEO_PACKET, cres.out_buf, cres.out_sz, chunk_sz);

	free(cres.out_buf);
}
