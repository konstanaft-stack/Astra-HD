return function(source)
 local before,after={},{}
 local native={epoch=0,open=false,available=false,begins=0,clears=0,invalidations=0,localStarts=0,restores=0,scans=0}
 local Game={}
 function Game.VRPreviewStereoUiEnabled() return true end
 function Game.VRPreviewSameController(a,b) return a.id==b.id end
 function Game.VRPreviewReadAppearanceSelection(ctrl) if ctrl.unready then return nil end;return {setup=ctrl.setup,slotName=ctrl.slot} end
 function Game.VRPreviewScopeBegin() native.epoch=native.epoch+1;native.open=true;native.available=false;native.begins=native.begins+1;return native.epoch end
 function Game.VRPreviewScopeClear(epoch) if epoch==native.epoch then native.open=false;native.available=false;native.clears=native.clears+1 end end
 function Game.VRPreviewResolveAppearanceAndPublish(ctrl,epoch,ready)
  if not native.open or epoch~=native.epoch then return false end
  native.available=ready==true;if not ready then native.invalidations=native.invalidations+1 end;return true
 end
 function Game.VRPreviewStereoUiPairSubmitted() return false end
 function Game.VRPreviewLocalHalfIpd() return 0 end
 function Game.VRPreviewLocalStart() native.localStarts=native.localStarts+1;return {status='ready',ready=true,eye=0} end
 function Game.VRPreviewLocalStep() end
 function Game.VRPreviewLocalRestore() native.restores=native.restores+1;return true end
 function Game.VRPreviewWidgetAliasStart() native.scans=native.scans+1;return {} end
 function Game.VRPreviewWidgetAliasStep() end
 local make=assert(loadstring('return function(Game,ObserveBefore,ObserveAfter,print)\n'..source..'\nend'))()
 local module=make(Game,function(c,n,f)before[c..'.'..n]=f end,function(c,n,f)after[c..'.'..n]=f end,function()end)
 local function controller(id) return {id=id,setup=6,slot='UI_Skin',GetClassName=function()return 'gameuiCharacterCreationPuppetPreviewGameController'end} end
 module.init();local ctrl=controller(1)
 after['inkPuppetPreviewGameController.OnPreviewInitialized'](ctrl)
 assert(native.open and native.available and native.begins==1,'initial editor not published')
 module.tick(.016)
 local epoch=native.epoch
 for _,category in ipairs({{9,'UI_Lips'},{2,'UI_Preview'},{6,'UI_Skin'}})do
  before['inkCharacterCreationPuppetPreviewGameController.OnSetCameraSetupEvent'](ctrl)
  assert(native.open and native.epoch==epoch and native.clears==0,'category refresh closed accepted packet lifetime')
  assert(not native.available,'category refresh left old current producer publication visible')
  ctrl.unready=true;module.tick(.016)
  assert(native.open and native.epoch==epoch,'unready setup gap closed accepted packet lifetime')
  ctrl.setup=category[1];ctrl.slot=category[2];ctrl.unready=false
  after['inkCharacterCreationPuppetPreviewGameController.OnSetCameraSetupEvent'](ctrl);module.tick(.016)
  assert(native.open and native.available and native.epoch==epoch and native.begins==1,'category changed editor epoch')
 end
 assert(native.localStarts==4 and native.restores==3 and native.scans==4,'category producer bookkeeping not refreshed exactly once')
 ctrl.setup=9;ctrl.slot='UI_Lips';module.tick(.016)
 assert(native.open and native.available and native.epoch==epoch,'tick-only category refresh changed epoch')
 local nextCtrl=controller(2);after['inkPuppetPreviewGameController.OnPreviewInitialized'](nextCtrl)
 assert(native.open and native.epoch~=epoch and native.clears==1,'new controller retained prior lifetime')
 before['MenuScenario_BaseMenu.OnLeaveScenario']()
 assert(not native.open and native.clears==2,'true editor exit retained accepted packet')
 module.shutdown();assert(native.clears==2,'shutdown repeated close of absent editor')
 return 'PASS actual Lua category hooks/tick retain lifecycle and invalidate producer; new controller/exit close.'
end
