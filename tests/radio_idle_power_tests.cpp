// SPDX-License-Identifier: GPL-2.0-or-later
#include "radio_idle_power.hpp"
#include <iostream>
#include <stdexcept>
#include <string>
using namespace std::chrono_literals;
static void check(bool ok,const char *text){if(!ok)throw std::runtime_error(text);}
int main(){
    try{
        using Power=xone::RadioIdlePower;
        const Power::Time start{};
        unsigned suspends=0,resumes=0;std::string reason;
        bool fail_suspend=false,fail_resume=false;
        Power power(start,[&]{if(fail_suspend)throw std::runtime_error("suspend fault");++suspends;},[&](const char *why){if(fail_resume)throw std::runtime_error("resume fault");++resumes;reason=why;});
        power.update(start+29999ms,false);check(!power.armed() && !suspends,"Suspended before idle grace");
        power.update(start+30s,false);check(power.armed() && suspends==1,"Did not arm after idle grace");
        power.update(start+31s,false);check(suspends==1 && !resumes,"Standby tick caused USB churn");
        power.wake(start+32s,"wireless-event");check(!power.armed() && resumes==1 && reason=="wireless-event","Wireless wake was delayed");
        for(unsigned n=33;n<=100;++n)power.update(start+std::chrono::seconds(n),true);
        check(!power.armed() && suspends==1,"Connected/connecting/pairing client was suspended");
        power.update(start+129s,false);check(!power.armed(),"Ignored post-client grace");
        power.update(start+130s,false);check(power.armed(),"Idle did not resume after final client");
        power.update(start+159s,false);check(power.armed(),"Watchdog woke early");
        power.update(start+160s,false);check(!power.armed() && reason=="watchdog","Missed bounded fallback wake");
        power.update(start+161s,false);check(!power.armed(),"Fallback receive window too short");
        power.update(start+162s,false);check(power.armed(),"Fallback window stayed active indefinitely");
        power.update(start+163s,true);check(!power.armed() && reason=="client-or-pairing","Busy state did not force wake");
        fail_suspend=true;bool threw=false;
        try{power.update(start+200s,false);}catch(const std::exception &){threw=true;}
        check(threw && !power.armed(),"Suspend failure reported a sleeping radio");
        fail_suspend=false;power.update(start+201s,false);check(power.armed(),"Suspend retry state corrupted");
        fail_resume=true;threw=false;const auto revision=power.revision();
        try{power.wake(start+202s,"wireless-event");}catch(const std::exception &){threw=true;}
        check(threw && power.armed() && power.revision()==revision,"Resume failure reported active radio");
        fail_resume=false;power.wake(start+203s,"shutdown");check(!power.armed() && reason=="shutdown","Shutdown left radio armed");
        std::cout<<"Radio idle power transitions and fault tests passed\n";return 0;
    }catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}
}
