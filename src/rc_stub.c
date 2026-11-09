#include "core.h"
void rc_run(void) { cpu_interp_run(); }
int rc_has(uint32_t pc) { (void)pc; return 0; }
