#pragma once

#include "library.h"
#include "player.h"

// the mpris interface on the session bus, by which other programs read what plays and control it
// all of it runs on the thread that drives the player
// false when there is no session bus or it does not answer, the latter is reported
bool mpris_init(const struct library* lib);
void mpris_fini(void);
// the descriptor that turns readable when a call has arrived, -1 without a bus
int mpris_fd(void);
// answers the calls that have arrived, true once one of them asked nusic to quit
bool mpris_dispatch(void);
// the status as it is now, the bus is told what changed
void mpris_update(const struct play_status* s);
