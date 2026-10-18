#pragma once
#include <cstddef>
#include <cstdint>
namespace cvr::camera {
enum MainEyeTransportBit:uint32_t {
    TransportSelectedPresent=1u<<0,TransportSourcePresent=1u<<1,TransportRequested=1u<<2,
    TransportCurrentPresent=1u<<3,TransportRequestMatches=1u<<4,TransportLatchMatches=1u<<5,
    TransportEpochsMatch=1u<<6,TransportClockMatches=1u<<7,TransportOrdinalWithin=1u<<8,
    TransportSourceWithin=1u<<9,TransportUsable=1u<<10,TransportAdmitted=1u<<11,
    TransportInvalidated=1u<<12,TransportExternal=1u<<13,TransportViewMain=1u<<14
};
enum class MainEyeTransportJoin:uint32_t {Serialize,Blend,MainRead,Descriptor};
// One bounded snapshot. Read export Seq/packet/Seq, accepting equal even Seq.
// Counters count actual hook boundaries while MAIN source mode is requested.
// Selected fields/bits are one MainRead pre-policy snapshot; latchAfter is the
// same call's post-policy value. Other join addresses/counters are cumulative.
struct MainEyeTransportTelemetry {
    uint32_t version=1,bytes=192;
    uint64_t serializeCalls{},serializeLabelled{},blendCalls{},blendLabelled{};
    uint64_t mainReadCalls{},mainReadMissing{},mainReadUnusable{},mainReadAdmitted{},routeInvalidations{};
    uint64_t mainReadTransferred{},descriptorCalls{},descriptorTransferred{};
    uint64_t selectedSetup{},serializedSetup{},publishedSetup{};
    int64_t selectedSourceId=-1,currentSourceId=-1;
    uint64_t selectedLatch{},currentLatch{},selectedPoseId{},selectedComponent{};
    uint32_t route{},bits{};
    uint64_t latchAfter{};
};
static_assert(sizeof(MainEyeTransportTelemetry)==192);
static_assert(offsetof(MainEyeTransportTelemetry,selectedSetup)==104);
static_assert(offsetof(MainEyeTransportTelemetry,selectedSourceId)==128);
static_assert(offsetof(MainEyeTransportTelemetry,route)==176);
void RecordMainEyeTransportJoin(MainEyeTransportJoin join,uintptr_t address,bool sourceTransferred);
}
