// src/raft/rpc/raft_client.c
// RPC client wrappers for Raft peer communication

#define _POSIX_C_SOURCE 200809L

#include "roole/raft/raft_rpc.h"
#include "roole/rpc/rpc_client.h"
#include "roole/rpc/rpc_types.h"
#include "roole/core/common.h"
#include "roole/logger/logger.h"
#include <stdlib.h>

// ============================================================================
// REQUESTVOTE RPC CLIENT
// ============================================================================

int raft_rpc_request_vote(rpc_client_t *client,
                          const raft_request_vote_req_t *req,
                          raft_request_vote_resp_t *out_resp,
                          int timeout_ms) {
    if (!client || !req || !out_resp) {
        LOG_ERROR("Raft RPC: Invalid RequestVote parameters");
        return RPC_STATUS_BAD_ARGUMENT;
    }
    
    // Serialize request
    uint8_t req_buffer[128];
    size_t req_len = raft_serialize_request_vote_req(req, req_buffer, sizeof(req_buffer));
    
    if (req_len == 0) {
        LOG_ERROR("Raft RPC: Failed to serialize RequestVote request");
        return RPC_STATUS_INTERNAL_ERROR;
    }
    
    // Send RPC
    uint8_t *resp_buffer = NULL;
    size_t resp_len = 0;
    
    int status = rpc_client_call(client, FUNC_ID_RAFT_REQUEST_VOTE,
                                 req_buffer, req_len,
                                 &resp_buffer, &resp_len,
                                 timeout_ms);
    
    if (status != RPC_STATUS_SUCCESS) {
        LOG_DEBUG("Raft RPC: RequestVote call failed with status %d", status);
        return status;
    }
    
    // Deserialize response
    if (raft_deserialize_request_vote_resp(resp_buffer, resp_len, out_resp) != 0) {
        LOG_ERROR("Raft RPC: Failed to deserialize RequestVote response");
        safe_free(resp_buffer);
        return RPC_STATUS_INTERNAL_ERROR;
    }
    
    safe_free(resp_buffer);
    
    LOG_DEBUG("Raft RPC: RequestVote completed - granted=%d, term=%lu",
              out_resp->vote_granted, out_resp->term);
    
    return RPC_STATUS_SUCCESS;
}

// ============================================================================
// APPENDENTRIES RPC CLIENT
// ============================================================================

int raft_rpc_append_entries(rpc_client_t *client,
                            const raft_append_entries_req_t *req,
                            raft_append_entries_resp_t *out_resp,
                            int timeout_ms) {
    if (!client || !req || !out_resp) {
        LOG_ERROR("Raft RPC: Invalid AppendEntries parameters");
        return RPC_STATUS_BAD_ARGUMENT;
    }
    
    // Allocate buffer for request (header + entries)
    // Estimate: 38 bytes header + entries
    size_t est_size = 38;
    for (size_t i = 0; i < req->entry_count; i++) {
        est_size += 31 + req->entries[i].data_len;
    }
    
    uint8_t *req_buffer = safe_malloc(est_size);
    if (!req_buffer) {
        LOG_ERROR("Raft RPC: Failed to allocate AppendEntries buffer");
        return RPC_STATUS_INTERNAL_ERROR;
    }
    
    // Serialize request
    size_t req_len = raft_serialize_append_entries_req(req, req_buffer, est_size);
    
    if (req_len == 0) {
        LOG_ERROR("Raft RPC: Failed to serialize AppendEntries request");
        safe_free(req_buffer);
        return RPC_STATUS_INTERNAL_ERROR;
    }
    
    // Send RPC
    uint8_t *resp_buffer = NULL;
    size_t resp_len = 0;
    
    int status = rpc_client_call(client, FUNC_ID_RAFT_APPEND_ENTRIES,
                                 req_buffer, req_len,
                                 &resp_buffer, &resp_len,
                                 timeout_ms);
    
    safe_free(req_buffer);
    
    if (status != RPC_STATUS_SUCCESS) {
        LOG_DEBUG("Raft RPC: AppendEntries call failed with status %d", status);
        return status;
    }
    
    // Deserialize response
    if (raft_deserialize_append_entries_resp(resp_buffer, resp_len, out_resp) != 0) {
        LOG_ERROR("Raft RPC: Failed to deserialize AppendEntries response");
        safe_free(resp_buffer);
        return RPC_STATUS_INTERNAL_ERROR;
    }
    
    safe_free(resp_buffer);
    
    LOG_DEBUG("Raft RPC: AppendEntries completed - success=%d, match=%lu",
              out_resp->success, out_resp->match_index);
    
    return RPC_STATUS_SUCCESS;
}

// ============================================================================
// INSTALLSNAPSHOT RPC CLIENT
// ============================================================================

int raft_rpc_install_snapshot(rpc_client_t *client,
                              const raft_install_snapshot_req_t *req,
                              raft_install_snapshot_resp_t *out_resp,
                              int timeout_ms) {
    if (!client || !req || !out_resp) {
        LOG_ERROR("Raft RPC: Invalid InstallSnapshot parameters");
        return RPC_STATUS_BAD_ARGUMENT;
    }
    
    // Allocate buffer for request
    size_t req_size = 39 + req->data_len;
    uint8_t *req_buffer = safe_malloc(req_size);
    if (!req_buffer) {
        LOG_ERROR("Raft RPC: Failed to allocate InstallSnapshot buffer");
        return RPC_STATUS_INTERNAL_ERROR;
    }
    
    // Serialize request
    size_t req_len = raft_serialize_install_snapshot_req(req, req_buffer, req_size);
    
    if (req_len == 0) {
        LOG_ERROR("Raft RPC: Failed to serialize InstallSnapshot request");
        safe_free(req_buffer);
        return RPC_STATUS_INTERNAL_ERROR;
    }
    
    // Send RPC
    uint8_t *resp_buffer = NULL;
    size_t resp_len = 0;
    
    int status = rpc_client_call(client, FUNC_ID_RAFT_INSTALL_SNAPSHOT,
                                 req_buffer, req_len,
                                 &resp_buffer, &resp_len,
                                 timeout_ms);
    
    safe_free(req_buffer);
    
    if (status != RPC_STATUS_SUCCESS) {
        LOG_DEBUG("Raft RPC: InstallSnapshot call failed with status %d", status);
        return status;
    }
    
    // Deserialize response
    if (raft_deserialize_install_snapshot_resp(resp_buffer, resp_len, out_resp) != 0) {
        LOG_ERROR("Raft RPC: Failed to deserialize InstallSnapshot response");
        safe_free(resp_buffer);
        return RPC_STATUS_INTERNAL_ERROR;
    }
    
    safe_free(resp_buffer);
    
    LOG_DEBUG("Raft RPC: InstallSnapshot completed - term=%lu", out_resp->term);
    
    return RPC_STATUS_SUCCESS;
}