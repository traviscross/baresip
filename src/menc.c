/**
 * @file menc.c  Media encryption
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <re.h>
#include <baresip.h>
#include "core.h"


/** Media Encryption state */
struct menc_st {
	struct menc *me;  /**< Media encryption */
};

static struct list mencl = LIST_INIT;


static void destructor(void *arg)
{
	struct menc *menc = arg;

	list_unlink(&menc->le);
}


/**
 * Register a new Media encryption module
 *
 * @param mencp    Pointer to allocated Media encryption module
 * @param id       Media encryption ID
 * @param alloch   Allocation handler
 * @param updateh  Update handler
 *
 * @return 0 if success, otherwise errorcode
 */
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


/**
 * Get the Media Encryption module from a Media Encryption state
 *
 * @param st Media Encryption state
 *
 * @return Media Encryption module
 */
struct menc *menc_get(const struct menc_st *st)
{
	return st ? st->me : NULL;
}


/**
 * Find a Media Encryption module by name
 *
 * @param id Name of the Media Encryption module to find
 *
 * @return Matching Media Encryption module if found, otherwise NULL
 */
const struct menc *menc_find(const char *id)
{
	struct le *le;

	for (le = mencl.head; le; le = le->next) {
		struct menc *me = le->data;

		if (0 == str_casecmp(id, me->id))
			return me;
	}

	return NULL;
}


/**
 * Convert Media encryption type to SDP Transport
 */
const char *menc2transp(const struct menc *menc)
{
	if (!menc)
		return sdp_proto_rtpavp;

	if (0 == str_casecmp(menc->id, "srtp-mand"))
		return sdp_proto_rtpsavp;
	else
		return sdp_proto_rtpavp;
}
