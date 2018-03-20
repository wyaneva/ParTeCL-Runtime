/*
 * Copyright 2016 Vanya Yaneva, The University of Edinburgh
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

#ifndef OPTIONS_H
#define OPTIONS_H

// default values
#define HANDLE_RESULTS 1
#define NUM_RUNS 1
#define DO_TIME 0
#define DO_CHOOSE_DEVICE 0
#define NUM_CHUNKS 1
#define LDIM 0

int read_options(int argc, char **argv, int *num_test_cases,
                 int *handle_results, int *do_time, int *num_runs, int *ldim,
                 int *do_choose_device, int *do_overlap, char **filename);

#endif
