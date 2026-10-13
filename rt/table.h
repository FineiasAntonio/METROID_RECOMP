#pragma once
#include "cpu.h"
typedef struct { uint64_t addr; void (*fn)(CPU *); } FuncEntry;
