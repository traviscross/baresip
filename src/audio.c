/**
 * @file audio.c  Audio stream
 *
 * Copyright (C) 2010 Creytiv.com
 */
#define _BSD_SOURCE 1
#include <string.h>
#include <stdlib.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_PTHREAD
#include <pthread.h>
#endif
#ifdef __APPLE__
#include "TargetConditionals.h"
#endif
#include <re.h>
#include <baresip.h>
#include "core.h"


#define DEBUG_MODULE "audio"
#define DEBUG_LEVEL 5
#include <re_dbg.h>

/** Magic number */
#define MAGIC 0x000a0d10
#include "magic.h"


/**
 * Implements a generic audio stream. The application can allocate multiple
 * instances of a audio stream, mapping it to a particular SDP media line.
 * The audio object has a DSP sound card sink and source, and an audio encoder
 * and decoder. A particular audio object is mapped to a generic media
 * stream object. Each audio channel has an optional audio filtering chain.
 *
 *<pre>
 *            write  read
 *              |    /|\
 *             \|/    |
 * .------.   .---------.    .-------.
 * |filter|<--|  audio  |--->|encoder|
 * '------'   |         |    |-------|
 *            | object  |--->|decoder|
 *            '---------'    '-------'
 *              |    /|\
 *              |     |
 *             \|/    |
 *         .------. .-----.
 *         |auplay| |ausrc|
 *         '------' '-----'
 *</pre>
 */
struct audio {
	MAGIC_DECL                    /**< Magic number for debugging      */
	struct stream *strm;          /**< Generic media stream            */
	struct aucodec_prm enc_prm;   /**< Encoder parameters              */
	struct aucodec_prm dec_prm;   /**< Decoder parameters              */
	struct aucodec_st *enc;       /**< Current audio encoder           */
	struct aucodec_st *dec;       /**< Current audio decoder           */
	struct aufilt_chain *fc;      /**< Audio filter chain              */
	struct ausrc_prm ausrc_prm;   /**< Audio Source parameters         */
	struct ausrc_st *ausrc;       /**< Audio Source                    */
	struct auplay_prm auplay_prm; /**< Audio Player parameters         */
	struct auplay_st *auplay;     /**< Audio Player                    */
	struct aubuf *aubuf_tx;       /**< Packetize outgoing stream       */
	struct aubuf *aubuf_rx;       /**< Incoming audio buffer           */
	struct mbuf *mb_rtp;          /**< Buffer for outgoing RTP packets */
	struct telev *telev;          /**< Telephony events                */
	audio_event_h *eventh;        /**< Event handler                   */
	audio_err_h *errh;            /**< Audio error handler             */
	void *arg;                    /**< Handler argument                */
	uint32_t ptime_tx;            /**< Packet time for sending         */
	uint32_t ptime_rx;            /**< Packet time for receiving       */
	int16_t avg;                  /**< Average audio level (playback)  */
	uint8_t pt_tx;                /**< Payload type for outgoing RTP   */
	uint8_t pt_rx;                /**< Payload type for incoming RTP   */
	uint32_t ts_tx;               /**< Timestamp for outgoing RTP      */
	bool vu_meter;                /**< Enable VU-meter                 */
	bool marker;                  /**< Marker bit for outgoing RTP     */
	bool is_g722;                 /**< Special hack for G.722 codec    */
	bool muted;                   /**< Audio source is muted           */
	bool tx_thread;               /**< Enable Audio Transmit thread    */
	uint8_t pt_telev_tx;          /**< Event payload type - transmit   */
	uint8_t pt_telev_rx;          /**< Event payload type - receive    */
	int cur_key;                  /**< Currently transmitted event     */
#ifdef HAVE_PTHREAD
	pthread_t tid_tx;             /**< Audio transmit thread           */
	bool run_tx;                  /**< Audio transmit thread running   */
#else
	struct tmr tmr_tx;            /**< Timer for sending RTP packets   */
#endif
};


static int audio_stream_decode(struct audio *a, const struct rtp_header *hdr,
			       struct mbuf *mb);


static int bytesps(enum aufmt fmt)
{
	switch (fmt) {

	case AUFMT_S16LE: return 2;
	case AUFMT_PCMA:  return 1;
	case AUFMT_PCMU:  return 1;
	default:          return 0;
	}
}


static void audio_destructor(void *data)
{
	struct audio *a = data;

	audio_stop(a);

	mem_deref(a->enc);
	mem_deref(a->dec);
	mem_deref(a->aubuf_tx);
	mem_deref(a->mb_rtp);
	mem_deref(a->aubuf_rx);
	mem_deref(a->strm);
	mem_deref(a->telev);
}


/**
 * Encoder audio and send via stream
 *
 * @note This function has REAL-TIME properties
 */
static void encode_rtp_send(struct audio *a, struct mbuf *mb, uint16_t nsamp)
{
	int err;

	if (!a->enc)
		return;

	/* Is the stream on hold? */
	if (!stream_is_active(a->strm))
		return;

	/* Make space for RTP header */
	a->mb_rtp->pos = STREAM_PRESZ;
	a->mb_rtp->end = STREAM_PRESZ;

	if (a->ausrc_prm.fmt == AUFMT_S16LE) {
		err = aucodec_get(a->enc)->ench(a->enc, a->mb_rtp, mb);
	}
	else {
		err = mbuf_write_mem(a->mb_rtp, mbuf_buf(mb),
				     mbuf_get_left(mb));
	}
	if (err)
		goto out;

	a->mb_rtp->pos = STREAM_PRESZ;
	err = stream_send(a->strm, a->marker, a->pt_tx, a->ts_tx, a->mb_rtp);
	if (err)
		goto out;

	a->ts_tx += nsamp;

 out:
	a->marker = false;
	if (err) {
		DEBUG_WARNING("encode rtp send: failed (%s)\n", strerror(err));
	}
}


/**
 * Process outgoing audio stream
 *
 * @note This function has REAL-TIME properties
 */
static void process_audio_encode(struct audio *a, struct mbuf *mb)
{
	int err;

	if (!mb)
		return;

	/* Audio filters */
	if (a->ausrc_prm.fmt == AUFMT_S16LE && a->fc) {
		err = aufilt_chain_encode(a->fc, mb);
		if (err) {
			DEBUG_WARNING("aufilt_chain_encode %s\n",
				      strerror(err));
		}
	}

	/* Encode and send */
	encode_rtp_send(a, mb, a->is_g722 ? mb->end/4 : mb->end/2);
}


static void send_packet(struct audio *a, size_t psize)
{
	struct mbuf *mb = mbuf_alloc(psize);
	int err;
	if (!mb)
		return;

	/* timed read from audio-buffer */
	err = aubuf_get(a->aubuf_tx, a->ptime_tx, mb->buf, mb->size);
	if (0 == err) {
		mb->end = mb->size;
		process_audio_encode(a, mb);
	}

	mem_deref(mb);
}


static void poll_aubuf_tx(struct audio *a)
{
	size_t psize = bytesps(a->ausrc_prm.fmt) * a->ausrc_prm.frame_size;

	if (aubuf_cur_size(a->aubuf_tx) >= psize)
		send_packet(a, psize);
}


static void check_telev(struct audio *a)
{
	bool marker = false;
	int err;

	/* Make space for RTP header */
	a->mb_rtp->pos = STREAM_PRESZ;
	a->mb_rtp->end = STREAM_PRESZ;

	err = telev_poll(a->telev, &marker, a->mb_rtp);
	if (err)
		return;

	a->mb_rtp->pos = STREAM_PRESZ;
	err = stream_send(a->strm, marker, a->pt_telev_tx, a->ts_tx,
			  a->mb_rtp);
	if (err) {
		DEBUG_WARNING("telev: stream_send %s\n", strerror(err));
	}
}


/**
 * Write samples to Audio Player.
 *
 * @note This function has REAL-TIME properties
 *
 * @note The application is responsible for filling in silence in
 *       the case of underrun
 */
static bool auplay_write_handler(uint8_t *buf, size_t sz, void *arg)
{
	struct audio *a = arg;

	aubuf_read(a->aubuf_rx, buf, sz);

	return true;
}


/**
 * Read samples from Audio Source
 *
 * @note This function has REAL-TIME properties
 */
static void ausrc_read_handler(const uint8_t *buf, size_t sz, void *arg)
{
	struct audio *a = arg;
	uint8_t *silence = NULL;
	const uint8_t *txbuf = buf;

	/* NOTE:
	 * some devices behave strangely if they receive no RTP,
	 * so we send silence when muted
	 */
	if (a->muted) {
		silence = mem_zalloc(sizeof(uint8_t) * sz, NULL);
		txbuf = silence;
	}

	if (a->aubuf_tx) {
		if (aubuf_write(a->aubuf_tx, txbuf, sz))
			goto out;

		/* XXX: on limited CPU and specifically coreaudio module
		 * calling this procedure, which results in audio encoding,
		 * seems to have an overall negative impact on system
		 * performance! (coming from interrupt context?)
		 */
		if (!a->tx_thread)
			poll_aubuf_tx(a);
	}

 out:
	/* Exact timing: send Telephony-Events from here */
	check_telev(a);
	mem_deref(silence);
}


static void ausrc_error_handler(int err, const char *str, void *arg)
{
	struct audio *a = arg;
	MAGIC_CHECK(a);

	if (a->errh)
		a->errh(err, str, a->arg);
}


static int pt_handler(struct audio *a, uint8_t pt_old, uint8_t pt_new)
{
	const struct sdp_format *lc;

	lc = sdp_media_lformat(stream_sdpmedia(a->strm), pt_new);
	if (!lc)
		return ENOENT;

	(void)re_fprintf(stderr, "Audio decoder changed payload %u -> %u\n",
			 pt_old, pt_new);

	return audio_decoder_set(a, lc->data, lc->pt);
}


/* Handle incoming stream data from the network */
static void stream_recv_handler(const struct rtp_header *hdr,
				struct mbuf *mb, void *arg)
{
	struct audio *a = arg;
	int err;

	MAGIC_CHECK(a);

	/* Telephone event? */
	if (hdr->pt == a->pt_telev_rx)
		goto out;

	/* Audio payload-type changed? */
	if (hdr->pt == a->pt_rx)
		goto out;

	err = pt_handler(a, a->pt_rx, hdr->pt);
	if (err)
		return;

 out:
	err = audio_stream_decode(a, hdr, mb);
	if (err) {
		DEBUG_WARNING("audio_stream_decode failed (%s)\n",
			      strerror(err));
	}
}


int audio_alloc(struct audio **ap, uint32_t ptime, audio_event_h *eventh,
		audio_err_h *errh, void *arg)
{
	struct audio *a;
	int err;

	if (!ap)
		return EINVAL;

	a = mem_zalloc(sizeof(*a), audio_destructor);
	if (!a)
		return ENOMEM;

	MAGIC_INIT(a);

#ifndef HAVE_PTHREAD
	tmr_init(&a->tmr_tx);
#endif

	err = stream_alloc(&a->strm, arg, "audio", 1, stream_recv_handler, a);
	if (err)
		goto out;

	/* This buffer will grow automatically */
	a->mb_rtp = mbuf_alloc(STREAM_PRESZ + 320);
	if (!a->mb_rtp) {
		err = ENOMEM;
		goto out;
	}

	err = telev_alloc(&a->telev, TELEV_PTIME);
	if (err)
		goto out;

	a->pt_tx = a->pt_rx = PT_NONE;
	a->pt_telev_tx = a->pt_telev_rx = PT_NONE;
	a->ts_tx    = 160;
	a->ptime_tx = a->ptime_rx = ptime;
	a->marker = true;
	a->eventh = eventh;
	a->errh = errh;
	a->arg = arg;

#if defined (TARGET_OS_IPHONE)
	a->tx_thread = true;
#else
	a->tx_thread = false;
#endif

 out:
	if (err)
		mem_deref(a);
	else
		*ap = a;

	return err;
}


#ifdef HAVE_PTHREAD
static void *tx_thread(void *arg)
{
	struct audio *a = arg;

#if TARGET_OS_IPHONE && !(TARGET_IPHONE_SIMULATOR)
	realtime_enable(true, 1);
#endif

	while (a->run_tx) {

		poll_aubuf_tx(a);

		usleep(5000);
	}

	return NULL;
}
#else
static void timeout_tx(void *arg)
{
	struct audio *a = arg;

	tmr_start(&a->tmr_tx, 5, timeout_tx, a);

	poll_aubuf_tx(a);
}
#endif


static int aufilt_setup(struct audio *a)
{
	struct aufilt_prm encprm, decprm;

	/* Encoder */
	if (config.audio.srate_src.min &&
	    a->ausrc_prm.srate < config.audio.srate_src.min) {
		encprm.srate       = config.audio.srate_src.min;
		encprm.srate_out   = a->ausrc_prm.srate;
		a->ausrc_prm.srate = config.audio.srate_src.min;
	}
	else if (config.audio.srate_src.max &&
		 a->ausrc_prm.srate > config.audio.srate_src.max) {
		encprm.srate       = config.audio.srate_src.max;
		encprm.srate_out   = a->ausrc_prm.srate;
		a->ausrc_prm.srate = config.audio.srate_src.max;
	}
	else {
		encprm.srate      = a->ausrc_prm.srate;
		encprm.srate_out  = a->ausrc_prm.srate;
	}
	encprm.ch         = a->ausrc_prm.ch;
	encprm.frame_size = a->ausrc_prm.frame_size;
	encprm.aec_len    = config.audio.aec_len;
	a->ausrc_prm.frame_size = calc_nsamp(a->ausrc_prm.srate,
					     a->ausrc_prm.ch, a->ptime_tx);

	/* Decoder */
	if (config.audio.srate_play.min &&
	    a->auplay_prm.srate < config.audio.srate_play.min) {
		decprm.srate        = a->auplay_prm.srate;
		decprm.srate_out    = config.audio.srate_play.min;
		a->auplay_prm.srate = config.audio.srate_play.min;
	}
	else if (config.audio.srate_play.max &&
		 a->auplay_prm.srate > config.audio.srate_play.max) {
		decprm.srate        = a->auplay_prm.srate;
		decprm.srate_out    = config.audio.srate_play.max;
		a->auplay_prm.srate = config.audio.srate_play.max;
	}
	else {
		decprm.srate        = a->auplay_prm.srate;
		decprm.srate_out    = a->auplay_prm.srate;
	}
	decprm.ch         = a->auplay_prm.ch;
	decprm.frame_size = a->auplay_prm.frame_size;
	decprm.aec_len    = config.audio.aec_len;
	a->auplay_prm.frame_size = calc_nsamp(a->auplay_prm.srate,
					      a->auplay_prm.ch, a->ptime_rx);

	return aufilt_chain_alloc(&a->fc, &encprm, &decprm);
}


int audio_start(struct audio *a, const char *dev)
{
	int err;

	if (!a)
		return EINVAL;

	if (!str_len(dev))
		dev = NULL;

	err = stream_start(a->strm);
	if (err)
		return err;

	/* Audio filter */
	if (!a->fc) {
		err = aufilt_setup(a);
		if (err)
			return err;
	}

	/* Start Audio Player */
	if (!a->auplay && auplay_find(NULL)) {

		if (!a->aubuf_rx) {
			const size_t psize = 2 * a->auplay_prm.frame_size;

			err = aubuf_alloc(&a->aubuf_rx, psize * 2, psize * 8);
			if (err)
				return err;
		}

		err = auplay_alloc(&a->auplay, NULL, &a->auplay_prm, dev,
				   auplay_write_handler, a);
		if (err) {
			DEBUG_WARNING("start: audio player failed: %s\n",
				      strerror(err));
			return err;
		}
	}

	/* Start Audio Source */
	if (!a->ausrc && ausrc_find(NULL)) {
		size_t psize;

		psize = bytesps(a->ausrc_prm.fmt) * a->ausrc_prm.frame_size;

		err = aubuf_alloc(&a->aubuf_tx, psize * 2, psize * 30);
		if (err)
			return err;

		err = ausrc_alloc(&a->ausrc, NULL, &a->ausrc_prm, dev,
				  ausrc_read_handler, ausrc_error_handler, a);
		if (err) {
			DEBUG_WARNING("start: audio source failed: %s\n",
				      strerror(err));
			return err;
		}

		if (a->tx_thread) {
#ifdef HAVE_PTHREAD
			if (!a->run_tx) {
				a->run_tx = true;
				err = pthread_create(&a->tid_tx, NULL,
						     tx_thread, a);
				if (err) {
					a->run_tx = false;
					return err;
				}
			}
#else
			tmr_start(&a->tmr_tx, 1, timeout_tx, a);
#endif
		}
	}

	return err;
}


void audio_stop(struct audio *a)
{
	if (!a)
		return;

#ifdef HAVE_PTHREAD
	if (a->run_tx) {
		a->run_tx = false;
		pthread_join(a->tid_tx, NULL);
	}
#else
	tmr_cancel(&a->tmr_tx);
#endif

	/* audio device must be stopped first */
	a->ausrc    = mem_deref(a->ausrc);
	a->auplay   = mem_deref(a->auplay);

	a->fc       = mem_deref(a->fc);
	a->aubuf_tx = mem_deref(a->aubuf_tx);
	a->aubuf_rx = mem_deref(a->aubuf_rx);

	stream_stop(a->strm);
}


/** We cannot use hardware codec when using audio-filters */
static enum aufmt aufmt(const char *name)
{
	(void)name;
	return AUFMT_S16LE;
}


int audio_encoder_set(struct audio *a, struct aucodec *ac,
		      uint8_t pt_tx, const char *params)
{
	struct pl fmtp = pl_null;
	bool sym = aucodec_cmp(ac, aucodec_get(a->dec));
	int err = 0;

	if (!a)
		return EINVAL;

	(void)re_fprintf(stderr, "Set audio encoder: %s %uHz %dch\n",
			 ac->name, ac->srate, ac->ch);

	pl_set_str(&fmtp, params);

	a->ausrc_prm.fmt = aufmt(ac->name);
	a->ausrc_prm.srate = ac->srate;
	a->ausrc_prm.ch = ac->ch;
	a->ausrc_prm.frame_size = calc_nsamp(ac->srate, ac->ch, a->ptime_tx);
	a->pt_tx = pt_tx;

	a->enc = mem_deref(a->enc);

	a->is_g722 = (0 == str_casecmp(ac->name, "G722"));

	if (sym && a->dec) {
		a->enc = mem_ref(a->dec);
		a->enc_prm = a->dec_prm;
		return 0;
	}

	a->enc_prm.srate = ac->srate;
	a->enc_prm.ptime = a->ptime_tx;
	err = ac->alloch(&a->enc, ac, &a->enc_prm, &a->dec_prm, &fmtp);
	if (err) {
		DEBUG_WARNING("audio_encoder_set: codec_alloc() failed (%s)\n",
			      strerror(err));
		return err;
	}

	if (a->enc_prm.srate != ac->srate) {
		DEBUG_INFO("encoder: srate changed %u --> %u\n",
			   ac->srate, a->enc_prm.srate);
		a->ausrc_prm.srate = a->enc_prm.srate;
		a->ausrc_prm.frame_size = calc_nsamp(a->ausrc_prm.srate,
						     ac->ch, a->ptime_tx);
	}

	if (a->enc_prm.ptime != a->ptime_tx) {
		DEBUG_NOTICE("encoder changed ptime: %u -> %u\n",
			     a->ptime_tx, a->enc_prm.ptime);
	}

	stream_set_srate(a->strm, a->ausrc_prm.srate, a->auplay_prm.srate);

	return err;
}


int audio_decoder_set(struct audio *a, struct aucodec *ac, uint8_t pt_rx)
{
	bool sym = aucodec_cmp(ac, aucodec_get(a->enc));
	int err = 0;

	(void)re_fprintf(stderr, "Set audio decoder: %s %uHz %dch\n",
			 ac->name, ac->srate, ac->ch);

	a->auplay_prm.fmt = aufmt(ac->name);
	a->auplay_prm.srate = ac->srate;
	a->auplay_prm.ch = ac->ch;
	a->auplay_prm.frame_size = calc_nsamp(ac->srate, ac->ch, a->ptime_rx);
	a->pt_rx = pt_rx;

	a->dec = mem_deref(a->dec);

	if (sym && a->enc) {
		a->dec = mem_ref(a->enc);
		a->dec_prm = a->enc_prm;
		goto out;
	}

	a->dec_prm.srate = ac->srate;
	a->dec_prm.ptime = a->ptime_rx;

	err = ac->alloch(&a->dec, ac, &a->enc_prm, &a->dec_prm,
			    NULL);
	if (err) {
		DEBUG_WARNING("set decoder: aucodec_alloc() failed: %s\n",
			      strerror(err));
		goto out;
	}

 out:
	if (a->dec_prm.srate != ac->srate) {
		DEBUG_INFO("decoder set: srate changed: %u --> %u\n",
			   ac->srate, a->dec_prm.srate);
		a->auplay_prm.srate = a->dec_prm.srate;
		a->auplay_prm.frame_size = calc_nsamp(a->auplay_prm.srate,
						      ac->ch, a->ptime_rx);
	}

	stream_set_srate(a->strm, a->ausrc_prm.srate, a->auplay_prm.srate);

	return err;
}


/*
 * A set of "setter" functions. Use these to "set" any format values, cause
 * they might trigger changes in other components.
 */

static void audio_ptime_tx_set(struct audio *a, uint32_t ptime_tx)
{
	if (ptime_tx != a->ptime_tx) {
		DEBUG_NOTICE("peer changed ptime_tx %u->%u\n",
			     a->ptime_tx, ptime_tx);
		a->ptime_tx = ptime_tx;
	}
}


static void handle_telev(struct audio *a, struct mbuf *mb)
{
	int event, digit;
	bool end;

	if (telev_recv(a->telev, mb, &event, &end))
		return;

	digit = telev_code2digit(event);
	if (digit >= 0 && a->eventh)
		a->eventh(digit, end, a->arg);
}


/**
 * Decode incoming packets using the Audio decoder
 *
 * NOTE: mb=NULL if no packet received
 */
static int audio_stream_decode(struct audio *a, const struct rtp_header *hdr,
			       struct mbuf *mb)
{
	struct mbuf *mbc;
	int err = 0;
	int n = 64;

	if (!a)
		return EINVAL;

	/* Comfort Noise (CN) as of RFC 3389 */
	if (PT_CN == hdr->pt)
		return 0;

	/* Check for telephone-event payload-type */
	if (hdr->pt == a->pt_telev_rx) {
		handle_telev(a, mb);
		return 0;
	}

	/* No decoder set */
	if (!a->dec)
		return 0;

	if (a->auplay_prm.fmt == AUFMT_S16LE) {
		mbc = mbuf_alloc(4*320);
		if (!mbc)
			return ENOMEM;

		/* Decode all packets */
		do {
			err = aucodec_get(a->dec)->dech(a->dec, mbc, mb);
		} while (n-- && mbuf_get_left(mb) && !err);

		if (!n) {
			DEBUG_WARNING("codec_decode fault (%s)\n",
				      strerror(err));
		}
		if (err) {
			DEBUG_WARNING("codec_decode: %s\n", strerror(err));
			goto out;
		}
		if (!mbc->end) {
			DEBUG_INFO("stream decode: no data decoded (%s)\n",
				   strerror(err));
		}
		mbc->pos = 0;

		/* Perform operations on the PCM samples */

		if (a->fc) {
			err |= aufilt_chain_decode(a->fc, mbc);
		}

		if (a->vu_meter)
			a->avg = calc_avg_s16(mbc);
	}
	else {
		mbc = mem_ref(mb);
	}

	if (a->aubuf_rx) {
		err = aubuf_append(a->aubuf_rx, mbc);
		if (err)
			goto out;
	}

 out:
	mem_deref(mbc);
	return err;
}


void audio_enable_vumeter(struct audio *a, bool en)
{
	if (!a)
		return;

	a->vu_meter = en;
}


struct stream *audio_strm(const struct audio *a)
{
	return a ? a->strm : NULL;
}


int audio_print_vu(struct re_printf *pf, const struct audio *a)
{
	char avg_buf[16];
	size_t i, res;

	if (!a || !a->vu_meter)
		return 0;

	res = min(2*sizeof(avg_buf)*a->avg/0x8000, sizeof(avg_buf)-1);
	memset(avg_buf, 0, sizeof(avg_buf));
	for (i=0; i<res; i++) {
		avg_buf[i] = '=';
	}

	return re_hprintf(pf, " [%-16s]", avg_buf);
}


void audio_enable_telev(struct audio *a, uint8_t pt_tx, uint8_t pt_rx)
{
	if (!a)
		return;

	(void)re_printf("Enable telephone-event: pt_tx=%u, pt_rx=%u\n",
			pt_tx, pt_rx);

	a->pt_telev_tx = pt_tx;
	a->pt_telev_rx = pt_rx;
}


int audio_send_digit(struct audio *a, char key)
{
	int err = 0;

	if (!a)
		return EINVAL;

	if (key > 0) {
		(void)re_printf("send DTMF digit: '%c'\n", key);
		err = telev_send(a->telev, telev_digit2code(key), false);
	}
	else if (a->cur_key) {
		/* Key release */
		(void)re_printf("send DTMF digit end: '%c'\n", a->cur_key);
		err = telev_send(a->telev, telev_digit2code(a->cur_key), true);
	}

	a->cur_key = key;

	return err;
}


void audio_mute(struct audio *a, bool muted)
{
	if (!a)
		return;

	a->muted = muted;
}


void audio_enable_txthread(struct audio *a, bool enabled)
{
	if (!a)
		return;

	a->tx_thread = enabled;
}


int audio_sdp_attr_encode(const struct audio *a, struct sdp_media *m)
{
	int err = 0;

	if (!a)
		return EINVAL;

	err |= sdp_media_set_lattr(m, true, "ptime", "%u", a->ptime_tx);

	err |= stream_sdp_attr_encode(a->strm, m);

	return err;
}


void audio_sdp_attr_decode(struct audio *a, struct sdp_media *m)
{
	const char *attr;

	/* This is probably only meaningful for audio data, but
	   may be used with other media types if it makes sense. */
	attr = sdp_media_rattr(m, "ptime");
	if (attr)
		audio_ptime_tx_set(a, atoi(attr));

	stream_sdp_attr_decode(a->strm);
}


int audio_debug(struct re_printf *pf, const struct audio *a)
{
	int err;

	if (!a)
		return 0;

	err  = re_hprintf(pf, "\n--- Audio stream ---\n");
	err |= re_hprintf(pf, " tx: fmt=%d %uHz/%dch@%ums pt=%u\n",
			  a->ausrc_prm.fmt, a->ausrc_prm.srate,
			  a->ausrc_prm.ch, a->ptime_tx, a->pt_tx);
	err |= re_hprintf(pf, " rx: fmt=%d %uHz/%dch@%ums pt=%u\n",
			  a->auplay_prm.fmt, a->auplay_prm.srate,
			  a->auplay_prm.ch, a->ptime_rx, a->pt_rx);

	err |= aufilt_chain_debug(pf, a->fc);
	err |= stream_debug(pf, a->strm);

	err |= re_hprintf(pf, " aubuf_tx: %H\n", aubuf_debug, a->aubuf_tx);
	err |= re_hprintf(pf, " aubuf_rx: %H\n", aubuf_debug, a->aubuf_rx);

	return err;
}
