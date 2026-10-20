// make_ws_queue (WO-082, SL-ARCH-003 ARCH3-R070): the Chase-Lev work-stealing deque is built by
// WO-086 (ws_deque.hpp, episode 088); until then the factory returns unsupported so a
// job_config that asks for it fails at job_system::start.

#include "sl/runtime/error.hpp"
#include "sl/runtime/work_queue.hpp"

#include <cstddef>
#include <memory>

namespace sl::runtime::jobs {

result<std::unique_ptr<work_queue>> make_ws_queue(std::size_t /*capacity*/) noexcept {
  return make_error(errc::unsupported, "make_ws_queue: the work-stealing deque arrives in WO-086");
}

} // namespace sl::runtime::jobs
