#define _DEFAULT_SOURCE

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <sys/signalfd.h>
#include <linux/netlink.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h> 
#include <syslog.h>
#include <errno.h>

#include "ludev.h"


int ludev_epoll_event_netlinkfd (
    struct ludev_s * ludev,
    struct epoll_event * event
)
{
    int ret = 0;
    int bytes_read = 0;
    char buf[8192];


    bytes_read = read(ludev->netlinkfd, buf, 8192);
    if (-1 == bytes_read) {
        syslog(LOG_ERR, "%s:%d:%s: read: %s", __FILE__, __LINE__, __func__, strerror(errno));
        return -1;
    }
    if (0 == bytes_read) {
        syslog(LOG_ERR, "%s:%d:%s: read 0 bytes", __FILE__, __LINE__, __func__);
        return -1;
    }
     
    syslog(LOG_INFO, "%s:%d:%s: len=%d, msg=%.*s", __FILE__, __LINE__, __func__, bytes_read, bytes_read, buf);

    ret = epoll_ctl(
        ludev->epollfd,
        EPOLL_CTL_MOD,
        event->data.fd,
        &(struct epoll_event){
            .events = EPOLLIN | EPOLLERR | EPOLLHUP | EPOLLONESHOT,
            .data = event->data
        }
    );
    if (-1 == ret) {
        syslog(LOG_ERR, "%s:%d:%s: epoll_ctl: %s", __FILE__, __LINE__, __func__, strerror(errno));
        return -1;
    }

    return 0;
}


static int ludev_epoll_event_dispatch (
    struct ludev_s * ludev,
    struct epoll_event * event
)
{
    if (event->data.fd == ludev->netlinkfd)
        return ludev_epoll_event_netlinkfd(ludev, event);

    syslog(LOG_ERR, "%s:%d:%s: No match on epoll event.", __FILE__, __LINE__, __func__);
    return -1;
}


static int ludev_epoll_handle_events (
    struct ludev_s * ludev,
    struct epoll_event epoll_events[8],
    int epoll_events_len
)
{
    int ret = 0;
    for (int i = 0; i < epoll_events_len; i++) {
        // (snippet: epdisp)
        ret = ludev_epoll_event_dispatch(ludev, &epoll_events[i]);
        if (0 != ret) {
            return ret;
        }
    }
    return 0;
}


int ludev_loop (
    struct ludev_s * ludev
)
{

    int ret = 0;

    int epoll_events_len = 0;
    struct epoll_event epoll_events[8];
    while (1) {
        epoll_events_len = epoll_wait(
            /* epollfd = */ ludev->epollfd,
            /* &events = */ epoll_events,
            /* events_len = */ 8,
            /* timeout = */ -1
        );

        // got interrupted, just try again.
        if (-1 == epoll_events_len && EINTR == errno) {
            continue;
        }

        if (-1 == epoll_events_len) {
            syslog(LOG_ERR, "%s:%d:%s: epoll_wait: %s", __FILE__, __LINE__, __func__, strerror(errno));
            return -1;
        }

        if (0 == epoll_events_len) {
            syslog(LOG_ERR, "%s:%d:%s: epoll_wait returned 0 events", __FILE__, __LINE__, __func__);
            return -1;
        }

        // dispatch on event
        // (snippet: epev)
        ret = ludev_epoll_handle_events(ludev, epoll_events, epoll_events_len);
        if (-1 == ret) {
            syslog(LOG_ERR, "%s:%d:%s: ludev_epoll_handle_events returned -1", __FILE__, __LINE__, __func__);
            return -1;
        }
    }
    

    return 0;
}


int ludev_init (
    struct ludev_s * ludev
)
{
    ludev->sentinel = LUDEV_SENTINEL;

    // Create the epoll instance
    ludev->epollfd = epoll_create1(EPOLL_CLOEXEC);
    if (-1 == ludev->epollfd) {
        syslog(LOG_ERR, "%s:%d:%s: epoll_create1: %s", __FILE__, __LINE__, __func__, strerror(errno));
        return -1;
    }

    return 0;
}


int ludev_netlink_open (
    struct ludev_s * ludev
)
{

    int ret = 0;

    if (NULL == ludev) {
        syslog(LOG_ERR, "%s:%d:%s: ludev pointer is NULL", __FILE__, __LINE__, __func__);
        return -1;
    }
    if (LUDEV_SENTINEL != ludev->sentinel) {
        syslog(LOG_ERR, "%s:%d:%s: sentinel is wrong", __FILE__, __LINE__, __func__);
        return -1;
    }

    // open a netlink socket
    ludev->netlinkfd = socket(
        /* domain = */ AF_NETLINK, // AF_UNSPEC, AF_INET, AF_INET6, AF_LOCAL
        /* type = */ SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK,
        /* protocol = */ NETLINK_KOBJECT_UEVENT
    );
    if (-1 == ludev->netlinkfd) {
        syslog(LOG_ERR, "%s:%d:%s: socket: %s", __FILE__, __LINE__, __func__, strerror(errno));
        return -1;
    }

    // bind a netlink source address
    // bind a source address to the socket
    ret = bind(
        /* fd = */ ludev->netlinkfd,
        /* sockaddr = */ (struct sockaddr*)&(struct sockaddr_nl){
            .nl_family = AF_NETLINK,
            .nl_pid = getpid(),
            .nl_groups = -1
        },
        /* sockaddr_len = */ sizeof(struct sockaddr_nl)
    );
    if (-1 == ret) {
        syslog(LOG_ERR, "%s:%d:%s: bind: %s", __FILE__, __LINE__, __func__, strerror(errno));
        return -1;
    }

    // Add the fd to epoll
    ret = epoll_ctl(
        ludev->epollfd,
        EPOLL_CTL_ADD,
        ludev->netlinkfd,
        &(struct epoll_event){
            .events = EPOLLIN | EPOLLERR | EPOLLHUP | EPOLLONESHOT,
            .data = {
                .fd = ludev->netlinkfd
            }
        }
    );
    if (-1 == ret) {
        syslog(LOG_ERR, "%s:%d:%s: epoll_ctl: %s", __FILE__, __LINE__, __func__, strerror(errno));
        return -1;
    }

    return 0;
}


int main (
    int argc,
    char const* argv[]
)
{
    int ret = 0;
    struct ludev_s ludev = {0};

    openlog("ludev", LOG_CONS | LOG_PID, LOG_USER); 

    ret = ludev_init(&ludev);
    if (-1 == ret) {
        syslog(LOG_ERR, "%s:%d:%s: ludev_init returned -1", __FILE__, __LINE__, __func__);
        return -1;
    }

    ret = ludev_netlink_open(&ludev);
    if (-1 == ret) {
        syslog(LOG_ERR, "%s:%d:%s: ludev_netlink_open returned -1", __FILE__, __LINE__, __func__);
    }

    ret = ludev_loop(&ludev);
    if (-1 == ret) {
        syslog(LOG_ERR, "%s:%d:%s: ludev_loop returned -1", __FILE__, __LINE__, __func__);
        return -1;
    }

    return 0;
    (void)argc;
    (void)argv;
}
