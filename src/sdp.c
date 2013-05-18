/**
 * @file sdp.c  SDP functions
 *
 * Copyright (C) 2011 Creytiv.com
 */
#include <stdlib.h>
#include <re.h>
#include <baresip.h>
#include "core.h"


uint32_t sdp_media_rattr_u32(const struct sdp_media *m, const char *name)
{
	const char *attr = sdp_media_rattr(m, name);
	return attr ? atoi(attr) : 0;
}


/*
 * Get a remote attribute from the SDP. Try the media-level first,
 * and if it does not exist then try session-level.
 */
const char *sdp_rattr(const struct sdp_session *s, const struct sdp_media *m,
		      const char *name)
{
	const char *x;

	x = sdp_media_rattr(m, name);
	if (x)
		return x;

	x = sdp_session_rattr(s, name);
	if (x)
		return x;

	return NULL;
}


/* RFC 4572 */
int sdp_fingerprint_decode(const char *attr, const char *hash,
			   uint8_t *md, size_t *sz)
{
	struct pl h, f;
	const char *p;
	int err;

	if (!attr || !md)
		return EINVAL;

	err = re_regex(attr, str_len(attr), "[^ ]+ [0-9A-F:]+", &h, &f);
	if (err)
		return err;

	if (0 != pl_strcasecmp(&h, hash))
		return EBADMSG;

	if (*sz < (f.l+1)/3)
		return EOVERFLOW;

	for (p = f.p; p < (f.p+f.l); p += 3) {
		*md++ = ch_hex(p[0]) << 4 | ch_hex(p[1]);
	}

	*sz = (f.l+1)/3;

	return 0;
}


bool sdp_media_has_media(const struct sdp_media *m)
{
	bool has;

	has = sdp_media_rformat(m, NULL) != NULL;
	if (has)
		return sdp_media_rport(m) != 0;

	return false;
}
