/*
 * Copyright: 2017-2018, Björn Ståhl
 * Description: A12 protocol state machine
 * License: 3-Clause BSD, see COPYING file in arcan source repository.
 * Reference: https://arcan-fe.com
 */
#include <arcan_shmif.h>
#include <arcan_shmif_server.h>

#include "a12.h"
#include "blake2.h"
#include "pack.h"
#include <inttypes.h>
#include <string.h>
#include <math.h>

/* Only used for quick logging, for compression we use miniz we a
 * possible xor- delta prepass */
#ifdef LOG_FRAME_OUTPUT
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#include "../../engine/external/stb_image_write.h"
#endif

#define MAC_BLOCK_SZ 16
#define CONTROL_PACKET_SIZE 128
#ifndef DYNAMIC_FREE
#define DYNAMIC_FREE free
#endif

#ifndef DYNAMIC_MALLOC
#define DYNAMIC_MALLOC malloc
#endif

#ifndef DYNAMIC_REALLOC
#define DYNAMIC_REALLOC realloc
#endif

#ifdef _DEBUG
#define DEBUG 1
#else
#define DEBUG 0
#endif

#define SEQUENCE_NUMBER_SIZE 8

#ifndef debug_print
#define debug_print(fmt, ...) \
            do { if (DEBUG) fprintf(stderr, "%s:%d:%s(): " fmt "\n", \
						"a12:", __LINE__, __func__,##__VA_ARGS__); } while (0)
#endif

enum {
	STATE_NOPACKET = 0,
	STATE_CONTROL_PACKET = 1,
	STATE_EVENT_PACKET = 2,
	STATE_AUDIO_PACKET = 3,
	STATE_VIDEO_PACKET = 4,
	STATE_BLOB_PACKET = 5,
	STATE_BROKEN
};

static int header_sizes[] = {
	MAC_BLOCK_SZ + 1, /* NO packet, just MAC + outer header */
	CONTROL_PACKET_SIZE,
	0, /* EVENT size is filled in at first open */
	1 + 4 + 2, /* VIDEO partial: ch, stream, len */
	1 + 4 + 2, /* AUDIO partial: ch, stream, len */
	1 + 4 + 2, /* BINARY partial: ch, stream, len */
	0
};

enum control_commands {
	COMMAND_HELLO = 0,
	COMMAND_SHUTDOWN,
	COMMAND_ENCNEG,
	COMMAND_REKEY,
	COMMAND_CANCELSTREAM,
	COMMAND_NEWCH,
	COMMAND_FAILURE,
	COMMAND_VIDEOFRAME,
	COMMAND_AUDIOFRAME,
	COMMAND_BINARYSTREAM
};

enum video_postprocess_method {
	POSTPROCESS_VIDEO_NONE = 0,
	POSTPROCESS_VIDEO_RGB,
	POSTPROCESS_VIDEO_RGB565,
/*
 * _PNG
 * AV1
 */
/*
 * Xor and RLE. Row based. First two bits in every row sets start state:
 * 0 : raw full row
 * 1 : xor raw format
 * 2 : rle format
 */
	POSTPROCESS_VIDEO_XRLE,
	POSTPROCESS_VIDEO_H264
};

struct video_frame {
	uint32_t id;
	uint16_t sw, sh;
	uint16_t w, h;
	uint16_t x, y;
	uint32_t flags;
	uint8_t postprocess;
	uint8_t commit; /* finish after this transfer? */

	uint8_t* inbuf; /* decode buffer, not used for all modes */
	uint32_t inbuf_pos;
	uint32_t inbuf_sz; /* bytes-total counter */
 /* separation between input-frame buffer and
	* decompression postprocessing, avoid 'zip-bombing' */
	uint32_t expanded_sz;
	size_t row_left;
	/* bytes left on current row for raw-dec */
};

struct a12_state {
/* we need to prepend this when we build the next MAC */
	uint8_t last_mac_out[MAC_BLOCK_SZ];
	uint8_t last_mac_in[MAC_BLOCK_SZ];

/* data needed to synthesize the next package */
	uint64_t current_seqnr;
	uint64_t last_seen_seqnr;

/* populate and forwarded output buffer */
	size_t buf_sz[2];
	uint8_t* bufs[2];
	uint8_t buf_ind;
	size_t buf_ofs;

/* multiple- channels over the same state tracker for subsegment handling */
	struct {
		bool active;
		struct arcan_shmif_cont* cont;
		union {
			struct video_frame vframe;
		} unpack_state;
	} channels[256];
	int in_channel;

/*
 * incoming buffer
 */
	uint8_t decode[65536];
	uint16_t decode_pos;
	uint16_t left;
	uint8_t state;

/* overflow state tracking cookie */
	volatile uint32_t cookie;

/* built at initial setup, then copied out for every time we add data */
	blake2bp_state mac_init, mac_dec;

/* when the channel has switched to a streamcipher, this is set to true */
	bool in_encstate;

	uint32_t canary;
};

static uint8_t* grow_array(uint8_t* dst, size_t* cur_sz, size_t new_sz)
{
	if (new_sz < *cur_sz)
		return dst;

/* pick the nearest higher power of 2 for good measure */
	size_t pow = 1;
	while (pow < new_sz)
		pow *= 2;
	new_sz = pow;

	debug_print("grow outqueue %zu => %zu", *cur_sz, new_sz);
	uint8_t* res = DYNAMIC_REALLOC(dst, new_sz);
	if (!res){
		debug_print("couldn't grow queue");
		free(dst);
		*cur_sz = 0;
		return NULL;
	}

	*cur_sz = new_sz;
	return res;
}

static void step_sequence(struct a12_state* S, uint8_t* outb)
{
	pack_u64(S->current_seqnr++, outb);
/* DBEUG: replace sequence with 's' */
	for (size_t i = 0; i < 8; i++) outb[i] = 's';
}

/*
 * Used when a full byte buffer for a packet has been prepared, important
 * since it will also encrypt, generate MAC and add to buffer prestate.
 *
 * This is where more advanced and fair queueing should be made in order
 * to not have the bandwidth hungry channels (i.e. video) consume everything.
 *
 * Another issue is that the raw vframes are big and ugly, and here is the
 * place where we performa an unavoidable copy unless we want interleaving
 * (and then it becomes expensive to perform). Should be possible to set a
 * direct-to-drain descriptor here and do the write calls to the socket or
 * descriptor.
 */
static void append_outb(
	struct a12_state* S, uint8_t type, uint8_t* out, size_t out_sz)
{
/* this means we can just continue our happy stream-cipher and apply to our
 * outgoing data */
	if (S->in_encstate){
/* cipher_update(&S->out_cstream, out, out_sz) */
	}

/* begin a new MAC, chained on our previous one
	blake2bp_state mac_state = S->mac_init;
	blake2bp_update(&mac_state, S->last_mac_out, MAC_BLOCK_SZ);
	blake2bp_update(&mac_state, &type, 1);
	blake2bp_update(&mac_state, out, out_sz);
 */

/* grow write buffer if the block doesn't fit */
	size_t required = S->buf_ofs + MAC_BLOCK_SZ + out_sz + 1;

	S->bufs[S->buf_ind] = grow_array(
		S->bufs[S->buf_ind],
		&S->buf_sz[S->buf_ind],
		required
	);

/* and if that didn't work, fatal */
	if (S->buf_sz[S->buf_ind] < required){
		debug_print("realloc failed: size (%zu) vs required (%zu)",
			S->buf_sz[S->buf_ind], required);

		S->state = STATE_BROKEN;
		return;
	}
	uint8_t* dst = S->bufs[S->buf_ind];

/* prepend MAC
	blake2bp_final(&mac_state, S->last_mac_out, MAC_BLOCK_SZ);
	memcpy(&dst[S->buf_ofs], S->last_mac_out, MAC_BLOCK_SZ);
 */

/* DEBUG: replace mac with 'm' */
	for (size_t i = 0; i < MAC_BLOCK_SZ; i++)
		dst[i] = 'm';
	S->buf_ofs += MAC_BLOCK_SZ;

/* our packet type */
	dst[S->buf_ofs++] = type;

/* and our data block, this costs us an extra copy which isn't very nice -
 * might want to set a target descriptor immediately for some uses here,
 * the problem is proper interleaving of packets while respecting kernel
 * buffer behavior */
	memcpy(&dst[S->buf_ofs], out, out_sz);
	S->buf_ofs += out_sz;
	debug_print("added %zu bytes, @%zu", out_sz, S->buf_ofs);
}

static void reset_state(struct a12_state* S)
{
	S->left = header_sizes[STATE_NOPACKET];
	S->state = STATE_NOPACKET;
	S->decode_pos = 0;
	S->in_channel = -1;
	S->mac_dec = S->mac_init;
}

static struct a12_state*
a12_setup(uint8_t* authk, size_t authk_sz)
{
	struct a12_state* res = DYNAMIC_MALLOC(sizeof(struct a12_state));
	if (!res)
		return NULL;

	*res = (struct a12_state){};
	if (-1 == blake2bp_init_key(&res->mac_init, 16, authk, authk_sz)){
		DYNAMIC_FREE(res);
		return NULL;
	}

	res->cookie = 0xfeedface;
	return res;
}

static void a12_init()
{
	static bool init;
	if (init)
		return;

/* make one nonsense- call first to figure out the current packing size */
	uint8_t outb[512];
	ssize_t evsz = arcan_shmif_eventpack(
		&(struct arcan_event){.category = EVENT_IO}, outb, 512);

	header_sizes[STATE_EVENT_PACKET] = evsz + SEQUENCE_NUMBER_SIZE;
	init = true;
}

struct a12_state*
a12_channel_build(uint8_t* authk, size_t authk_sz)
{
	a12_init();

	struct a12_state* res = a12_setup(authk, authk_sz);
	if (!res)
		return NULL;

	return res;
}

struct a12_state* a12_channel_open(uint8_t* authk, size_t authk_sz)
{
	a12_init();

	struct a12_state* S = a12_setup(authk, authk_sz);
	if (!S)
		return NULL;

/* send initial hello packet */
	uint8_t outb[CONTROL_PACKET_SIZE] = {0};
	step_sequence(S, outb);

/* DEBUG: replace control with 'c' */
	for (size_t i = 8; i < CONTROL_PACKET_SIZE; i++)
		outb[i] = 'c';

	outb[17] = 0;

	debug_print("channel open, add control packet");
	append_outb(S, STATE_CONTROL_PACKET, outb, CONTROL_PACKET_SIZE);

	return S;
}

void
a12_channel_close(struct a12_state* S)
{
	if (!S || S->cookie != 0xfeedface)
		return;

	DYNAMIC_FREE(S->bufs[0]);
	DYNAMIC_FREE(S->bufs[1]);
	*S = (struct a12_state){};
	S->cookie = 0xdeadbeef;

	DYNAMIC_FREE(S);
}

/*
 * NOPACKET:
 * MAC
 * command byte
 */
static void process_nopacket(struct a12_state* S)
{
/* only work with a whole packet */
	if (S->left > 0)
		return;

	S->mac_dec = S->mac_init;
/* MAC(MAC_IN | KEY)
	blake2bp_update(&S->mac_dec, S->last_mac_in, MAC_BLOCK_SZ);
 */

/* save last known MAC for later comparison */
	memcpy(S->last_mac_in, S->decode, MAC_BLOCK_SZ);

/* CRYPTO-fixme: if we are in stream cipher mode, decode just the one byte
 * blake2bp_update(&S->mac_dec, &S->decode[MAC_BLOCK_SZ], 1); */

	S->state = S->decode[MAC_BLOCK_SZ];

	if (S->state >= STATE_BROKEN){
		debug_print("channel broken, unknown command val: %"PRIu8, S->state);
		S->state = STATE_BROKEN;
		return;
	}

	debug_print("left: %"PRIu16", state: %"PRIu8, S->left, S->state);
	S->left = header_sizes[S->state];
	S->decode_pos = 0;
}

static bool process_mac(struct a12_state* S)
{
	uint8_t final_mac[MAC_BLOCK_SZ];

/*
 * Authentication is on the todo when everything is not in flux and we can
 * do the full verification & validation process
 */
	return true;

	blake2bp_update(&S->mac_dec, S->decode, S->decode_pos);
	blake2bp_final(&S->mac_dec, final_mac, MAC_BLOCK_SZ);

/* Option to continue with broken authentication, ... */
	if (memcmp(final_mac, S->last_mac_in, MAC_BLOCK_SZ) != 0){
		debug_print("authentication mismatch on packet \n");
		S->state = STATE_BROKEN;
		return false;
	}

	debug_print("authenticated packet");
	return true;
}

static void command_videoframe(struct a12_state* S)
{
	uint8_t channel = S->decode[16];
	struct video_frame* vframe = &S->channels[channel].unpack_state.vframe;
 /* new vstream, from README.md:
	* currently unused
	* [36    ] : dataflags: uint8
	*/
/* [18..21] : stream-id: uint32 */
	vframe->postprocess = S->decode[22]; /* [22] : format : uint8 */
/* [23..24] : surfacew: uint16
 * [25..26] : surfaceh: uint16 */
	unpack_u16(&vframe->sw, &S->decode[23]);
	unpack_u16(&vframe->sh, &S->decode[25]);
/* [27..28] : startx: uint16 (0..outw-1)
 * [29..30] : starty: uint16 (0..outh-1) */
	unpack_u16(&vframe->x, &S->decode[27]);
	unpack_u16(&vframe->y, &S->decode[29]);
/* [31..32] : framew: uint16 (outw-startx + framew < outw)
 * [33..34] : frameh: uint16 (outh-starty + frameh < outh) */
	unpack_u16(&vframe->w, &S->decode[31]);
	unpack_u16(&vframe->h, &S->decode[33]);
/* [35] : dataflags */
	unpack_u32(&vframe->inbuf_sz, &S->decode[36]);
/* [41]     : commit: uint8 */
	unpack_u32(&vframe->expanded_sz, &S->decode[40]);
	vframe->commit = S->decode[44];

	S->in_channel = -1;

/* If channel set, apply resize immediately - synch cost should be offset with
 * the buffering being performed at lower layers. Right now the rejection of a
 * resize is not being forwarded, which can cause problems in some edge cases
 * where the WM have artificially restricted the size of a client window etc. */
	struct arcan_shmif_cont* cont = S->channels[channel].cont;
	if (!cont){
		debug_print("no segment mapped on channel");
		vframe->commit = 255;
		return;
	}

	if (vframe->sw != cont->w || vframe->sh != cont->h){
		arcan_shmif_resize(cont, vframe->sw, vframe->sh);
		if (vframe->sw != cont->w || vframe->sh != cont->h){
			debug_print("parent size rejected");
			vframe->commit = 255;
		}
		else
			debug_print("resized segment to %"PRIu32",%"PRIu32, vframe->sw, vframe->sh);
	}

	debug_print("new vframe (%d): %"PRIu16"*%"
		PRIu16"@%"PRIu16",%"PRIu16"+%"PRIu16",%"PRIu16,
		vframe->postprocess,
		vframe->sw, vframe->sh, vframe->x, vframe->y, vframe->w, vframe->h
	);

/* Validation is done just above, making sure the sub- region doesn't extend
 * the specified source surface. Remaining length- field is verified before
 * the write in process_video. */

/* For RAW pixels, note that we count row, pos, etc. in the native
 * shmif_pixel and thus use pitch instead of stride */
	if (vframe->postprocess == POSTPROCESS_VIDEO_NONE ||
		vframe->postprocess == POSTPROCESS_VIDEO_RGB565 ||
		vframe->postprocess == POSTPROCESS_VIDEO_RGB){
		vframe->row_left = vframe->w;
		vframe->inbuf_pos = vframe->y * cont->pitch + vframe->x;
		debug_print(
			"row-length: %zu at buffer pos %"PRIu32, vframe->row_left, vframe->inbuf_pos);
	}
	else {
		debug_print("unhandled vframe method: %"PRIu8, vframe->postprocess);
	}
}

/*
 * Control command,
 * current MAC calculation in s->mac_dec
 */
static void process_control(struct a12_state* S)
{
	if (!process_mac(S))
		return;

/* ignore these two for now
	uint64_t last_seen;
	uint8_t entropy[8];
 */

	uint8_t command = S->decode[17];

	switch(command){
	case COMMAND_HELLO:
		debug_print("HELO");
	break;
	case COMMAND_SHUTDOWN: break;
	case COMMAND_ENCNEG: break;
	case COMMAND_REKEY: break;
	case COMMAND_CANCELSTREAM: break;
	case COMMAND_NEWCH: break;
	case COMMAND_FAILURE: break;
	case COMMAND_VIDEOFRAME:
		command_videoframe(S);
	break;
	case COMMAND_AUDIOFRAME: break;
	case COMMAND_BINARYSTREAM: break;
	default:
		debug_print("unhandled control message");
	break;
	}

	debug_print("decode control packet");
	reset_state(S);
}

static void process_event(struct a12_state* S,
	void* tag, void (*on_event)(int chid, struct arcan_event*, void*))
{
	if (!process_mac(S))
		return;

/* crypto-FIXME: apply stream-cipher */
/* serialization-FIXME: proper unpack uint64_t */
	struct arcan_event aev;
	unpack_u64(&S->last_seen_seqnr, S->decode);
	if (-1 == arcan_shmif_eventunpack(
		&S->decode[SEQUENCE_NUMBER_SIZE], S->decode_pos - SEQUENCE_NUMBER_SIZE, &aev)){
		debug_print("broken event packet received");
	}
	else if (on_event)
		on_event(0, &aev, tag);

	reset_state(S);
}

/*
 * We have an incoming video packet, first we need to match it to the channel
 * that it represents (as we might get interleaved updates) and match the state
 * we are building.  With real MAC, the return->reenter loop is wrong.
 */
static void process_video(struct a12_state* S)
{
	debug_print("incoming video frame (ch: %d)", S->in_channel);
	if (!process_mac(S))
		return;

/* in_channel is used to track if we are waiting for the header or not */
	if (S->in_channel == -1){
		uint32_t stream;
		S->in_channel = S->decode[0];
		unpack_u32(&stream, &S->decode[1]);
		unpack_u16(&S->left, &S->decode[5]);
		S->decode_pos = 0;
		debug_print("video[%d:%"PRIx32"], left: %"PRIu16, S->in_channel, stream, S->left);
		return;
	}

/* the 'video_frame' structure for the current channel (segment) tracks
 * decode buffer etc. for the current stream */
	struct video_frame* cvf = &S->channels[S->in_channel].unpack_state.vframe;
	if (!S->channels[S->in_channel].cont){
		debug_print("data on unmapped channel");
		reset_state(S);
		return;
	}

/* postprocessing that requires an intermediate decode buffer before pushing */
	if (cvf->postprocess == POSTPROCESS_VIDEO_H264){
		size_t left = cvf->inbuf_sz - cvf->inbuf_pos;
		debug_print("compressed video-frame left: %zu", left);

		if (left >= S->decode_pos){
			memcpy(&cvf->inbuf[cvf->inbuf_pos], S->decode, S->decode_pos);
			cvf->inbuf_pos += S->decode_pos;
			left -= S->decode_pos;

			if (cvf->inbuf_sz == cvf->inbuf_pos){
				debug_print("decode-buffer size reached");
			}
		}
		else if (left != 0){
			debug_print("overflow, stream length and packet size mismatch");
		}

		if (left == 0){
			debug_print("finished, decode");
		}

		reset_state(S);
		return;
	}

/* if we are in discard state, just continue */
	if (cvf->commit == 255){
		debug_print("discard state, ignore video");
		reset_state(S);
		return;
	}

	if (cvf->inbuf_sz < S->decode_pos){
		debug_print("mischevios client, byte count mismatch");
		reset_state(S);
		return;
	}

/* raw frame types, the implementations and variations are so small that
 * we can just do it here - no need for the more complex stages like for
 * 264, ... */
	struct arcan_shmif_cont* cont = S->channels[S->in_channel].cont;
	if (cvf->postprocess == POSTPROCESS_VIDEO_NONE){
		debug_print("postprocess(raw@4): %"PRIu16, S->decode_pos);
		for (size_t i = 0; i < S->decode_pos; i += 4){
			cont->vidp[cvf->inbuf_pos++] = SHMIF_RGBA(
				S->decode[i+0], S->decode[i+1], S->decode[i+2], S->decode[i+3]);
			cvf->row_left--;
			if (cvf->row_left == 0){
				cvf->inbuf_pos -= cvf->w;
				cvf->inbuf_pos += cont->pitch;
				cvf->row_left = cvf->w;
			}
		}
	}
	else if (cvf->postprocess == POSTPROCESS_VIDEO_RGB){
		for (size_t i = 0; i < S->decode_pos; i += 3){
			cont->vidp[cvf->inbuf_pos++] = SHMIF_RGBA(
				S->decode[i+0], S->decode[i+1], S->decode[i+2], 0xff);
			cvf->row_left--;
			if (cvf->row_left == 0){
				cvf->inbuf_pos -= cvf->w;
				cvf->inbuf_pos += cont->pitch;
				cvf->row_left = cvf->w;
			}
		}
	}

	else if (cvf->postprocess == POSTPROCESS_VIDEO_RGB565){
		static const uint8_t rgb565_lut5[] = {
			0,   8,  16,  25,  33,  41,  49,  58,  66,   74,  82,  90,  99, 107,
			115,123, 132, 140, 148, 156, 165, 173, 181, 189,  197, 206, 214, 222,
			230, 239, 247,255
		};

		static const uint8_t rgb565_lut6[] = { 0,   4,   8,  12,  16,  20,  24,
			28,  32,  36,  40,  45,  49,  53,  57, 61, 65,  69,  73,  77,  81,  85,
			89,  93,  97, 101, 105, 109, 113, 117, 121, 125, 130, 134, 138, 142, 146,
			150, 154, 158, 162, 166, 170, 174, 178, 182, 186, 190, 194, 198, 202,
			206, 210, 215, 219, 223, 227, 231, 235, 239, 243, 247, 251, 255 };

		for (size_t i = 0; i < S->decode_pos; i += 2){
			uint16_t px;
			unpack_u16(&px, &S->decode[i]);
			cont->vidp[cvf->inbuf_pos++] =
				SHMIF_RGBA(
					rgb565_lut5[ (px & 0xf800) >> 11],
					rgb565_lut6[ (px & 0x07e0) >>  5],
					rgb565_lut5[ (px & 0x001f)      ],
					0xff
				);
			cvf->row_left--;
			if (cvf->row_left == 0){
				cvf->inbuf_pos -= cvf->w;
				cvf->inbuf_pos += cont->pitch;
				cvf->row_left = cvf->w;
			}
		}
	}

	cvf->inbuf_sz -= S->decode_pos;
	if (cvf->inbuf_sz == 0){
		debug_print("video frame completed, commit:%"PRIu8, cvf->commit);
		if (cvf->commit){
			arcan_shmif_signal(cont, SHMIF_SIGVID);
		}
	}
	else {
		debug_print("video buffer left: %"PRIu32, cvf->inbuf_sz);
	}

	reset_state(S);
}

static void process_audio(struct a12_state* S)
{
/* FIXME: header-stage then frame-stage, decode into context based on channel */
}

static void process_binary(struct a12_state* S)
{
/* FIXME: forward as descriptor and stream into/out from it, or dump to memtemp
 * and read full first */
}

void a12_set_destination(struct a12_state* S, struct arcan_shmif_cont* wnd, int chid)
{
	if (!S){
		debug_print("invalid set_destination call");
		return;
	}

	if (chid != 0){
		debug_print("multi-channel support unfinished");
		return;
	}

	S->channels[0].cont = wnd;
	S->channels[0].active = false;
}

void
a12_channel_unpack(struct a12_state* S,
	const uint8_t* buf, size_t buf_sz,
	void* tag, void (*on_event)(int chid, struct arcan_event*, void*))
{
	if (S->state == STATE_BROKEN)
		return;

/* Unknown state? then we're back waiting for a command packet */
	if (S->left == 0)
		reset_state(S);

/* iteratively flush, we tail- recurse should the need arise, optimization
 * here would be to forward buf immediately if it fits - saves a copy */
	size_t ntr = buf_sz > S->left ? S->left : buf_sz;

	memcpy(&S->decode[S->decode_pos], buf, ntr);
	S->left -= ntr;
	S->decode_pos += ntr;
	buf_sz -= ntr;

/* do we need to buffer more? */
	if (S->left)
		return;

/* otherwise dispatch based on state */
	switch(S->state){
	case STATE_NOPACKET:
		process_nopacket(S);
	break;
	case STATE_CONTROL_PACKET:
		process_control(S);
	break;
	case STATE_VIDEO_PACKET:
		process_video(S);
	break;
	case STATE_AUDIO_PACKET:
		process_audio(S);
	break;
	case STATE_EVENT_PACKET:
		process_event(S, tag, on_event);
	break;
	default:
		debug_print("unknown state");
		S->state = STATE_BROKEN;
		return;
	break;
	}

/* slide window and tail- if needed */
	if (buf_sz)
		a12_channel_unpack(S, &buf[ntr], buf_sz, tag, on_event);
}

size_t
a12_channel_flush(struct a12_state* S, uint8_t** buf)
{
	if (S->buf_ofs == 0 || S->state == STATE_BROKEN || S->cookie != 0xfeedface)
		return 0;

	size_t rv = S->buf_ofs;

/* switch out "output buffer" and return how much there is to send, it is
 * expected that by the next non-0 returning channel_flush, its contents have
 * been pushed to the other side */
	*buf = S->bufs[S->buf_ind];
	S->buf_ofs = 0;
	S->buf_ind = (S->buf_ind + 1) % 2;

	return rv;
}

int
a12_channel_poll(struct a12_state* S)
{
	if (!S || S->state == STATE_BROKEN || S->cookie != 0xfeedface)
		return -1;

	return S->left > 0 ? 1 : 0;
}

/*
 * Slice up the input region as fixed-size packets of up to slice_sz and
 * append to the output buffer
 */
static void pack_rgba_region(struct a12_state* S,
	size_t x, size_t y, size_t w, size_t h,
	struct shmifsrv_vbuffer* vb, size_t slice_sz)
{
	slice_sz = slice_sz > 65535 ? 65535 : slice_sz;
	size_t px_sz = 4;
	size_t hdr = header_sizes[STATE_VIDEO_PACKET];
	size_t ppb = (slice_sz - hdr) / px_sz;
	size_t bpb = ppb * px_sz;
	size_t blocks = w * h / ppb;

	shmif_pixel* inbuf = vb->buffer;
	size_t pos = y * vb->pitch + x;

	uint8_t outb[hdr + bpb];
	outb[0] = 0; /* [0] : channel id */
	pack_u32(0xbacabaca, &outb[1]); /* [1..4] : stream */
	pack_u16(bpb, &outb[5]); /* [5..6] : length */

	debug_print("source-buffer: %zu*%zu, pitch:%zu, stride: %zu",
		vb->w, vb->h, vb->stride, vb->pitch);

	debug_print("split frame into %zu/%zu at px-ofs %zu", blocks, bpb, pos);

/* define to test/log compression and buffering */
#ifdef LOG_FRAME_OUTPUT
	static size_t counter;
	static size_t nb_raw;
	char newfn[24];
	sprintf(newfn, "seq_%zu.png", counter++);
	char* tmpbuf = malloc(w * h * 4);
	char* bufpos = tmpbuf;
#endif

	size_t row_len = w;
/* aligned chunk sizes */
	for (size_t i = 0; i < blocks; i++){
		for (size_t j = 0; j < bpb; j += px_sz){
			uint8_t* dst = &outb[hdr+j];
			SHMIF_RGBA_DECOMP(inbuf[pos++], &dst[0], &dst[1], &dst[2], &dst[3]);
#ifdef LOG_FRAME_OUTPUT
			bufpos[0] = dst[0]; bufpos[1] = dst[1];
			bufpos[2] = dst[2]; bufpos[3] = dst[3];
			bufpos += 4;
#endif

/* source buffer might not be tightly packed or we have a subregion */
			row_len--;
			if (row_len == 0){
				pos += vb->pitch - w;
				row_len = w;
			}
		}

/* possible point to 'yield' unless we go with the 'reorder in queue' */
		debug_print("frame %zu / %zu", i, blocks);
		append_outb(S, STATE_VIDEO_PACKET, outb, hdr + bpb);
	}

/* last chunk */
	size_t left = ((w * h) - (blocks * ppb)) * px_sz;

	if (left){
		pack_u16(left, &outb[5]);
		debug_print("small block of %zu bytes", left);
		for (size_t i = 0; i < left; i+= px_sz){
			uint8_t* dst = &outb[hdr+i];
			SHMIF_RGBA_DECOMP(inbuf[pos++], &dst[0], &dst[1], &dst[2], &dst[3]);
#ifdef LOG_FRAME_OUTPUT
			bufpos[0] = dst[0]; bufpos[1] = dst[1];
			bufpos[2] = dst[2]; bufpos[3] = dst[3];
			bufpos += 4;
#endif

			row_len--;
			if (row_len == 0){
				pos += vb->pitch - w;
				row_len = w;
			}
		}
		append_outb(S, STATE_VIDEO_PACKET, outb, left+hdr);
	}

#ifdef LOG_FRAME_OUTPUT
	static size_t nb_compressed;
	stbi_write_png(newfn, w, h, 4, tmpbuf, 0);
	struct stat sbuf;
	stat(newfn, &sbuf);
	nb_compressed += sbuf.st_size;
	nb_raw += w * h * 4;
	debug_print("compressed_total: %zu, raw_total: %zu", nb_compressed, nb_raw);
	free(tmpbuf);
#endif
}

/* stripped version of pack_rgba, see that one for comments and synch
 * here in the event of any relevant modifications */
static void pack_rgb_region(
	struct a12_state* S, size_t x, size_t y, size_t w, size_t h,
	struct shmifsrv_vbuffer* vb, size_t slice_sz)
{
	slice_sz = slice_sz > 65535 ? 65535 : slice_sz;
	size_t px_sz = 3;
	size_t hdr = header_sizes[STATE_VIDEO_PACKET];
	size_t ppb = (slice_sz - hdr) / px_sz;
	size_t bpb = ppb * px_sz;
	size_t blocks = w * h / ppb;

	shmif_pixel* inbuf = vb->buffer;
	size_t pos = y * vb->pitch + x;

	uint8_t outb[hdr + bpb];
	outb[0] = 0; /* [0] : channel id */
	pack_u32(0xbacabaca, &outb[1]); /* [1..4] : stream */
	pack_u16(bpb, &outb[5]); /* [5..6] : length */

	size_t row_len = w;
	for (size_t i = 0; i < blocks; i++){
		for (size_t j = 0; j < bpb; j += px_sz){
			uint8_t* dst = &outb[hdr+j];
			uint8_t ign;
			SHMIF_RGBA_DECOMP(inbuf[pos++], &dst[0], &dst[1], &dst[2], &ign);
			row_len--;
			if (row_len == 0){
				pos += vb->pitch - w;
				row_len = w;
			}
		}
		append_outb(S, STATE_VIDEO_PACKET, outb, hdr + bpb);
	}

/* last chunk */
	size_t left = ((w * h) - (blocks * ppb)) * px_sz;

	if (left){
		pack_u16(left, &outb[5]);
		debug_print("small block of %zu bytes", left);
		for (size_t i = 0; i < left; i+= px_sz){
			uint8_t* dst = &outb[hdr+i];
			uint8_t ign;
			SHMIF_RGBA_DECOMP(inbuf[pos++], &dst[0], &dst[1], &dst[2], &ign);
			row_len--;
			if (row_len == 0){
				pos += vb->pitch - w;
				row_len = w;
			}
		}
		append_outb(S, STATE_VIDEO_PACKET, outb, left+hdr);
	}
}
static void pack_rgb565_region(
	struct a12_state* S, size_t x, size_t y, size_t w, size_t h,
	struct shmifsrv_vbuffer* vb, size_t slice_sz)
{
	slice_sz = slice_sz > 65535 ? 65535 : slice_sz;
	size_t px_sz = 2;
	size_t hdr = header_sizes[STATE_VIDEO_PACKET];
	size_t ppb = (slice_sz - hdr) / px_sz;
	size_t bpb = ppb * px_sz;
	size_t blocks = w * h / ppb;

	shmif_pixel* inbuf = vb->buffer;
	size_t pos = y * vb->pitch + x;

	uint8_t outb[hdr + bpb];
	outb[0] = 0; /* [0] : channel id */
	pack_u32(0xbacabaca, &outb[1]); /* [1..4] : stream */
	pack_u16(bpb, &outb[5]); /* [5..6] : length */

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
			pack_u16(px, &outb[hdr+j]);
			row_len--;
			if (row_len == 0){
				pos += vb->pitch - w;
				row_len = w;
			}
		}
		append_outb(S, STATE_VIDEO_PACKET, outb, hdr + bpb);
	}

/* last chunk */
	size_t left = ((w * h) - (blocks * ppb)) * px_sz;

	if (left){
		pack_u16(left, &outb[5]);
		debug_print("small block of %zu bytes", left);
		for (size_t i = 0; i < left; i+= px_sz){
			uint8_t r, g, b, ign;
			uint16_t px;
			SHMIF_RGBA_DECOMP(inbuf[pos++], &r, &g, &b, &ign);
			px =
				(((b >> 3) & 0x1f) << 0) |
				(((g >> 2) & 0x3f) << 5) |
				(((r >> 3) & 0x1f) << 11)
			;
			pack_u16(px, &outb[hdr+i]);
			row_len--;
			if (row_len == 0){
				pos += vb->pitch - w;
				row_len = w;
			}
		}
		append_outb(S, STATE_VIDEO_PACKET, outb, left+hdr);
	}
}

void
a12_channel_vframe(struct a12_state* S,
	uint8_t chid, struct shmifsrv_vbuffer* vb, struct a12_vframe_opts opts)
{
	if (!S || S->cookie != 0xfeedface || S->state == STATE_BROKEN)
		return;

/* use a fix size now as the outb- writer lacks queueing and interleaving */
	size_t chunk_sz = 32768;

/* avoid dumb updates */
	size_t x = 0, y = 0, w = vb->w, h = vb->h;
	if (vb->flags.subregion){
		x = vb->region.x1;
		y = vb->region.y1;
		w = vb->region.x2 - x;
		h = vb->region.y2 - y;
	}

/* sanity check against a dumb client here as well */
	if (x + w > vb->w || y + h > vb->h){
		debug_print("client provided bad/broken subregion");
		x = 0;
		y = 0;
		w = vb->w;
		h = vb->h;
	}

/* option: determine region delta, quick xor and early out - protects
 * against buggy clients sending updates even if there are none, not
 * uncommon with retro- like games various toolkits and 3D clients */

/* option: n-px splitting planes down xor buffer into regions? cuts
 * down on memory bandwidth versus RLEing */

/* dealing with each flag:
 * origo_ll - do the coversion in our own encode- stage
 * ignore_alpha - set pxfmt to 3
 * subregion - feed as information to the delta encoder
 * srgb - info to encoder, other leave be
 * vpts - possibly add as feedback to a scheduler and if there is
 *        near-deadline data, send that first or if it has expired,
 *        drop the frame. This is only a real target for game/decode
 *        but that decision can be pushed to the caller.
 *
 * then we have the problem of the meta- area
 */

	uint8_t buf[CONTROL_PACKET_SIZE] = {0};
	pack_u64(S->last_seen_seqnr, &buf[0]);
/* uint8_t entropy[8]; */
/* [16] : channel */
	buf[17] = COMMAND_VIDEOFRAME; /* [17] : command */
	pack_u32(0, &buf[18]); /* [18..21] : stream-id */
/* [22] : format - unused */
	pack_u16(vb->w, &buf[23]); /* [23..24] : surfacew */
	pack_u16(vb->h, &buf[25]); /* [25..26] : surfaceh */
	pack_u16(x, &buf[27]); /* [27..28] : startx */
	pack_u16(y, &buf[29]); /* [29..30] : starty */
	pack_u16(w, &buf[31]); /* [31..32] : framew */
	pack_u16(h, &buf[33]); /* [33..34] : frameh */
/* [35] : dataflags: uint8 */
/* [40] Commit on completion, this is always set right now but will change
 * when 'chain of deltas' mode for shmif is added */
	buf[44] = 1;

	debug_print("out vframe: %zu*%zu @0,0+%zu,%zu", vb->w, vb->h, vb->w, vb->h);

/* RGB888x */
	if (opts.rgb565) {
		pack_u32(w * h * 2, &buf[36]); /* [36..39] : wrong if */
		pack_u32(w * h * 2, &buf[40]); /* [36..39] : expanded sz */
		buf[22] = POSTPROCESS_VIDEO_RGB565;
		append_outb(S, STATE_CONTROL_PACKET, buf, CONTROL_PACKET_SIZE);
		pack_rgb565_region(S, x, y, w, h, vb, chunk_sz);
	}
	else if (vb->flags.ignore_alpha || opts.skip_alpha){
		pack_u32(w * h * 3, &buf[36]); /* [36..39] : wrong if */
		pack_u32(w * h * 3, &buf[40]); /* [36..39] : expanded sz */
		buf[22] = POSTPROCESS_VIDEO_RGB;
		append_outb(S, STATE_CONTROL_PACKET, buf, CONTROL_PACKET_SIZE);
		pack_rgb_region(S, x, y, w, h, vb, chunk_sz);
	}
	else {
		pack_u32(w * h * 4, &buf[36]); /* [36..39] : wrong if */
		pack_u32(w * h * 4, &buf[40]); /* [36..39] : expanded sz */
		append_outb(S, STATE_CONTROL_PACKET, buf, CONTROL_PACKET_SIZE);
		pack_rgba_region(S, x, y, w, h, vb, chunk_sz);
	}
}

void
a12_channel_enqueue(struct a12_state* S, struct arcan_event* ev)
{
	if (!S || S->cookie != 0xfeedface || !ev)
		return;

/* ignore descriptor- passing events for the time being as they add
 * queueing requirements, possibly compression and so on */
	if (arcan_shmif_descrevent(ev)){
		char msg[512];
		struct arcan_event aev = *ev;
		debug_print("ignoring descriptor event: %s",
			arcan_shmif_eventstr(&aev, msg, 512));

		return;
	}

/*
 * MAC and cipher state is managed in the append-outb stage
 */
	uint8_t outb[header_sizes[STATE_EVENT_PACKET]];
	step_sequence(S, outb);

	ssize_t step = arcan_shmif_eventpack(
		ev, &outb[SEQUENCE_NUMBER_SIZE], sizeof(outb) - SEQUENCE_NUMBER_SIZE);
	if (-1 == step)
		return;

/*
 * DEBUG: replace with 'e'
	for (size_t i = SEQUENCE_NUMBER_SIZE; i < step + SEQUENCE_NUMBER_SIZE; i++)
		outb[i] = 'e';
 */

	append_outb(S, STATE_EVENT_PACKET, outb, step + SEQUENCE_NUMBER_SIZE);
	debug_print("enqueue event %s", arcan_shmif_eventstr(ev, NULL, 0));
}
