#include "os/rtos.hpp"

#include "config/system_config.hpp"

#ifdef LORAMESHER_BUILD_ARDUINO
#include "os/rtos_freertos.hpp"

namespace loramesher {
namespace os {

// Eager file-scope singleton. Constructed during this TU's static init,
// so any *runtime* call to instance() observes a non-null pointer. Calls
// from another TU's static initializer are not guaranteed to see it (the
// C++ standard does not order dynamic init across TUs); see rtos.hpp.
namespace {
RTOSFreeRTOS g_impl;
RTOS* g_rtos = &g_impl;
}  // namespace

RTOS& RTOS::instance() {
    return *g_rtos;
}

}  // namespace os
}  // namespace loramesher

#else
#include "os/rtos_mock.hpp"

namespace loramesher {
namespace os {

// Thread-local storage definition for node address cache
thread_local char RTOSMock::thread_local_node_address_[8] = {};

// Eager file-scope singleton. See note in the Arduino branch above and
// the doc comment on RTOS::instance() in rtos.hpp.
namespace {
RTOSMock g_impl;
RTOS* g_rtos = &g_impl;
}  // namespace

RTOS& RTOS::instance() {
    return *g_rtos;
}

}  // namespace os
}  // namespace loramesher

#endif  // LORAMESHER_BUILD_ARDUINO
