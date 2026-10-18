#include "cpvr/core.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace cpvr {
Validation apply_diagnostic_generation(Settings& settings,std::string_view text){
    if(text.size()<4||text.size()>49||text[0]<'2'||text[0]>'6'||text[1]!='|')
        return {false,"Expected explicit multiplier2..6|LEFT-RIGHT-cycle"};
    auto candidate=settings;
    candidate.multiplier=unsigned(text[0]-'0');candidate.frame_generation=true;
    candidate.display_pattern=DisplayPattern::Custom;candidate.eye_pattern=std::string(text.substr(2));
    const auto valid=validate_settings(candidate);
    if(valid.valid)settings=std::move(candidate);
    return valid;
}
bool SettingsControl::observe(const AppliedState&state){
    std::lock_guard lock(mutex_);
    if(!state.revision||state.revision!=applied_.revision)return false;
    applied_=state;return true;
}
namespace {
constexpr std::uint32_t dimension_limit=16384; // D3D12 2D texture limit.
bool finite_range(float value, float low, float high) {
    return std::isfinite(value) && value>=low && value<=high;
}
bool valid_hotkey(int value) {
    // Single keyboard keys only: no mouse buttons, modifiers, reserved VKs or chords.
    if ((value>=0x30 && value<=0x39) || (value>=0x41 && value<=0x5a) ||
        (value>=0x60 && value<=0x6f) || (value>=0x70 && value<=0x87) ||
        (value>=0x21 && value<=0x28) || (value>=0xba && value<=0xc0) ||
        (value>=0xdb && value<=0xdf)) return true;
    switch (value) {
    case 0x08: case 0x09: case 0x0d: case 0x13: case 0x14: case 0x1b:
    case 0x20: case 0x2c: case 0x2d: case 0x2e: case 0x90: case 0x91: case 0xe2:
        return true;
    default: return false;
    }
}
double nominal_scale(const Settings& settings) {
    // These are UI estimates, never NGX's actual input dimensions.
    switch (settings.sr_mode) {
    case SrMode::Quality: return 2.0/3.0;
    case SrMode::Balanced: return 0.58;
    case SrMode::Performance: return 0.5;
    case SrMode::UltraPerformance: return 1.0/3.0;
    case SrMode::Dlaa: return 1.0;
    case SrMode::Custom: return settings.custom_render_scale;
    }
    return 0;
}
bool valid_fov(const Fov& fov) {
    constexpr double half_pi=std::numbers::pi_v<double>/2;
    for (auto angle : {fov.left,fov.right,fov.up,fov.down})
        if (!std::isfinite(angle) || angle<=-half_pi || angle>=half_pi) return false;
    return fov.left<fov.right && fov.down<fov.up;
}
std::uint32_t aligned_dimension(double value) {
    return static_cast<std::uint32_t>(std::ceil(value/8.0)*8.0);
}
}

Validation validate_settings(const Settings& settings) {
    auto fail=[](const char* message) { return Validation{false,message}; };
    if (!finite_range(settings.ppd,5,50))
        return fail("PPD должен быть конечным числом от 5 до 50.");
    switch (settings.sr_mode) {
    case SrMode::Quality: case SrMode::Balanced: case SrMode::Performance:
    case SrMode::UltraPerformance: case SrMode::Dlaa: case SrMode::Custom: break;
    default: return fail("Неизвестный режим DLSS SR.");
    }
    switch (settings.sr_model) {
    case SrModel::Game: case SrModel::K: case SrModel::L: case SrModel::M: break;
    default: return fail("Неизвестная модель SR.");
    }
    if (!finite_range(settings.custom_render_scale,0.1f,1))
        return fail("Пользовательский масштаб SR должен быть от 0,1 до 1.");
    if (!finite_range(settings.sharpness,0,1))
        return fail("Резкость должна быть конечным числом от 0 до 1.");
    if (!finite_range(settings.neural_strength,0,1))
        return fail("Сила NR должна быть конечным числом от 0 до 1.");
    if (settings.multiplier<2 || settings.multiplier>6)
        return fail("Множитель MFG должен быть от 2 до 6.");
    switch(settings.display_pattern){
    case DisplayPattern::Chessboard: case DisplayPattern::StereoPairs: case DisplayPattern::Custom: break;
    default: return fail("Неизвестная схема показа глаз.");
    }
    if(settings.display_pattern==DisplayPattern::Custom){
        const auto& pattern=settings.eye_pattern;
        if(pattern.empty())return fail("Пользовательская схема должна содержать от 1 до 16 пар RR/GG/GR/RG.");
        std::size_t begin=0,tokens=0;
        for(;;){
            const auto comma=pattern.find(',',begin);
            const auto end=comma==std::string::npos?pattern.size():comma;
            if(end-begin!=2||(pattern[begin]!='R'&&pattern[begin]!='G')||
               (pattern[begin+1]!='R'&&pattern[begin+1]!='G'))
                return fail("Схема: RR, GG, GR или RG через запятую, без пробелов. Первый символ — левый глаз, второй — правый.");
            if(++tokens>16)return fail("Пользовательская схема допускает не больше 16 пар.");
            if(comma==std::string::npos)break;
            begin=comma+1;
        }
    }
    switch (settings.cadence) {
    case Cadence::StereoPairs: case Cadence::AlternateEyes: case Cadence::MixedPairs: break;
    default: return fail("Неизвестный кадровый ритм.");
    }
    if (settings.cadence==Cadence::MixedPairs) {
        const auto& pattern=settings.pair_pattern;
        if (pattern.empty() || pattern.size()>64)
            return fail("Смешанный цикл должен содержать от 1 до 64 символов R/G.");
        if (pattern.find_first_not_of("RG")!=std::string::npos)
            return fail("В смешанном цикле разрешены только R и G.");
        const auto first_real=pattern.find('R');
        if (first_real==std::string::npos)
            return fail("Цикл должен содержать настоящий кадр R.");
        if (settings.frame_generation) {
            if (pattern.find('G')==std::string::npos)
                return fail("При включённом MFG смешанный цикл должен содержать R и G.");
            std::size_t generated_run=0;
            // Start just after a real frame and include the wrap back to it.
            for (std::size_t offset=1;offset<=pattern.size();++offset) {
                if (pattern[(first_real+offset)%pattern.size()]=='R') generated_run=0;
                else if (++generated_run>5)
                    return fail("Между R допускается не больше пяти G, включая стык цикла.");
            }
        }
    }
    if (!valid_hotkey(settings.panel_key) || !valid_hotkey(settings.fg_key) ||
        !valid_hotkey(settings.nr_key))
        return fail("Выберите отдельные клавиши клавиатуры без модификаторов.");
    if (settings.panel_key==settings.fg_key || settings.panel_key==settings.nr_key ||
        settings.fg_key==settings.nr_key)
        return fail("Клавиши панели, MFG и NR должны различаться.");
    return {true,{}};
}

std::optional<ResolutionPlan> plan_resolution(const Settings& settings,
        const std::array<Fov,2>& fovs, Extent maximum, std::string& error) {
    error.clear();
    auto validation=validate_settings(settings);
    if (!validation.valid) { error=validation.message; return std::nullopt; }
    if (!maximum.width || !maximum.height || maximum.width>dimension_limit ||
        maximum.height>dimension_limit) {
        error="Недопустимый предел размера D3D12-текстуры глаза."; return std::nullopt;
    }
    ResolutionPlan result; result.ppd=settings.ppd;
    const double focal_pixels=settings.ppd*180.0/std::numbers::pi_v<double>;
    const double scale=nominal_scale(settings);
    for (std::size_t eye=0;eye<fovs.size();++eye) {
        const auto& fov=fovs[eye];
        if (!valid_fov(fov)) {
            error=eye==0 ? "Недопустимый FOV левого глаза." : "Недопустимый FOV правого глаза.";
            return std::nullopt;
        }
        const double width=focal_pixels*(std::tan(static_cast<double>(fov.right))-
                                         std::tan(static_cast<double>(fov.left)));
        const double height=focal_pixels*(std::tan(static_cast<double>(fov.up))-
                                          std::tan(static_cast<double>(fov.down)));
        // Check in floating point before conversion to avoid integer overflow for wide FOVs.
        if (!std::isfinite(width) || !std::isfinite(height) || width<=0 || height<=0 ||
            width>maximum.width || height>maximum.height) {
            error="Выбранный PPD превышает предел текстуры глаза; PPD не уменьшен автоматически.";
            return std::nullopt;
        }
        const Extent output{aligned_dimension(width),aligned_dimension(height)};
        if (output.width>maximum.width || output.height>maximum.height) {
            error="Размер после округления вверх до 8 превышает предел текстуры глаза.";
            return std::nullopt;
        }
        result.output[eye]=output;
        result.estimated_input[eye]={aligned_dimension(output.width*scale),
                                     aligned_dimension(output.height*scale)};
    }
    return result;
}

Settings SettingsControl::requested() const {
    std::lock_guard lock(mutex_); return requested_;
}
AppliedState SettingsControl::applied() const {
    std::lock_guard lock(mutex_); return applied_;
}
std::uint64_t SettingsControl::requested_revision() const {
    std::lock_guard lock(mutex_); return revision_;
}
Validation SettingsControl::request(const Settings& settings) {
    auto validation=validate_settings(settings);
    if (!validation.valid) return validation;
    std::lock_guard lock(mutex_);
    if (revision_==std::numeric_limits<std::uint64_t>::max())
        return {false,"Исчерпан счётчик запросов настроек."};
    requested_=settings;
    pending_=SettingsRequest{++revision_,settings};
    return validation;
}
std::optional<SettingsRequest> SettingsControl::take_pending() {
    std::lock_guard lock(mutex_);
    if (in_flight_ || !pending_) return std::nullopt;
    auto result=std::move(pending_);
    pending_.reset(); in_flight_=result->revision;
    return result;
}
bool SettingsControl::acknowledge(std::uint64_t revision, const AppliedState& state) {
    std::lock_guard lock(mutex_);
    if (!in_flight_ || *in_flight_!=revision) return false;
    in_flight_.reset(); // The old resource transaction finished, even if superseded.
    if (revision!=revision_) return false;
    applied_=state; applied_.revision=revision;
    return true;
}

bool Hotkeys::key(SettingsControl& control, int virtual_key, bool down, bool foreground) {
    if (virtual_key<0 || virtual_key>=static_cast<int>(held_.size())) return false;
    const bool previously_held=held_[virtual_key];
    held_[virtual_key]=down; // Background key-downs cannot become a fresh foreground press.
    if (!foreground || !down || previously_held) return false;
    auto settings=control.requested();
    if (virtual_key==settings.panel_key) {
        panel_visible_=!panel_visible_; return true;
    }
    if (virtual_key==settings.fg_key) settings.frame_generation=!settings.frame_generation;
    else if (virtual_key==settings.nr_key) settings.neural_rendering=!settings.neural_rendering;
    else return false;
    return control.request(settings).valid;
}
}
