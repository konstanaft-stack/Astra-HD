#include "Framegen/GameGenerationOwner.hpp"
#include <Windows.h>
namespace cvr::framegen {
namespace { GameGenerationSelection selection; }
void PublishGameGenerationChoice(unsigned provider,unsigned multiplier) noexcept {
    selection.Observe(GameGenerationProvider(provider),multiplier,GetTickCount64());
}
GameGenerationChoice ReadGameGenerationChoice() noexcept {return selection.Read(GetTickCount64());}
}
