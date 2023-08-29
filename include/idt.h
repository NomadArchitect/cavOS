#ifndef IDT_H
#define IDT_H

#include "types.h"

/* Functions implemented in idt.c */
void set_idt_gate(int n, uint32 handler);
void set_idt();

#endif
