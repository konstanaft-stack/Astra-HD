#pragma once
#include <array>
#include <cstdint>
namespace cvr::framegen {
enum class PairedGeometryReason:uint32_t {
 NotExamined,Ready,SerialMissing,OriginMissing,NotStereo,ResourceEpochMissing,LeftColorMissing,RightColorMissing,SameColor,
 LeaseMissing,LeaseOwnerMissing,LeaseReleaseMissing,CopyNotScheduled,ColorState,ColorDevice,
 ProjectionMissing,LeaseResource,SourceSerialMissing,SourceSerialMismatch,PoseMissing,PoseWriteMissing,PoseMismatch,SpaceMismatch,
 NativeFrameUnknown,ReferenceOpticsInvalid,EngineViewMismatch,EngineViewUnsupported,ProjectionView,ProjectionFrame,ProjectionPose,
 ProjectionPoseWrite,ProjectionOrigin,ProjectionClock,ProjectionApplicationTime,ProjectionContentTime,ProjectionPredictionTime,
 ProjectionQueueMissing,ProjectionColorDevice,ProjectionFenceDevice,PairNativeFrame,
 TextureDimension,WidthMissing,HeightMissing,WidthOverflow,HeightLimit,ArraySize,MipLevels,SampleCount,SampleQuality,ColorFormat,
 ProjectionInvalid,ReferenceFit,ExpectedFov,ScaleXNotFinite,ScaleYNotFinite,ScaleXMinimum,ScaleXMaximum,ScaleYMinimum,ScaleYMaximum,
 AnisotropicScale,CenterX,CenterY,CropLeft,CropTop,CropWidth,CropHeight,CropRight,CropBottom,TangentInvalid,TangentNonPositive,
 MainViewMissing,VrcamViewMissing,PairScale
};
// Scalars only. Each eye knownBits: metadata=1, shape=2, projection=4,
// fit=8, expected FOV=16, scale/center=32, crop=64, tangents=128.
struct PairedGeometryDiagnosticEye {
 uint32_t knownBits{},width{},height{};
 std::array<float,6> projection{}; // p00,p11,p20,p21,p23,p32, only after Projection passed.
 std::array<float,4> expectedFov{},crop{},tangents{};
 double expectedX{},expectedY{},scaleX{},scaleY{},centerX{},centerY{},computedScale{};
};
struct PairedGeometryDiagnostic {
 PairedGeometryReason reason=PairedGeometryReason::NotExamined;
 uint32_t eye=UINT32_MAX,knownBits{},component=UINT32_MAX;
 std::array<PairedGeometryDiagnosticEye,2> eyes{};
};
}
