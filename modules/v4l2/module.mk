#
# module.mk
#
# Copyright (C) 2010 Creytiv.com
#

MOD		:= v4l2
$(MOD)_SRCS	+= v4l2.c
$(MOD)_LFLAGS	+= -lv4l2 -lavformat -lswscale

include mk/mod.mk
