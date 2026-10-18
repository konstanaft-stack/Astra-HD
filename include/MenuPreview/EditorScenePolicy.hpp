#pragma once
#include <cstdint>
#include "MenuPreview/NativePreviewSource.hpp"
namespace cvr::menu_preview {
inline bool EditorSceneRecordingSubmissionValid(bool open,bool successfulClose,uint64_t current,uint64_t recorded){return !open&&successfulClose&&current&&current==recorded;}
inline bool EditorSceneExecutionAllowsFinalize(NativePreviewMode mode,bool nativeExecutionValid,uint64_t uiSubmission)noexcept{
 return mode==NativePreviewMode::NativeEditorDual?nativeExecutionValid:
  mode==NativePreviewMode::LegacyOneView&&uiSubmission!=0;
}
template<class Image,class Receipt> bool EditorSceneMatches(const Image& image,const Receipt& receipt){
 if(!image.application||!image.producerUiSubmission||image.producerUiSubmission>receipt.uiSubmission||!image.queue)return false;
 const auto& app=*image.application;const auto& a=app.metadata;const auto& b=receipt.scope;
 return a.screen==2&&b.screen==2&&a.screenEpoch&&a.screenEpoch==b.screenEpoch&&a.controllerIdentity&&a.controllerIdentity==b.controllerIdentity&&
  a.setupID==b.setupID&&a.componentIdentity&&a.componentIdentity==b.componentIdentity&&a.virtualCameraNameHash&&a.virtualCameraNameHash==b.virtualCameraNameHash&&
  app.frame.eye<2&&app.frame.eye==receipt.eye&&app.frame.nativeFrame==receipt.nativeFrame&&app.frame.sourceSequence&&app.frame.sourceSequence==receipt.sourceSequence&&
  app.localPoseRevision&&app.localPoseRevision==receipt.cameraRevision&&app.projection.valid&&receipt.projection.valid&&app.projection.origin&&app.projection.origin==receipt.projection.origin;
}
}
