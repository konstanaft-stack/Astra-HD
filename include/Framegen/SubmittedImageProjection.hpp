#pragma once
#include "Camera/ImagePoseIdentity.hpp"
#include "Framegen/FrameProjectionMetadata.hpp"
namespace cvr::framegen {
enum class SubmittedProjectionReason:uint32_t {Ready,ImageUnknown,IdentityUnknown,CameraMissing,IdentityMismatch};
// The caller passes the actual submitted-image ledger label, never a current
// global pose or an arbitrary queue. This supplies display metadata only.
std::optional<FrameProjectionMetadata> AcquireSubmittedImageProjection(
    const cvr::camera::ImagePoseIdentity&,SubmittedProjectionReason* reason=nullptr);
}
