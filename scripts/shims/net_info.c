#define _GNU_SOURCE
#include "net_info.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>

static char s_cached_ip[64] = "192.168.137.227";
static char s_cached_host[64] = "unx.dj";
static time_t s_last_check = 0;

void get_system_net_info(char *ip_buf, size_t ip_len, char *host_buf, size_t host_len)
{
    time_t now = time(NULL);
    if (now - s_last_check > 4 || s_last_check == 0) {
        s_last_check = now;

        /* Get Hostname */
        if (gethostname(s_cached_host, sizeof(s_cached_host)) != 0 || s_cached_host[0] == '\0') {
            strncpy(s_cached_host, "unx.dj", sizeof(s_cached_host));
        }

        /* Determine active IP via UDP route lookup */
        int s = socket(AF_INET, SOCK_DGRAM, 0);
        if (s >= 0) {
            struct sockaddr_in serv;
            memset(&serv, 0, sizeof(serv));
            serv.sin_family = AF_INET;
            serv.sin_addr.s_addr = inet_addr("1.1.1.1");
            serv.sin_port = htons(53);
            if (connect(s, (const struct sockaddr *)&serv, sizeof(serv)) == 0) {
                struct sockaddr_in name;
                socklen_t namelen = sizeof(name);
                if (getsockname(s, (struct sockaddr *)&name, &namelen) == 0) {
                    inet_ntop(AF_INET, &name.sin_addr, s_cached_ip, sizeof(s_cached_ip));
                }
            }
            close(s);
        }
    }

    if (ip_buf && ip_len > 0) {
        strncpy(ip_buf, s_cached_ip, ip_len - 1);
        ip_buf[ip_len - 1] = '\0';
    }
    if (host_buf && host_len > 0) {
        strncpy(host_buf, s_cached_host, host_len - 1);
        host_buf[host_len - 1] = '\0';
    }
}
