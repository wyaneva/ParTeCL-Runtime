#ifndef CPU_GEN_H
#define CPU_GEN_H
#include "structs.h"

void populate_inputs(struct partecl_input*, int, char**, int, char**);

void compare_results(struct partecl_result*, struct partecl_result*, int);

#endif