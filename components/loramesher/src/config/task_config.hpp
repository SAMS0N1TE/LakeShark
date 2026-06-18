// src/config/task_config.hpp
#pragma once

#include "system_config.hpp"

namespace loramesher {
namespace config {

/**
 * @brief Configuration for FreeRTOS task stack sizes.
 *
 * Convention: every value here is in **bytes**. Call sites pass
 * `kFooStackSize / kStackBytesPerWord` to xTaskCreate, since this build's
 * FreeRTOS port (Arduino-ESP32 / IDF) interprets `usStackDepth` as words.
 * Watermark APIs return bytes — they convert in the wrapper.
 */
struct TaskConfig {
    /// FreeRTOS stack-element width on this target. ESP32 = 4 bytes/word.
    static constexpr size_t kStackBytesPerWord = 4;

    /// Minimum reasonable usable stack size (bytes).
    static constexpr size_t kMinStackWatermark = 512;

    /// Periodic-monitor warning threshold (bytes free).
    static constexpr size_t kStackWarnBytes = 1024;

    /// Radio event task. Determined empirically: 28% margin (~3664 B free)
    /// at the heaviest tested radio-IRQ load.
    static constexpr size_t kRadioEventStackSize = 13120;

    /// Main protocol task. Observed peak ~7 KB during single-node boot;
    /// sized at 16 KB to absorb deeper paths under multi-neighbor mesh
    /// traffic (routing-table processing, message deserialization).
    static constexpr size_t kProtocolMainStackSize = 16384;

    /// Superframe update task. Observed peak ~7.2 KB; 12 KB gives ~40% margin.
    static constexpr size_t kSuperframeStackSize = 12288;

    /// PingPong message-processing task.
    static constexpr size_t kPingPongProcessStackSize = 8192;

    /// PingPong timeout task.
    static constexpr size_t kPingPongTimeoutStackSize = 8192;
};

/**
 * @brief System-wide task priority definitions
 * 
 * Defines priority levels for all system tasks to ensure proper
 * task scheduling and prevent priority conflicts.
 */
struct TaskPriorities {
    static constexpr uint32_t kIdleTaskPriority = 0;
    static constexpr uint32_t kLowPriority = 5;
    static constexpr uint32_t kNormalPriority = 10;
    static constexpr uint32_t kHighPriority = 15;
    static constexpr uint32_t kRadioEventPriority = kHighPriority;

    // Runtime checks for priority relationships
    static_assert(kRadioEventPriority > kNormalPriority,
                  "Radio events must have higher priority than normal tasks");
};

}  // namespace config
}  // namespace loramesher