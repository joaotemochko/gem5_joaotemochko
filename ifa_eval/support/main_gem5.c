/* Replacement for Embench's support/main.c that also prints the benchmark
   result, so program output can be compared across simulator configurations. */
#include <stdio.h>
#include "support.h"

int
main (void)
{
  volatile int result;
  int correct;

  initialise_board ();
  initialise_benchmark ();
  warm_caches (WARMUP_HEAT);
  start_trigger ();
  result = benchmark ();
  stop_trigger ();
  correct = verify_benchmark (result);
  printf ("result=%d verify=%s\n", result, correct ? "OK" : "FAIL");
  return !correct;
}
