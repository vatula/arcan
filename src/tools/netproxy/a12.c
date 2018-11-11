/*
 * Work in progress a12- state machine
 *
 * [x] normal event transfer
 * [ ] uncompressed video transfer
 * [ ] video compression
 * [ ] uncompressed audio transfer
 * [ ] aux. binary transfer
 * [ ] audio compression
 * [ ] compression parameter controls
 * [ ] proper MAC / stream cipher
 * [ ] curve25519 management
 * [ ] progressive multiplexed transfer
 * [ ] multithreaded progressive transfer
 */

#include <arcan_shmif.h>
#include <arcan_shmif_server.h>

#include "a12.h"
#include "blake2.h"
#include "pack.h"
#include <inttypes.h>
#include <string.h>

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

#define DECODE_BUFFER_CAP 9000

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
	1 + 8 + 4, /* VIDEO partial: ch, stream, len */
	1 + 8 + 4, /* AUDIO partial: ch, stream, len */
	1 + 8 + 4, /* BINARY partial: ch, stream, len */
	0
};

enum video_postprocess_method {
	POSTPROCESS_VIDEO_NONE,
	POSTPROCESS_VIDEO_X264
};

struct video_frame {
	uint32_t id;
	size_t w, h;
	size_t x, y;
	uint32_t flags;
	int postprocess;

	uint8_t* inbuf;
	size_t inbuf_sz;
	size_t inbuf_pos;
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

/* fixed size incoming buffer as either the compressed decoders need to buffer
 * and request output store, or we have a known 'we need to decode this much' */
	uint8_t decode[9000];
	size_t decode_pos;
	size_t left;
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

	uint8_t* res = DYNAMIC_REALLOC(dst, new_sz);
	if (!res){
		return dst;
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
 * since it will also encrypt, generate MAC and add to buffer prestate
 */
static void append_outb(
	struct a12_state* S, uint8_t type, uint8_t* out, size_t out_sz)
{
/* this means we can just continue our happy stream-cipher and apply to our
 * outgoing data */
	if (S->in_encstate){
/* cipher_update(&S->out_cstream, out, out_sz) */
	}

/* begin a new MAC, chained on our previous one */
	blake2bp_state mac_state = S->mac_init;
	blake2bp_update(&mac_state, S->last_mac_out, MAC_BLOCK_SZ);
	blake2bp_update(&mac_state, &type, 1);
	blake2bp_update(&mac_state, out, out_sz);

/* grow write buffer if the block doesn't fit */
	size_t required =
		S->buf_sz[S->buf_ind] + MAC_BLOCK_SZ + out_sz + S->buf_ofs + 1;
	S->bufs[S->buf_ind] = grow_array(
		S->bufs[S->buf_ind],
		&S->buf_sz[S->buf_ind],
		required
	);

/* and if that didn't work, fatal */
	if (S->buf_sz[S->buf_ind] < required){
		S->state = STATE_BROKEN;
		return;
	}
	uint8_t* dst = S->bufs[S->buf_ind];

/* prepend MAC */
	blake2bp_final(&mac_state, S->last_mac_out, MAC_BLOCK_SZ);
	memcpy(&dst[S->buf_ofs], S->last_mac_out, MAC_BLOCK_SZ);

/* DEBUG: replace mac with 'm' */
	for (size_t i = 0; i < MAC_BLOCK_SZ; i++)
		dst[i] = 'm';
	S->buf_ofs += MAC_BLOCK_SZ;

/* our packet type */
	dst[S->buf_ofs++] = type;

/* and our data block, this costs us an extra copy which isn't very nice -
 * might want to set a target descriptor immediately for some uses here */
	memcpy(&dst[S->buf_ofs], out, out_sz);
	S->buf_ofs += out_sz;
}

static void reset_state(struct a12_state* S)
{
	S->left = header_sizes[STATE_NOPACKET];
	S->state = STATE_NOPACKET;
	S->decode_pos = 0;
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

	debug_print("(a12) channel open, add control packet");
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
	blake2bp_update(&S->mac_dec, S->last_mac_in, MAC_BLOCK_SZ);

/* save last known MAC for later comparison */
	memcpy(S->last_mac_in, S->decode, MAC_BLOCK_SZ);

/* CRYPTO-fixme: if we are in stream cipher mode, decode just the one byte */
	blake2bp_update(&S->mac_dec, &S->decode[MAC_BLOCK_SZ], 1);
	S->state = S->decode[MAC_BLOCK_SZ];

	if (S->state >= STATE_BROKEN){
		debug_print("(a12) channel broken, unknown command val: %"PRIu8, S->state);
		S->state = STATE_BROKEN;
		return;
	}

	debug_print("(a12) left: %zu, state: %"PRIu8, S->left, S->state);
	S->left = header_sizes[S->state];
	S->decode_pos = 0;
}

static bool process_mac(struct a12_state* S)
{
	uint8_t final_mac[MAC_BLOCK_SZ];
	return true;

	blake2bp_update(&S->mac_dec, S->decode, S->decode_pos);
	blake2bp_final(&S->mac_dec, final_mac, MAC_BLOCK_SZ);

/* Option to continue with broken authentication, ... */
	if (memcmp(final_mac, S->last_mac_in, MAC_BLOCK_SZ) != 0){
		debug_print("(a12) authentication mismatch on packet \n");
		S->state = STATE_BROKEN;
		return false;
	}

	debug_print("(a12) authenticated packet");
	return true;
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

	uint8_t channel = S->decode[16];
	uint8_t command = S->decode[17];

	switch(command){
	case 0: break; /* HELLO */
	case 1: break; /* SHUTDOWN */
	case 2: break; /* ENC-NEG */
	case 3: break; /* REKEY */
	case 4: break; /* CANCEL-STREAM */
 /* new vstream */
	case 5:{
		struct video_frame* vframe = &S->channels[channel].unpack_state.vframe;
		unpack_u32(&vframe.id, S->decode[19]
		vframe.id = unpack_32(
	}
	break; /* NEW CHANNEL */
	case 6: break; /* FAILURE */
	case 7:
	break; /* VIDEO */
	case 8: break; /* AUDIO */
	case 9: break; /* BINARY */
	default:
		debug_print("(a12) unhandled control message");
	break;
	}

	debug_print("(a12) decode control packet");
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
		debug_print("(a12) broken event packet received");
	}
	else if (on_event)
		on_event(0, &aev, tag);

	reset_state(S);
}

/*
 * We have an incoming video packet, first we need to match it to the
 * channel that it represents (as we might get interleaved updates) and
 * match the state we are building.
 */
static void process_video(struct a12_state* S)
{
	debug_print("(a12) incoming video frame");
	if (!process_mac(S))
		return;

	if (S->in_channel != -1){
		uint64_t stream;
		uint16_t frame_sz;
		unpack_u64(&stream, &S->decode[1]);
		unpack_u16(&frame_sz, &S->decode[9]);
		S->in_channel = S->decode[0];
		S->left = frame_sz;
		return;
	}

	struct video_frame* cvf = &S->channels[S->in_channel].unpack_state.vframe;
	if (!S->channels[S->in_channel].cont){
		debug_print("(a12) data on unmapped channel");
		reset_state(S);
		return;
	}

/* post-processing / decoder? then just buffer */
	size_t left = cvf->inbuf_sz - cvf->inbuf_pos;
	if (cvf->postprocess != POSTPROCESS_VIDEO_NONE){
		if (left >= S->decode_pos){
			memcpy(&cvf->inbuf[cvf->inbuf_pos], S->decode, S->decode_pos);
			cvf->inbuf_pos += S->decode_pos;

			if (cvf->inbuf_sz == cvf->inbuf_pos){
				debug_print("(a12) decode-buffer size reached");
			}
		}
		else {
			debug_print("(a12) overflow, stream length and packet size mismatch");
		}
		reset_state(S);
		return;
	}

/* raw frame, just unpack into where we are in the context and signal --
 * this does not correctly represent any padding in stride/pitch and just
 * assumes correctly packed */
	struct arcan_shmif_cont* cont = S->channels[S->in_channel].cont;
	if (S->decode_pos + cvf->inbuf_pos <= cont->h * cont->stride){
		memcpy(&cont->vidp[cvf->inbuf_pos], S->decode, S->decode_pos);
		cvf->inbuf_pos += S->decode_pos;
	}
	else {
		debug_print("(a12) overflow, stream length and packet size mismatch");
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
		debug_print("(a12) invalid set_destination call");
		return;
	}

	if (chid != 0){
		debug_print("(a12) multi-channel support unfinished");
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

/* iteratively flush, we tail- recurse should the need arise */
	size_t ntr = buf_sz > S->left ? S->left : buf_sz;
	if (ntr > DECODE_BUFFER_CAP)
		ntr = DECODE_BUFFER_CAP;

/* first add to scratch buffer */
	memcpy(&S->decode[S->decode_pos], buf, ntr);
	S->left -= ntr;
	S->decode_pos += ntr;

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
		debug_print("(a12) unknown state");
		S->state = STATE_BROKEN;
		return;
	break;
	}
/* slide window and tail- if needed */
	buf += ntr;
	buf_sz -= ntr;

	if (buf_sz)
		a12_channel_unpack(S, buf, buf_sz, tag, on_event);
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

void
a12_channel_vframe(struct a12_state* S, struct shmifsrv_vbuffer* vb)
{
	if (!S || S->cookie != 0xfeedface || S->state == STATE_BROKEN)
		return;

	uint8_t outb[header_sizes[STATE_VIDEO_PACKET]];
	memset(outb, 'v', sizeof(outb));
	step_sequence(S, outb);

/* PROGRESSIVE -
 * this is where we'd need actual multiplexing as large V frames will
 * introduce additional input latency that we could work around */

/* Output format:
 * uint32 stream_id
 * uint8 format
 * uint16 x
 * uint16 y
 * uint16 w
 * uint16 h
 * uint8 render_flags
 * uint32 frame_size
 */

/* dealing with each flag:
 * origo_ll - do the coversion in our own encode- stage
 * ignore_alpha - do nothing
 * subregion - feed as information to the delta encoder
 * srgb - info to encoder, other leave be
 * vpts - possibly add as feedback to a scheduler and if there is
 *        near-deadline data, send that first or if it has expired,
 *        drop the frame. This is only a real target for game/decode
 *        but that decision can be pushed to the caller.
 *
 * then we have the problem of the meta- area
 */

	debug_print("(a12) video frame");
	append_outb(S, STATE_VIDEO_PACKET, outb, sizeof(outb));
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
		debug_print("(a12) ignoring descriptor event: %s",
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
	debug_print("(a12) enqueue event %s", arcan_shmif_eventstr(ev, NULL, 0));
}
