#
# module.mk
#
# Copyright (C) 2010 Creytiv.com
#

MOD		:= amr
$(MOD)_SRCS	+= amr.c
$(MOD)_LFLAGS	+= -lamrwb -lamrnb -lm

include mk/mod.mk
