#pragma once
struct ImGuiContext;
inline thread_local ImGuiContext* cpvr_imgui_context=nullptr;
#define GImGui cpvr_imgui_context
