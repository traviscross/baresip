/**
 * @file os.c  OS support
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_PWD_H
#include <pwd.h>
#endif
#ifdef WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <direct.h>
#include <lmaccess.h>
#undef WIN32_LEAN_AND_MEAN
#endif
#include <re.h>
#include <baresip.h>


int mkpath(const char *path)
{
	int ret;
#if defined (WIN32)
	ret = _mkdir(path);
#else
	ret = mkdir(path, 0755);
#endif
	if (ret < 0) {
		if (errno == EEXIST)
			return 0;
		else
			return errno;
	}

	return 0;
}


int get_login_name(const char **login)
{
#ifdef HAVE_PWD_H
	struct passwd *pw;

	*login = getenv("LOGNAME");
	if (!*login) {
#ifdef HAVE_UNISTD_H
		*login = getlogin();
#endif
		if (!*login)
			return errno;
	}

	pw = getpwnam(*login);
	if (!pw)
		return errno;

	return 0;
#else
	(void)login;
	return ENOSYS;
#endif
}


#ifdef WIN32
static int get_home_win32(char *path, uint32_t sz)
{
	char win32_path[MAX_PATH];

	if (S_OK != SHGetFolderPath(NULL,
				    CSIDL_APPDATA | CSIDL_FLAG_CREATE,
				    NULL,
				    0,
				    win32_path)) {
		return ENOENT;
	}

	if (re_snprintf(path, sz, "%s\\baresip", win32_path) < 0)
		return ENOMEM;

	return 0;
}
#elif defined(__SYMBIAN32__)
static int get_home_symbian(char *path, uint32_t sz)
{
	return re_snprintf(path, sz, "c:\\Data\\baresip") < 0 ? ENOMEM : 0;
}
#else
static int get_home_unix(char *path, uint32_t sz)
{
#ifdef HAVE_PWD_H
	struct passwd *pw;
	char *loginname = NULL;

	loginname = getenv("LOGNAME");
	if (!loginname) {
#ifdef HAVE_UNISTD_H
		loginname = getlogin();
#endif
		if (!loginname)
			return errno;
	}

	pw = getpwnam(loginname);
	if (!pw)
		return errno;

	if (re_snprintf(path, sz, "%s/.baresip", pw->pw_dir) < 0)
		return ENOMEM;

	return 0;
#else
	(void)path;
	(void)sz;
	return ENOSYS;
#endif
}
#endif


int get_homedir(char *path, uint32_t sz)
{
#ifdef WIN32
	return get_home_win32(path, sz);
#elif defined(__SYMBIAN32__)
	return get_home_symbian(path, sz);
#else
	return get_home_unix(path, sz);
#endif
}
