#
# module.mk
#
# Copyright (C) 2010 Creytiv.com
#

MOD		:= g7221
$(MOD)_SRCS	+= g7221.c
$(MOD)_LFLAGS	+= -lg722_1

include mk/mod.mk
