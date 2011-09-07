#
# module.mk
#
# Copyright (C) 2010 Creytiv.com
#

MOD		:= opus
$(MOD)_SRCS	+= opus.c
$(MOD)_LFLAGS	+= -lopus -lm

include mk/mod.mk
