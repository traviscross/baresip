/**
 * @file baresip.h  Public Interface to Baresip
 *
 * Copyright (C) 2010 Creytiv.com
 */

#ifdef __cplusplus
extern "C" {
#endif


/* forward declarations */
struct sa;
struct sdp_media;
struct sdp_session;


/** Defines the exit handler */
typedef void (exit_h)(int ret);


/* Generic Video types */

struct vidsz {
	int w, h;
};

struct vidframe {
	uint8_t *data[4];
	uint16_t linesize[4];
	struct vidsz size;
	bool     valid;
};

struct vidpt {
	int x;
	int y;
};

struct vidrect {
	struct vidpt origin;
	struct vidsz size;
	int r;
};


static inline bool vidsz_cmp(const struct vidsz *a, const struct vidsz *b)
{
	if (!a || !b)
		return false;

	if (a == b)
		return true;

	return a->w == b->w && a->h == b->h;
}


/* vutil.c */
void vidframe_init(struct vidframe *vf, const struct vidsz *sz,
		   uint8_t *data[4], int linesize[4]);
void vidframe_yuv420p_init(struct vidframe *vf, uint8_t *ptr,
			   const struct vidsz *sz);
void vidframe_rgb32_init(struct vidframe *vf, const struct vidsz *sz,
			 uint8_t *buf);
struct vidframe *vidframe_alloc(const struct vidsz *sz);
int vidframe_alloc_filled(struct vidframe **vfp, const struct vidsz *sz,
                          uint32_t r, uint32_t g, uint32_t b);
int  vidframe_print(struct re_printf *pf, const struct vidframe *vf);
void vidrect_init(struct vidrect *rect, int x, int y, int w, int h, int r);


/* calc - Interface to calculation routines */

/** Calculate the average of two numeric values */
#define avg(a, b) ((a)+(b))/2

/** Safe division macro */
#define DIV(num, denom) (denom) ? ((num)/(denom)) : 0


uint32_t calc_nsamp(uint32_t srate, int channels, uint16_t ptime);
uint32_t calc_ptime(uint32_t srate, int channels, uint32_t nsamp);
int16_t  calc_avg_s16(struct mbuf *mb);
uint32_t yuv420p_size(const struct vidsz *sz);


/* call */
struct call;

int  call_modify(struct call *call);
int  call_hold(struct call *call, bool hold);
int  call_send_digit(struct call *call, char key);
void call_audioencoder_cycle(struct call *call);
void call_videoencoder_cycle(struct call *call);
uint32_t call_duration(const struct call *call);
const char *call_peername(const struct call *call);
const char *call_peerrpid(const struct call *call);
struct audio *call_audio(struct call *call);
struct video *call_video(struct call *call);
struct list *call_streaml(const struct call *call);
bool call_has_audio(const struct call *call);
bool call_has_video(const struct call *call);
const struct mnat *call_mnat(const struct call *call);


/* conf */

struct range {
	uint32_t min;
	uint32_t max;
};

static inline bool in_range(const struct range *rng, uint32_t val)
{
	return rng ? (val >= rng->min && val <= rng->max) : false;
}


struct conf {
	/** Input */
	struct {
		char device[64];
		uint32_t port;
	} input;

	/** SIP User-Agent */
	struct {
		uint32_t trans_bsize;  /**< SIP Transaction bucket size */
		char local[64];        /**< Local SIP Address           */
	} sip;

	/** Audio */
	struct {
		char device[64];       /**< Audio device name              */
		struct range srate;    /**< Audio sampling rate in [Hz]    */
		struct range channels; /**< Nr. of audio channels (1=mono) */
		uint32_t aec_len;      /**< AEC Tail length in [ms]        */
		struct range srate_play;
		struct range srate_src;
	} audio;

	/** Video */
	struct {
		char device[64];
		struct vidsz size;
		uint32_t bitrate;      /**< Encoder bitrate in [bit/s] */
		uint32_t fps;
	} video;

	/** Jitter Buffer */
	struct {
		struct range delay;    /**< Delay, number of frames      */
	} jbuf;

	/** Audio/Video Transport */
	struct {
		uint8_t rtp_tos;       /**< Type-of-Service for outgoing RTP */
		struct range rtp_ports;/**< RTP port range                   */
		struct range rtp_bandwidth;/**< RTP Bandwidth range [bit/s]  */
		bool rtcp_enable;      /**< RTCP is enabled                  */
	} avt;

	/** NAT Behavior Discovery */
	struct {
		uint32_t interval;     /**< Interval in [s], 0 to disable */
	} natbd;
};

struct mod;
extern struct conf config;

/** Defines the configuration line handler */
typedef int (confline_h)(const struct pl *addr);

int  conf_accounts_get(confline_h *ch, uint32_t *n);
int  conf_contacts_get(confline_h *ch, uint32_t *n);
int  conf_system_get(const char *path);
int  conf_system_get_file(const char *path);
int  conf_system_get_buf(const uint8_t *buf, size_t sz);
int  configure(void);
void conf_path_set(const char *path);
int  conf_path_get(char *path, uint32_t sz);
const char *conf_modpath(void);
struct conf *conf_cur(void);
void conf_set_domain(const char *domain);
int  conf_load_module(struct mod **mp, const char *name);


/* contact */
void contact_init(void);
void contact_close(void);
int  contact_add(const struct pl *addr);


/* Audio Source */

enum aufmt {AUFMT_S16LE, AUFMT_PCMA, AUFMT_PCMU};
struct ausrc;
struct ausrc_st;
struct ausrc_prm {
	enum aufmt fmt;
	uint32_t   srate;
	uint8_t    ch;
	uint32_t   frame_size;
};

typedef void (ausrc_read_h)(const uint8_t *buf, size_t sz, void *arg);
typedef void (ausrc_error_h)(int err, const char *str, void *arg);

typedef int  (ausrc_alloc_h)(struct ausrc_st **stp, struct ausrc *ausrc,
			     struct ausrc_prm *prm, const char *device,
			     ausrc_read_h *rh, ausrc_error_h *errh, void *arg);

int ausrc_register(struct ausrc **asp, const char *name,
		   ausrc_alloc_h *alloch);
const struct ausrc *ausrc_find(const char *name);
int ausrc_alloc(struct ausrc_st **stp, const char *name,
		struct ausrc_prm *prm, const char *device,
		ausrc_read_h *rh, ausrc_error_h *errh, void *arg);


/* Audio Player */

struct auplay;
struct auplay_st;
struct auplay_prm {
	enum aufmt fmt;
	uint32_t   srate;
	uint8_t    ch;
	uint32_t   frame_size;
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


/* Audio Filter */

struct aufilt;
struct aufilt_st;
struct aufilt_prm {
	uint32_t srate;       /**< Sampling rate in [Hz]        */
	uint32_t srate_out;   /**< Output sampling rate in [Hz] */
	uint8_t  ch;          /**< Number of channels           */
	uint32_t frame_size;  /**< Number of samples per frame  */
	uint32_t aec_len;     /**< AEC tail length in [ms]      */
};

typedef int (aufilt_alloc_h)(struct aufilt_st **stp, struct aufilt *af,
			     const struct aufilt_prm *encprm,
			     const struct aufilt_prm *decprm);
typedef int (aufilt_enc_h)(struct aufilt_st *st, struct mbuf *mb);
typedef int (aufilt_dec_h)(struct aufilt_st *st, struct mbuf *mb);
typedef int (aufilt_dbg_h)(struct re_printf *pf, const struct aufilt_st *st);

int aufilt_register(struct aufilt **afp, const char *name,
		    aufilt_alloc_h *alloch, aufilt_enc_h *ench,
		    aufilt_dec_h *dech, aufilt_dbg_h *dbgh);
struct list *aufilt_list(void);


/* Audio Buffer */
struct aubuf;

int  aubuf_alloc(struct aubuf **abp, size_t min_sz, size_t max_sz);
int  aubuf_append(struct aubuf *ab, struct mbuf *mb);
int  aubuf_write(struct aubuf *ab, const uint8_t *p, size_t sz);
void aubuf_read(struct aubuf *ab, uint8_t *p, size_t sz);
int  aubuf_get(struct aubuf *ab, uint32_t ptime, uint8_t *p, size_t sz);
int  aubuf_debug(struct re_printf *pf, const struct aubuf *ab);
size_t aubuf_cur_size(const struct aubuf *ab);


/* menc - media encryption */
struct menc;
struct menc_st;

typedef int  (menc_alloc_h)(struct menc_st **stp, struct menc *me, int proto,
			    void *rtpsock, void *rtcpsock,
			    struct sdp_media *sdpm);
typedef int  (menc_update_h)(struct menc_st *st);


int menc_register(struct menc **mencp, const char *id, menc_alloc_h *alloch,
		  menc_update_h *updateh);


/* natbd */
struct dnsc;
int  natbd_init(const struct sa *laddr, const struct pl *host, uint16_t port,
		struct dnsc *dnsc);
void natbd_close(void);
int  natbd_status(struct re_printf *pf, void *unused);


/* net */

typedef void (net_change_h)(void *arg);

int  net_init(bool prefer_ipv6);
void net_close(void);
int  net_dnssrv_add(const struct sa *sa);
void net_change(uint32_t interval, net_change_h *ch, void *arg);
int  net_debug(struct re_printf *pf, void *unused);
const struct sa *net_laddr(void);
struct dnsc *net_dnsc(void);


/* os */
int mkpath(const char *path);
int get_login_name(const char **login);
int get_homedir(char *path, uint32_t sz);


/* play */
struct play;
int play_file(struct play **playp, const char *filename, int repeat);
int play_tone(struct play **playp, struct mbuf *tone,
	      int srate, int ch, int repeat);


/* ua */
struct ua;

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
	STATMODE_JBUF,
	STATMODE_OFF,

	/* marker */
	STATMODE_N
};

/** Defines the User-Agent event handler */
typedef void (ua_event_h)(enum ua_event ev, const char *prm, void *arg);

struct sip_msg;
typedef void (options_resp_h)(int err, const struct sip_msg *msg, void *arg);

/* Multiple instances */
int  ua_alloc(struct ua **uap, const struct pl *aor);
void ua_set_event_handler(struct ua *ua, ua_event_h *eh, void *arg);
int  ua_connect(struct ua *ua, const char *uri);
void ua_hangup(struct ua *ua);
void ua_answer(struct ua *ua);
void ua_play_digit(struct ua *ua, int key);
int  ua_im_send(struct ua *ua, struct mbuf *peer, struct mbuf *msg);
void ua_toggle_statmode(struct ua *ua);
void ua_set_statmode(struct ua *ua, enum statmode mode);
int  ua_debug(struct re_printf *pf, const struct ua *ua);
int  ua_auth(struct ua *ua, char **username, char **password,
	     const char *realm);
int  ua_options_send(struct ua *ua, const char *uri,
		     options_resp_h *resph, void *arg);
char *ua_aor(struct ua *ua);
struct call *ua_call(struct ua *ua);
int ua_sipfd(const struct ua *ua);


/* One instance */
int  ua_init(const char *software, bool udp, bool tcp, bool tls,
	     exit_h *exith);
void ua_set_uuid(const char *uuid);
void ua_close(void);
void ua_stack_suspend(void);
int  ua_stack_resume(const char *software, bool udp, bool tcp, bool tls);
int  ua_start_all(void);
void ua_stop_all(bool forced);
void ua_next(void);
int  ua_register(struct ua *ua);
int  ua_reset_transp(void);
struct ua *ua_cur(void);
int  ua_print_sip_status(struct re_printf *pf, void *unused);
int  ua_print_reg_status(struct re_printf *pf, void *unused);
int  ua_print_call_status(struct re_printf *pf, void *unused);
struct sip *uag_sip(void);
struct sipsess_sock *uag_sipsess_sock(void);
const char *ua_event_str(enum ua_event ev);


/* ui - user interface */
struct ui;
struct ui_st;
struct ui_prm {
	char *device;
	uint16_t port;
};
typedef void (ui_input_h)(char key, struct re_printf *pf, void *arg);

typedef int  (ui_alloc_h)(struct ui_st **stp, struct ui_prm *prm,
			  ui_input_h *ih, void *arg);
typedef int  (ui_output_h)(struct ui_st *st, const char *str);

int  ui_init(exit_h *exith, bool rdaemon);
void ui_close(void);
int  ui_start(void);
void ui_input(char key, struct re_printf *pf);
void ui_out(const char *str);
void ui_splash(uint32_t n_uas);
void ui_set_incall(bool incall);
int  ui_register(struct ui **uip, const char *name,
		 ui_alloc_h *alloch, ui_output_h *outh);
int ui_allocate(const char *name, ui_input_h *inputh, void *arg);


void audio_loop_test(bool stop);
void video_loop_test(bool stop);


/* Video Source */
struct vidsrc;
struct vidsrc_st;
struct vidsrc_prm {
	void *view;         /**< Optional selfview (set by application) */
	struct vidsz size;
	int fps;
};

typedef void (vidsrc_frame_h)(const struct vidframe *frame, void *arg);
typedef void (vidsrc_error_h)(int err, void *arg);

typedef int  (vidsrc_alloc_h)(struct vidsrc_st **vsp, struct vidsrc *vs,
			      struct vidsrc_prm *prm,
			      const char *fmt, const char *dev,
			      vidsrc_frame_h *frameh,
			      vidsrc_error_h *errorh, void *arg);

int vidsrc_register(struct vidsrc **vp, const char *name,
		    vidsrc_alloc_h *alloch);
const struct vidsrc *vidsrc_find(const char *name);
struct list *vidsrc_list(void);
int vidsrc_alloc(struct vidsrc_st **stp, const char *name,
		 struct vidsrc_prm *prm, const char *fmt, const char *dev,
		 vidsrc_frame_h *frameh, vidsrc_error_h *errorh, void *arg);


/* Video Display */
struct vidisp;
struct vidisp_st;
struct vidisp_prm {
	void *view;  /**< Optional view (set by application or module) */
};

typedef void (vidisp_input_h)(char key, void *arg);
typedef void (vidisp_resize_h)(const struct vidsz *size, void *arg);

typedef int  (vidisp_alloc_h)(struct vidisp_st **vp, struct vidisp *vd,
			      struct vidisp_prm *prm,
			      const char *dev, vidisp_input_h *inputh,
			      vidisp_resize_h *resizeh, void *arg);
typedef int  (vidisp_disp_h)(struct vidisp_st *st, const char *title,
			     const struct vidframe *frame);
typedef void (vidisp_hide_h)(struct vidisp_st *st);

int vidisp_register(struct vidisp **vp, const char *name,
		    vidisp_alloc_h *alloch,
		    vidisp_disp_h *disph, vidisp_hide_h *hideh);


/* Audio Codec */

struct aucodec;
struct aucodec_st;    /* must "inherit" from struct aucodec */

struct aucodec_prm {
	uint32_t srate;
	uint32_t ptime;
};

typedef int (aucodec_alloc_h)(struct aucodec_st **asp, struct aucodec *ac,
			      struct aucodec_prm *encp,
			      struct aucodec_prm *decp,
			      const struct pl *sdp_fmtp);
typedef int (aucodec_enc_h)(struct aucodec_st *s, struct mbuf *dst,
			   struct mbuf *src);
typedef int (aucodec_dec_h)(struct aucodec_st *s, struct mbuf *dst,
			   struct mbuf *src);

int aucodec_register(struct aucodec **ap, const char *pt, const char *name,
		     uint32_t srate, uint8_t ch, const char *fmtp,
		     aucodec_alloc_h *alloch,
		     aucodec_enc_h *ench, aucodec_dec_h *dech);
const char *aucodec_pt(const struct aucodec *ac);
const char *aucodec_name(const struct aucodec *ac);
struct list *aucodec_list(void);
uint32_t aucodec_srate(const struct aucodec *ac);
uint8_t  aucodec_ch(const struct aucodec *ac);
int  aucodec_alloc(struct aucodec_st **sp, const char *name, uint32_t srate,
		   int channels, struct aucodec_prm *enc_prm,
		   struct aucodec_prm *dec_prm, const struct pl *sdp_fmtp);
int aucodec_encode(struct aucodec_st *st, struct mbuf *dst, struct mbuf *src);
int aucodec_decode(struct aucodec_st *st, struct mbuf *dst, struct mbuf *src);


/* Video Codec */
struct vidcodec;
struct vidcodec_st;    /* must "inherit" from struct vidcodec */

struct vidcodec_prm {
	struct vidsz size;
	int fps;
	int bitrate;        /**< Encoder bitrate in [bit/s] */
};

typedef int (vidcodec_send_h)(bool marker, struct mbuf *mb, void *arg);

typedef int (vidcodec_alloc_h)(struct vidcodec_st **sp, struct vidcodec *c,
			       const char *name, struct vidcodec_prm *encp,
			       struct vidcodec_prm *decp,
			       const struct pl *sdp_fmtp,
			       vidcodec_send_h *sendh, void *arg);
typedef int (vidcodec_enc_h)(struct vidcodec_st *s, bool update,
			     const struct vidframe *frame);
typedef int (vidcodec_dec_h)(struct vidcodec_st *s, struct vidframe *frame,
			     bool marker, struct mbuf *src);

int vidcodec_register(struct vidcodec **cp, const char *pt, const char *name,
		      const char *fmtp, vidcodec_alloc_h *alloch,
		      vidcodec_enc_h *ench, vidcodec_dec_h *dech);
const struct vidcodec *vidcodec_find(const char *name);
const char *vidcodec_pt(const struct vidcodec *vc);
const char *vidcodec_name(const struct vidcodec *vc);
struct list *vidcodec_list(void);
void vidcodec_set_fmtp(struct vidcodec *vc, const char *fmtp);


/* Audio */
void audio_mute(struct audio *a, bool muted);
void audio_enable_txthread(struct audio *a, bool enabled);


/* Video */
struct video;
void video_mute(struct video *v, bool muted);
void *video_view(const struct video *v);
int video_selfview(struct video *v, void *view);
int video_pip(struct video *v, const struct vidrect *rect);
int video_set_fullscreen(struct video *v, bool fs);


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
const struct mnat *mnat_find(const char *id);


/* Real-time */
int realtime_enable(bool enable, int fps);


/* Modules */
#ifdef STATIC
#define DECL_EXPORTS(name) exports_ ##name
#else
#define DECL_EXPORTS(name) exports
#endif

#ifdef __cplusplus
}
#endif
