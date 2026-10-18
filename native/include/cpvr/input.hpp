#pragma once
#include "body.hpp"
namespace cpvr {
struct HandButtons {
    float trigger{}, squeeze{};
    std::array<float,2> stick{};
    bool primary{}, secondary{}, stick_click{}, active{};
};
struct InputSample {
    BodyTracking tracking;
    std::array<HandButtons,2> hands;
    bool focused{};
};
}
