"""CET lifecycle simulation only; no game APIs or files are touched."""
import pathlib
import sys

if len(sys.argv) > 1:
    sys.path.insert(0, sys.argv[1])
from lupa import LuaRuntime

root = pathlib.Path(__file__).resolve().parents[2]
lua = LuaRuntime(unpack_returned_tuples=True)
lua.execute(r'''
before, after, Game = {}, {}, {}
epoch, active, publishCount = 0, nil, 0
function ObserveBefore(class, method, fn) before[class..'.'..method] = fn end
function ObserveAfter(class, method, fn) after[class..'.'..method] = fn end
function Game.VRPreviewSameController(a,b) return a~=nil and b~=nil and (rawequal(a,b) or (a.nativeId~=nil and a.nativeId==b.nativeId)) end
function Game.VRPreviewScopeBegin() epoch=epoch+1; active=nil; return epoch end
function Game.VRPreviewScopeClear(token) if active and active.epoch==token then active=nil end; return true end
function Game.VRPreviewResolveAndPublish(ctrl,token,screen,ready)
    publishCount=publishCount+1
    if token~=epoch then return false end
    if not ready or ctrl.ambiguous then active=nil; return not ready end
    active={epoch=token,setup=ctrl.actualSetup}; return true
end
function puppet(setup) return {actualSetup=setup,GetClassName=function() return 'gameuiInventoryPuppetPreviewGameController' end} end
''')
module = lua.execute((root / "mods/cet/CyberpunkVRPort_Stereo/modules/menu_preview.lua").read_text(encoding="utf-8"))
lua.globals().M = module
module.init()
lua.execute(r'''
ui1, ui2 = {}, {}
before['gameuiInventoryGameController.OnInitialize'](ui1)
p1=puppet(0)
before['inkPuppetPreviewGameController.OnInitialize'](p1)
assert(active==nil)
after['inkPuppetPreviewGameController.OnPreviewInitialized'](p1)
assert(active and active.epoch==1 and active.setup==0)
before['inkInventoryPuppetPreviewGameController.OnSetCameraSetupEvent'](p1)
assert(active==nil)
p1.actualSetup=23
after['inkInventoryPuppetPreviewGameController.OnSetCameraSetupEvent'](p1)
assert(active and active.setup==23)
before['gameuiInventoryGameController.OnInitialize'](ui2)
p2=puppet(1)
before['inkPuppetPreviewGameController.OnInitialize'](p2)
after['inkPuppetPreviewGameController.OnPreviewInitialized'](p2)
assert(active and active.epoch==2)
before['gameuiInventoryGameController.OnUninitialize'](ui1)
assert(active and active.epoch==2) -- late old owner close must not use current epoch
p2.ambiguous=true
M.tick(0)
assert(active==nil)
p2.ambiguous=false
M.tick(0)
assert(active and active.epoch==2)
before['gameuiInventoryGameController.OnUninitialize'](ui2)
assert(active==nil)
local count=publishCount; M.tick(1); assert(publishCount==count)
M.shutdown(); assert(active==nil)
assert(not M.getState().ready and M.getState().epoch==0)
ui3={};before['gameuiInventoryGameController.OnInitialize'](ui3)
p3=puppet(2);before['inkPuppetPreviewGameController.OnInitialize'](p3)
local token=epoch
before['inkPuppetPreviewGameController.OnInitialize'](p3)
assert(epoch==token) -- duplicate initialize cannot invalidate its own scope
assert(not M.getState().ready)
M.tick(0.1) -- readiness callback intentionally omitted
assert(active and M.getState().ready and M.getState().epoch==token)
p3.ambiguous=true;M.tick(0.1);assert(not M.getState().ready)
before['gameuiInventoryGameController.OnCloseMenu'](ui3)
assert(not M.getState().ready and M.getState().epoch==0)
ui4={};before['gameuiInventoryGameController.OnInitialize'](ui4)
p4=puppet(0);before['inkPuppetPreviewGameController.OnInitialize'](p4);M.tick(0.1)
local newToken=epoch
before['gameuiInventoryGameController.OnCloseMenu'](ui3)
before['gameuiInventoryGameController.OnUninitialize'](ui3)
assert(M.getState().ready and M.getState().epoch==newToken)
before['gameuiInventoryGameController.OnCloseMenu'](ui4)
assert(not M.getState().ready and M.getState().epoch==0)
ui5={nativeId=50};before['gameuiInventoryGameController.OnInitialize'](ui5)
p5=puppet(0);p5.nativeId=51
before['inkPuppetPreviewGameController.OnInitialize'](p5);M.tick(0.1)
local stableToken=epoch
p5alias=puppet(0);p5alias.nativeId=51
before['inkPuppetPreviewGameController.OnInitialize'](p5alias)
assert(epoch==stableToken)
before['gameuiInventoryGameController.OnCloseMenu']({nativeId=50})
assert(not M.getState().ready and M.getState().epoch==0)
''')
# Parse the loader without executing its other game modules.
parse = lua.eval("function(source) return assert(load(source)) ~= nil end")
assert parse((root / "mods/cet/CyberpunkVRPort_Stereo/init.lua").read_text(encoding="utf-8"))
print("PASS: Lua parse + mocked Inventory readiness/setup/late owner close/ambiguity/paused heartbeat/shutdown")
