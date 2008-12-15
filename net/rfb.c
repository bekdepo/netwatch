#include <stdint.h>
#include <minilib.h>
#include <output.h>
#include <fb.h>
#include <keyboard.h>

#include "lwip/tcp.h"
#include "lwip/stats.h"

#include "rfb.h"

#define SET_PIXEL_FORMAT	0
#define SET_ENCODINGS		2
#define FB_UPDATE_REQUEST	3
#define KEY_EVENT		4
#define POINTER_EVENT		5
#define CLIENT_CUT_TEXT		6

#define RFB_BUF_SIZE	1536

#define SCREEN_CHUNKS_X 8
#define SCREEN_CHUNKS_Y 8

struct pixel_format {
	uint8_t bpp;
	uint8_t depth;
	uint8_t big_endian;
	uint8_t true_color;
	uint16_t red_max;
	uint16_t green_max;
	uint16_t blue_max;
	uint8_t red_shift;
	uint8_t green_shift;
	uint8_t blue_shift;
	uint8_t padding[3];
};

struct server_init_message {
	uint16_t fb_width;
	uint16_t fb_height;
	struct pixel_format fmt;
	uint32_t name_length;
	char name_string[8];
};

struct fb_update_req {
	uint8_t msgtype;
	uint8_t incremental;
	uint16_t xpos;
	uint16_t ypos;
	uint16_t width;
	uint16_t height;
};

struct set_encs_req {
	uint8_t msgtype;
	uint8_t padding;
	uint16_t num;
	int32_t encodings[];
};

struct key_event_pkt {
	uint8_t msgtype;
	uint8_t downflag;
	uint8_t pad[2];
	uint32_t keysym;
};

struct pointer_event_pkt {
	uint8_t msgtype;
	uint8_t button_mask;
	uint16_t x;
	uint16_t y;
};

struct text_event_pkt {
	uint8_t msgtype;
	uint8_t padding[3];
	uint32_t length;
	char text[];
};

struct update_header {
	uint8_t msgtype;
	uint8_t padding;
	uint16_t nrects;
	uint16_t xpos;
	uint16_t ypos;
	uint16_t width;
	uint16_t height;
	int32_t enctype;
};

struct rfb_state {
	enum {
		ST_BEGIN = 0,
		ST_CLIENTINIT,
		ST_MAIN
	} state;
	int version;
	int encs_remaining;

	char data[RFB_BUF_SIZE];
	int readpos;
	int writepos;

	char next_update_incremental;
	char update_requested;

	struct fb_update_req client_interest_area;

	enum {
		SST_IDLE = 0,
		SST_HEADER,
		SST_DATA
	} send_state;

	uint32_t checksums[SCREEN_CHUNKS_X][SCREEN_CHUNKS_Y];

	uint32_t chunk_xnum;
	uint32_t chunk_ynum;
	uint32_t chunk_xpos;
	uint32_t chunk_ypos;
	uint32_t chunk_width;
	uint32_t chunk_height;

	uint32_t chunk_bytes_sent;
	
	uint32_t chunk_checksum;

	int chunk_actually_sent;
	int try_in_a_bit;

	char * blockbuf;
};

static struct server_init_message server_info;

static void init_server_info() {
	server_info.name_length = htonl(8);
	memcpy(server_info.name_string, "NetWatch", 8);
}

static void update_server_info() {
	if (fb != NULL) {
		outputf("RFB: setting fmt %d", fb->curmode.format);
		server_info.fb_width = htons(fb->curmode.xres);
		server_info.fb_height = htons(fb->curmode.yres);
		switch (fb->curmode.format) {
		case FB_RGB888:
			server_info.fmt.bpp = 32;
			server_info.fmt.depth = 24;
			server_info.fmt.big_endian = 0;
			server_info.fmt.true_color = 1;
			server_info.fmt.red_max = htons(255);
			server_info.fmt.green_max = htons(255);
			server_info.fmt.blue_max = htons(255);
			server_info.fmt.red_shift = 0;
			server_info.fmt.green_shift = 8;
			server_info.fmt.blue_shift = 16;
			break;
		default:
			outputf("RFB: unknown fb fmt %d", fb->curmode.format);
			break;
		}
	} else {
		outputf("RFB: fb null");
	}
}

static int advance_chunk(struct rfb_state *state) {

	state->chunk_xnum += 1;

	if (state->chunk_xnum == SCREEN_CHUNKS_X) {
		state->chunk_ynum += 1;
		state->chunk_xnum = 0;
	}

	if (state->chunk_ynum == SCREEN_CHUNKS_Y) {
		state->chunk_ynum = 0;
		state->send_state = SST_IDLE;
		if (!(state->chunk_actually_sent))
			state->try_in_a_bit = 2;
			return 1;
	}

	return 0;
}

static int ceildiv(int a, int b) {
	int res = a / b;
	if (a % b != 0) {
		res++;
	}
	return res;
}
	
static void send_fsm(struct tcp_pcb *pcb, struct rfb_state *state) {
	struct update_header hdr;
	int bytes_left;
	int totaldim;
	err_t err;

	while(1) {

		switch (state->send_state) {

		case SST_IDLE:
			/* Nothing to do */

			if (state->update_requested) {
				outputf("RFB send: update requested");
				state->update_requested = 0;
				state->chunk_actually_sent = 0;
				state->send_state = SST_HEADER;
			} else {
				return;
			}
	
			/* FALL THROUGH to SST_HEADER */

		case SST_HEADER:

			/* Calculate the width and height for this chunk, remembering
			 * that if SCREEN_CHUNKS_[XY] do not evenly divide the width and
			 * height, we may need to have shorter chunks at the edge of
			 * the screen. */

			state->chunk_width = ceildiv(fb->curmode.xres, SCREEN_CHUNKS_X);
			state->chunk_xpos = state->chunk_width * state->chunk_xnum;
			totaldim = state->chunk_width * (state->chunk_xnum + 1);
			if (totaldim > fb->curmode.xres) {
				state->chunk_width -= (totaldim - fb->curmode.xres);
			}

			state->chunk_height = ceildiv(fb->curmode.yres, SCREEN_CHUNKS_Y);
			state->chunk_ypos = state->chunk_height
						 * state->chunk_ynum;
			totaldim = state->chunk_height * (state->chunk_ynum + 1);
			if (totaldim > fb->curmode.yres) {
				state->chunk_height -= (totaldim - fb->curmode.yres);
			}

			/* Do we _actually_ need to send this chunk? */
			if (fb->checksum_rect) {
				state->chunk_checksum = fb->checksum_rect(state->chunk_xpos, state->chunk_ypos,
								state->chunk_width, state->chunk_height);

				if (state->chunk_checksum == state->checksums[state->chunk_xnum][state->chunk_ynum]) {
					if (advance_chunk(state))
						return;
					continue;
				}
				/* Checksum gets set in data block, AFTER the data has been sent. */
			}

			state->chunk_actually_sent = 1;

			/* Send a header */
			hdr.msgtype = 0;
			state->chunk_bytes_sent = 0;
			hdr.nrects = htons(1);
			hdr.xpos = htons(state->chunk_xpos);
			hdr.ypos = htons(state->chunk_ypos);
			hdr.width = htons(state->chunk_width);
			hdr.height= htons(state->chunk_height);
			hdr.enctype = htonl(0);

			err = tcp_write(pcb, &hdr, sizeof(hdr), TCP_WRITE_FLAG_COPY);

			if (err != ERR_OK) {
				if (err != ERR_MEM)
					outputf("RFB: header send error %d", err);

				/* Try again later. */
				return;
			}

			state->send_state = SST_DATA;

			/* Snag the data. */
			fb->copy_pixels(state->blockbuf,
				state->chunk_xpos, state->chunk_ypos,
				state->chunk_width, state->chunk_height);

			/* FALL THROUGH to SST_DATA */

		case SST_DATA:

			bytes_left = 4 * state->chunk_width * state->chunk_height - state->chunk_bytes_sent;

			if (bytes_left == 0) {
				state->send_state = SST_HEADER;
				state->checksums[state->chunk_xnum][state->chunk_ynum] = state->chunk_checksum;
				if (advance_chunk(state))
					return;
				break;
			}

			/* That's enough. */
			if (bytes_left > 1400) {
				bytes_left = 1400;
			}

			err = tcp_write(pcb, state->blockbuf + state->chunk_bytes_sent,
				bytes_left, TCP_WRITE_FLAG_COPY);

			if (err == ERR_OK) {
				state->chunk_bytes_sent += bytes_left;
			} else {
				if (err != ERR_MEM)
					outputf("RFB: send error %d", err);

				return;
			}
				
			if (tcp_sndbuf(pcb) == 0) {
				return;
			}
		}
	}
	
	if (tcp_output(pcb) != ERR_OK)
		outputf("RFB: tcp_output bailed in send_fsm?");
}

static err_t rfb_sent(void *arg, struct tcp_pcb *pcb, uint16_t len) {
	struct rfb_state *state = arg;
	send_fsm(pcb, state);
	return ERR_OK;
}

static err_t rfb_poll(void *arg, struct tcp_pcb *pcb) {
	struct rfb_state *state = arg;
	send_fsm(pcb, state);
	if (state->try_in_a_bit) {
		state->try_in_a_bit--;
		if (!(state->try_in_a_bit)) {
			state->update_requested = 1;
		}
	}
/*
	stats_display();
*/
	return ERR_OK;
}

static void close_conn(struct tcp_pcb *pcb, struct rfb_state *state) {
	outputf("close_conn: bailing");
	tcp_arg(pcb, NULL);
	tcp_sent(pcb, NULL);
	tcp_recv(pcb, NULL);
	mem_free(state);
	mem_free(state->blockbuf);
	tcp_close(pcb);
	outputf("close_conn: done");
}

enum fsm_result {
	NEEDMORE,
	OK,
	FAIL
};

static enum fsm_result recv_fsm(struct tcp_pcb *pcb, struct rfb_state *state) {
	int i;
	int pktsize;

	outputf("RFB FSM: st %d rp %d wp %d", state->state, state->readpos,
		state->writepos);

	switch(state->state) {
	case ST_BEGIN:
		if (state->writepos < 12) return NEEDMORE;

		if (!strncmp(state->data, "RFB 003.003\n", 12)) {
			state->version = 3;
		} else if (!strncmp(state->data, "RFB 003.005\n", 12)) {
			/* Spec states that "RFB 003.005", an incorrect value,
			 * should be treated by the server as 3.3. */
			state->version = 3;
		} else if (!strncmp(state->data, "RFB 003.007\n", 12)) {
			state->version = 7;
		} else if (!strncmp(state->data, "RFB 003.008\n", 12)) {
			state->version = 8;
		} else {
			outputf("RFB: Negotiation fail");
			return FAIL;
		}

		outputf("RFB: Negotiated v3.%d", state->version);

		state->readpos += 12;
		state->state = ST_CLIENTINIT;

		/* We support one security type, currently "none".
		 * Send that and SecurityResult. */
		if (state->version >= 7) {
			tcp_write(pcb, "\x01\x01\x00\x00\x00\x00", 6, 0);
		} else {
			tcp_write(pcb, "\x01\x00\x00\x00\x00", 5, 0);
		}

		tcp_output(pcb);

		return OK;

	case ST_CLIENTINIT:
		if (state->version >= 7) {
			/* Ignore the security type and ClientInit */
			if (state->writepos < 2) return NEEDMORE;
			state->readpos += 2;
		} else {
			/* Just ClientInit */
			if (state->writepos < 1) return NEEDMORE;
			state->readpos += 1;
		}

		state->state = ST_MAIN;

		outputf("RFB: Sending server info", state->version);
		tcp_write(pcb, &server_info, sizeof(server_info), TCP_WRITE_FLAG_COPY);
		tcp_output(pcb);

		return OK;

	case ST_MAIN:
		if (state->writepos < 1) return NEEDMORE;

		outputf("RFB: cmd %d", state->data[0]);
		switch (state->data[0]) {

		case SET_PIXEL_FORMAT:
			/* SetPixelFormat */
			if (state->writepos < (sizeof(struct pixel_format) + 4))
				return NEEDMORE;
			outputf("RFB: SetPixelFormat");
/*
			struct pixel_format * new_fmt =
				(struct pixel_format *)(&state->data[4]);
*/
			/* XXX ... */

			state->readpos += sizeof(struct pixel_format) + 4;
			return OK;

		case SET_ENCODINGS:
			if (state->writepos < 4) return NEEDMORE;

			struct set_encs_req * req = (struct set_encs_req *)state->data;

			pktsize = sizeof(struct set_encs_req) + (4 * ntohs(req->num));

			outputf("RFB: SetEncodings [%d]", ntohs(req->num));
			if (state->writepos < pktsize) return NEEDMORE;

			for (i = 0; i < ntohs(req->num); i++) {
				outputf("RFB: Encoding: %d", ntohl(req->encodings[i]));
				/* XXX ... */
			}

			state->readpos += pktsize;
			return OK;

		case FB_UPDATE_REQUEST:
			if (state->writepos < sizeof(struct fb_update_req))
				return NEEDMORE;
			outputf("RFB: UpdateRequest");

			state->update_requested = 1;
			memcpy(&state->client_interest_area, state->data,
			       sizeof(struct fb_update_req)); 

			state->readpos += sizeof(struct fb_update_req);
			return OK;

		case KEY_EVENT:
			if (state->writepos < sizeof(struct key_event_pkt))
				return NEEDMORE;

			struct key_event_pkt * p = (struct key_event_pkt *)state->data;

			outputf("RFB: Key: %d (%c)", htonl(p->keysym), (htonl(p->keysym) & 0xFF));
			kbd_inject_keysym(htonl(p->keysym), p->downflag);

			state->readpos += sizeof(struct key_event_pkt);
			return OK;

		case POINTER_EVENT:
			if (state->writepos < sizeof(struct pointer_event_pkt))
				return NEEDMORE;
			outputf("RFB: Pointer");

			/* XXX stub */

			state->readpos += sizeof(struct pointer_event_pkt);
			return OK;

		case CLIENT_CUT_TEXT:
			if (state->writepos < sizeof(struct text_event_pkt))
				return NEEDMORE;
			outputf("RFB: Cut Text");

			struct text_event_pkt * pkt =
				(struct text_event_pkt *)state->data;

			if (state->writepos < sizeof(struct text_event_pkt)
					      + pkt->length)
				return NEEDMORE;

			/* XXX stub */

			state->readpos += sizeof(struct text_event_pkt)
					  + pkt->length;
			return OK;

		default:
			outputf("RFB: Bad command: %d", state->data[0]);
			return FAIL;
		}
	default:
		outputf("RFB: Bad state");
		return FAIL;
	}
}

static err_t rfb_recv(void *arg, struct tcp_pcb *pcb,
		      struct pbuf *p, err_t err) {
	struct rfb_state *state = arg;
	uint16_t copylen;

	if (state == NULL) 

	if (err != ERR_OK) {
		outputf("RFB: recv err %d", err);
		/* FIXME do something better here? */
		return ERR_OK;
	}

	if (p == NULL) {
		outputf("RFB: Connection closed");
		close_conn(pcb, state);
		return ERR_OK;
	}

	if (p->tot_len > (RFB_BUF_SIZE - state->writepos)) {
		/* Overflow! */
		outputf("RFB: Overflow!");
		close_conn(pcb, state);
		return ERR_OK;
	}

	copylen = pbuf_copy_partial(p, state->data + state->writepos, p->tot_len, 0);
	outputf("RFB: Processing %d, wp %d, cp %d", p->tot_len, state->writepos, copylen);
	state->writepos += p->tot_len;

	tcp_recved(pcb, p->tot_len);
	pbuf_free(p);

	while (1) {
		switch (recv_fsm(pcb, state)) {
		case NEEDMORE:
			outputf("RFB FSM: blocking");
			goto doneprocessing;

		case OK:
			outputf("RFB FSM: ok");

			if (state->readpos == state->writepos) {
				state->readpos = 0;
				state->writepos = 0;
				goto doneprocessing;
			} else {
				memmove(state->data,
					state->data + state->readpos,
					state->writepos - state->readpos);
				state->writepos -= state->readpos;
				state->readpos = 0;
			}
			break;
		case FAIL:
			/* Shit */
			outputf("RFB: Protocol error");
			close_conn(pcb, state);
			return ERR_OK;
		}
	}

doneprocessing:

	/* Kick off a send. */
	if (state->send_state == SST_IDLE && state->update_requested) {
		send_fsm(pcb, state);
	}

	return ERR_OK;
}	
		
static err_t rfb_accept(void *arg, struct tcp_pcb *pcb, err_t err) {
	struct rfb_state *state;
	char * blockbuf;

	LWIP_UNUSED_ARG(arg);
	LWIP_UNUSED_ARG(err);

	state = (struct rfb_state *)mem_malloc(sizeof(struct rfb_state));

	if (!state)
	{
		outputf("rfb_accept: out of memory\n");
		return ERR_MEM;
	}

	memset(state, 0, sizeof(struct rfb_state));

	blockbuf = mem_malloc(ceildiv(fb->curmode.xres, SCREEN_CHUNKS_X)
	                    * ceildiv(fb->curmode.yres, SCREEN_CHUNKS_Y) * 4);

	if (!blockbuf)
	{
		outputf("rfb_accept: out of memory allocating blockbuf\n");
		mem_free(state);
		return ERR_MEM;
	}

	state->blockbuf = blockbuf;
	state->state = ST_BEGIN;
	state->send_state = SST_IDLE;

	/* XXX: update_server_info() should be called from the 64ms timer, and deal
	 * with screen resizes appropriately. */
	update_server_info();

	tcp_arg(pcb, state);
	tcp_recv(pcb, rfb_recv);
	tcp_sent(pcb, rfb_sent);
	tcp_poll(pcb, rfb_poll, 1);
/*
	tcp_err(pcb, rfb_err);
*/
	tcp_write(pcb, "RFB 003.008\n", 12, 0);
	tcp_output(pcb);

	return ERR_OK;
}

void rfb_init() {
	struct tcp_pcb *pcb;

	init_server_info();

	pcb = tcp_new();
	tcp_bind(pcb, IP_ADDR_ANY, RFB_PORT);
	pcb = tcp_listen(pcb);
	tcp_accept(pcb, rfb_accept);
}
