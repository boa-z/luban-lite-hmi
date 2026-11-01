#ifndef TEST_RTDEVICE_H
#define TEST_RTDEVICE_H
#include "rtthread.h"
typedef struct rt_device
{
    unsigned unused;
} *rt_device_t;
struct rt_can_msg
{
    uint32_t id;
    uint8_t len, ide, rtr;
    int hdr;
    uint8_t data[8];
};
struct rt_can_status
{
    unsigned errcode, dropedrcvpkg, rcverrcnt;
};
#define RT_DEVICE_FLAG_INT_RX 1
#define RT_DEVICE_FLAG_INT_TX 2
#define RT_CAN_CMD_SET_BAUD 10
#define RT_DEVICE_CTRL_SET_INT 11
#define RT_CAN_CMD_GET_STATUS 12
#define RT_CAN_CMD_SET_MODE 13
#define RT_CAN_MODE_LISTEN 1
rt_device_t rt_device_find(const char *);
rt_err_t rt_device_open(rt_device_t, int);
rt_err_t rt_device_close(rt_device_t);
rt_err_t rt_device_control(rt_device_t, int, void *);
rt_err_t rt_device_set_rx_indicate(rt_device_t, rt_err_t (*)(rt_device_t, size_t));
size_t rt_device_read(rt_device_t, int, void *, size_t);
size_t rt_device_write(rt_device_t, int, const void *, size_t);

#endif
