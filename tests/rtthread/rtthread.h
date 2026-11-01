/* 测试替身只实现真实 CAN worker 使用的 RT-Thread 原语。 */
#ifndef TEST_RTTHREAD_H
#define TEST_RTTHREAD_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>
typedef int rt_err_t;
typedef uintptr_t rt_ubase_t;
typedef uint32_t rt_uint32_t, rt_tick_t;
typedef uint8_t rt_uint8_t;
typedef size_t rt_size_t;
struct rt_thread
{
    unsigned unused;
};
struct rt_semaphore
{
    unsigned value;
};
struct rt_messagequeue
{
    uint8_t *pool;
    size_t size, capacity, head, count;
};
#define RT_EOK 0
#define RT_ERROR 1
#define RT_EFULL 2
#define RT_NULL NULL
#define RT_ALIGN_SIZE sizeof(void *)
#define RT_ALIGN(n, a) (((n) + (a) - 1) / (a) * (a))
#define RT_IPC_FLAG_FIFO 0
#define RT_IPC_CMD_RESET 0
#define RT_WAITING_FOREVER -1
rt_err_t rt_mq_init(struct rt_messagequeue *, const char *, void *, size_t, size_t, int);
rt_err_t rt_mq_send(struct rt_messagequeue *, const void *, size_t);
rt_err_t rt_mq_recv(struct rt_messagequeue *, void *, size_t, int);
rt_err_t rt_sem_release(struct rt_semaphore *);
rt_err_t rt_sem_init(struct rt_semaphore *, const char *, unsigned, int);
rt_err_t rt_sem_take(struct rt_semaphore *, int);
rt_err_t rt_sem_control(struct rt_semaphore *, int, void *);
rt_err_t rt_thread_init(struct rt_thread *, const char *, void (*)(void *), void *, void *, size_t, unsigned,
                        unsigned);
rt_err_t rt_thread_startup(struct rt_thread *);
void rt_thread_mdelay(int);
int rt_kprintf(const char *, ...);
static inline int rt_tick_from_millisecond(unsigned ms)
{
    return (int)ms;
}
#endif
