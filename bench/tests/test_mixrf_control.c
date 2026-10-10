#include "ls_test.h"
#include "ls_mixrf_control.h"

LS_CASE(first_holder_keeps_cc1101)
{
    for(int holder=LS_MIXRF_OWNER_MONITOR;holder<=LS_MIXRF_OWNER_FIND_2;holder++) {
        ls_mixrf_status_t s={.keyboard=true,.cc=true};
        LS_CHECK(ls_mixrf_cc_request(&s,holder,true,315000000,false));
        for(int caller=LS_MIXRF_OWNER_MONITOR;caller<=LS_MIXRF_OWNER_FIND_2;caller++) {
            if(caller==holder)continue;
            LS_CHECK(!ls_mixrf_cc_request(&s,caller,true,433920000,false));
            LS_CHECK(!ls_mixrf_cc_request(&s,caller,false,0,false));
            LS_EQ_INT(s.cc_owner,holder);
            LS_CHECK(s.receive_requested);
            LS_EQ_INT(s.capture_requested,holder==LS_MIXRF_OWNER_REC);
            LS_CHECK(strstr(ls_mixrf_cc_refusal(&s),ls_mixrf_owner_name(holder))!=NULL);
        }
        LS_CHECK(ls_mixrf_cc_request(&s,holder,true,915000000,false));
        LS_CHECK(ls_mixrf_cc_request(&s,holder,false,0,false));
        LS_EQ_INT(s.cc_owner,LS_MIXRF_OWNER_NONE);
        LS_CHECK(!s.receive_requested && !s.capture_requested);
        LS_CHECK(ls_mixrf_cc_request(&s,LS_MIXRF_OWNER_REC,true,315000000,false));
    }
}
LS_CASE(find_stop_preserves_monitor_and_survey)
{
    ls_mixrf_status_t s={.cc=true};
    LS_CHECK(ls_mixrf_cc_request(&s,LS_MIXRF_OWNER_MONITOR,true,433920000,false));
    LS_CHECK(ls_mixrf_claim(&s.scan_owner,LS_MIXRF_OWNER_MONITOR,true));
    LS_CHECK(!ls_mixrf_cc_request(&s,LS_MIXRF_OWNER_FIND,false,0,false));
    LS_CHECK(!ls_mixrf_claim(&s.scan_owner,LS_MIXRF_OWNER_FIND,false));
    LS_EQ_INT(s.scan_owner,LS_MIXRF_OWNER_MONITOR);
    LS_CHECK(s.receive_requested);
    LS_CHECK(ls_mixrf_cc_request(&s,LS_MIXRF_OWNER_MONITOR,false,0,false));
    LS_CHECK(ls_mixrf_cc_request(&s,LS_MIXRF_OWNER_FIND,true,315000000,false));
    LS_CHECK(ls_mixrf_cc_request(&s,LS_MIXRF_OWNER_FIND,false,0,false));
    LS_EQ_INT(s.scan_owner,LS_MIXRF_OWNER_MONITOR);
}
LS_CASE(absent_probe_finishes)
{
    ls_mixrf_status_t s={.busy=true};
    ls_mixrf_probe_complete(&s);
    LS_CHECK(s.ready && !s.busy);
    LS_CHECK(!s.cc && !s.nrf && !s.nfc);
}
LS_CASE(detach_clears_watch_and_claims)
{
    ls_mixrf_status_t s={.keyboard=true,.cc=true,.nrf=true,.nfc=true,.power=true};
    LS_CHECK(ls_mixrf_cc_request(&s,LS_MIXRF_OWNER_REC,true,315000000,false));
    s.capturing=s.receiving=s.scan_requested=s.scanning=true;
    s.scan_owner=LS_MIXRF_OWNER_FIND;
    ls_mixrf_detached(&s);
    LS_CHECK(!s.capture_requested && !s.receive_requested && !s.capturing && !s.receiving);
    LS_CHECK(!s.scan_requested && !s.scanning);
    LS_EQ_INT(s.cc_owner,LS_MIXRF_OWNER_NONE);
    LS_EQ_INT(s.scan_owner,LS_MIXRF_OWNER_NONE);
    LS_CHECK(!s.cc && !s.nrf && !s.nfc);
    LS_CHECK(strstr(ls_mixrf_cc_refusal(&s),"Keyboard absent")!=NULL);
    s.keyboard=s.cc=true;
    ls_mixrf_probe_complete(&s);
    LS_CHECK(!s.capture_requested);
    LS_CHECK(ls_mixrf_cc_request(&s,LS_MIXRF_OWNER_MONITOR,true,433920000,false));
}
LS_CASE(unavailable_receiver_cannot_claim)
{
    ls_mixrf_status_t s={0};
    LS_CHECK(!ls_mixrf_cc_request(&s,LS_MIXRF_OWNER_MONITOR,true,433920000,false));
    s.cc=true;s.busy=true;
    LS_CHECK(!ls_mixrf_cc_request(&s,LS_MIXRF_OWNER_REC,true,315000000,false));
    s.busy=false;
    LS_CHECK(!ls_mixrf_cc_request(&s,LS_MIXRF_OWNER_FIND,true,433920000,true));
    LS_CHECK(!ls_mixrf_cc_request(&s,LS_MIXRF_OWNER_FIND,true,100000000,false));
    LS_EQ_INT(s.cc_owner,LS_MIXRF_OWNER_NONE);
}

LS_CASE(nfc_work_refuses_active_radios)
{
    ls_mixrf_status_t s={.nfc=true,.cc=true};
    LS_CHECK(ls_mixrf_suite_allowed(&s));
    LS_CHECK(ls_mixrf_cc_request(&s,LS_MIXRF_OWNER_REC,true,433920000,false));
    LS_CHECK(!ls_mixrf_suite_allowed(&s));
    LS_CHECK(ls_mixrf_cc_request(&s,LS_MIXRF_OWNER_REC,false,0,false));
    s.capturing=true;LS_CHECK(!ls_mixrf_suite_allowed(&s));s.capturing=false;
    s.scanning=true;LS_CHECK(!ls_mixrf_suite_allowed(&s));s.scanning=false;
    s.scan_requested=true;LS_CHECK(!ls_mixrf_suite_allowed(&s));s.scan_requested=false;
    s.transmitting=true;LS_CHECK(!ls_mixrf_suite_allowed(&s));
}
static uint8_t registers[64];
static bool write_register(uint8_t reg,uint8_t value){registers[reg]=value;return true;}
LS_CASE(receive_clears_replay_frequency_offset)
{
    for(int capture=0;capture<2;capture++) {
        registers[0x0c]=0x7f;
        LS_CHECK(ls_mixrf_rx_profile(write_register,capture));
        LS_EQ_INT(registers[0x0c],0);
        LS_EQ_INT(registers[0x12],capture?0x30:0);
    }
}
LS_CASE(nrf_only_surveys_publish_each_completed_sweep)
{
    ls_mixrf_status_t s={.scanning=true,.samples=0};
    LS_CHECK(!ls_mixrf_survey_fresh(&s,0));
    for(uint32_t heard=0;heard<3;heard++) {
        s.sweeps=heard+1;
        LS_CHECK(ls_mixrf_survey_fresh(&s,heard));
        LS_CHECK(!ls_mixrf_survey_fresh(&s,s.sweeps));
    }
    s.scanning=false;LS_CHECK(!ls_mixrf_survey_fresh(&s,0));
}
