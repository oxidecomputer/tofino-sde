#define _GNU_SOURCE

#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/ip.h>
#include <arpa/inet.h>
#include <dlfcn.h>

static int (*real_socket)(int, int, int) = NULL;
static int (*real_bind)(int, const struct sockaddr *, socklen_t);
static int (*real_accept)(int, struct sockaddr *, socklen_t *);
static int (*real_connect)(int, const struct sockaddr *, socklen_t);

static int inbound_fd = -1;
static struct in_addr inbound_address = { s_addr: 0 };

static void link_init() {
	if (real_socket == NULL) {
		real_socket = dlsym(RTLD_NEXT, "socket");
		real_bind = dlsym(RTLD_NEXT, "bind");
		real_accept = dlsym(RTLD_NEXT, "accept");
		real_connect = dlsym(RTLD_NEXT, "connect");
	}
}

int
socket(int domain, int type, int protocol) {
	link_init();

	return (real_socket(domain, type, protocol));
}

int
bind(int fd, const struct sockaddr *addr, socklen_t addrlen) {
	link_init();

	if (addr->sa_family == AF_INET) {
		struct sockaddr_in *in = (struct sockaddr_in *)addr;
		if (ntohs(in->sin_port) == 8001) {
			inbound_fd = fd;
		}
	}
	return (real_bind(fd, addr, addrlen));
}

int
accept(int fd, struct sockaddr *addr, socklen_t *addrlen) {
	link_init();

	int rval = real_accept(fd, addr, addrlen);
	if (rval >= 0 && fd == inbound_fd && addr->sa_family == AF_INET) {
		struct sockaddr_in *in = (struct sockaddr_in *)addr;
		inbound_address.s_addr = in->sin_addr.s_addr;
	}

	return (rval);
}

int
connect(int fd, const struct sockaddr *addr, socklen_t addrlen) {
	link_init();

	struct sockaddr copy = *addr;
	if (addr->sa_family == AF_INET) {
		struct sockaddr_in *in = (struct sockaddr_in *)&copy;
		if (ntohs(in->sin_port) == 8002) {
			in->sin_addr = inbound_address;
		}
	}
	return (real_connect(fd, &copy, addrlen));
}
