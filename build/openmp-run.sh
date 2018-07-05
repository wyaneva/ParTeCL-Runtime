#!/bin/sh

NUM_TESTS=$1
DO_TIME=$2
DO_RESULTS=$3
NUM_RUNS=$4
NUM_THREADS=$5
FSM_FILENAME=$6
DO_SORT_TC=$7

export OMP_NUM_THREADS=$NUM_THREADS
./../build/cpu-test $NUM_TESTS -time $DO_TIME -results $DO_RESULTS -runs $NUM_RUNS -filename $FSM_FILENAME -sort $DO_SORT

