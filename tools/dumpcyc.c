#include <stdio.h>
#include "m68k.h"
#include "m68kops.h"
unsigned int m68k_read_memory_8(unsigned int a){return 0;} unsigned int m68k_read_memory_16(unsigned int a){return 0;} unsigned int m68k_read_memory_32(unsigned int a){return 0;}
void m68k_write_memory_8(unsigned int a, unsigned int v){} void m68k_write_memory_16(unsigned int a, unsigned int v){} void m68k_write_memory_32(unsigned int a, unsigned int v){}
unsigned int m68k_read_disassembler_8(unsigned int a){return 0;} unsigned int m68k_read_disassembler_16(unsigned int a){return 0;} unsigned int m68k_read_disassembler_32(unsigned int a){return 0;}
int main(){ m68k_init(); FILE*f=fopen("cycles68k.bin","wb"); fwrite(m68ki_cycles[0],1,0x10000,f); fclose(f); f=fopen("cycles020.bin","wb"); fwrite(m68ki_cycles[2],1,0x10000,f); fclose(f); return 0;}
