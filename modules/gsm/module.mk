#
# module.mk
#
# Copyright (C) 2010 Creytiv.com
#

MOD		:= gsm
$(MOD)_SRCS	+= gsm.c
$(MOD)_LFLAGS	+= -L/usr/local/lib -lgsm
CFLAGS		+= -I/usr/local/include/gsm -I/usr/include/gsm

include mk/mod.mk
