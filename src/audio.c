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


enum audio_mode {
	AUDIO_MODE_POLL = 0,
	AUDIO_MODE_THREAD,
	AUDIO_MODE_TMR
};

struct audio {
	MAGIC_DECL                    /**< Magic number for debugging      */
	struct stream *strm;          /**< Generic media stream            */
	struct aucodec_st *enc;       /**< Current audio encoder           */
	struct aucodec_st *dec;       /**< Current audio decoder           */
	struct aufilt_chain *fc;      /**< Audio filter chain              */
	struct auplay_st *auplay;     /**< Audio Player                    */
	struct ausrc_st *ausrc;       /**< Audio Source                    */
	struct aubuf *aubuf_tx;       /**< Packetize outgoing stream       */
	struct aubuf *aubuf_rx;       /**< Incoming audio buffer           */
	struct telev *telev;          /**< Telephony events                */
	struct mbuf *mb_rtp;          /**< Buffer for outgoing RTP packets */
	audio_event_h *eventh;        /**< Event handler                   */
	audio_err_h *errh;            /**< Audio error handler             */
	void *arg;                    /**< Handler argument                */
	size_t psize;                 /**< Packet size for sending         */
	uint32_t ptime_tx;            /**< Packet time for sending         */
	uint32_t ptime_rx;            /**< Packet time for receiving       */
	int16_t avg;                  /**< Average audio level (playback)  */
	uint8_t pt_tx;                /**< Payload type for outgoing RTP   */
	uint8_t pt_rx;                /**< Payload type for incoming RTP   */
	uint8_t pt_tel_tx;            /**< Event payload type - transmit   */
	uint8_t pt_tel_rx;            /**< Event payload type - receive    */
	uint32_t ts_tx;               /**< Timestamp for outgoing RTP      */
	bool vu_meter;                /**< Enable VU-meter                 */
	bool marker;                  /**< Marker bit for outgoing RTP     */
	bool is_g722;                 /**< Set if encoder is G.722 codec   */
	bool muted;                   /**< Audio source is muted           */
	int cur_key;                  /**< Currently transmitted event     */
	struct tmr tmr_tx;            /**< Timer for sending RTP packets   */
	enum audio_mode mode;         /**< Audio mode for sending packets  */
#ifdef HAVE_PTHREAD
	pthread_t tid_tx;             /**< Audio transmit thread           */
	bool run_tx;                  /**< Audio transmit thread running   */
#endif
};


static void audio_destructor(void *arg)
{
	struct audio *a = arg;

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
 * Get the DSP samplerate for an audio-codec (exception for G.722)
 */
static inline uint32_t get_srate(const struct aucodec *ac)
{
	if (!ac)
		return 0;

	return !str_casecmp(ac->name, "G722") ? 16000 : ac->srate;
}


static bool aucodec_equal(const struct aucodec *a, const struct aucodec *b)
{
	if (!a || !b)
		return false;

	return get_srate(a) == get_srate(b) && a->ch == b->ch;
}


static int add_audio_codec(struct sdp_media *m, struct aucodec *ac)
{
	if (!in_range(&config.audio.srate, ac->srate)) {
		DEBUG_INFO("skip codec with %uHz (audio range %uHz - %uHz)\n",
			   ac->srate,
			   config.audio.srate.min, config.audio.srate.max);
		return 0;
	}

	if (!in_range(&config.audio.channels, ac->ch)) {
		DEBUG_INFO("skip codec with %uch (audio range %uch-%uch)\n",
			   ac->ch, config.audio.channels.min,
			   config.audio.channels.max);
		return 0;
	}

	return sdp_format_add(NULL, m, false, ac->pt, ac->name, ac->srate,
			      ac->ch, NULL, ac, false, "%s", ac->fmtp);
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

	a->mb_rtp->pos = a->mb_rtp->end = STREAM_PRESZ;

	err = aucodec_get(a->enc)->ench(a->enc, a->mb_rtp, mb);
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
	if (a->fc) {
		err = aufilt_chain_encode(a->fc, mb);
		if (err) {
			DEBUG_WARNING("aufilt_chain_encode %s\n",
				      strerror(err));
		}
	}

	/* Encode and send */
	encode_rtp_send(a, mb, a->is_g722 ? mb->end/4 : mb->end/2);
}


static void poll_aubuf_tx(struct audio *a)
{
	struct mbuf *mb = mbuf_alloc(a->psize);
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


static void check_telev(struct audio *a)
{
	bool marker = false;
	int err;

	a->mb_rtp->pos = a->mb_rtp->end = STREAM_PRESZ;

	err = telev_poll(a->telev, &marker, a->mb_rtp);
	if (err)
		return;

	a->mb_rtp->pos = STREAM_PRESZ;
	err = stream_send(a->strm, marker, a->pt_tel_tx, a->ts_tx, a->mb_rtp);
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
		silence = mem_zalloc(sizeof(*silence) * sz, NULL);
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
		if (a->mode == AUDIO_MODE_POLL)
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
static int audio_stream_decode(struct audio *a, struct mbuf *mb)
{
	struct mbuf *mbc;
	int err = 0;
	int n = 64;

	if (!a)
		return EINVAL;

	/* No decoder set */
	if (!a->dec)
		return 0;

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

	if (a->aubuf_rx) {
		err = aubuf_append(a->aubuf_rx, mbc);
		if (err)
			goto out;
	}

 out:
	mem_deref(mbc);
	return err;
}


/* Handle incoming stream data from the network */
static void stream_recv_handler(const struct rtp_header *hdr,
				struct mbuf *mb, void *arg)
{
	struct audio *a = arg;
	int err;

	if (!mb)
		goto out;

	/* Telephone event? */
	if (hdr->pt == a->pt_tel_rx) {
		handle_telev(a, mb);
		return;
	}

	/* Comfort Noise (CN) as of RFC 3389 */
	if (PT_CN == hdr->pt)
		return;

	/* Audio payload-type changed? */
	if (hdr->pt != a->pt_rx) {

		err = pt_handler(a, a->pt_rx, hdr->pt);
		if (err)
			return;
	}

 out:
	err = audio_stream_decode(a, mb);
	if (err) {
		DEBUG_WARNING("audio_stream_decode failed (%s)\n",
			      strerror(err));
	}
}


int audio_alloc(struct audio **ap, struct call *call,
		struct sdp_session *sdp_sess, int label,
		const struct mnat *mnat, struct mnat_sess *mnat_sess,
		const struct menc *menc, uint32_t ptime,
		audio_event_h *eventh, audio_err_h *errh, void *arg)
{
	struct audio *a;
	struct le *le;
	int err;

	if (!ap)
		return EINVAL;

	a = mem_zalloc(sizeof(*a), audio_destructor);
	if (!a)
		return ENOMEM;

	MAGIC_INIT(a);

	tmr_init(&a->tmr_tx);

	err = stream_alloc(&a->strm, call, sdp_sess, "audio", label,
			   mnat, mnat_sess, menc, stream_recv_handler, a);
	if (err)
		goto out;

	err = sdp_media_set_lattr(stream_sdpmedia(a->strm), true,
				  "ptime", "%u", ptime);
	if (err)
		goto out;

	/* Audio codecs */
	for (le = list_head(ua_aucodecl(call_get_ua(call)));
	     le;
	     le = le->next) {
		struct aucodec *ac = le->data;
		err |= add_audio_codec(stream_sdpmedia(a->strm), ac);
	}

	/* This buffer will grow automatically */
	a->mb_rtp = mbuf_alloc(STREAM_PRESZ + 320);
	if (!a->mb_rtp) {
		err = ENOMEM;
		goto out;
	}

	err = telev_alloc(&a->telev, TELEV_PTIME);
	if (err)
		goto out;

	a->pt_tx     = a->pt_rx     = PT_NONE;
	a->pt_tel_tx = a->pt_tel_rx = PT_NONE;
	a->ptime_tx  = a->ptime_rx  = ptime;
	a->ts_tx     = 160;
	a->marker    = true;
	a->eventh    = eventh;
	a->errh      = errh;
	a->arg       = arg;

#if TARGET_OS_IPHONE

#ifdef HAVE_PTHREAD
	a->mode = AUDIO_MODE_THREAD;
#else
	a->mode = AUDIO_MODE_POLL;
#endif

#else /* #if defined (TARGET_OS_IPHONE) */

#ifdef ANDROID
	a->mode = AUDIO_MODE_TMR;
#else
	a->mode = AUDIO_MODE_POLL;
#endif

#endif /* #if defined (TARGET_OS_IPHONE) */

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
#endif


static void timeout_tx(void *arg)
{
	struct audio *a = arg;

	tmr_start(&a->tmr_tx, 5, timeout_tx, a);

	poll_aubuf_tx(a);
}


/**
 * Setup the audio-filter chain
 *
 * must be called before auplay/ausrc-alloc
 */
static int aufilt_setup(struct audio *a, uint32_t *srate_enc,
			uint32_t *srate_dec)
{
	struct aufilt_prm encprm, decprm;

	/* Encoder */
	if (a->enc) {
		const struct range *srate_src = &config.audio.srate_src;
		const struct aucodec *ac = aucodec_get(a->enc);

		if (srate_src->min)
			encprm.srate = max(srate_src->min, get_srate(ac));
		else if (srate_src->max)
			encprm.srate = min(srate_src->max, get_srate(ac));
		else
			encprm.srate = get_srate(ac);

		encprm.srate_out  = get_srate(ac);
		encprm.ch         = ac->ch;
		encprm.frame_size = calc_nsamp(encprm.srate_out, encprm.ch,
					       a->ptime_tx);
		encprm.aec_len    = config.audio.aec_len;

		/* read back updated sample-rate */
		*srate_enc = encprm.srate;
	}
	else
		memset(&encprm, 0, sizeof(encprm));

	/* Decoder */
	if (a->dec) {
		const struct range *srate_play = &config.audio.srate_play;
		const struct aucodec *ac = aucodec_get(a->dec);

		if (srate_play->min)
			decprm.srate_out = max(srate_play->min, get_srate(ac));
		else if (srate_play->max)
			decprm.srate_out = min(srate_play->max, get_srate(ac));
		else
			decprm.srate_out = get_srate(ac);

		decprm.srate      = get_srate(ac);
		decprm.ch         = ac->ch;
		decprm.frame_size = calc_nsamp(encprm.srate, encprm.ch,
					       a->ptime_rx);
		decprm.aec_len    = config.audio.aec_len;

		/* read back updated sample-rate */
		*srate_dec = decprm.srate_out;
	}
	else
		memset(&decprm, 0, sizeof(decprm));

	return aufilt_chain_alloc(&a->fc, &encprm, &decprm);
}


/**
 * Start the audio playback and recording
 *
 * @param a Audio object
 *
 * @return 0 if success, otherwise errorcode
 */
int audio_start(struct audio *a)
{
	uint32_t srate_enc = 0, srate_dec = 0;
	const char *dev = config.audio.device;
	int err;

	if (!a)
		return EINVAL;

	if (!str_len(dev))
		dev = NULL;

	err = stream_start(a->strm);
	if (err)
		return err;

	/* Audio filter */
	if (!a->fc && !list_isempty(aufilt_list())) {
		err = aufilt_setup(a, &srate_enc, &srate_dec);
		if (err)
			return err;
	}

	/* TODO: configurable order of play/src start */

	/* Start Audio Player */
	if (!a->auplay && auplay_find(NULL) && a->dec) {

		const struct aucodec *ac = aucodec_get(a->dec);
		struct auplay_prm prm;

		prm.fmt        = AUFMT_S16LE;
		prm.srate      = srate_dec ? srate_dec : get_srate(ac);
		prm.ch         = ac->ch;
		prm.frame_size = calc_nsamp(prm.srate, prm.ch, a->ptime_rx);

		if (!a->aubuf_rx) {
			const size_t psize = 2 * prm.frame_size;

			err = aubuf_alloc(&a->aubuf_rx, psize * 1, psize * 8);
			if (err)
				return err;
		}

		err = auplay_alloc(&a->auplay, NULL, &prm, dev,
				   auplay_write_handler, a);
		if (err) {
			DEBUG_WARNING("start: audio player failed: %s\n",
				      strerror(err));
			return err;
		}
	}

	/* Start Audio Source */
	if (!a->ausrc && ausrc_find(NULL) && a->enc) {

		const struct aucodec *ac = aucodec_get(a->enc);
		struct ausrc_prm prm;

		prm.fmt        = AUFMT_S16LE;
		prm.srate      = srate_enc ? srate_enc : get_srate(ac);
		prm.ch         = ac->ch;
		prm.frame_size = calc_nsamp(prm.srate, prm.ch, a->ptime_tx);

		a->psize = 2 * prm.frame_size;

		if (!a->aubuf_tx) {
			err = aubuf_alloc(&a->aubuf_tx, a->psize * 2,
					  a->psize * 30);
			if (err)
				return err;
		}

		err = ausrc_alloc(&a->ausrc, NULL, &prm, dev,
				  ausrc_read_handler, ausrc_error_handler, a);
		if (err) {
			DEBUG_WARNING("start: audio source failed: %s\n",
				      strerror(err));
			return err;
		}

		switch (a->mode) {
#ifdef HAVE_PTHREAD
		case AUDIO_MODE_THREAD:
			if (!a->run_tx) {
				a->run_tx = true;
				err = pthread_create(&a->tid_tx, NULL,
						     tx_thread, a);
				if (err) {
					a->run_tx = false;
					return err;
				}
			}
			break;
#endif

		case AUDIO_MODE_TMR:
			tmr_start(&a->tmr_tx, 1, timeout_tx, a);
			break;

		default:
			break;
		}
	}

	return err;
}


/**
 * Stop the audio playback and recording
 *
 * @param a Audio object
 */
void audio_stop(struct audio *a)
{
	void *p;

	if (!a)
		return;

	switch (a->mode) {

#ifdef HAVE_PTHREAD
	case AUDIO_MODE_THREAD:
		if (a->run_tx) {
			a->run_tx = false;
			pthread_join(a->tid_tx, NULL);
		}
		break;
#endif
	case AUDIO_MODE_TMR:
		tmr_cancel(&a->tmr_tx);
		break;

	default:
		break;
	}

	/* audio device must be stopped first */
	a->ausrc    = mem_deref(a->ausrc);
	a->auplay   = mem_deref(a->auplay);

	p = a->fc;
	a->fc = NULL;
	mem_deref(p);
	a->aubuf_tx = mem_deref(a->aubuf_tx);
	a->aubuf_rx = mem_deref(a->aubuf_rx);
}


int audio_encoder_set(struct audio *a, struct aucodec *ac,
		      uint8_t pt_tx, const char *params)
{
	struct pl fmtp = pl_null;
	struct aucodec *ac_old;
	int err = 0;

	if (!a || !ac)
		return EINVAL;

	(void)re_fprintf(stderr, "Set audio encoder: %s %uHz %dch\n",
			 ac->name, get_srate(ac), ac->ch);

	ac_old = aucodec_get(a->enc);
	pl_set_str(&fmtp, params);

	a->is_g722 = (0 == str_casecmp(ac->name, "G722"));
	a->pt_tx = pt_tx;
	a->enc = mem_deref(a->enc);

	if (aucodec_cmp(ac, aucodec_get(a->dec))) {

		a->enc = mem_ref(a->dec);
	}
	else {
		struct aucodec_prm prm;

		prm.srate = get_srate(ac);
		prm.ptime = a->ptime_tx;

		err = ac->alloch(&a->enc, ac, &prm, NULL, &fmtp);
		if (err) {
			DEBUG_WARNING("alloc encoder: %s\n", strerror(err));
			return err;
		}

		a->ptime_tx = prm.ptime;
	}

	stream_set_srate(a->strm, get_srate(ac), get_srate(ac));

	if (ac_old && !aucodec_equal(ac_old, ac)) {

		a->ausrc = mem_deref(a->ausrc);

		err |= audio_start(a);
	}

	return err;
}


int audio_decoder_set(struct audio *a, struct aucodec *ac, uint8_t pt_rx)
{
	struct aucodec *ac_old;
	int err = 0;

	if (!a || !ac)
		return EINVAL;

	(void)re_fprintf(stderr, "Set audio decoder: %s %uHz %dch\n",
			 ac->name, get_srate(ac), ac->ch);

	ac_old = aucodec_get(a->dec);
	a->pt_rx = pt_rx;
	a->dec = mem_deref(a->dec);

	if (aucodec_cmp(ac, aucodec_get(a->enc))) {

		a->dec = mem_ref(a->enc);
	}
	else {
		err = ac->alloch(&a->dec, ac, NULL, NULL, NULL);
		if (err) {
			DEBUG_WARNING("alloc decoder: %s\n", strerror(err));
			return err;
		}
	}

	stream_set_srate(a->strm, get_srate(ac), get_srate(ac));

	if (ac_old && !aucodec_equal(ac_old, ac)) {

		a->auplay = mem_deref(a->auplay);

		/* Reset audio filter chain */
		a->fc = mem_deref(a->fc);

		err |= audio_start(a);
	}

	return err;
}


/*
 * A set of "setter" functions. Use these to "set" any format values, cause
 * they might trigger changes in other components.
 */

static void audio_ptime_tx_set(struct audio *a, uint32_t ptime_tx)
{
	if (ptime_tx != a->ptime_tx) {
		DEBUG_NOTICE("peer changed ptime_tx %u -> %u\n",
			     a->ptime_tx, ptime_tx);
		a->ptime_tx = ptime_tx;

		/* todo: refresh a->psize */
	}
}


void audio_update(struct audio *a, bool speakerphone)
{
	if (a && a->fc)
		(void)aufilt_chain_update(a->fc, speakerphone);
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

	a->pt_tel_tx = pt_tx;
	a->pt_tel_rx = pt_rx;
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

	a->mode = enabled ? AUDIO_MODE_THREAD : AUDIO_MODE_POLL;
}


void audio_sdp_attr_decode(struct audio *a)
{
	const char *attr;

	/* This is probably only meaningful for audio data, but
	   may be used with other media types if it makes sense. */
	attr = sdp_media_rattr(stream_sdpmedia(a->strm), "ptime");
	if (attr)
		audio_ptime_tx_set(a, atoi(attr));

	stream_sdp_attr_decode(a->strm);
}


static int aucodec_print(struct re_printf *pf, const struct aucodec_st *st)
{
	const struct aucodec *ac = aucodec_get(st);

	if (!ac)
		return 0;

	return re_hprintf(pf, "%s %uHz/%dch", ac->name, get_srate(ac), ac->ch);
}


int audio_debug(struct re_printf *pf, const struct audio *a)
{
	int err;

	if (!a)
		return 0;

	err  = re_hprintf(pf, "\n--- Audio stream ---\n");

	err |= re_hprintf(pf, " tx/enc:   %H ptime=%ums pt=%u\n",
			  aucodec_print, a->enc, a->ptime_tx, a->pt_tx);
	err |= re_hprintf(pf, " rx/dec:   %H ptime=%ums pt=%u\n",
			  aucodec_print, a->dec, a->ptime_rx, a->pt_rx);
	err |= re_hprintf(pf, " aubuf_tx: %H\n", aubuf_debug, a->aubuf_tx);
	err |= re_hprintf(pf, " aubuf_rx: %H\n", aubuf_debug, a->aubuf_rx);

	err |= stream_debug(pf, a->strm);

	return err;
}
