#ifndef HMI_FIRMWARE_H
#define HMI_FIRMWARE_H
#include "contracts/hmi_domain.h"
struct hmi_core;
/** 构建期 Product 引导：运行时启动前调用一次，不做设备 I/O。
 * domain 给运行时独占的 Core，presentation 是发布给 UI 的副本，ui 是 UI 线程自己的副本，三者不得重叠；
 * 不声明信号的 Product 可以全部留空。locale_init 可选：在 UI 线程里、平台 i18n 初始化之后、创建 UI 之前
 * 注册 Product 的语言包。所选 Product 只提供一份实现，不存在运行期加载。 */
typedef struct
{
    hmi_core_storage_t domain, presentation, ui;
    bool (*locale_init)(void);
    /** @brief 运行时线程启动时调用一次，可在打开总线前初始化已绑定的 Core。 */
    bool (*runtime_init)(struct hmi_core *core);
    /** @brief 本机动作应用后通知 Product，不得执行阻塞 I/O。 */
    void (*action_applied)(uint32_t now_ms);
} hmi_firmware_composition_t;
bool hmi_firmware_compose(hmi_firmware_composition_t *out);
#endif
