#include <stdio.h>
#include <time.h>
#include <sys/ioctl.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <errno.h>
#include <assert.h>
#include <signal.h>

#include "ff_config.h"
#include "ff_api.h"
#include "ff_epoll.h"

#define MAX_EVENTS 512

struct epoll_event ev;
struct epoll_event events[MAX_EVENTS];

static int message_size = 4096;
static char *buf = NULL;

static int epfd;
static int sockfd;

/* Echo server */
int loop(void *arg)
{
    /* Wait for events to happen */

    int nevents = ff_epoll_wait(epfd, events, MAX_EVENTS, 0);
    int i;

    for (i = 0; i < nevents; ++i)
    {
        /* Handle new connect */
        if (events[i].data.fd == sockfd)
        {
            while (1)
            {
                int nclientfd = ff_accept(sockfd, NULL, NULL);
                if (nclientfd < 0)
                    break;
                int one = 1;
                ff_setsockopt(nclientfd, SOL_TCP, TCP_NODELAY, &one, sizeof(one));
                /* Add to event list */
                ev.data.fd = nclientfd;
                ev.events  = EPOLLIN;
                if (ff_epoll_ctl(epfd, EPOLL_CTL_ADD, nclientfd, &ev) != 0)
                {
                    printf("ff_epoll_ctl failed:%d, %s\n", errno, strerror(errno));
                    break;
                }
            }
        }
        else
        {
            if (events[i].events & EPOLLERR)
            {
                /* Simply close socket */
                ff_epoll_ctl(epfd, EPOLL_CTL_DEL, events[i].data.fd, NULL);
                ff_close(events[i].data.fd);
            }
            else if (events[i].events & EPOLLIN)
            {
                size_t readlen = ff_read(events[i].data.fd, buf, message_size);
                if (readlen > 0)
                    ff_write(events[i].data.fd, buf, readlen);
                else
                {
                    ff_epoll_ctl(epfd, EPOLL_CTL_DEL, events[i].data.fd, NULL);
                    ff_close(events[i].data.fd);
                }
            }
            else
            {
                printf("unknown event: %8.8X\n", events[i].events);
            }
        }
    }
}

static uint64_t sum_us = 0, count_us = 0;
static int to_write = 0;
static int to_read = 0;
static int connected = 0;
static struct timespec connect_time;
static struct timespec start, end;

void sigint_handler(int signum)
{
    struct timespec stop_time;
    clock_gettime(CLOCK_REALTIME, &stop_time);
    double duration_sec = ((stop_time.tv_nsec - connect_time.tv_nsec)/1000000000.0 + 1.0*(stop_time.tv_sec - connect_time.tv_sec));
    uint64_t mps = count_us/duration_sec;
    printf(
        "%lu messages in %.2f seconds (%lu mps), %lu us avg ping-pong\n", count_us,
        duration_sec, mps, sum_us/(count_us == 0 ? 1 : count_us)
    );
    exit(0);
}

int loop_client(void *arg)
{
    ssize_t len = 0;
    int nevents = ff_epoll_wait(epfd, events, MAX_EVENTS, 0);
    int i;
    for (i = 0; i < nevents; ++i)
    {
        if (events[i].events & (EPOLLHUP|EPOLLERR))
        {
            /* Simply close socket */
            ff_epoll_ctl(epfd, EPOLL_CTL_DEL, events[i].data.fd, NULL);
            ff_close(events[i].data.fd);
            sigint_handler(SIGINT);
            exit(0);
        }
        else if (events[i].events & EPOLLIN)
        {
            /* Read everything back */
            if (to_read > 0)
            {
                len = ff_read(events[i].data.fd, buf, message_size);
                if (len > 0)
                    to_read = to_read > len ? to_read-len : 0;
                if (to_read == 0)
                {
                    /* Calculate latency */
                    clock_gettime(CLOCK_REALTIME, &end);
                    sum_us += (end.tv_nsec - start.tv_nsec)/1000 + (end.tv_sec - start.tv_sec)*1000000;
                    count_us++;
                    /* Send next request */
                    start = end;
                    memset(buf, 0xab, message_size);
                    to_write = message_size;
                    len = ff_write(events[i].data.fd, buf, to_write);
                    if (len > 0)
                        to_write -= len;
                    if (to_write == 0)
                        to_read = message_size;
                }
            }
        }
        else if (events[i].events & EPOLLOUT)
        {
            /* Connected or buffer available */
            if (!connected)
            {
                connected = 1;
                printf("Connected, starting ping-pong, press Ctrl-C to interrupt\n");
                int one = 1;
                ff_setsockopt(events[i].data.fd, SOL_TCP, TCP_NODELAY, &one, sizeof(one));
                clock_gettime(CLOCK_REALTIME, &connect_time);
                start = connect_time;
                memset(buf, 0xab, message_size);
                to_write = message_size;
            }
            if (to_write > 0)
            {
                len = ff_write(events[i].data.fd, buf, to_write);
                if (len > 0)
                    to_write -= len;
                if (to_write == 0)
                    to_read = message_size;
            }
        }
        else
        {
            printf("unknown event: %8.8X\n", events[i].events);
        }
    }
    return 0;
}

int main(int argc, char *argv[])
{
    char *server = NULL;
    char **f_argv = malloc(sizeof(char*) * (argc+1));
    int f_argidx = 1;
    f_argv[0] = argv[0];
    for (int i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "--size"))
        {
            if (i < argc-1)
                message_size = atoi(argv[++i]);
            if (message_size < 4)
            {
                printf("invalid argument for --size\n");
                exit(1);
            }
        }
        else if (argv[i][0] == '-')
        {
            f_argv[f_argidx++] = argv[i++];
            f_argv[f_argidx++] = argv[i];
        }
        else
        {
            server = argv[i];
            break;
        }
    }
    f_argv[f_argidx] = NULL;

    buf = malloc(message_size);
    if (!buf)
    {
        printf("failed to allocate buffer\n");
        exit(1);
    }

    ff_init(f_argidx, f_argv);

    sockfd = ff_socket(AF_INET, SOCK_STREAM, 0);
    printf("sockfd:%d\n", sockfd);
    if (sockfd < 0)
    {
        printf("ff_socket failed\n");
        exit(1);
    }

    int on = 1;
    ff_ioctl(sockfd, FIONBIO, &on);

    if (server)
    {
        printf("Client mode, message size %d, connecting to %s\n", message_size, server);

        struct sockaddr_in my_addr;
        bzero(&my_addr, sizeof(my_addr));
        my_addr.sin_family = AF_INET;
        my_addr.sin_port = htons(7777);
        my_addr.sin_addr.s_addr = inet_addr(server);

        int ret = ff_connect(sockfd, (struct linux_sockaddr *)&my_addr, sizeof(my_addr));
        if (ret < 0 && errno != EINPROGRESS)
        {
            printf("ff_connect failed: %d (%s)\n", errno, strerror(errno));
            exit(1);
        }
    }
    else
    {
        struct sockaddr_in my_addr;
        bzero(&my_addr, sizeof(my_addr));
        my_addr.sin_family = AF_INET;
        my_addr.sin_port = htons(7777);
        my_addr.sin_addr.s_addr = htonl(INADDR_ANY);

        int ret = ff_bind(sockfd, (struct linux_sockaddr *)&my_addr, sizeof(my_addr));
        if (ret < 0)
        {
            printf("ff_bind failed\n");
            exit(1);
        }

        ret = ff_listen(sockfd, MAX_EVENTS);
        if (ret < 0)
        {
            printf("ff_listen failed\n");
            exit(1);
        }
    }

    assert((epfd = ff_epoll_create(0)) > 0);
    ev.data.fd = sockfd;
    ev.events = (server ? EPOLLOUT : 0) | EPOLLIN|EPOLLHUP;
    ff_epoll_ctl(epfd, EPOLL_CTL_ADD, sockfd, &ev);
    if (server)
        signal(SIGINT, sigint_handler);
    ff_run(server ? loop_client : loop, NULL);
    return 0;
}
