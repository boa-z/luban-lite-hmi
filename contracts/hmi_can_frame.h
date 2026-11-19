#ifndef HMI_CAN_FRAME_H
#define HMI_CAN_FRAME_H
#include <stdbool.h>
#include <stdint.h>
typedef enum
{
    HMI_BUS_CAN0,
    HMI_BUS_CAN1,
    HMI_BUS_COUNT
} hmi_bus_role_t;
typedef struct
{
    hmi_bus_role_t bus;
    uint32_t id;
    uint32_t timestamp_ms;
    bool extended;
    bool remote;
    uint8_t size;
    uint8_t data[8];
} hmi_can_frame_t;
#define HMI_CAN_RX_ID_LIMIT 8u
typedef struct
{
    uint32_t id, mask;
    bool extended;
} hmi_can_rx_id_t;
typedef struct
{
    hmi_can_rx_id_t entries[HMI_CAN_RX_ID_LIMIT];
    unsigned count;
} hmi_can_rx_ids_t;
typedef uint16_t hmi_frame_route_owner_t;
/* 一条总线/帧格式的后备路由，用于原始报文监视；精确 ID 路由优先。 */
#define HMI_CAN_ID_ANY UINT32_MAX
typedef struct
{
    hmi_bus_role_t bus;
    uint32_t id;
    bool extended;
    hmi_frame_route_owner_t owner;
} hmi_frame_route_t;
#endif
