/**
 * @file call.c  Call Control
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <re.h>
#include <baresip.h>
#include "core.h"


#define DEBUG_MODULE "call"
#define DEBUG_LEVEL 5
#include <re_dbg.h>

/** Magic number */
#define MAGIC 0xca11ca11
#include "magic.h"


#ifndef RELEASE
#define CALL_DEBUG       1  /**< Enable call debugging */
#endif

#define FOREACH_STREAM						\
	for (le = call->streaml.head; le; le = le->next)

enum {
	PTIME           = 20,    /**< Packet time for audio               */
	LOCAL_TIMEOUT   = 120,   /**< Incoming call timeout in [seconds]  */
	AUDIO_BANDWIDTH = 128000 /**< Bandwidth for audio in bits/s       */
};


/** Call States */
enum state {
	STATE_IDLE = 0,
	STATE_INCOMING,
	STATE_OUTGOING,
	STATE_RINGING,
	STATE_EARLY,
	STATE_ESTABLISHED,
	STATE_TERMINATED
};

/** SIP Call Control object */
struct call {
	MAGIC_DECL                /**< Magic number for debugging           */
	struct le le;             /**< Linked list element                  */
	struct ua *ua;            /**< SIP User-agent                       */
	struct sipsess *sess;     /**< SIP Session                          */
	struct sdp_session *sdp;  /**< SDP Session                          */
	struct play *play;        /**< Playback of ringtones etc.           */
	struct list streaml;      /**< List of mediastreams (struct stream) */
	struct audio *audio;      /**< Audio stream                         */
	struct video *video;      /**< Video stream                         */
	enum state state;         /**< Call state                           */
	char *local_name;         /**< Local Display name                   */
	char *local_uri;          /**< Local SIP uri                        */
	char *peer_uri;           /**< Peer SIP Address                     */
	char *peer_name;          /**< Peer display name                    */
	char *cuser;              /**< SIP Contact username                 */
	struct tmr tmr_inv;       /**< Timer for incoming calls             */
	time_t time_start;        /**< Time when call started               */
	time_t time_stop;         /**< Time when call stopped               */
	bool got_offer;           /**< Got SDP Offer from Peer              */
	uint8_t pt_telev_rx;      /**< Payload type for incoming tel-events */
	struct mnat_sess *mnats;  /**< Media NAT session                    */
	const struct mnat *mnat;  /**< Media NAT object                     */
	bool mnat_wait;           /**< Waiting for MNAT to establish        */
	call_event_h *eh;         /**< Event handler                        */
	void *arg;                /**< Handler argument                     */
};


static int send_invite(struct call *call);


static const char *state_name(enum state st)
{
	switch (st) {

	case STATE_IDLE:        return "IDLE";
	case STATE_INCOMING:    return "INCOMING";
	case STATE_OUTGOING:    return "OUTGOING";
	case STATE_RINGING:     return "RINGING";
	case STATE_EARLY:       return "EARLY";
	case STATE_ESTABLISHED: return "ESTABLISHED";
	case STATE_TERMINATED:  return "TERMINATED";
	default:                return "???";
	}
}


static void set_state(struct call *call, enum state st)
{
	DEBUG_INFO("State %s -> %s\n", state_name(call->state),
		   state_name(st));
	call->state = st;
}


static int add_telev_codec(struct call *call, struct sdp_media *m)
{
	struct sdp_format *sf;
	int err;

	/* Use payload-type 101 if available, for CiscoGW interop */
	err = sdp_format_add(&sf, m, false,
			     (!sdp_media_lformat(m, 101)) ? "101" : NULL,
			     telev_rtpfmt, TELEV_SRATE, 1,
			     NULL, NULL, false, "0-15");
	if (err)
		return err;

	call->pt_telev_rx = sf->pt;

	return err;
}


/** Populate all codecs from modules */
static int call_codecs_populate(struct call *call)
{
	int err = 0;

	err |= add_telev_codec(call,
			       stream_sdpmedia(audio_strm(call->audio)));

	return err;
}


static void set_telev_pt(struct call *call, struct sdp_media *sdpm)
{
	const struct sdp_format *sc;

	sc = sdp_media_rformat(sdpm, telev_rtpfmt);
	if (!sc) {

		/* NOTE: we force telephone-event if other peer
		         does not support it */
		(void)re_printf("no remote pt found for telev -- forcing\n");
		sc = sdp_media_lformat(sdpm, call->pt_telev_rx);
	}
	if (!sc) {
		(void)re_printf("no remote pt found for telev - disabling\n");
		return;
	}

	audio_enable_telev(call->audio, sc->pt, call->pt_telev_rx);
}


static void call_stream_start(struct call *call, bool active)
{
	const struct sdp_format *sc;
	int err;

	/* Audio Stream */
	sc = sdp_media_rformat(stream_sdpmedia(audio_strm(call->audio)), NULL);
	if (sc) {
		struct aucodec *ac = sc->data;

		stream_remote_set(audio_strm(call->audio), call->local_uri);

		if (ac) {
			err  = audio_encoder_set(call->audio, sc->data,
						 sc->pt, sc->params);
			err |= audio_decoder_set(call->audio, sc->data,
						 sc->pt, sc->params);
			if (!err) {
				err = audio_start(call->audio);
			}
			if (err) {
				DEBUG_WARNING("audio stream: %s\n",
					      strerror(err));
			}
		}
		else {
			(void)re_printf("no common audio-codecs..\n");
		}

		set_telev_pt(call, stream_sdpmedia(audio_strm(call->audio)));
	}
	else {
		(void)re_printf("audio stream is disabled..\n");
	}

#ifdef USE_VIDEO
	/* Video Stream */
	sc = sdp_media_rformat(stream_sdpmedia(video_strm(call->video)), NULL);
	if (sc) {
		(void)re_printf("enable video stream [%s]\n", sc->params);

		stream_remote_set(video_strm(call->video), call->local_uri);

		err  = video_encoder_set(call->video, sc->data, sc->pt,
					 sc->params);
		err |= video_decoder_set(call->video, sc->data, sc->pt);
		if (!err) {
			err = video_start(call->video, NULL,
					  config.video.device, call->peer_uri);
		}
		if (err) {
			DEBUG_WARNING("video stream: %s\n", strerror(err));
		}
	}
	else {
		(void)re_printf("video stream is disabled..\n");
	}
#endif

	if (active) {
		struct le *le;

		tmr_cancel(&call->tmr_inv);
		call->time_start = time(NULL);

		FOREACH_STREAM {
			stream_reset(le->data);
			stream_start_keepalive(le->data);
		}
	}
}


static void call_stream_stop(struct call *call)
{
	if (!call)
		return;

	call->time_stop = time(NULL);

	/* Audio */
	audio_stop(call->audio);

	/* Video */
#ifdef USE_VIDEO
	video_stop(call->video);
#endif

	tmr_cancel(&call->tmr_inv);
}


static void call_event_handler(struct call *call, enum call_event ev,
			       const char *prm)
{
	call_event_h *eh = call->eh;
	void *eh_arg = call->arg;

	if (eh)
		eh(ev, prm, eh_arg);
}


static void invite_timeout(void *arg)
{
	struct call *call = arg;

	(void)re_printf("%s: Local timeout after %u seconds\n",
			call->peer_uri, LOCAL_TIMEOUT);

	call_event_handler(call, CALL_EVENT_CLOSED, "Local timeout");
}


static const char *translate_errorcode(uint16_t scode)
{
	switch (scode) {

	case 404: return "notfound.wav";
	case 486: return "busy.wav";
	case 487: return NULL; /* ignore */
	default:  return "error.wav";
	}
}


static void mnat_handle_call(struct call *call)
{
	int err;

	switch (call->state) {

	case STATE_OUTGOING:
		err = send_invite(call);
		if (err) {
			DEBUG_WARNING("mnat: send_invite: %s\n",
				      strerror(err));
		}
		break;

	case STATE_INCOMING:
		call_event_handler(call, CALL_EVENT_INCOMING, call->peer_uri);
		break;

	default:
		DEBUG_WARNING("mnat: unexpected state: %s\n",
			      state_name(call->state));
		break;
	}
}


/** Called when all media streams are established */
static void mnat_handler(int err, uint16_t scode, const char *reason,
			 void *arg)
{
	struct call *call = arg;
	MAGIC_CHECK(call);

	(void)reason;

	if (err || scode) {
		call_event_handler(call, CALL_EVENT_CLOSED, strerror(err));
		return;
	}

	/* Re-INVITE */
	if (!call->mnat_wait) {
		DEBUG_NOTICE("MNAT Established: Send Re-INVITE\n");
		(void)call_modify(call);
		return;
	}

	call->mnat_wait = false;

	mnat_handle_call(call);
}


static int update_media(struct call *call)
{
	struct le *le;
	int err = 0;

	/* media attributes */
	audio_sdp_attr_decode(call->audio);
#ifdef USE_VIDEO
	if (call->video)
		video_sdp_attr_decode(call->video);
#endif

	/* Update remote address on each stream */
	FOREACH_STREAM {
		struct stream *s = le->data;

		if (stream_has_media(s))
			stream_remote_set(s, call->local_uri);
	}

	if (call->mnat && call->mnat->updateh && call->mnats)
		err = call->mnat->updateh(call->mnats);

	return err;
}


static void call_destructor(void *arg)
{
	struct call *call = arg;

	call_stream_stop(call);
	list_unlink(&call->le);

	mem_deref(call->play);
	mem_deref(call->sess);
	mem_deref(call->peer_uri);
	mem_deref(call->peer_name);
	mem_deref(call->local_uri);
	mem_deref(call->local_name);
	mem_deref(call->cuser);
	mem_deref(call->audio);
	mem_deref(call->video);
	mem_deref(call->sdp);
	mem_deref(call->mnats);
}


static void audio_event_handler(int key, bool end, void *arg)
{
	struct call *call = arg;
	MAGIC_CHECK(call);

	(void)re_printf("received event: '%c' (end=%d)\n", key, end);
}


static void audio_error_handler(int err, const char *str, void *arg)
{
	struct call *call = arg;
	MAGIC_CHECK(call);

	if (err) {
		DEBUG_WARNING("Audio error: %s (%s)\n", strerror(err), str);
	}

	(void)call_hangup(call);
}


#ifdef USE_VIDEO
static void video_exclude(const struct stream *strm, const char *excl)
{
	const struct sdp_media *m = stream_sdpmedia(strm);
	struct pl pl;

	pl_set_str(&pl, excl);

	while (pl.l > 0) {

		struct sdp_format *sf;
		struct pl val, comm;
		char cname[64];

		comm.l = 0;
		if (re_regex(pl.p, pl.l, "[^,]+[,]*", &val, &comm))
			break;

		pl_advance(&pl, val.l + comm.l);

		pl_strcpy(&val, cname, sizeof(cname));

		sf = sdp_media_format(m, true, NULL, -1, cname, -1, -1);
		if (sf)
			mem_deref(sf);
	}
}
#endif


/**
 * Allocate a new Call state object
 *
 * @param callp       Pointer to allocated Call state object
 * @param ua          User-Agent
 * @param prm         Call parameters
 * @param mnat        Media NAT traversal (optional)
 * @param stun_user   STUN username
 * @param stun_pass   STUN password
 * @param stun_host   STUN server hostname
 * @param stun_port   STUN server port number
 * @param menc        Media encryption (optional)
 * @param local_name  Local SIP name
 * @param local_uri   Local SIP uri
 * @param cuser       Local contact user
 * @param msg         SIP message for incoming calls
 * @param eh          Call event handler
 * @param arg         Handler argument
 *
 * @return 0 if success, otherwise errorcode
 */
int call_alloc(struct call **callp, struct ua *ua, const struct call_prm *prm,
	       const struct mnat *mnat,
	       const char *stun_user, const char *stun_pass,
	       const char *stun_host, uint16_t stun_port,
	       const struct menc *menc, const char *local_name,
	       const char *local_uri, const char *cuser,
	       const struct sip_msg *msg, call_event_h *eh, void *arg)
{
	struct call *call;
	const uint32_t ptime = prm ? prm->ptime : 0;
	enum audio_mode aumode = prm ? prm->aumode : AUDIO_MODE_POLL;
	enum vidmode vidmode = prm ? prm->vidmode : VIDMODE_OFF;
	bool use_video = true, got_offer = false;
	int label = 0;
	int err = 0;

	if (!callp)
		return EINVAL;

	call = mem_zalloc(sizeof(*call), call_destructor);
	if (!call)
		return ENOMEM;

	MAGIC_INIT(call);

	tmr_init(&call->tmr_inv);

	call->ua     = ua;
	call->mnat   = mnat;
	call->state  = STATE_IDLE;
	call->eh     = eh;
	call->arg    = arg;
	call->pt_telev_rx = PT_NONE;

	err |= str_dup(&call->local_name, local_name);
	err |= str_dup(&call->local_uri, local_uri);
	err |= str_dup(&call->cuser, cuser);
	if (err)
		goto out;

	/* Init SDP info */
	err = sdp_session_alloc(&call->sdp, net_laddr());
	if (err)
		goto out;

	err = sdp_session_set_lattr(call->sdp, true,
				    "tool", "baresip " VERSION);
	if (err)
		goto out;

	/* Check for incoming SDP Offer */
	if (msg && mbuf_get_left(msg->mb))
		got_offer = true;

	/* Initialise media NAT handling */
	if (mnat) {
		err = call->mnat->sessh(&call->mnats, net_dnsc(),
					stun_host, stun_port,
					stun_user, stun_pass,
					call->sdp, !got_offer,
					mnat_handler, call);
		if (err) {
			DEBUG_WARNING("mnat session: %s\n", strerror(err));
			goto out;
		}
	}
	call->mnat_wait = true;

	/* Audio stream */
	err = audio_alloc(&call->audio, call, call->sdp, ++label,
			  mnat, call->mnats, menc,
			  ptime ? ptime : PTIME, aumode,
			  audio_event_handler, audio_error_handler, call);
	if (err)
		goto out;

#ifdef USE_VIDEO
	/* We require at least one video codec, and at least one
	   video source or video display */
	use_video = (vidmode != VIDMODE_OFF)
		&& (list_head(ua_vidcodecl(call->ua)) != NULL)
		&& (NULL != vidsrc_find(NULL) || NULL != vidisp_find(NULL));

	/* Video stream */
	if (use_video) {
 		err = video_alloc(&call->video, call, call->sdp, ++label,
				  mnat, call->mnats, menc, "main");
 		if (err)
			goto out;

		if (vidmode == VIDMODE_SHUTTERED)
			video_set_shuttered(call->video, true);

		/* exclude some codecs for video-calls */
		if (!msg) {
			video_exclude(audio_strm(call->audio),
				      config.video.exclude);
		}
 	}
#else
	(void)use_video;
	(void)vidmode;
#endif

	err = call_codecs_populate(call);
	if (err)
		goto out;

	/* Bandwidth management [bit/s] */

#if 0
	/* todo: needed to full H.264 spec */
	sdp_session_set_lbandwidth(call->sdp, SDP_BANDWIDTH_CT, 4096);
#endif

	if (config.avt.rtp_bw.max >= AUDIO_BANDWIDTH) {
		uint32_t bwa = AUDIO_BANDWIDTH;
#ifdef USE_VIDEO
		uint32_t bwv = config.avt.rtp_bw.max - AUDIO_BANDWIDTH;
#endif

		stream_set_bw(audio_strm(call->audio), bwa);
#ifdef USE_VIDEO
		stream_set_bw(video_strm(call->video), bwv);
#endif
	}
	else {
		DEBUG_WARNING("bandwidth too low (%u bit/s)\n",
			      config.avt.rtp_bw.max);
	}

 out:
	if (err)
		mem_deref(call);
	else
		*callp = call;

	return err;
}


int call_connect(struct call *call, const struct pl *paddr)
{
	struct sip_addr addr;
	int err;

	if (!call || !paddr)
		return EINVAL;

	(void)re_printf("connecting to '%r'..\n", paddr);

	if (0 == sip_addr_decode(&addr, paddr)) {
		err = pl_strdup(&call->peer_uri, &addr.auri);
	}
	else {
		err = pl_strdup(&call->peer_uri, paddr);
	}
	if (err)
		return err;

	set_state(call, STATE_OUTGOING);

	/* If we are using asyncronous medianat like STUN/TURN, then
	 * wait until completed before sending the INVITE */
	if (!call->mnat)
		err = send_invite(call);

	return err;
}


/**
 * Update the current call by sending Re-INVITE or UPDATE
 *
 * @param call Call object
 *
 * @return 0 if success, otherwise errorcode
 */
int call_modify(struct call *call)
{
	struct mbuf *desc;
	int err;

	if (!call)
		return EINVAL;

	err = call_sdp_get(call, &desc, true);
	if (!err)
		err = sipsess_modify(call->sess, desc);

	mem_deref(desc);

	return err;
}


int call_hangup(struct call *call)
{
	int err = 0;

	if (!call)
		return EINVAL;

	switch (call->state) {

	case STATE_INCOMING:
		(void)re_printf("rejecting incoming call from %s\n",
				call->peer_uri);
		(void)sipsess_reject(call->sess, 486, "Rejected", NULL);
		call->play = mem_deref(call->play);
		break;

	default:
		(void)re_printf("terminate call with %s\n", call->peer_uri);
		call->sess = mem_deref(call->sess);
		break;
	}

	set_state(call, STATE_TERMINATED);

	call_stream_stop(call);

	return err;
}


int call_progress(struct call *call)
{
	struct mbuf *desc;
	int err;

	if (!call)
		return EINVAL;

	err = call_sdp_get(call, &desc, false);
	if (err)
		return err;

	err = sipsess_progress(call->sess, 183, "Session Progress",
			       desc, NULL);

	if (!err)
		call_stream_start(call, false);

	mem_deref(desc);

	return 0;
}


int call_answer(struct call *call, uint16_t scode)
{
	struct mbuf *desc;
	int err;

	if (!call || !call->sess)
		return EINVAL;

	if (STATE_INCOMING != call->state) {
		DEBUG_NOTICE("no call to accept (%s)\n",
			     state_name(call->state));
		return 0;
	}

	call->play = mem_deref(call->play);

	(void)re_printf("answering call from %s with %u\n",
			call->peer_uri, scode);

	if (call->got_offer) {

		err = update_media(call);
		if (err)
			return err;
	}

	err = sdp_encode(&desc, call->sdp, !call->got_offer);
	if (err)
		return err;

	err = sipsess_answer(call->sess, scode, "Answering", desc, NULL);

	mem_deref(desc);

	return err;
}


/**
 * Check if the current call has an active audio stream
 *
 * @param call  Call object
 *
 * @return True if active stream, otherwise false
 */
bool call_has_audio(const struct call *call)
{
	if (!call)
		return false;

	return stream_has_media(audio_strm(call->audio));
}


/**
 * Check if the current call has an active video stream
 *
 * @param call  Call object
 *
 * @return True if active stream, otherwise false
 */
bool call_has_video(const struct call *call)
{
	if (!call)
		return false;

#ifdef USE_VIDEO
	return stream_has_media(video_strm(call->video));
#else
	return false;
#endif
}


/**
 * Put the current call on hold/resume
 *
 * @param call  Call object
 * @param hold  True to hold, false to resume
 *
 * @return 0 if success, otherwise errorcode
 */
int call_hold(struct call *call, bool hold)
{
	struct le *le;

	if (!call || !call->sess)
		return EINVAL;

	(void)re_printf("%s %s\n", hold ? "hold" : "resume", call->peer_uri);

	FOREACH_STREAM
		stream_hold(le->data, hold);

	return call_modify(call);
}


int call_ringtone(struct call *call, const char *ringtone)
{
	if (!call)
		return EINVAL;

	return play_file(&call->play, ringtone, -1);
}


int call_sdp_get(const struct call *call, struct mbuf **descp, bool offer)
{
	return sdp_encode(descp, call->sdp, offer);
}


const char *call_peeruri(const struct call *call)
{
	return call ? call->peer_uri : NULL;
}


/**
 * Get the name of the peer
 *
 * @param call  Call object
 *
 * @return Peer name
 */
const char *call_peername(const struct call *call)
{
	return call ? call->peer_name : NULL;
}


static const struct sdp_format *sdp_media_format_cycle(struct sdp_media *m)
{
	struct sdp_format *sf;
	struct list *lst;

 again:
	sf = (struct sdp_format *)sdp_media_rformat(m, NULL);
	if (!sf)
		return NULL;

	lst = sf->le.list;

	/* move top-most codec to end of list */
	list_unlink(&sf->le);
	list_append(lst, &sf->le, sf);

	sf = (struct sdp_format *)sdp_media_rformat(m, NULL);
	if (!str_casecmp(sf->name, telev_rtpfmt))
		goto again;

	return sf;
}


/**
 * Use the next audio encoder in the local list of negotiated codecs
 *
 * @param call  Call object
 */
void call_audioencoder_cycle(struct call *call)
{
	const struct sdp_format *rc = NULL;

	if (!call)
		return;

	rc = sdp_media_format_cycle(stream_sdpmedia(audio_strm(call->audio)));
	if (!rc) {
		(void)re_printf("cycle audio: no remote codec found\n");
		return;
	}

	(void)audio_encoder_set(call->audio, rc->data, rc->pt, rc->params);
}


#ifdef USE_VIDEO
/**
 * Use the next video encoder in the local list of negotiated codecs
 *
 * @param call  Call object
 */
void call_videoencoder_cycle(struct call *call)
{
	const struct sdp_format *rc = NULL;

	if (!call)
		return;

	rc = sdp_media_format_cycle(stream_sdpmedia(video_strm(call->video)));
	if (!rc) {
		(void)re_printf("cycle video: no remote codec found\n");
		return;
	}

	(void)video_encoder_set(call->video, rc->data, rc->pt, rc->params);
}
#endif


int call_debug(struct re_printf *pf, const struct call *call)
{
	int err;

	if (!call)
		return 0;

	err = re_hprintf(pf, "===== Call debug (%s) =====\n",
			 state_name(call->state));

	/* SIP Session debug */
	err |= re_hprintf(pf, "*** ");
	err |= re_hprintf(pf, " mnat=%s peer=%s\n",
			  call->mnat ? call->mnat->id : "none",
			  call->peer_uri);

	/* SDP debug */
	err |= sdp_session_debug(pf, call->sdp);

	return err;
}


int call_status(struct re_printf *pf, const struct call *call)
{
	const uint32_t dur = call_duration(call);
	const uint32_t sec = dur%60%60;
	const uint32_t min = dur/60%60;
	const uint32_t hrs = dur/60/60;
	struct le *le;
	int err;

	if (!call)
		return EINVAL;

	switch (call->state) {

	case STATE_EARLY:
	case STATE_ESTABLISHED:
		break;
	default:
		return 0;
	}

	err = re_hprintf(pf, "\r[%u:%02u:%02u]", hrs, min, sec);

	FOREACH_STREAM
		err |= stream_print(pf, le->data);

	err |= re_hprintf(pf, " (bit/s)");

#ifdef USE_VIDEO
	if (call->video)
		err |= video_print(pf, call->video);
#endif

	err |= audio_print_vu(pf, call->audio);

	return err;
}


int call_jbuf_stat(struct re_printf *pf, const struct call *call)
{
	struct le *le;
	int err = 0;

	if (!call)
		return EINVAL;

	FOREACH_STREAM
		err |= stream_jbuf_stat(pf, le->data);

	return err;
}


void call_enable_vumeter(struct call *call, bool en)
{
	if (!call)
		return;

	audio_enable_vumeter(call->audio, en);
}


/**
 * Send a DTMF digit to the peer
 *
 * @param call  Call object
 * @param key   DTMF digit to send
 *
 * @return 0 if success, otherwise errorcode
 */
int call_send_digit(struct call *call, char key)
{
	if (!call)
		return EINVAL;

	return audio_send_digit(call->audio, key);
}


struct ua *call_get_ua(const struct call *call)
{
	return call ? call->arg : NULL;
}


static int sipsess_auth_handler(char **username, char **password,
				const char *realm, void *arg)
{
	struct call *call = arg;
	return ua_auth(call->ua, username, password, realm);
}


static int sipsess_offer_handler(struct mbuf **descp,
				 const struct sip_msg *msg, void *arg)
{
	const bool got_offer = mbuf_get_left(msg->mb);
	struct call *call = arg;
	int err;

	MAGIC_CHECK(call);

	DEBUG_NOTICE("got re-INVITE%s\n", got_offer ? " (SDP Offer)" : "");

	if (got_offer) {

		/* Decode SDP Offer */
		err = sdp_decode(call->sdp, msg->mb, true);
		if (err)
			return err;

		err = update_media(call);
		if (err)
			return err;
	}

	/* Encode SDP Answer */
	return sdp_encode(descp, call->sdp, !got_offer);
}


static void decode_part(const struct pl *part, struct mbuf *mb)
{
	struct pl hdrs, body;

	if (re_regex(part->p, part->l, "\r\n\r\n[^]+", &body))
		return;

	hdrs.p = part->p;
	hdrs.l = body.p - part->p - 2;

	if (0 == re_regex(hdrs.p, hdrs.l, "application/sdp")) {

		mb->pos += (body.p - (char *)mbuf_buf(mb));
		mb->end  = mb->pos + body.l;
	}
}


/** Decode a multipart/mixed message and find the part with application/sdp */
static int decode_multipart_sdp(const struct pl *ctype, struct mbuf *mb)
{
	struct pl bnd, s, e, p;
	char expr[64];
	int err;

	/* fetch the boundary tag, excluding quotes */
	err = re_regex(ctype->p, ctype->l,
		       "multipart/mixed;[ \t]*boundary=[~]+", NULL, &bnd);
	if (err)
		return err;

	if (re_snprintf(expr, sizeof(expr), "--%r[^]+", &bnd) < 0)
		return ENOMEM;

	/* find 1st boundary */
	err = re_regex((char *)mbuf_buf(mb), mbuf_get_left(mb), expr, &s);
	if (err) {
		DEBUG_NOTICE("multipart: could not find 1st boundary (%s)\n",
			      expr);
		return err;
	}

	/* iterate over each part */
	while (s.l > 2) {
		if (re_regex(s.p, s.l, expr, &e))
			return 0;

		p.p = s.p + 2;
		p.l = e.p - p.p - bnd.l - 2;

		/* valid part in "p" */
		decode_part(&p, mb);

		s = e;
	}

	return 0;
}


static int sipsess_answer_handler(const struct sip_msg *msg, void *arg)
{
	struct call *call = arg;
	int err;

	MAGIC_CHECK(call);

	(void)decode_multipart_sdp(&msg->ctype, msg->mb);

	err = sdp_decode(call->sdp, msg->mb, false);
	if (err) {
		DEBUG_WARNING("answer: sdp_decode: %s\n", strerror(err));
		return err;
	}

	err = update_media(call);
	if (err)
		return err;

	return 0;
}


static void sipsess_estab_handler(const struct sip_msg *msg, void *arg)
{
	struct call *call = arg;

	MAGIC_CHECK(call);

	(void)msg;

	if (call->state == STATE_ESTABLISHED)
		return;

	set_state(call, STATE_ESTABLISHED);

	call->play = mem_deref(call->play);
	call_stream_start(call, true);
	call_event_handler(call, CALL_EVENT_ESTABLISHED, call->peer_uri);
}


static void call_handle_info_req(struct call *call, const struct sip_msg *req)
{
	struct pl body;
	bool pfu;
	int err;

	(void)call;

	pl_set_mbuf(&body, req->mb);

	err = mctrl_handle_media_control(&body, &pfu);
	if (err)
		return;

#ifdef USE_VIDEO
	if (pfu) {
		video_update_picture(call->video);
	}
#endif
}


static void sipsess_info_handler(struct sip *sip, const struct sip_msg *msg,
				 void *arg)
{
	struct call *call = arg;

	if (!pl_strcasecmp(&msg->ctype, "application/media_control+xml")) {
		call_handle_info_req(call, msg);
		(void)sip_reply(sip, msg, 200, "OK");
	}
	else {
		(void)sip_reply(sip, msg, 488, "Not Acceptable Here");
	}
}


static void sipsess_close_handler(int err, const struct sip_msg *msg,
				  void *arg)
{
	struct call *call = arg;
	char reason[128] = "";

	MAGIC_CHECK(call);

	if (err) {
		(void)re_printf("%s: session closed: %s\n",
				call->peer_uri, strerror(err));
	}
	else if (msg) {
		const char *tone = translate_errorcode(msg->scode);

		(void)re_snprintf(reason, sizeof(reason), "%u %r",
				  msg->scode, &msg->reason);

		(void)re_printf("%s: session closed: %u %r\n",
				call->peer_uri, msg->scode, &msg->reason);

		if (tone)
			(void)play_file(NULL, tone, 1);
	}
	else {
		(void)re_printf("%s: session closed\n", call->peer_uri);
	}

	call_stream_stop(call);
	call_event_handler(call, CALL_EVENT_CLOSED, reason);
}


int call_accept(struct call *call, struct sipsess_sock *sess_sock,
		const struct sip_msg *msg, const char *cuser)
{
	bool got_offer;
	int err;

	if (!call || !msg)
		return EINVAL;

	got_offer = (mbuf_get_left(msg->mb) > 0);

	err = pl_strdup(&call->peer_uri, &msg->from.auri);
	if (err)
		return err;

	if (pl_isset(&msg->from.dname)) {
		err = pl_strdup(&call->peer_name, &msg->from.dname);
		if (err)
			return err;
	}

	if (got_offer) {

		err = sdp_decode(call->sdp, msg->mb, true);
		if (err)
			return err;

		call->got_offer = true;
	}

	err = sipsess_accept(&call->sess, sess_sock, msg, 180, "Ringing",
			     cuser, "application/sdp", NULL,
			     sipsess_auth_handler, call, false,
			     sipsess_offer_handler, sipsess_answer_handler,
			     sipsess_estab_handler, sipsess_info_handler,
			     NULL, sipsess_close_handler, call, NULL);
	if (err) {
		DEBUG_WARNING("sipsess_accept: %s\n", strerror(err));
		return err;
	}

	set_state(call, STATE_INCOMING);

	/* New call */
	tmr_start(&call->tmr_inv, LOCAL_TIMEOUT*1000, invite_timeout, call);

	if (!call->mnat)
		call_event_handler(call, CALL_EVENT_INCOMING, call->peer_uri);

	return err;
}


static void sipsess_progr_handler(const struct sip_msg *msg, void *arg)
{
	struct call *call = arg;
	bool media;

	MAGIC_CHECK(call);

	(void)re_printf("SIP Progress: %u %r (%r)\n",
			msg->scode, &msg->reason, &msg->ctype);

	if (msg->scode <= 100)
		return;

	/* check for 18x and content-type
	 *
	 * 1. start media-stream if application/sdp
	 * 2. play local ringback tone if not
	 *
	 * we must also handle changes to/from 180 and 183,
	 * so we reset the media-stream/ringback each time.
	 */
	if (!pl_strcasecmp(&msg->ctype, "application/sdp")
	    && mbuf_get_left(msg->mb)
	    && !sdp_decode(call->sdp, msg->mb, false)) {
		media = true;
	}
	else if (!decode_multipart_sdp(&msg->ctype, msg->mb) &&
		 !sdp_decode(call->sdp, msg->mb, false)) {
		media = true;
	}
	else
		media = false;

	switch (msg->scode) {

	case 180:
		set_state(call, STATE_RINGING);
		break;

	case 183:
		set_state(call, STATE_EARLY);
		break;
	}

	if (media)
		call_event_handler(call, CALL_EVENT_PROGRESS, call->peer_uri);
	else
		call_event_handler(call, CALL_EVENT_RINGING, call->peer_uri);

	call->play = mem_deref(call->play);
	call_stream_stop(call);

	if (media)
		call_stream_start(call, false);
	else
		(void)play_file(&call->play, "ringback.wav", -1);
}


static int send_invite(struct call *call)
{
	const char *routev[1];
	struct mbuf *desc;
	int err;

	routev[0] = ua_outbound(call->ua);

	err = call_sdp_get(call, &desc, true);
	if (err)
		return err;

	err = sipsess_connect(&call->sess, uag_sipsess_sock(),
			      call->peer_uri,
			      call->local_name,
			      call->local_uri,
			      call->cuser,
			      routev[0] ? routev : NULL,
			      routev[0] ? 1 : 0,
			      "application/sdp", desc,
			      sipsess_auth_handler, call, false,
			      sipsess_offer_handler, sipsess_answer_handler,
			      sipsess_progr_handler, sipsess_estab_handler,
			      sipsess_info_handler, NULL,
			      sipsess_close_handler, call, NULL);
	if (err) {
		DEBUG_WARNING("sipsess_connect: %s\n", strerror(err));
	}

	mem_deref(desc);

	return err;
}


/**
 * Get the current call duration in seconds
 *
 * @param call  Call object
 *
 * @return Duration in seconds
 */
uint32_t call_duration(const struct call *call)
{
	if (!call || !call->time_start)
		return 0;

	return (uint32_t)(time(NULL) - call->time_start);
}


/**
 * Get the audio object for the current call
 *
 * @param call  Call object
 *
 * @return Audio object
 */
struct audio *call_audio(const struct call *call)
{
	return call ? call->audio : NULL;
}


/**
 * Get the video object for the current call
 *
 * @param call  Call object
 *
 * @return Video object
 */
struct video *call_video(const struct call *call)
{
	return call ? call->video : NULL;
}


/**
 * Get the list of media streams for the current call
 *
 * @param call  Call object
 *
 * @return List of media streams
 */
struct list *call_streaml(const struct call *call)
{
	return call ? (struct list *)&call->streaml : NULL;
}


int call_reset_transp(struct call *call)
{
	if (!call)
		return EINVAL;

	sdp_session_set_laddr(call->sdp, net_laddr());

	return call_modify(call);
}
