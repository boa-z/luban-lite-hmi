/* SPDX-License-Identifier: Apache-2.0 */
/* SDK 的 LVGL 线程独占 LVGL 与 UI；运行时线程独占 Domain 与协议。 */
#define LOG_TAG "hmi.boot"
#define LOG_LVL LOG_LVL_INFO
#include "contracts/hmi_firmware.h"
#include "contracts/hmi_product.h"
#include "hmi_build_identity.h"
#include "hmi_display_version.h"
#include "platform/rtthread/debug/hmi_debug_console.h"
#include "platform/rtthread/hmi_board_port.h"
#include "platform/rtthread/hmi_execution_port.h"
#include "ui/common/i18n/hmi_i18n_runtime.h"
#include <lvgl.h>
#include <rtthread.h>
#include <ulog.h>
#define HMI_STRING_IMPL(x) #x
#define HMI_STRING(x) HMI_STRING_IMPL(x)
#define HMI_UI_VERSION                                                                                       \
    HMI_STRING(LVGL_VERSION_MAJOR) "." HMI_STRING(LVGL_VERSION_MINOR) "." HMI_STRING(LVGL_VERSION_PATCH)

static hmi_core_t core;
static hmi_diagnostics_t diagnostics;
static hmi_build_info_t identity;
/* Product 和组合配置由主线程发布，呈现状态由 LVGL 线程管理。 */
static struct
{
    const hmi_product_t *product;
    hmi_firmware_composition_t composition;
    hmi_snapshot_t snapshot;
    void *ui;
    uint32_t ui_start;
} app;

static hmi_diagnostics_t ui_diagnostics;
static void present_tick(lv_timer_t *timer)
{
    const uint32_t now = hmi_board_now_ms();
    if (!app.ui || !hmi_execution_present(&app.snapshot, &app.composition.ui))
        return;
    (void)hmi_i18n_set_language(app.snapshot.language);
    app.product->ui->present(app.ui, &app.snapshot, now - app.ui_start);
    hmi_diag_increment(&ui_diagnostics.data.ui.present_count);
    hmi_board_diagnostics(&ui_diagnostics);
    hmi_execution_ui_diagnostics(&ui_diagnostics);
    hmi_debug_log_drain();
    (void)timer;
}

/* 整个 UI 启动与周期呈现都留在 SDK 的 LVGL 线程里，由它的主循环运行 lv_timer_handler()。 */
static void attach_ui(lv_timer_t *timer)
{
    if (!hmi_execution_present(&app.snapshot, &app.composition.ui))
        return;
    const hmi_product_t *product = app.product;
    if (!product->ui)
    {
        lv_timer_delete(timer);
        return;
    }
    static const hmi_ui_actions_t actions = {.send = hmi_execution_action};
    hmi_diagnostics_init(&ui_diagnostics);
    hmi_debug_lvgl_log_init();
    if (!hmi_i18n_init() || (app.composition.locale_init && !app.composition.locale_init()) ||
        !hmi_board_display_init() || !(app.ui = product->ui->create(lv_screen_active(), &actions)))
    {
        LOG_E("Product UI startup failed");
        lv_timer_delete(timer);
        return;
    }
    app.ui_start = hmi_board_now_ms();
    ui_diagnostics.data.ui.available = true;
    lv_timer_set_cb(timer, present_tick);
    LOG_I("product=%s; UI running in the LVGL thread", product->id);
}

int main(void)
{
    app.product = hmi_product_get();
    const hmi_product_t *product = app.product;
    if (!hmi_firmware_compose(&app.composition) || (product->prepare && !product->prepare()))
    {
        LOG_E("Product composition failed");
        return -1;
    }
    hmi_diagnostics_init(&diagnostics);
    identity = (hmi_build_info_t){.product = product->id,
                                  .platform_revision = HMI_BUILD_PLATFORM,
                                  .sdk_revision = HMI_BUILD_SDK,
                                  .lvgl_version = HMI_UI_VERSION,
                                  .board = HMI_BUILD_BOARD,
                                  .build_date = __DATE__,
                                  .build_time = __TIME__};
    if (HMI_FIRMWARE_DISPLAY_VERSION[0])
        identity.display_version = HMI_FIRMWARE_DISPLAY_VERSION;
    if (!hmi_debug_init(&diagnostics, &identity) ||
        !hmi_core_init_with_settings(&core, product->catalog, &app.composition.domain,
                                     product->initial_settings) ||
        (product->start && !product->start()))
    {
        LOG_E("Product runtime preparation failed");
        return -1;
    }
    const hmi_execution_config_t config = {.product = product,
                                           .core = &core,
                                           .runtime_init = app.composition.runtime_init,
                                           .action_applied = app.composition.action_applied,
                                           .published_storage = app.composition.presentation,
                                           .public_diagnostics = &diagnostics};
    if (!hmi_execution_start(&config))
    {
        LOG_E("runtime startup failed");
        return -1;
    }
    return 0;
}

/* SDK 的 LVGL 线程在显示与输入设备初始化之后调用 lv_user_gui_init()。SConscript 用链接器的
   -Wl,--wrap=lv_user_gui_init 把这个调用接到下面的函数上：先跑 SDK 原来的实现，再启动本应用的 UI，
   不需要改动 SDK 源码。 */
extern void __real_lv_user_gui_init(void);
void __wrap_lv_user_gui_init(void)
{
    __real_lv_user_gui_init();
    if (!lv_timer_create(attach_ui, 16, NULL))
        LOG_E("UI attach timer unavailable");
}
