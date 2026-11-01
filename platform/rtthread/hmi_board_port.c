#define LOG_TAG "hmi.board"
#define LOG_LVL LOG_LVL_INFO
#include "platform/rtthread/hmi_board_port.h"
#include "platform/rtthread/hmi_board_color.h"
#include <aic_core.h>
#include <aic_time.h>
#include <lvgl.h>
#include <mpp_fb.h>
#include <rtdevice.h>
#include <rtthread.h>
#include <string.h>
#include <ulog.h>
uint32_t hmi_board_now_ms(void)
{
    return (uint32_t)aic_get_time_ms();
}
#ifdef AIC_HMI_DISPLAY_EXCLUSIVE
/* UI 独占显示层：关掉视频层与额外 UI 矩形，全局 alpha 255，关闭色键。用视频层的产品在 Kconfig 关掉此项。 */
static bool display_takeover(void)
{
    struct mpp_fb *fb = mpp_fb_open();
    if (!fb)
        return false;
    struct aicfb_layer_data layer = {.layer_id = AICFB_LAYER_TYPE_VIDEO};
    bool ok = mpp_fb_ioctl(fb, AICFB_UPDATE_LAYER_CONFIG, &layer) == 0;
    layer.layer_id = AICFB_LAYER_TYPE_UI;
    for (unsigned rect = 1; rect < 4; ++rect)
    {
        layer.rect_id = rect;
        if (mpp_fb_ioctl(fb, AICFB_UPDATE_LAYER_CONFIG, &layer) != 0)
            ok = false;
    }
    struct aicfb_alpha_config alpha = {
        .layer_id = AICFB_LAYER_TYPE_UI, .enable = 1, .mode = AICFB_GLOBAL_ALPHA_MODE, .value = 255};
    struct aicfb_ck_config ck = {.layer_id = AICFB_LAYER_TYPE_UI};
    if (mpp_fb_ioctl(fb, AICFB_UPDATE_ALPHA_CONFIG, &alpha) != 0)
        ok = false;
    if (mpp_fb_ioctl(fb, AICFB_UPDATE_CK_CONFIG, &ck) != 0)
        ok = false;
    mpp_fb_close(fb);
    if (ok)
        LOG_I("display exclusive: video/extra UI rects off; alpha=255; color key off");
    else
        LOG_E("display exclusive takeover failed");
    return ok;
}
#endif
static uint32_t flush_count;
static uint32_t touch_events;
static void flush_event(lv_event_t *event)
{
    (void)event;
    ++flush_count;
}
static void touch_event(lv_event_t *event)
{
    (void)event;
    ++touch_events;
}
static lv_indev_t *pointer_indev(void)
{
    for (lv_indev_t *indev = lv_indev_get_next(NULL); indev; indev = lv_indev_get_next(indev))
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER)
            return indev;
    return NULL;
}
bool hmi_board_display_init(void)
{
    /* SDK 的 LVGL 线程已经建好显示与输入设备（lv_port_disp_init / lv_port_indev_init）。 */
    lv_display_t *display = lv_display_get_default();
    if (!display)
    {
        LOG_E("no LVGL display");
        return false;
    }
#ifdef AIC_HMI_DISPLAY_EXCLUSIVE
    if (!display_takeover())
        return false;
#endif
    lv_display_add_event_cb(display, flush_event, LV_EVENT_FLUSH_FINISH, NULL);
    lv_indev_t *pointer = pointer_indev();
    if (pointer)
        lv_indev_add_event_cb(pointer, touch_event, LV_EVENT_PRESSED, NULL);
    else
        LOG_W("no pointer input device; UI runs without touch");
    hmi_board_color_init();
    return true;
}
void hmi_board_display_close(hmi_diagnostics_t *diag)
{
    if (diag)
    {
        diag->data.touch.available = false;
        diag->data.ui.available = false;
        diag->data.ui.flush_available = false;
    }
}
void hmi_board_diagnostics(hmi_diagnostics_t *diag)
{
    if (!diag)
        return;
    diag->data.ui.flush_available = true;
    diag->data.ui.flush_count = flush_count;
    lv_indev_t *pointer = pointer_indev();
    lv_display_t *display = lv_display_get_default();
    if (pointer && display)
    {
        /* SDK 的输入驱动不暴露中断、读取与恢复计数，这些字段保持 0；这里只报告 LVGL 看得到的。 */
        lv_point_t point;
        lv_indev_get_point(pointer, &point);
        diag->data.touch = (hmi_diag_touch_t){.available = true,
                                              .range_x = lv_display_get_horizontal_resolution(display),
                                              .range_y = lv_display_get_vertical_resolution(display),
                                              .x = point.x,
                                              .y = point.y,
                                              .state = lv_indev_get_state(pointer),
                                              .events = touch_events};
    }
}
