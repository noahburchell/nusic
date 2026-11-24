#pragma once

#include <stddef.h>

// DIR/nusic/name, DIR is the environment variable env when it is an absolute path, otherwise $HOME/fallback
// false when the environment names no such place, which is reported with what as the thing without a place
bool nusic_path(char* buf, size_t n, const char* env, const char* fallback, const char* name, const char* what);
// creates the directories above path, on a failure errno is set and path ends after the directory that failed
bool make_parents(char* path);
