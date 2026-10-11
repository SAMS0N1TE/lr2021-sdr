#pragma once
#include "lr2021_iq.h"
int radio_init(void);
int radio_configure(uint32_t hz);
int radio_info(void);
const lr2021_iq_hal *radio_hal(void);
