#include "Overlay/PpdCalibration.hpp"
#include <cmath>
#include <iostream>
#include <limits>

namespace {
int checks{};
void require(bool pass,const char* message) {
    ++checks;
    if(!pass) {std::cerr<<message<<'\n';std::exit(1);}
}
bool close(float a,float b,float epsilon=0.002f){return std::abs(a-b)<=epsilon;}
}
int main() {
    using namespace cvr::ppd;
    // 2000x1000 covers +/-2 horizontal tangents and +/-1 vertical:
    // exactly 500 pixels per tangent, hence 500*pi/180 pixels/degree.
    const TangentExtent symmetric{1,1,2,2};
    const auto base=CalibratePpd(2000,1000,{}, {symmetric,symmetric});
    require(base.valid,"symmetric calibration rejected");
    require(close(base.pixelsPerTangent,500),"physical focal density differs");
    require(close(base.ppd,8.72664642f),"central PPD differs from analytic derivative");
    require(close(base.nominalWidth,2000)&&close(base.nominalHeight,1000),"fitted extents differ");
    const auto doubled=PreviewCalibratedPpd(base,2*base.ppd);
    require(doubled.valid&&doubled.width==4000&&doubled.height==2000,"doubling density did not double dimensions");
    const auto targetTen=PreviewCalibratedPpd(base,10);
    require(targetTen.valid&&targetTen.width==2288&&targetTen.height==1152,"nearest16 preview differs");
    // Maxima are taken independently over both eyes, rather than averaging FOV.
    const auto asym=CalibratePpd(1600,1800,{.4f,.6f,.1f,0},
        {TangentExtent{1,.8f,1.2f,1.2f},TangentExtent{1.1f,.9f,1,1.4f}});
    require(asym.valid&&close(asym.pixelsPerTangent,1600.0f/3.0f),"asymmetric limiting direction wrong");
    require(close(asym.nominalWidth,1600)&&close(asym.nominalHeight,1200),"aspect fitting failed");
    const PpdCalibration ties{10,1032,1016,10/0.01745329238474369f,true};
    const auto rounded=PreviewCalibratedPpd(ties,10);
    require(rounded.valid&&rounded.width==1040&&rounded.height==1024,"positive half ties must round up");
    const auto forced=PreviewCalibratedPpd(base,base.ppd,{.1f,10,1,false});
    require(forced.valid&&forced.width==2000&&forced.height==2000,"forced aspect must extend rather than crop");
    const auto bounded=PreviewCalibratedPpd(base,50,{.1f,10,0,true});
    require(bounded.valid&&bounded.width==4096&&bounded.height==2048,"4096 optional cap incorrect");
    require(!CalibratePpd(0,1000,{}, {symmetric,symmetric}).valid,"zero dimension accepted");
    require(!CalibratePpd(1000,1000,{.5f,.5f,.5f,0},{symmetric,symmetric}).valid,"empty usable edge accepted");
    require(!CalibratePpd(1000,1000,{}, {symmetric,TangentExtent{0,1,1,1}}).valid,"invalid eye tangent accepted");
    require(!CalibratePpd(1000,1000,{}, {symmetric,TangentExtent{NAN,1,1,1}}).valid,"NaN FOV accepted");
    require(!PreviewCalibratedPpd({},13).valid,"unobserved calibration accepted");
    require(!PreviewCalibratedPpd(base,std::numeric_limits<float>::infinity()).valid,"infinite target accepted");
    require(!PreviewCalibratedPpd(base,50000).valid,"unbounded texture size accepted");
    std::cout<<checks<<" calibrated PPD checks PASS\n";
}
