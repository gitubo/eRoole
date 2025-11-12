// src/transport/tcp_transport.c
#define _POSIX_C_SOURCE 200809L

#include "roole/transport/tcp_transport.h"
#include "roole/core/logger.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <netinet/tcp.h>

#define MAX_EVENTS 64
#define DEFAULT_BACKLOG 128

typedef struct tcp_connection {
    int socket_fd;
    struct sockaddr_in addr;
    tcp_transport_t *transport;
    tcp_connection_callback_fn callback;
    void *user_data;
    uint8_t *recv_buffer;
    size_t buffer_size;
    size_t data_len;
    int marked_for_deletion;
    struct tcp_connection *next;
} tcp_connection_impl_t;

typedef struct tcp_transport {
    transport_t base;
    int listen_fd;
    int epoll_fd;
    pthread_t accept_thread;
    pthread_t worker_thread;
    tcp_connection_impl_t *connections;
    pthread_mutex_t connections_lock;
    tcp_connection_callback_fn accept_callback;
    void *accept_user_data;
    int threads_started;
} tcp_transport_impl_t;

static void* tcp_accept_thread(void *arg);
static void* tcp_worker_thread(void *arg);
static void handle_connection(tcp_connection_impl_t *conn);

tcp_transport_t* tcp_transport_create_server(const char *bind_addr, uint16_t port,
                                            size_t max_connections) {
    tcp_transport_impl_t *tcp = calloc(1, sizeof(tcp_transport_impl_t));
    if (!tcp) return NULL;
    
    tcp->base.type = TRANSPORT_TYPE_TCP;
    safe_strncpy(tcp->base.bind_addr, bind_addr ? bind_addr : "0.0.0.0", 16);
    tcp->base.port = port;
    tcp->base.shutdown_flag = 0;
    tcp->threads_started = 0;
    
    pthread_mutex_init(&tcp->connections_lock, NULL);
    
    // Create listen socket
    tcp->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (tcp->listen_fd < 0) {
        LOG_ERROR("Failed to create TCP socket: %s", strerror(errno));
        free(tcp);
        return NULL;
    }
    
    // Set socket options
    int opt = 1;
    if (setsockopt(tcp->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        LOG_WARN("Failed to set SO_REUSEADDR: %s", strerror(errno));
    }
    
    // Bind
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    
    if (bind_addr && strcmp(bind_addr, "0.0.0.0") != 0) {
        if (inet_pton(AF_INET, bind_addr, &addr.sin_addr) != 1) {
            LOG_ERROR("Invalid bind address: %s", bind_addr);
            close(tcp->listen_fd);
            free(tcp);
            return NULL;
        }
    } else {
        addr.sin_addr.s_addr = INADDR_ANY;
    }
    
    if (bind(tcp->listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("Failed to bind TCP socket: %s", strerror(errno));
        close(tcp->listen_fd);
        free(tcp);
        return NULL;
    }
    
    // Listen
    if (listen(tcp->listen_fd, DEFAULT_BACKLOG) < 0) {
        LOG_ERROR("Failed to listen on TCP socket: %s", strerror(errno));
        close(tcp->listen_fd);
        free(tcp);
        return NULL;
    }
    
    // Create epoll
    tcp->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (tcp->epoll_fd < 0) {
        LOG_ERROR("Failed to create epoll: %s", strerror(errno));
        close(tcp->listen_fd);
        free(tcp);
        return NULL;
    }
    
    LOG_INFO("TCP transport server created on %s:%u", tcp->base.bind_addr, port);
    return (tcp_transport_t*)tcp;
}

tcp_connection_t* tcp_transport_connect(const char *remote_ip, uint16_t port) {
    int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        LOG_ERROR("Failed to create TCP socket: %s", strerror(errno));
        return NULL;
    }
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    
    if (inet_pton(AF_INET, remote_ip, &addr.sin_addr) != 1) {
        LOG_ERROR("Invalid remote IP: %s", remote_ip);
        close(sock_fd);
        return NULL;
    }
    
    if (connect(sock_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("Failed to connect to %s:%u: %s", remote_ip, port, strerror(errno));
        close(sock_fd);
        return NULL;
    }
    
    tcp_connection_impl_t *conn = calloc(1, sizeof(tcp_connection_impl_t));
    if (!conn) {
        close(sock_fd);
        return NULL;
    }
    
    conn->socket_fd = sock_fd;
    conn->addr = addr;
    conn->buffer_size = 4096;
    conn->recv_buffer = malloc(conn->buffer_size);
    
    if (!conn->recv_buffer) {
        free(conn);
        close(sock_fd);
        return NULL;
    }
    
    LOG_INFO("TCP connection established to %s:%u", remote_ip, port);
    return (tcp_connection_t*)conn;
}

int tcp_transport_start_accepting(tcp_transport_t *transport,
                              tcp_connection_callback_fn callback,
                              void *user_data) {
    tcp_transport_impl_t *tcp = (tcp_transport_impl_t*)transport;
    
    tcp->accept_callback = callback;
    tcp->accept_user_data = user_data;
    
    // Start worker thread first
    if (pthread_create(&tcp->worker_thread, NULL, tcp_worker_thread, tcp) != 0) {
        LOG_ERROR("Failed to create worker thread");
        return -1;
    }
    
    // Start accept thread
    if (pthread_create(&tcp->accept_thread, NULL, tcp_accept_thread, tcp) != 0) {
        LOG_ERROR("Failed to create accept thread");
        tcp->base.shutdown_flag = 1;
        close(tcp->epoll_fd);
        pthread_join(tcp->worker_thread, NULL);
        return -1;
    }
    
    tcp->threads_started = 1;
    return 0;
}

ssize_t tcp_connection_send(tcp_connection_t *connection,
                           const uint8_t *data, size_t len)
{
    tcp_connection_impl_t *c = (tcp_connection_impl_t *)connection;
    if (c->socket_fd < 0) return -1;

    // Disable Nagle algorithm for immediate sending
    int flag = 1;
    setsockopt(c->socket_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    // Blocking send with retry
    ssize_t total = 0;
    while (total < (ssize_t)len) {
        ssize_t nw = send(c->socket_fd, data + total, len - total, MSG_NOSIGNAL);
        if (nw <= 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(1000);
                continue;
            }
            LOG_ERROR("TCP send failed: %s", strerror(errno));
            return -1;
        }
        total += nw;
    }
    return total;
}

ssize_t tcp_connection_recv(tcp_connection_t *connection,
                           uint8_t *buffer, size_t buffer_size)
{
    tcp_connection_impl_t *c = (tcp_connection_impl_t *)connection;
    if (c->socket_fd < 0) return -1;

    ssize_t nr = recv(c->socket_fd, buffer, buffer_size, 0);
    if (nr < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        LOG_ERROR("TCP recv failed: %s", strerror(errno));
    }
    return nr;
}

void tcp_connection_close(tcp_connection_t *connection) {
    if (!connection) return;
    
    tcp_connection_impl_t *conn = (tcp_connection_impl_t*)connection;
    
    if (conn->socket_fd >= 0) {
        // Graceful shutdown
        shutdown(conn->socket_fd, SHUT_RDWR);
        close(conn->socket_fd);
        conn->socket_fd = -1;
    }
    
    free(conn->recv_buffer);
    conn->recv_buffer = NULL;
    free(conn);
}

void tcp_transport_shutdown(tcp_transport_t *transport) {
    if (!transport) return;
    
    tcp_transport_impl_t *tcp = (tcp_transport_impl_t*)transport;
    tcp->base.shutdown_flag = 1;
    
    // Close listen socket to unblock accept()
    if (tcp->listen_fd >= 0) {
        shutdown(tcp->listen_fd, SHUT_RDWR);
        close(tcp->listen_fd);
        tcp->listen_fd = -1;
    }
    
    // Close epoll to unblock epoll_wait()
    if (tcp->epoll_fd >= 0) {
        close(tcp->epoll_fd);
        tcp->epoll_fd = -1;
    }
}

void tcp_transport_destroy(tcp_transport_t *transport) {
    if (!transport) return;
    
    tcp_transport_impl_t *tcp = (tcp_transport_impl_t*)transport;
    
    // Trigger shutdown
    tcp_transport_shutdown(transport);
    
    // Wait for threads to exit
    if (tcp->threads_started) {
        pthread_join(tcp->accept_thread, NULL);
        pthread_join(tcp->worker_thread, NULL);
    }
    
    // Close all connections
    pthread_mutex_lock(&tcp->connections_lock);
    tcp_connection_impl_t *conn = tcp->connections;
    while (conn) {
        tcp_connection_impl_t *next = conn->next;
        if (conn->socket_fd >= 0) {
            close(conn->socket_fd);
            conn->socket_fd = -1;
        }
        free(conn->recv_buffer);
        free(conn);
        conn = next;
    }
    tcp->connections = NULL;
    pthread_mutex_unlock(&tcp->connections_lock);
    
    pthread_mutex_destroy(&tcp->connections_lock);
    free(tcp);
    LOG_INFO("TCP transport destroyed");
}

static void* tcp_accept_thread(void *arg) {
    tcp_transport_impl_t *tcp = (tcp_transport_impl_t*)arg;
    
    while (!tcp->base.shutdown_flag) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        int client_fd = accept(tcp->listen_fd, (struct sockaddr*)&client_addr, &client_len);
        
        if (client_fd < 0) {
            if (tcp->base.shutdown_flag) break;
            if (errno == EINTR || errno == ECONNABORTED) continue;
            if (errno == EINVAL || errno == EBADF) break; // Socket closed
            LOG_ERROR("Accept failed: %s", strerror(errno));
            break;
        }
        
        // Set non-blocking
        int flags = fcntl(client_fd, F_GETFL, 0);
        if (flags == -1 || fcntl(client_fd, F_SETFL, flags | O_NONBLOCK) == -1) {
            LOG_WARN("Failed to set client socket non-blocking");
            close(client_fd);
            continue;
        }
        
        // Disable Nagle algorithm
        int nodelay = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
        
        tcp_connection_impl_t *conn = calloc(1, sizeof(tcp_connection_impl_t));
        if (!conn) {
            close(client_fd);
            continue;
        }
        
        conn->socket_fd = client_fd;
        conn->addr = client_addr;
        conn->transport = (tcp_transport_t*)tcp;
        conn->callback = tcp->accept_callback;
        conn->user_data = tcp->accept_user_data;
        conn->buffer_size = 4096;
        conn->recv_buffer = malloc(conn->buffer_size);
        conn->marked_for_deletion = 0;
        
        if (!conn->recv_buffer) {
            free(conn);
            close(client_fd);
            continue;
        }
        
        // Add to epoll
        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
        ev.data.ptr = conn;
        
        if (epoll_ctl(tcp->epoll_fd, EPOLL_CTL_ADD, client_fd, &ev) < 0) {
            LOG_ERROR("Failed to add client to epoll: %s", strerror(errno));
            free(conn->recv_buffer);
            free(conn);
            close(client_fd);
            continue;
        }
        
        // Add to connections list
        pthread_mutex_lock(&tcp->connections_lock);
        conn->next = tcp->connections;
        tcp->connections = conn;
        pthread_mutex_unlock(&tcp->connections_lock);
        
        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
        LOG_INFO("New TCP connection from %s:%d (fd=%d)", 
                 client_ip, ntohs(client_addr.sin_port), client_fd);
    }
    
    LOG_DEBUG("Accept thread exiting");
    return NULL;
}

static void* tcp_worker_thread(void *arg) {
    tcp_transport_impl_t *tcp = (tcp_transport_impl_t*)arg;
    struct epoll_event events[MAX_EVENTS];
    
    while (!tcp->base.shutdown_flag) {
        int nfds = epoll_wait(tcp->epoll_fd, events, MAX_EVENTS, 100);
        
        if (nfds < 0) {
            if (errno == EINTR) continue;
            if (errno == EBADF) break; // epoll_fd closed
            LOG_ERROR("epoll_wait failed: %s", strerror(errno));
            break;
        }
        
        for (int i = 0; i < nfds; i++) {
            tcp_connection_impl_t *conn = (tcp_connection_impl_t*)events[i].data.ptr;
            
            if (!conn || conn->marked_for_deletion) continue;
            
            if (events[i].events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
                LOG_DEBUG("TCP connection error/hangup on fd=%d", conn->socket_fd);
                conn->marked_for_deletion = 1;
                
                if (tcp->epoll_fd >= 0) {
                    epoll_ctl(tcp->epoll_fd, EPOLL_CTL_DEL, conn->socket_fd, NULL);
                }
                
                if (conn->socket_fd >= 0) {
                    close(conn->socket_fd);
                    conn->socket_fd = -1;
                }
                continue;
            }
            
            if (events[i].events & EPOLLIN) {
                handle_connection(conn);
            }
        }
        
        // Cleanup marked connections
        pthread_mutex_lock(&tcp->connections_lock);
        tcp_connection_impl_t **pp = &tcp->connections;
        while (*pp) {
            tcp_connection_impl_t *curr = *pp;
            if (curr->marked_for_deletion) {
                *pp = curr->next;
                free(curr->recv_buffer);
                free(curr);
            } else {
                pp = &curr->next;
            }
        }
        pthread_mutex_unlock(&tcp->connections_lock);
    }
    
    LOG_DEBUG("Worker thread exiting");
    return NULL;
}

static void handle_connection(tcp_connection_impl_t *conn) {
    if (conn->marked_for_deletion) return;
    
    while (1) {
        ssize_t n = recv(conn->socket_fd, 
                        conn->recv_buffer + conn->data_len,
                        conn->buffer_size - conn->data_len, 0);
        
        if (n > 0) {
            conn->data_len += n;
            
            // Call user callback with received data
            if (conn->callback) {
                conn->callback((tcp_connection_t*)conn, 
                             conn->recv_buffer, 
                             conn->data_len, 
                             conn->user_data);
            }
            
            // Reset buffer for next read
            conn->data_len = 0;
        } else if (n == 0) {
            // Connection closed by peer
            LOG_DEBUG("TCP connection closed by peer (fd=%d)", conn->socket_fd);
            conn->marked_for_deletion = 1;
            
            tcp_transport_impl_t *tcp = (tcp_transport_impl_t*)conn->transport;
            if (tcp && tcp->epoll_fd >= 0) {
                epoll_ctl(tcp->epoll_fd, EPOLL_CTL_DEL, conn->socket_fd, NULL);
            }
            
            if (conn->socket_fd >= 0) {
                close(conn->socket_fd);
                conn->socket_fd = -1;
            }
            break;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break; // No more data available
            }
            LOG_ERROR("TCP recv error on fd=%d: %s", conn->socket_fd, strerror(errno));
            conn->marked_for_deletion = 1;
            
            tcp_transport_impl_t *tcp = (tcp_transport_impl_t*)conn->transport;
            if (tcp && tcp->epoll_fd >= 0) {
                epoll_ctl(tcp->epoll_fd, EPOLL_CTL_DEL, conn->socket_fd, NULL);
            }
            
            if (conn->socket_fd >= 0) {
                close(conn->socket_fd);
                conn->socket_fd = -1;
            }
            break;
        }
    }
}