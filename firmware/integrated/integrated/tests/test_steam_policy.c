#include "gbs_steam_policy.h"
#include <assert.h>
#include <stdio.h>

static const int64_t MIDNIGHT = 1793577600; // 2026-11-02 00:00 UTC, 08:00 Beijing.
static gbs_steam_input_t input(void)
{
    return (gbs_steam_input_t){.now_ms=1000,.unix_seconds=MIDNIGHT,.clock_ready=true,
        .network_ready=true,.fresh=true,.online=true,.idle=true,.steam_enabled=true};
}
int main(void)
{
    assert(gbs_steam_window(0)==0);
    assert(gbs_steam_window(MIDNIGHT-3601)==0);
    assert(gbs_steam_window(MIDNIGHT-3600)%3==1);
    assert(gbs_steam_window(MIDNIGHT+4*3600-1)%3==1);
    assert(gbs_steam_window(MIDNIGHT+4*3600)==0);
    assert(gbs_steam_window(MIDNIGHT+6*3600-1)==0);
    assert(gbs_steam_window(MIDNIGHT+6*3600)%3==2);
    assert(gbs_steam_window(MIDNIGHT+12*3600-1)%3==2);
    assert(gbs_steam_window(MIDNIGHT+12*3600)==0);
    assert(gbs_steam_window(MIDNIGHT+86400)==gbs_steam_window(MIDNIGHT)+3);
    gbs_steam_input_t in=input();
    gbs_steam_policy_t p={0};
    assert(!gbs_steam_policy_step(&p,&in)&&p.status.phase==GBS_STEAM_DISABLED);
    p.status.enabled=true; in.clock_ready=false;
    assert(!gbs_steam_policy_step(&p,&in)&&!p.session);
    in.clock_ready=true; in.fresh=false;
    assert(!gbs_steam_policy_step(&p,&in)&&!p.session);
    in.fresh=true; in.idle=false;
    assert(!gbs_steam_policy_step(&p,&in)&&p.status.phase==GBS_STEAM_WAIT_IDLE);
    in.idle=true;
    assert(gbs_steam_policy_step(&p,&in)&&!p.status.target);
    p.record=(gbs_steam_record_t){p.session_window,GBS_STEAM_UNKNOWN};
    assert(!gbs_steam_policy_step(&p,&in));
    // Interrupted writes survive a reboot without replay, even if the target is absent.
    gbs_steam_policy_t reboot={.status.enabled=true,.record=p.record};
    assert(!gbs_steam_policy_step(&reboot,&in)&&reboot.status.phase==GBS_STEAM_UNKNOWN);
    // A later matching observation clears UNKNOWN, including a restored journal, without another write.
    in.steam_enabled=false; in.fresh=false;
    assert(!gbs_steam_policy_step(&reboot,&in)&&reboot.record.outcome==GBS_STEAM_UNKNOWN);
    in.fresh=true; in.online=false;
    assert(!gbs_steam_policy_step(&reboot,&in)&&reboot.record.outcome==GBS_STEAM_UNKNOWN);
    in.online=true; in.clock_ready=false;
    assert(!gbs_steam_policy_step(&reboot,&in)&&reboot.record.outcome==GBS_STEAM_UNKNOWN);
    in.clock_ready=true;
    assert(!gbs_steam_policy_step(&reboot,&in)&&reboot.record.outcome==GBS_STEAM_READY);
    reboot.record.outcome=GBS_STEAM_MANUAL;
    assert(!gbs_steam_policy_step(&reboot,&in)&&reboot.record.outcome==GBS_STEAM_MANUAL);
    p.record.outcome=GBS_STEAM_READY; in.steam_enabled=false;
    assert(!gbs_steam_policy_step(&p,&in)&&p.status.phase==GBS_STEAM_READY);
    in.steam_enabled=true;
    assert(!gbs_steam_policy_step(&p,&in)&&p.record.outcome==GBS_STEAM_MANUAL);
    in.fresh=false; in.now_ms+=2000;
    assert(!gbs_steam_policy_step(&p,&in));
    in.fresh=true; in.now_ms+=1000;
    assert(!gbs_steam_policy_step(&p,&in)&&p.status.phase==GBS_STEAM_MANUAL);
    // Crossing a window while the same session stays online does not switch boilers.
    in.unix_seconds+=6*3600; in.steam_enabled=false;
    assert(!gbs_steam_policy_step(&p,&in)&&p.status.phase==GBS_STEAM_OUTSIDE);
    in.fresh=false; in.now_ms+=1000; assert(!gbs_steam_policy_step(&p,&in));
    in.now_ms+=60001; assert(!gbs_steam_policy_step(&p,&in));
    in.fresh=true; assert(gbs_steam_policy_step(&p,&in)&&p.status.target);
    in.steam_enabled=true;
    assert(!gbs_steam_policy_step(&p,&in)&&p.status.phase==GBS_STEAM_READY);
    // A local network outage cannot rearm a continuously running machine.
    in.fresh=false; in.network_ready=false; in.now_ms+=70000;
    assert(!gbs_steam_policy_step(&p,&in)&&p.session);
    puts("Smart steam policy: Beijing boundaries, fresh clock, busy/deferred, manual override and no-replay PASS");
}
