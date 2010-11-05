/**
 * @file src/uuid.c  Load UUID
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <string.h>
#include <re.h>
#include <baresip.h>
#include "core.h"


#define DEBUG_MODULE "uuid"
#define DEBUG_LEVEL 5
#include <re_dbg.h>


/**
 * Load the UUID from persistent storage
 *
 * @param uuid String buffer to store UUID
 * @param sz   Size of buffer
 *
 * @return 0 if success, otherwise errorcode
 */
int uuid_load(char *uuid, uint32_t sz)
{
	char path[256], file[256];
	FILE *f = NULL;
	int err;

	err = conf_path_get(path, sizeof(path));
	if (err)
		return err;

	if (re_snprintf(file, sizeof(file), "%s/uuid", path) < 0)
		return ENOMEM;

	f = fopen(file, "r");
	if (!f) {
		struct mod *m;

		err = conf_load_module(&m, "uuid");
		if (err) {
			DEBUG_NOTICE("uuid_load: %s\n", strerror(err));
			goto out;
		}
		mem_deref(m);

		f = fopen(file, "r");
		if (!f) {
			err = errno;
			goto out;
		}
	}

	if (!fgets(uuid, sz, f)) {
		err = errno;
		goto out;
	}

 out:
	if (f)
		(void)fclose(f);
	return err;
}
