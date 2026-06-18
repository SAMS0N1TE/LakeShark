/**
 * @file subslot_scheduler.cpp
 * @brief Implementation of deterministic subslot-based collision mitigation
 */

#include "subslot_scheduler.hpp"

#include <algorithm>

#include "utils/logger.hpp"

namespace loramesher {
namespace protocols {
namespace lora_mesh {

SubslotTiming SubslotScheduler::ComputeTiming(uint32_t slot_duration_ms,
                                              const SubslotConfig& config,
                                              uint16_t node_identifier) {
    SubslotTiming timing;

    if (config.num_subslots == 0 || slot_duration_ms == 0) {
        LOG_WARNING("Invalid subslot config: num_subslots=%d, slot_duration=%u",
                    config.num_subslots, slot_duration_ms);
        return timing;  // is_valid = false
    }

    // Compute which subslot this node belongs to
    switch (config.strategy) {
        case SubslotAssignment::HOP_BASED:
            timing.assigned_subslot =
                static_cast<uint8_t>(node_identifier % config.num_subslots);
            break;
        case SubslotAssignment::ADDRESS_MODULO:
            timing.assigned_subslot =
                static_cast<uint8_t>(node_identifier % config.num_subslots);
            break;
        case SubslotAssignment::RANDOM:
            timing.assigned_subslot =
                static_cast<uint8_t>(node_identifier % config.num_subslots);
            break;
    }

    // Each subslot = guard_time + tx_window
    // Total = num_subslots * (guard_time + tx_window)
    // We want this to fit within slot_duration_ms
    uint32_t total_guard_time =
        static_cast<uint32_t>(config.num_subslots) * config.guard_time_ms;

    if (total_guard_time >= slot_duration_ms) {
        LOG_WARNING("Guard times alone (%u ms) exceed slot duration (%u ms)",
                    total_guard_time, slot_duration_ms);
        return timing;  // is_valid = false
    }

    // Reserve a trailing guard so the last subslot's TX completes before the
    // slot boundary, leaving the superframe task time to re-schedule.
    uint32_t trailing_guard =
        std::min(kTrailingGuardMs, slot_duration_ms / 10u);
    if (total_guard_time + trailing_guard >= slot_duration_ms) {
        trailing_guard = 0;  // degrade gracefully for very short slots
    }
    uint32_t available_tx_time =
        slot_duration_ms - total_guard_time - trailing_guard;
    timing.tx_window_ms = available_tx_time / config.num_subslots;
    timing.subslot_duration_ms = config.guard_time_ms + timing.tx_window_ms;

    if (timing.tx_window_ms == 0) {
        LOG_WARNING("TX window is zero: slot_duration=%u, num_subslots=%d",
                    slot_duration_ms, config.num_subslots);
        return timing;  // is_valid = false
    }

    // TX start = subslot_index * subslot_duration + guard_time
    timing.tx_start_offset_ms = static_cast<uint32_t>(timing.assigned_subslot) *
                                    timing.subslot_duration_ms +
                                config.guard_time_ms;

    timing.is_valid = true;

    LOG_DEBUG(
        "Subslot timing: node_id=%u, subslot=%d/%d, tx_start=%u ms, "
        "tx_window=%u ms",
        node_identifier, timing.assigned_subslot, config.num_subslots,
        timing.tx_start_offset_ms, timing.tx_window_ms);

    return timing;
}

Result SubslotScheduler::ValidateConfig(uint32_t slot_duration_ms,
                                        const SubslotConfig& config,
                                        uint32_t estimated_toa_ms) {
    if (config.num_subslots == 0) {
        return Result(LoraMesherErrorCode::kInvalidParameter,
                      "Number of subslots must be > 0");
    }

    if (slot_duration_ms == 0) {
        return Result(LoraMesherErrorCode::kInvalidParameter,
                      "Slot duration must be > 0");
    }

    uint32_t total_guard_time =
        static_cast<uint32_t>(config.num_subslots) * config.guard_time_ms;

    if (total_guard_time >= slot_duration_ms) {
        return Result(LoraMesherErrorCode::kInvalidParameter,
                      "Guard times exceed slot duration");
    }

    uint32_t available_tx_time = slot_duration_ms - total_guard_time;
    uint32_t trailing_guard =
        std::min(kTrailingGuardMs, slot_duration_ms / 10u);
    if (total_guard_time + trailing_guard < slot_duration_ms) {
        available_tx_time -= trailing_guard;
    }
    uint32_t tx_window_ms = available_tx_time / config.num_subslots;

    if (tx_window_ms == 0) {
        return Result(LoraMesherErrorCode::kInvalidParameter,
                      "TX window would be zero");
    }

    if (estimated_toa_ms > tx_window_ms) {
        return Result(LoraMesherErrorCode::kInvalidParameter,
                      "Estimated ToA exceeds TX window per subslot");
    }

    return Result::Success();
}

bool SubslotScheduler::IsSubslottedSlotType(
    types::protocols::lora_mesh::SlotAllocation::SlotType slot_type) {
    using SlotType = types::protocols::lora_mesh::SlotAllocation::SlotType;
    return slot_type == SlotType::SYNC_BEACON_TX ||
           slot_type == SlotType::DISCOVERY_TX ||
           slot_type == SlotType::DISCOVERY_RX;
}

}  // namespace lora_mesh
}  // namespace protocols
}  // namespace loramesher
