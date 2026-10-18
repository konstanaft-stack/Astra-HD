// Typed NodeRef resolution on the script/game thread. No IDs travel through
// Lua numbers or shared floats. Camera selection is native and fails closed
// unless the owner has exactly one enabled RTT camera.
native func VRPreviewEditorActorAnchor(controller: ref<inkPuppetPreviewGameController>, actor: ref<Entity>, player: ref<Entity>, cameraOwner: ref<Entity>, epoch: Uint32, position: Vector4, orientation: Quaternion, staging: Vector4) -> Bool;
native func VRPreviewScopeBegin() -> Uint32;
native func VRPreviewScopeClear(epoch: Uint32) -> Bool;
native func VRPreviewLocalHalfIpd() -> Float;
native func VRPreviewStereoUiEnabled() -> Bool;
native func VRPreviewStereoUiPairSubmitted(epoch: Uint32) -> Bool;
native func VRPreviewStereoUiCanAdvance(epoch: Uint32, eye: Int32) -> Bool;
native func VRPreviewLocalClock() -> Float;
native func VRPreviewLocalPrepare(controller: ref<inkPuppetPreviewGameController>, epoch: Uint32, setup: Int32) -> Bool;
native func VRPreviewLocalPublish(controller: ref<inkPuppetPreviewGameController>, epoch: Uint32, setup: Int32, eye: Int32) -> Bool;
native func VRPreviewLocalReset(epoch: Uint32) -> Bool;
native func VRPreviewWidgetAliasEnabled() -> Bool;
native func VRPreviewObserveTypedImage(controller: ref<inkPuppetPreviewGameController>, epoch: Uint32, image: ref<inkImage>) -> Bool;
native func VRPreviewLocalCamera(controller: ref<inkPuppetPreviewGameController>, epoch: Uint32, setup: Int32) -> ref<IPlacedComponent>;
native func VRPreviewScopeUpdate(controller: ref<inkPuppetPreviewGameController>, cameraOwner: ref<Entity>, camera: ref<IComponent>, epoch: Uint32, screen: Int32, ready: Bool, branch: Int32) -> Bool;

// CET can wrap the same engine controller in different Lua userdata per event.
// Compare engine references here, without exporting pointer values to Lua.
func VRPreviewSameController(a: ref<inkGameController>, b: ref<inkGameController>) -> Bool {
  return IsDefined(a) && IsDefined(b) && a == b;
}

func VRPreviewResolveAndPublish(controller: ref<inkInventoryPuppetPreviewGameController>, epoch: Uint32, screen: Int32, ready: Bool) -> Bool {
  if !IsDefined(controller) || !ready {
    VRPreviewScopeUpdate(controller, null, null, epoch, screen, false, 0);
    return false;
  };
  let player = controller.GetPlayerControlledObject();
  if !IsDefined(player) {
    VRPreviewScopeUpdate(controller, null, null, epoch, screen, false, 0);
    return false;
  };
  let root: GlobalNodeRef;
  let resolved = ResolveNodeRef(controller.cameraRef, root);
  if !GlobalNodeRef.IsDefined(resolved) {
    VRPreviewScopeUpdate(controller, null, null, epoch, screen, false, 0);
    return false;
  };
  let owner = GameInstance.FindEntityByID(player.GetGame(), Cast<EntityID>(resolved)) as Entity;
  if !IsDefined(owner) {
    VRPreviewScopeUpdate(controller, null, null, epoch, screen, false, 0);
    return false;
  };
  return VRPreviewScopeUpdate(controller, owner, null, epoch, screen, true, 0);
}

class VRPreviewAppearanceSelection {
  public let setup: Int32;
  public let slotName: CName;
}
func VRPreviewReadAppearanceSelection(controller: ref<inkCharacterCreationPuppetPreviewGameController>) -> ref<VRPreviewAppearanceSelection> {
  if !IsDefined(controller) { return null; };
  let puppet=controller.GetGamePuppet();
  if !IsDefined(puppet) { return null; };
  let system=GameInstance.GetCharacterCustomizationSystem(puppet.GetGame());
  if !IsDefined(system) || system.GetPuppetPreviewGameController()!=controller { return null; };
  let bank=controller.cameraController;
  let setup=Cast<Int32>(bank.activeSetup);
  if setup<0 || setup>=ArraySize(bank.cameraSetup) || Equals(bank.cameraSetup[setup].slotName,n"") { return null; };
  let selected=new VRPreviewAppearanceSelection();selected.setup=setup;selected.slotName=bank.cameraSetup[setup].slotName;return selected;
}
func VRPreviewAppearanceReady(controller: ref<inkCharacterCreationPuppetPreviewGameController>) -> Bool {
  return IsDefined(VRPreviewReadAppearanceSelection(controller));
}

func VRPreviewResolveAppearanceAndPublish(controller: ref<inkCharacterCreationPuppetPreviewGameController>, epoch: Uint32, ready: Bool) -> Bool {
  if !ready || !VRPreviewAppearanceReady(controller) {
    VRPreviewScopeUpdate(controller, null, null, epoch, 2, false, 0);return false;
  };
  let puppet=controller.GetGamePuppet();
  if !IsDefined(puppet) { VRPreviewScopeUpdate(controller, null, null, epoch, 2, false, 0);return false; };
  let selectedCamera: NodeRef;
  let branch: Int32=0;
  let gender=puppet.GetResolvedGenderName();
  if Equals(gender,n"Female") { selectedCamera=controller.femaleCamera01Ref;branch=1; };
  if Equals(gender,n"Male") { selectedCamera=controller.maleCamera01Ref;branch=2; };
  if branch==0 { VRPreviewScopeUpdate(controller, null, null, epoch, 2, false, 0);return false; };
  let root: GlobalNodeRef;
  let resolved=ResolveNodeRef(selectedCamera,root);
  if !GlobalNodeRef.IsDefined(resolved) { VRPreviewScopeUpdate(controller, null, null, epoch, 2, false, 0);return false; };
  let owner=GameInstance.FindEntityByID(puppet.GetGame(),Cast<EntityID>(resolved)) as Entity;
  if !IsDefined(owner) { VRPreviewScopeUpdate(controller, null, null, epoch, 2, false, 0);return false; };
  let published=VRPreviewScopeUpdate(controller,owner,null,epoch,2,true,branch);
  if !published { return false; };
  let player=controller.GetPlayerControlledObject() as PlayerPuppet;
  if IsDefined(player) && puppet==player { VRPreviewScopeClear(epoch);return false; };
  return VRPreviewEditorActorAnchor(controller,puppet,player,owner,epoch,puppet.GetWorldPosition(),puppet.GetWorldOrientation(),owner.GetWorldPosition());
}

// Single full-body constant-left diagnostic. Imported setters execute here on
// the script/game callback, never inside a render hook. No eye receipt is made.
class VRPreviewLocalDiagnostic {
  public let controller: ref<inkPuppetPreviewGameController>;
  public let camera: ref<IPlacedComponent>;
  public let epoch: Uint32;
  public let setup: Int32;
  public let half: Float;
  public let eye: Int32;
  public let stereo: Bool;
  public let published: Bool;
  public let baseline: Vector4;
  public let orientation: Quaternion;
  public let stableWorld: Matrix;
  public let worldBaseline: Vector4;
  public let expectedWorld: Vector4;
  public let appliedPosition: Vector4;
  public let stable: Int32;
  public let startedAt: Float;
  public let appliedAt: Float;
  public let applied: Bool;
  public let ready: Bool;
  public let finished: Bool;
  public let status: String;
}

func VRPreviewLocalNear(a: Vector4, b: Vector4) -> Bool {
  return AbsF(a.X-b.X)<0.0001 && AbsF(a.Y-b.Y)<0.0001 && AbsF(a.Z-b.Z)<0.0001 && AbsF(a.W-b.W)<0.0001;
}
func VRPreviewLocalDot(a: Vector4, b: Vector4) -> Float {
  return a.X*b.X+a.Y*b.Y+a.Z*b.Z;
}
func VRPreviewLocalRigid(m: Matrix) -> Bool {
  let x = Matrix.GetAxisX(m);
  let y = Matrix.GetAxisY(m);
  let z = Matrix.GetAxisZ(m);
  let determinant=x.X*(y.Y*z.Z-y.Z*z.Y)-x.Y*(y.X*z.Z-y.Z*z.X)+x.Z*(y.X*z.Y-y.Y*z.X);
  return Matrix.IsOk(m) && AbsF(VRPreviewLocalDot(x,x)-1.0)<0.001 &&
    AbsF(VRPreviewLocalDot(y,y)-1.0)<0.001 && AbsF(VRPreviewLocalDot(z,z)-1.0)<0.001 &&
    AbsF(VRPreviewLocalDot(x,y))<0.001 && AbsF(VRPreviewLocalDot(x,z))<0.001 && AbsF(VRPreviewLocalDot(y,z))<0.001 && AbsF(determinant-1.0)<0.001;
}
func VRPreviewLocalStart(controller: ref<inkPuppetPreviewGameController>, epoch: Uint32, setup: Int32) -> ref<VRPreviewLocalDiagnostic> {
  if setup<0 { return null; };
  let half = VRPreviewLocalHalfIpd();
  if half==0.0 { return null; };
  let state = new VRPreviewLocalDiagnostic();
  state.controller=controller;state.epoch=epoch;state.setup=setup;state.half=half;state.status="waiting-stable-fullbody";
  state.startedAt=VRPreviewLocalClock();
  state.stereo=VRPreviewStereoUiEnabled();
  state.eye=half<0.0?1:0;
  return state;
}
func VRPreviewLocalRestore(state: ref<VRPreviewLocalDiagnostic>) -> Bool {
  if !IsDefined(state) { return false; };
  VRPreviewLocalReset(state.epoch);
  state.ready=false;state.finished=true;
  if !state.applied { state.status="finished-unapplied";return false; };
  let current=VRPreviewLocalCamera(state.controller,state.epoch,state.setup);
  if !IsDefined(current) || current!=state.camera || !VRPreviewLocalNear(current.GetLocalPosition(),state.appliedPosition) ||
     AbsF(Quaternion.Dot(current.GetLocalOrientation(),state.orientation)-1.0)>=0.0001 {
    state.status="superseded-no-restore";state.applied=false;return false;
  };
  current.SetLocalPosition(state.baseline);
  state.applied=false;state.status="restored-own-fingerprint";return true;
}
func VRPreviewLocalStep(state: ref<VRPreviewLocalDiagnostic>) -> Void {
  if !IsDefined(state) || state.finished { return; };
  let now=VRPreviewLocalClock();
  if !state.applied && now-state.startedAt>=8.0 { VRPreviewLocalRestore(state);return; };
  let camera=VRPreviewLocalCamera(state.controller,state.epoch,state.setup);
  if !IsDefined(camera) { if state.applied { state.ready=false;state.status="scope-unavailable"; };return; };
  let position=camera.GetLocalPosition();
  let orientation=camera.GetLocalOrientation();
  let world=camera.GetLocalToWorld();
  if !VRPreviewLocalRigid(world) || AbsF(Quaternion.Dot(orientation,orientation)-1.0)>=0.001 {
    VRPreviewLocalRestore(state);state.status="non-rigid-transform-rejected";return;
  };
  if state.applied {
    if camera!=state.camera || !VRPreviewLocalNear(position,state.appliedPosition) ||
       AbsF(Quaternion.Dot(orientation,state.orientation)-1.0)>=0.0001 {
      VRPreviewLocalReset(state.epoch);
      state.applied=false;state.ready=false;state.finished=true;state.status="superseded-no-restore";return;
    };
    let delta=Matrix.GetTranslation(world)-state.worldBaseline;
    delta.W=state.expectedWorld.W;
    state.ready=VRPreviewLocalNear(delta,state.expectedWorld);
    if state.ready && !state.published {
      state.published=VRPreviewLocalPublish(state.controller,state.epoch,state.setup,state.eye);
    };
    state.ready=state.ready && state.published;
    state.status=state.ready?"applied-world-delta-verified":"applied-waiting-world-delta";
    if state.stereo && state.ready && VRPreviewStereoUiCanAdvance(state.epoch,state.eye) {
      state.eye=1-state.eye;state.half=-state.half;
      let nextRight=Quaternion.GetXAxis(state.orientation);nextRight.W=0.0;
      state.appliedPosition=state.baseline-nextRight*state.half;
      state.expectedWorld=Matrix.GetAxisX(world)*(-state.half);state.expectedWorld.W=0.0;
      state.ready=false;state.published=false;
      camera.SetLocalPosition(state.appliedPosition);
      state.appliedAt=now;state.status="stereo-waiting-world-delta";
    } else {
      if !state.stereo && now-state.appliedAt>=5.0 { VRPreviewLocalRestore(state); };
    };
    return;
  };
  // Local pose may already be stable while the preview owner is still moving.
  // Require the same world position and orientation across these samples too.
  if !IsDefined(state.camera) || state.camera!=camera || !VRPreviewLocalNear(position,state.baseline) ||
     AbsF(Quaternion.Dot(orientation,state.orientation)-1.0)>=0.0001 ||
     !VRPreviewLocalNear(Matrix.GetTranslation(world),Matrix.GetTranslation(state.stableWorld)) ||
     !VRPreviewLocalNear(Matrix.GetAxisX(world),Matrix.GetAxisX(state.stableWorld)) ||
     !VRPreviewLocalNear(Matrix.GetAxisY(world),Matrix.GetAxisY(state.stableWorld)) ||
     !VRPreviewLocalNear(Matrix.GetAxisZ(world),Matrix.GetAxisZ(state.stableWorld)) {
    state.camera=camera;state.baseline=position;state.orientation=orientation;state.stableWorld=world;state.stable=1;return;
  };
  state.stable+=1;
  if state.stable<3 { return; };
  let right=Quaternion.GetXAxis(orientation);
  right.W=0.0;
  state.appliedPosition=state.baseline-right*state.half;
  state.worldBaseline=Matrix.GetTranslation(world);
  state.expectedWorld=Matrix.GetAxisX(world)*(-state.half);
  state.expectedWorld.W=0.0;
  if !VRPreviewLocalPrepare(state.controller,state.epoch,state.setup) { state.finished=true;state.status="native-baseline-rejected";return; };
  camera.SetLocalPosition(state.appliedPosition);
  state.applied=true;state.appliedAt=now;state.status="applied-waiting-world-delta";
}

// Supported widget APIs, game/script callback only. The queue holds strong
// local refs converted from engine weak getters; nothing persists across ticks.
class VRPreviewWidgetAliasScan {
  public let controller: ref<inkPuppetPreviewGameController>;
  public let epoch: Uint32;
  public let passes: Int32;
  public let visited: Int32;
  public let images: Int32;
  public let matches: Int32;
  public let totalMatches: Int32;
  public let truncated: Bool;
  public let status: String;
  public let nextAt: Float;
}
func VRPreviewWidgetAliasStart(controller: ref<inkPuppetPreviewGameController>, epoch: Uint32) -> ref<VRPreviewWidgetAliasScan> {
  if !VRPreviewWidgetAliasEnabled() { return null; };
  let state=new VRPreviewWidgetAliasScan();state.controller=controller;state.epoch=epoch;state.status="waiting-root";
  return state;
}
func VRPreviewWidgetAliasStep(state: ref<VRPreviewWidgetAliasScan>) -> Void {
  if !IsDefined(state) || state.passes>=12 { return; };
  let now=VRPreviewLocalClock();
  if now<state.nextAt { return; };
  state.nextAt=now+0.25;
  state.passes+=1;state.visited=0;state.images=0;state.matches=0;state.truncated=false;
  let root: ref<inkWidget> = state.controller.GetRootWidget();
  if !IsDefined(root) { state.status="root-unavailable";return; };
  let queue: array<ref<inkWidget>>;
  let depths: array<Int32>;
  ArrayPush(queue,root);ArrayPush(depths,0);
  let cursor: Int32=0;
  let examined: Int32=0;
  while cursor<ArraySize(queue) && cursor<128 && state.images<32 && examined<256 {
    let widget: ref<inkWidget> = queue[cursor];
    let depth=depths[cursor];cursor+=1;
    if IsDefined(widget) {
      state.visited+=1;
      let image=widget as inkImage;
      if IsDefined(image) {
        state.images+=1;
        if VRPreviewObserveTypedImage(state.controller,state.epoch,image) { state.matches+=1; };
      };
      let compound=widget as inkCompoundWidget;
      if IsDefined(compound) {
        if depth>=8 { if compound.GetNumChildren()>0 { state.truncated=true; }; }
        else {
          let index: Int32=0;
          while index<compound.GetNumChildren() && ArraySize(queue)<128 && examined<256 {
            examined+=1;
            let child: ref<inkWidget> = compound.GetWidgetByIndex(index);index+=1;
            if IsDefined(child) {
              let seen: Int32=0;
              let duplicate: Bool=false;
              while seen<ArraySize(queue) {
                if queue[seen]==child { duplicate=true; };
                seen+=1;
              };
              if !duplicate { ArrayPush(queue,child);ArrayPush(depths,depth+1); };
            };
          };
          if index<compound.GetNumChildren() { state.truncated=true; };
        };
      };
    };
  };
  if cursor<ArraySize(queue) { state.truncated=true; };
  state.totalMatches+=state.matches;
  state.status=state.matches>0?"matched-cpu-resource":(state.truncated?"bounded-scan-incomplete":"no-alias-in-root");
}
