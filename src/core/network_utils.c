// src/core/network_utils.c (NEW FILE)
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ifaddrs.h>

/**
 * Get first non-loopback IPv4 address
 * Fallback to 127.0.0.1 if no external interface found
 */
int get_default_advertise_ip(char *out_ip, size_t max_len) {
    struct ifaddrs *ifaddr, *ifa;
    
    if (getifaddrs(&ifaddr) == -1) {
        safe_strncpy(out_ip, "127.0.0.1", max_len);
        return -1;
    }
    
    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL) continue;
        
        if (ifa->ifa_addr->sa_family == AF_INET) {
            struct sockaddr_in *addr = (struct sockaddr_in *)ifa->ifa_addr;
            char ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &addr->sin_addr, ip, sizeof(ip));
            
            // Skip loopback
            if (strcmp(ip, "127.0.0.1") != 0) {
                safe_strncpy(out_ip, ip, max_len);
                freeifaddrs(ifaddr);
                return 0;
            }
        }
    }
    
    // Fallback to loopback
    safe_strncpy(out_ip, "127.0.0.1", max_len);
    freeifaddrs(ifaddr);
    return 0;
}