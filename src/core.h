/**
 * @file core.h  Internal API
 *
 * Copyright (C) 2010 Creytiv.com
 */


/**
 * RFC 3551:
 *
 *    0 -  95  Static payload types
 *   96 - 127  Dynamic payload types
 */
enum {
	PT_CN       = 13,
	PT_STAT_MIN = 0,
	PT_STAT_MAX = 95,
	PT_DYN_MIN  = 96,
	PT_DYN_MAX  = 127
};


/*
 * Audio Player
 */

struct auplay {
	struct le        le;
	const char      *name;
	auplay_alloc_h  *alloch;
};


/*
 * Audio Source
 */

struct ausrc {
	struct le        le;
	const char      *name;
	ausrc_alloc_h   *alloch;
};


/*
 * Audio Stream
 */

struct audio;

typedef void (audio_event_h)(int key, bool end, void *arg);
typedef void (audio_err_h)(int err, const char *str, void *arg);

int  audio_alloc(struct audio **ap, struct call *call,
		 struct sdp_session *sdp_sess, int label,
		 const struct mnat *mnat, struct mnat_sess *mnat_sess,
		 const struct menc *menc, struct menc_sess *menc_sess,
		 uint32_t ptime, enum audio_mode mode,
		 const struct list *aucodecl,
		 audio_event_h *eventh, audio_err_h *errh, void *arg);
int  audio_start(struct audio *a);
void audio_stop(struct audio *a);
int  audio_encoder_set(struct audio *a, const struct aucodec *ac,
		       int pt_tx, const char *params);
int  audio_decoder_set(struct audio *a, const struct aucodec *ac,
		       int pt_rx, const char *params);
struct stream *audio_strm(const struct audio *a);
int  audio_send_digit(struct audio *a, char key);
void audio_sdp_attr_decode(struct audio *a);


/*
 * BFCP
 */

struct bfcp;
int bfcp_alloc(struct bfcp **bfcpp, struct sdp_session *sdp_sess,
	       const char *proto, bool offerer,
	       const struct mnat *mnat, struct mnat_sess *mnat_sess);
int bfcp_start(struct bfcp *bfcp);


/*
 * Call Control
 */

struct call;

enum call_event {
	CALL_EVENT_INCOMING,
	CALL_EVENT_RINGING,
	CALL_EVENT_PROGRESS,
	CALL_EVENT_ESTABLISHED,
	CALL_EVENT_CLOSED,
	CALL_EVENT_TRANSFER,
};

/** Call parameters */
struct call_prm {
	uint32_t ptime;
	enum audio_mode aumode;
	enum vidmode vidmode;
	int af;
};

typedef void (call_event_h)(struct call *call, enum call_event ev,
			    const char *str, void *arg);

int call_alloc(struct call **callp, struct list *lst,
	       struct ua *ua, const struct call_prm *prm,
	       const struct mnat *mnat,
	       const char *stun_user, const char *stun_pass,
	       const char *stun_host, uint16_t stun_port,
	       const struct menc *menc, const char *local_name,
	       const char *local_uri,
	       const struct sip_msg *msg, struct call *xcall,
	       call_event_h *eh, void *arg);
int  call_connect(struct call *call, const struct pl *paddr);
int  call_accept(struct call *call, struct sipsess_sock *sess_sock,
		 const struct sip_msg *msg);
int  call_hangup(struct call *call);
int  call_progress(struct call *call);
int  call_answer(struct call *call, uint16_t scode);
int  call_ringtone(struct call *call, const char *ringtone, int repeat);
int  call_sdp_get(const struct call *call, struct mbuf **descp, bool offer);
const char *call_peeruri(const struct call *call);
int  call_debug(struct re_printf *pf, const struct call *call);
int  call_jbuf_stat(struct re_printf *pf, const struct call *call);
int  call_info(struct re_printf *pf, const struct call *call);
struct ua *call_get_ua(const struct call *call);
int call_reset_transp(struct call *call);
int call_notify_sipfrag(struct call *call, uint16_t scode,
			const char *reason, ...);
int call_af(const struct call *call);


/*
 * Media control
 */

int mctrl_handle_media_control(struct pl *body, bool *pfu);


/*
 * Media NAT traversal
 */

struct mnat {
	struct le le;
	const char *id;
	const char *ftag;
	mnat_sess_h *sessh;
	mnat_media_h *mediah;
	mnat_update_h *updateh;
};

const struct mnat *mnat_find(const char *id);


/*
 * Module
 */

int module_init(const struct conf *conf);
void module_app_unload(void);


/*
 * Network
 */

int net_reset(void);


/*
 * Register client
 */

struct reg;

int  reg_add(struct list *lst, struct ua *ua, int regid);
int  reg_register(struct reg *reg, const char *reg_uri,
		    const char *params, uint32_t regint, const char *outbound);
void reg_unregister(struct reg *reg);
bool reg_isok(const struct reg *reg);
int  reg_sipfd(const struct reg *reg);
int  reg_debug(struct re_printf *pf, const struct reg *reg);
int  reg_status(struct re_printf *pf, const struct reg *reg);


/*
 * RTP keepalive
 */

struct rtpkeep;

int  rtpkeep_alloc(struct rtpkeep **rkp, const char *method, int proto,
		   struct rtp_sock *rtp, struct sdp_media *sdp);
void rtpkeep_refresh(struct rtpkeep *rk, uint32_t ts);


/*
 * SIP Request
 */

int sip_req_send(struct ua *ua, const char *method, const char *uri,
		 sip_resp_h *resph, void *arg, const char *fmt, ...);


/*
 * Stream
 */

struct stream;
struct rtp_header;

enum {STREAM_PRESZ = 4+12}; /* same as RTP_HEADER_SIZE */

typedef void (stream_rtp_h)(const struct rtp_header *hdr, struct mbuf *mb,
			    void *arg);
typedef void (stream_rtcp_h)(struct rtcp_msg *msg, void *arg);

int  stream_alloc(struct stream **sp, struct call *call,
		  struct sdp_session *sdp_sess,
		  const char *name, int label,
		  const struct mnat *mnat, struct mnat_sess *mnat_sess,
		  const struct menc *menc, struct menc_sess *menc_sess,
		  stream_rtp_h *rtph, stream_rtcp_h *rtcph, void *arg);
struct sdp_media *stream_sdpmedia(const struct stream *s);
int  stream_start(struct stream *s);
void stream_start_keepalive(struct stream *s);
int  stream_send(struct stream *s, bool marker, int pt, uint32_t ts,
		 struct mbuf *mb);
void stream_update(struct stream *s, const char *cname);
void stream_update_encoder(struct stream *s, int pt_enc);
int  stream_jbuf_stat(struct re_printf *pf, const struct stream *s);
void stream_hold(struct stream *s, bool hold);
void stream_set_srate(struct stream *s, uint32_t srate_tx, uint32_t srate_rx);
void stream_send_fir(struct stream *s, bool pli);
void stream_reset(struct stream *s);
void stream_set_bw(struct stream *s, uint32_t bps);
bool stream_has_media(const struct stream *s);
int  stream_debug(struct re_printf *pf, const struct stream *s);
int  stream_print(struct re_printf *pf, const struct stream *s);


/*
 * User-Agent
 */

struct ua;

const char  *ua_param(const struct ua *ua, const char *key);
struct list *ua_aucodecl(const struct ua *ua);
struct list *ua_vidcodecl(const struct ua *ua);
void         ua_event(struct ua *ua, enum ua_event ev, const char *fmt, ...);
void         ua_printf(const struct ua *ua, const char *fmt, ...);

void         uag_check_registrations(void);
struct tls  *uag_tls(void);
struct list *uag_list(void);
const char  *uag_allowed_methods(void);


/*
 * Video Display
 */

struct vidisp {
	struct le        le;
	const char      *name;
	vidisp_alloc_h  *alloch;
	vidisp_update_h *updateh;
	vidisp_disp_h   *disph;
	vidisp_hide_h   *hideh;
};

struct vidisp *vidisp_get(struct vidisp_st *st);


/*
 * Video Source
 */

struct vidsrc {
	struct le         le;
	const char       *name;
	vidsrc_alloc_h   *alloch;
	vidsrc_update_h  *updateh;
};

struct vidsrc *vidsrc_get(struct vidsrc_st *st);


/*
 * Video Stream
 */

struct video;

int  video_alloc(struct video **vp, struct call *call,
		 struct sdp_session *sdp_sess, int label,
		 const struct mnat *mnat, struct mnat_sess *mnat_sess,
		 const struct menc *menc, struct menc_sess *menc_sess,
		 const char *content, const struct list *vidcodecl);
int  video_start(struct video *v, const char *src, const char *dev,
		 const char *peer);
void video_stop(struct video *v);
int  video_encoder_set(struct video *v, struct vidcodec *vc,
		       int pt_tx, const char *params);
int  video_decoder_set(struct video *v, struct vidcodec *vc, int pt_rx,
		       const char *fmtp);
struct stream *video_strm(const struct video *v);
void video_update_picture(struct video *v);
void video_sdp_attr_decode(struct video *v);
int  video_print(struct re_printf *pf, const struct video *v);
