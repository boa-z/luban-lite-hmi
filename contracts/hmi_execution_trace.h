#ifndef HMI_EXECUTION_TRACE_H
#define HMI_EXECUTION_TRACE_H

#include <stdint.h>

typedef enum
{
    HMI_BB_APP,
    HMI_BB_UI,
    HMI_BB_TX0,
    HMI_BB_TX1,
    HMI_BB_SDO,
    HMI_BB_NVM,
    HMI_BB_OWNERS
} hmi_bb_owner_t;

typedef enum
{
    HMI_BB_IDLE = 1,
    HMI_BB_STARTUP,
    HMI_BB_CAN_READ,
    HMI_BB_CAN_DISPATCH,
    HMI_BB_ACTIONS,
    HMI_BB_PROTOCOL,
    HMI_BB_PERIODIC,
    HMI_BB_BACKLIGHT,
    HMI_BB_PUBLISH,
    HMI_BB_UI_SNAPSHOT,
    HMI_BB_UI_RENDER,
    HMI_BB_UI_DIAG,
    HMI_BB_UI_LOG,
    HMI_BB_CAN_WRITE,
    HMI_BB_CAN_CONFIGURE,
    HMI_BB_SDO_UPLOAD,
    HMI_BB_SDO_DOWNLOAD,
    HMI_BB_NVM_WRITE
} hmi_bb_stage_t;

#if defined(HMI_RTTHREAD) && defined(AIC_HMI_BLACKBOX)
void hmi_bb_phase(hmi_bb_owner_t owner, hmi_bb_stage_t stage, uint32_t detail);
void hmi_bb_complete(hmi_bb_owner_t owner, uint32_t result, uint32_t code);
#else
#define hmi_bb_phase(owner, stage, detail) ((void)0)
#define hmi_bb_complete(owner, result, code) ((void)0)
#endif

#endif
