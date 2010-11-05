/**
 * @file menc.c  Media encryption
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <re.h>
#include <baresip.h>
#include "core.h"


/* Base type */
struct menc_st {
	struct menc *me;
};

static struct list mencl = LIST_INIT;


static void destructor(void *arg)
{
	struct menc *menc = arg;

	list_unlink(&menc->le);
}


int menc_register(struct menc **mencp, const char *id, menc_alloc_h *alloch,
		  menc_update_h *updateh)
{
	struct menc *menc;

	if (!mencp || !id || !alloch)
		return EINVAL;

	menc = mem_zalloc(sizeof(*menc), destructor);
	if (!menc)
		return ENOMEM;

	list_append(&mencl, &menc->le, menc);

	menc->id      = id;
	menc->alloch  = alloch;
	menc->updateh = updateh;

	(void)re_printf("mediaenc: %s\n", id);

	*mencp = menc;

	return 0;
}


struct menc *menc_get(struct menc_st *st)
{
	return st ? st->me : NULL;
}


const struct menc *menc_find(const char *id)
{
	struct le *le;

	for (le = mencl.head; le; le = le->next) {
		struct menc *me = le->data;

		if (0 == str_casecmp(me->id, id))
			return me;
	}

	return NULL;
}


int menc_alloc(struct menc_st **mep, const char *id, int proto,
	       void *rtpsock, void *rtcpsock, struct sdp_media *sdpm)
{
	struct menc *me = (struct menc *)menc_find(id);
	if (!me)
		return ENOENT;

	if (me->alloch)
		return me->alloch(mep, me, proto, rtpsock, rtcpsock, sdpm);

	return 0;
}


/**
 * Convert Media encryption type to SDP Transport
 *
 *   Freeswitch requires RTP/SAVP
 *   pjsip requires RTP/AVP
 */
const char *menc2transp(const char *type)
{
	if (0 == str_casecmp(type, "srtp-mand"))
		return sdp_proto_rtpsavp;
	else
		return sdp_proto_rtpavp;
}
