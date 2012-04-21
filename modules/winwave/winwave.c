/**
 * @file winwave.c Windows sound driver
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <re.h>
#include <rem.h>
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <baresip.h>


#define DEBUG_MODULE "winwave"
#define DEBUG_LEVEL 5
#include <re_dbg.h>


#define WRITE_BUFFERS  4
#define READ_BUFFERS   4

/* Increment with wrapping */
#define INC_WPOS(a) ((a) = (((a) + 1) % WRITE_BUFFERS))
#define INC_RPOS(a) ((a) = (((a) + 1) % READ_BUFFERS))


struct dspbuf {
	WAVEHDR      wh;
	struct mbuf *mb;
};

struct ausrc_st {
	struct ausrc *as;      /* inheritance */
	struct dspbuf bufs[READ_BUFFERS];
	int pos;
	HWAVEIN wavein;
	struct ausrc_prm prm;
	bool rdy;
	bool stop;
	size_t inuse;
	size_t n;
	ausrc_read_h *rh;
	void *arg;
};

struct auplay_st {
	struct auplay *ap;      /* inheritance */
	struct dspbuf  bufs[WRITE_BUFFERS];
	int pos;
	struct auplay_prm prm;
	HWAVEOUT waveout;
	bool rdy;
	size_t inuse;
	size_t n;
	auplay_write_h *wh;
	void *arg;
};

static struct ausrc *ausrc;
static struct auplay *auplay;
static int play_dev_count = 0;
static int src_dev_count = 0;


static int dsp_write(struct auplay_st *st)
{
	MMRESULT res;
	WAVEHDR *wh;
	struct mbuf *mb;

	if (!st->rdy)
		return EINVAL;

	wh = &st->bufs[st->pos].wh;
	if (wh->dwFlags & WHDR_PREPARED) {
		return EINVAL;
	}
	mb = st->bufs[st->pos].mb;
	wh->lpData = (LPSTR)mb->buf;

	if (st->wh) {
		st->n++;
		st->wh(mb->buf, mb->size, st->arg);
	}

	mb->pos = 0;
	mb->end = mb->size;
	wh->dwBufferLength = mb->size;
	wh->dwFlags = 0;
	wh->dwUser = (DWORD_PTR) mb;
	waveOutPrepareHeader(st->waveout, wh, sizeof(WAVEHDR));
	INC_WPOS(st->pos);
	res = waveOutWrite(st->waveout, wh, sizeof (WAVEHDR));
	if (res != MMSYSERR_NOERROR)
		DEBUG_WARNING("dsp_write: waveOutWrite: failed: %08x\n", res);
	else
		st->inuse++;

	return 0;
}


static void CALLBACK waveOutCallback(HWAVEOUT hwo,
				     UINT uMsg,
				     DWORD_PTR dwInstance,
				     DWORD_PTR dwParam1,
				     DWORD_PTR dwParam2)
{
	struct auplay_st *st = (struct auplay_st *) dwInstance;
	WAVEHDR   *wh = (WAVEHDR *) dwParam1;

	(void)hwo;
	(void)dwParam2;

	switch (uMsg) {

	case WOM_OPEN:
		st->rdy = true;
		break;

	case WOM_DONE:
		/*LOCK();*/
		waveOutUnprepareHeader(st->waveout, wh, sizeof(WAVEHDR));
		/*UNLOCK();*/
		st->inuse--;
		dsp_write(st);
		break;

	case WOM_CLOSE:
		st->rdy = false;
		break;

	default:
		break;
	}
}


static void add_wave_in(struct ausrc_st *st)
{
	MMRESULT   res;
	struct dspbuf *db = &st->bufs[st->pos];
	WAVEHDR *wh = &db->wh;

	wh->lpData = (LPSTR)db->mb->buf;
	wh->dwBufferLength = db->mb->size;
	wh->dwBytesRecorded = 0;
	wh->dwFlags = 0;
	wh->dwUser = (DWORD_PTR) db->mb;
	waveInPrepareHeader(st->wavein, wh, sizeof(WAVEHDR));
	res = waveInAddBuffer(st->wavein, wh, sizeof(WAVEHDR));
	if (res != MMSYSERR_NOERROR)
		DEBUG_WARNING("add_wave_in: waveOutWrite fail: %08x\n", res);
	INC_RPOS(st->pos);

	st->inuse++;
}


static void CALLBACK waveInCallback(HWAVEOUT hwo,
				    UINT uMsg,
				    DWORD_PTR dwInstance,
				    DWORD_PTR dwParam1,
				    DWORD_PTR dwParam2)
{
	struct ausrc_st  *st = (struct ausrc_st *) dwInstance;
	WAVEHDR   *wh = (WAVEHDR *) dwParam1;
	struct mbuf *mb;

	(void)hwo;
	(void)dwParam2;

	switch (uMsg) {

	case WIM_CLOSE:
		st->rdy = false;
		break;

	case WIM_OPEN:
		st->rdy = true;
		break;

	case WIM_DATA:
		if (st->stop)
			break;

		if (st->inuse < 3)
			add_wave_in(st);

		mb = (struct mbuf *) wh->dwUser;
		mb->pos = 0;
		mb->end = wh->dwBytesRecorded;
		if (st->rh) {
			st->n++;
			st->rh((uint8_t *)wh->lpData, wh->dwBytesRecorded,
			       st->arg);
		}

		waveInUnprepareHeader(st->wavein, wh, sizeof(WAVEHDR));
		st->inuse--;
		break;

	default:
		break;
	}
}


static int read_stream_open(struct ausrc_st *st)
{
	MMRESULT err;
	WAVEFORMATEX  wfmt;
	int i;

	/* Open an audio INPUT stream. */
	st->wavein = NULL;
	st->pos = 0;
	st->rdy = false;
	st->stop = false;

	for (i = 0; i < READ_BUFFERS; i++) {
		memset(&st->bufs[i].wh, 0, sizeof(WAVEHDR));
		st->bufs[i].mb = mbuf_alloc(2 * st->prm.frame_size);
	}

	wfmt.wFormatTag      = WAVE_FORMAT_PCM;
	wfmt.nChannels       = st->prm.ch;
	wfmt.nSamplesPerSec  = st->prm.srate;
	wfmt.wBitsPerSample  = 16;
	wfmt.nBlockAlign     = (st->prm.ch * wfmt.wBitsPerSample) / 8;
	wfmt.nAvgBytesPerSec = wfmt.nSamplesPerSec * wfmt.nBlockAlign;
	wfmt.cbSize          = 0;
	err = waveInOpen(&st->wavein,
			  WAVE_MAPPER,
			  &wfmt,
			  (DWORD_PTR) waveInCallback,
			  (DWORD_PTR) st,
			  CALLBACK_FUNCTION | WAVE_FORMAT_DIRECT);
	if (err != MMSYSERR_NOERROR) {
		DEBUG_WARNING("waveInOpen: failed %d\n", err);
		return EINVAL;
	}
	/* Prepare enough IN buffers to suite at least 50ms of data */
	for (i = 0; i < READ_BUFFERS; i++)
		add_wave_in(st);

	waveInStart(st->wavein);

	return 0;
}


static int write_stream_open(struct auplay_st *st)
{
	MMRESULT err;
	WAVEFORMATEX  wfmt;
	int i;

	/* Open an audio I/O stream. */
	st->waveout = NULL;
	st->pos = 0;
	st->rdy = false;

	for (i = 0; i < WRITE_BUFFERS; i++) {
		memset(&st->bufs[i].wh, 0, sizeof(WAVEHDR));
		st->bufs[i].mb = mbuf_alloc(2 * st->prm.frame_size);
	}

	wfmt.wFormatTag      = WAVE_FORMAT_PCM;
	wfmt.nChannels       = st->prm.ch;
	wfmt.nSamplesPerSec  = st->prm.srate;
	wfmt.wBitsPerSample  = 16;
	wfmt.nBlockAlign     = (st->prm.ch * wfmt.wBitsPerSample) / 8;
	wfmt.nAvgBytesPerSec = wfmt.nSamplesPerSec * wfmt.nBlockAlign;
	wfmt.cbSize          = 0;

	/* Re-open the device since some drivers fail to open
	 * the device properly if previous user did not
	 * shutdown gracefully
	 */
	err = waveOutOpen(&st->waveout,
			  WAVE_MAPPER,
			  &wfmt,
			  (DWORD_PTR) waveOutCallback,
			  (DWORD_PTR) st,
			  CALLBACK_FUNCTION | WAVE_FORMAT_DIRECT);
	if (err != MMSYSERR_NOERROR) {
		DEBUG_WARNING("waveOutOpen: failed %d\n", err);
		return EINVAL;
	}
	waveOutClose(st->waveout);
	err = waveOutOpen(&st->waveout,
			  WAVE_MAPPER,
			  &wfmt,
			  (DWORD_PTR) waveOutCallback,
			  (DWORD_PTR) st,
			  CALLBACK_FUNCTION | WAVE_FORMAT_DIRECT);
	if (err != MMSYSERR_NOERROR) {
		DEBUG_WARNING("waveOutOpen: failed %d\n", err);
		return EINVAL;
	}

	return 0;
}


static void read_stream_close(struct ausrc_st *st)
{
	int i;

	/* Mark the input as stopped, so we wont be
	 * working on the buffers in the callback
	 */
	st->stop = true;

	waveInStop(st->wavein);
	waveInReset(st->wavein);
	waveInClose(st->wavein);

	for (i = 0; i < READ_BUFFERS; i++) {
		waveInUnprepareHeader(st->wavein, &st->bufs[i].wh,
				      sizeof(WAVEHDR));
		st->bufs[i].mb = mem_deref(st->bufs[i].mb);
	}
}


static void write_stream_close(struct auplay_st *st)
{
	int i;

	/* Mark the device for closing, and wait for all the
	 * buffers to be returned by the driver
	 */
	st->rdy = false;
	while (st->inuse > 0)
		Sleep(50);

	waveOutClose(st->waveout);

	for (i = 0; i < WRITE_BUFFERS; i++) {
		waveOutUnprepareHeader(st->waveout, &st->bufs[i].wh,
				       sizeof(WAVEHDR));
		st->bufs[i].mb = mem_deref(st->bufs[i].mb);
	}
}


static void ausrc_destructor(void *arg)
{
	struct ausrc_st *st = arg;

	read_stream_close(st);
	mem_deref(st->as);
}


static void auplay_destructor(void *arg)
{
	struct auplay_st *st = arg;

	write_stream_close(st);
	mem_deref(st->ap);
}


static int src_alloc(struct ausrc_st **stp, struct ausrc *as,
		     struct media_ctx **ctx,
		     struct ausrc_prm *prm, const char *device,
		     ausrc_read_h *rh, ausrc_error_h *errh, void *arg)
{
	struct ausrc_st *st;
	int err;

	(void)ctx;
	(void)device;
	(void)errh;

	if (src_dev_count < 1) {
		DEBUG_WARNING("no winwave play devices\n");
		return ENODEV;
	}

	st = mem_zalloc(sizeof(*st), ausrc_destructor);
	if (!st)
		return ENOMEM;

	st->as = mem_ref(as);
	st->rh = rh;
	st->arg = arg;
	if (prm) {
		prm->fmt = AUFMT_S16LE;
		st->prm = *prm;
	}

	err = read_stream_open(st);

	if (err)
		mem_deref(st);
	else
		*stp = st;

	return err;
}


static int play_alloc(struct auplay_st **stp, struct auplay *ap,
		      struct auplay_prm *prm, const char *device,
		      auplay_write_h *wh, void *arg)
{
	struct auplay_st *st;
	int err;
	int i;

	(void)device;

	if (play_dev_count < 1) {
		DEBUG_WARNING("no winwave play devices\n");
		return ENODEV;
	}

	st = mem_zalloc(sizeof(*st), auplay_destructor);
	if (!st)
		return ENOMEM;

	st->ap  = mem_ref(ap);
	st->wh  = wh;
	st->arg = arg;
	if (prm) {
		prm->fmt = AUFMT_S16LE;
		st->prm = *prm;
	}

	err = write_stream_open(st);
	if (err)
		goto out;

	/* The write runs at 100ms intervals
	 * prepare enough buffers to suite its needs
	 */
	for (i = 0; i < 5; i++)
		dsp_write(st);

 out:
	if (err)
		mem_deref(st);
	else
		*stp = st;

	return err;
}


static int ww_init(void)
{
	int err;

	play_dev_count = waveOutGetNumDevs();
	DEBUG_INFO("ww_init: %d output devices found\n", play_dev_count);
	src_dev_count = waveInGetNumDevs();
	DEBUG_INFO("ww_init: %d input devices found\n", src_dev_count);

	err  = ausrc_register(&ausrc, "winwave", src_alloc);
	err |= auplay_register(&auplay, "winwave", play_alloc);

	return err;
}


static int ww_close(void)
{
	ausrc = mem_deref(ausrc);
	auplay = mem_deref(auplay);

	return 0;
}


EXPORT_SYM const struct mod_export DECL_EXPORTS(winwave) = {
	"winwave",
	"sound",
	ww_init,
	ww_close
};
