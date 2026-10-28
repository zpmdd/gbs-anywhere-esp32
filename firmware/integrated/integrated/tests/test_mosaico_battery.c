#include "gbs_mosaico_battery.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    gbs_device_status_t status={.orientation=GBS_ORIENTATION_USB_TOP};
    gbs_mosaico_battery_update(&status,true,8,23,32,54,100);
    assert(status.valid && status.battery_present && status.battery_percent_known && status.battery_percent==54);
    assert(status.charging_known && status.charging && status.orientation==GBS_ORIENTATION_USB_TOP);
    gbs_mosaico_battery_update(&status,true,8,0,32,54,200);
    assert(status.charging_known && !status.charging);
    gbs_mosaico_battery_update(&status,true,9,-20,32,53,300);
    assert(status.charging_known && !status.charging);
    gbs_mosaico_battery_update(&status,true,9,20,32,53,400);
    assert(!status.charging_known && !status.charging);
    gbs_mosaico_battery_update(&status,true,8,20,0,53,500);
    assert(!status.battery_percent_known && !status.charging_known);
    gbs_mosaico_battery_update(&status,true,8,20,32|1024,53,600);
    assert(!status.battery_percent_known && !status.charging_known);
    gbs_mosaico_battery_update(&status,true,8,20,32|1,53,700);
    assert(!status.battery_percent_known && !status.charging_known);
    gbs_mosaico_battery_update(&status,true,8,20,32,101,800);
    assert(!status.battery_percent_known);
    gbs_mosaico_battery_update(&status,true,0,20,32,53,900);
    assert(status.valid && status.battery_present_known && !status.battery_present);
    assert(!status.battery_percent_known && !status.charging_known);
    gbs_mosaico_battery_update(&status,true,8,20,32,53,1000);
    gbs_mosaico_battery_update(&status,false,0,0,0,0,6000);
    assert(status.valid && status.observed_ms==1000 && status.battery_percent==53);
    gbs_mosaico_battery_update(&status,false,0,0,0,0,6001);
    assert(!status.valid && !status.charging_known && !status.battery_percent_known);
    gbs_mosaico_battery_update(&status,true,8,20,32,53,7000);
    gbs_mosaico_battery_update(&status,false,0,0,0,0,6999);
    assert(!status.valid && status.orientation==GBS_ORIENTATION_USB_TOP);
    puts("Mosaico battery status checks passed");
    return 0;
}
