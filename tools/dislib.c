#include <string.h>
#include "m68k.h"
static const unsigned char *g_rom; static unsigned g_size;
void dis_set_rom(const unsigned char *r, unsigned n){g_rom=r;g_size=n;}
unsigned m68k_read_disassembler_16(unsigned a){a&=0xffffff; if(a+1>=g_size) return 0; return (g_rom[a]<<8)|g_rom[a+1];}
unsigned m68k_read_disassembler_32(unsigned a){return (m68k_read_disassembler_16(a)<<16)|m68k_read_disassembler_16(a+2);}
unsigned m68k_read_disassembler_8(unsigned a){a&=0xffffff; return a<g_size?g_rom[a]:0;}
static unsigned g_type = M68K_CPU_TYPE_68000;
void dis_set_cpu(int c){ g_type = c ? M68K_CPU_TYPE_68EC020 : M68K_CPU_TYPE_68000; }
unsigned dis(unsigned pc, char *buf){ return m68k_disassemble(buf, pc, g_type); }
int dis_valid(unsigned op){ return m68k_is_valid_instruction(op, g_type); }
