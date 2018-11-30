#ifndef HAVE_A12_INT
#define HAVE_A12_INT

#include <arcan_shmif.h>
#include <arcan_shmif_server.h>

#include <inttypes.h>
#include <string.h>
#include <math.h>

/* crypto / line format */
#include "blake2.h"
#include "pack.h"

#include "miniz/miniz.h"

#ifdef WANT_X264
#include <x264.h>
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

enum {
	STATE_NOPACKET = 0,
	STATE_CONTROL_PACKET = 1,
	STATE_EVENT_PACKET = 2,
	STATE_AUDIO_PACKET = 3,
	STATE_VIDEO_PACKET = 4,
	STATE_BLOB_PACKET = 5,
	STATE_BROKEN
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

#define SEQUENCE_NUMBER_SIZE 8

#ifdef _DEBUG
#define DEBUG 2
#else
#define DEBUG 0
#endif

#ifndef debug_print
#define debug_print(lvl, fmt, ...) \
            do { if (DEBUG >= lvl) fprintf(stderr, "%s:%d:%s(): " fmt "\n", \
						"a12:", __LINE__, __func__,##__VA_ARGS__); } while (0)
#endif

enum {
	POSTPROCESS_VIDEO_RGBA = 0,
	POSTPROCESS_VIDEO_RGB,
	POSTPROCESS_VIDEO_RGB565,
	POSTPROCESS_VIDEO_DMINIZ,
	POSTPROCESS_VIDEO_MINIZ,
	POSTPROCESS_VIDEO_H264
};

size_t a12int_header_size(int type);

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
	size_t out_pos;

	uint8_t pxbuf[4];
	uint8_t carry;

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

/* encoding (recall, both sides can actually do this) */
		struct shmifsrv_vbuffer acc;
		union {
			uint8_t* compression;
#ifdef WANT_X264
			struct {
				x264_t* encoder;
				x264_picture_t pict_in, pict_out;
			} x264;
#endif
	};
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

void a12int_append_out(
	struct a12_state* S, uint8_t type, uint8_t* out, size_t out_sz,
	uint8_t* prepend, size_t prepend_sz);

#endif
