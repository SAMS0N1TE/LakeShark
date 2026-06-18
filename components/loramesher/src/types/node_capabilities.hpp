/**
 * @file node_capabilities.hpp
 * @brief Node capability definitions for mesh networking
 */

#pragma once

#include <cstdint>

namespace loramesher {

/**
 * @brief Bit flags for node capabilities
 *
 * Capabilities define what a node can do in the mesh network.
 * Multiple capabilities can be combined using bitwise OR.
 */
enum NodeCapabilities : uint8_t {
    NONE = 0x00,      ///< No special capabilities
    GATEWAY = 0x01,   ///< Node has internet connectivity
    RESERVED = 0x40,  ///< Reserved for future use
};

}  // namespace loramesher
