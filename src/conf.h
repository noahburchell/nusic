#pragma once

// what the config sets, conf_load fills it before anything reads it
struct conf {
	double knee;
	bool   soft_clip;
	bool   semitones;
	bool   always_tabbed;
	bool   sort_ignore_articles;
};

extern struct conf conf;

// reads $XDG_CONFIG_HOME/nusic/config, a config that does not exist is written with every key at its default
// false when the config cannot be read or has a line that cannot be used, which is reported
bool conf_load(void);
