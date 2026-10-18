#include "Overlay/PpdResolution.hpp"
#include <cmath>
#include <limits>
#include <cstdio>
#include <cstdlib>

static void check(bool ok,const char* message){if(!ok){std::fprintf(stderr,"FAIL: %s\n",message);std::exit(1);}}
int main(){
    using namespace cvr::ppd;
    const auto current=ForWidth(2064);
    check(current.width==2064 && current.height==2296,"preserve the current Quest3 target exactly");
    check(ForWidth(3096).height==3448,"half-up height agrees with generated assets");
    check(std::abs(CenterPpd(2064,108)-13.087) < .01,"central angular sampling, not average width/FOV");
    check(!FromPpd(std::numeric_limits<double>::quiet_NaN(),108).valid,"reject NaN");
    check(!FromPpd(15,0).valid && !FromPpd(15,179).valid,"reject unknown or invalid FOV");
    check(!FromPpd(1,108).valid && !FromPpd(90,108).valid,"do not silently lower requested PPD to fit bounds");
    int previous=0;
    for(int step=50;step<=380;++step){
        const auto r=FromPpd(step/10.0,108);
        check(r.valid,"all slider values need a concrete target");
        check(r.width%8==0 && r.height%8==0,"both dimensions aligned to8");
        check(r.width>previous,"0.1PPD steps must change real resolution");
        check(std::abs(CenterPpd(r.width,108)-step/10.0)<.026,"quantization within half an8-pixel step");
        check(std::abs(double(r.width)/r.height-double(2064)/2296)<.005,"keep Quest3 projection aspect");
        previous=r.width;
    }
    check(FromPpd(14.7,108).width!=2064 && FromPpd(14.7,108).width!=2296,"intermediate choice is not snapped to old presets");
    check(FromPpd(15,100).width<FromPpd(15,108).width,"FOV override participates in PPD conversion");
    double parsed=0;
    check(ParsePpd(L"14,7",parsed) && std::abs(parsed-14.7)<1e-9,"Russian decimal comma");
    check(ParsePpd(L" 14.70 ",parsed) && std::abs(parsed-14.7)<1e-9,"decimal point and surrounding whitespace");
    check(!ParsePpd(L"14x",parsed) && !ParsePpd(L"",parsed) && !ParsePpd(L"nan",parsed),"reject malformed input");
    std::puts("PASS: central PPD,331 intermediate targets, alignment, aspect, limits, FOV and decimal input");
}
