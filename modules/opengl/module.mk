#
# module.mk
#
# Copyright (C) 2010 Creytiv.com
#

MOD		:= opengl
$(MOD)_SRCS	+= opengl.m
$(MOD)_LFLAGS	+= -framework QTKit -framework CoreVideo -lavcodec -lswscale

include mk/mod.mk
