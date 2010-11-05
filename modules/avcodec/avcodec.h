/**
 * @file avcodec.h  Video codecs using FFmpeg libavcodec -- internal API
 *
 * Copyright (C) 2010 Creytiv.com
 */


enum {
	MAX_RTP_SIZE     = 1024,
	RTP_PRESZ        = 4 + RTP_HEADER_SIZE
};

struct picsz {
	enum h263_fmt fmt;  /**< Picture size */
	uint8_t mpi;        /**< Minimum Picture Interval (1-32) */
};

struct vidcodec_st {
	struct vidcodec *vc;  /* must be first member */

	struct {
		AVCodec *codec;
		AVCodecContext *ctx;
		AVFrame *pict;
		struct vidsz size;
		struct mbuf *mb;
		size_t sz_max; /* todo: figure out proper buffer size */
	} enc, dec;
#ifdef USE_X264
	x264_t *x264;
	int64_t pts;
#endif
	struct mbuf *mb_frag;

	enum CodecID codec_id;
	union {
		struct {
			struct picsz picszv[8];
			uint32_t picszn;
		} h263;

		struct {
			uint32_t packetization_mode;
			uint32_t profile_idc;
			uint32_t profile_iop;
			uint32_t level_idc;
			uint32_t max_fs;
			uint32_t max_smbps;
		} h264;
	} u;

	vidcodec_send_h *sendh;
	void *arg;
};

int decode_sdpparam_h263(struct vidcodec_st *st, const struct pl *name,
			 const struct pl *val);
int decode_sdpparam_h264(struct vidcodec_st *st, const struct pl *name,
			 const struct pl *val);
int h263_packetize(struct vidcodec_st *st, struct mbuf *mb);
int h264_packetize(struct vidcodec_st *st, struct mbuf *mb);
int h264_decode(struct vidcodec_st *st, struct mbuf *src);
#ifdef USE_X264
int enc_x264(struct vidcodec_st *st, bool update,
	     const struct vidframe *frame);
#endif
