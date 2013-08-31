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

enum {
	AUDIO_SAMPSZ    = 1920,
};


/**
 * Audio transmit/encoder
 *
 *
 \verbatim

 Processing encoder pipeline:

 .    .-------.   .-------.   .--------.   .--------.   .--------.
 |    |       |   |       |   |        |   |        |   |        |
 |O-->| ausrc |-->| aubuf |-->| resamp |-->| aufilt |-->| encode |---> RTP
 |    |       |   |       |   |        |   |        |   |        |
 '    '-------'   '-------'   '--------'   '--------'   '--------'

 \endverbatim
 *
 */
struct autx {
	struct ausrc_st *ausrc;       /**< Audio Source                    */
	const struct aucodec *ac;     /**< Current audio encoder           */
	struct auenc_state *enc;      /**< Audio encoder state (optional)  */
	struct aubuf *ab;             /**< Packetize outgoing stream       */
	struct auresamp *resamp;      /**< Optional resampler for DSP      */
	struct mbuf *mb;              /**< Buffer for outgoing RTP packets */
	int16_t *sampv;               /**< Sample buffer                   */
	int16_t *sampv_rs;            /**< Sample buffer for resampler     */
	uint32_t ptime;               /**< Packet time for sending         */
	uint32_t ts;                  /**< Timestamp for outgoing RTP      */
	uint32_t ts_tel;              /**< Timestamp for Telephony Events  */
	size_t psize;                 /**< Packet size for sending         */
	bool marker;                  /**< Marker bit for outgoing RTP     */
	bool is_g722;                 /**< Set if encoder is G.722 codec   */
	bool muted;                   /**< Audio source is muted           */
	int cur_key;                  /**< Currently transmitted event     */

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


/**
 * Audio receive/decoder
 *
 \verbatim

 Processing decoder pipeline:

       .--------.   .-------.   .--------.   .--------.   .--------.
 |\    |        |   |       |   |        |   |        |   |        |
 | |<--| auplay |<--| aubuf |<--| resamp |<--| aufilt |<--| decode |<--- RTP
 |/    |        |   |       |   |        |   |        |   |        |
       '--------'   '-------'   '--------'   '--------'   '--------'

 \endverbatim
 */
struct aurx {
	struct auplay_st *auplay;     /**< Audio Player                    */
	const struct aucodec *ac;     /**< Current audio decoder           */
	struct audec_state *dec;      /**< Audio decoder state (optional)  */
	struct aubuf *ab;             /**< Incoming audio buffer           */
	struct auresamp *resamp;      /**< Optional resampler for DSP      */
	int16_t *sampv;               /**< Sample buffer                   */
	int16_t *sampv_rs;            /**< Sample buffer for resampler     */
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
	struct list filtl;            /**< Audio filters (struct aufilt_st)*/
	struct telev *telev;          /**< Telephony events                */
	struct config_audio cfg;      /**< Audio configuration             */
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
	mem_deref(a->tx.sampv);
	mem_deref(a->rx.sampv);
	mem_deref(a->rx.ab);
	mem_deref(a->tx.sampv_rs);
	mem_deref(a->tx.resamp);
	mem_deref(a->rx.sampv_rs);
	mem_deref(a->rx.resamp);

	mem_deref(a->strm);
	mem_deref(a->telev);
	list_flush(&a->filtl);
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


static inline uint32_t get_framesize(const struct aucodec *ac,
				     uint32_t ptime)
{
	if (!ac)
		return 0;

	return calc_nsamp(get_srate(ac), ac->ch, ptime);
}


static bool aucodec_equal(const struct aucodec *a, const struct aucodec *b)
{
	if (!a || !b)
		return false;

	return get_srate(a) == get_srate(b) && a->ch == b->ch;
}


static int add_audio_codec(struct audio *a, struct sdp_media *m,
			   struct aucodec *ac)
{
	if (!in_range(&a->cfg.srate, ac->srate)) {
		DEBUG_INFO("skip codec with %uHz (audio range %uHz - %uHz)\n",
			   ac->srate, a->cfg.srate.min, a->cfg.srate.max);
		return 0;
	}

	if (!in_range(&a->cfg.channels, ac->ch)) {
		DEBUG_INFO("skip codec with %uch (audio range %uch-%uch)\n",
			   ac->ch, a->cfg.channels.min, a->cfg.channels.max);
		return 0;
	}

	return sdp_format_add(NULL, m, false, ac->pt, ac->name, ac->srate,
			      ac->ch, ac->fmtp_ench, ac->fmtp_cmph, ac, false,
			      "%s", ac->fmtp);
}


/**
 * Encoder audio and send via stream
 *
 * @note This function has REAL-TIME properties
 */
static void encode_rtp_send(struct audio *a, struct autx *tx,
			    int16_t *sampv, size_t sampc)
{
	size_t len;
	int err;

	if (!tx->ac)
		return;

	tx->mb->pos = tx->mb->end = STREAM_PRESZ;
	len = mbuf_get_space(tx->mb);

	err = tx->ac->ench(tx->enc, mbuf_buf(tx->mb), &len, sampv, sampc);
	if (err) {
		DEBUG_WARNING("%s encode error: %d samples (%m)\n",
			      tx->ac->name, sampc, err);
		goto out;
	}

	tx->mb->pos = STREAM_PRESZ;
	tx->mb->end = STREAM_PRESZ + len;

	if (mbuf_get_left(tx->mb)) {

		err = stream_send(a->strm, tx->marker, -1, tx->ts, tx->mb);
		if (err)
			goto out;
	}

	tx->ts += (uint32_t)(tx->is_g722 ? sampc/2 : sampc);

 out:
	tx->marker = false;
}


/*
 * @note This function has REAL-TIME properties
 */
static void poll_aubuf_tx(struct audio *a)
{
	size_t sampc = a->tx.psize / 2;
	struct autx *tx = &a->tx;
	int16_t *sampv = tx->sampv;
	struct le *le;
	int err = 0;

	/* timed read from audio-buffer */
	if (aubuf_get_samp(tx->ab, tx->ptime, tx->sampv, sampc))
		return;

	/* optional resampler */
	if (tx->resamp) {
		size_t sampc_rs = AUDIO_SAMPSZ;

		err = auresamp_process(tx->resamp,
				       tx->sampv_rs, &sampc_rs,
				       tx->sampv, sampc);
		if (err)
			return;

		sampv = tx->sampv_rs;
		sampc = sampc_rs;
	}

	/* Process exactly one audio-frame in list order */
	for (le = a->filtl.head; le; le = le->next) {
		struct aufilt_st *st = le->data;

		if (st->af->ench)
			err |= st->af->ench(st, sampv, &sampc);
	}

	/* Encode and send */
	encode_rtp_send(a, tx, sampv, sampc);
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
 *
 * @return true for valid audio samples, false for silence
 */
static bool auplay_write_handler(uint8_t *buf, size_t sz, void *arg)
{
	struct aurx *rx = arg;

	aubuf_read(rx->ab, buf, sz);

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
		if (a->cfg.txmode == AUDIO_MODE_POLL)
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

	if (pt_old != (uint8_t)-1) {
		(void)re_fprintf(stderr, "Audio decoder changed payload"
				 " %u -> %u\n",
				 pt_old, pt_new);
	}

	a->rx.pt = pt_new;

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
	size_t sampc = AUDIO_SAMPSZ;
	int16_t *sampv;
	struct le *le;
	int err = 0;

	if (!a)
		return EINVAL;

	/* No decoder set */
	if (!rx->ac)
		return 0;

	if (mbuf_get_left(mb)) {
		err = rx->ac->dech(rx->dec, rx->sampv, &sampc,
				   mbuf_buf(mb), mbuf_get_left(mb));
	}
	else if (rx->ac->plch) {
		err = rx->ac->plch(rx->dec, rx->sampv, &sampc);
	}
	else {
		/* no PLC in the codec, might be done in filters below */
		sampc = 0;
	}

	if (err) {
		DEBUG_WARNING("%s codec decode %u bytes: %m\n",
			      rx->ac->name, mbuf_get_left(mb), err);
		goto out;
	}

	/* Process exactly one audio-frame in reverse list order */
	for (le = a->filtl.tail; le; le = le->prev) {
		struct aufilt_st *st = le->data;

		if (st->af->dech)
			err |= st->af->dech(st, rx->sampv, &sampc);
	}

	if (!rx->ab)
		goto out;

	sampv = rx->sampv;

	/* optional resampler */
	if (rx->resamp) {
		size_t sampc_rs = AUDIO_SAMPSZ;

		err = auresamp_process(rx->resamp,
				       rx->sampv_rs, &sampc_rs,
				       rx->sampv, sampc);
		if (err)
			return err;

		sampv = rx->sampv_rs;
		sampc = sampc_rs;
	}

	err = aubuf_write_samp(rx->ab, sampv, sampc);
	if (err)
		goto out;

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


int audio_alloc(struct audio **ap, const struct config *cfg,
		struct call *call, struct sdp_session *sdp_sess, int label,
		const struct mnat *mnat, struct mnat_sess *mnat_sess,
		const struct menc *menc, struct menc_sess *menc_sess,
		uint32_t ptime, const struct list *aucodecl,
		audio_event_h *eventh, audio_err_h *errh, void *arg)
{
	struct audio *a;
	struct autx *tx;
	struct aurx *rx;
	struct le *le;
	int err;

	if (!ap || !cfg)
		return EINVAL;

	a = mem_zalloc(sizeof(*a), audio_destructor);
	if (!a)
		return ENOMEM;

	MAGIC_INIT(a);

	a->cfg = cfg->audio;
	tx = &a->tx;
	rx = &a->rx;

	err = stream_alloc(&a->strm, &cfg->avt, call, sdp_sess,
			   "audio", label,
			   mnat, mnat_sess, menc, menc_sess,
			   stream_recv_handler, NULL, a);
	if (err)
		goto out;

	stream_set_bw(a->strm, AUDIO_BANDWIDTH);

	err = sdp_media_set_lattr(stream_sdpmedia(a->strm), true,
				  "ptime", "%u", ptime);
	if (err)
		goto out;

	/* Audio codecs */
	for (le = list_head(aucodecl); le; le = le->next) {
		err = add_audio_codec(a, stream_sdpmedia(a->strm), le->data);
		if (err)
			goto out;
	}

	tx->mb = mbuf_alloc(STREAM_PRESZ + 4096);
	tx->sampv = mem_zalloc(AUDIO_SAMPSZ * 2, NULL);
	rx->sampv = mem_zalloc(AUDIO_SAMPSZ * 2, NULL);
	if (!tx->mb || !tx->sampv || !rx->sampv) {
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

	rx->pt     = -1;
	rx->ptime  = ptime;

	a->eventh    = eventh;
	a->errh      = errh;
	a->arg       = arg;

	if (a->cfg.txmode == AUDIO_MODE_TMR)
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
	if (a->cfg.txmode == AUDIO_MODE_THREAD_REALTIME)
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


static void aufilt_param_set(struct aufilt_prm *prm,
			     const struct aucodec *ac, uint32_t ptime)
{
	if (!ac) {
		DEBUG_WARNING("aufilt param: NO CODEC!\n");
		memset(prm, 0, sizeof(*prm));
		return;
	}

	prm->srate      = get_srate(ac);
	prm->ch         = ac->ch;
	prm->frame_size = calc_nsamp(get_srate(ac), ac->ch, ptime);
}


/**
 * Setup the audio-filter chain
 *
 * must be called before auplay/ausrc-alloc
 */
static int aufilt_setup(struct audio *a)
{
	struct aufilt_prm encprm, decprm;
	struct autx *tx = &a->tx;
	struct aurx *rx = &a->rx;
	struct le *le;
	int err = 0;

	aufilt_param_set(&encprm, tx->ac ? tx->ac : rx->ac, tx->ptime);
	aufilt_param_set(&decprm, rx->ac ? rx->ac : tx->ac, rx->ptime);

	/* Audio filters */
	for (le = list_head(aufilt_list()); le; le = le->next) {
		struct aufilt *af = le->data;
		struct aufilt_st *st = NULL;

		err = af->updh(&st, af, &encprm, &decprm);
		if (err) {
			DEBUG_WARNING("audio-filter '%s' update failed (%m)\n",
				      af->name, err);
			break;
		}

		st->af = af;
		list_append(&a->filtl, &st->le, st);

		if (le == list_head(aufilt_list()))
			(void)re_printf("audio filters: (dsp)");

		(void)re_printf("<--->[%s]", af->name);

		if (le == list_tail(aufilt_list()))
			(void)re_printf("<--->(codec)\n");
	}

	return 0;
}


static int start_player(struct aurx *rx, struct audio *a)
{
	const struct aucodec *ac = rx->ac;
	uint32_t srate_dsp = get_srate(ac);
	int err;

	if (!ac)
		return 0;

	/* Optional resampler, if configured */
	if (a->cfg.srate_play && a->cfg.srate_play != srate_dsp
	    && !rx->resamp) {

		srate_dsp = a->cfg.srate_play;

		(void)re_printf("enable auplay resampler: %u --> %u Hz\n",
				get_srate(ac), srate_dsp);

		rx->sampv_rs = mem_zalloc(AUDIO_SAMPSZ * 2, NULL);
		if (!rx->sampv_rs)
			return ENOMEM;

		err = auresamp_alloc(&rx->resamp, AUDIO_SAMPSZ,
				     get_srate(ac), ac->ch,
				     srate_dsp, ac->ch);
		if (err)
			return err;
	}

	/* Start Audio Player */
	if (!rx->auplay && auplay_find(NULL)) {

		struct auplay_prm prm;

		prm.fmt        = AUFMT_S16LE;
		prm.srate      = srate_dsp;
		prm.ch         = ac->ch;
		prm.frame_size = calc_nsamp(prm.srate, prm.ch, rx->ptime);

		if (!rx->ab) {
			const size_t psize = 2 * prm.frame_size;

			err = aubuf_alloc(&rx->ab, psize * 1, psize * 8);
			if (err)
				return err;
		}

		err = auplay_alloc(&rx->auplay, a->cfg.play_mod,
				   &prm, a->cfg.play_dev,
				   auplay_write_handler, rx);
		if (err) {
			DEBUG_WARNING("start_player failed (%s.%s): %m\n",
				      a->cfg.play_mod,
				      a->cfg.play_dev, err);
			return err;
		}
	}

	return 0;
}


static int start_source(struct autx *tx, struct audio *a)
{
	const struct aucodec *ac = tx->ac;
	uint32_t srate_dsp = get_srate(tx->ac);
	int err;

	if (!ac)
		return 0;

	/* Optional resampler, if configured */
	if (a->cfg.srate_src && a->cfg.srate_src != srate_dsp &&
	    !tx->resamp) {

		srate_dsp = a->cfg.srate_src;

		(void)re_printf("enable ausrc resampler: %u --> %u Hz\n",
				get_srate(ac), srate_dsp);

		tx->sampv_rs = mem_zalloc(AUDIO_SAMPSZ * 2, NULL);
		if (!tx->sampv_rs)
			return ENOMEM;

		err = auresamp_alloc(&tx->resamp, AUDIO_SAMPSZ,
				     srate_dsp, ac->ch,
				     get_srate(ac), ac->ch);
		if (err)
			return err;
	}

	/* Start Audio Source */
	if (!tx->ausrc && ausrc_find(NULL)) {

		struct ausrc_prm prm;

		prm.fmt        = AUFMT_S16LE;
		prm.srate      = srate_dsp;
		prm.ch         = ac->ch;
		prm.frame_size = calc_nsamp(prm.srate, prm.ch, tx->ptime);

		tx->psize = 2 * prm.frame_size;

		if (!tx->ab) {
			err = aubuf_alloc(&tx->ab, tx->psize * 2,
					  tx->psize * 30);
			if (err)
				return err;
		}

		err = ausrc_alloc(&tx->ausrc, NULL, a->cfg.src_mod,
				  &prm, a->cfg.src_dev,
				  ausrc_read_handler, ausrc_error_handler, a);
		if (err) {
			DEBUG_WARNING("start_source failed: %m\n", err);
			return err;
		}

		switch (a->cfg.txmode) {
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
	int err;

	if (!a)
		return EINVAL;

	err = stream_start(a->strm);
	if (err)
		return err;

	/* Audio filter */
	if (!a->filtl.head && !list_isempty(aufilt_list())) {
		err = aufilt_setup(a);
		if (err)
			return err;
	}

	/* configurable order of play/src start */
	if (a->cfg.src_first) {
		err |= start_source(&a->tx, a);
		err |= start_player(&a->rx, a);
	}
	else {
		err |= start_player(&a->rx, a);
		err |= start_source(&a->tx, a);
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

	switch (a->cfg.txmode) {

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

	list_flush(&a->filtl);
	tx->ab = mem_deref(tx->ab);
	rx->ab = mem_deref(rx->ab);
}


int audio_encoder_set(struct audio *a, const struct aucodec *ac,
		      int pt_tx, const char *params)
{
	struct autx *tx;
	int err = 0;
	bool reset;

	if (!a || !ac)
		return EINVAL;

	tx = &a->tx;

	reset = !aucodec_equal(ac, tx->ac);

	if (ac != tx->ac) {
		(void)re_fprintf(stderr, "Set audio encoder: %s %uHz %dch\n",
				 ac->name, get_srate(ac), ac->ch);

		/* Audio source must be stopped first */
		if (reset) {
			tx->ausrc = mem_deref(tx->ausrc);
		}

		tx->is_g722 = (0 == str_casecmp(ac->name, "G722"));
		tx->enc = mem_deref(tx->enc);
		tx->ac = ac;
	}

	if (ac->encupdh) {
		struct auenc_param prm;

		prm.ptime = tx->ptime;

		err = ac->encupdh(&tx->enc, ac, &prm, params);
		if (err) {
			DEBUG_WARNING("alloc encoder: %m\n", err);
			return err;
		}
	}

	stream_set_srate(a->strm, get_srate(ac), get_srate(ac));
	stream_update_encoder(a->strm, pt_tx);

	if (!tx->ausrc) {
		err |= audio_start(a);
	}

	return err;
}


int audio_decoder_set(struct audio *a, const struct aucodec *ac,
		      int pt_rx, const char *params)
{
	struct aurx *rx;
	bool reset = false;
	int err = 0;

	if (!a || !ac)
		return EINVAL;

	rx = &a->rx;

	reset = !aucodec_equal(ac, rx->ac);

	if (ac != rx->ac) {

		(void)re_fprintf(stderr, "Set audio decoder: %s %uHz %dch\n",
				 ac->name, get_srate(ac), ac->ch);

		rx->pt = pt_rx;
		rx->ac = ac;
		rx->dec = mem_deref(rx->dec);
	}

	if (ac->decupdh) {
		err = ac->decupdh(&rx->dec, ac, params);
		if (err) {
			DEBUG_WARNING("alloc decoder: %m\n", err);
			return err;
		}
	}

	stream_set_srate(a->strm, get_srate(ac), get_srate(ac));

	if (reset) {

		rx->auplay = mem_deref(rx->auplay);

		/* Reset audio filter chain */
		list_flush(&a->filtl);

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


static int aucodec_print(struct re_printf *pf, const struct aucodec *ac)
{
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
			  aucodec_print, tx->ac,
			  aubuf_debug, tx->ab,
			  tx->ptime);

	err |= re_hprintf(pf, " rx:   %H %H ptime=%ums pt=%d\n",
			  aucodec_print, rx->ac,
			  aubuf_debug, rx->ab,
			  rx->ptime, rx->pt);

	err |= stream_debug(pf, a->strm);

	return err;
}
