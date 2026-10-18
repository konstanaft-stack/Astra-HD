#pragma once
#include "MenuPreview/CameraApplication.hpp"
#include "MenuPreview/InitialEditorFraming.hpp"
#include "MenuPreview/SpatialEditorPolicy.hpp"
#include "MenuPreview/EditorControllerPolicy.hpp"
namespace cvr::menu_preview {
void ClearInitialEditorFraming()noexcept;
InitialFramingResult PublishInitialEditorFraming(const Metadata&,uint64_t,uint64_t,uint64_t,const std::array<double,3>&,const std::array<double,4>&,const std::array<double,3>&)noexcept;
// Typed ownership is resolved by the existing preview scope. The native
// renderer receives the complete tracked eye camera; the game's own camera
// component remains available to its animation and camera controller.
bool ReadEditorComponentCamera(const Metadata&,std::array<uint8_t,32>&)noexcept;
void NoteSpatialEditorScope(const Metadata&)noexcept;
bool ComposeSpatialEditorFrame(const Metadata&,const Descriptor&,CameraApplication&)noexcept;
enum class EditorNativeComposeFailure:uint8_t {None,LeaseLifetime,LeaseSource,Screen,UiStereo,ScopeFresh,RegisteredSource,HeadAcquire,HeadValid,HeadOrigin,Clock,TypedCamera,Map,InitialFraming,PoseWrite,Scale,HalfIpd,Controls,Offset,OrbitView,OrbitFraming,OrbitPose,FrameCache,EyeMath,Exception};
const char* EditorNativeComposeFailureName(EditorNativeComposeFailure)noexcept;
bool ComposeSpatialEditorNativeFrame(const Lease&,const Descriptor&,CameraApplication&,EditorNativeComposeFailure* failure=nullptr)noexcept;
bool HandleSpatialEditorMouse(uint32_t message,uint64_t wparam)noexcept;
// Once per actual XR input sample. A true result claims only the left
// grip/stick/click for the active editor; caller routes other inputs normally.
bool HandleSpatialEditorController(const EditorControllerSample&)noexcept;
void NoteSpatialEditorControlsPresented(uint64_t revision)noexcept;
void ClearSpatialEditor()noexcept;
}
