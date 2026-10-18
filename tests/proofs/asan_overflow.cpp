// Planted defect for `just sanitize-proof` (WO-025): a heap-buffer-overflow. Built only with
// -DSL_PROOFS=ON, and only meaningful under the asan preset, where AddressSanitizer must report it
// and the program must exit non-zero.
#include <cstddef>
#include <cstdio>
#include <cstdlib>

int main(int argc, char ** /*argv*/) {
  const std::size_t size = 8;
  auto *const buffer = static_cast<char *>(std::malloc(size));
  if (buffer == nullptr) {
    return EXIT_FAILURE;
  }
  // The index depends on argc so the compiler cannot prove it out of bounds and drop or warn about
  // the store.
  const std::size_t index = size + static_cast<std::size_t>(argc);
  buffer[index] = 'x'; // one past the allocation
  std::printf("wrote %c\n", buffer[index]);
  std::free(buffer);
  return EXIT_SUCCESS;
}
