#ifndef HOST_PLATFORM_H
#define HOST_PLATFORM_H
#include <lvgl.h>
#include <stdbool.h>
#include <stdint.h>
/* 窗口与输入接口只由持有 LVGL 的唯一 UI 线程调用；
 * 其他线程并发访问 SDL 或 lv_timer_handler() 都不安全。 */
bool hmi_host_open(bool hidden);
bool hmi_host_events(void);
void hmi_host_click(int x, int y, bool pressed);
bool hmi_host_capture(const char *path);
void hmi_host_close(void);
uint64_t hmi_host_counter(void);
double hmi_host_us(uint64_t before, uint64_t after);
void hmi_host_delay(unsigned ms);
#endif
