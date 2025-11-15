#!/bin/bash
# test/scripts/cli.sh - Interactive CLI for Roole Distributed Datastore
# Aligned with pure datastore architecture (no DAG/RAG execution)

set -e

# --- Configuration ---
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

# Paths
BUILD_DIR="$PROJECT_ROOT/build"
CONFIG_DIR="$PROJECT_ROOT/config"
LOGS_DIR="$PROJECT_ROOT/logs"

# Executables
ROOLE_NODE="$BUILD_DIR/bin/roole-node"

# Configurations
ROUTER_CONFIG="$CONFIG_DIR/router.ini"
WORKER_CONFIG="$CONFIG_DIR/worker_100.ini"

# Logs
ROUTER_LOG="$LOGS_DIR/router.log"
WORKER_LOG="$LOGS_DIR/worker.log"

# Default settings
METRICS_PORT=7002
INGRESS_HOST="127.0.0.1"
INGRESS_PORT=8081

# RPC Protocol Constants (matching rpc_types.h)
FUNC_ID_DATASTORE_SET=0x30
FUNC_ID_DATASTORE_GET=0x31
FUNC_ID_DATASTORE_UNSET=0x32
FUNC_ID_DATASTORE_LIST=0x33

# --- Colors for output ---
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
MAGENTA='\033[0;35m'
NC='\033[0m' # No Color

# --- Helper Functions ---

print_header() {
    echo ""
    echo "=========================================="
    echo "$1"
    echo "=========================================="
}

print_success() {
    echo -e "${GREEN}✓${NC} $1"
}

print_error() {
    echo -e "${RED}✗${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}⚠${NC} $1"
}

print_info() {
    echo -e "${BLUE}ℹ${NC} $1"
}

print_data() {
    echo -e "${CYAN}→${NC} $1"
}

# Check if build exists
check_build() {
    if [ ! -f "$ROOLE_NODE" ]; then
        print_error "roole-node not found at $ROOLE_NODE"
        echo "Please build the project first:"
        echo "  cd $PROJECT_ROOT"
        echo "  mkdir -p build && cd build"
        echo "  cmake .. && make"
        return 1
    fi
    return 0
}

# Create necessary directories
setup_directories() {
    mkdir -p "$LOGS_DIR"
    print_info "Log directory: $LOGS_DIR"
}

# Function to safely kill nodes
cleanup() {
    echo ""
    print_header "Shutting down nodes"
    
    # Find and kill roole-node processes
    local router_pids=$(pgrep -f "roole-node.*router.ini" 2>/dev/null || true)
    local worker_pids=$(pgrep -f "roole-node.*worker_100.ini" 2>/dev/null || true)
    
    if [ -n "$router_pids" ]; then
        print_info "Stopping router (PIDs: $router_pids)"
        kill -TERM $router_pids 2>/dev/null || true
        sleep 2
        # Force kill if still running
        kill -KILL $router_pids 2>/dev/null || true
    fi
    
    if [ -n "$worker_pids" ]; then
        print_info "Stopping worker (PIDs: $worker_pids)"
        kill -TERM $worker_pids 2>/dev/null || true
        sleep 2
        # Force kill if still running
        kill -KILL $worker_pids 2>/dev/null || true
    fi
    
    print_success "Cleanup complete"
}

# Check if a process is running
check_process() {
    local config_file=$1
    if pgrep -f "roole-node.*$config_file" > /dev/null 2>&1; then
        return 0 # Running
    else
        return 1 # Not running
    fi
}

# Get PID of a running process
get_pid() {
    local config_file=$1
    pgrep -f "roole-node.*$config_file" 2>/dev/null | head -1
}

# --- RPC Helper Functions ---

# Convert string to hex bytes for RPC payload
string_to_hex() {
    local str="$1"
    echo -n "$str" | xxd -p | tr -d '\n'
}

# Pack RPC message (simplified - for demonstration)
# In production, you'd use a proper RPC client binary
send_rpc_request() {
    local func_id=$1
    local payload_hex=$2
    
    print_warning "RPC client not yet implemented"
    print_info "To send requests, implement a C client using rpc_client.h"
    print_info "Function ID: 0x$(printf '%02x' $func_id)"
    print_info "Payload: $payload_hex"
    
    return 1
}

# --- Command Functions ---

# 🚀 Start the router node
start_router() {
    print_header "Starting Router Node"
    
    if check_process "router.ini"; then
        local pid=$(get_pid "router.ini")
        print_warning "Router is already running (PID: $pid)"
        return 0
    fi
    
    if [ ! -f "$ROUTER_CONFIG" ]; then
        print_error "Router config not found: $ROUTER_CONFIG"
        return 1
    fi
    
    print_info "Starting router (ingress-capable node)"
    print_info "Log: $ROUTER_LOG"
    
    # Start router in background
    "$ROOLE_NODE" "$ROUTER_CONFIG" > "$ROUTER_LOG" 2>&1 &
    local pid=$!
    
    sleep 3
    
    if check_process "router.ini"; then
        print_success "Router started (PID: $(get_pid 'router.ini'))"
        print_info "Ingress endpoint: $INGRESS_HOST:$INGRESS_PORT"
        print_info "View logs: tail -f $ROUTER_LOG"
        return 0
    else
        print_error "Router failed to start. Check log: $ROUTER_LOG"
        return 1
    fi
}

# 🏭 Start the worker node
start_worker() {
    print_header "Starting Worker Node"
    
    if check_process "worker_100.ini"; then
        local pid=$(get_pid "worker_100.ini")
        print_warning "Worker is already running (PID: $pid)"
        return 0
    fi
    
    if [ ! -f "$WORKER_CONFIG" ]; then
        print_error "Worker config not found: $WORKER_CONFIG"
        return 1
    fi
    
    print_info "Starting worker (peer-to-peer only)"
    print_info "Log: $WORKER_LOG"
    
    # Start worker in background
    "$ROOLE_NODE" "$WORKER_CONFIG" > "$WORKER_LOG" 2>&1 &
    local pid=$!
    
    sleep 3
    
    if check_process "worker_100.ini"; then
        print_success "Worker started (PID: $(get_pid 'worker_100.ini'))"
        print_info "View logs: tail -f $WORKER_LOG"
        return 0
    else
        print_error "Worker failed to start. Check log: $WORKER_LOG"
        return 1
    fi
}

# 🔗 Check cluster formation
check_cluster() {
    print_header "Checking Cluster Status"
    
    if ! check_process "router.ini"; then
        print_error "Router is not running"
        return 1
    fi
    
    print_info "Waiting for cluster formation (5s)..."
    sleep 5
    
    # Query Prometheus metrics
    if ! command -v curl &> /dev/null; then
        print_warning "curl not found, skipping metrics check"
        return 0
    fi
    
    local metrics_url="http://localhost:$METRICS_PORT/metrics"
    local metrics=$(curl -s "$metrics_url" 2>/dev/null || echo "")
    
    if [ -z "$metrics" ]; then
        print_warning "Could not fetch metrics from $metrics_url"
        return 1
    fi
    
    # Parse cluster metrics
    local total=$(echo "$metrics" | grep "cluster_members_total{" | grep -v "^#" | awk '{print $2}' | head -1)
    local active=$(echo "$metrics" | grep "cluster_members_active{" | grep -v "^#" | awk '{print $2}' | head -1)
    local records=$(echo "$metrics" | grep "datastore_records{" | grep -v "^#" | awk '{print $2}' | head -1)
    
    if [ -z "$total" ]; then
        print_warning "Cluster metrics not yet available"
        return 1
    fi
    
    print_info "Cluster Status:"
    echo "  Total Members: $total"
    echo "  Active Members: $active"
    echo "  Datastore Records: ${records:-0}"
    
    if [ "$active" = "2" ] || [ "$active" -ge "2" ]; then
        print_success "Cluster formed successfully ($active active members)"
        return 0
    else
        print_warning "Cluster formation incomplete (active: $active, expected: 2+)"
        print_info "This may be normal - give it more time"
        return 1
    fi
}

# 📝 Set a record in the datastore
datastore_set() {
    print_header "Set Datastore Record"
    
    if ! check_process "router.ini"; then
        print_error "Router is not running"
        return 1
    fi
    
    echo ""
    read -p "Enter key: " key
    read -p "Enter value: " value
    
    if [ -z "$key" ] || [ -z "$value" ]; then
        print_error "Key and value cannot be empty"
        return 1
    fi
    
    print_info "Setting record: key='$key', value='$value'"
    
    # Note: This requires implementing a proper RPC client
    # For now, show what needs to be sent
    print_warning "RPC Client Implementation Required"
    echo ""
    print_info "To implement SET operation:"
    echo "  1. Create RPC client using rpc_client.h"
    echo "  2. Connect to $INGRESS_HOST:$INGRESS_PORT"
    echo "  3. Send RPC request:"
    echo "     - Function ID: 0x30 (FUNC_ID_DATASTORE_SET)"
    echo "     - Payload format: [key_len:2][key][value_len:4][value]"
    echo "  4. Receive response: [ack:1][version:8]"
    echo ""
    print_info "Example C code:"
    echo "  rpc_client_t *client = rpc_client_connect(\"$INGRESS_HOST\", $INGRESS_PORT, RPC_CHANNEL_INGRESS, 8192);"
    echo "  // Pack request with key and value"
    echo "  rpc_client_call(client, FUNC_ID_DATASTORE_SET, request, request_len, &response, &response_len, 5000);"
    echo ""
    
    return 1
}

# 🔍 Get a record from the datastore
datastore_get() {
    print_header "Get Datastore Record"
    
    if ! check_process "router.ini"; then
        print_error "Router is not running"
        return 1
    fi
    
    echo ""
    read -p "Enter key: " key
    
    if [ -z "$key" ]; then
        print_error "Key cannot be empty"
        return 1
    fi
    
    print_info "Getting record: key='$key'"
    
    print_warning "RPC Client Implementation Required"
    echo ""
    print_info "To implement GET operation:"
    echo "  1. Create RPC client using rpc_client.h"
    echo "  2. Connect to $INGRESS_HOST:$INGRESS_PORT"
    echo "  3. Send RPC request:"
    echo "     - Function ID: 0x31 (FUNC_ID_DATASTORE_GET)"
    echo "     - Payload format: [key_len:2][key]"
    echo "  4. Receive response: [found:1][value_len:4][value][version:8]"
    echo ""
    
    return 1
}

# 🗑️ Unset (delete) a record from the datastore
datastore_unset() {
    print_header "Unset Datastore Record"
    
    if ! check_process "router.ini"; then
        print_error "Router is not running"
        return 1
    fi
    
    echo ""
    read -p "Enter key to delete: " key
    
    if [ -z "$key" ]; then
        print_error "Key cannot be empty"
        return 1
    fi
    
    read -p "Are you sure you want to delete '$key'? (y/N): " confirm
    
    if [ "$confirm" != "y" ] && [ "$confirm" != "Y" ]; then
        print_info "Deletion cancelled"
        return 0
    fi
    
    print_info "Deleting record: key='$key'"
    
    print_warning "RPC Client Implementation Required"
    echo ""
    print_info "To implement UNSET operation:"
    echo "  1. Create RPC client using rpc_client.h"
    echo "  2. Connect to $INGRESS_HOST:$INGRESS_PORT"
    echo "  3. Send RPC request:"
    echo "     - Function ID: 0x32 (FUNC_ID_DATASTORE_UNSET)"
    echo "     - Payload format: [key_len:2][key]"
    echo "  4. Receive response: [ack:1]"
    echo ""
    
    return 1
}

# 📋 List all keys in the datastore
datastore_list() {
    print_header "List Datastore Keys"
    
    if ! check_process "router.ini"; then
        print_error "Router is not running"
        return 1
    fi
    
    print_info "Listing all keys..."
    
    print_warning "RPC Client Implementation Required"
    echo ""
    print_info "To implement LIST operation:"
    echo "  1. Create RPC client using rpc_client.h"
    echo "  2. Connect to $INGRESS_HOST:$INGRESS_PORT"
    echo "  3. Send RPC request:"
    echo "     - Function ID: 0x33 (FUNC_ID_DATASTORE_LIST)"
    echo "     - Payload: empty (or optional prefix filter)"
    echo "  4. Receive response: [count:4][key1_len:2][key1]...[keyN_len:2][keyN]"
    echo ""
    
    return 1
}

# 🧪 Run interactive datastore test
datastore_test() {
    print_header "Interactive Datastore Test"
    
    if ! check_process "router.ini"; then
        print_error "Router is not running"
        return 1
    fi
    
    print_info "This test demonstrates typical datastore operations"
    echo ""
    
    print_info "Test sequence:"
    echo "  1. SET key1 = 'Hello, Roole!'"
    echo "  2. SET key2 = 'Distributed KV Store'"
    echo "  3. GET key1"
    echo "  4. LIST all keys"
    echo "  5. UNSET key1"
    echo "  6. LIST all keys (verify deletion)"
    echo ""
    
    read -p "Press Enter to start test (or Ctrl+C to cancel)..."
    
    print_warning "Implementation Required: Create test_datastore_client.c"
    print_info "This client should:"
    echo "  - Connect to router ingress port"
    echo "  - Execute the test sequence above"
    echo "  - Print results for each operation"
    echo "  - Verify eventual consistency across cluster"
    
    return 0
}

# 🔍 Show current status
show_status() {
    print_header "Current Roole Datastore Status"
    
    # Check processes
    echo "Processes:"
    if check_process "router.ini"; then
        local pid=$(get_pid "router.ini")
        print_success "Router: RUNNING (PID: $pid)"
    else
        print_info "Router: STOPPED"
    fi
    
    if check_process "worker_100.ini"; then
        local pid=$(get_pid "worker_100.ini")
        print_success "Worker: RUNNING (PID: $pid)"
    else
        print_info "Worker: STOPPED"
    fi
    
    echo ""
    
    # Check cluster status (non-blocking)
    if check_process "router.ini" && command -v curl &> /dev/null; then
        local metrics_url="http://localhost:$METRICS_PORT/metrics"
        local metrics=$(curl -s "$metrics_url" 2>/dev/null || echo "")
        
        if [ -n "$metrics" ]; then
            local total=$(echo "$metrics" | grep "cluster_members_total{" | grep -v "^#" | awk '{print $2}' | head -1)
            local active=$(echo "$metrics" | grep "cluster_members_active{" | grep -v "^#" | awk '{print $2}' | head -1)
            local records=$(echo "$metrics" | grep "datastore_records{" | grep -v "^#" | awk '{print $2}' | head -1)
            local bytes=$(echo "$metrics" | grep "datastore_bytes_total{" | grep -v "^#" | awk '{print $2}' | head -1)
            local sets=$(echo "$metrics" | grep "datastore_sets_total{" | grep -v "^#" | awk '{print $2}' | head -1)
            local gets=$(echo "$metrics" | grep "datastore_gets_total{" | grep -v "^#" | awk '{print $2}' | head -1)
            local unsets=$(echo "$metrics" | grep "datastore_unsets_total{" | grep -v "^#" | awk '{print $2}' | head -1)
            
            echo "Cluster:"
            echo "  Members (total): ${total:-0}"
            echo "  Members (active): ${active:-0}"
            echo ""
            echo "Datastore:"
            echo "  Records: ${records:-0}"
            echo "  Total Bytes: ${bytes:-0}"
            echo "  Operations:"
            echo "    SETs: ${sets:-0}"
            echo "    GETs: ${gets:-0}"
            echo "    UNSETs: ${unsets:-0}"
        fi
    fi
    
    echo ""
    
    # Show recent logs
    echo "Recent Logs:"
    if [ -f "$ROUTER_LOG" ]; then
        echo "  Router (last 5 lines):"
        tail -5 "$ROUTER_LOG" | sed 's/^/    /'
    fi
    
    if [ -f "$WORKER_LOG" ]; then
        echo ""
        echo "  Worker (last 5 lines):"
        tail -5 "$WORKER_LOG" | sed 's/^/    /'
    fi
}

# 📊 Show metrics
show_metrics() {
    print_header "Prometheus Metrics"
    
    if ! check_process "router.ini"; then
        print_error "Router is not running"
        return 1
    fi
    
    if ! command -v curl &> /dev/null; then
        print_error "curl not found - cannot fetch metrics"
        return 1
    fi
    
    local metrics_url="http://localhost:$METRICS_PORT/metrics"
    
    print_info "Fetching metrics from $metrics_url"
    echo ""
    
    local metrics=$(curl -s "$metrics_url")
    
    # Filter and display relevant metrics
    echo "=== Cluster Metrics ==="
    echo "$metrics" | grep "cluster_members" | grep -v "^#"
    echo ""
    
    echo "=== Datastore Metrics ==="
    echo "$metrics" | grep "datastore" | grep -v "^#"
    echo ""
    
    echo "=== System Metrics ==="
    echo "$metrics" | grep "uptime_seconds" | grep -v "^#"
    echo ""
    
    print_info "Full metrics: curl $metrics_url | less"
}

# 📁 View logs
view_logs() {
    print_header "Log Viewer"
    
    echo "Available logs:"
    echo "  1) Router log"
    echo "  2) Worker log"
    echo "  3) Both (tail -f)"
    echo ""
    read -p "Select log to view: " choice
    
    case $choice in
        1)
            if [ -f "$ROUTER_LOG" ]; then
                less +G "$ROUTER_LOG"
            else
                print_error "Router log not found: $ROUTER_LOG"
            fi
            ;;
        2)
            if [ -f "$WORKER_LOG" ]; then
                less +G "$WORKER_LOG"
            else
                print_error "Worker log not found: $WORKER_LOG"
            fi
            ;;
        3)
            if [ -f "$ROUTER_LOG" ] && [ -f "$WORKER_LOG" ]; then
                tail -f "$ROUTER_LOG" "$WORKER_LOG"
            else
                print_error "Log files not found"
            fi
            ;;
        *)
            print_error "Invalid choice"
            ;;
    esac
}

# 🔧 Show implementation guide
show_implementation_guide() {
    print_header "RPC Client Implementation Guide"
    
    echo ""
    print_info "To interact with the datastore, implement a C client:"
    echo ""
    
    echo "1. Create src/tools/datastore_client.c:"
    echo ""
    cat << 'EOF'
#include "roole/rpc/rpc_client.h"
#include "roole/rpc/rpc_types.h"
#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <host> <port> <operation> [args]\n", argv[0]);
        fprintf(stderr, "Operations: set <key> <value> | get <key> | unset <key> | list\n");
        return 1;
    }
    
    const char *host = argv[1];
    uint16_t port = atoi(argv[2]);
    const char *op = argv[3];
    
    // Connect to ingress
    rpc_client_t *client = rpc_client_connect(host, port, RPC_CHANNEL_INGRESS, 8192);
    if (!client) {
        fprintf(stderr, "Failed to connect to %s:%u\n", host, port);
        return 1;
    }
    
    if (strcmp(op, "set") == 0 && argc == 6) {
        // SET operation
        const char *key = argv[4];
        const char *value = argv[5];
        
        // Pack request: [key_len:2][key][value_len:4][value]
        uint8_t request[4096];
        uint8_t *ptr = request;
        
        uint16_t key_len = strlen(key);
        uint16_t key_len_net = htons(key_len);
        memcpy(ptr, &key_len_net, 2); ptr += 2;
        memcpy(ptr, key, key_len); ptr += key_len;
        
        uint32_t value_len = strlen(value);
        uint32_t value_len_net = htonl(value_len);
        memcpy(ptr, &value_len_net, 4); ptr += 4;
        memcpy(ptr, value, value_len); ptr += value_len;
        
        size_t request_len = ptr - request;
        
        // Send request
        uint8_t *response = NULL;
        size_t response_len = 0;
        
        int status = rpc_client_call(client, FUNC_ID_DATASTORE_SET, 
                                     request, request_len,
                                     &response, &response_len, 5000);
        
        if (status == RPC_STATUS_SUCCESS) {
            printf("✓ Record set successfully\n");
            if (response_len >= 9) {
                uint64_t version;
                memcpy(&version, response + 1, 8);
                version = be64toh(version);
                printf("Version: %lu\n", version);
            }
        } else {
            fprintf(stderr, "✗ SET failed with status: %d\n", status);
        }
        
        free(response);
    }
    // ... implement GET, UNSET, LIST similarly ...
    
    rpc_client_close(client);
    return 0;
}
EOF
    echo ""
    
    echo "2. Add to CMakeLists.txt:"
    echo "   add_executable(datastore-client src/tools/datastore_client.c)"
    echo "   target_link_libraries(datastore-client roole-rpc roole-core)"
    echo ""
    
    echo "3. Build and use:"
    echo "   cd build && make"
    echo "   ./bin/datastore-client 127.0.0.1 8081 set mykey myvalue"
    echo "   ./bin/datastore-client 127.0.0.1 8081 get mykey"
    echo "   ./bin/datastore-client 127.0.0.1 8081 list"
    echo "   ./bin/datastore-client 127.0.0.1 8081 unset mykey"
    echo ""
}

# --- Menu Logic ---

MENU_OPTIONS=(
    "🚀 Start Router"
    "🏭 Start Worker"
    "🌐 Start All & Check Cluster"
    "🔗 Check Cluster Status"
    "━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    "📝 Set Record (Datastore)"
    "🔍 Get Record (Datastore)"
    "🗑️  Unset Record (Datastore)"
    "📋 List All Keys"
    "🧪 Run Datastore Test"
    "━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    "📊 Show Status"
    "📈 Show Metrics"
    "📁 View Logs"
    "🔧 Implementation Guide"
    "━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    "🛑 Stop All Nodes"
    "🚪 Exit"
)

menu_loop() {
    PS3=$'\n'"${CYAN}Select action:${NC} "
    
    select opt in "${MENU_OPTIONS[@]}"
    do
        echo ""
        case $opt in
            *"Start Router")
                start_router
                ;;
            *"Start Worker")
                start_worker
                ;;
            *"Start All & Check Cluster")
                start_router && \
                start_worker && \
                check_cluster
                ;;
            *"Check Cluster Status")
                check_cluster
                ;;
            *"Set Record"*)
                datastore_set
                ;;
            *"Get Record"*)
                datastore_get
                ;;
            *"Unset Record"*)
                datastore_unset
                ;;
            *"List All Keys")
                datastore_list
                ;;
            *"Run Datastore Test")
                datastore_test
                ;;
            *"Show Status")
                show_status
                ;;
            *"Show Metrics")
                show_metrics
                ;;
            *"View Logs")
                view_logs
                ;;
            *"Implementation Guide")
                show_implementation_guide
                ;;
            *"Stop All Nodes")
                cleanup
                ;;
            *"Exit")
                echo ""
                print_info "Exiting Roole CLI"
                cleanup
                exit 0
                ;;
            "━"*)
                # Separator - do nothing
                ;;
            *)
                print_error "Invalid option: $REPLY"
                ;;
        esac
        echo ""
    done
}

# --- Main Execution ---

# Trap cleanup on exit
trap cleanup EXIT INT TERM

# Initial checks
clear
echo ""
echo "╔════════════════════════════════════════════════════════╗"
echo "║   ROOLE DISTRIBUTED DATASTORE - Interactive CLI       ║"
echo "║   Pure Key-Value Storage with Gossip & Replication    ║"
echo "╚════════════════════════════════════════════════════════╝"
echo ""

print_info "Project root: $PROJECT_ROOT"

if ! check_build; then
    exit 1
fi

setup_directories

print_success "Ready to start"
echo ""
print_warning "Note: RPC client implementation required for datastore operations"
print_info "Select 'Implementation Guide' for details"
echo ""
print_info "Quick Start: Select 'Start All & Check Cluster'"
echo ""

# Start interactive loop
menu_loop