#include "cli.h"
#include "handler.h"
#include "statistics.h"
#include "third/nfqueue-mnl.h"
#include "util.h"

#ifdef UA2F_ENABLE_UCI
#include "config.h"
#endif

#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#pragma clang diagnostic push
#pragma ide diagnostic ignored "EndlessLoop"

volatile int should_exit = false;

void signal_handler(const int signum) {
    syslog(LOG_ERR, "Signal %s received, exiting...", strsignal(signum));
    should_exit = true;
}

int parse_packet(const struct nf_queue *queue, struct nf_buffer *buf) {
    struct nf_packet packet[1] = {0};

    while (!should_exit) {
        const __auto_type status = nfqueue_next(buf, packet);
        switch (status) {
        case IO_READY:
            handle_packet(queue, packet);
            break;
        default:
            return status;
        }
    }

    return IO_ERROR;
}

int read_buffer(struct nf_queue *queue, struct nf_buffer *buf) {
    const __auto_type buf_status = nfqueue_receive(queue, buf, 0);
    switch (buf_status) {
    case IO_READY:
        return parse_packet(queue, buf);
    default:
        return buf_status;
    }
}

bool retry_disable_conntrack(struct nf_queue *queue) {
    nfqueue_close(queue);

    syslog(LOG_INFO, "Retrying to disable conntrack");
    const __auto_type ret = nfqueue_open(queue, QUEUE_NUM, 0, true);
    if (!ret) {
        syslog(LOG_ERR, "Failed to open nfqueue with conntrack disabled");
        return false;
    }
    return true;
}

void main_loop(struct nf_queue *queue) {
    struct nf_buffer buf[1] = {0};
    bool retried = false;

    while (!should_exit) {
        if (read_buffer(queue, buf) == IO_ERROR) {
            if (!retried) {
                retried = true;
                if (!retry_disable_conntrack(queue)) {
                    break;
                }
            } else {
                break;
            }
        }
    }

    free(buf->data);
}

int main(const int argc, char *argv[]) {
    openlog("UA2F", LOG_PID, LOG_SYSLOG);

#ifdef UA2F_ENABLE_UCI
    load_config();
#else
    syslog(LOG_INFO, "uci support is disabled");
#endif

    try_print_info(argc, argv);

    require_root();

    init_statistics();
    init_handler();

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGQUIT, signal_handler);
    signal(SIGSEGV, signal_handler);
    signal(SIGABRT, signal_handler);

    struct nf_queue queue[1] = {0};

    const __auto_type ret = nfqueue_open(queue, QUEUE_NUM, 0, false);
    if (!ret) {
        syslog(LOG_ERR, "Failed to open nfqueue");
        return EXIT_FAILURE;
    }

    main_loop(queue);

    nfqueue_close(queue);

    return EXIT_SUCCESS;
}

#pragma clang diagnostic pop
