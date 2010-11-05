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


struct ausrc {
	struct le        le;
	const char      *name;
	ausrc_alloc_h   *alloch;
};

struct auplay {
	struct le        le;
	const char      *name;
	auplay_alloc_h  *alloch;
};

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

struct vidsrc {
	struct le         le;
	const char       *name;
	vidsrc_alloc_h   *alloch;
};

struct vidisp {
	struct le        le;
	const char      *name;
	vidisp_alloc_h  *alloch;
	vidisp_disp_h   *disph;
	vidisp_hide_h   *hideh;
};

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
struct aucodec *aucodec_get(struct aucodec_st *st);
const struct aucodec *aucodec_find(const char *name, uint32_t srate, int ch);
int  aucodec_clone(struct list *l, const struct aucodec *src);
bool aucodec_cmp(const struct aucodec *l, const struct aucodec *r);
int  aucodec_debug(struct re_printf *pf, const struct list *vcl);


/* Video codec */
int  vidcodec_alloc(struct vidcodec_st **sp, const char *name,
		   struct vidcodec_prm *encp, struct vidcodec_prm *decp,
		   const struct pl *sdp_fmtp,
		   vidcodec_send_h *sendh, void *arg);
struct vidcodec *vidcodec_get(struct vidcodec_st *st);
int  vidcodec_clone(struct list *l, const struct vidcodec *src);
bool vidcodec_cmp(const struct vidcodec *l, const struct vidcodec *r);
int  vidcodec_debug(struct re_printf *pf, const struct list *vcl);


/* Video Display */
const struct vidisp *vidisp_find(const char *name);
int vidisp_alloc(struct vidisp_st **stp, struct vidisp_prm *prm,
		 const char *name, const char *dev, vidisp_input_h *input,
		 vidisp_resize_h *resizeh, void *arg);
int vidisp_display(struct vidisp_st *st, const char *title,
		   const struct vidframe *frame);


/* Media NAT traversal */
struct mnat {
	struct le le;
	const char *id;
	const char *ftag;
	mnat_sess_h *sessh;
	mnat_media_h *mediah;
	mnat_update_h *updateh;
};


/* Media Encryption */
struct menc {
	struct le le;
	const char *id;
	menc_alloc_h *alloch;
	menc_update_h *updateh;
};

const struct menc *menc_find(const char *id);
int  menc_alloc(struct menc_st **mep, const char *id, int proto,
		void *rtpsock, void *rtcpsock, struct sdp_media *sdpm);
struct menc *menc_get(struct menc_st *st);

const char *menc2transp(const char *type);


/* audio stream */

struct audio;

typedef void (audio_event_h)(int key, bool end, void *arg);
typedef void (audio_err_h)(int err, const char *str, void *arg);

int  audio_alloc(struct audio **ap, uint32_t ptime, audio_event_h *eventh,
		 audio_err_h *errh, void *arg);
int  audio_start(struct audio *a, const char *dev);
void audio_stop(struct audio *a);
int  audio_encoder_set(struct audio *a, struct aucodec *ac,
		       uint8_t pt_tx, const char *params);
int  audio_decoder_set(struct audio *a, struct aucodec *ac, uint8_t pt_rx);
void audio_enable_vumeter(struct audio *a, bool en);
struct stream *audio_strm(const struct audio *a);
int  audio_print_vu(struct re_printf *pf, const struct audio *a);
void audio_enable_telev(struct audio *a, uint8_t pt_tx, uint8_t pt_rx);
int  audio_send_digit(struct audio *a, char key);
int  audio_sdp_attr_encode(const struct audio *a, struct sdp_media *m);
void audio_sdp_attr_decode(struct audio *a, struct sdp_media *m);
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

typedef void (call_event_h)(enum call_event ev, const char *prm, void *arg);

int  call_alloc(struct call **callp, struct ua *ua, uint32_t ptime,
		struct mnat *mnat,
		const char *stun_user, const char *stun_pass,
		const char *stun_host, uint16_t stun_port,
		const char *menc, call_event_h *eh, void *arg,
		const char *local_name, const char *local_uri,
		const char *cuser);
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
void call_summary(const struct call *call, struct mbuf *mb);
void call_enable_vumeter(struct call *call, bool en);
struct ua *call_get_ua(const struct call *call);
int call_reset_transp(struct call *call);


/* media control */
int mctrl_handle_media_control(struct pl *body, bool *pfu);


/* network */
int  net_reset(void);


/* User-Agent */
struct ua;

typedef void (message_resp_h)(int err, const struct sip_msg *msg, void *arg);

char *ua_outbound(const struct ua *ua);
struct list *ua_aucodecl(struct ua *ua);
struct list *ua_vidcodecl(struct ua *ua);
int sip_options_send(struct ua *ua, const char *uri,
		     options_resp_h *resph, void *arg);
int sip_message_send(struct ua *ua, const char *uri, struct mbuf *msg,
		     message_resp_h *resph, void *arg);


/* Audio Filters */
struct aufilt_chain;

int aufilt_chain_alloc(struct aufilt_chain **fcp,
		       const struct aufilt_prm *encprm,
		       const struct aufilt_prm *decprm);
int aufilt_chain_encode(struct aufilt_chain *fc, struct mbuf *mb);
int aufilt_chain_decode(struct aufilt_chain *fc, struct mbuf *mb);
int aufilt_chain_debug(struct re_printf *pf, const struct aufilt_chain *fc);
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

int  stream_alloc(struct stream **sp, struct call *call, const char *name,
		  int label, stream_recv_h *rh, void *arg);
void stream_set_sdpmedia(struct stream *s, struct sdp_media *m);
struct sdp_media *stream_sdpmedia(const struct stream *s);
int  stream_menc_set(struct stream *s, const char *type);
int  stream_mnat_init(struct stream *s, const struct mnat *mnat,
		      struct mnat_sess *mnat_sess);
int  stream_start(struct stream *s);
void stream_stop(struct stream *s);
void stream_start_keepalive(struct stream *s);
int  stream_send(struct stream *s, bool marker, uint8_t pt, uint32_t ts,
		 struct mbuf *mb);
const struct sa *stream_local(const struct stream *s);
void stream_remote_set(struct stream *s, const char *cname);
int  stream_sdp_attr_encode(const struct stream *s, struct sdp_media *m);
void stream_sdp_attr_decode(struct stream *s);
void *stream_arg(const struct stream *s);
int  stream_jbuf_stat(struct re_printf *pf, const struct stream *s);
void stream_set_active(struct stream *s, bool active);
bool stream_is_active(const struct stream *s);
void stream_hold(struct stream *s, bool hold);
void stream_set_srate(struct stream *s, uint32_t srate_tx, uint32_t srate_rx);
void stream_send_fir(struct stream *s);
void stream_reset(struct stream *s);
int  stream_debug(struct re_printf *pf, const struct stream *s);
int  stream_print(struct re_printf *pf, const struct stream *s);


/* UUID loader */
int uuid_load(char *uuid, uint32_t sz);


/* video stream */

struct video;

int  video_alloc(struct video **vp, struct call *call);
int  video_start(struct video *v, const char *dev, const char *peer);
void video_stop(struct video *v);
int  video_encoder_set(struct video *v, struct vidcodec *vc,
		       uint8_t pt_tx, const char *params);
int  video_decoder_set(struct video *v, struct vidcodec *vc, uint8_t pt_rx);
struct stream *video_strm(const struct video *v);
void video_update_picture(struct video *v);
int  video_sdp_attr_encode(const struct video *v, struct sdp_media *m);
void video_sdp_attr_decode(struct video *v, struct sdp_media *m);
int  video_debug(struct re_printf *pf, const struct video *v);
int  video_print(struct re_printf *pf, const struct video *v);
