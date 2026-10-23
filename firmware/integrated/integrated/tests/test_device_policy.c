#include "gbs_device_policy.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

static int64_t now;
static gbs_orientation_t sample(gbs_rotation_policy_t *p, bool enabled, bool touching,
    bool valid, float x, float y, float z)
{
    now+=50;
    return gbs_rotation_policy_update(p,enabled,touching,valid,x,y,z,now);
}

static void hold(gbs_rotation_policy_t *p,float x,float y,float z)
{
    for(unsigned i=0;i<18;i++) sample(p,true,false,true,x,y,z);
}

static void rotations(void)
{
    gbs_rotation_policy_t p;
    gbs_rotation_policy_init(&p);
    assert(p.current==GBS_ORIENTATION_USB_RIGHT);
    for(unsigned i=0;i<30;i++) sample(&p,false,false,true,0,1,0);
    assert(p.current==GBS_ORIENTATION_USB_RIGHT);
    for(unsigned i=0;i<10;i++) sample(&p,true,false,true,0,1,0);
    assert(p.current==GBS_ORIENTATION_USB_RIGHT);
    /* A held finger cancels the candidate, not just the final transition. */
    for(unsigned i=0;i<30;i++) sample(&p,true,true,true,0,1,0);
    for(unsigned i=0;i<10;i++) sample(&p,true,false,true,0,1,0);
    assert(p.current==GBS_ORIENTATION_USB_RIGHT);
    hold(&p,0,1,0); assert(p.current==GBS_ORIENTATION_USB_BOTTOM);
    hold(&p,1,0,0); assert(p.current==GBS_ORIENTATION_USB_LEFT);
    hold(&p,0,-1,0); assert(p.current==GBS_ORIENTATION_USB_TOP);
    hold(&p,-1,0,0); assert(p.current==GBS_ORIENTATION_USB_RIGHT);
    /* Flat, diagonal, moving and invalid samples cannot choose an orientation. */
    hold(&p,.25f,0,.97f);
    hold(&p,.71f,.71f,0);
    hold(&p,1.5f,0,0);
    for(unsigned i=0;i<30;i++) sample(&p,true,false,true,i%2?1.0f:.75f,0,0);
    for(unsigned i=0;i<30;i++) sample(&p,true,false,false,1,0,0);
    for(unsigned i=0;i<30;i++) sample(&p,true,false,true,NAN,0,0);
    assert(p.current==GBS_ORIENTATION_USB_RIGHT);
    for(unsigned i=0;i<10;i++) sample(&p,true,false,true,0,1,0);
    now+=5000;
    sample(&p,true,false,true,0,1,0);
    assert(p.current==GBS_ORIENTATION_USB_RIGHT);
    hold(&p,0,1,0);
    assert(p.current==GBS_ORIENTATION_USB_BOTTOM);
    hold(&p,0,0,-1);
    for(unsigned i=0;i<20;i++) sample(&p,false,false,true,-1,0,0);
    assert(p.current==GBS_ORIENTATION_USB_BOTTOM); /* Direction lock retains current. */
    /* Stable physical samples captured with USB-right, then USB-bottom. */
    hold(&p,-1.010f,0,.010f); assert(p.current==GBS_ORIENTATION_USB_RIGHT);
    hold(&p,.010f,.942f,.342f); assert(p.current==GBS_ORIENTATION_USB_BOTTOM);
    hold(&p,0,.935f,.331f); assert(p.current==GBS_ORIENTATION_USB_BOTTOM);
    for(unsigned i=0;i<18;i++) sample(&p,true,true,true,-1.010f,0,.010f);
    assert(p.current==GBS_ORIENTATION_USB_BOTTOM);
}

static void touches(void)
{
    uint16_t x,y;
    /* Measured raw corners with the physical USB-right panel upright. */
    const uint16_t corners[][2]={{48,70},{426,62},{435,450},{28,442}};
    for(unsigned i=0;i<4;i++) {
        assert(gbs_device_map_touch(GBS_ORIENTATION_USB_RIGHT,corners[i][0],corners[i][1],&x,&y));
        assert(x==corners[i][0] && y==corners[i][1]);
    }
    /* Panel-addressed logical pixels must map back to the same logical
     * position through each quarter-turn of the calibrated raw axes. */
    for(unsigned o=0;o<4;o++) {
        uint16_t raw_x=438,raw_y=44;
        switch(gbs_device_panel_madctl(o)) {
        case 0x00: break;
        case 0xA0: raw_x=44; raw_y=479-438; break;
        case 0xC0: raw_x=479-438; raw_y=479-44; break;
        case 0x60: raw_x=479-44; raw_y=438; break;
        default: assert(false);
        }
        assert(gbs_device_map_touch(o,raw_x,raw_y,&x,&y));
        assert(x==438 && y==44);
    }
    const uint16_t expected[][2]={{12,37},{442,12},{467,442},{37,467}};
    for(unsigned o=0;o<4;o++) {
        assert(gbs_device_map_touch(o,12,37,&x,&y));
        assert(x==expected[o][0] && y==expected[o][1]);
        assert(gbs_device_map_touch(o,0,0,&x,&y)); assert(x<480 && y<480);
        assert(gbs_device_map_touch(o,479,479,&x,&y)); assert(x<480 && y<480);
    }
    assert(!gbs_device_map_touch(0,480,0,&x,&y));
    assert(!gbs_device_map_touch(0,0,480,&x,&y));
    assert(!gbs_device_map_touch(4,0,0,&x,&y));
    assert(!gbs_device_map_touch(0,0,0,NULL,&y));
}

static void touch_idle_reports(void)
{
    gbs_touch_policy_t s={0};
    assert(gbs_device_touch_idle(&s,true,true));
    assert(!gbs_device_touch_idle(&s,true,false));
    assert(!gbs_device_touch_idle(&s,false,true)); /* I2C failure, not an idle marker. */
    assert(!gbs_device_touch_idle(NULL,true,true));
    gbs_rotation_policy_t p;
    gbs_rotation_policy_init(&p);
    for(unsigned i=0;i<18;i++) {
        bool idle=gbs_device_touch_idle(&s,true,true);
        gbs_device_touch_update(&s,idle,false,0,0,false);
        sample(&p,true,s.physical_down || !idle,true,0,1,0);
    }
    assert(p.current==GBS_ORIENTATION_USB_BOTTOM);
    gbs_device_touch_update(&s,true,true,80,90,false);
    bool idle=gbs_device_touch_idle(&s,true,true);
    assert(!idle);
    gbs_device_touch_update(&s,idle,false,0,0,false);
    assert(s.physical_down && s.visible_down && s.x==80 && s.y==90);
    for(unsigned i=0;i<18;i++) sample(&p,true,s.physical_down || !idle,true,1,0,0);
    assert(p.current==GBS_ORIENTATION_USB_BOTTOM);
    gbs_device_touch_update(&s,true,false,0,0,false);
    assert(gbs_device_touch_idle(&s,true,true));
    hold(&p,1,0,0); assert(p.current==GBS_ORIENTATION_USB_LEFT);
    gbs_device_touch_update(&s,true,true,81,91,true);
    assert(!gbs_device_touch_idle(&s,true,true));
    gbs_device_touch_update(&s,false,false,0,0,false);
    assert(s.physical_down && s.wake_gesture && !s.visible_down);
}

static void batteries(void)
{
    gbs_device_status_t s={.orientation=GBS_ORIENTATION_USB_LEFT};
    /* Battery present + VBUS good; system on, charge direction and CC stage. */
    gbs_device_decode_pmu(&s,true,0x28,0x32,true,84,123);
    assert(s.valid && s.battery_present_known && s.battery_present && s.battery_percent_known);
    assert(s.battery_percent==84 && s.charging_known && s.charging && s.observed_ms==123);
    gbs_device_decode_pmu(&s,true,8,0x55,true,0,200);
    assert(s.battery_percent_known && s.battery_percent==0 && s.charging_known && !s.charging);
    gbs_device_decode_pmu(&s,true,0x28,0x14,true,100,300);
    assert(s.battery_percent_known && s.battery_percent==100 && !s.charging);
    gbs_device_decode_pmu(&s,true,0x28,0x72,true,255,400);
    assert(s.valid && !s.battery_percent_known && !s.charging_known && !s.charging);
    gbs_device_decode_pmu(&s,true,0x20,0x32,true,84,500);
    assert(s.valid && s.battery_present_known && !s.battery_present && !s.battery_percent_known);
    assert(!s.charging_known && !s.charging);
    gbs_device_decode_pmu(&s,true,0x28,0x32,false,84,600);
    assert(s.valid && !s.battery_percent_known && s.charging_known && s.charging);
    gbs_device_decode_pmu(&s,false,0x28,0x32,true,84,700);
    assert(!s.valid && !s.battery_present_known && !s.battery_percent_known && !s.charging_known);
    assert(s.battery_percent==0 && s.observed_ms==0 && s.orientation==GBS_ORIENTATION_USB_LEFT);
    /* No VBUS must suppress even a contradictory charging direction.
     * Reserved bit7 is not part of direction; direction 3 stays unknown. */
    for (unsigned vbus=0;vbus<2;vbus++) for (unsigned reserved=0;reserved<2;reserved++)
        for (unsigned direction=0;direction<4;direction++) {
            uint8_t raw00=8|(vbus<<5),raw01=(reserved<<7)|(direction<<5)|0x12;
            gbs_device_decode_pmu(&s,true,raw00,raw01,true,98,800);
            assert(s.valid && s.battery_percent_known && s.battery_percent==98);
            assert(s.charging_known==(direction!=3));
            assert(s.charging==(vbus && direction==1));
        }
}

static void battery_read_recovery(void)
{
    gbs_device_status_t s={.orientation=GBS_ORIENTATION_USB_RIGHT};
    gbs_device_update_pmu(&s,false,0,0,false,0,1000); assert(!s.valid);
    gbs_device_update_pmu(&s,true,0x28,0x32,true,96,2000);
    gbs_device_update_pmu(&s,false,0,0,false,0,4000);
    assert(s.valid&&s.battery_percent==96&&s.charging&&s.observed_ms==2000);
    gbs_device_update_pmu(&s,false,0,0,false,0,7000); assert(s.valid&&s.observed_ms==2000);
    gbs_device_update_pmu(&s,false,0,0,false,0,7001); assert(!s.valid&&!s.battery_percent_known&&!s.charging_known);
    gbs_device_update_pmu(&s,true,8,0x55,true,0,8000);
    assert(s.valid&&s.battery_percent_known&&s.battery_percent==0&&!s.charging);
    gbs_device_update_pmu(&s,false,0,0,false,0,7999); assert(!s.valid);
    gbs_device_update_pmu(&s,true,8,0x55,true,95,9000);
    gbs_device_update_pmu(&s,true,0x20,0x32,false,0,9100);
    assert(s.valid&&s.battery_present_known&&!s.battery_present&&!s.battery_percent_known);
    gbs_device_update_pmu(&s,true,8,0x55,true,94,9200);
    assert(s.valid&&s.battery_percent==94&&s.observed_ms==9200);
    gbs_device_update_pmu(&s,true,8,0x55,false,0,9300);
    assert(s.valid&&!s.battery_percent_known&&s.observed_ms==9300);
    assert(s.orientation==GBS_ORIENTATION_USB_RIGHT);
}

static void touch_failures(void)
{
    gbs_touch_policy_t s={0};
    unsigned releases=0;
    const bool valid[]={true,false,true,true};
    const bool down[]={true,false,true,false};
    for(unsigned i=0;i<4;i++) {
        bool previously_pressed=s.visible_down;
        gbs_device_touch_update(&s,valid[i],down[i],80,90,false);
        if(previously_pressed && !s.visible_down) releases++;
        assert(releases==(i==3?1u:0u));
        if(i<3) assert(s.physical_down && s.visible_down && s.x==80 && s.y==90);
    }
    /* A dim-screen gesture never reaches LVGL, even if its failed read claims
     * release and the screen has become bright in the meantime. */
    gbs_device_touch_update(&s,true,true,120,130,true);
    assert(s.wake_gesture && s.physical_down && !s.visible_down);
    gbs_device_touch_update(&s,false,false,0,0,false);
    assert(s.wake_gesture && s.physical_down && !s.visible_down && s.x==120 && s.y==130);
    gbs_device_touch_update(&s,true,true,121,131,false);
    assert(s.wake_gesture && !s.visible_down);
    gbs_device_touch_update(&s,true,false,0,0,false);
    assert(!s.wake_gesture && !s.visible_down && !s.physical_down);
    gbs_device_touch_update(&s,true,true,30,40,false);
    assert(s.visible_down && s.physical_down);
}

static void buttons(void)
{
    gbs_button_policy_t s={0};
    assert(gbs_device_button_update(&s,true,false,0)==GBS_BUTTON_NONE);
    assert(gbs_device_button_update(&s,true,false,3000)==GBS_BUTTON_NONE);
    assert(gbs_device_button_update(&s,false,false,3010)==GBS_BUTTON_NONE);
    assert(gbs_device_button_update(&s,false,false,3040)==GBS_BUTTON_NONE);
    /* Contact bounce cannot produce a short press. */
    assert(gbs_device_button_update(&s,true,false,3100)==GBS_BUTTON_NONE);
    assert(gbs_device_button_update(&s,false,false,3110)==GBS_BUTTON_NONE);
    assert(gbs_device_button_update(&s,false,false,3140)==GBS_BUTTON_NONE);
    assert(gbs_device_button_update(&s,true,false,3200)==GBS_BUTTON_NONE);
    assert(gbs_device_button_update(&s,true,false,3230)==GBS_BUTTON_NONE);
    assert(gbs_device_button_update(&s,false,false,3300)==GBS_BUTTON_NONE);
    assert(gbs_device_button_update(&s,false,false,3330)==GBS_BUTTON_SHORT);
    assert(gbs_device_button_update(&s,false,false,3400)==GBS_BUTTON_NONE);
    /* A long press emits once, and release never becomes a second action. */
    assert(gbs_device_button_update(&s,true,false,3500)==GBS_BUTTON_NONE);
    assert(gbs_device_button_update(&s,true,false,3530)==GBS_BUTTON_NONE);
    assert(gbs_device_button_update(&s,true,false,5029)==GBS_BUTTON_NONE);
    assert(gbs_device_button_update(&s,true,false,5030)==GBS_BUTTON_LONG);
    assert(gbs_device_button_update(&s,true,false,6000)==GBS_BUTTON_NONE);
    assert(gbs_device_button_update(&s,false,false,6010)==GBS_BUTTON_NONE);
    assert(gbs_device_button_update(&s,false,false,6040)==GBS_BUTTON_NONE);
    /* A dark-screen press wakes once; holding/releasing cannot navigate or
     * switch rotation even after the screen is already bright. */
    assert(gbs_device_button_update(&s,true,true,6100)==GBS_BUTTON_NONE);
    assert(gbs_device_button_update(&s,true,true,6130)==GBS_BUTTON_WAKE);
    assert(gbs_device_button_update(&s,true,false,8000)==GBS_BUTTON_NONE);
    assert(gbs_device_button_update(&s,false,false,8010)==GBS_BUTTON_NONE);
    assert(gbs_device_button_update(&s,false,false,8040)==GBS_BUTTON_NONE);
    gbs_button_policy_t released_at_boot={0};
    assert(gbs_device_button_update(&released_at_boot,false,false,0)==GBS_BUTTON_NONE);
    assert(gbs_device_button_update(&released_at_boot,false,false,30)==GBS_BUTTON_NONE);
    assert(released_at_boot.armed);
}

static void touch_key_arbitration(void)
{
    gbs_touch_policy_t s={0};
    gbs_device_touch_update(&s,true,true,20,30,false);
    assert(s.visible_down);
    gbs_device_touch_inhibit(&s,true);
    assert(s.inhibited && s.wake_gesture && !s.visible_down);
    gbs_device_touch_update(&s,false,false,0,0,false);
    assert(s.physical_down && !s.visible_down);
    gbs_device_touch_update(&s,true,false,0,0,false);
    assert(s.wake_gesture);
    gbs_device_touch_update(&s,true,true,21,31,false);
    assert(!s.visible_down);
    gbs_device_touch_inhibit(&s,false);
    gbs_device_touch_update(&s,false,false,0,0,false);
    assert(s.wake_gesture && !s.visible_down);
    gbs_device_touch_update(&s,true,true,22,32,false);
    assert(!s.visible_down);
    gbs_device_touch_update(&s,true,false,0,0,false);
    assert(!s.wake_gesture);
    gbs_device_touch_update(&s,true,true,23,33,false);
    assert(s.visible_down);
}

int main(void)
{
    rotations(); touches(); touch_idle_reports(); batteries(); battery_read_recovery(); touch_failures(); buttons(); touch_key_arbitration();
    puts("device policy: rotation, touch failure/key gestures, battery validity and button sequences passed");
    return 0;
}
