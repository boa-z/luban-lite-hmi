#ifndef HMI_PROTOCOL_H
#define HMI_PROTOCOL_H
#include "contracts/hmi_can_frame.h"
#include "contracts/hmi_domain.h"
struct hmi_diagnostics;
/** @brief 同步提交 Domain 更新；返回是否接受更新。 */
typedef bool (*hmi_update_sink_t)(void *context, const hmi_update_t *update);
/** @brief 排队帧的发送约束；结构复制入队，context 必须保持到队列排空。
 * valid 仅在 TX worker 交给驱动前调用，不得执行 I/O、修改协议栈或重新提交发送。
 */
typedef struct
{
    bool priority, expires;
    uint32_t expires_ms, tag;
    bool (*valid)(void *context, uint32_t tag);
    void *context;
} hmi_can_tx_options_t;
typedef enum
{
    HMI_CAN_CONFIG_FAILED,
    HMI_CAN_CONFIG_PENDING,
    HMI_CAN_CONFIG_OK
} hmi_can_config_result_t;
/** @brief 平台无关的 CAN 发送端口；true 只表示帧已进入该总线的发送队列，不代表已上线或被应答。 */
typedef struct
{
    bool (*send)(void *context, const hmi_can_frame_t *frame);
    void *context;
    /** @brief 可选单向静默请求；返回 true 仅表示接收请求。平台拒绝后续发送并丢弃排队帧，
        当前驱动写返回后通过原生控制器接口停止总线参与；重启前不恢复。 */
    bool (*stop)(void *context, hmi_bus_role_t bus);
    /** @brief 可选优先队列、期限与事务检查；拒绝时不入队，已交给驱动的帧不可撤回。 */
    bool (*send_checked)(void *context, const hmi_can_frame_t *frame, const hmi_can_tx_options_t *options);
    /** @brief 运行时反复查询同一速率请求，TX worker 串行执行设备操作；返回最终结果后请求结束。 */
    hmi_can_config_result_t (*configure)(void *context, hmi_bus_role_t bus, uint32_t bitrate);
} hmi_can_tx_port_t;
/** @brief 发给设备协议的命令，与 UI 的本地设置动作完全分离。单位与含义由产品协议定义。 */
typedef struct
{
    uint16_t id;
    float value;
    uint32_t argument;
} hmi_command_t;
/**
 * @brief App 的命令端口。
 *
 * App 与协议适配器运行在同一个运行时线程：send 同步按 Product 的 command_route 找到目标适配器并调用其
 * command()，返回适配器是否接受。需要远端应答、超时或重试的事务由适配器自己在 on_frame/process 里推进，
 * 结果以信号写入 Domain。
 */
typedef struct
{
    void *context;
    bool (*send)(void *context, const hmi_command_t *command);
} hmi_command_port_t;
/**
 * @brief 适配器可用的协议服务，只在回调期间有效。
 *
 * update 写入持续 Domain 信号；tx 为板级发送端口；diagnostics 为可选观测实例。
 */
typedef struct
{
    hmi_update_sink_t update;
    void *update_context;
    const hmi_can_tx_port_t *tx;
    struct hmi_diagnostics *diagnostics;
} hmi_protocol_services_t;
/**
 * @brief 有状态协议适配器（SDO 客户端/服务端、CANopenNode 包装、私有握手等）。
 *
 * 回调都在运行时线程串行执行，不得阻塞、不得触碰 LVGL。process 每轮调用一次（无帧时也调用），
 * 可用于推进超时或轮询非 CAN 的 I/O；reset 在会话代次切换时调用。无状态的 DBC 解码使用 hmi_decode_fn_t。
 */
typedef struct
{
    void *context;
    bool (*on_frame)(void *context, const hmi_can_frame_t *frame, const hmi_protocol_services_t *services);
    bool (*process)(void *context, uint32_t now_ms, const hmi_protocol_services_t *services);
    bool (*command)(void *context, const hmi_command_t *command, const hmi_protocol_services_t *services);
    void (*reset)(void *context);
} hmi_protocol_adapter_t;
#endif
