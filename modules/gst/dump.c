/**
 * @file dump.c  Gstreamer playbin pipeline - dump utilities
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <re.h>
#include <gst/gst.h>
#include "gst.h"


void gst_dump_props(GstElement *g)
{
	uint64_t u64;
	gchar *strval;
	double volume;
	int n;

	re_printf("Gst properties:\n");

	g_object_get(g, "delay", &u64, NULL);
	re_printf(" delay:           %lu ns\n", u64);

	g_object_get(g, "uri", &strval, NULL);
	re_printf(" uri:             %s\n", strval);
	g_free(strval);

	g_object_get(g, "suburi", &strval, NULL);
	re_printf(" suburi:          %s\n", strval);
	g_free(strval);

	g_object_get(g, "queue-size", &u64, NULL);
	re_printf(" queue-size:      %lu ns\n", u64);

	g_object_get(g, "queue-threshold", &u64, NULL);
	re_printf(" queue-threshold: %lu ns\n", u64);

	g_object_get(g, "nstreams", &n, NULL);
	re_printf(" nstreams:        %d\n", n);

	g_object_get(g, "volume", &volume, NULL);
	re_printf(" Volume:          %f\n", volume);
}


void gst_dump_caps(const GstCaps *caps)
{
	GstStructure *s;
	int rate, channels, width;

	if (!caps)
		return;

	if (!gst_caps_get_size(caps))
		return;

	s = gst_caps_get_structure(caps, 0);

	gst_structure_get_int(s, "rate",     &rate);
	gst_structure_get_int(s, "channels", &channels);
	gst_structure_get_int(s, "width",    &width);

	re_printf("gst caps dump: %d Hz, %d channels, width=%d\n",
		  rate, channels, width);
}
