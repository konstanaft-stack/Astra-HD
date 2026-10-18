#pragma once
#include <cstdint>
namespace cvr::camera {
enum MainEyeWriteBit:uint32_t {
    WriteSourcePresent=1u<<0,WritePoseLocated=1u<<1,WriteHaveQuaternion=1u<<2,WriteOrientationWritten=1u<<3,
    WritePositionReadable=1u<<4,WriteTranslationWanted=1u<<5,WriteHeadDeltaReady=1u<<6,WriteTranslationValid=1u<<7,
    WriteFreshDeltaEnabled=1u<<8,WriteAnchorRead=1u<<9,WriteTranslationLabelKnown=1u<<10,WriteDirty=1u<<11,
    WritePositionWritten=1u<<12,WritePoseValid=1u<<13,WritePoseIdPresent=1u<<14,WritePoseTimeOriginMatch=1u<<15,
    WriteReceiptUsable=1u<<16,WriteUseSource=1u<<17,WriteIpdEnabled=1u<<18,WriteIpdNonzero=1u<<19,
    WriteRightVectorValid=1u<<20,WriteBdMainSkip=1u<<21,WriteSourceApplied=1u<<22,WritePublisherCalled=1u<<23,
    WritePublisherHasSource=1u<<24,WriteBdOnScene=1u<<25
};
enum MainEyeLocateBit:uint32_t {
    LocateComponentPresent=1u<<0,LocateComponentIsMain=1u<<1,LocateReceiptPresent=1u<<2,
    LocateSourceStamped=1u<<3,LocateSourceRead=1u<<4,LocatePoseIdMatch=1u<<5,LocateHasXR=1u<<6
};
struct MainEyeWriteObservation {
    uint32_t bits{};int64_t sourceId=-1;uint64_t poseId{};int64_t requestedTimeNs{},writtenTimeNs{};
    void Set(MainEyeWriteBit bit,bool value){if(value)bits|=bit;}
};
struct MainEyeWriteTelemetry {
    uint32_t version=1,bytes=128;
    uint64_t calls{},publishCalls{},sourceCalls{},orientationMissing{},translationMissing{},poseIdMissing{},sourceMismatch{},positionFailed{};
    uint32_t writeBits{},locateBits{};
    int64_t sourceId=-1;uint64_t writtenPoseId{};int64_t requestedTimeNs{},writtenTimeNs{};
    uint64_t locateCalls{},locateMissingSource{};
};
static_assert(sizeof(MainEyeWriteTelemetry)==128);
void RecordMainEyeWrite(const MainEyeWriteObservation&);
void RecordMainEyeLocate(uint32_t bits);
}
