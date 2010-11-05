#
# module.mk
#
# Copyright (C) 2010 Creytiv.com
#

MOD		:= celt
$(MOD)_SRCS	+= celt.c
$(MOD)_LFLAGS	+= -lcelt0

include mk/mod.mk
