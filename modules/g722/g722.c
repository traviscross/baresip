/**
 * @file g722.c  G.722 audio codec
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <re.h>
#include <baresip.h>
#define SPANDSP_EXPOSE_INTERNAL_STRUCTURES 1
#include <spandsp.h>


#define DEBUG_MODULE "g722"
#define DEBUG_LEVEL 5
#include <re_dbg.h>


/*
  http://www.soft-switch.org/spandsp-modules.html
 */

/* From RFC 3551:

 4.5.2 G722

   G722 is specified in ITU-T Recommendation G.722, "7 kHz audio-coding
   within 64 kbit/s".  The G.722 encoder produces a stream of octets,
   each of which SHALL be octet-aligned in an RTP packet.  The first bit
   transmitted in the G.722 octet, which is the most significant bit of
   the higher sub-band sample, SHALL correspond to the most significant
   bit of the octet in the RTP packet.

   Even though the actual sampling rate for G.722 audio is 16,000 Hz,
   the RTP clock rate for the G722 payload format is 8,000 Hz because
   that value was erroneously assigned in RFC 1890 and must remain
   unchanged for backward compatibility.  The octet rate or sample-pair
   rate is 8,000 Hz.
 */

enum {
	G722_SAMPLE_RATE = 16000,
	G722_BITRATE_48k = 48000,
	G722_BITRATE_56k = 56000,
	G722_BITRATE_64k = 64000
};

struct aucodec_st {
	struct aucodec *ac;  /* inheritance */
	g722_encode_state_t enc;
	g722_decode_state_t dec;
};

static struct aucodec *g722;


static void destructor(void *arg)
{
	struct aucodec_st *st = arg;

	mem_deref(st->ac);
}


static int alloc(struct aucodec_st **stp, struct aucodec *ac,
		 struct aucodec_prm *encp, struct aucodec_prm *decp,
		 const char *fmtp)
{
	struct aucodec_st *st;
	int err = 0;

	(void)fmtp;

	st = mem_alloc(sizeof(*st), destructor);
	if (!st)
		return ENOMEM;

	st->ac = mem_ref(ac);

	if (!g722_encode_init(&st->enc, G722_BITRATE_64k, 0)) {
		DEBUG_WARNING("g722_encode_init failed\n");
		err = EPROTO;
		goto out;
	}

	if (!g722_decode_init(&st->dec, G722_BITRATE_64k, 0)) {
		DEBUG_WARNING("g722_decode_init failed\n");
		err = EPROTO;
		goto out;
	}

	/* This is an exception for the G.722 codec */
	if (encp)
		encp->srate = G722_SAMPLE_RATE;
	if (decp)
		decp->srate = G722_SAMPLE_RATE;

 out:
	if (err)
		mem_deref(st);
	else
		*stp = st;

	return err;
}


static int encode(struct aucodec_st *st, struct mbuf *dst, struct mbuf *src)
{
	size_t n;
	int err, len;

	n = mbuf_get_left(src);

	/* Make sure there is enough space */
	if (mbuf_get_space(dst) < n/4) {
		err = mbuf_resize(dst, 2 * (dst->pos + n/4));
		if (err)
			return err;
	}

	len = g722_encode(&st->enc, mbuf_buf(dst),
			  (int16_t *)mbuf_buf(src), (int)n/2);
	if (len <= 0) {
		DEBUG_WARNING("g722_encode: len=%d\n", len);
	}
	else if (len > (int)mbuf_get_space(dst)) {
		DEBUG_WARNING("encode: wrote %d in %d buf\n",
			      len, mbuf_get_space(dst));
		return EBADMSG;
	}

	mbuf_advance(src, n);
	mbuf_set_end(dst, dst->end + len);

	return 0;
}


/* src=NULL means lost packet */
static int decode(struct aucodec_st *st, struct mbuf *dst, struct mbuf *src)
{
	int nsamp, err;
	size_t n;

	if (!mbuf_get_left(src))
		return 0;

	n = 4 * mbuf_get_left(src);

	/* Make sure there is enough space in the buffer */
	if (mbuf_get_space(dst) < n) {
		DEBUG_NOTICE("decode: buffer too small (size=%u, need %u)\n",
			     mbuf_get_space(dst), n);
		err = mbuf_resize(dst, 2 * (dst->pos + n));
		if (err)
			return err;
	}

	nsamp = g722_decode(&st->dec, (int16_t *)mbuf_buf(dst), mbuf_buf(src),
			    (int)mbuf_get_left(src));
	if (nsamp <= 0) {
		DEBUG_WARNING("g722_decode: nsamp=%d\n", nsamp);
	}

	mbuf_skip_to_end(src);
	mbuf_set_end(dst, dst->end + nsamp*2);

	return 0;
}


static int module_init(void)
{
	return aucodec_register(&g722, "9", "G722", 8000, 1, NULL,
				alloc, encode, decode, NULL);
}


static int module_close(void)
{
	g722 = mem_deref(g722);
	return 0;
}


/** Module exports */
EXPORT_SYM const struct mod_export DECL_EXPORTS(g722) = {
	"g722",
	"codec",
	module_init,
	module_close
};
