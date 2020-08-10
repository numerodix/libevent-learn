#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/signal.h>
#include <sys/socket.h>
#include <sys/types.h>

#include "event2/event.h"

struct timeval TIMEOUT_S = {30, 0};

struct conn_state_t {
    struct sockaddr_in sin;
    struct event* event_read_socket;
};

struct conn_state_t *alloc_conn_state() {
    struct conn_state_t *state = calloc(sizeof(struct conn_state_t), 1);
    if (state == NULL) {
        perror("Failed to calloc conn_state_t\n");
    }
    return state;
}

const char* format_address(uint32_t addr) {
    addr = htonl(addr);
    int first = (addr & 0xff000000) >> 24;
    int second = (addr & 0x00ff0000) >> 16;
    int third = (addr & 0x0000ff00) >> 8;
    int fourth = (addr & 0x000000ff);

    char *buf = malloc(15);
    if (buf == NULL) {
        perror("Failed to malloc address buffer\n");
        return NULL;
    }

    sprintf(buf, "%d.%d.%d.%d", first, second, third, fourth);
    return buf;
}

void cb_timer(evutil_socket_t fd, short what, void *arg) {
    struct timeval *tv = arg;
    printf("Timer fired after %ld seconds!\n", tv->tv_sec);
}

void cb_sigint(evutil_socket_t fd, short what, void *arg) {
    struct event_base *base = arg;

    printf("Got SIGINT - shutting down the event loop!\n");
    event_base_loopexit(base, NULL);
}

void cb_read_socket(evutil_socket_t fd, short what, void *arg) {
    struct conn_state_t *state = arg;

    const char* addr = format_address(state->sin.sin_addr.s_addr);
    int port = state->sin.sin_port;

    char buf[1024];
    ssize_t num_read = recv(fd, buf, sizeof(buf), 0);

    // There was actually nothing to read. We assume this means the peer
    // disconnected so we remove the read event.
    if (num_read < 1) {
        printf("Peer %s:%d disconnected\n", addr, port);
        event_del(state->event_read_socket);
        return;
    }

    printf("Read %ld bytes from peer: %s:%d: ", num_read, addr, port);
    for (ssize_t i = 0; i < num_read; ++i) {
        printf("%d ", buf[i]);
    }
    printf("\n");
}

void cb_accept_conn(evutil_socket_t listener, short what, void *arg) {
    printf("Got incoming connection!\n");

    struct event_base *base = arg;

    // Accept the connection and add a new event that waits until we can read
    // from the socket
    struct conn_state_t *state = alloc_conn_state();
    socklen_t slen = sizeof(state->sin);

    int fd = accept(listener, (struct sockaddr*) &state->sin, &slen);
    if (fd < 0) {
        perror("Failed to accept connection\n");
        return;
    }
    evutil_make_socket_nonblocking(fd);

    state->event_read_socket = event_new(base, fd, EV_READ | EV_PERSIST,
                                         cb_read_socket, (void*) state);
    if (event_add(state->event_read_socket, NULL)) {
        perror("Failed to add read socket event\n");
    }
}

int main(int argc, char *argv[]) {
    struct event_base *base = event_base_new();
    if (!base) {
        perror("Failed to create event base\n");
        return 1;
    }

    // Add event that listens for timeouts.
    // Use stdin as fd in lieu of actual meaningful file/socket (because this is
    // just a timer so doesn't act on any IO anyway)
    evutil_socket_t fd_timer = 0;
    struct event *event_timer = event_new(base, fd_timer, EV_TIMEOUT | EV_PERSIST,
                                          cb_timer, &TIMEOUT_S);
    if (event_add(event_timer, &TIMEOUT_S)) {
        perror("Failed to add timeout event\n");
        return 1;
    }

    // Add event that listens for signal SIGINT
    struct event *event_sigint = evsignal_new(base, SIGINT, cb_sigint, (void*) base);
    if (evsignal_add(event_sigint, NULL)) {
        perror("Failed to add SIGINT event\n");
        return 1;
    }

    // Add event that listens on a socket
    evutil_socket_t listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener == -1) {
        perror("Failed to create listener socket\n");
        return 1;
    }
    evutil_make_socket_nonblocking(listener);

    int enabled = 1;
    if (setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) < 0) {
        perror("Failed to set SO_REUSEADDR socket option\n");
        return 1;
    }

    struct sockaddr_in sin;
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = 0; // listen on all interfaces
    sin.sin_port = htons(8888); // listen on :8888

    if (bind(listener, (struct sockaddr*) &sin, sizeof(sin)) < 0) {
        perror("Failed to bind to :8888\n");
        return 1;
    }

    int backlog_sz = 1;
    if (listen(listener, backlog_sz) < 0) {
        perror("Failed to listen on :8888\n");
        return 1;
    }

    struct event *event_listener = event_new(base, listener, EV_READ | EV_PERSIST,
                                             cb_accept_conn, (void *)base);
    if (event_add(event_listener, NULL)) {
        perror("Failed to add socket listener event\n");
        return 1;
    }

    printf(
        "Listening for events:\n"
        "- Connections on :8888 - use 'nc localhost 8888', type something and hit Enter\n"
        "- Timer every 30s\n"
        "- SIGINT (Ctrl+C in terminal)\n"
    );

    // run loop forever as long as there are any events to listen for
    event_base_dispatch(base);

    return 0;
}