/*
 * Copyright 2016-2018 Vanya Yaneva, The University of Edinburgh
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "../utils/options.h"
#include "../utils/read-test-cases.h"
#include "../utils/timing.h"
#include "../utils/utils.h"
#include "../utils/fsm-utils.h"
#include "cl-utils.h"
#include "constants.h"
#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <CL/cl.h>
#include "../kernel-gen/cpu-gen.h"
#include "../kernel-gen/fsm.cl"
#include "../kernel-gen/fsm.h"

#define GPU_SOURCE "../source/main-working.cl"
#define KERNEL_NAME "execute_fsm"

// IMPORTANT: kernel options should be defined in Makefile, based on the
// machine, on which we are compiling  otherwise, the kernel will not build
#ifndef KERNEL_OPTIONS
#define KERNEL_OPTIONS ""
#endif

// we will use pinned memory with DMA transfer to transfer inputs and results
// this is particularly the case for padded and padded-transposed
#define DMA 1

#if FSM_INPUTS_WITH_OFFSETS
#define KNL_ARG_TRANSITIONS 3
#define KNL_ARG_STARTING_STATE 4
#define KNL_ARG_INPUT_LENGTH 5
#define KNL_ARG_OUTPUT_LENGTH 6
#define KNL_ARG_NUM_TEST_CASES 7
#else
#define KNL_ARG_TRANSITIONS 2
#define KNL_ARG_STARTING_STATE 3
#define KNL_ARG_INPUT_LENGTH 4
#define KNL_ARG_OUTPUT_LENGTH 5
#define KNL_ARG_NUM_TEST_CASES 6
#define KNL_ARG_PADDED_INPUT_SIZE 7
#endif

void calculate_dimensions(cl_device_id *, size_t[3], size_t[3], int, int);
void pad_test_case_number(const cl_device_id *, int *);
void read_expected_results(struct partecl_result *, int);

void transpose_inputs_char(char *inputs, int max_input_size, int num_test_cases,
                           int input_length);

void transpose_results_back_char(const char *results_coal,
                                 struct partecl_result *results,
                                 int max_input_size, int num_test_cases);

void calculate_chunks_params(int *num_chunks, size_t *size_inputs_total,
                             const struct partecl_input *inputs_par,
                             const int num_test_cases, const int size_chunks,
                             int *padded_input_chunks, int *inputs_chunks,
                             size_t *size_inputs_chunks,
                             size_t *buf_offsets_chunks,
                             const cl_device_id *device);

void add_kernel_arguments(cl_kernel *knl, cl_mem *buf_inputs,
                          cl_mem *buf_results, cl_mem *buf_offsets,
                          cl_mem *buf_transitions, int *starting_state,
                          int *input_length, int *output_length,
                          int *num_test_cases) {
  cl_int err = clSetKernelArg(*knl, 0, sizeof(cl_mem), buf_inputs);
  if (err != CL_SUCCESS)
    printf("error: clSetKernelArg 0: %d\n", err);

  err = clSetKernelArg(*knl, 1, sizeof(cl_mem), buf_results);
  if (err != CL_SUCCESS)
    printf("error: clSetKernelArg 1: %d\n", err);

#if FSM_INPUTS_WITH_OFFSETS
  err = clSetKernelArg(*knl, 2, sizeof(cl_mem), buf_offsets);
  if (err != CL_SUCCESS)
    printf("error: clSetKernelArg 2: %d\n", err);
#endif

  err = clSetKernelArg(*knl, KNL_ARG_TRANSITIONS, sizeof(cl_mem),
                       buf_transitions);
  if (err != CL_SUCCESS)
    printf("error: clSetKernelArg %d: %d\n", KNL_ARG_TRANSITIONS, err);

  err =
      clSetKernelArg(*knl, KNL_ARG_STARTING_STATE, sizeof(int), starting_state);
  if (err != CL_SUCCESS)
    printf("error: clSetKernelArg %d: %d\n", KNL_ARG_STARTING_STATE, err);

  err = clSetKernelArg(*knl, KNL_ARG_INPUT_LENGTH, sizeof(int), input_length);
  if (err != CL_SUCCESS)
    printf("error: clSetKernelArg %d: %d\n", KNL_ARG_INPUT_LENGTH, err);

  err = clSetKernelArg(*knl, KNL_ARG_OUTPUT_LENGTH, sizeof(int), output_length);
  if (err != CL_SUCCESS)
    printf("error: clSetKernelArg %d: %d\n", KNL_ARG_OUTPUT_LENGTH, err);

  err =
      clSetKernelArg(*knl, KNL_ARG_NUM_TEST_CASES, sizeof(int), num_test_cases);
  if (err != CL_SUCCESS)
    printf("error: clSetKernelArg %d: %d\n", KNL_ARG_NUM_TEST_CASES, err);
}

int main(int argc, char **argv) {

  print_sanity_checks();

  // read command line options
  int do_compare_results = HANDLE_RESULTS;
  int num_runs = NUM_RUNS;
  int do_time = DO_TIME;
  int ldim0 = LDIM;
  int do_choose_device = DO_CHOOSE_DEVICE;
  int size_chunks = SIZE_CHUNKS;
  int do_pad_test_cases = DO_PAD_TEST_CASES;
  int do_sort_test_cases = DO_SORT_TEST_CASES;
  int num_test_cases = 1;
  char *filename = NULL;

  if (read_options(argc, argv, &num_test_cases, &do_compare_results, &do_time,
                   &num_runs, &ldim0, &do_choose_device, &size_chunks,
                   &do_pad_test_cases, &do_sort_test_cases,
                   &filename) == FAIL) {
    return 0;
  }

  // create queue and context
  cl_context ctx;
  cl_command_queue queue_inputs;
  cl_command_queue queue_kernel;
  cl_command_queue queue_results;
  cl_int err;
  cl_device_id device;
  create_context_on_gpu(&ctx, &device, do_choose_device);
  create_command_queue(&queue_inputs, &ctx, &device);
  create_command_queue(&queue_kernel, &ctx, &device);
  create_command_queue(&queue_results, &ctx, &device);

  // execute main code from FSM (TODO: plug main code from source file)
  int num_transitions;
  int starting_state;
  int input_length;
  int output_length;
  if (filename == NULL) {
    printf("Please provide an FSM filename.\n");
    return 0;
  }
  printf("Reading fsm: %s\n", filename);
  transition *transitions =
      read_fsm(filename, &num_transitions, &starting_state, &input_length,
               &output_length);

  if (transitions == NULL) {
    printf("Reading the FSM failed.");
    return -1;
  }

#if DMA
  printf("Using DMA transfer!\n");
#else
  printf("Not using DMA transfer!\n");
#endif

  size_t size_transitions =
      sizeof(transition) * NUM_STATES * MAX_NUM_TRANSITIONS_PER_STATE;
  printf("Size of FSM with %d transitions is %ld bytes.\n", num_transitions,
         size_transitions);

  if (do_pad_test_cases) {
    // pad the test case number to nearest multiple of workgroup size
    pad_test_case_number(&device, &num_test_cases);
  }
  printf("Number of test cases: %d\n", num_test_cases);

  // allocate CPU memory and generate test cases
  struct partecl_input *inputs_par;
  size_t size_inputs_par = sizeof(struct partecl_input) * num_test_cases;
  inputs_par = (struct partecl_input *)malloc(size_inputs_par);
  struct partecl_result *results_par;
  size_t size_results_par = sizeof(struct partecl_result) * num_test_cases;
  results_par = (struct partecl_result *)malloc(size_results_par);

  // read the test cases
  if (read_test_cases(inputs_par, num_test_cases) == FAIL) {
    printf("Failed reading the test cases.\n");
    return -1;
  }

  if (do_sort_test_cases) {

    if (sort_test_cases_by_length(inputs_par, num_test_cases, SORT_ASCENDING) ==
        FAIL) {
      printf("Failed sorting the test cases.\n");
      return -1;
    } else {
      printf("SORTED test cases? YES! Ascending? %d\n", SORT_ASCENDING);
    }
  } else {
    printf("SORTED test cases? NO!\n");
  }

  // calculate the number of chunks
  int num_chunks = 0;
  size_chunks *= KB_TO_B; // turn into bytes
  size_t size_inputs_total = 0;

#if FSM_INPUTS_WITH_OFFSETS || FSM_INPUTS_COAL_CHAR4
  // do not chunk for with-offsets and padded-transposed-char4
  num_chunks = 1;
#else

  if (size_chunks == 0) {

    // we are not chunking
    num_chunks = 1;
    size_inputs_total = sizeof(char) * num_test_cases * PADDED_INPUT_ARRAY_SIZE;

  } else {

    // we are chunking
    // calculate the number of chunks and total size dynamically
    calculate_chunks_params(&num_chunks, &size_inputs_total, inputs_par,
                            num_test_cases, size_chunks, NULL, NULL, NULL, NULL,
                            &device);
  }

  // calculate arrays for chunks
  int num_tests_chunks[num_chunks];
  size_t size_inputs_chunks[num_chunks];
  size_t buf_offsets_chunks[num_chunks];
  int padded_input_size_chunks[num_chunks];

  if (size_chunks == 0) {

    // we are not chunking
    num_tests_chunks[0] = num_test_cases;
    size_inputs_chunks[0] = size_inputs_total;
    buf_offsets_chunks[0] = 0;
    padded_input_size_chunks[0] = PADDED_INPUT_ARRAY_SIZE;

  } else {

    // we are chunking
    // calculating again so set to 0
    num_chunks = 0;
    size_inputs_total = 0;
    calculate_chunks_params(&num_chunks, &size_inputs_total, inputs_par,
                            num_test_cases, size_chunks,
                            padded_input_size_chunks, num_tests_chunks,
                            size_inputs_chunks, buf_offsets_chunks, &device);
  }

  // allocate & populate inputs and results
  char *inputs_chunks[num_chunks];
  char *results_chunks[num_chunks];

  int testid_start = 0;
  for (int j = 0; j < num_chunks; j++) {

    inputs_chunks[j] = (char *)malloc(size_inputs_chunks[j]);
    int num_tests = num_tests_chunks[j];
    char *inptptr = inputs_chunks[j];

    for (int i = testid_start; i < testid_start + num_tests; i++) {

      int padded_size = padded_input_size_chunks[j];
      for (int k = 0; k < padded_size; k++) {

        *inptptr = inputs_par[i].input_ptr[k];
        inptptr++;
      }
    }

    testid_start += num_tests;
    results_chunks[j] = (char *)malloc(size_inputs_chunks[j]);
  }

  for (int j = 0; j < num_chunks; j++) {
    printf("chunk: %d\t num tests: %d\t size: %ld\t padded input size: %d\n", j,
           num_tests_chunks[j], size_inputs_chunks[j],
           padded_input_size_chunks[j]);
  }

  free(inputs_par);
#endif

#if FSM_INPUTS_COAL_CHAR

  for (int j = 0; j < num_chunks; j++) {
    int max_input_size = padded_input_size_chunks[j];
    transpose_inputs_char(inputs_chunks[j], max_input_size, num_tests_chunks[j],
                          input_length);
  }
#else
#if FSM_INPUTS_COAL_CHAR4
  // TODO: NOTE WE ARE NOT TAKING INPUT LENGTH INTO ACCOUNT HERE
  int padded_size =
      PADDED_INPUT_ARRAY_SIZE + CHAR_N - PADDED_INPUT_ARRAY_SIZE % CHAR_N;
  size_t size_inputs_coal_char4 =
      sizeof(cl_char4) * padded_size * num_test_cases / CHAR_N;
  cl_char4 *inputs_coal_char4 = (cl_char4 *)malloc(size_inputs_coal_char4);
  int max_num_inputs =
      PADDED_INPUT_ARRAY_SIZE /
      input_length; // this is the maximum number of inputs per test case
  for (int i = 0; i < max_num_inputs;
       i += CHAR_N) { // which input inside the test case
    for (int j = 0; j < num_test_cases; j++) { // which test case
      struct partecl_input current_input = inputs_par[j];

      size_t idx = (i / CHAR_N) * num_test_cases + j;
      for (int k = 0; k < CHAR_N; k++) {
        char current_symbol = current_input.input_ptr[i + k];
        inputs_coal_char4[idx].s[k] = current_symbol;
      }
    }
  }
  cl_char4 *results_coal_char4 = (cl_char4 *)malloc(size_inputs_coal_char4);
  printf("Size of %d test inputs is %ld bytes.\n", num_test_cases,
         size_inputs_coal_char4);
  printf("Size of %d test results is %ld bytes.\n", num_test_cases,
         size_inputs_coal_char4);

  free(inputs_par);
#else
#if FSM_INPUTS_WITH_OFFSETS
  // calculate sizes
  int total_number_of_inputs;
  size_t size_inputs_offset;
  calculate_sizes_with_offset(&total_number_of_inputs, &size_inputs_offset,
                              inputs_par, num_test_cases);

  // allocate memory
  char *inputs_offset = (char *)malloc(size_inputs_offset);
  char *results_offset = (char *)malloc(size_inputs_offset);
  int *offsets = (int *)malloc(sizeof(int) * num_test_cases);

  // copy data
  partecl_input_to_input_with_offsets(inputs_par, inputs_offset, offsets,
                                      num_test_cases);

  printf("Size of %d test inputs is %ld bytes.\n", num_test_cases,
         size_inputs_offset);
  printf("Size of %d test results is %ld bytes.\n", num_test_cases,
         size_inputs_offset);

  free(inputs_par);
#endif
#endif
#endif

#if !FSM_INPUTS_WITH_OFFSETS && !FSM_INPUTS_COAL_CHAR4
  printf("Size of %d test inputs is %ld bytes.\n", num_test_cases,
         size_inputs_total);
  printf("Size of %d test results is %ld bytes.\n", num_test_cases,
         size_inputs_total);
#endif

  struct partecl_result *exp_results;
  exp_results = (struct partecl_result *)malloc(sizeof(struct partecl_result) *
                                                num_test_cases);
  if (do_compare_results)
    read_expected_results(exp_results, num_test_cases);

  // clalculate dimensions
  size_t gdim[num_chunks][3], ldim[num_chunks][3]; // assuming three dimensions
  for (int j = 0; j < num_chunks; j++) {
    int num_tests = num_test_cases;
#if !FSM_INPUTS_WITH_OFFSETS && !FSM_INPUTS_COAL_CHAR4
    num_tests = num_tests_chunks[j];
#endif
    calculate_dimensions(&device, gdim[j], ldim[j], num_tests, ldim0);
    printf("LDIM = %zd, chunks = %d\n", ldim[j][0], num_chunks);
  }

  // create kernel
  char *knl_text = read_file(GPU_SOURCE);
  if (!knl_text) {
    printf("Couldn't read file %s. Exiting!\n", GPU_SOURCE);
    return -1;
  }

  // build the kernel options
  char kernel_options[1000];
  char *kernel_options_ptr = &kernel_options[0];
  kernel_options_ptr = concatenate_strings(kernel_options_ptr, KERNEL_OPTIONS);
  char ko_num_transitions[50];
  sprintf(ko_num_transitions, " -DNUM_TRANSITIONS_KERNEL=%d",
          NUM_STATES * MAX_NUM_TRANSITIONS_PER_STATE);
  kernel_options_ptr =
      concatenate_strings(kernel_options_ptr, ko_num_transitions);

  // will fsm fit in constant or local memory?
#if FSM_OPTIMISE_CONST_MEM
  int enough_constant_memory =
      size_transitions > get_constant_mem_size(&device) ? 0 : 1;
#else
  int enough_constant_memory = 0;
#endif

  cl_kernel knl;
  if (enough_constant_memory) { // first try to fit in constant memory
    printf("FSM in CONST memory.\n");
    kernel_options_ptr =
        concatenate_strings(kernel_options_ptr, " -DFSM_CONSTANT_MEMORY=1");
    knl = kernel_from_string(ctx, knl_text, KERNEL_NAME, kernel_options);
  } else { // try to fit into local memory
    int enough_local_memory =
        size_transitions > get_local_mem_size(&device) ? 0 : 1;

    if (enough_local_memory) {
      printf("FSM in LOCAL memory.\n");
      kernel_options_ptr =
          concatenate_strings(kernel_options_ptr, " -DFSM_LOCAL_MEMORY=1");
      knl = kernel_from_string(ctx, knl_text, KERNEL_NAME, kernel_options);

    } else {
      printf("FSM in GLOBAL memory.\n");
      knl = kernel_from_string(ctx, knl_text, KERNEL_NAME, kernel_options);
    }
  }
  free(knl_text);

  // start kernel operations
  if (do_time) {
    printf("Time in ms\n");
    printf("trans-inputs\ttrans-results\texec-kernel\ttime-total\n");
  }

  for (int i = 0; i < num_runs; i++) {
    // timing variables
    double trans_fsm = 0.0;
    double trans_inputs = 0.0;
    double trans_results = 0.0;
    double time_gpu = 0.0;
    double end_to_end = 0.0;
    struct timespec ete_start, ete_end;
    cl_ulong ev_start_time, ev_end_time;
    size_t goffset[3] = {0, 0, 0};

#if DMA
    //TODO: probably don't need inputs and results
    struct timespec ete_start_kernel[num_chunks];
    struct timespec ete_end_kernel[num_chunks];
    struct timespec ete_start_inputs[num_chunks];
    struct timespec ete_end_inputs[num_chunks];
    struct timespec ete_start_results[num_chunks];
    struct timespec ete_end_results[num_chunks];
#endif

    // allocate device memory
#if FSM_INPUTS_WITH_OFFSETS
    cl_mem buf_inputs =
        clCreateBuffer(ctx, CL_MEM_READ_WRITE, size_inputs_offset, NULL, &err);
    if (err != CL_SUCCESS)
      printf("error: clCreateBuffer buf_inputs: %d\n", err);

    cl_mem buf_results =
        clCreateBuffer(ctx, CL_MEM_READ_WRITE, size_inputs_offset, NULL, &err);
    if (err != CL_SUCCESS)
      printf("error: clCreateBuffer buf_results: %d\n", err);

    cl_mem buf_offsets = clCreateBuffer(
        ctx, CL_MEM_READ_WRITE, sizeof(int) * num_test_cases, NULL, &err);
    if (err != CL_SUCCESS)
      printf("error: clCreateBuffer buf_offsets: %d\n", err);
#else
#if FSM_INPUTS_COAL_CHAR4
    cl_mem buf_inputs = clCreateBuffer(ctx, CL_MEM_READ_WRITE,
                                       size_inputs_coal_char4, NULL, &err);
    if (err != CL_SUCCESS)
      printf("error: clCreateBuffer buf_inputs: %d\n", err);

    cl_mem buf_results = clCreateBuffer(ctx, CL_MEM_READ_WRITE,
                                        size_inputs_coal_char4, NULL, &err);
    if (err != CL_SUCCESS)
      printf("error: clCreateBuffer buf_results: %d\n", err);
#else
#if DMA
    // we allocate buffers separately for every chunk
    cl_mem buf_inputs_host[num_chunks];
    cl_mem buf_inputs[num_chunks];
    cl_mem buf_results[num_chunks];

    for (int j = 0; j < num_chunks; j++) {
      // host buffer
      buf_inputs_host[j] =
          clCreateBuffer(ctx, CL_MEM_ALLOC_HOST_PTR,
              size_inputs_chunks[j], NULL, &err);
      if (err != CL_SUCCESS)
        printf("error: clCreateBuffer buf_inputs_host[%d]: %d\n", j, err);

      // map host buffer
      char *inputs_dma =
          clEnqueueMapBuffer(queue_inputs, buf_inputs_host[j], CL_TRUE, CL_MAP_WRITE, 0,
              size_inputs_chunks[j], 0, NULL, NULL, &err);
      if (err != CL_SUCCESS)
        printf("error: clEnqueueMapBuffer buf_inputs_host[%d]: %d\n", j, err);

      memcpy(inputs_dma, inputs_chunks[j], size_inputs_chunks[j]);

      // unmap host buffer
      err = clEnqueueUnmapMemObject(queue_inputs, buf_inputs_host[j], inputs_dma, 0, NULL, NULL);
      if (err != CL_SUCCESS)
        printf("error: clEnqueueUnmapMemObject buf_inputs_host[%d]: %d\n", j, err);

      // device buffer
      buf_inputs[j] =
          clCreateBuffer(ctx, CL_MEM_READ_ONLY,
                         size_inputs_chunks[j], NULL, &err);
      if (err != CL_SUCCESS)
        printf("error: clCreateBuffer buf_inputs[%d]: %d\n", j, err);

      buf_results[j] =
          clCreateBuffer(ctx, CL_MEM_USE_HOST_PTR | CL_MEM_WRITE_ONLY,
                         size_inputs_chunks[j], results_chunks[j], &err);
      if (err != CL_SUCCESS)
        printf("error: clCreateBuffer buf_results: %d\n", err);
    }

#else
    cl_mem buf_inputs =
        clCreateBuffer(ctx, CL_MEM_READ_WRITE, size_inputs_total, NULL, &err);
    if (err != CL_SUCCESS)
      printf("error: clCreateBuffer buf_inputs: %d\n", err);

    cl_mem buf_results =
        clCreateBuffer(ctx, CL_MEM_READ_WRITE, size_inputs_total, NULL, &err);
    if (err != CL_SUCCESS)
      printf("error: clCreateBuffer buf_results: %d\n", err);
#endif
#endif
#endif

    cl_mem buf_transitions =
        clCreateBuffer(ctx, CL_MEM_READ_ONLY, size_transitions, NULL, &err);
    if (err != CL_SUCCESS)
      printf("error: clCreateBuffer buf_transitions: %d\n", err);

#if !FSM_INPUTS_WITH_OFFSETS && !FSM_INPUTS_COAL_CHAR4 && DMA
      // we will allocate inputs and results kernel arguments for each chunk
#else

      // add kernel arguments

#if !FSM_INPUTS_WITH_OFFSETS
    add_kernel_arguments(&knl, &buf_inputs, &buf_results, NULL,
                         &buf_transitions, &starting_state, &input_length,
                         &output_length, &num_test_cases);
#else
    add_kernel_arguments(&knl, &buf_inputs, &buf_results, &buf_offsets,
                         &buf_transitions, &starting_state, &input_length,
                         &output_length, &num_test_cases);
#endif

#endif

    // declare events
    cl_event event_inputs[num_chunks];
    cl_event event_offsets[num_chunks];
    cl_event event_kernel[num_chunks];
    cl_event event_results[num_chunks];

    // flush the queues before timing
    err = clFinish(queue_inputs);
    if (err != CL_SUCCESS)
      printf("error: clFinish queue_inputs: %d\n", err);

    err = clFinish(queue_kernel);
    if (err != CL_SUCCESS)
      printf("error: clFinish queue_kernel: %d\n", err);

    err = clFinish(queue_results);
    if (err != CL_SUCCESS)
      printf("error: clFinish queue_results: %d\n", err);

    get_timestamp(&ete_start);

    // transfer FSM to GPU only once
    cl_event event_fsm;
    err = clEnqueueWriteBuffer(queue_inputs, buf_transitions, CL_FALSE, 0,
                               size_transitions, transitions, 0, NULL,
                               &event_fsm);
    if (err != CL_SUCCESS)
      printf("error: clEnqueueWriteBuffer buf_transitions: %d\n", err);

    for (int j = 0; j < num_chunks; j++) {

      // transfer input to device
      
#if FSM_INPUTS_WITH_OFFSETS
      err = clEnqueueWriteBuffer(queue_inputs, buf_offsets, CL_FALSE, 0,
                                 sizeof(int) * num_test_cases, offsets, 0, NULL,
                                 &event_offsets[j]);
      if (err != CL_SUCCESS)
        printf("error: clEnqueueWriteBuffer %d: %d\n", j, err);

      err = clEnqueueWriteBuffer(queue_inputs, buf_inputs, CL_FALSE, 0,
                                 size_inputs_offset, inputs_offset, 1,
                                 &event_offsets[j], &event_inputs[j]);
      if (err != CL_SUCCESS)
        printf("error: clEnqueueWriteBuffer %d: %d\n", j, err);

#else
#if FSM_INPUTS_COAL_CHAR4
      err = clEnqueueWriteBuffer(queue_inputs, buf_inputs, CL_FALSE, 0,
                                 size_inputs_coal_char4, inputs_coal_char4, 0,
                                 NULL, &event_inputs[j]);
      if (err != CL_SUCCESS)
        printf("error: clEnqueueWriteBuffer %d: %d\n", j, err);
#else

      int num_waits = 1;
      cl_event *wait_event = j == 0 ? &event_fsm : &event_inputs[j - 1];

#if DMA
      // add  kernel args
      add_kernel_arguments(&knl, &buf_inputs[j], &buf_results[j], NULL,
                           &buf_transitions, &starting_state, &input_length,
                           &output_length, &num_test_cases);

      get_timestamp(&ete_start_inputs[j]);

      err = clEnqueueCopyBuffer(queue_inputs, buf_inputs_host[j], buf_inputs[j], 0, 
                                0, size_inputs_chunks[j], num_waits, wait_event, 
                                &event_inputs[j]);
      if (err != CL_SUCCESS)
        printf("error: clEnqueueCopyBuffer %d: %d\n", j, err);

      get_timestamp(&ete_end_inputs[j]);
#else

      err = clEnqueueWriteBuffer(queue_inputs, buf_inputs, CL_FALSE,
                                 buf_offsets_chunks[j], size_inputs_chunks[j],
                                 inputs_chunks[j], num_waits, wait_event,
                                 &event_inputs[j]);
      if (err != CL_SUCCESS)
        printf("error: clEnqueueWriteBuffer %d: %d\n", j, err);
#endif

#if !FSM_INPUTS_COAL_CHAR
      // set the padded size argument for the kernel
      err = clSetKernelArg(knl, KNL_ARG_PADDED_INPUT_SIZE, sizeof(int),
                           &padded_input_size_chunks[j]);
      if (err != CL_SUCCESS)
        printf("error: clSetKernelArg %d chunk %d: %d\n",
               KNL_ARG_PADDED_INPUT_SIZE, j, err);
#endif

#endif
#endif

        // launch kernel
#if DMA
      get_timestamp(&ete_start_kernel[j]);
#endif

      err = clEnqueueNDRangeKernel(queue_kernel, knl, 1, goffset, gdim[j],
                                   ldim[j], 1, &event_inputs[j],
                                   &event_kernel[j]);
      if (err != CL_SUCCESS)
        printf("error: clEnqueueNDRangeKernel %d: %d\n", j, err);

#if DMA
      get_timestamp(&ete_end_kernel[j]);
#endif

      // transfer results back
#if FSM_INPUTS_WITH_OFFSETS
      err = clEnqueueReadBuffer(queue_results, buf_results, CL_FALSE, 0,
                                size_inputs_offset, results_offset, 1,
                                &event_kernel[j], &event_results[j]);
      if (err != CL_SUCCESS)
        printf("error: clEnqueueReadBuffer %d: %d\n", j, err);
#else
#if FSM_INPUTS_COAL_CHAR4
      err = clEnqueueReadBuffer(queue_results, buf_results, CL_FALSE, 0,
                                size_inputs_coal_char4, results_coal_char4, 1,
                                &event_kernel[j], &event_results[j]);
      if (err != CL_SUCCESS)
        printf("error: clEnqueueReadBuffer %d: %d\n", j, err);
#else

#if DMA
      // map and unmap results buffer
      get_timestamp(&ete_start_results[j]);

      char *results_dma = clEnqueueMapBuffer(
          queue_results, buf_results[j], CL_FALSE, CL_MAP_READ, 0,
          size_inputs_chunks[j], 1, &event_kernel[j], &event_results[j], &err);
      if (err != CL_SUCCESS)
        printf("error: clEnqueueMapBuffer %d: %d\n", j, err);

      err = clEnqueueUnmapMemObject(queue_results, buf_results[j], results_dma,
                                    1, &event_results[j], NULL);

      get_timestamp(&ete_end_results[j]);
#else

      err = clEnqueueReadBuffer(queue_results, buf_results, CL_FALSE,
                                buf_offsets_chunks[j], size_inputs_chunks[j],
                                results_chunks[j], 1, &event_kernel[j],
                                &event_results[j]);
      if (err != CL_SUCCESS)
        printf("error: clEnqueueReadBuffer %d: %d\n", j, err);
#endif

#endif
#endif
    }

    // finish the kernels
    err = clFinish(queue_inputs);
    if (err != CL_SUCCESS)
      printf("error: clFinish queue_inputs: %d\n", err);

    err = clFinish(queue_kernel);
    if (err != CL_SUCCESS)
      printf("error: clFinish queue_kernel: %d\n", err);

    err = clFinish(queue_results);
    if (err != CL_SUCCESS)
      printf("error: clFinish queue_results: %d\n", err);

    get_timestamp(&ete_end);

    // free memory buffers
#if !FSM_INPUTS_WITH_OFFSETS && !FSM_INPUTS_COAL_CHAR4 && DMA
    // we release all memory buffers for all chunks
    for (int j = 0; j < num_chunks; j++) {
      err = clReleaseMemObject(buf_inputs_host[j]);

      if (err != CL_SUCCESS)
        printf("error: clReleaseMemObject buf_inputs_host[%d]: %d\n", j, err);

      err = clReleaseMemObject(buf_inputs[j]);
      if (err != CL_SUCCESS)
        printf("error: clReleaseMemObject buf_inputs[%d]: %d\n", j, err);

      err = clReleaseMemObject(buf_results[j]);
      if (err != CL_SUCCESS)
        printf("error: clReleaseMemObject buf_results[%d]: %d\n", j, err);
    }
#else
    err = clReleaseMemObject(buf_inputs);
    if (err != CL_SUCCESS)
      printf("error: clReleaseMemObject: %d\n", err);

    err = clReleaseMemObject(buf_results);
    if (err != CL_SUCCESS)
      printf("error: clReleaseMemObjec: %d\n", err);
#endif

    err = clReleaseMemObject(buf_transitions);
    if (err != CL_SUCCESS)
      printf("error: clReleaseMemObjec: %d\n", err);

#if FSM_INPUTS_WITH_OFFSETS
    err = clReleaseMemObject(buf_offsets);
    if (err != CL_SUCCESS)
      printf("error: clReleaseMemObjec: %d\n", err);
#endif

    // gather performance data
    clGetEventProfilingInfo(event_fsm, CL_PROFILING_COMMAND_START,
                            sizeof(cl_ulong), &ev_start_time, NULL);
    clGetEventProfilingInfo(event_fsm, CL_PROFILING_COMMAND_END,
                            sizeof(cl_ulong), &ev_end_time, NULL);
    trans_fsm += (double)(ev_end_time - ev_start_time) / 1000000;

    double total_inputs = 0.0;
    double total_results = 0.0;
    double total_gpu = 0.0;
    for (int j = 0; j < num_chunks; j++) {
#if DMA
      // calculate input transfer through kernel time
      trans_inputs =
          timestamp_diff_in_seconds(ete_start_kernel[j], ete_end_kernel[j]) *
          1000; // in ms
#else
      clGetEventProfilingInfo(event_inputs[j], CL_PROFILING_COMMAND_START,
                              sizeof(cl_ulong), &ev_start_time, NULL);
      clGetEventProfilingInfo(event_inputs[j], CL_PROFILING_COMMAND_END,
                              sizeof(cl_ulong), &ev_end_time, NULL);
      trans_inputs = (double)(ev_end_time - ev_start_time) / 1000000;
      total_inputs += trans_inputs;
#endif

      clGetEventProfilingInfo(event_results[j], CL_PROFILING_COMMAND_START,
                              sizeof(cl_ulong), &ev_start_time, NULL);
      clGetEventProfilingInfo(event_results[j], CL_PROFILING_COMMAND_END,
                              sizeof(cl_ulong), &ev_end_time, NULL);
      trans_results = (double)(ev_end_time - ev_start_time) / 1000000;
      total_results += trans_results;

      clGetEventProfilingInfo(event_kernel[j], CL_PROFILING_COMMAND_START,
                              sizeof(cl_ulong), &ev_start_time, NULL);
      clGetEventProfilingInfo(event_kernel[j], CL_PROFILING_COMMAND_END,
                              sizeof(cl_ulong), &ev_end_time, NULL);
      time_gpu = (double)(ev_end_time - ev_start_time) / 1000000;
      total_gpu += time_gpu;

#if DMA
      // subtract kernel event time from total kernel time to get inputs time
      trans_inputs -= time_gpu;
      total_inputs += trans_inputs;
#else
#endif

      if (do_time) {
        if (j == num_chunks - 1) {
          end_to_end =
              timestamp_diff_in_seconds(ete_start, ete_end) * 1000; // in ms
          printf("%.6f\t%.6f\t%.6f\t%.6f\n", trans_inputs, trans_results,
                 time_gpu, end_to_end);
          printf("totals: %.6f\t%.6f\t%.6f\t%.6f\n", total_inputs,
                 total_results, total_gpu,
                 total_inputs + total_results + total_gpu);
        } else {
          printf("%.6f\t%.6f\t%.6f\n", trans_inputs, trans_results, time_gpu);
        }
      }
    }

#if FSM_INPUTS_WITH_OFFSETS
    results_with_offsets_to_partecl_results(results_offset, results_par,
                                            total_number_of_inputs, offsets,
                                            num_test_cases);
#else
#if FSM_INPUTS_COAL_CHAR
    struct partecl_result *results_parptr = results_par;
    for (int j = 0; j < num_chunks; j++) {

      int max_input_size = padded_input_size_chunks[j];
      int num_tests = num_tests_chunks[j];
      transpose_results_back_char(results_chunks[j], results_parptr,
                                  max_input_size, num_tests);
      results_parptr += num_tests;
    }
#else
#if FSM_INPUTS_COAL_CHAR4
    for (int i = 0; i < num_test_cases; i++) {
      char *outputptr = results_par[i].output;
      int reached_end = 0;
      for (int j = i; j < (padded_size / CHAR_N) * num_test_cases;
           j += num_test_cases) {
        if (reached_end) {
          break;
        }

        for (int k = 0; k < CHAR_N; k++) {
          *outputptr = results_coal_char4[j].s[k];
          if (*outputptr == '\0') {
            reached_end = 1;
            break;
          }
          outputptr++;
        }
      }
    }
#else
    struct partecl_result *results_parptr = results_par;
    for (int j = 0; j < num_chunks; j++) {
      char *resultsptr = results_chunks[j];
      for (int i = 0; i < num_tests_chunks[j]; i++) {
        int padded_size = padded_input_size_chunks[j];
        for (int k = 0; k < padded_size; k++) {
          (*results_parptr).output[k] = *resultsptr;
          resultsptr++;
        }
        results_parptr++;
      }
    }
#endif
#endif
#endif

    // check results
    if (do_compare_results)
      compare_results(results_par, exp_results, num_test_cases);

    for (int j = 0; j < num_chunks; j++) {
#if !DMA // we do not use this event with DMA, as data is transferred when the
         // kernel is started
      err = clReleaseEvent(event_inputs[j]);
      if (err != CL_SUCCESS)
        printf("error: clReleaseEvent (event_inputs) %d: %d\n", j, err);
#endif
      err = clReleaseEvent(event_results[j]);
      if (err != CL_SUCCESS)
        printf("error: clReleaseEvent (event_results) %d: %d\n", j, err);
      err = clReleaseEvent(event_kernel[j]);
      if (err != CL_SUCCESS)
        printf("error: clReleaseEvent (event_kernel) %d: %d\n", j, err);
    }
  }

  free(results_par);
  free(exp_results);
#if !FSM_INPUTS_WITH_OFFSETS && !FSM_INPUTS_COAL_CHAR4
  for (int i = 0; i < num_chunks; i++) {
    free(inputs_chunks[i]);
    free(results_chunks[i]);
  }
#endif
#if FSM_INPUTS_COAL_CHAR4
  free(inputs_coal_char4);
#endif
#if FSM_INPUTS_WITH_OFFSETS
  free(inputs_offset);
  free(results_offset);
  free(offsets);
#endif
}

void pad_test_case_number(const cl_device_id *device, int *num_test_cases) {
  // find out maximum dimensions for device
  cl_int err;

  cl_uint num_dims;
  err = clGetDeviceInfo(*device, CL_DEVICE_MAX_WORK_ITEM_DIMENSIONS,
                        sizeof(num_dims), &num_dims, NULL);
  if (err != CL_SUCCESS)
    printf("error: clGetDeviceInfo CL_DEVICE_MAX_WORK_ITEM_DIMENSIONS: %d\n",
           err);

  size_t dims[num_dims];
  err = clGetDeviceInfo(*device, CL_DEVICE_MAX_WORK_ITEM_SIZES, sizeof(dims),
                        dims, NULL);
  if (err != CL_SUCCESS)
    printf("error: clGetDeviceInfo CL_DEVICE_MAX_WORK_ITEM_SIZES: %d\n", err);

  if (*num_test_cases % dims[0] != 0) {
    int coef = *num_test_cases / dims[0];
    *num_test_cases = (coef + 1) * dims[0];
  }
}

void calculate_dimensions(cl_device_id *device, size_t gdim[3], size_t ldim[3],
                          int num_test_cases, int ldimsupplied) {
  // find out maximum dimensions for device
  cl_int err;

  cl_uint num_dims;
  err = clGetDeviceInfo(*device, CL_DEVICE_MAX_WORK_ITEM_DIMENSIONS,
                        sizeof(num_dims), &num_dims, NULL);
  if (err != CL_SUCCESS)
    printf("error: clGetDeviceInfo CL_DEVICE_MAX_WORK_ITEM_DIMENSIONS: %d\n",
           err);

  size_t dims[num_dims];
  err = clGetDeviceInfo(*device, CL_DEVICE_MAX_WORK_ITEM_SIZES, sizeof(dims),
                        dims, NULL);
  if (err != CL_SUCCESS)
    printf("error: clGetDeviceInfo CL_DEVICE_MAX_WORK_ITEM_SIZES: %d\n", err);

  // calculate local dimension
  int ldim0 = num_test_cases;

  if (ldimsupplied != LDIM) {
    // use the given dimension
    ldim0 = ldimsupplied;
  } else {
    // calculate a dimension
    int div = num_test_cases / dims[0]; // maximum size per work-group
    if (div > 0)
      ldim0 = num_test_cases / (div + 1);

    // ensure that the dimensions will be properly distributed across
    while ((num_test_cases / ldim0) * ldim0 != num_test_cases) {
      div++;
      if (div > 0)
        ldim0 = num_test_cases / div;
    }
  }

  gdim[0] = num_test_cases;
  gdim[1] = 1;
  gdim[2] = 1;
  ldim[0] = ldim0;
  ldim[1] = 1;
  ldim[2] = 1;
}

void transpose_inputs_char(char *inputs, int max_input_size, int num_test_cases,
                           int input_length) {
  // transpose inputs for coalesced reading on gpu
  size_t size = sizeof(char) * max_input_size * num_test_cases;
  char *inputs_temp = (char *)malloc(size);

  for (int i = 0; i < num_test_cases; i++) {
    char *inputs_temp_ptr = inputs_temp + i;
    char *inputs_ptr = inputs + i * max_input_size;
    for (int j = 0; j < max_input_size; j++) {
      *inputs_temp_ptr = *inputs_ptr;
      inputs_temp_ptr += num_test_cases;
      inputs_ptr++;
    }
  }

  char *inputs_ptr = inputs;
  char *inputs_temp_ptr = inputs_temp;
  for (int i = 0; i < num_test_cases * max_input_size; i++) {
    *inputs_ptr = *inputs_temp_ptr;
    inputs_ptr++;
    inputs_temp_ptr++;
  }

  free(inputs_temp);
}

void transpose_results_back_char(const char *results_coal,
                                 struct partecl_result *results,
                                 int max_input_size, int num_test_cases) {
  for (int i = 0; i < num_test_cases; i++) {
    char *outputptr = results[i].output;
    for (int j = i; j < max_input_size * num_test_cases; j += num_test_cases) {
      *outputptr = results_coal[j];
      outputptr++;
    }
  }
}

void calculate_chunk_padding(const struct partecl_input inputs_par,
                             int *current_max_test_length,
                             int *padded_input_length) {
  int tc_length = strlen(inputs_par.input_ptr);
  if (tc_length > *current_max_test_length) {
    *padded_input_length =
        tc_length + 1; // this is the max length in this chunk
    *current_max_test_length = tc_length;
  }

  int length_diff = *padded_input_length - tc_length;
  if (length_diff > 0) {
    tc_length += length_diff;
  }
}

void populate_chunk_arrays(int *padded_input_chunks, int *num_tests_chunks,
                           size_t *size_inputs_chunks,
                           size_t *buf_offsets_chunks, const int chunk_id,
                           const int padded_input_length, const int num_tests,
                           const size_t buf_offset, const size_t size_chunk) {
  if (padded_input_chunks) {
    padded_input_chunks[chunk_id] = padded_input_length;
  }

  if (num_tests_chunks) {
    num_tests_chunks[chunk_id] = num_tests;
  }

  if (buf_offsets_chunks) {
    buf_offsets_chunks[chunk_id] = buf_offset;
  }

  if (size_inputs_chunks) {
    size_inputs_chunks[chunk_id] = size_chunk;
  }
}

void calculate_chunks_params(int *num_chunks, size_t *size_inputs_total,
                             const struct partecl_input *inputs_par,
                             const int num_test_cases, const int size_chunks,
                             int *padded_input_chunks, int *num_tests_chunks,
                             size_t *size_inputs_chunks,
                             size_t *buf_offsets_chunks,
                             const cl_device_id *device) {
  int num_tests = 0;
  int padded_input_length = PADDED_INPUT_ARRAY_SIZE;
  int current_max_test_length = 0;
  size_t size_current_chunk = 0;
  size_t current_buf_offset = 0;
  int testid_start = 0;

  for (int i = testid_start; i < num_test_cases; i++) {

    calculate_chunk_padding(inputs_par[i], &current_max_test_length,
                            &padded_input_length);

    // add the current test case to the chunk
    num_tests++;
    size_current_chunk = sizeof(char) * num_tests * padded_input_length;

    if (size_current_chunk >= (size_t)size_chunks) {

      // we have enough test cases in this chunk

      // pad the number of test cases to be divisible by local workgroup size
      int padded_num_tests = num_tests;
      pad_test_case_number(device, &padded_num_tests);
      if (padded_num_tests > num_tests) {
        continue;
      }

      populate_chunk_arrays(padded_input_chunks, num_tests_chunks,
                            size_inputs_chunks, buf_offsets_chunks, *num_chunks,
                            padded_input_length, num_tests, current_buf_offset,
                            size_current_chunk);
      (*num_chunks)++;
      *size_inputs_total += size_current_chunk;

      num_tests = 0;
      size_current_chunk = 0;
      current_max_test_length = 0;
      current_buf_offset += size_current_chunk;
      testid_start = i + 1;
    }
  }

  // handle remaining tests
  int num_remaining_tests = num_test_cases - testid_start;
  if (num_remaining_tests > 0) {
    size_current_chunk = 0;

    // calculate the size of the last chunk
    for (int i = testid_start; i < num_test_cases; i++) {

      calculate_chunk_padding(inputs_par[i], &current_max_test_length,
                              &padded_input_length);
    }
    size_current_chunk =
        sizeof(char) * num_remaining_tests * padded_input_length;

    populate_chunk_arrays(padded_input_chunks, num_tests_chunks,
                          size_inputs_chunks, buf_offsets_chunks, *num_chunks,
                          padded_input_length, num_remaining_tests,
                          current_buf_offset, size_current_chunk);

    (*num_chunks)++;
    *size_inputs_total += size_current_chunk;
  }
}

void read_expected_results(struct partecl_result *results, int num_test_cases) {

  // TODO:
}

