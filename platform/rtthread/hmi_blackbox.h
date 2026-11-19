#ifndef HMI_BLACKBOX_H
#define HMI_BLACKBOX_H

#ifdef HMI_RTTHREAD
#include <rtconfig.h>
#endif
#include "contracts/hmi_execution_trace.h"

typedef enum
{
    HMI_BB_WDT_FEED = 1,
    HMI_BB_WDT_WITHHELD,
    HMI_BB_WDT_FEED_ERROR
} hmi_bb_wdt_state_t;

#ifdef AIC_HMI_BLACKBOX
#include "diagnostics/hmi_diagnostics.h"

void hmi_bb_heartbeat(hmi_bb_owner_t owner, uint32_t now);
void hmi_bb_watchdog(uint32_t now, uint32_t state, uint32_t channel);
void hmi_bb_can(unsigned bus, const hmi_diag_can_t *counts, uint32_t tx_drop, uint32_t tx_queue_full,
                uint32_t rx_budget_hits);
void hmi_bb_can_status(unsigned bus, uint32_t state, uint32_t tx_errors, uint32_t ack_errors);
void hmi_bb_can_recovered(unsigned bus, bool success);
#else
#define hmi_bb_heartbeat(owner, now) ((void)0)
#define hmi_bb_watchdog(now, state, channel) ((void)0)
#define hmi_bb_can(bus, counts, tx_drop, tx_queue_full, rx_budget_hits) ((void)0)
#define hmi_bb_can_status(bus, state, tx_errors, ack_errors) ((void)0)
#define hmi_bb_can_recovered(bus, success) ((void)0)
#endif

#endif
