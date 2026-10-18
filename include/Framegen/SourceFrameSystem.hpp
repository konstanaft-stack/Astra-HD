#pragma once
#include "Framegen/SourceSceneEpoch.hpp"
#include <memory>

namespace cvr::framegen {
using SourceSceneSink=void (*)(const SourceSceneFrame&,void*);
struct SourceFrameSystemStatus {
    SourceSceneFrame scene;
    uint64_t frameBeginCalls{},registrations{},callbackErrors{};
    bool installed{};
};
// Register once during plugin load. Uses the existing RED4ext world update
// mechanism, not a timer or another render/VR thread.
void RegisterSourceFrameSystem();
// A copied owner lease keeps an already-dispatched callback alive while the
// root replaces/detaches its sink. The sink performs metadata work only.
void SetSourceSceneSink(SourceSceneSink,std::shared_ptr<void>);
SourceFrameSystemStatus ReadSourceFrameSystemStatus();
}
