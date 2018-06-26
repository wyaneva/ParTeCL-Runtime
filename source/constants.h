#ifndef CONSTANTS_H
#define CONSTANTS_H

/**********************************
 * Used in kernel for optimisations
 **********************************/

/*
 * FSM_OPTIMISE toggles optimisations
 *  1. coalesced memory allocation
 *  2. constant memory storage
 */
#ifndef FSM_OPTIMISE
#define FSM_OPTIMISE 1
#endif

#if FSM_OPTIMISE
#define TEST_INPUTS_ATTR global
#else
#define TEST_INPUTS_ATTR
#endif

/*
 * FSM_CONSTANT_MEMORY and FSM_LOCAL_MEMORY 
 * put the FSM in the corresponding memory spaces
 * when there is enough space
 */
#ifndef FSM_CONSTANT_MEMORY
#define FSM_CONSTANT_MEMORY 0
#endif 

#ifndef FSM_LOCAL_MEMORY
#define FSM_LOCAL_MEMORY 0
#endif

#if FSM_CONSTANT_MEMORY
#define FSM_ATTR constant
#define FSM_ATTR_KNL constant
#else 

#if FSM_LOCAL_MEMORY
#define FSM_ATTR local
#define FSM_ATTR_KNL global
#else
#define FSM_ATTR global
#define FSM_ATTR_KNL global
#endif

#endif

#endif
