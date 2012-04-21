#
# srcs.mk All application source files.
#
# Copyright (C) 2010 Creytiv.com
#

SRCS	+= aucodec.c
SRCS	+= audio.c
SRCS	+= aufilt.c
SRCS	+= auloop.c
SRCS	+= auplay.c
SRCS	+= ausrc.c
SRCS	+= calc.c
SRCS	+= call.c
SRCS	+= conf.c
SRCS	+= contact.c
SRCS	+= mctrl.c
SRCS	+= menc.c
SRCS	+= mnat.c
SRCS	+= net.c
SRCS	+= os.c
SRCS	+= play.c
SRCS	+= realtime.c
SRCS	+= rtpkeep.c
SRCS	+= stream.c
SRCS	+= sipreq.c
SRCS	+= ua.c
SRCS	+= ui.c
SRCS	+= uuid.c
SRCS	+= vidcodec.c
SRCS	+= vidisp.c
SRCS	+= vidsrc.c

ifneq ($(USE_VIDEO),)
SRCS	+= video.c
SRCS	+= vidloop.c
endif

ifneq ($(STATIC),)
SRCS	+= static.c
endif

APP_SRCS += main.c
