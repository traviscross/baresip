#
# module.mk
#
# Copyright (C) 2010 Creytiv.com
#

MOD		:= qtcapture
$(MOD)_SRCS	+= qtcapture.m
$(MOD)_LFLAGS	+= -framework Cocoa -framework QTKit -framework CoreVideo \
			-lavcodec -lswscale

include mk/mod.mk
