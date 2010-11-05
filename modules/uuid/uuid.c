/**
 * @file modules/uuid/uuid.c  Generate UUID
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <stdlib.h>
#include <string.h>
#include <uuid/uuid.h>
#include <re.h>
#include <baresip.h>


#define DEBUG_MODULE "mod_uuid"
#define DEBUG_LEVEL 5
#include <re_dbg.h>


static int uuid_init(void)
{
	char file[256], path[256], uuid[37];
	uuid_t uu;
	FILE *f = NULL;
	int err;

	err = conf_path_get(path, sizeof(path));
	if (err)
		return err;

	if (re_snprintf(file, sizeof(file), "%s/uuid", path) < 0)
		return ENOMEM;

	f = fopen(file, "r");
	if (f) {
		err = 0;
		goto out;
	}

	f = fopen(file, "w");
	if (!f) {
		err = errno;
		DEBUG_WARNING("init: fopen() %s (%s)\n", file, strerror(err));
		goto out;
	}

	uuid_generate(uu);

	uuid_unparse(uu, uuid);

	re_fprintf(f, "%s", uuid);

	DEBUG_NOTICE("init: generated new UUID (%s)\n", uuid);

 out:
	if (f)
		fclose(f);

	return err;
}


EXPORT_SYM const struct mod_export DECL_EXPORTS(uuid) = {
	"uuid",
	NULL,
	uuid_init,
	NULL
};
