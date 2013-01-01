/**
 * @file vidfilt.c Video Filter
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <re.h>
#include <baresip.h>
#include "core.h"


static struct list vfl;


/**
 * Register a new Video Filter
 *
 * @param vf Video Filter to register
 */
void vidfilt_register(struct vidfilt *vf)
{
	if (!vf)
		return;

	list_append(&vfl, &vf->le, vf);

	(void)re_printf("vidfilt: %s\n", vf->name);
}


/**
 * Unregister a Video Filter
 *
 * @param vf Video Filter to unregister
 */
void vidfilt_unregister(struct vidfilt *vf)
{
	if (!vf)
		return;

	list_unlink(&vf->le);
}


/**
 * Get the list of registered Video Filters
 *
 * @return List of Video Filters
 */
struct list *vidfilt_list(void)
{
	return &vfl;
}
