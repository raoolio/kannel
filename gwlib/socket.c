#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>

#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/utsname.h>

#include "gwlib.h"


static Mutex *inet_mutex;

static Octstr *official_name = NULL;
static Octstr *official_ip = NULL;

#ifndef UDP_PACKET_MAX_SIZE
#define UDP_PACKET_MAX_SIZE (64*1024)
#endif


int make_server_socket(int port)
{
    struct sockaddr_in addr;
    int s;
    int reuse;

    s = socket(PF_INET, SOCK_STREAM, 0);
    if (s == -1) {
        error(errno, "socket failed");
        goto error;
    }

    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    reuse = 1;
    if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (char *) &reuse,
                   sizeof(reuse)) == -1) {
        error(errno, "setsockopt failed for server address");
        goto error;
    }

    if (bind(s, (struct sockaddr *) &addr, sizeof(addr)) == -1) {
        error(errno, "bind failed");
        goto error;
    }

    if (listen(s, 10) == -1) {
        error(errno, "listen failed");
        goto error;
    }

    return s;

error:
    if (s >= 0)
        (void) close(s);
    return -1;
}


int tcpip_connect_to_server(char *hostname, int port)
{

    return tcpip_connect_to_server_with_port(hostname, port, 0);
}


int tcpip_connect_to_server_with_port(char *hostname, int port, int our_port)
{
    struct sockaddr_in addr;
    struct sockaddr_in o_addr;
    struct hostent hostinfo;
    int s;

    s = socket(PF_INET, SOCK_STREAM, 0);
    if (s == -1) {
        error(errno, "Couldn't create new socket.");
        goto error;
    }

    if (gw_gethostbyname(&hostinfo, hostname) == -1) {
        error(errno, "gethostbyname failed");
        goto error;
    }

    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr = *(struct in_addr *) hostinfo.h_addr;

    if (our_port > 0) {
        int reuse;

        o_addr.sin_family = AF_INET;
        o_addr.sin_port = htons(our_port);
        o_addr.sin_addr.s_addr = htonl(INADDR_ANY);

        reuse = 1;
        if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (char *) &reuse,
                       sizeof(reuse)) == -1) {
            error(errno, "setsockopt failed before bind");
            goto error;
        }
        if (bind(s, (struct sockaddr *) &o_addr, sizeof(o_addr)) == -1) {
            error(errno, "bind to local port %d failed", our_port);
            goto error;
        }
    }

    if (connect(s, (struct sockaddr *) &addr, sizeof(addr)) == -1) {
        error(errno, "connect failed");
        goto error;
    }

    return s;

error:
    error(0, "error connecting to server `%s' at port `%d'",
          hostname, port);
    if (s >= 0)
        close(s);
    return -1;
}


int write_to_socket(int socket, char *str)
{
    size_t len;
    int ret;

    len = strlen(str);
    while (len > 0) {
        ret = write(socket, str, len);
        if (ret == -1) {
            if (errno == EAGAIN) continue;
            if (errno == EINTR) continue;
            error(errno, "Writing to socket failed");
            return -1;
        }
        /* ret may be less than len, if the writing was interrupted
           by a signal. */
        len -= ret;
        str += ret;
    }
    return 0;
}


int socket_query_blocking(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0) {
        warning(errno, "cannot tell if fd %d is blocking", fd);
        return -1;
    }

    return (flags & O_NONBLOCK) != 0;
}

int socket_set_blocking(int fd, int blocking)
{
    int flags, newflags;

    flags = fcntl(fd, F_GETFL);
    if (flags < 0) {
        error(errno, "cannot get flags for fd %d", fd);
        return -1;
    }

    if (blocking)
        newflags = flags & ~O_NONBLOCK;
    else
        newflags = flags | O_NONBLOCK;

    if (newflags != flags) {
        if (fcntl(fd, F_SETFL, newflags) < 0) {
            error(errno, "cannot set flags for fd %d", fd);
            return -1;
        }
    }

    return 0;
}

char *socket_get_peer_ip(int s)
{
    socklen_t len;
    struct sockaddr_in addr;

    len = sizeof(addr);
    if (getsockname(s, (struct sockaddr *) &addr, &len) == -1) {
        error(errno, "getsockname failed");
        return gw_strdup("0.0.0.0");
    }

    gw_assert(addr.sin_family == AF_INET);
    return gw_strdup(inet_ntoa(addr.sin_addr));  /* XXX not thread safe */
}

int read_line(int fd, char *line, int max)
{
    char *start;
    int ret;

    start = line;
    while (max > 0) {
        ret = read(fd, line, 1);
        if (ret == -1) {
            if (errno == EAGAIN) continue;
            if (errno == EINTR) continue;
            error(errno, "read failed");
            return -1;
        }
        if (ret == 0)
            break;
        ++line;
        --max;
        if (line[-1] == '\n')
            break;
    }

    if (line == start)
        return 0;

    if (line[-1] == '\n')
        --line;
    if (line[-1] == '\r')
        --line;
    *line = '\0';

    return 1;
}


int read_to_eof(int fd, char **data, size_t *len)
{
    size_t size;
    int ret;
    char *p;

    *len = 0;
    size = 0;
    *data = NULL;
    for (;;) {
        if (*len == size) {
            size += 16 * 1024;
            p = gw_realloc(*data, size);
            *data = p;
        }
        ret = read(fd, *data + *len, size - *len);
        if (ret == -1) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN) continue;
            error(errno, "Error while reading");
            goto error;
        }
        if (ret == 0)
            break;
        *len += ret;
    }

    return 0;

error:
    gw_free(*data);
    *data = NULL;
    return -1;
}


int read_available(int fd, long wait_usec)
{
    fd_set rf;
    struct timeval to;
    int ret;
    div_t waits;

    gw_assert(fd >= 0);

    FD_ZERO(&rf);
    FD_SET(fd, &rf);
    waits = div(wait_usec, 1000000);
    to.tv_sec = waits.quot;
    to.tv_usec = waits.rem;
retry:
    ret = select(fd + 1, &rf, NULL, NULL, &to);
    if (ret > 0 && FD_ISSET(fd, &rf))
        return 1;
    if (ret < 0) {
        /* In most select() implementations, to will now contain the
         * remaining time rather than the original time.  That is exactly
         * what we want when retrying after an interrupt. */
        switch (errno) {
            /*The first two entries here are OK*/
        case EINTR:
            goto retry;
        case EAGAIN:
            return 1;
            /* We are now sucking mud, figure things out here
             * as much as possible before it gets lost under
             * layers of abstraction.  */
        case EBADF:
            if (!FD_ISSET(fd, &rf)) {
                warning(0, "Tried to select on fd %d, not in the set!\n", fd);
            } else {
                warning(0, "Tried to select on invalid fd %d!\n", fd);
            }
            break;
        case EINVAL:
            /* Solaris catchall "It didn't work" error, lets apply
             * some tests and see if we can catch it. */

            /* First up, try invalid timeout*/
            if (to.tv_sec > 10000000)
                warning(0, "Wait more than three years for a select?\n");
            if (to.tv_usec > 1000000)
                warning(0, "There are only 1000000 usec in a second...\n");
            break;


        }
        return -1; 	/* some error */
    }
    return 0;
}



int udp_client_socket(void)
{
    int s;

    s = socket(PF_INET, SOCK_DGRAM, 0);
    if (s == -1) {
        error(errno, "Couldn't create a UDP socket");
        return -1;
    }

    return s;
}


int udp_bind(int port, const char *interface_name)
{
    int s;
    struct sockaddr_in sa;
    struct hostent hostinfo;

    s = socket(PF_INET, SOCK_DGRAM, 0);
    if (s == -1) {
        error(errno, "Couldn't create a UDP socket");
        return -1;
    }

    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    if (strcmp(interface_name, "*") == 0)
        sa.sin_addr.s_addr = htonl(INADDR_ANY);
    else {
        if (gw_gethostbyname(&hostinfo, interface_name) == -1) {
            error(errno, "gethostbyname failed");
            return -1;
        }
        sa.sin_addr = *(struct in_addr *) hostinfo.h_addr;
    }

    if (bind(s, (struct sockaddr *) &sa, (int) sizeof(sa)) == -1) {
        error(errno, "Couldn't bind a UDP socket to port %d", port);
        (void) close(s);
        return -1;
    }

    return s;
}


Octstr *udp_create_address(Octstr *host_or_ip, int port)
{
    struct sockaddr_in sa;
    struct hostent h;

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);

    if (strcmp(octstr_get_cstr(host_or_ip), "*") == 0) {
        sa.sin_addr.s_addr = INADDR_ANY;
    } else {
        if (gw_gethostbyname(&h, octstr_get_cstr(host_or_ip)) == -1) {
            error(0, "Couldn't find the IP number of `%s'",
                  octstr_get_cstr(host_or_ip));
            return NULL;
        }
        sa.sin_addr = *(struct in_addr *) h.h_addr_list[0];
    }

    return octstr_create_from_data((char *) &sa, sizeof(sa));
}


int udp_get_port(Octstr *addr)
{
    struct sockaddr_in sa;

    gw_assert(octstr_len(addr) == sizeof(sa));
    memcpy(&sa, octstr_get_cstr(addr), sizeof(sa));
    return ntohs(sa.sin_port);
}


Octstr *udp_get_ip(Octstr *addr)
{
    struct sockaddr_in sa;

    gw_assert(octstr_len(addr) == sizeof(sa));
    memcpy(&sa, octstr_get_cstr(addr), sizeof(sa));
    return octstr_create(inet_ntoa(sa.sin_addr));
}


int udp_sendto(int s, Octstr *datagram, Octstr *addr)
{
    struct sockaddr_in sa;

    gw_assert(octstr_len(addr) == sizeof(sa));
    memcpy(&sa, octstr_get_cstr(addr), sizeof(sa));
    if (sendto(s, octstr_get_cstr(datagram), octstr_len(datagram), 0,
               (struct sockaddr *) &sa, (int) sizeof(sa)) == -1) {
        error(errno, "Couldn't send UDP packet");
        return -1;
    }
    return 0;
}


int udp_recvfrom(int s, Octstr **datagram, Octstr **addr)
{
    struct sockaddr_in sa;
    int salen;
    char *buf;
    int bytes;

    buf = gw_malloc(UDP_PACKET_MAX_SIZE);

    salen = sizeof(sa);
    bytes = recvfrom(s, buf, UDP_PACKET_MAX_SIZE, 0,
                     (struct sockaddr *) &sa, &salen);
    if (bytes == -1) {
        if (errno != EAGAIN)
            error(errno, "Couldn't receive UDP packet");
	gw_free(buf);
        return -1;
    }

    *datagram = octstr_create_from_data(buf, bytes);
    *addr = octstr_create_from_data((char *) &sa, salen);
    
    gw_free(buf);

    return 0;
}


Octstr *host_ip(struct sockaddr_in addr)
{
    Octstr *ret;
    mutex_lock(inet_mutex);

    ret = octstr_create(inet_ntoa(addr.sin_addr));
    mutex_unlock(inet_mutex);
    return ret;
}


Octstr *get_official_name(void)
{
    gw_assert(official_name != NULL);
    return official_name;
}


Octstr *get_official_ip(void)
{
    gw_assert(official_ip != NULL);
    return official_ip;
}


static void setup_official_name(void)
{
    struct utsname u;
    struct hostent h;

    gw_assert(official_name == NULL);
    if (uname(&u) == -1)
        panic(0, "uname failed - can't happen, unless Kannel is buggy.");
    if (gw_gethostbyname(&h, u.nodename) == -1) {
        error(0, "Can't find out official hostname for this host, "
              "using `%s' instead.", u.nodename);
        official_name = octstr_create(u.nodename);
	official_ip = octstr_create("127.0.0.1");
    } else {
        official_name = octstr_create(h.h_name);
	official_ip = octstr_create(inet_ntoa(*(struct in_addr *) h.h_addr));
    }
}


void socket_init(void)
{
    inet_mutex = mutex_create();
    setup_official_name();
}

void socket_shutdown(void)
{
    mutex_destroy(inet_mutex);
    octstr_destroy(official_name);
    official_name = NULL;
}


Octstr *gw_netaddr_to_octstr4(unsigned char* src)
{
    return octstr_format("%d.%d.%d.%d",src[0],src[1],src[2],src[3]);
}


#ifdef AF_INET6

#define INET6_OCTETS 16

Octstr *gw_netaddr_to_octstr6(unsigned char* src)
{
    Octstr *address;

    address=octstr_format("%x:%x:%x:%x:%x:%x:%x:%x:%x:%x:%x:%x:%x:%x:%x:%x",
	src[0],src[1],src[2],src[3],src[4],src[5],src[6],src[7],src[8],
	src[9],src[10],src[11],src[12],src[13],src[14],src[15]);
    
    return address;
}
#endif

Octstr *gw_netaddr_to_octstr(int af, void* src)
{
    switch(af){

	case AF_INET:
	return gw_netaddr_to_octstr4((char*)src);

#ifdef AF_INET6
	case AF_INET6:
	return gw_netaddr_to_octstr6((char*)src);
#endif
	default:
	    return NULL;
    } 

    return NULL;
}


