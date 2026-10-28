#pragma once
#include "gbs_device.h"

/* BQ27220 TRM SLUUBD4A: BatteryStatus, Current, OperationStatus and SOC.
 * Read the factory gauge profile; do not replace its calibrated capacity. */
static inline void gbs_mosaico_battery_update(gbs_device_status_t *status,
    bool valid, uint16_t flags, int16_t current_ma, uint16_t operation,
    uint16_t percent, int64_t now)
{
    if (!valid && status->valid && now>=status->observed_ms && now-status->observed_ms<=5000) return;
    gbs_orientation_t orientation=status->orientation;
    *status=(gbs_device_status_t){.orientation=orientation};
    if (!valid) return;
    status->valid=true;
    status->observed_ms=now;
    status->battery_present_known=true;
    status->battery_present=(flags & (1u<<3))!=0;
    if (!status->battery_present) return;
    bool ready=(operation & (1u<<5)) && !(operation & ((1u<<10)|1u));
    status->battery_percent_known=ready && percent<=100;
    if (status->battery_percent_known) status->battery_percent=percent;
    /* DSG clear also means relaxation; a positive measured current is required. */
    status->charging_known=ready && (current_ma<=0 || !(flags & 1u));
    status->charging=status->charging_known && current_ma>0;
}
