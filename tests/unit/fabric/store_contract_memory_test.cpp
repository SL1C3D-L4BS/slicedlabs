// FAB-INV-STO-05 (SL-ARCH-002 ARCH2-R02): the store_backend contract suite passes on
// memory_backend, with and without an idle fault hook attached.

#include "sl/fabric/memory_backend.hpp"
#include "sl/fabric/store.hpp"

#include "../../../engine/fabric/testing/store_contract.hpp"
#include "../../../engine/fabric/testing/store_faults.hpp"

#include <doctest/doctest.h>

#include <memory>

namespace {

struct memory_factory {
  static std::unique_ptr<sl::fabric::store_backend> make() {
    return std::make_unique<sl::fabric::memory_backend>();
  }
};

struct faulty_memory_factory {
  static std::unique_ptr<sl::fabric::store_backend> make() {
    return std::make_unique<sl::fabric::memory_backend>(
        std::make_shared<sl::fabric::testing::store_faults>());
  }
};

} // namespace

TEST_CASE("FAB-INV-STO-05: the backend contract suite passes on memory_backend") {
  memory_factory factory;
  sl::fabric::testing::store_contract(factory);
}

TEST_CASE("FAB-INV-STO-05: the contract suite passes on memory_backend with an idle fault hook") {
  faulty_memory_factory factory;
  sl::fabric::testing::store_contract(factory);
}
