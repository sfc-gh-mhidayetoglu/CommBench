#define PORT_CUDA
#include "commbench.h"

#include <math.h> // for 2^i

using namespace CommBench;

#define Type float

int main(int argc, char *argv[])
{

  // initialize CommBench
  init();

  // count = 2^i
  int i_min = atoi(argv[1]);
  int i_max = atoi(argv[2]);
  int window = atoi(argv[3]);
  size_t count_min = pow(2, i_min);
  size_t count_max = pow(2, i_max);
  if (myid == 0) {
    printf("i_min = %d i_max = %d window = %d\n", i_min, i_max, window);
    for (size_t count = count_min; count <= count_max; count = count * 2)
      printf("count = %ld\n", count);
  }

  // allocate memory
  Type *buffer = allocate<Type>(count_max);
  // report total memory
  report_memory();
  // measurement loop
  for (size_t count = count_min; count <= count_max; count = count * 2) {
    // create new communicator
    Comm<Type> test(NCCL);
    for (int iter = 0; iter < window; iter++)
      test.add(buffer, buffer, count, 0, 8);
    // measure bandwidth and latency
    test.measure(5, 20, count * window);
  }

  // finalize CommBench
  finalize();
}
