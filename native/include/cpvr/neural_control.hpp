#pragma once
#include "gpu.hpp"
namespace cpvr {
inline constexpr ULONG_PTR neural_input_marker=0x435056524E52ull;
struct NeuralStatus {bool requested{},commanded{},pending{},observer_available{},evaluating{};std::uint64_t successful_evaluations{};std::string message;};
class NeuralControl {
public:
    NeuralControl();~NeuralControl();
    bool start(HWND,const std::wstring&driver,Log);
    void request(bool);
    void update(std::uint64_t present_count,bool world_ready);
    NeuralStatus status() const;
    void stop();
private:struct Impl;std::unique_ptr<Impl> impl_;
};
}
