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
	PT_DYN_MAX  = 127,
	PT_NONE     = 255
};


/** Audio Source */
struct ausrc {
	struct le        le;
	const char      *name;
	ausrc_alloc_h   *alloch;
};

/** Audio Player */
struct auplay {
	struct le        le;
	const char      *name;
	auplay_alloc_h  *alloch;
};

/** Audio Codec */
struct aucodec {
	struct le        le;
	const char      *pt;
	const char      *name;
	uint32_t         srate;
	uint8_t          ch;
	const char      *fmtp;
	aucodec_alloc_h *alloch;
	aucodec_enc_h   *ench;
	aucodec_dec_h   *dech;
};

/** Video Source */
struct vidsrc {
	struct le         le;
	const char       *name;
	vidsrc_alloc_h   *alloch;
	vidsrc_update_h  *updateh;
};

/** Video Display */
struct vidisp {
	struct le        le;
	const char      *name;
	vidisp_alloc_h  *alloch;
	vidisp_update_h *updateh;
	vidisp_disp_h   *disph;
	vidisp_hide_h   *hideh;
};

/** Video Codec */
struct vidcodec {
	struct le         le;
	const char       *pt;
	const char       *name;
	const char       *fmtp;
	vidcodec_alloc_h *alloch;
	vidcodec_enc_h   *ench;
	vidcodec_dec_h   *dech;
};


/* Audio codec */
struct aucodec *aucodec_get(const struct aucodec_st *st);
const struct aucodec *aucodec_find(const char *name, uint32_t srate, int ch);
int  aucodec_clone(struct list *l, const struct aucodec *src);
bool aucodec_cmp(const struct aucodec *l, const struct aucodec *r);
int  aucodec_debug(struct re_printf *pf, const struct list *vcl);


/* Video codec */
int  vidcodec_alloc(struct vidcodec_st **sp, const char *name,
		   struct vidcodec_prm *encp, struct vidcodec_prm *decp,
		   const struct pl *sdp_fmtp,
		   vidcodec_send_h *sendh, void *arg);
struct vidcodec *vidcodec_get(const struct vidcodec_st *st);
int  vidcodec_clone(struct list *l, const struct vidcodec *src);
bool vidcodec_cmp(const struct vidcodec *l, const struct vidcodec *r);
int  vidcodec_debug(struct re_printf *pf, const struct list *vcl);


/* Video Display */
const struct vidisp *vidisp_find(const char *name);
int vidisp_display(struct vidisp_st *st, const char *title,
		   const struct vidframe *frame);
struct vidisp *vidisp_get(struct vidisp_st *st);


/* Video Source */
struct vidsrc *vidsrc_get(struct vidsrc_st *st);


/** Media NAT traversal */
struct mnat {
	struct le le;
	const char *id;
	const char *ftag;
	mnat_sess_h *sessh;
	mnat_media_h *mediah;
	mnat_update_h *updateh;
};

const struct mnat *mnat_find(const char *id);


/** Media Encryption */
struct menc {
	struct le le;
	const char *id;
	menc_alloc_h *alloch;
	menc_update_h *updateh;
};

const struct menc *menc_find(const char *id);
struct menc *menc_get(const struct menc_st *st);
const char *menc2transp(const struct menc *menc);


/* audio stream */

struct audio;

typedef void (audio_event_h)(int key, bool end, void *arg);
typedef void (audio_err_h)(int err, const char *str, void *arg);

int  audio_alloc(struct audio **ap, struct call *call,
		 struct sdp_session *sdp_sess, int label,
		 const struct mnat *mnat, struct mnat_sess *mnat_sess,
		 const struct menc *menc, uint32_t ptime, enum audio_mode mode,
		 audio_event_h *eventh, audio_err_h *errh, void *arg);
int  audio_start(struct audio *a);
void audio_stop(struct audio *a);
int  audio_encoder_set(struct audio *a, struct aucodec *ac,
		       uint8_t pt_tx, const char *params);
int  audio_decoder_set(struct audio *a, struct aucodec *ac,
		       uint8_t pt_rx, const char *params);
void audio_enable_vumeter(struct audio *a, bool en);
struct stream *audio_strm(const struct audio *a);
int  audio_print_vu(struct re_printf *pf, const struct audio *a);
void audio_enable_telev(struct audio *a, uint8_t pt_tx, uint8_t pt_rx);
int  audio_send_digit(struct audio *a, char key);
void audio_sdp_attr_decode(struct audio *a);
int  audio_debug(struct re_printf *pf, const struct audio *a);


/* aufile - audiofile reader */
int aufile_load(struct mbuf *mbf, const char *filename,
		uint32_t *srate, uint8_t *channels);


/* call control */

struct call;

enum call_event {
	CALL_EVENT_INCOMING,
	CALL_EVENT_RINGING,
	CALL_EVENT_PROGRESS,
	CALL_EVENT_ESTABLISHED,
	CALL_EVENT_CLOSED
};

/** Call parameters */
struct call_prm {
	uint32_t ptime;
	enum audio_mode aumode;
	enum vidmode vidmode;
};

typedef void (call_event_h)(enum call_event ev, const char *prm, void *arg);

int call_alloc(struct call **callp, struct ua *ua, const struct call_prm *prm,
	       const struct mnat *mnat,
	       const char *stun_user, const char *stun_pass,
	       const char *stun_host, uint16_t stun_port,
	       const struct menc *menc, const char *local_name,
	       const char *local_uri, const char *cuser,
	       const struct sip_msg *msg, call_event_h *eh, void *arg);
int  call_connect(struct call *call, const struct pl *paddr);
int  call_accept(struct call *call, struct sipsess_sock *sess_sock,
		 const struct sip_msg *msg, const char *cuser);
int  call_hangup(struct call *call);
int  call_progress(struct call *call);
int  call_answer(struct call *call, uint16_t scode);
int  call_ringtone(struct call *call, const char *ringtone);
int  call_sdp_get(const struct call *call, struct mbuf **descp, bool offer);
const char *call_peeruri(const struct call *call);
int  call_debug(struct re_printf *pf, const struct call *call);
int  call_status(struct re_printf *pf, const struct call *call);
int  call_jbuf_stat(struct re_printf *pf, const struct call *call);
void call_enable_vumeter(struct call *call, bool en);
struct ua *call_get_ua(const struct call *call);
int call_reset_transp(struct call *call);


/* media control */
int mctrl_handle_media_control(struct pl *body, bool *pfu);


/* network */
int  net_reset(void);


/* User-Agent */
struct ua;

char        *ua_outbound(const struct ua *ua);
const char  *ua_param(const struct ua *ua, const char *key);
struct list *ua_aucodecl(struct ua *ua);
struct list *ua_vidcodecl(struct ua *ua);
int          ua_auth(struct ua *ua, char **username, char **password,
		     const char *realm);
int          ua_debug(struct re_printf *pf, const struct ua *ua);

void        ua_next(void);
struct tls *uag_tls(void);


/* SIP Request */
int sip_req_send(struct ua *ua, const char *method, const char *uri,
		 sip_resp_h *resph, void *arg, const char *fmt, ...);


/* Audio Filters */
struct aufilt_chain;

int aufilt_chain_alloc(struct aufilt_chain **fcp,
		       const struct aufilt_prm *encprm,
		       const struct aufilt_prm *decprm);
int aufilt_chain_encode(struct aufilt_chain *fc, struct mbuf *mb);
int aufilt_chain_decode(struct aufilt_chain *fc, struct mbuf *mb);
int aufilt_chain_update(struct aufilt_chain *fc);
int aufilt_debug(struct re_printf *pf, void *unused);


/* Contacts handling */
struct contact;

int  contact_find(const struct pl *str, struct mbuf *uri);
void contact_send_options(struct ua *ua, const char *uri, bool verbose);
void contact_send_options_to_all(struct ua *ua, bool verbose);
int  contact_debug(struct re_printf *pf, void *unused);


/* generic media stream */

struct stream;
struct rtp_header;

enum {STREAM_PRESZ = 4+12}; /* same as RTP_HEADER_SIZE */

typedef void (stream_recv_h)(const struct rtp_header *hdr, struct mbuf *mb,
			     void *arg);

int  stream_alloc(struct stream **sp, struct call *call,
		  struct sdp_session *sdp_sess,
		  const char *name, int label,
		  const struct mnat *mnat, struct mnat_sess *mnat_sess,
		  const struct menc *menc,
		  stream_recv_h *rh, void *arg);
struct sdp_media *stream_sdpmedia(const struct stream *s);
int  stream_start(struct stream *s);
void stream_start_keepalive(struct stream *s);
int  stream_send(struct stream *s, bool marker, uint8_t pt, uint32_t ts,
		 struct mbuf *mb);
void stream_remote_set(struct stream *s, const char *cname);
void stream_sdp_attr_decode(struct stream *s);
int  stream_jbuf_stat(struct re_printf *pf, const struct stream *s);
void stream_hold(struct stream *s, bool hold);
void stream_set_srate(struct stream *s, uint32_t srate_tx, uint32_t srate_rx);
void stream_send_fir(struct stream *s);
void stream_reset(struct stream *s);
void stream_set_bw(struct stream *s, uint32_t bps);
bool stream_has_media(const struct stream *s);
int  stream_debug(struct re_printf *pf, const struct stream *s);
int  stream_print(struct re_printf *pf, const struct stream *s);


/* UUID loader */
int uuid_load(char *uuid, uint32_t sz);


/* video stream */

struct video;

int  video_alloc(struct video **vp, struct call *call,
		 struct sdp_session *sdp_sess, int label,
		 const struct mnat *mnat, struct mnat_sess *mnat_sess,
		 const struct menc *menc, const char *content);
int  video_start(struct video *v, const char *src, const char *dev,
		 const char *peer);
void video_stop(struct video *v);
int  video_encoder_set(struct video *v, struct vidcodec *vc,
		       uint8_t pt_tx, const char *params);
int  video_decoder_set(struct video *v, struct vidcodec *vc, uint8_t pt_rx);
struct stream *video_strm(const struct video *v);
void video_update_picture(struct video *v);
void video_sdp_attr_decode(struct video *v);
int  video_debug(struct re_printf *pf, const struct video *v);
int  video_print(struct re_printf *pf, const struct video *v);
int  video_set_shuttered(struct video *v, bool shuttered);


/* RTP keepalive */
struct rtpkeep;
int  rtpkeep_alloc(struct rtpkeep **rkp, const char *method, int proto,
		   struct rtp_sock *rtp, struct sdp_media *sdp);
void rtpkeep_refresh(struct rtpkeep *rk, uint32_t ts);


/* os */
int mkpath(const char *path);
int get_login_name(const char **login);
int get_homedir(char *path, uint32_t sz);
