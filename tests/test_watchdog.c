#include "runtime/hmi_watchdog.h"
#include <assert.h>

int main(void)
{
    hmi_watchdog_state_t state;
    hmi_watchdog_init(&state);
    const hmi_watchdog_policy_t policy = {.enabled = true, .stall_ms = {4000u, 3000u}};
    hmi_watchdog_input_t input = {.expected = 1u << HMI_WATCHDOG_APP};
    assert(hmi_watchdog_step(&state, &policy, &input, 0u));
    ++input.counter[HMI_WATCHDOG_APP];
    assert(hmi_watchdog_step(&state, &policy, &input, 4001u));

    input.expected |= 1u << HMI_WATCHDOG_UI;
    ++input.counter[HMI_WATCHDOG_UI];
    assert(hmi_watchdog_step(&state, &policy, &input, 6000u));
    ++input.counter[HMI_WATCHDOG_APP];
    assert(hmi_watchdog_step(&state, &policy, &input, 9000u));
    assert(!hmi_watchdog_step(&state, &policy, &input, 9001u));
    assert(state.stalled == HMI_WATCHDOG_UI);

    ++input.counter[HMI_WATCHDOG_UI];
    assert(hmi_watchdog_step(&state, &policy, &input, 12000u));
    ++input.counter[HMI_WATCHDOG_UI];
    assert(!hmi_watchdog_step(&state, &policy, &input, 13001u));
    assert(state.stalled == HMI_WATCHDOG_APP);
    return 0;
}
