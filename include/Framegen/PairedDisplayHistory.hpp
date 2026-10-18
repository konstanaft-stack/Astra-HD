#pragma once
#include <cstdint>

namespace cvr::framegen {
// Generation still requires its baseline calibration. Keep real output until
// that baseline has settled, so small native FOV residuals do not repeatedly
// destroy and recreate generation history while scanning or entering zoom.
struct PairedDisplayHistory {
    bool realOnly{};
    int64_t stableSince{};
    bool Update(bool baseline,int64_t displayTime) {
        if(!baseline){stableSince=0;realOnly=true;}
        else if(realOnly&&displayTime>0){
            if(!stableSince||displayTime<stableSince)stableSince=displayTime;
            else if(displayTime-stableSince>=250000000){realOnly=false;stableSince=0;}
        }
        return realOnly;
    }
};
}
