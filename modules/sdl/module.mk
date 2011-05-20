#
# module.mk
#
# Copyright (C) 2010 Creytiv.com
#

MOD		:= sdl
SDL_VERSION	:= $(shell sdl-config --version | \
	sed -e 's/\([0-9]*\).\([0-9]*\).\([0-9]*\)/\1.\2/')
ifeq ($(SDL_VERSION),1.2)
$(MOD)_SRCS	+= sdl-1.2.c
else
$(MOD)_SRCS	+= sdl.c
endif
$(MOD)_SRCS	+= util.c

CFLAGS		+= -DUSE_SDL
$(MOD)_LFLAGS	+= -lSDL
ifeq ($(OS),darwin)
# note: APP_LFLAGS is needed, as main.o links to -lSDLmain
APP_LFLAGS	+= -lSDL -lSDLmain -lobjc \
	-framework CoreFoundation -framework Foundation -framework Cocoa
endif

include mk/mod.mk
