#include "path.h"

#include "report.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

bool nusic_path(char* buf, size_t n, const char* env, const char* fallback, const char* name, const char* what) {
	const char* base = getenv(env);
	const char* home = getenv("HOME");
	int len;

	// the base directory specification has a relative directory ignored
	if(base && base[0] == '/') {
		len = snprintf(buf, n, "%s/nusic/%s", base, name);
	} else if(home && home[0] == '/') {
		len = snprintf(buf, n, "%s/%s/nusic/%s", home, fallback, name);
	} else {
		report("neither %s nor HOME is an absolute path, %s has no place", env, what);
		return false;
	}

	if((size_t)len >= n) {
		report("%s: path is too long", base && base[0] == '/' ? base : home);
		return false;
	}

	return true;
}

// the base directory specification asks for mode 0700 on a directory that is created
bool make_parents(char* path) {
	char* p;

	for(p = strchr(path + 1, '/'); p; p = strchr(p + 1, '/')) {
		*p = 0;
		if(mkdir(path, 0700) < 0 && errno != EEXIST) {
			return false;
		}

		*p = '/';
	}

	return true;
}
