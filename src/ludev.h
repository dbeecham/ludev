#pragma once

#define LUDEV_SENTINEL 8090

struct ludev_s {
    int sentinel;
    int epollfd;
    int netlinkfd;
};
