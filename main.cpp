#define PORT_HIP
#include "commbench.h"

using namespace CommBench;

#define Type float

int main(int argc, char *argv[]) {

  init();

  // buffer size
  size_t count = 1e9; // 4 GB

  // communicator
  Comm<Type> test(NCCL);
  // pattern
  for (int node = 1; node < 2; node++)
    for (int i = 0; i < 8; i++)
      test.add(count, i, node * 8 + i);

  // report total memory
  report_memory();

  // measure bandwidth and latency
  test.measure(5, 20);

  finalize();
}
