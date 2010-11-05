/**
 * @file stub.cpp  System functions stub
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <e32def.h>
#include <unistd.h>
#include <pwd.h>


char *getlogin(void)
{
	return NULL;
}


struct passwd *getpwnam(const char *)
{
	return NULL;
}
