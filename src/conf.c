/**
 * @file conf.c  Application Configuration
 *
 * Copyright (C) 2010 Creytiv.com
 */
#define _BSD_SOURCE 1
#include <fcntl.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <stdio.h>
#include <sys/stat.h>
#ifdef HAVE_IO_H
#include <io.h>
#endif
#include <re.h>
#include <rem.h>
#include <baresip.h>
#include "core.h"


#define DEBUG_MODULE "conf"
#define DEBUG_LEVEL 5
#include <re_dbg.h>


#ifdef WIN32
#define open _open
#define read _read
#define close _close
#endif


#undef MOD_PRE
#define MOD_PRE ""  /**< Module prefix */


static const char *conf_path = NULL;
static const char file_accounts[] = "accounts";
static const char file_config[]   = "config";
static struct conf *conf_obj;


/** Core Run-time Configuration - populated from config file */
struct config config = {
	/* Input */
	{
		"/dev/event0",
		5555
	},

	/** SIP User-Agent */
	{
		16,
		"",
		""
	},

	/** Audio */
	{
		"","",
		"","",
		"","",
		{8000, 48000},
		{1, 2},
		0,
		0,
		false,
	},

	/** Video */
	{
		"", "",
		352, 288,
		384000,
		25,
	},

	/** Audio/Video Transport */
	{
		0xb8,
		{1024, 49152},
		{512000, 1024000},
		true,
		false,
		{5, 10}
	},

	{
		""
	},

	{
		""
	},
};


/**
 * Check if a file exists
 *
 * @param path Filename
 *
 * @return True if exist, False if not
 */
bool conf_fileexist(const char *path)
{
	struct stat st;

	if (!path)
		 return false;

	if (stat(path, &st) < 0)
		 return false;

	if ((st.st_mode & S_IFMT) != S_IFREG)
		 return false;

	return st.st_size > 0;
}


static void print_populated(const char *what, uint32_t n)
{
	(void)re_printf("Populated %u %s%s\n", n, what, 1==n ? "" : "s");
}


/**
 * Parse a config file, calling handler for each line
 *
 * @param filename Config file
 * @param ch       Line handler
 *
 * @return 0 if success, otherwise errorcode
 */
int conf_parse(const char *filename, confline_h *ch)
{
	struct pl pl, val;
	struct mbuf *mb;
	int err = 0, fd = open(filename, O_RDONLY);
	if (fd < 0)
		return errno;

	mb = mbuf_alloc(1024);
	if (!mb) {
		err = ENOMEM;
		goto out;
	}

	for (;;) {
		uint8_t buf[1024];

		const ssize_t n = read(fd, (void *)buf, sizeof(buf));
		if (n < 0) {
			err = errno;
			break;
		}
		else if (n == 0)
			break;

		err |= mbuf_write_mem(mb, buf, n);
	}

	pl.p = (const char *)mb->buf;
	pl.l = mb->end;

	while (pl.p < ((const char *)mb->buf + mb->end) && !err) {
		const char *lb = pl_strchr(&pl, '\n');

		val.p = pl.p;
		val.l = lb ? (uint32_t)(lb - pl.p) : pl.l;
		pl_advance(&pl, val.l + 1);

		if (!val.l || val.p[0] == '#')
			continue;

		err = ch(&val);
	}

 out:
	mem_deref(mb);
	(void)close(fd);

	return err;
}


static int conf_write_template(const char *file)
{
	FILE *f = NULL;
	const char *login, *pass, *domain;

	DEBUG_NOTICE("creating configuration template %s\n", file);

	f = fopen(file, "w");
	if (!f) {
		DEBUG_WARNING("writing %s: %m\n", file, errno);
		return errno;
	}

	login = pass = sys_username();
	if (!login) {
		login = "user";
		pass = "pass";
	}

	domain = net_domain();
	if (!domain)
		domain = "domain";

	(void)re_fprintf(f,
			 "#\n"
			 "# SIP accounts - one account per line\n"
			 "#\n"
			 "# Displayname <sip:user:password@domain"
			 ";uri-params>;addr-params\n"
			 "#\n"
			 "#  uri-params:\n"
			 "#    ;transport={udp,tcp,tls}\n"
			 "#\n"
			 "#  addr-params:\n"
			 "#    ;answermode={manual,early,auto}\n"
			 "#    ;audio_codecs=speex/16000,pcma,...\n"
			 "#    ;auth_user=username\n"
			 "#    ;mediaenc={srtp,srtp-mand}\n"
			 "#    ;medianat={stun,turn,ice}\n"
			 "#    ;outbound=sip:primary.example.com\n"
			 "#    ;outbound2=sip:secondary.example.com\n"
			 "#    ;ptime={10,20,30,40,...}\n"
			 "#    ;regint=3600\n"
			 "#    ;regq=0.5\n"
			 "#    ;rtpkeep={zero,stun,dyna,rtcp}\n"
			 "#    ;sipnat={outbound}\n"
			 "#    ;stunserver=stun:[user:pass]@host[:port]\n"
			 "#    ;video_codecs=h264,h263,...\n"
			 "#\n"
			 "# Examples:\n"
			 "#\n"
			 "#  <sip:user:secret@domain.com;transport=tcp>\n"
			 "#  <sip:user:secret@1.2.3.4;transport=tcp>\n"
			 "#  <sip:user:secret@"
			 "[2001:df8:0:16:216:6fff:fe91:614c]:5070"
			 ";transport=tcp>\n"
			 "#\n"
			 "<sip:%s:%s@%s>\n", login, pass, domain);

	if (f)
		(void)fclose(f);

	return 0;
}


static int conf_write_config_template(const char *file)
{
	FILE *f = NULL;
	int err = 0;

	DEBUG_NOTICE("creating config template %s\n", file);

	f = fopen(file, "w");
	if (!f) {
		DEBUG_WARNING("writing %s: %m\n", file, errno);
		return errno;
	}

	(void)re_fprintf(f, "#\n");
	(void)re_fprintf(f, "# baresip configuration\n");
	(void)re_fprintf(f, "#\n");
	(void)re_fprintf(f, "\n#------------------------------------"
			 "------------------------------------------\n");

	(void)re_fprintf(f, "\n# Core\n");
	(void)re_fprintf(f, "poll_method\t\t%s\t\t# poll, select, epoll ..\n",
			 poll_method_name(poll_method_best()));

	(void)re_fprintf(f, "\n# Input\n");
	(void)re_fprintf(f, "input_device\t\t/dev/event0\n");
	(void)re_fprintf(f, "input_port\t\t5555\n");

	(void)re_fprintf(f, "\n# SIP\n");
	(void)re_fprintf(f, "sip_trans_bsize\t\t128\n");
	(void)re_fprintf(f, "#sip_listen\t\t127.0.0.1:5050\n");
	(void)re_fprintf(f, "#sip_certificate\t\tcert.pem\n");

	(void)re_fprintf(f, "\n# Audio\n");
	(void)re_fprintf(f, "#audio_player\t\talsa,default\n");
	(void)re_fprintf(f, "#audio_source\t\talsa,default\n");
	(void)re_fprintf(f, "#audio_alert\t\talsa,default\n");
	(void)re_fprintf(f, "audio_srate\t\t%u-%u\n", config.audio.srate.min,
			 config.audio.srate.max);
	(void)re_fprintf(f, "audio_channels\t\t%u-%u\n",
			 config.audio.channels.min, config.audio.channels.max);
	(void)re_fprintf(f, "#ausrc_srate\t\t%u\n",
			 config.audio.srate_src);
	(void)re_fprintf(f, "#auplay_srate\t\t%u\n",
			 config.audio.srate_play);

#ifdef USE_VIDEO
	(void)re_fprintf(f, "\n# Video\n");
	(void)re_fprintf(f, "#video_source\t\tv4l2,/dev/video0\n");
	(void)re_fprintf(f, "video_size\t\t%dx%d\n", config.video.width,
			 config.video.height);
	(void)re_fprintf(f, "video_bitrate\t\t%u\n", config.video.bitrate);
	(void)re_fprintf(f, "video_fps\t\t%u\n", config.video.fps);
	(void)re_fprintf(f, "#video_selfview\t\twindow # {window,pip}\n");
#endif

	(void)re_fprintf(f, "\n# AVT - Audio/Video Transport\n");
	(void)re_fprintf(f, "rtp_tos\t\t\t184\n");
	(void)re_fprintf(f, "#rtp_ports\t\t\t10000-20000\n");
	(void)re_fprintf(f, "#rtp_bandwidth\t\t\t512-1024 # [kbit/s]\n");
	(void)re_fprintf(f, "rtcp_enable\t\t\tyes\n");
	(void)re_fprintf(f, "rtcp_mux\t\t\tno\n");
	(void)re_fprintf(f, "jitter_buffer_delay\t%u-%u\t\t# frames\n",
			 config.avt.jbuf_del.min, config.avt.jbuf_del.max);

	(void)re_fprintf(f, "\n# Network\n");
	(void)re_fprintf(f, "#dns_server\t\t10.0.0.1:53\n");
	(void)re_fprintf(f, "#net_interface\t\teth0\n");

	(void)re_fprintf(f, "\n#------------------------------------"
			 "------------------------------------------\n");
	(void)re_fprintf(f, "# Modules\n");
	(void)re_fprintf(f, "\n");

#ifdef WIN32
	(void)re_fprintf(f, "module_path\t\t\n");
#elif defined (PREFIX)
	(void)re_fprintf(f, "module_path\t\t" PREFIX "/lib/baresip/modules\n");
#else
	(void)re_fprintf(f, "module_path\t\t/usr/lib/baresip/modules\n");
#endif

	(void)re_fprintf(f, "\n# UI Modules\n");
	(void)re_fprintf(f, "module\t\t\t" MOD_PRE "stdio" MOD_EXT "\n");
	(void)re_fprintf(f, "module\t\t\t" MOD_PRE "cons" MOD_EXT "\n");
	(void)re_fprintf(f, "#module\t\t\t" MOD_PRE "evdev" MOD_EXT "\n");

	(void)re_fprintf(f, "\n# Audio codec Modules (in order)\n");
	(void)re_fprintf(f, "#module\t\t\t" MOD_PRE "opus" MOD_EXT "\n");
	(void)re_fprintf(f, "#module\t\t\t" MOD_PRE "silk" MOD_EXT "\n");
	(void)re_fprintf(f, "#module\t\t\t" MOD_PRE "amr" MOD_EXT "\n");
	(void)re_fprintf(f, "#module\t\t\t" MOD_PRE "g7221" MOD_EXT "\n");
	(void)re_fprintf(f, "#module\t\t\t" MOD_PRE "g722" MOD_EXT "\n");
	(void)re_fprintf(f, "module\t\t\t" MOD_PRE "g711" MOD_EXT "\n");
	(void)re_fprintf(f, "#module\t\t\t" MOD_PRE "gsm" MOD_EXT "\n");
	(void)re_fprintf(f, "#module\t\t\t" MOD_PRE "l16" MOD_EXT "\n");
	(void)re_fprintf(f, "#module\t\t\t" MOD_PRE "speex" MOD_EXT "\n");
	(void)re_fprintf(f, "#module\t\t\t" MOD_PRE "bv32" MOD_EXT "\n");

	(void)re_fprintf(f, "\n# Audio filter Modules (in encoding order)\n");
	(void)re_fprintf(f, "# NOTE: AEC should be before Preproc\n");
	(void)re_fprintf(f, "module\t\t\t" MOD_PRE "vumeter" MOD_EXT "\n");
	(void)re_fprintf(f, "#module\t\t\t" MOD_PRE "sndfile" MOD_EXT "\n");
	(void)re_fprintf(f, "#module\t\t\t" MOD_PRE "speex_aec" MOD_EXT "\n");
	(void)re_fprintf(f, "#module\t\t\t" MOD_PRE "speex_pp" MOD_EXT "\n");
	(void)re_fprintf(f, "#module\t\t\t" MOD_PRE "plc" MOD_EXT "\n");

	(void)re_fprintf(f, "\n# Audio driver Modules\n");
#if defined (WIN32)
	(void)re_fprintf(f, "module\t\t\t" MOD_PRE "winwave" MOD_EXT "\n");
#elif defined (__SYMBIAN32__)
	(void)re_fprintf(f, "module\t\t\t" MOD_PRE "mda" MOD_EXT "\n");
#elif defined (DARWIN)
	(void)re_fprintf(f, "module\t\t\t" MOD_PRE "coreaudio" MOD_EXT "\n");
#else
	(void)re_fprintf(f, "module\t\t\t" MOD_PRE "oss" MOD_EXT "\n");
	(void)re_fprintf(f, "#module\t\t\t" MOD_PRE "alsa" MOD_EXT "\n");
#endif
	(void)re_fprintf(f, "#module\t\t\t" MOD_PRE "portaudio" MOD_EXT "\n");
	(void)re_fprintf(f, "#module\t\t\t" MOD_PRE "gst" MOD_EXT "\n");

	(void)re_fprintf(f, "\n# Video codec Modules (in order)\n");
#ifdef USE_FFMPEG
	(void)re_fprintf(f, "module\t\t\t" MOD_PRE "avcodec" MOD_EXT "\n");
#else
	(void)re_fprintf(f, "#module\t\t\t" MOD_PRE "avcodec" MOD_EXT "\n");
#endif
	(void)re_fprintf(f, "#module\t\t\t" MOD_PRE "vpx" MOD_EXT "\n");

	(void)re_fprintf(f, "\n# Video filter Modules (in order)\n");
	(void)re_fprintf(f, "#module\t\t\t" MOD_PRE "selfview" MOD_EXT "\n");

	(void)re_fprintf(f, "\n# Video source modules\n");
#if defined (DARWIN)
	(void)re_fprintf(f, "module\t\t\t" MOD_PRE "qtcapture" MOD_EXT "\n");
	(void)re_fprintf(f, "#module\t\t\t" MOD_PRE "quicktime" MOD_EXT "\n");
#else
	(void)re_fprintf(f, "#module\t\t\t" MOD_PRE "v4l" MOD_EXT "\n");
	(void)re_fprintf(f, "#module\t\t\t" MOD_PRE "v4l2" MOD_EXT "\n");
#endif
#ifdef USE_FFMPEG
	(void)re_fprintf(f, "#module\t\t\t" MOD_PRE "avformat" MOD_EXT "\n");
#endif
	(void)re_fprintf(f, "#module\t\t\t" MOD_PRE "x11grab" MOD_EXT "\n");

	(void)re_fprintf(f, "\n# Video display modules\n");
#ifdef USE_SDL
	(void)re_fprintf(f, "module\t\t\t" MOD_PRE "sdl" MOD_EXT "\n");
#else
	(void)re_fprintf(f, "#module\t\t\t" MOD_PRE "sdl" MOD_EXT "\n");
#endif
#ifdef DARWIN
	(void)re_fprintf(f, "module\t\t\t" MOD_PRE "opengl" MOD_EXT "\n");
#endif
	(void)re_fprintf(f, "#module\t\t\t" MOD_PRE "x11" MOD_EXT "\n");

	(void)re_fprintf(f, "\n# Media NAT modules\n");
	(void)re_fprintf(f, "module\t\t\t" MOD_PRE "stun" MOD_EXT "\n");
	(void)re_fprintf(f, "module\t\t\t" MOD_PRE "turn" MOD_EXT "\n");
	(void)re_fprintf(f, "module\t\t\t" MOD_PRE "ice" MOD_EXT "\n");

	(void)re_fprintf(f, "\n# Media encoding modules\n");
	(void)re_fprintf(f, "#module\t\t\t" MOD_PRE "srtp" MOD_EXT "\n");
	(void)re_fprintf(f, "\n");

	(void)re_fprintf(f, "\n#------------------------------------"
			 "------------------------------------------\n");
	(void)re_fprintf(f, "# Temporary Modules (loaded then unloaded)\n");
	(void)re_fprintf(f, "\n");
	(void)re_fprintf(f, "#module_tmp\t\t" MOD_PRE "uuid" MOD_EXT "\n");
	(void)re_fprintf(f, "\n");

	(void)re_fprintf(f, "\n#------------------------------------"
			 "------------------------------------------\n");
	(void)re_fprintf(f, "# Application Modules\n");
	(void)re_fprintf(f, "\n");
	(void)re_fprintf(f, "#module_app\t\t" MOD_PRE "auloop"MOD_EXT"\n");
	(void)re_fprintf(f, "module_app\t\t"  MOD_PRE "contact"MOD_EXT"\n");
	(void)re_fprintf(f, "module_app\t\t"  MOD_PRE "menu"MOD_EXT"\n");
	(void)re_fprintf(f, "#module_app\t\t" MOD_PRE "natbd"MOD_EXT"\n");
	(void)re_fprintf(f, "#module_app\t\t" MOD_PRE "presence"MOD_EXT"\n");
	(void)re_fprintf(f, "#module_app\t\t" MOD_PRE "syslog"MOD_EXT"\n");
	(void)re_fprintf(f, "#module_app\t\t" MOD_PRE "vidloop"MOD_EXT"\n");
	(void)re_fprintf(f, "\n");

	(void)re_fprintf(f, "\n#------------------------------------"
			 "------------------------------------------\n");
	(void)re_fprintf(f, "# Module parameters\n");
	(void)re_fprintf(f, "\n");

	(void)re_fprintf(f, "\n# Speex codec parameters\n");
	(void)re_fprintf(f, "speex_quality\t\t7 # 0-10\n");
	(void)re_fprintf(f, "speex_complexity\t7 # 0-10\n");
	(void)re_fprintf(f, "speex_enhancement\t0 # 0-1\n");
	(void)re_fprintf(f, "speex_vbr\t\t0 # Variable Bit Rate 0-1\n");
	(void)re_fprintf(f, "speex_vad\t\t0 # Voice Activity Detection 0-1\n");
	(void)re_fprintf(f, "speex_agc_level\t8000\n");

	(void)re_fprintf(f, "\n# NAT Behavior Discovery\n");
	(void)re_fprintf(f, "natbd_server\t\tcreytiv.com\n");
	(void)re_fprintf(f, "natbd_interval\t\t600\t\t# in seconds\n");

	if (f)
		(void)fclose(f);

	return err;
}


/**
 * Set the path to configuration files
 *
 * @param path Configuration path
 */
void conf_path_set(const char *path)
{
	conf_path = path;
}


#if defined (WIN32) || defined (__SYMBIAN32__)
#define DIR_SEP "\\"
#else
#define DIR_SEP "/"
#endif


/**
 * Get the path to configuration files
 *
 * @param path Buffer to write path
 * @param sz   Size of path buffer
 *
 * @return 0 if success, otherwise errorcode
 */
int conf_path_get(char *path, size_t sz)
{
	char buf[256];
	int err;

	/* Use explicit conf path */
	if (conf_path) {
		if (re_snprintf(path, sz, "%s", conf_path) < 0)
			return ENOMEM;
		return 0;
	}

	err = fs_gethome(buf, sizeof(buf));
	if (err)
		return err;

	if (re_snprintf(path, sz, "%s" DIR_SEP ".baresip", buf) < 0)
		return ENOMEM;

	return 0;
}


/**
 * Get the SIP accounts
 *
 * @param ch Account handler
 *
 * @return 0 if success, otherwise errorcode
 */
int conf_accounts_get(confline_h *ch)
{
	char path[256] = "", file[256] = "";
	int err;

	err = conf_path_get(path, sizeof(path));
	if (err) {
		DEBUG_WARNING("accounts: conf_path_get (%m)\n", err);
		return err;
	}

	if (re_snprintf(file, sizeof(file), "%s/%s", path, file_accounts) < 0)
		return ENOMEM;

	if (!conf_fileexist(file)) {

		(void)fs_mkdir(path, 0755);

		err = conf_write_template(file);
		if (err)
			return err;
	}

	err = conf_parse(file, ch);
	if (err)
		return err;

	print_populated("account", list_count(uag_list()));

	if (list_isempty(uag_list())) {
		DEBUG_WARNING("No SIP accounts found - check your config\n");
		return ENOENT;
	}

	return 0;
}


static int conf_get_range(struct conf *conf, const char *name,
			  struct range *rng)
{
	struct pl r, min, max;
	uint32_t v;
	int err;

	err = conf_get(conf, name, &r);
	if (err)
		return err;

	err = re_regex(r.p, r.l, "[0-9]+-[0-9]+", &min, &max);
	if (err) {
		/* fallback to non-range numeric value */
		err = conf_get_u32(conf, name, &v);
		if (err) {
			DEBUG_WARNING("%s: could not parse range: (%r)\n",
				      name, &r);
			return err;
		}

		rng->min = rng->max = v;

		return err;
	}

	rng->min = pl_u32(&min);
	rng->max = pl_u32(&max);

	return 0;
}


static int conf_get_csv(struct conf *conf, const char *name,
			char *str1, size_t sz1, char *str2, size_t sz2)
{
	struct pl r, pl1, pl2 = pl_null;
	int err;

	err = conf_get(conf, name, &r);
	if (err)
		return err;

	err = re_regex(r.p, r.l, "[^,]+,[^]*", &pl1, &pl2);
	if (err)
		return err;

	(void)pl_strcpy(&pl1, str1, sz1);
	if (pl_isset(&pl2))
		(void)pl_strcpy(&pl2, str2, sz2);

	return 0;
}


int conf_get_vidsz(struct conf *conf, const char *name, struct vidsz *sz)
{
	struct pl r, w, h;
	int err;

	err = conf_get(conf, name, &r);
	if (err)
		return err;

	w.l = h.l = 0;
	err = re_regex(r.p, r.l, "[0-9]+x[0-9]+", &w, &h);
	if (err)
		return err;

	if (pl_isset(&w) && pl_isset(&h)) {
		sz->w = pl_u32(&w);
		sz->h = pl_u32(&h);
	}

	/* check resolution */
	if (sz->w & 0x1 || sz->h & 0x1) {
		DEBUG_WARNING("%s: should be multiple of 2 (%u x %u)\n",
			      name, sz->w, sz->h);
		return EINVAL;
	}

	return 0;
}


static int dns_server_handler(const struct pl *pl, void *arg)
{
	struct sa sa;
	int err;

	(void)arg;

	err = sa_decode(&sa, pl->p, pl->l);
	if (err) {
		DEBUG_WARNING("dns_server: could not decode `%r'\n", pl);
		return err;
	}

	err = net_dnssrv_add(&sa);
	if (err) {
		DEBUG_WARNING("failed to add nameserver %r: %m\n", pl, err);
	}

	return err;
}


static int config_parse(struct conf *conf)
{
	struct pl pollm, as, ap;
	enum poll_method method;
	struct vidsz size = {0, 0};
	uint32_t v;
	int err = 0;

	/* Core */
	if (0 == conf_get(conf, "poll_method", &pollm)) {
		if (0 == poll_method_type(&method, &pollm)) {
			err = poll_method_set(method);
			if (err) {
				DEBUG_WARNING("poll method (%r) set: %m\n",
					      &pollm, err);
			}
		}
		else {
			DEBUG_WARNING("unknown poll method (%r)\n", &pollm);
		}
	}

	/* Input */
	(void)conf_get_str(conf, "input_device", config.input.device,
			   sizeof(config.input.device));
	(void)conf_get_u32(conf, "input_port", &config.input.port);

	/* SIP */
	(void)conf_get_u32(conf, "sip_trans_bsize", &config.sip.trans_bsize);
	(void)conf_get_str(conf, "sip_listen", config.sip.local,
			   sizeof(config.sip.local));
	(void)conf_get_str(conf, "sip_certificate", config.sip.cert,
			   sizeof(config.sip.cert));

	/* Audio */
	(void)conf_get_csv(conf, "audio_player",
			   config.audio.play_mod,
			   sizeof(config.audio.play_mod),
			   config.audio.play_dev,
			   sizeof(config.audio.play_dev));

	(void)conf_get_csv(conf, "audio_source",
			   config.audio.src_mod, sizeof(config.audio.src_mod),
			   config.audio.src_dev, sizeof(config.audio.src_dev));

	(void)conf_get_csv(conf, "audio_alert",
			   config.audio.alert_mod,
			   sizeof(config.audio.alert_mod),
			   config.audio.alert_dev,
			   sizeof(config.audio.alert_dev));

	(void)conf_get_range(conf, "audio_srate", &config.audio.srate);
	(void)conf_get_range(conf, "audio_channels", &config.audio.channels);
	(void)conf_get_u32(conf, "ausrc_srate", &config.audio.srate_src);
	(void)conf_get_u32(conf, "auplay_srate", &config.audio.srate_play);

	if (0 == conf_get(conf, "audio_source", &as) &&
	    0 == conf_get(conf, "audio_player", &ap))
		config.audio.src_first = as.p < ap.p;

	/* Video */
	(void)conf_get_csv(conf, "video_source",
			   config.video.src_mod, sizeof(config.video.src_mod),
			   config.video.src_dev, sizeof(config.video.src_dev));
	if (0 == conf_get_vidsz(conf, "video_size", &size)) {
		config.video.width  = size.w;
		config.video.height = size.h;
	}
	(void)conf_get_u32(conf, "video_bitrate", &config.video.bitrate);
	(void)conf_get_u32(conf, "video_fps", &config.video.fps);

	/* AVT - Audio/Video Transport */
	if (0 == conf_get_u32(conf, "rtp_tos", &v))
		config.avt.rtp_tos = v;
	(void)conf_get_range(conf, "rtp_ports", &config.avt.rtp_ports);
	if (0 == conf_get_range(conf, "rtp_bandwidth",
				&config.avt.rtp_bw)) {
		config.avt.rtp_bw.min *= 1024;
		config.avt.rtp_bw.max *= 1024;
	}
	(void)conf_get_bool(conf, "rtcp_enable", &config.avt.rtcp_enable);
	(void)conf_get_bool(conf, "rtcp_mux", &config.avt.rtcp_mux);
	(void)conf_get_range(conf, "jitter_buffer_delay",
			     &config.avt.jbuf_del);

	if (err) {
		DEBUG_WARNING("configure parse error (%m)\n", err);
	}

	(void)conf_apply(conf, "dns_server", dns_server_handler, NULL);

	(void)conf_get_str(conf, "net_interface",
			   config.net.ifname, sizeof(config.net.ifname));

	/* BFCP */
	(void)conf_get_str(conf, "bfcp_proto", config.bfcp.proto,
			   sizeof(config.bfcp.proto));

	return err;
}


static int config_mod_parse(const struct conf *conf)
{
	int err;

	err = module_init(conf);
	if (err) {
		DEBUG_WARNING("configure module parse error (%m)\n", err);
	}

	return err;
}


/**
 * Configure the system with default settings
 *
 * @return 0 if success, otherwise errorcode
 */
int conf_configure(void)
{
	char path[256], file[256];
	int err;

#if defined (WIN32) || defined (__SYMBIAN32__)
	dbg_init(DBG_INFO, DBG_NONE);
#endif

	err = conf_path_get(path, sizeof(path));
	if (err) {
		DEBUG_WARNING("could not get config path: %m\n", err);
		return err;
	}

	if (re_snprintf(file, sizeof(file), "%s/%s", path, &file_config) < 0)
		return ENOMEM;

	if (!conf_fileexist(file)) {

		(void)fs_mkdir(path, 0755);

		err = conf_write_config_template(file);
		if (err)
			goto out;
	}

	err = conf_alloc(&conf_obj, file);
	if (err)
		goto out;

	err = config_parse(conf_obj);
	if (err)
		goto out;

 out:
	conf_obj = mem_deref(conf_obj);
	return err;
}


/**
 * Load all modules from config file
 *
 * @return 0 if success, otherwise errorcode
 */
int conf_modules(void)
{
	char path[256], file[256];
	int err;

	err = conf_path_get(path, sizeof(path));
	if (err)
		return err;

	if (re_snprintf(file, sizeof(file), "%s/%s", path, &file_config) < 0)
		return ENOMEM;

	err = conf_alloc(&conf_obj, file);
	if (err)
		goto out;

	err = config_mod_parse(conf_obj);
	if (err)
		goto out;

	print_populated("audio codec",  list_count(aucodec_list()));
	print_populated("audio filter", list_count(aufilt_list()));
	print_populated("video codec",  list_count(vidcodec_list()));
	print_populated("video filter", list_count(vidfilt_list()));

 out:
	conf_obj = mem_deref(conf_obj);
	return err;
}


/**
 * Get system configuration from a specific file
 *
 * @param file Config file
 *
 * @return 0 if success, otherwise errorcode
 */
int conf_system_get_file(const char *file)
{
	int err;

	err = conf_alloc(&conf_obj, file);
	if (err)
		return err;

	err = config_parse(conf_obj);
	if (err)
		goto out;

	err = config_mod_parse(conf_obj);

 out:
	conf_obj = mem_deref(conf_obj);
	return err;
}


/**
 * Get system configuration for a given path
 *
 * @param path Path to config file
 *
 * @return 0 if success, otherwise errorcode
 */
int conf_system_get(const char *path)
{
	char file[512];

	if (re_snprintf(file, sizeof(file), "%s/%s", path, &file_config) < 0)
		return ENOMEM;

	return conf_system_get_file(file);
}


/**
 * Get system configuration from a buffer
 *
 * @param buf Config buffer
 * @param sz  Size of buffer
 *
 * @return 0 if success, otherwise errorcode
 */
int conf_system_get_buf(const uint8_t *buf, size_t sz)
{
	int err;

	err = conf_alloc_buf(&conf_obj, buf, sz);
	if (err)
		return err;

	err = config_parse(conf_obj);
	if (err)
		goto out;

	err = config_mod_parse(conf_obj);

 out:
	conf_obj = mem_deref(conf_obj);
	return err;
}


/**
 * Get the current configuration object
 *
 * @return Config object
 *
 * @note It is only available during init
 */
struct conf *conf_cur(void)
{
	return conf_obj;
}
