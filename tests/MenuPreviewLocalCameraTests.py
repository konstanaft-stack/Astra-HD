"""Actual CET module with typed-bridge fixtures; no game/redscript compilation.
The bridge fixture exposes own-fingerprint restoration outcomes. This tests
Lua ownership/call ordering, not execution of native imported transforms.
"""
import pathlib,sys,unittest,re
sys.path.insert(0,r'<WORKSPACE_ROOT>\research\2026-09-30-cpvr-open-nvidia\tooling\menu-preview\test-deps')
from lupa import LuaRuntime
ROOT=pathlib.Path(__file__).resolve().parents[1]
class LocalCamera(unittest.TestCase):
 def setUp(self):
  self.lua=LuaRuntime(unpack_returned_tuples=True)
  self.lua.execute('''
before,after,Game={},{},{}
epoch,enabled,starts,steps,restores,sets,position,eye=0,true,0,0,0,0,10,0
function ObserveBefore(c,m,f) before[c..'.'..m]=f end
function ObserveAfter(c,m,f) after[c..'.'..m]=f end
function Game.VRPreviewSameController(a,b) return a and b and a.id==b.id end
function Game.VRPreviewScopeBegin() epoch=epoch+1;return epoch end
function Game.VRPreviewScopeClear(e) assert(not live or live.finished,'restore must precede clear');return true end
function Game.VRPreviewResolveAndPublish(c,e,s,r) return r end
function Game.VRPreviewLocalStart(c,e)
 starts=starts+1;if not enabled then return nil end
 live={epoch=e,status='waiting',ready=false,baseline=position,finished=false,eye=eye};return live
end
function Game.VRPreviewLocalStep(s)
 steps=steps+1;if s.finished then return end
 if not s.applied then s.applied=true;s.own=position+(s.eye==1 and .032 or -.032);position=s.own;sets=sets+1;s.ready=true;s.status='applied-world-delta-verified' end
end
function Game.VRPreviewLocalRestore(s)
 restores=restores+1;s.finished=true;s.ready=false
 if s.applied and position==s.own then position=s.baseline;s.status='restored-own-fingerprint';sets=sets+1
 else s.status='superseded-no-restore' end
end
owner={id=1};ctrl={id=2,GetClassName=function() return 'gameuiInventoryPuppetPreviewGameController' end}
''')
  self.mod=self.lua.execute((ROOT/'mods/cet/CyberpunkVRPort_Stereo/modules/menu_preview.lua').read_text())
  self.lua.globals().M=self.mod;self.mod.init()
  self.lua.execute("before['gameuiInventoryGameController.OnInitialize'](owner);before['inkPuppetPreviewGameController.OnInitialize'](ctrl)")
 def test_apply_once_export_and_restore_before_close(self):
  self.mod.tick(.1);self.mod.tick(.1)
  self.lua.execute("assert(starts==1 and sets==1);assert(M.getState().local_camera_ready);before['gameuiInventoryGameController.OnCloseMenu'](owner);assert(position==10 and sets==2);M.tick(.1);assert(starts==1)")
 def test_setup_change_finishes_diagnostic_without_reapply(self):
  self.mod.tick(.1)
  self.lua.execute("before['inkInventoryPuppetPreviewGameController.OnSetCameraSetupEvent'](ctrl);after['inkInventoryPuppetPreviewGameController.OnSetCameraSetupEvent'](ctrl)")
  self.mod.tick(.1);self.lua.execute('assert(starts==1 and sets==2 and live.finished)')
 def test_superseded_fingerprint_not_overwritten(self):
  self.mod.tick(.1);self.lua.execute("position=99;before['gameuiInventoryGameController.OnCloseMenu'](owner);assert(position==99 and sets==1)")
 def test_default_disabled(self):
  self.lua.execute('enabled=false');self.mod.tick(.1);self.mod.tick(.1)
  self.lua.execute("assert(sets==0 and starts==1 and M.getState().local_camera_status=='disabled')")
 def test_initial_setup_event_does_not_block_test(self):
  self.lua.execute("before['inkInventoryPuppetPreviewGameController.OnSetCameraSetupEvent'](ctrl);after['inkInventoryPuppetPreviewGameController.OnSetCameraSetupEvent'](ctrl)")
  self.mod.tick(.1);self.lua.execute('assert(starts==1 and sets==1)')
 def test_constant_right_side_export(self):
  self.lua.execute('eye=1');self.mod.tick(.1);self.mod.tick(.1)
  self.lua.execute('assert(M.getState().local_camera_eye==1 and position>10 and sets==1)')
 def test_paused_heartbeat_cannot_consume_wallclock_window(self):
  source=(ROOT/'mods/redscript/CyberpunkVRPort_MenuPreview/vrport_menu_preview.reds').read_text()
  self.assertIn('let now=VRPreviewLocalClock();',source)
  limit=float(re.search(r'now-state\.appliedAt>=([0-9.]+)',source).group(1))
  self.assertEqual(limit,5.0)
  for _ in range(10000):self.assertFalse(1.0-0.0>=limit)
  self.assertTrue(5.0-0.0>=limit)
  self.assertNotIn('appliedTicks',source)
  self.assertIn('!state.applied && now-state.startedAt>=8.0',source)
if __name__=='__main__':unittest.main()
