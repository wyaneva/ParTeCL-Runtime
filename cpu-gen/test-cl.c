#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../source/constants.h"
#include "../kernel-gen/fsm.h"
#include "../kernel-gen/structs.h"

/**
 * Looksup an FSM input symbol, given the symbol and the current state.
 * Returns the next state or -1 if transition isn't found.
 */
short lookup_symbol(transition *transitions, short current_state, char input[],
                    int length, char *output_ptr) {

#if BMRK_NETWORK
  if (input[0] == '\n') {
    return current_state;
  }
#endif

  int idx = get_index(current_state, input[0]);
  transition trans = transitions[idx];

  if (trans.next_state == -1) {
    printf("\nCouldn't find transition for state %d, input %s.\n",
           current_state, input);
  }

  strcpy(output_ptr, trans.output);
  return trans.next_state;
}

/**
 * Executes the FSM.
 * Returns the final state.
 */
void run_main(struct partecl_input input, struct partecl_result *result,
              transition *transitions, short starting_state, int input_length,
              int output_length) {

  char *input_ptr = input.input_ptr;
  char* output_ptr = result->output;

  // output
  short current_state = starting_state; // transitions[0].current_state;
  while (*input_ptr != '\0') {

    if (current_state == -1) {
      return;
    }

    current_state = lookup_symbol(transitions, current_state, input_ptr,
                                  input_length, output_ptr);

    input_ptr += input_length;
    output_ptr += output_length;
  }

  int length = strlen(result->output);
  result->length=length;
}
