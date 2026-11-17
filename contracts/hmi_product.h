#ifndef HMI_PRODUCT_H
#define HMI_PRODUCT_H
#include "contracts/hmi_can_frame.h"
#include "contracts/hmi_domain.h"
#include "contracts/hmi_periodic.h"
#include "contracts/hmi_protocol.h"
#include "contracts/hmi_update_view.h"
#include <stddef.h>
/** @brief 无状态解码：把一帧写成若干 Domain 更新。 */
typedef bool (*hmi_decode_fn_t)(const hmi_can_frame_t *frame, hmi_update_sink_t sink, void *context);
/** @brief 一个协议 owner：无状态 decode 与有状态 adapter 二选一（adapter 优先）。 */
typedef struct
{
    hmi_frame_route_owner_t owner;
    hmi_decode_fn_t decode;
    const hmi_protocol_adapter_t *adapter;
} hmi_protocol_binding_t;
typedef struct
{
    const hmi_protocol_binding_t *bindings;
    size_t count;
} hmi_protocol_profile_t;
/** @brief 业务命令到协议 owner 的路由；返回 false 表示未知命令。路由本身不得发送。 */
typedef bool (*hmi_command_route_fn_t)(void *context, const hmi_command_t *command,
                                         hmi_frame_route_owner_t *owner_out);
typedef struct
{
    const hmi_frame_route_t *entries;
    size_t count;
} hmi_route_profile_t;
typedef struct
{
    void *(*create)(void *parent, const hmi_ui_actions_t *actions);
    /** @brief `elapsed_ms` 是自 UI 创建起累计的运行毫秒（单调时基，允许 32 位回绕），
        闪烁相位与开机停留都按它算；传帧间隔会让这类计时永远走不完。 */
    void (*present)(void *ui, const hmi_snapshot_t *snapshot, uint32_t elapsed_ms);
    void (*destroy)(void *ui);
    /** @brief 可选只读升级展示，由 UI 线程调用，不触发下载或激活。 */
    void (*present_update)(void *ui, const hmi_update_view_t *view, hmi_language_t language);
} hmi_ui_factory_t;
/** @brief Product 本机设置记录策略；未绑定或 enabled=false 时平台不启用持久化。
    always_writable=true 时介质状态不阻止保存：读不出有效记录（损坏、不兼容、读错误）就用当前设置覆盖，
    写失败后下一轮重扫再写，不停在只读状态。代价是不兼容的旧记录会被覆盖，总线长期不通时会持续重试。
    默认 false 保持“未知内容不覆盖、失败后需显式重试”的保守口径。 */
typedef struct
{
    bool enabled;
    uint16_t record_type, product_namespace, schema;
    uint32_t debounce_ms, max_delay_ms;
    bool always_writable;
} hmi_storage_profile_t;
/**
 * @brief 一个构建目标唯一的 Product 描述。
 *
 * prepare/start 在启动线程、运行时启动之前执行；UI 回调在 UI 线程执行。
 * 其余回调在同一个运行时线程里串行执行，不得阻塞或执行耗时设备 I/O
 *（介质写入、Flash 由平台的独立 worker 完成）。
 */
typedef struct
{
    const char *id;
    const hmi_catalog_t *catalog;
    const hmi_ui_factory_t *ui;
    /** @brief 可选启动默认设置；之后由持久化恢复覆盖。 */
    const hmi_initial_settings_t *initial_settings;

    /** @brief 启动时恢复 Product 状态和初始设置，在 Core 初始化之前调用一次；不得启动后台业务。 */
    bool (*prepare)(void);
    /** @brief 平台设备就绪后、UI 创建和协议推进前调用一次；启动 Product 的外设服务，不访问 LVGL。 */
    bool (*start)(void);

    /** @brief 可选：该总线本次开机使用的速率；返回 0 时沿用 configured（统一的 can_rate）。
        在打开总线前按总线调用，用于各总线独立默认波特率；未绑定或返回 0 时全部沿用 configured。 */
    uint32_t (*bus_bitrate)(hmi_bus_role_t bus, uint32_t configured);

    /* 可选协议：接收路由、owner 绑定、命令路由与周期发送。只做本地业务的 Product 全部留空，
       此时不打开任何 CAN 总线（CAN 升级端点除外）。 */
    const hmi_protocol_profile_t *protocols;
    const hmi_route_profile_t *routes;
    hmi_command_route_fn_t command_route;
    void *command_route_context;
    const hmi_periodic_frame_t *periodic;
    size_t periodic_count;

    /* 运行时线程每轮的 Product 业务：先 app_run（可经命令端口驱动协议），信号超时处理后再 app_tick。 */
    void (*app_run)(uint32_t now_ms, const hmi_command_port_t *commands);
    /** @brief 本机周期计算；snapshot 仅在回调内借用，变更须递增 revision。升级维护期间仍调用。 */
    void (*app_tick)(hmi_snapshot_t *snapshot, uint32_t now_ms);
    /** @brief 会话代次切换（启动、进入/退出升级维护）时先复位 Product 工作流。 */
    void (*app_reset)(uint32_t generation);
    /** @brief HMI_ACTION_PRODUCT 动作入口；变更快照须递增 revision，返回是否应用。 */
    bool (*local_action)(hmi_snapshot_t *snapshot, const hmi_action_t *action, uint32_t now_ms);

    /* 持久化与升级。 */
    const hmi_storage_profile_t *storage;
    /** @brief 显式允许同一 schema 的旧记录缺少新增参数；已有 ID/含义/范围不得改变。 */
    bool allow_parameter_extension;
    /** @brief 可选升级身份、容量与 UDS 寻址；未绑定时不启用。 */
    const struct hmi_update_policy *update;
    /** @brief 升级准入：App 给出快照与维护请求，Product 决定是否允许下载。 */
    bool (*update_admission)(const hmi_snapshot_t *snapshot, bool maintenance);
    /** @brief 升级维护期间暂停正常协议业务、非关键周期帧与 Product 界面。 */
    bool update_exclusive;
} hmi_product_t;
const hmi_product_t *hmi_product_get(void);
#endif
