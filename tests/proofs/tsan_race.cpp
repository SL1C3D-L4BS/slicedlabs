// Planted defect for `just sanitize-proof` (WO-025): two threads write one int with no
// synchronisation. Built only with -DSL_PROOFS=ON; under the tsan preset ThreadSanitizer must
// report "data race" and the program exit non-zero.
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace {
int shared_counter = 0;

void bump() {
  for (int i = 0; i < 1000; ++i) {
    shared_counter = shared_counter + 1;
  }
}
} // namespace

int main() {
  std::thread first(bump);
  std::thread second(bump);
  first.join();
  second.join();
  std::printf("counter %d\n", shared_counter);
  return EXIT_SUCCESS;
}
