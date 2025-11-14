#!/bin/bash
# test/scripts/cli.sh - Interactive CLI for Roole Manual Testing
# Updated for refactored architecture

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
ROOLE_CLIENT="$BUILD_DIR/bin/roole-client"
ROOLE_DAG_REGISTER="$BUILD_DIR/bin/roole-dag-register"

# Configurations
ROUTER_CONFIG="$CONFIG_DIR/router.ini"
WORKER_CONFIG="$CONFIG_DIR/worker_100.ini"

# Logs
ROUTER_LOG="$LOGS_DIR/router.log"
WORKER_LOG="$LOGS_DIR/worker.log"

# Default settings
ROUTER_EXECUTOR_THREADS=2
WORKER_EXECUTOR_THREADS=4
METRICS_PORT=7002
CLIENT_HOST="127.0.0.1"
CLIENT_PORT=8081

# Test data
TEST_DAG_ID="1"
TEST_DAG_NAME="test_dag"
TEST_DAG_NODES="3"

# --- Colors for output ---
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
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
    
    print_info "Starting router with $ROUTER_EXECUTOR_THREADS executor threads"
    print_info "Log: $ROUTER_LOG"
    
    # Start router in background
    "$ROOLE_NODE" "$ROUTER_CONFIG" $ROUTER_EXECUTOR_THREADS > "$ROUTER_LOG" 2>&1 &
    local pid=$!
    
    sleep 3
    
    if check_process "router.ini"; then
        print_success "Router started (PID: $(get_pid 'router.ini'))"
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
    
    print_info "Starting worker with $WORKER_EXECUTOR_THREADS executor threads"
    print_info "Log: $WORKER_LOG"
    
    # Start worker in background
    "$ROOLE_NODE" "$WORKER_CONFIG" $WORKER_EXECUTOR_THREADS > "$WORKER_LOG" 2>&1 &
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
    
    if [ -z "$total" ]; then
        print_warning "Cluster metrics not yet available"
        return 1
    fi
    
    print_info "Cluster Members:"
    echo "  Total: $total"
    echo "  Active: $active"
    
    if [ "$active" = "2" ] || [ "$active" -ge "2" ]; then
        print_success "Cluster formed successfully ($active active members)"
        return 0
    else
        print_warning "Cluster formation incomplete (active: $active, expected: 2+)"
        print_info "This may be normal - give it more time"
        return 1
    fi
}

# 📝 Register test DAG
register_dag() {
    print_header "Registering Test DAG"
    
    if ! check_process "router.ini"; then
        print_error "Router is not running"
        return 1
    fi
    
    if [ ! -f "$ROOLE_DAG_REGISTER" ]; then
        print_error "roole-dag-register not found: $ROOLE_DAG_REGISTER"
        print_info "This tool needs to be implemented"
        return 1
    fi
    
    print_info "Registering DAG '$TEST_DAG_NAME' (ID: $TEST_DAG_ID, nodes: $TEST_DAG_NODES)"
    
    if "$ROOLE_DAG_REGISTER" "$CLIENT_HOST" "$CLIENT_PORT" \
       "$TEST_DAG_ID" "$TEST_DAG_NAME" "$TEST_DAG_NODES"; then
        print_success "DAG registered successfully"
        return 0
    else
        print_error "DAG registration failed"
        return 1
    fi
}

# ✉️ Send test message
send_message() {
    print_header "Sending Test Message"
    
    if ! check_process "router.ini"; then
        print_error "Router is not running"
        return 1
    fi
    
    if [ ! -f "$ROOLE_CLIENT" ]; then
        print_error "roole-client not found: $ROOLE_CLIENT"
        print_info "This tool needs to be implemented"
        return 1
    fi
    
    read -p "Enter message to send (default: 'Hello from CLI test!'): " message
    message="${message:-Hello from CLI test!}"
    
    print_info "Sending message: \"$message\""
    
    if "$ROOLE_CLIENT" "$CLIENT_HOST" "$CLIENT_PORT" "$message"; then
        print_success "Message sent successfully"
        return 0
    else
        print_error "Failed to send message"
        return 1
    fi
}

# 🔍 Show current status
show_status() {
    print_header "Current Roole Status"
    
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
    if check_process "router.ini"; then
        check_cluster || true
    fi
    
    echo ""
    
    # Show recent logs
    echo "Recent Logs:"
    if [ -f "$ROUTER_LOG" ]; then
        echo "  Router (last 5 lines):"
        tail -5 "$ROUTER_LOG" | sed 's/^/    /'
    fi
    
    if [ -f "$WORKER_LOG" ]; then
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
    
    curl -s "$metrics_url" | grep -v "^#" | head -30
    
    echo ""
    print_info "View all metrics: curl $metrics_url"
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

# --- Menu Logic ---

MENU_OPTIONS=(
    "Start Router"
    "Start Worker"
    "Start All & Check Cluster"
    "Check Cluster Status"
    "Register Test DAG"
    "Send Test Message"
    "Show Status"
    "Show Metrics"
    "View Logs"
    "Stop All Nodes"
    "Exit"
)

menu_loop() {
    PS3=$'\nSelect action: '
    
    select opt in "${MENU_OPTIONS[@]}"
    do
        echo ""
        case $opt in
            "Start Router")
                start_router
                ;;
            "Start Worker")
                start_worker
                ;;
            "Start All & Check Cluster")
                start_router && \
                start_worker && \
                check_cluster
                ;;
            "Check Cluster Status")
                check_cluster
                ;;
            "Register Test DAG")
                register_dag
                ;;
            "Send Test Message")
                send_message
                ;;
            "Show Status")
                show_status
                ;;
            "Show Metrics")
                show_metrics
                ;;
            "View Logs")
                view_logs
                ;;
            "Stop All Nodes")
                cleanup
                ;;
            "Exit")
                echo ""
                print_info "Exiting Roole CLI"
                cleanup
                exit 0
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
print_header "Roole Interactive Testing CLI"
print_info "Project root: $PROJECT_ROOT"

if ! check_build; then
    exit 1
fi

setup_directories

print_success "Ready to start"
echo ""
print_info "Tip: Use 'Start All & Check Cluster' for quick setup"
echo ""

# Start interactive loop
menu_loop