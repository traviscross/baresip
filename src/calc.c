/**
 * @file calc.c  Calculation routines
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <stdlib.h>
#include <re.h>
#include <baresip.h>


/**
 * Calculate number of samples from sample rate, channels and packet time
 *
 * @param srate    Sample rate in [Hz]
 * @param channels Number of channels
 * @param ptime    Packet time in [ms]
 *
 * @return Number of samples
 */
uint32_t calc_nsamp(uint32_t srate, int channels, uint16_t ptime)
{
	return srate * channels * ptime / 1000;
}


/**
 * Calculate packet time from sample rate, channels and number of samples
 *
 * @param srate    Sample rate in [Hz]
 * @param channels Number of channels
 * @param nsamp    Number of samples
 *
 * @return Packet time in [ms]
 *
 * <pre>
 * ptime = nsamp * 1000 / (srate * ch)
 * </pre>
 */
uint32_t calc_ptime(uint32_t srate, int channels, uint32_t nsamp)
{
	return (nsamp * 1000) / (srate * channels);
}


/**
 * Calculate the average value of a buffer of 16-bit samples
 *
 * @param mb PCM buffer
 *
 * @return Average sample value
 */
int16_t calc_avg_s16(struct mbuf *mb)
{
	size_t pos = mb->pos;
	int16_t v = 0;

	while (mbuf_get_left(mb) >= 2) {
		const int16_t s = mbuf_read_u16(mb);
		v = avg(v, abs(s));
	}

	mbuf_set_pos(mb, pos);

	return v;
}
