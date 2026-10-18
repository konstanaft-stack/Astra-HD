#pragma once
namespace cvr::framegen {
inline bool NeedGuideCopies(bool projectionCapture,bool aer,bool ownedFg,bool nativeRequested,bool nativeKnown,bool nativeEnabled){
    return projectionCapture&&(aer||ownedFg||(nativeRequested&&(!nativeKnown||nativeEnabled)));
}
}
