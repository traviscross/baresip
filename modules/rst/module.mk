#
# module.mk
#
# Copyright (C) 2011 Creytiv.com
#

MOD		:= rst
$(MOD)_SRCS	+= rst.c
$(MOD)_LFLAGS	+= -lmpg123

include mk/mod.mk
