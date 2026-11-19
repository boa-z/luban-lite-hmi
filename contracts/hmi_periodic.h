#ifndef HMI_PERIODIC_H
#define HMI_PERIODIC_H
#include "contracts/hmi_can_frame.h"
#include "contracts/hmi_domain.h"
/**
 * @brief 周期帧编码：运行时线程在发送期限到达时调用，从当前快照填写 frame 的数据。
 *
 * frame 进入时是定义里的模板（总线、标识、长度、固定字节）。返回 false 表示本周期不发送。
 * 回调不得阻塞、不得修改快照；需要“值过期时发无效标志”之类的策略，按信号的 state 自行判断。
 */
typedef bool (*hmi_periodic_encoder_t)(const hmi_snapshot_t *snapshot, uint32_t now_ms,
                                         hmi_can_frame_t *frame);
/**
 * @brief 不可变的 Product 周期帧定义。
 *
 * 同一条目的上一帧还在发送队列里时不会再排下一帧，超过一个周期仍未交给驱动的帧被丢弃，
 * 所以总线无应答时不会积压。
 */
typedef struct
{
    hmi_can_frame_t frame;
    uint32_t period_ms;
    hmi_periodic_encoder_t encode; /* NULL 时原样发送模板。 */
} hmi_periodic_frame_t;
#endif
