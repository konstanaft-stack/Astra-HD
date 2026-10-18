#pragma once
#include "Framegen/PresentationStatus.hpp"
#include <atomic>
#include <cstdint>
namespace cvr::framegen {
enum class GameGenerationProvider : unsigned { Unknown, Off, Nvidia, Other };
struct GameGenerationChoice { bool known{};GameGenerationProvider provider{};unsigned multiplier{};uint64_t observedAt{}; };
class GameGenerationSelection {
    std::atomic<uint64_t> packet_{};
public:
    void Observe(GameGenerationProvider provider,unsigned multiplier,uint64_t now) noexcept {
        if(unsigned(provider)>unsigned(GameGenerationProvider::Other))provider=GameGenerationProvider::Unknown;
        if(provider!=GameGenerationProvider::Nvidia||multiplier<2||multiplier>4)multiplier=0;
        if(now>(UINT64_MAX>>8))provider=GameGenerationProvider::Unknown;
        packet_.store((now<<8)|(uint64_t(multiplier)<<3)|unsigned(provider),std::memory_order_release);
    }
    GameGenerationChoice Read(uint64_t now) const noexcept {
        const auto p=packet_.load(std::memory_order_acquire),at=p>>8;
        const auto provider=GameGenerationProvider(p&7);
        if(!at||now<at||now-at>3000||provider==GameGenerationProvider::Unknown)return {};
        return {true,provider,unsigned((p>>3)&31),at};
    }
};
enum class GenerationOwner { Owned, Game, Unknown };
inline GenerationOwner ResolveGenerationOwner(bool nativeAdapter,GameGenerationChoice choice,
        NativePolicyStatus native,bool nativeWasEnabled) noexcept {
    if(native.known&&native.enabled)return GenerationOwner::Game;
    if(choice.known&&choice.provider!=GameGenerationProvider::Off)return GenerationOwner::Game;
    if(!nativeAdapter)return GenerationOwner::Owned;
    if(!choice.known)return GenerationOwner::Unknown;
    if(native.known&&!native.enabled)return GenerationOwner::Owned;
    return nativeWasEnabled?GenerationOwner::Unknown:GenerationOwner::Owned;
}
// Read-only game-settings channel. This choice reserves a requested owner; it
// does not prove that any backend has produced or submitted generated frames.
void PublishGameGenerationChoice(unsigned provider,unsigned multiplier) noexcept;
GameGenerationChoice ReadGameGenerationChoice() noexcept;
}
