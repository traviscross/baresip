/**
 * @file src/audio.c  Audio stream
 *
 * Copyright (C) 2010 Creytiv.com
 * \ref GenericAudioStream
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
#include <re.h>
#include <rem.h>
#include <baresip.h>
#include "core.h"


#define DEBUG_MODULE "audio"
#define DEBUG_LEVEL 5
#include <re_dbg.h>

/** Magic number */
#define MAGIC 0x000a0d10
#include "magic.h"


/**
 * \page GenericAudioStream Generic Audio Stream
 *
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


/** Audio transmit/encoder */
struct autx {
	struct ausrc_st *ausrc;       /**< Audio Source                    */
	struct aucodec_st *enc;       /**< Current audio encoder           */
	struct aubuf *ab;             /**< Packetize outgoing stream       */
	struct mbuf *mb;              /**< Buffer for outgoing RTP packets */
	uint32_t ptime;               /**< Packet time for sending         */
	uint32_t ts;                  /**< Timestamp for outgoing RTP      */
	uint32_t ts_tel;              /**< Timestamp for Telephony Events  */
	size_t psize;                 /**< Packet size for sending         */
	bool marker;                  /**< Marker bit for outgoing RTP     */
	bool is_g722;                 /**< Set if encoder is G.722 codec   */
	bool muted;                   /**< Audio source is muted           */
	int cur_key;                  /**< Currently transmitted event     */

	enum audio_mode mode;         /**< Audio mode for sending packets  */
	union {
		struct tmr tmr;       /**< Timer for sending RTP packets   */
#ifdef HAVE_PTHREAD
		struct {
			pthread_t tid;/**< Audio transmit thread           */
			bool run;     /**< Audio transmit thread running   */
		} thr;
#endif
	} u;

};


/** Audio receive/decoder */
struct aurx {
	struct auplay_st *auplay;     /**< Audio Player                    */
	struct aucodec_st *dec;       /**< Current audio decoder           */
	struct aubuf *ab;             /**< Incoming audio buffer           */
	struct mbuf *mb;              /**< Buffer for decoded audio        */
	uint32_t ptime;               /**< Packet time for receiving       */
	int pt;                       /**< Payload type for incoming RTP   */
	int pt_tel;                   /**< Event payload type - receive    */
};


/** Generic Audio stream */
struct audio {
	MAGIC_DECL                    /**< Magic number for debugging      */
	struct autx tx;               /**< Transmit                        */
	struct aurx rx;               /**< Receive                         */
	struct stream *strm;          /**< Generic media stream            */
	struct aufilt_chain *fc;      /**< Audio filter chain              */
	struct telev *telev;          /**< Telephony events                */
	audio_event_h *eventh;        /**< Event handler                   */
	audio_err_h *errh;            /**< Audio error handler             */
	void *arg;                    /**< Handler argument                */
};


static void audio_destructor(void *arg)
{
	struct audio *a = arg;

	audio_stop(a);

	mem_deref(a->tx.enc);
	mem_deref(a->rx.dec);
	mem_deref(a->tx.ab);
	mem_deref(a->tx.mb);
	mem_deref(a->rx.mb);
	mem_deref(a->rx.ab);
	mem_deref(a->strm);
	mem_deref(a->telev);
}


/**
 * Calculate number of samples from sample rate, channels and packet time
 *
 * @param srate    Sample rate in [Hz]
 * @param channels Number of channels
 * @param ptime    Packet time in [ms]
 *
 * @return Number of samples
 */
static inline uint32_t calc_nsamp(uint32_t srate, uint8_t channels,
				  uint16_t ptime)
{
	return srate * channels * ptime / 1000;
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
			      ac->ch, NULL, ac->cmph, ac, true,
			      "%s", ac->fmtp);
}


/**
 * Encoder audio and send via stream
 *
 * @note This function has REAL-TIME properties
 */
static void encode_rtp_send(struct audio *a, struct autx *tx,
			    struct mbuf *mb, uint16_t nsamp)
{
	int err;

	if (!tx->enc)
		return;

	tx->mb->pos = tx->mb->end = STREAM_PRESZ;

	err = aucodec_get(tx->enc)->ench(tx->enc, tx->mb, mb);
	if (err)
		goto out;

	tx->mb->pos = STREAM_PRESZ;

	if (mbuf_get_left(tx->mb)) {

		err = stream_send(a->strm, tx->marker, -1,
				  tx->ts, tx->mb);
		if (err)
			goto out;
	}

	tx->ts += nsamp;

 out:
	tx->marker = false;
}


/**
 * Process outgoing audio stream
 *
 * @note This function has REAL-TIME properties
 */
static void process_audio_encode(struct audio *a, struct mbuf *mb)
{
	if (!mb)
		return;

	/* Audio filters */
	if (a->fc) {
		(void)aufilt_chain_encode(a->fc, mb);
	}

	/* Encode and send */
	encode_rtp_send(a, &a->tx, mb, a->tx.is_g722 ? mb->end/4 : mb->end/2);
}


static void poll_aubuf_tx(struct audio *a)
{
	struct mbuf *mb = mbuf_alloc(a->tx.psize);
	int err;
	if (!mb)
		return;

	/* timed read from audio-buffer */
	err = aubuf_get(a->tx.ab, a->tx.ptime, mb->buf, mb->size);
	if (0 == err) {
		mb->end = mb->size;
		process_audio_encode(a, mb);
	}

	mem_deref(mb);
}


static void check_telev(struct audio *a, struct autx *tx)
{
	const struct sdp_format *fmt;
	bool marker = false;
	int err;

	tx->mb->pos = tx->mb->end = STREAM_PRESZ;

	err = telev_poll(a->telev, &marker, tx->mb);
	if (err)
		return;

	if (marker)
		tx->ts_tel = tx->ts;

	fmt = sdp_media_rformat(stream_sdpmedia(audio_strm(a)), telev_rtpfmt);
	if (!fmt)
		return;

	tx->mb->pos = STREAM_PRESZ;
	err = stream_send(a->strm, marker, fmt->pt, tx->ts_tel, tx->mb);
	if (err) {
		DEBUG_WARNING("telev: stream_send %m\n", err);
	}
}


/**
 * Write samples to Audio Player.
 *
 * @note This function has REAL-TIME properties
 *
 * @note The application is responsible for filling in silence in
 *       the case of underrun
 *
 * @note This function may be called from any thread
 */
static bool auplay_write_handler(uint8_t *buf, size_t sz, void *arg)
{
	struct audio *a = arg;

	aubuf_read(a->rx.ab, buf, sz);

	return true;
}


/**
 * Read samples from Audio Source
 *
 * @note This function has REAL-TIME properties
 *
 * @note This function may be called from any thread
 */
static void ausrc_read_handler(const uint8_t *buf, size_t sz, void *arg)
{
	struct audio *a = arg;
	struct autx *tx = &a->tx;
	uint8_t *silence = NULL;
	const uint8_t *txbuf = buf;

	/* NOTE:
	 * some devices behave strangely if they receive no RTP,
	 * so we send silence when muted
	 */
	if (tx->muted) {
		silence = mem_zalloc(sizeof(*silence) * sz, NULL);
		txbuf = silence;
	}

	if (tx->ab) {
		if (aubuf_write(tx->ab, txbuf, sz))
			goto out;

		/* XXX: on limited CPU and specifically coreaudio module
		 * calling this procedure, which results in audio encoding,
		 * seems to have an overall negative impact on system
		 * performance! (coming from interrupt context?)
		 */
		if (tx->mode == AUDIO_MODE_POLL)
			poll_aubuf_tx(a);
	}

 out:
	/* Exact timing: send Telephony-Events from here */
	check_telev(a, tx);
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

	return audio_decoder_set(a, lc->data, lc->pt, lc->params);
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
static int audio_stream_decode(struct audio *a, struct aurx *rx,
			       struct mbuf *mb)
{
	int err = 0;
	int n = 64;

	if (!a)
		return EINVAL;

	/* No decoder set */
	if (!rx->dec)
		return 0;

	mbuf_rewind(rx->mb);

	/* Decode all packets */
	do {
		err = aucodec_get(rx->dec)->dech(rx->dec, rx->mb, mb);
	} while (n-- && mbuf_get_left(mb) && !err);

	if (err) {
		DEBUG_WARNING("codec_decode: %m\n", err);
		goto out;
	}

	rx->mb->pos = 0;

	/* Perform operations on the PCM samples */
	if (a->fc) {
		err |= aufilt_chain_decode(a->fc, rx->mb);
	}

	if (rx->ab) {

		err = aubuf_write(rx->ab, rx->mb->buf, rx->mb->end);
		if (err)
			goto out;
	}

 out:
	return err;
}


/* Handle incoming stream data from the network */
static void stream_recv_handler(const struct rtp_header *hdr,
				struct mbuf *mb, void *arg)
{
	struct audio *a = arg;
	struct aurx *rx = &a->rx;
	int err;

	if (!mb)
		goto out;

	/* Telephone event? */
	if (hdr->pt == rx->pt_tel) {
		handle_telev(a, mb);
		return;
	}

	/* Comfort Noise (CN) as of RFC 3389 */
	if (PT_CN == hdr->pt)
		return;

	/* Audio payload-type changed? */
	/* XXX: this logic should be moved to stream.c */
	if (hdr->pt != rx->pt) {

		err = pt_handler(a, rx->pt, hdr->pt);
		if (err)
			return;
	}

 out:
	(void)audio_stream_decode(a, &a->rx, mb);
}


static int add_telev_codec(struct audio *a)
{
	struct sdp_media *m = stream_sdpmedia(audio_strm(a));
	struct sdp_format *sf;
	int err;

	/* Use payload-type 101 if available, for CiscoGW interop */
	err = sdp_format_add(&sf, m, false,
			     (!sdp_media_lformat(m, 101)) ? "101" : NULL,
			     telev_rtpfmt, TELEV_SRATE, 1, NULL,
			     NULL, NULL, false, "0-15");
	if (err)
		return err;

	a->rx.pt_tel = sf->pt;

	return err;
}


int audio_alloc(struct audio **ap, struct call *call,
		struct sdp_session *sdp_sess, int label,
		const struct mnat *mnat, struct mnat_sess *mnat_sess,
		const struct menc *menc, uint32_t ptime, enum audio_mode mode,
		const struct list *aucodecl,
		audio_event_h *eventh, audio_err_h *errh, void *arg)
{
	struct audio *a;
	struct autx *tx;
	struct aurx *rx;
	struct le *le;
	int err;

	if (!ap)
		return EINVAL;

	a = mem_zalloc(sizeof(*a), audio_destructor);
	if (!a)
		return ENOMEM;

	MAGIC_INIT(a);

	tx = &a->tx;
	rx = &a->rx;

	err = stream_alloc(&a->strm, call, sdp_sess, "audio", label,
			   mnat, mnat_sess, menc,
			   stream_recv_handler, NULL, a);
	if (err)
		goto out;

	err = sdp_media_set_lattr(stream_sdpmedia(a->strm), true,
				  "ptime", "%u", ptime);
	if (err)
		goto out;

	/* Audio codecs */
	for (le = list_head(aucodecl); le; le = le->next) {
		err = add_audio_codec(stream_sdpmedia(a->strm), le->data);
		if (err)
			goto out;
	}

	tx->mb = mbuf_alloc(STREAM_PRESZ + 320);
	rx->mb = mbuf_alloc(4 * 320);
	if (!tx->mb || !rx->mb) {
		err = ENOMEM;
		goto out;
	}

	err = telev_alloc(&a->telev, TELEV_PTIME);
	if (err)
		goto out;

	err = add_telev_codec(a);
	if (err)
		goto out;

	tx->ptime  = ptime;
	tx->ts     = 160;
	tx->marker = true;
	tx->mode   = mode;

	rx->pt     = -1;
	rx->ptime  = ptime;

	a->eventh    = eventh;
	a->errh      = errh;
	a->arg       = arg;

	if (mode == AUDIO_MODE_TMR)
		tmr_init(&tx->u.tmr);

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

	/* Enable Real-time mode for this thread, if available */
	if (a->tx.mode == AUDIO_MODE_THREAD_REALTIME)
		(void)realtime_enable(true, 1);

	while (a->tx.u.thr.run) {

		poll_aubuf_tx(a);

		sys_msleep(5);
	}

	return NULL;
}
#endif


static void timeout_tx(void *arg)
{
	struct audio *a = arg;

	tmr_start(&a->tx.u.tmr, 5, timeout_tx, a);

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
	struct autx *tx = &a->tx;
	struct aurx *rx = &a->rx;

	/* Encoder */
	if (tx->enc) {
		const struct range *srate_src = &config.audio.srate_src;
		const struct aucodec *ac = aucodec_get(tx->enc);

		if (srate_src->min)
			encprm.srate = max(srate_src->min, get_srate(ac));
		else if (srate_src->max)
			encprm.srate = min(srate_src->max, get_srate(ac));
		else
			encprm.srate = get_srate(ac);

		encprm.srate_out  = get_srate(ac);
		encprm.ch         = ac->ch;
		encprm.frame_size = calc_nsamp(encprm.srate_out, encprm.ch,
					       tx->ptime);

		/* read back updated sample-rate */
		*srate_enc = encprm.srate;
	}
	else
		memset(&encprm, 0, sizeof(encprm));

	/* Decoder */
	if (rx->dec) {
		const struct range *srate_play = &config.audio.srate_play;
		const struct aucodec *ac = aucodec_get(rx->dec);

		if (srate_play->min)
			decprm.srate_out = max(srate_play->min, get_srate(ac));
		else if (srate_play->max)
			decprm.srate_out = min(srate_play->max, get_srate(ac));
		else
			decprm.srate_out = get_srate(ac);

		decprm.srate      = get_srate(ac);
		decprm.ch         = ac->ch;
		decprm.frame_size = calc_nsamp(encprm.srate, encprm.ch,
					       rx->ptime);

		/* read back updated sample-rate */
		*srate_dec = decprm.srate_out;
	}
	else
		memset(&decprm, 0, sizeof(decprm));

	return aufilt_chain_alloc(&a->fc, &encprm, &decprm);
}


static int start_player(struct audio *a, uint32_t srate_dec)
{
	struct aurx *rx = &a->rx;
	int err;

	/* Start Audio Player */
	if (!rx->auplay && auplay_find(NULL) && rx->dec) {

		const struct aucodec *ac = aucodec_get(rx->dec);
		struct auplay_prm prm;

		prm.fmt        = AUFMT_S16LE;
		prm.srate      = srate_dec ? srate_dec : get_srate(ac);
		prm.ch         = ac->ch;
		prm.frame_size = calc_nsamp(prm.srate, prm.ch, rx->ptime);

		if (!rx->ab) {
			const size_t psize = 2 * prm.frame_size;

			err = aubuf_alloc(&rx->ab, psize * 1, psize * 8);
			if (err)
				return err;
		}

		err = auplay_alloc(&rx->auplay, config.audio.play_mod,
				   &prm, config.audio.play_dev,
				   auplay_write_handler, a);
		if (err) {
			DEBUG_WARNING("start_player failed: %m\n", err);
			return err;
		}
	}

	return 0;
}


static int start_source(struct audio *a, uint32_t srate_enc)
{
	struct autx *tx = &a->tx;
	int err;

	/* Start Audio Source */
	if (!tx->ausrc && ausrc_find(NULL) && tx->enc) {

		const struct aucodec *ac = aucodec_get(tx->enc);
		struct ausrc_prm prm;

		prm.fmt        = AUFMT_S16LE;
		prm.srate      = srate_enc ? srate_enc : get_srate(ac);
		prm.ch         = ac->ch;
		prm.frame_size = calc_nsamp(prm.srate, prm.ch, tx->ptime);

		tx->psize = 2 * prm.frame_size;

		if (!tx->ab) {
			err = aubuf_alloc(&tx->ab, tx->psize * 2,
					  tx->psize * 30);
			if (err)
				return err;
		}

		err = ausrc_alloc(&tx->ausrc, NULL, config.audio.src_mod,
				  &prm, config.audio.src_dev,
				  ausrc_read_handler, ausrc_error_handler, a);
		if (err) {
			DEBUG_WARNING("start_source failed: %m\n", err);
			return err;
		}

		switch (tx->mode) {
#ifdef HAVE_PTHREAD
		case AUDIO_MODE_THREAD:
		case AUDIO_MODE_THREAD_REALTIME:
			if (!tx->u.thr.run) {
				tx->u.thr.run = true;
				err = pthread_create(&tx->u.thr.tid, NULL,
						     tx_thread, a);
				if (err) {
					tx->u.thr.tid = false;
					return err;
				}
			}
			break;
#endif

		case AUDIO_MODE_TMR:
			tmr_start(&tx->u.tmr, 1, timeout_tx, a);
			break;

		default:
			break;
		}
	}

	return 0;
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
	int err;

	if (!a)
		return EINVAL;

	err = stream_start(a->strm);
	if (err)
		return err;

	/* Audio filter */
	if (!a->fc && !list_isempty(aufilt_list())) {
		err = aufilt_setup(a, &srate_enc, &srate_dec);
		if (err)
			return err;
	}

	/* configurable order of play/src start */
	if (config.audio.src_first) {
		err |= start_source(a, srate_enc);
		err |= start_player(a, srate_dec);
	}
	else {
		err |= start_player(a, srate_dec);
		err |= start_source(a, srate_enc);
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
	struct autx *tx;
	struct aurx *rx;

	if (!a)
		return;

	tx = &a->tx;
	rx = &a->rx;

	switch (tx->mode) {

#ifdef HAVE_PTHREAD
	case AUDIO_MODE_THREAD:
	case AUDIO_MODE_THREAD_REALTIME:
		if (tx->u.thr.run) {
			tx->u.thr.run = false;
			pthread_join(tx->u.thr.tid, NULL);
		}
		break;
#endif
	case AUDIO_MODE_TMR:
		tmr_cancel(&tx->u.tmr);
		break;

	default:
		break;
	}

	/* audio device must be stopped first */
	tx->ausrc  = mem_deref(tx->ausrc);
	rx->auplay = mem_deref(rx->auplay);

	a->fc  = mem_deref(a->fc);
	tx->ab = mem_deref(tx->ab);
	rx->ab = mem_deref(rx->ab);
}


int audio_encoder_set(struct audio *a, struct aucodec *ac,
		      int pt_tx, const char *params)
{
	struct aucodec *ac_old;
	struct autx *tx;
	bool reset;
	int err = 0;

	if (!a || !ac)
		return EINVAL;

	tx = &a->tx;

	(void)re_fprintf(stderr, "Set audio encoder: %s %uHz %dch\n",
			 ac->name, get_srate(ac), ac->ch);

	ac_old = aucodec_get(tx->enc);

	reset = ac_old && !aucodec_equal(ac_old, ac);

	/* Audio source must be stopped first */
	if (reset) {
		tx->ausrc = mem_deref(tx->ausrc);
	}

	tx->is_g722 = (0 == str_casecmp(ac->name, "G722"));
	tx->enc = mem_deref(tx->enc);

	if (aucodec_cmp(ac, aucodec_get(a->rx.dec))) {

		tx->enc = mem_ref(a->rx.dec);
	}
	else {
		struct aucodec_prm prm;

		prm.srate = get_srate(ac);
		prm.ptime = a->tx.ptime;

		err = ac->alloch(&tx->enc, ac, &prm, NULL, params);
		if (err) {
			DEBUG_WARNING("alloc encoder: %m\n", err);
			return err;
		}

		a->tx.ptime = prm.ptime;
	}

	stream_set_srate(a->strm, get_srate(ac), get_srate(ac));
	stream_update_encoder(a->strm, pt_tx);

	if (reset) {

		err |= audio_start(a);
	}

	return err;
}


int audio_decoder_set(struct audio *a, struct aucodec *ac,
		      int pt_rx, const char *params)
{
	struct aucodec *ac_old;
	struct aurx *rx;
	int err = 0;

	if (!a || !ac)
		return EINVAL;

	rx = &a->rx;

	(void)re_fprintf(stderr, "Set audio decoder: %s %uHz %dch\n",
			 ac->name, get_srate(ac), ac->ch);

	ac_old = aucodec_get(rx->dec);
	rx->pt = pt_rx;
	rx->dec = mem_deref(rx->dec);

	if (aucodec_cmp(ac, aucodec_get(a->tx.enc))) {

		rx->dec = mem_ref(a->tx.enc);
	}
	else {
		err = ac->alloch(&rx->dec, ac, NULL, NULL, params);
		if (err) {
			DEBUG_WARNING("alloc decoder: %m\n", err);
			return err;
		}
	}

	stream_set_srate(a->strm, get_srate(ac), get_srate(ac));

	if (ac_old && !aucodec_equal(ac_old, ac)) {

		rx->auplay = mem_deref(rx->auplay);

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
	if (ptime_tx != a->tx.ptime) {
		DEBUG_NOTICE("peer changed ptime_tx %u -> %u\n",
			     a->tx.ptime, ptime_tx);
		a->tx.ptime = ptime_tx;

		/* todo: refresh a->psize */
	}
}


struct stream *audio_strm(const struct audio *a)
{
	return a ? a->strm : NULL;
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
	else if (a->tx.cur_key) {
		/* Key release */
		(void)re_printf("send DTMF digit end: '%c'\n", a->tx.cur_key);
		err = telev_send(a->telev,
				 telev_digit2code(a->tx.cur_key), true);
	}

	a->tx.cur_key = key;

	return err;
}


/**
 * Mute the audio stream
 *
 * @param a      Audio stream
 * @param muted  True to mute, false to un-mute
 */
void audio_mute(struct audio *a, bool muted)
{
	if (!a)
		return;

	a->tx.muted = muted;
}


void audio_sdp_attr_decode(struct audio *a)
{
	const char *attr;

	if (!a)
		return;

	/* This is probably only meaningful for audio data, but
	   may be used with other media types if it makes sense. */
	attr = sdp_media_rattr(stream_sdpmedia(a->strm), "ptime");
	if (attr)
		audio_ptime_tx_set(a, atoi(attr));
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
	const struct autx *tx;
	const struct aurx *rx;
	int err;

	if (!a)
		return 0;

	tx = &a->tx;
	rx = &a->rx;

	err  = re_hprintf(pf, "\n--- Audio stream ---\n");

	err |= re_hprintf(pf, " tx:   %H %H ptime=%ums\n",
			  aucodec_print, tx->enc,
			  aubuf_debug, tx->ab,
			  tx->ptime);

	err |= re_hprintf(pf, " rx:   %H %H ptime=%ums pt=%d\n",
			  aucodec_print, rx->dec,
			  aubuf_debug, rx->ab,
			  rx->ptime, rx->pt);

	err |= stream_debug(pf, a->strm);

	return err;
}
