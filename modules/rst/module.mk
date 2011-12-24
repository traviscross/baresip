#
# module.mk
#
# Copyright (C) 2011 Creytiv.com
#

MOD		:= rst
$(MOD)_SRCS	+= audio.c
$(MOD)_SRCS	+= rst.c
$(MOD)_SRCS	+= video.c
$(MOD)_LFLAGS	+= -lmpg123 -lcairo

include mk/mod.mk
