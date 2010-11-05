#
# module.mk
#
# Copyright (C) 2010 Creytiv.com
#

MOD		:= avformat
$(MOD)_SRCS	+= avf.c
$(MOD)_LFLAGS	+= -lavdevice -lavformat -lavcodec -lswscale

include mk/mod.mk
