// src/gossip/protocol/failure_detector.c
// Failure detection logic - separated from state machine

#define _POSIX_C_SOURCE 200809L

#include "roole/gossip/gossip_protocol.h"
#include "roole/core/logger.h"
#include <stdlib.h>

// Note: This is integrated into gossip_protocol.c for simplicity
// In a larger system, you could extract timeout checking into a separate module

// The failure detector logic is now part of gossip_protocol_check_timeouts()
// which handles:
// 1. ACK timeout detection (ALIVE -> SUSPECT)
// 2. Suspect timeout detection (SUSPECT -> DEAD)

// This file serves as documentation that these concerns are separated
// and could be further extracted if needed.

// Example of how to use the protocol:
/*
void failure_detector_example() {
    gossip_protocol_t *proto = ...;
    
    // Periodically check for timeouts
    while (running) {
        gossip_protocol_check_timeouts(proto);
        usleep(100000); // 100ms
    }
}
*/