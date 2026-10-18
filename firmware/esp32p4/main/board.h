#pragma once
#include "board_profile.h"

int board_load(void);
const board_profile *board_active(void);
int board_command(int argc, char **argv);
