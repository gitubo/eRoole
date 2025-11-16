// src/raft/rpc/raft_serialization.c
// Complete Raft RPC message serialization/deserialization

#define _POSIX_C_SOURCE 200809L

#include "roole/raft/raft_rpc.h"
#include "roole/core/common.h"
#include "roole/logger/logger.h"
#include <string.h>
#include <arpa/inet.h>
#include <endian.h>

// ============================================================================
// HELPER: Network Byte Order Conversion
// ============================================================================

static inline void write_u16(uint8_t *buffer, uint16_t value) {
    uint16_t net_value = htons(value);
    memcpy(buffer, &net_value, sizeof(uint16_t));
}

static inline void write_u32(uint8_t *buffer, uint32_t value) {
    uint32_t net_value = htonl(value);
    memcpy(buffer, &net_value, sizeof(uint32_t));
}

static inline void write_u64(uint8_t *buffer, uint64_t value) {
    uint64_t net_value = htobe64(value);
    memcpy(buffer, &net_value, sizeof(uint64_t));
}

static inline uint16_t read_u16(const uint8_t *buffer) {
    uint16_t net_value;
    memcpy(&net_value, buffer, sizeof(uint16_t));
    return ntohs(net_value);
}

static inline uint32_t read_u32(const uint8_t *buffer) {
    uint32_t net_value;
    memcpy(&net_value, buffer, sizeof(uint32_t));
    return ntohl(net_value);
}

static inline uint64_t read_u64(const uint8_t *buffer) {
    uint64_t net_value;
    memcpy(&net_value, buffer, sizeof(uint64_t));
    return be64toh(net_value);
}

// ============================================================================
// LOG ENTRY SERIALIZATION
// ============================================================================

/**
 * Serialize log entry
 * Format: [term:8][index:8][type:1][data_len:4][data][timestamp:8][client_id:2]
 * Total: 31 + data_len bytes
 */
size_t raft_serialize_log_entry(const raft_log_entry_t *entry,
                                 uint8_t *buffer,
                                 size_t buffer_size) {
    if (!entry || !buffer) {
        LOG_ERROR("Invalid log entry serialization parameters");
        return 0;
    }
    
    size_t required = 8 + 8 + 1 + 4 + entry->data_len + 8 + 2;
    
    if (buffer_size < required) {
        LOG_ERROR("Buffer too small for log entry: need %zu, have %zu", 
                  required, buffer_size);
        return 0;
    }
    
    size_t offset = 0;
    
    // Term (8 bytes)
    write_u64(buffer + offset, entry->term);
    offset += 8;
    
    // Index (8 bytes)
    write_u64(buffer + offset, entry->index);
    offset += 8;
    
    // Type (1 byte)
    buffer[offset++] = (uint8_t)entry->type;
    
    // Data length (4 bytes)
    write_u32(buffer + offset, (uint32_t)entry->data_len);
    offset += 4;
    
    // Data (variable)
    if (entry->data_len > 0 && entry->data) {
        memcpy(buffer + offset, entry->data, entry->data_len);
        offset += entry->data_len;
    }
    
    // Timestamp (8 bytes)
    write_u64(buffer + offset, entry->timestamp_ms);
    offset += 8;
    
    // Client ID (2 bytes)
    write_u16(buffer + offset, entry->client_id);
    offset += 2;
    
    LOG_DEBUG("Serialized log entry: index=%lu, term=%lu, size=%zu bytes",
              entry->index, entry->term, offset);
    
    return offset;
}

/**
 * Deserialize log entry
 */
int raft_deserialize_log_entry(const uint8_t *buffer,
                                size_t buffer_len,
                                raft_log_entry_t *entry) {
    if (!buffer || !entry || buffer_len < 31) {
        LOG_ERROR("Invalid log entry deserialization parameters");
        return -1;
    }
    
    memset(entry, 0, sizeof(raft_log_entry_t));
    
    size_t offset = 0;
    
    // Term
    entry->term = read_u64(buffer + offset);
    offset += 8;
    
    // Index
    entry->index = read_u64(buffer + offset);
    offset += 8;
    
    // Type
    entry->type = (raft_entry_type_t)buffer[offset++];
    
    // Data length
    entry->data_len = read_u32(buffer + offset);
    offset += 4;
    
    // Validate data length
    if (entry->data_len > 0) {
        if (offset + entry->data_len + 8 + 2 > buffer_len) {
            LOG_ERROR("Invalid log entry data length: %zu", entry->data_len);
            return -1;
        }
        
        // Allocate and copy data
        entry->data = safe_malloc(entry->data_len);
        if (!entry->data) {
            LOG_ERROR("Failed to allocate log entry data");
            return -1;
        }
        
        memcpy(entry->data, buffer + offset, entry->data_len);
        offset += entry->data_len;
    } else {
        entry->data = NULL;
    }
    
    // Timestamp
    entry->timestamp_ms = read_u64(buffer + offset);
    offset += 8;
    
    // Client ID
    entry->client_id = read_u16(buffer + offset);
    offset += 2;
    
    LOG_DEBUG("Deserialized log entry: index=%lu, term=%lu, data_len=%zu",
              entry->index, entry->term, entry->data_len);
    
    return 0;
}

// ============================================================================
// REQUEST VOTE SERIALIZATION
// ============================================================================

/**
 * Serialize RequestVote request
 * Format: [term:8][candidate_id:2][last_log_index:8][last_log_term:8]
 * Total: 26 bytes
 */
size_t raft_serialize_request_vote_req(const raft_request_vote_req_t *req,
                                        uint8_t *buffer,
                                        size_t buffer_size) {
    if (!req || !buffer || buffer_size < 26) {
        LOG_ERROR("Invalid RequestVote request serialization parameters");
        return 0;
    }
    
    size_t offset = 0;
    
    // Term
    write_u64(buffer + offset, req->term);
    offset += 8;
    
    // Candidate ID
    write_u16(buffer + offset, req->candidate_id);
    offset += 2;
    
    // Last log index
    write_u64(buffer + offset, req->last_log_index);
    offset += 8;
    
    // Last log term
    write_u64(buffer + offset, req->last_log_term);
    offset += 8;
    
    LOG_DEBUG("Serialized RequestVote request: term=%lu, candidate=%u",
              req->term, req->candidate_id);
    
    return offset;
}

/**
 * Deserialize RequestVote request
 */
int raft_deserialize_request_vote_req(const uint8_t *buffer,
                                       size_t buffer_len,
                                       raft_request_vote_req_t *req) {
    if (!buffer || !req || buffer_len < 26) {
        LOG_ERROR("Invalid RequestVote request deserialization parameters");
        return -1;
    }
    
    size_t offset = 0;
    
    req->term = read_u64(buffer + offset);
    offset += 8;
    
    req->candidate_id = read_u16(buffer + offset);
    offset += 2;
    
    req->last_log_index = read_u64(buffer + offset);
    offset += 8;
    
    req->last_log_term = read_u64(buffer + offset);
    offset += 8;
    
    LOG_DEBUG("Deserialized RequestVote request: term=%lu, candidate=%u",
              req->term, req->candidate_id);
    
    return 0;
}

/**
 * Serialize RequestVote response
 * Format: [term:8][vote_granted:1]
 * Total: 9 bytes
 */
size_t raft_serialize_request_vote_resp(const raft_request_vote_resp_t *resp,
                                         uint8_t *buffer,
                                         size_t buffer_size) {
    if (!resp || !buffer || buffer_size < 9) {
        LOG_ERROR("Invalid RequestVote response serialization parameters");
        return 0;
    }
    
    size_t offset = 0;
    
    // Term
    write_u64(buffer + offset, resp->term);
    offset += 8;
    
    // Vote granted
    buffer[offset++] = resp->vote_granted ? 1 : 0;
    
    LOG_DEBUG("Serialized RequestVote response: term=%lu, granted=%d",
              resp->term, resp->vote_granted);
    
    return offset;
}

/**
 * Deserialize RequestVote response
 */
int raft_deserialize_request_vote_resp(const uint8_t *buffer,
                                        size_t buffer_len,
                                        raft_request_vote_resp_t *resp) {
    if (!buffer || !resp || buffer_len < 9) {
        LOG_ERROR("Invalid RequestVote response deserialization parameters");
        return -1;
    }
    
    size_t offset = 0;
    
    resp->term = read_u64(buffer + offset);
    offset += 8;
    
    resp->vote_granted = (buffer[offset++] != 0);
    
    LOG_DEBUG("Deserialized RequestVote response: term=%lu, granted=%d",
              resp->term, resp->vote_granted);
    
    return 0;
}

// ============================================================================
// APPEND ENTRIES SERIALIZATION
// ============================================================================

/**
 * Serialize AppendEntries request
 * Format: [term:8][leader_id:2][prev_log_index:8][prev_log_term:8]
 *         [leader_commit:8][entry_count:4][entries...]
 * Header: 38 bytes + entries
 */
size_t raft_serialize_append_entries_req(const raft_append_entries_req_t *req,
                                          uint8_t *buffer,
                                          size_t buffer_size) {
    if (!req || !buffer || buffer_size < 38) {
        LOG_ERROR("Invalid AppendEntries request serialization parameters");
        return 0;
    }
    
    size_t offset = 0;
    
    // Term
    write_u64(buffer + offset, req->term);
    offset += 8;
    
    // Leader ID
    write_u16(buffer + offset, req->leader_id);
    offset += 2;
    
    // Prev log index
    write_u64(buffer + offset, req->prev_log_index);
    offset += 8;
    
    // Prev log term
    write_u64(buffer + offset, req->prev_log_term);
    offset += 8;
    
    // Leader commit
    write_u64(buffer + offset, req->leader_commit);
    offset += 8;
    
    // Entry count
    write_u32(buffer + offset, (uint32_t)req->entry_count);
    offset += 4;
    
    // Serialize each entry
    for (size_t i = 0; i < req->entry_count; i++) {
        size_t remaining = buffer_size - offset;
        size_t entry_size = raft_serialize_log_entry(&req->entries[i],
                                                      buffer + offset,
                                                      remaining);
        
        if (entry_size == 0) {
            LOG_ERROR("Failed to serialize log entry %zu/%zu", i + 1, req->entry_count);
            return 0;
        }
        
        offset += entry_size;
    }
    
    LOG_DEBUG("Serialized AppendEntries request: term=%lu, leader=%u, entries=%zu, size=%zu",
              req->term, req->leader_id, req->entry_count, offset);
    
    return offset;
}

/**
 * Deserialize AppendEntries request
 */
int raft_deserialize_append_entries_req(const uint8_t *buffer,
                                         size_t buffer_len,
                                         raft_append_entries_req_t *req) {
    if (!buffer || !req || buffer_len < 38) {
        LOG_ERROR("Invalid AppendEntries request deserialization parameters");
        return -1;
    }
    
    memset(req, 0, sizeof(raft_append_entries_req_t));
    
    size_t offset = 0;
    
    // Term
    req->term = read_u64(buffer + offset);
    offset += 8;
    
    // Leader ID
    req->leader_id = read_u16(buffer + offset);
    offset += 2;
    
    // Prev log index
    req->prev_log_index = read_u64(buffer + offset);
    offset += 8;
    
    // Prev log term
    req->prev_log_term = read_u64(buffer + offset);
    offset += 8;
    
    // Leader commit
    req->leader_commit = read_u64(buffer + offset);
    offset += 8;
    
    // Entry count
    req->entry_count = read_u32(buffer + offset);
    offset += 4;
    
    // Validate entry count
    if (req->entry_count > 1000) {
        LOG_ERROR("Invalid entry count: %zu (max 1000)", req->entry_count);
        return -1;
    }
    
    // Allocate entries array if needed
    if (req->entry_count > 0) {
        req->entries = safe_calloc(req->entry_count, sizeof(raft_log_entry_t));
        if (!req->entries) {
            LOG_ERROR("Failed to allocate entries array");
            return -1;
        }
        
        // Deserialize each entry
        for (size_t i = 0; i < req->entry_count; i++) {
            size_t remaining = buffer_len - offset;
            
            if (raft_deserialize_log_entry(buffer + offset, remaining,
                                           &req->entries[i]) != 0) {
                LOG_ERROR("Failed to deserialize log entry %zu/%zu", i + 1, req->entry_count);
                
                // Cleanup already deserialized entries
                for (size_t j = 0; j < i; j++) {
                    if (req->entries[j].data) {
                        safe_free(req->entries[j].data);
                    }
                }
                safe_free(req->entries);
                req->entries = NULL;
                return -1;
            }
            
            // Calculate size of this entry to advance offset
            size_t entry_size = 31 + req->entries[i].data_len;
            offset += entry_size;
        }
    } else {
        req->entries = NULL;
    }
    
    LOG_DEBUG("Deserialized AppendEntries request: term=%lu, leader=%u, entries=%zu",
              req->term, req->leader_id, req->entry_count);
    
    return 0;
}

/**
 * Serialize AppendEntries response
 * Format: [term:8][success:1][match_index:8]
 * Total: 17 bytes
 */
size_t raft_serialize_append_entries_resp(const raft_append_entries_resp_t *resp,
                                           uint8_t *buffer,
                                           size_t buffer_size) {
    if (!resp || !buffer || buffer_size < 17) {
        LOG_ERROR("Invalid AppendEntries response serialization parameters");
        return 0;
    }
    
    size_t offset = 0;
    
    // Term
    write_u64(buffer + offset, resp->term);
    offset += 8;
    
    // Success
    buffer[offset++] = resp->success ? 1 : 0;
    
    // Match index
    write_u64(buffer + offset, resp->match_index);
    offset += 8;
    
    LOG_DEBUG("Serialized AppendEntries response: term=%lu, success=%d, match=%lu",
              resp->term, resp->success, resp->match_index);
    
    return offset;
}

/**
 * Deserialize AppendEntries response
 */
int raft_deserialize_append_entries_resp(const uint8_t *buffer,
                                          size_t buffer_len,
                                          raft_append_entries_resp_t *resp) {
    if (!buffer || !resp || buffer_len < 17) {
        LOG_ERROR("Invalid AppendEntries response deserialization parameters");
        return -1;
    }
    
    size_t offset = 0;
    
    resp->term = read_u64(buffer + offset);
    offset += 8;
    
    resp->success = (buffer[offset++] != 0);
    
    resp->match_index = read_u64(buffer + offset);
    offset += 8;
    
    LOG_DEBUG("Deserialized AppendEntries response: term=%lu, success=%d, match=%lu",
              resp->term, resp->success, resp->match_index);
    
    return 0;
}

// ============================================================================
// INSTALL SNAPSHOT SERIALIZATION
// ============================================================================

/**
 * Serialize InstallSnapshot request
 * Format: [term:8][leader_id:2][last_included_index:8][last_included_term:8]
 *         [offset:8][data_len:4][data][done:1]
 * Header: 39 bytes + data
 */
size_t raft_serialize_install_snapshot_req(const raft_install_snapshot_req_t *req,
                                            uint8_t *buffer,
                                            size_t buffer_size) {
    if (!req || !buffer || buffer_size < 39) {
        LOG_ERROR("Invalid InstallSnapshot request serialization parameters");
        return 0;
    }
    
    size_t required = 39 + req->data_len;
    if (buffer_size < required) {
        LOG_ERROR("Buffer too small for InstallSnapshot: need %zu, have %zu",
                  required, buffer_size);
        return 0;
    }
    
    size_t offset = 0;
    
    // Term
    write_u64(buffer + offset, req->term);
    offset += 8;
    
    // Leader ID
    write_u16(buffer + offset, req->leader_id);
    offset += 2;
    
    // Last included index
    write_u64(buffer + offset, req->last_included_index);
    offset += 8;
    
    // Last included term
    write_u64(buffer + offset, req->last_included_term);
    offset += 8;
    
    // Offset
    write_u64(buffer + offset, req->offset);
    offset += 8;
    
    // Data length
    write_u32(buffer + offset, (uint32_t)req->data_len);
    offset += 4;
    
    // Data
    if (req->data_len > 0 && req->data) {
        memcpy(buffer + offset, req->data, req->data_len);
        offset += req->data_len;
    }
    
    // Done flag
    buffer[offset++] = req->done ? 1 : 0;
    
    LOG_DEBUG("Serialized InstallSnapshot request: term=%lu, last_idx=%lu, size=%zu",
              req->term, req->last_included_index, offset);
    
    return offset;
}

/**
 * Deserialize InstallSnapshot request
 */
int raft_deserialize_install_snapshot_req(const uint8_t *buffer,
                                           size_t buffer_len,
                                           raft_install_snapshot_req_t *req) {
    if (!buffer || !req || buffer_len < 39) {
        LOG_ERROR("Invalid InstallSnapshot request deserialization parameters");
        return -1;
    }
    
    memset(req, 0, sizeof(raft_install_snapshot_req_t));
    
    size_t offset = 0;
    
    // Term
    req->term = read_u64(buffer + offset);
    offset += 8;
    
    // Leader ID
    req->leader_id = read_u16(buffer + offset);
    offset += 2;
    
    // Last included index
    req->last_included_index = read_u64(buffer + offset);
    offset += 8;
    
    // Last included term
    req->last_included_term = read_u64(buffer + offset);
    offset += 8;
    
    // Offset
    req->offset = read_u64(buffer + offset);
    offset += 8;
    
    // Data length
    req->data_len = read_u32(buffer + offset);
    offset += 4;
    
    // Validate data length
    if (req->data_len > 0) {
        if (offset + req->data_len + 1 > buffer_len) {
            LOG_ERROR("Invalid snapshot data length: %zu", req->data_len);
            return -1;
        }
        
        // Allocate and copy data
        req->data = safe_malloc(req->data_len);
        if (!req->data) {
            LOG_ERROR("Failed to allocate snapshot data");
            return -1;
        }
        
        memcpy(req->data, buffer + offset, req->data_len);
        offset += req->data_len;
    } else {
        req->data = NULL;
    }
    
    // Done flag
    req->done = (buffer[offset++] != 0);
    
    LOG_DEBUG("Deserialized InstallSnapshot request: term=%lu, last_idx=%lu, data_len=%zu",
              req->term, req->last_included_index, req->data_len);
    
    return 0;
}

/**
 * Serialize InstallSnapshot response
 * Format: [term:8]
 * Total: 8 bytes
 */
size_t raft_serialize_install_snapshot_resp(const raft_install_snapshot_resp_t *resp,
                                             uint8_t *buffer,
                                             size_t buffer_size) {
    if (!resp || !buffer || buffer_size < 8) {
        LOG_ERROR("Invalid InstallSnapshot response serialization parameters");
        return 0;
    }
    
    write_u64(buffer, resp->term);
    
    LOG_DEBUG("Serialized InstallSnapshot response: term=%lu", resp->term);
    
    return 8;
}

/**
 * Deserialize InstallSnapshot response
 */
int raft_deserialize_install_snapshot_resp(const uint8_t *buffer,
                                            size_t buffer_len,
                                            raft_install_snapshot_resp_t *resp) {
    if (!buffer || !resp || buffer_len < 8) {
        LOG_ERROR("Invalid InstallSnapshot response deserialization parameters");
        return -1;
    }
    
    resp->term = read_u64(buffer);
    
    LOG_DEBUG("Deserialized InstallSnapshot response: term=%lu", resp->term);
    
    return 0;
}

// ============================================================================
// CLEANUP HELPERS
// ============================================================================

/**
 * Free AppendEntries request entries
 */
void raft_free_append_entries_req(raft_append_entries_req_t *req) {
    if (!req) return;
    
    if (req->entries) {
        for (size_t i = 0; i < req->entry_count; i++) {
            if (req->entries[i].data) {
                safe_free(req->entries[i].data);
            }
        }
        safe_free(req->entries);
        req->entries = NULL;
    }
    req->entry_count = 0;
}

/**
 * Free InstallSnapshot request data
 */
void raft_free_install_snapshot_req(raft_install_snapshot_req_t *req) {
    if (!req) return;
    
    if (req->data) {
        safe_free(req->data);
        req->data = NULL;
    }
    req->data_len = 0;
}