#include "conf.h"

#include "path.h"
#include "report.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CONF_MAX 8192

// the config that the first run writes, the default of every key is read from it
static const char defaults[] = "soft-clip=false\n"
                               "soft-clip-knee=0.8\n"
                               "rate-default-unit=st\n"
                               "always-tabbed=false\n"
                               "sort-ignore-articles=false\n";

struct conf conf;

static bool blank(char c) {
	return c == ' ' || c == '\t' || c == '\r';
}

static char* trim(char* s) {
	char* end;

	while(blank(*s)) {
		s++;
	}

	end = s + strlen(s);
	while(end > s && blank(end[-1])) {
		end--;
	}

	*end = 0;

	return s;
}

static bool flag(const char* v, bool* out) {
	bool on = !strcmp(v, "true");

	if(!on && strcmp(v, "false")) {
		return false;
	}

	*out = on;

	return true;
}

static bool unit(const char* v, bool* semitones) {
	bool st = !strcmp(v, "st");

	if(!st && strcmp(v, "x")) {
		return false;
	}

	*semitones = st;

	return true;
}

// digits around at most one point, from 0 to 1, strtod would take the decimal comma of a locale instead
static bool share(const char* v, double* out) {
	double n = 0.0;
	double div = 1.0;
	bool digits = false;
	bool point = false;

	for(; *v; v++) {
		if(*v == '.' && !point) {
			point = true;
			continue;
		}

		if(*v < '0' || *v > '9') {
			return false;
		}

		n = n * 10.0 + (double)(*v - '0');
		div *= point ? 10.0 : 1.0;
		digits = true;
	}

	// too many digits make both infinite, their quotient then fails the comparison
	if(!digits || !(n / div <= 1.0)) {
		return false;
	}

	*out = n / div;

	return true;
}

static bool set(const char* path, unsigned line, const char* key, const char* v) {
	const char* expected;
	bool ok;

	if(!strcmp(key, "soft-clip")) {
		ok = flag(v, &conf.soft_clip);
		expected = "true or false";
	} else if(!strcmp(key, "soft-clip-knee")) {
		ok = share(v, &conf.knee);
		expected = "0 to 1";
	} else if(!strcmp(key, "rate-default-unit")) {
		ok = unit(v, &conf.semitones);
		expected = "x or st";
	} else if(!strcmp(key, "always-tabbed")) {
		ok = flag(v, &conf.always_tabbed);
		expected = "true or false";
	} else if(!strcmp(key, "sort-ignore-articles")) {
		ok = flag(v, &conf.sort_ignore_articles);
		expected = "true or false";
	} else {
		report("%s:%u: unknown key '%s'", path, line, key);
		return false;
	}

	if(!ok) {
		report("%s:%u: %s: invalid value '%s', expected %s", path, line, key, v, expected);
	}

	return ok;
}

// text ends with a 0 byte and is cut up in place, every line that cannot be used is reported
static bool parse(const char* path, char* text) {
	unsigned line = 0;
	bool ok = true;
	char* next;

	for(; *text; text = next) {
		char* end = strchr(text, '\n');
		char* key;
		char* eq;

		line++;
		next = end ? end + 1 : text + strlen(text);
		if(end) {
			*end = 0;
		}

		key = trim(text);
		if(!*key || *key == '#') {
			continue;
		}

		eq = strchr(key, '=');
		if(!eq) {
			report("%s:%u: expected KEY=VALUE", path, line);
			ok = false;
			continue;
		}

		*eq = 0;
		if(!set(path, line, trim(key), trim(eq + 1))) {
			ok = false;
		}
	}

	return ok;
}

// nusic runs on the defaults either way, so a failure is a warning
static void write_defaults(char* path) {
	int fd;

	if(!make_parents(path)) {
		report("warning: mkdir %s: %s, the config is not written", path, strerror(errno));
		return;
	}

	fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
	if(fd < 0) {
		// another nusic wrote it in the meantime
		if(errno != EEXIST) {
			report("warning: open %s: %s, the config is not written", path, strerror(errno));
		}

		return;
	}

	if(write(fd, defaults, sizeof(defaults) - 1) < 0) {
		report("warning: write %s: %s, the config is not written", path, strerror(errno));
		unlink(path);
	}

	close(fd);
}

bool conf_load(void) {
	char path[PATH_MAX];
	char text[CONF_MAX + 1];
	struct stat st;
	size_t len = 0;
	ssize_t r = 0;
	int fd;

	memcpy(text, defaults, sizeof(defaults));
	parse("defaults", text);
	if(!nusic_path(path, sizeof(path), "XDG_CONFIG_HOME", ".config", "config", "the config")) {
		return false;
	}

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if(fd < 0 && errno == ENOENT) {
		write_defaults(path);
		return true;
	}

	if(fd < 0) {
		report("open %s: %s", path, strerror(errno));
		return false;
	}

	if(fstat(fd, &st) == 0 && st.st_size > CONF_MAX) {
		report("%s: is %lld bytes, the maximum is %d", path, (long long)st.st_size, CONF_MAX);
		close(fd);
		return false;
	}

	while(len < CONF_MAX && (r = read(fd, text + len, CONF_MAX - len)) > 0) {
		len += (size_t)r;
	}

	if(r < 0) {
		report("read %s: %s", path, strerror(errno));
		close(fd);
		return false;
	}

	close(fd);
	text[len] = 0;

	return parse(path, text);
}
