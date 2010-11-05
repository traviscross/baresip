#
# module.mk
#
# Copyright (C) 2010 Creytiv.com
#

MOD		:= winwave
$(MOD)_SRCS	+= winwave.c
$(MOD)_LFLAGS	+= -lwinmm

include mk/mod.mk
