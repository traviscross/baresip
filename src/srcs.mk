#
# srcs.mk All application source files.
#
# Copyright (C) 2010 Creytiv.com
#

SRCS	+= aucodec.c
SRCS	+= audio.c
SRCS	+= aufilt.c
SRCS	+= auplay.c
SRCS	+= ausrc.c
SRCS	+= bfcp.c
SRCS	+= call.c
SRCS	+= cmd.c
SRCS	+= conf.c
SRCS	+= contact.c
SRCS	+= mctrl.c
SRCS	+= menc.c
SRCS	+= mnat.c
SRCS	+= module.c
SRCS	+= net.c
SRCS	+= play.c
SRCS	+= realtime.c
SRCS	+= reg.c
SRCS	+= rtpkeep.c
SRCS	+= sdp.c
SRCS	+= sipreq.c
SRCS	+= stream.c
SRCS	+= ua.c
SRCS	+= ui.c
SRCS	+= vidcodec.c
SRCS	+= vidfilt.c
SRCS	+= vidisp.c
SRCS	+= vidsrc.c

ifneq ($(USE_VIDEO),)
SRCS	+= video.c
endif

ifneq ($(STATIC),)
SRCS	+= static.c
endif

APP_SRCS += main.c
