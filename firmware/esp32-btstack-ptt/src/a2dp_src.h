#ifndef A2DP_SRC_H
#define A2DP_SRC_H

#include <stdbool.h>
#include "btstack.h"

// A2DP source (SBC, MOD-player test audio) alongside the HFP AG.
void a2dp_src_init(void);
void a2dp_src_connect(bd_addr_t addr);
bool a2dp_src_streaming(void);

#endif
