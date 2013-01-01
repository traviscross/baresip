/**
 * @file baresip.h  Public Interface to Baresip
 *
 * Copyright (C) 2010 Creytiv.com
 */

#ifndef BARESIP_H__
#define BARESIP_H__

#ifdef __cplusplus
extern "C" {
#endif


/** Defines the Baresip version string */
#define BARESIP_VERSION "0.4.3"


/* forward declarations */
struct sa;
struct sdp_media;
struct sdp_session;
struct sip_msg;
struct ua;
struct vidframe;
struct vidrect;
struct vidsz;


/*
 * Call
 */

struct call;

int  call_modify(struct call *call);
int  call_hold(struct call *call, bool hold);
int  call_send_digit(struct call *call, char key);
void call_audioencoder_cycle(struct call *call);
void call_videoencoder_cycle(struct call *call);
uint32_t call_duration(const struct call *call);
const char *call_peername(const struct call *call);
struct audio *call_audio(const struct call *call);
struct video *call_video(const struct call *call);
struct list *call_streaml(const struct call *call);
bool call_has_audio(const struct call *call);
bool call_has_video(const struct call *call);
int call_transfer(struct call *call, const char *uri);


/*
 * Conf
 */

/** A range of numbers */
struct range {
	uint32_t min;  /**< Minimum number */
	uint32_t max;  /**< Maximum number */
};

static inline bool in_range(const struct range *rng, uint32_t val)
{
	return rng ? (val >= rng->min && val <= rng->max) : false;
}

/** Core configuration */
struct config {
	/** Input */
	struct {
		char device[64];       /**< Input device name */
		uint32_t port;         /**< Input port number */
	} input;

	/** SIP User-Agent */
	struct {
		uint32_t trans_bsize;  /**< SIP Transaction bucket size */
		char local[64];        /**< Local SIP Address           */
	} sip;

	/** Audio */
	struct {
		char src_mod[16];      /**< Audio source module            */
		char src_dev[128];     /**< Audio source device            */
		char play_mod[16];     /**< Audio playback module          */
		char play_dev[128];    /**< Audio playback device          */
		char alert_mod[16];    /**< Audio alert module             */
		char alert_dev[128];   /**< Audio alert device             */
		struct range srate;    /**< Audio sampling rate in [Hz]    */
		struct range channels; /**< Nr. of audio channels (1=mono) */
		struct range srate_play;/**< Sampling rates for player     */
		struct range srate_src; /**< Sampling rates for source     */
		bool src_first;        /**< Audio source opened first      */
	} audio;

	/** Video */
	struct {
		char src_mod[16];      /**< Video source module           */
		char src_dev[128];     /**< Video source device           */
		int width, height;     /**< Video resolution              */
		uint32_t bitrate;      /**< Encoder bitrate in [bit/s]    */
		uint32_t fps;          /**< Video framerate               */
	} video;

	/** Audio/Video Transport */
	struct {
		uint8_t rtp_tos;       /**< Type-of-Service for outgoing RTP */
		struct range rtp_ports;/**< RTP port range                   */
		struct range rtp_bw;   /**< RTP Bandwidth range [bit/s]      */
		bool rtcp_enable;      /**< RTCP is enabled                  */
		bool rtcp_mux;         /**< RTP/RTCP multiplexing            */
		struct range jbuf_del; /**< Delay, number of frames          */
	} avt;

	/* Network */
	struct {
		char ifname[16];       /**< Bind to interface (optional)     */
	} net;
};

extern struct config config;


/** Defines the configuration line handler */
typedef int (confline_h)(const struct pl *addr);

int  conf_accounts_get(confline_h *ch);
int  conf_system_get(const char *path);
int  conf_system_get_file(const char *path);
int  conf_system_get_buf(const uint8_t *buf, size_t sz);
int  configure(void);
int  conf_modules(void);
void conf_path_set(const char *path);
int  conf_path_get(char *path, size_t sz);
int  conf_parse(const char *filename, confline_h *ch);
struct conf *conf_cur(void);
bool conf_fileexist(const char *path);


/*
 * Contact
 */

enum presence_status {
	PRESENCE_UNKNOWN,
	PRESENCE_OPEN,
	PRESENCE_CLOSED,
	PRESENCE_BUSY
};

struct contact;

int contact_add(struct contact **contactp, const struct pl *addr);
int contacts_print(struct re_printf *pf, void *unused);
struct sip_addr *contact_addr(const struct contact *c);
const char      *contact_str(const struct contact *c);
void             contact_set_presence(struct contact *c,
				      enum presence_status status);
const char      *contact_presence_str(enum presence_status status);
struct list     *contact_list(void);


/*
 * Media Context
 */

/** Media Context */
struct media_ctx {
	const char *id;  /**< Media Context identifier */
};


/*
 * Audio Source
 */

struct ausrc;
struct ausrc_st;

/** Audio Source parameters */
struct ausrc_prm {
	int        fmt;         /**< Audio format (enum aufmt) */
	uint32_t   srate;       /**< Sampling rate in [Hz] */
	uint8_t    ch;          /**< Number of channels    */
	uint32_t   frame_size;  /**< Frame size in samples */
};

typedef void (ausrc_read_h)(const uint8_t *buf, size_t sz, void *arg);
typedef void (ausrc_error_h)(int err, const char *str, void *arg);

typedef int  (ausrc_alloc_h)(struct ausrc_st **stp, struct ausrc *ausrc,
			     struct media_ctx **ctx,
			     struct ausrc_prm *prm, const char *device,
			     ausrc_read_h *rh, ausrc_error_h *errh, void *arg);

int ausrc_register(struct ausrc **asp, const char *name,
		   ausrc_alloc_h *alloch);
const struct ausrc *ausrc_find(const char *name);
int ausrc_alloc(struct ausrc_st **stp, struct media_ctx **ctx,
		const char *name,
		struct ausrc_prm *prm, const char *device,
		ausrc_read_h *rh, ausrc_error_h *errh, void *arg);


/*
 * Audio Player
 */

struct auplay;
struct auplay_st;

/** Audio Player parameters */
struct auplay_prm {
	int        fmt;         /**< Audio format (enum aufmt) */
	uint32_t   srate;       /**< Sampling rate in [Hz] */
	uint8_t    ch;          /**< Number of channels    */
	uint32_t   frame_size;  /**< Frame size in samples */
};

typedef bool (auplay_write_h)(uint8_t *buf, size_t sz, void *arg);

typedef int  (auplay_alloc_h)(struct auplay_st **stp, struct auplay *ap,
			      struct auplay_prm *prm, const char *device,
			      auplay_write_h *wh, void *arg);

int auplay_register(struct auplay **pp, const char *name,
		    auplay_alloc_h *alloch);
const struct auplay *auplay_find(const char *name);
int auplay_alloc(struct auplay_st **stp, const char *name,
		 struct auplay_prm *prm, const char *device,
		 auplay_write_h *wh, void *arg);


/*
 * Audio Filter
 */

struct aufilt;
struct aufilt_st;

/** Audio Filter Parameters */
struct aufilt_prm {
	uint32_t srate;       /**< Sampling rate in [Hz]        */
	uint32_t srate_out;   /**< Output sampling rate in [Hz] */
	uint8_t  ch;          /**< Number of channels           */
	uint32_t frame_size;  /**< Number of samples per frame  */
};

typedef int (aufilt_alloc_h)(struct aufilt_st **stp, struct aufilt *af,
			     const struct aufilt_prm *encprm,
			     const struct aufilt_prm *decprm);
typedef int (aufilt_enc_h)(struct aufilt_st *st, struct mbuf *mb);
typedef int (aufilt_dec_h)(struct aufilt_st *st, struct mbuf *mb);
typedef int (aufilt_update_h)(struct aufilt_st *st);

int aufilt_register(struct aufilt **afp, const char *name,
		    aufilt_alloc_h *alloch, aufilt_enc_h *ench,
		    aufilt_dec_h *dech, aufilt_update_h *updh);
struct list *aufilt_list(void);
int aufilt_debug(struct re_printf *pf, void *unused);


/*
 * Menc - Media encryption
 */

struct menc;
struct menc_st;

typedef int  (menc_alloc_h)(struct menc_st **stp, struct menc *me, int proto,
			    void *rtpsock, void *rtcpsock,
			    struct sdp_media *sdpm);
typedef int  (menc_update_h)(struct menc_st *st);


int menc_register(struct menc **mencp, const char *id, menc_alloc_h *alloch,
		  menc_update_h *updateh);


/*
 * Net - Networking
 */

typedef void (net_change_h)(void *arg);

int  net_init(void);
void net_close(void);
int  net_dnssrv_add(const struct sa *sa);
void net_change(uint32_t interval, net_change_h *ch, void *arg);
bool net_check(void);
int  net_debug(struct re_printf *pf, void *unused);
const struct sa *net_laddr_af(int af);
struct dnsc *net_dnsc(void);
const char *net_domain(void);


/*
 * Play - audio file player
 */

struct play;

int  play_file(struct play **playp, const char *filename, int repeat);
int  play_tone(struct play **playp, struct mbuf *tone, uint32_t srate,
	       uint8_t ch, int repeat);
void play_close(void);


/*
 * User Agent
 */

struct ua;
struct ua_prm;

/** Events from User-Agent */
enum ua_event {
	UA_EVENT_REGISTERING = 0,
	UA_EVENT_REGISTER_OK,
	UA_EVENT_REGISTER_FAIL,
	UA_EVENT_UNREGISTERING,
	UA_EVENT_UNREGISTER_OK,
	UA_EVENT_UNREGISTER_FAIL,
	UA_EVENT_CALL_INCOMING,
	UA_EVENT_CALL_RINGING,
	UA_EVENT_CALL_PROGRESS,
	UA_EVENT_CALL_ESTABLISHED,
	UA_EVENT_CALL_CLOSED,

	UA_EVENT_MAX,
};

/** Defines the status modes */
enum statmode {
	STATMODE_CALL = 0,
	STATMODE_OFF,

	/* marker */
	STATMODE_N
};

/** Audio transmit mode */
enum audio_mode {
	AUDIO_MODE_POLL = 0,         /**< Polling mode                  */
	AUDIO_MODE_THREAD,           /**< Use dedicated thread          */
	AUDIO_MODE_THREAD_REALTIME,  /**< Use dedicated realtime-thread */
	AUDIO_MODE_TMR               /**< Use timer                     */
};

/** Video mode */
enum vidmode {
	VIDMODE_OFF = 0,    /**< Video disabled                */
	VIDMODE_ON,         /**< Video enabled                 */
};

/** Defines the User-Agent event handler */
typedef void (ua_event_h)(enum ua_event ev, const char *prm, void *arg);
typedef void (ua_message_h)(const struct pl *peer, const struct pl *ctype,
			    struct mbuf *body, void *arg);
typedef void (options_resp_h)(int err, const struct sip_msg *msg, void *arg);

/* Multiple instances */
int  ua_alloc(struct ua **uap, const char *aor,
	      ua_event_h *eh, ua_message_h *msgh, void *arg);
int  ua_add(const struct pl *addr);
int  ua_connect(struct ua *ua, const char *uri, const char *params,
		const char *mnatid, enum vidmode vmode);
void ua_hangup(struct ua *ua);
void ua_answer(struct ua *ua);
int  ua_im_send(struct ua *ua, const char *peer, const char *msg);
void ua_set_statmode(struct ua *ua, enum statmode mode);
int  ua_options_send(struct ua *ua, const char *uri,
		     options_resp_h *resph, void *arg);
int  ua_register(struct ua *ua);
int  ua_sipfd(const struct ua *ua);
int  ua_auth(struct ua_prm *prm, char **username, char **password,
	     const char *realm);
const char *ua_aor(const struct ua *ua);
const char *ua_cuser(const struct ua *ua);
const char *ua_outbound(const struct ua *ua);
struct call *ua_call(const struct ua *ua);
struct ua_prm *ua_prm(const struct ua *ua);


/* One instance */
int  ua_init(const char *software, bool udp, bool tcp, bool tls,
	     bool prefer_ipv6);
void ua_set_uuid(const char *uuid);
void ua_set_aumode(enum audio_mode aumode);
void ua_close(void);
void ua_stack_suspend(void);
int  ua_stack_resume(const char *software, bool udp, bool tcp, bool tls);
int  ua_start_all(void);
void ua_stop_all(bool forced);
int  ua_reset_transp(bool reg, bool reinvite);
struct ua *ua_cur(void);
int  ua_print_sip_status(struct re_printf *pf, void *unused);
int  ua_print_reg_status(struct re_printf *pf, void *unused);
int  ua_print_call_status(struct re_printf *pf, void *unused);
struct sip *uag_sip(void);
struct sipsess_sock *uag_sipsess_sock(void);
struct sipevent_sock *uag_sipevent_sock(void);
const char *ua_event_str(enum ua_event ev);
struct ua *ua_find(const struct pl *cuser);
struct ua *ua_find_aor(const char *aor);


/*
 * User Interface
 */

struct ui;
struct ui_st;

/** User Interface parameters */
struct ui_prm {
	char *device;   /**< Device name */
	uint16_t port;  /**< Port number */
};
typedef void (ui_input_h)(char key, struct re_printf *pf, void *arg);

typedef int  (ui_alloc_h)(struct ui_st **stp, struct ui_prm *prm,
			  ui_input_h *ih, void *arg);
typedef int  (ui_output_h)(struct ui_st *st, const char *str);

void ui_input(char key);
void ui_input_str(const char *str);
void ui_output(const char *str);
int  ui_register(struct ui **uip, const char *name,
		 ui_alloc_h *alloch, ui_output_h *outh);


/*
 * Command interface
 */

/** Command flags */
enum {
	CMD_PRM  = (1<<0),              /**< Command with parameter */
	CMD_PROG = (1<<1),              /**< Show progress          */

	CMD_IPRM = CMD_PRM | CMD_PROG,  /**< Interactive parameter  */
};

/** Command arguments */
struct cmd_arg {
	char key;         /**< Which key was pressed  */
	char *prm;        /**< Optional parameter     */
	bool complete;    /**< True if complete       */
};

/** Defines a command */
struct cmd {
	char key;         /**< Input character        */
	int flags;        /**< Optional command flags */
	const char *desc; /**< Description string     */
	re_printf_h *h;   /**< Command handler        */
};

struct cmd_ctx;

int  cmd_register(const struct cmd *cmdv, size_t cmdc);
void cmd_unregister(const struct cmd *cmdv);
int  cmd_process(struct cmd_ctx **ctxp, char key, struct re_printf *pf);
int  cmd_print(struct re_printf *pf, void *unused);


/*
 * Video Source
 */

struct vidsrc;
struct vidsrc_st;

/** Video Source parameters */
struct vidsrc_prm {
	int orient;       /**< Wanted picture orientation (enum vidorient) */
	int fps;          /**< Wanted framerate                            */
};

typedef void (vidsrc_frame_h)(const struct vidframe *frame, void *arg);
typedef void (vidsrc_error_h)(int err, void *arg);

typedef int  (vidsrc_alloc_h)(struct vidsrc_st **vsp, struct vidsrc *vs,
			      struct media_ctx **ctx, struct vidsrc_prm *prm,
			      const struct vidsz *size,
			      const char *fmt, const char *dev,
			      vidsrc_frame_h *frameh,
			      vidsrc_error_h *errorh, void *arg);

typedef void (vidsrc_update_h)(struct vidsrc_st *st, struct vidsrc_prm *prm,
			       const char *dev);

int vidsrc_register(struct vidsrc **vp, const char *name,
		    vidsrc_alloc_h *alloch, vidsrc_update_h *updateh);
const struct vidsrc *vidsrc_find(const char *name);
struct list *vidsrc_list(void);
int vidsrc_alloc(struct vidsrc_st **stp, const char *name,
		 struct media_ctx **ctx, struct vidsrc_prm *prm,
		 const struct vidsz *size, const char *fmt, const char *dev,
		 vidsrc_frame_h *frameh, vidsrc_error_h *errorh, void *arg);


/*
 * Video Display
 */

struct vidisp;
struct vidisp_st;

/** Video Display parameters */
struct vidisp_prm {
	void *view;  /**< Optional view (set by application or module) */
};

typedef void (vidisp_input_h)(char key, void *arg);
typedef void (vidisp_resize_h)(const struct vidsz *size, void *arg);

typedef int  (vidisp_alloc_h)(struct vidisp_st **vp, struct vidisp_st *parent,
			      struct vidisp *vd, struct vidisp_prm *prm,
			      const char *dev, vidisp_input_h *inputh,
			      vidisp_resize_h *resizeh, void *arg);
typedef int  (vidisp_update_h)(struct vidisp_st *st, bool fullscreen,
			       int orient, const struct vidrect *window);
typedef int  (vidisp_disp_h)(struct vidisp_st *st, const char *title,
			     const struct vidframe *frame);
typedef void (vidisp_hide_h)(struct vidisp_st *st);

int vidisp_register(struct vidisp **vp, const char *name,
		    vidisp_alloc_h *alloch, vidisp_update_h *updateh,
		    vidisp_disp_h *disph, vidisp_hide_h *hideh);
int vidisp_alloc(struct vidisp_st **stp, const char *name,
		 struct vidisp_st *parent,
		 struct vidisp_prm *prm, const char *dev,
		 vidisp_input_h *inputh, vidisp_resize_h *resizeh, void *arg);
int vidisp_display(struct vidisp_st *st, const char *title,
		   const struct vidframe *frame);
const struct vidisp *vidisp_find(const char *name);


/*
 * Audio Codec
 */

struct aucodec;
struct aucodec_st;    /* must "inherit" from struct aucodec */

/** Audio Codec Parameters */
struct aucodec_prm {
	uint32_t srate;  /**< Sampling rate in [Hz] */
	uint32_t ptime;  /**< Packet time in [ms]   */
};

typedef int (aucodec_alloc_h)(struct aucodec_st **asp, struct aucodec *ac,
			      struct aucodec_prm *encp,
			      struct aucodec_prm *decp,
			      const char *fmtp);
typedef int (aucodec_enc_h)(struct aucodec_st *s, struct mbuf *dst,
			   struct mbuf *src);
typedef int (aucodec_dec_h)(struct aucodec_st *s, struct mbuf *dst,
			   struct mbuf *src);

int aucodec_register(struct aucodec **ap, const char *pt, const char *name,
		     uint32_t srate, uint8_t ch, const char *fmtp,
		     aucodec_alloc_h *alloch, aucodec_enc_h *ench,
		     aucodec_dec_h *dech, sdp_fmtp_cmp_h *cmph);
const char *aucodec_pt(const struct aucodec *ac);
const char *aucodec_name(const struct aucodec *ac);
struct list *aucodec_list(void);
uint32_t aucodec_srate(const struct aucodec *ac);
uint8_t  aucodec_ch(const struct aucodec *ac);
int  aucodec_alloc(struct aucodec_st **sp, const char *name, uint32_t srate,
		   uint8_t channels, struct aucodec_prm *enc_prm,
		   struct aucodec_prm *dec_prm, const char *fmtp);
int aucodec_encode(struct aucodec_st *st, struct mbuf *dst, struct mbuf *src);
int aucodec_decode(struct aucodec_st *st, struct mbuf *dst, struct mbuf *src);
int aucodec_debug(struct re_printf *pf, const struct list *acl);


/*
 * Video Codec
 */

struct vidcodec;
struct vidcodec_st;    /* must "inherit" from struct vidcodec */

/** Video Codec parameters */
struct vidcodec_prm {
	int fps;            /**< Video framerate            */
	int bitrate;        /**< Encoder bitrate in [bit/s] */
};

typedef int (vidcodec_send_h)(bool marker, struct mbuf *mb, void *arg);

typedef int (vidcodec_enq_h)(bool marker, const uint32_t hdr,
			     const uint8_t *buf, size_t len, void *arg);

typedef int (vidcodec_alloc_h)(struct vidcodec_st **sp, struct vidcodec *c,
			       const char *name, struct vidcodec_prm *encp,
			       const char *fmtp, vidcodec_enq_h *enqh,
			       vidcodec_send_h *sendh, void *arg);
typedef int (vidcodec_enc_h)(struct vidcodec_st *s, bool update,
			     const struct vidframe *frame);
typedef int (vidcodec_pktize_h)(struct vidcodec_st *s, bool first, bool last,
				bool marker, uint32_t hdr, const uint8_t *buf,
				size_t len, size_t maxlen);
typedef int (vidcodec_dec_h)(struct vidcodec_st *s, struct vidframe *frame,
			     bool marker, struct mbuf *src);

int vidcodec_register(struct vidcodec **vp, const char *pt, const char *name,
		      const char *fmtp, vidcodec_alloc_h *alloch,
		      vidcodec_enc_h *ench, vidcodec_pktize_h *pktizeh,
		      vidcodec_dec_h *dech, sdp_fmtp_cmp_h *cmph);
const struct vidcodec *vidcodec_find(const char *name);
struct vidcodec *vidcodec_get(const struct vidcodec_st *st);
const char *vidcodec_pt(const struct vidcodec *vc);
const char *vidcodec_name(const struct vidcodec *vc);
struct list *vidcodec_list(void);
void vidcodec_set_fmtp(struct vidcodec *vc, const char *fmtp);
int  vidcodec_alloc(struct vidcodec_st **sp, const char *name,
		    struct vidcodec_prm *encp, const char *fmtp,
		    vidcodec_enq_h *enqh, vidcodec_send_h *sendh, void *arg);
int  vidcodec_encode(struct vidcodec_st *st, bool update,
		     const struct vidframe *frame);
int  vidcodec_decode(struct vidcodec_st *st, struct vidframe *frame,
		     bool marker, struct mbuf *src);
int  vidcodec_debug(struct re_printf *pf, const struct list *vcl);


/*
 * Video Filter
 */

struct vidfilt;

/* Base class */
struct vidfilt_st {
	struct vidfilt *vf;
	struct le le;
};

typedef int (vidfilt_update_h)(struct vidfilt_st **stp, struct vidfilt *vf);
typedef int (vidfilt_encode_h)(struct vidfilt_st *st, struct vidframe *frame);
typedef int (vidfilt_decode_h)(struct vidfilt_st *st, struct vidframe *frame);

struct vidfilt {
	struct le le;
	const char *name;
	vidfilt_update_h *updh;
	vidfilt_encode_h *ench;
	vidfilt_decode_h *dech;
};

void vidfilt_register(struct vidfilt *vf);
void vidfilt_unregister(struct vidfilt *vf);
struct list *vidfilt_list(void);


/*
 * Audio stream
 */

struct audio;

void audio_mute(struct audio *a, bool muted);


/*
 * Video stream
 */

struct video;

void  video_mute(struct video *v, bool muted);
void *video_view(const struct video *v);
int   video_set_fullscreen(struct video *v, bool fs);
int   video_set_orient(struct video *v, int orient);
void  video_vidsrc_set_device(struct video *v, const char *dev);
int   video_set_source(struct video *v, const char *name, const char *dev);


/*
 * Media NAT
 */

struct mnat;
struct mnat_sess;
struct mnat_media;

typedef void (mnat_estab_h)(int err, uint16_t scode, const char *reason,
			    void *arg);

typedef int (mnat_sess_h)(struct mnat_sess **sessp, struct dnsc *dnsc,
			  const char *srv, uint16_t port,
			  const char *user, const char *pass,
			  struct sdp_session *sdp, bool offerer,
			  mnat_estab_h *estabh, void *arg);

typedef int (mnat_media_h)(struct mnat_media **mp, struct mnat_sess *sess,
			   int proto, void *sock1, void *sock2,
			   struct sdp_media *sdpm);

typedef int (mnat_update_h)(struct mnat_sess *sess);

int mnat_register(struct mnat **mnatp, const char *id, const char *ftag,
		  mnat_sess_h *sessh, mnat_media_h *mediah,
		  mnat_update_h *updateh);


/*
 * Real-time
 */
int realtime_enable(bool enable, int fps);


/*
 * Modules
 */

#ifdef STATIC
#define DECL_EXPORTS(name) exports_ ##name
#else
#define DECL_EXPORTS(name) exports
#endif


#ifdef __cplusplus
}
#endif


#endif /* BARESIP_H__ */
