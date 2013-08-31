/**
 * @file modules/uuid/uuid.c  Generate and load UUID
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


static int uuid_init(const char *file)
{
	char uuid[37];
	uuid_t uu;
	FILE *f = NULL;
	int err = 0;

	f = fopen(file, "r");
	if (f) {
		err = 0;
		goto out;
	}

	f = fopen(file, "w");
	if (!f) {
		err = errno;
		DEBUG_WARNING("init: fopen() %s (%m)\n", file, err);
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


static int uuid_load(const char *file, char *uuid, size_t sz)
{
	FILE *f = NULL;
	int err = 0;

	f = fopen(file, "r");
	if (!f)
		return errno;

	if (!fgets(uuid, (int)sz, f))
		err = errno;

	(void)fclose(f);

	return err;
}


static int module_init(void)
{
	struct config *cfg = conf_config();
	char path[256];
	int err = 0;

	err = conf_path_get(path, sizeof(path));
	if (err)
		return err;

	strncat(path, "/uuid", sizeof(path));

	err = uuid_init(path);
	if (err)
		return err;

	err = uuid_load(path, cfg->sip.uuid, sizeof(cfg->sip.uuid));
	if (err)
		return err;

	return 0;
}


EXPORT_SYM const struct mod_export DECL_EXPORTS(uuid) = {
	"uuid",
	NULL,
	module_init,
	NULL
};
