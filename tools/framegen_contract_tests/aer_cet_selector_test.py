"""Run the actual CET selector in pinned Lua with component/readback fixtures."""
from pathlib import Path
import argparse, sys
parser=argparse.ArgumentParser()
parser.add_argument('--lupa',required=True)
args=parser.parse_args()
sys.path.insert(0,args.lupa)
from lupa import LuaRuntime
root=Path(__file__).resolve().parents[2]
lua=LuaRuntime(unpack_returned_tuples=True)
lua.execute('''
token=0; world=1; pregame=false; loading=false; confirmations={}; wanted="1"
function comp(name)
  return {name={value=name},virtualCameraName={value="vrcam_feed_1000x1000"},isEnabled=false,
    Toggle=function(self,on) if not self.ignoreToggle then self.isEnabled=on end end}
end
components={comp("vrcam_1000x1000"),comp("vrcam_1200x1200")}
function makePlayer(list,id)return{GetComponents=function()return list end,GetEntityID=function()return{hash=id}end}end
player=makePlayer(components,31)
Game={GetPlayer=function()return player end,
 GetVRAerVrcamOffRequest=function()return token end,
 GetVRAerWorldGeneration=function()return world end,
 ConfirmVRAerVrcamOff=function(t,n,ok)table.insert(confirmations,{t,n,ok});return ok end,
 GetSystemRequestsHandler=function()return{IsPreGame=function()return pregame end}end,
 GetInkSystem=function()return nil end}
io.open=function(path,mode)
 if mode=="w" then return{write=function()end,close=function()end}end
 local text=path=="vrcam.json" and '{"component":"vrcam_1000x1000"}' or wanted
 return{read=function()return text end,close=function()end}
end
print=function()end
''')
module=lua.execute((root/'mods/cet/CyberpunkVRPort_Stereo/modules/vrcam_select.lua').read_text('utf-8'))
lua.globals().selector=module
lua.execute('''
checks=0
function check(x,label) checks=checks+1;if not x then error(label)end end
selector.init();selector.tick(.01)
check(components[1].isEnabled and not components[2].isEnabled,"paired default keeps selected camera")
token=12;selector.tick(.01)
check(not components[1].isEnabled and not components[2].isEnabled,"AER disables actual components")
local c=confirmations[#confirmations]
check(c[1]==12 and c[2]==2 and c[3],"ack has current token, scan count, actual state")
token=21;selector.tick(.01)
check(confirmations[#confirmations][1]==21,"new world/resize token forces immediate readback")
token=0;selector.tick(.01)
check(components[1].isEnabled,"leaving AER restores paired-mode preference")
components[1].ignoreToggle=true;token=24;selector.tick(.01)
check(not confirmations[#confirmations][3],"successful Toggle call with enabled readback is NOT proof")
components[1].ignoreToggle=false;token=25;selector.tick(.01)
check(confirmations[#confirmations][3],"new request can recover after actual disable")
local former=components
former[1].isEnabled=true;former[1].ignoreToggle=true
components={comp("vrcam_1000x1000"),comp("vrcam_1200x1200")};player=makePlayer(components,32)
selector.ticksToRetry=0;selector.tick(.01)
check(not confirmations[#confirmations][3],"previous player still enabled blocks AER")
former[1].ignoreToggle=false;selector.ticksToRetry=0;selector.tick(.01)
check(confirmations[#confirmations][3],"temporary previous-player readback failure recovers after actual disable")
pregame=true;selector.tick(.01)
c=confirmations[#confirmations]
check(c[1]==25 and c[2]==0 and not c[3],"loading revokes proof before old player is released")
pregame=false;components={};player=makePlayer(components,33);token=26;selector.tick(.01)
c=confirmations[#confirmations]
check(c[2]==0 and not c[3],"empty entity is not a disabled VRCAM proof")
selector.aerPendingOverflow=true;token=27;selector.tick(.01)
check(selector.aerPendingOverflow,"new request alone cannot forget old cameras")
world=2;components={comp("vrcam_1000x1000")};player=makePlayer(components,34);token=28;selector.tick(.01)
check(not selector.aerPendingOverflow and confirmations[#confirmations][3],"new native world releases obsolete pending handles and rechecks current components")
''')
print(f'PASS {lua.globals().checks} actual CET selector checks (Lua {lua.lua_version}, lupa 2.5)')
