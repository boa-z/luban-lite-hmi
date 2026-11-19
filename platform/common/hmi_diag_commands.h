#ifndef HMI_DIAG_COMMANDS_H
#define HMI_DIAG_COMMANDS_H
#include "diagnostics/hmi_diagnostics.h"
/** @brief MSH 已分词后的查询类型；不实现 UART 或交互 Shell。 */
typedef enum
{
    HMI_QUERY_INFO,
    HMI_QUERY_DIAG,
    HMI_QUERY_RUNTIME,
    HMI_QUERY_CAN,
    HMI_QUERY_DOMAIN,
    HMI_QUERY_SIGNAL,
    HMI_QUERY_TOUCH,
    HMI_QUERY_TRACE,
    HMI_QUERY_TRACE_DUMP,
    HMI_QUERY_TRACE_CLEAR
} hmi_diag_query_kind_t;
/** @brief key 借用 argv 生命周期；bus=-1 表示全部总线。 */
typedef struct
{
    hmi_diag_query_kind_t kind;
    int bus;
    const char *key;
} hmi_diag_query_t;
/** @brief 一次查询使用的副本；render 期间调用者持有全部缓冲，不能指向正在变化的私有对象。 */
typedef struct
{
    const hmi_diag_snapshot_t *snapshot;
    const hmi_build_info_t *build;
    const hmi_diag_signal_t *signal;
    const hmi_trace_entry_t *entries;
    size_t entry_count;
    uint32_t cleared;
} hmi_diag_view_t;
/** @brief 同步输出一行；文本只在回调期间有效，仅在线程查询上下文使用。 */
typedef void (*hmi_diag_line_fn)(void *context, const char *line);
/** @brief 校验原生 MSH argv；不修改状态，未知或多余参数返回 false。 */
bool hmi_diag_query(int argc, const char *const *argv, hmi_diag_query_t *out);
/** @brief 格式化已复制的查询结果；不控制任何设备，输出阻塞行为由前端决定。 */
void hmi_diag_render(const hmi_diag_query_t *query, const hmi_diag_view_t *view, hmi_diag_line_fn emit,
                     void *context);
/** @brief 将稳定的模块、事件编号映射为显示名称，未知编号保留数字可供排障。 */
const char *hmi_trace_module_name(uint16_t module);
/** @brief 返回静态事件名；无分配，调用者无需释放。 */
const char *hmi_trace_event_name(uint16_t module, uint16_t event);
#endif
