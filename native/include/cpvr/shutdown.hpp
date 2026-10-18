#pragma once
namespace cpvr {
struct GraphicsStopResult {bool generation{},xr{},source{};};
// Source admission is closed by the caller first. Continue other cleanup even
// when one owner conservatively retains unproven GPU work.
template<class GenerationStop,class XrStop,class SourceStop>
GraphicsStopResult stop_graphics_owners(GenerationStop generation,XrStop xr,SourceStop source){
    GraphicsStopResult result;
    result.generation=generation();result.xr=xr();result.source=source();
    return result;
}
}
