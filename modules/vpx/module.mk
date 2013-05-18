#
# module.mk
#
# Copyright (C) 2010 Creytiv.com
#

MOD		:= vpx
$(MOD)_SRCS	+= vpx.c sdp.c decode.c encode.c
$(MOD)_LFLAGS	+= -lvpx

include mk/mod.mk
