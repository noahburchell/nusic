#pragma once

#include "library.h"

// runs the interface until the user quits, notify is the eventfd the player writes
void ui_run(const struct library* lib, int notify);
