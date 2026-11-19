#include "contracts/hmi_product.h"
#include "catalog/catalog.h"
#include "i18n/generated/product_i18n.h"
#ifndef HMI_HEADLESS
extern const hmi_ui_factory_t product_ui;
#define UI_FACTORY &product_ui
#else
#define UI_FACTORY NULL
#endif
/* CAN：一路由 DBC 生成的解码器，路由表也是生成的（tools/protocol/generate_can.py）。 */ /* [can] */
extern bool product_decode(const hmi_can_frame_t *, hmi_update_sink_t, void *); /* [can] */
static const hmi_protocol_binding_t bindings[] = {{1, product_decode, NULL}}; /* [can] */
static const hmi_protocol_profile_t protocols = {bindings, 1}; /* [can] */
static const hmi_frame_route_t entries[] = { /* [can] */
#include "generated/can/routes.inc" /* [can] */
}; /* [can] */
static const hmi_route_profile_t routes = {entries, sizeof(entries) / sizeof(entries[0])}; /* [can] */

/* 本地业务示例：运行时线程每轮调用，发布开机后的秒数；内容不变时不触发 UI 刷新。 */
static void app_tick(hmi_snapshot_t *snapshot, uint32_t now_ms)
{
    (void)hmi_snapshot_write(snapshot, PRODUCT_UPTIME, (float)(now_ms / 1000u), now_ms);
}

static const hmi_initial_settings_t initial_settings = {.language = PRODUCT_I18N_DEFAULT_LANGUAGE, .brightness = 100};
/* 最小 Product：目录、UI 与本地业务。按需再加 protocols/routes、periodic、local_action、storage。 */
static const hmi_product_t product = {
    .id = "@PRODUCT_ID@",
    .catalog = &product_catalog,
    .ui = UI_FACTORY,
    .initial_settings = &initial_settings,
    .app_tick = app_tick,
    .protocols = &protocols, /* [can] */
    .routes = &routes,       /* [can] */
};
/** @brief 返回静态产品定义；实例及其所引用对象在进程生命周期内有效。 */
const hmi_product_t *hmi_product_get(void)
{
    return &product;
}
