"""Exercise the Lua fixture's control lifecycle; does not emulate rendering."""
from pathlib import Path
import argparse
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--lua-deps', type=Path, required=True)
args = parser.parse_args()
sys.path.insert(0, str(args.lua_deps))
from lupa.luajit21 import LuaRuntime

root = Path(__file__).resolve().parents[2]
mod = root / 'mods/cet/CyberpunkVRPort_ImageProbe'
lua = LuaRuntime(unpack_returned_tuples=True)
pattern = lua.execute((mod / 'pattern.lua').read_text(encoding='utf-8'))
lua.globals().require = lambda name: pattern if name == 'pattern' else None
lua.execute(r'''
files, events, calls = {}, {}, {}
cameraPosition = {x=10,y=20,z=30}
playerLoaded, drawFailure = true, false
json={encode=function(x)return x end,decode=function(x)return x end}
io={open=function(name,mode)
  if mode=='r' and files[name]==nil then return nil end
  return {read=function()return files[name] end,
          write=function(_,value)files[name]=value end,close=function()end}
end}
function registerForEvent(name,fn)events[name]=fn end
Vector4={new=function(x,y,z,w)return {x=x,y=y,z=z,w=w} end}
Color={new=function(r,g,b,a)return {r=r,g=g,b=b,a=a} end}
camera={GetLocalToWorld=function()return {} end}
player={GetGame=function()return {} end,GetFPPCameraComponent=function()return camera end}
Game={GetPlayer=function()if playerLoaded then return player end end}
visualizer={DrawLine3D=function(_,a,b,color,life)
  if drawFailure then error('draw unavailable') end
  calls[#calls+1]={a=a,b=b,color=color,life=life}
end}
GameInstance={GetDebugVisualizerSystem=function()return visualizer end}
function GetSingleton(name)
  if name=='Matrix' then return {
    GetTranslation=function()return cameraPosition end,ToQuat=function()return {} end}
  elseif name=='Quaternion' then return {
    GetForward=function()return {x=0,y=1,z=0} end,
    GetRight=function()return {x=1,y=0,z=0} end,
    GetUp=function()return {x=0,y=0,z=1} end}
  end
end
''')
lua.execute((mod / 'init.lua').read_text(encoding='utf-8'))
lua.execute(r'''
-- A command left by a prior run is not an instruction to auto-arm this run.
files['command.json']={id=1,action='arm'}
events.onInit(); events.onUpdate(0.3)
assert(#calls==0 and not files['status.json'].enabled)
files['command.json']={id=2,action='arm',distance=3}
events.onUpdate(0.3);events.onUpdate(1)
assert(#calls>0 and files['status.json'].enabled)
local first=calls[1].a
cameraPosition={x=100,y=200,z=300}
calls={};events.onUpdate(0.1)
assert(calls[1].a.x==first.x and calls[1].a.y==first.y and calls[1].a.z==first.z,
       'pattern moved with camera instead of staying in world space')
files['command.json']={id=3,action='stop'}
events.onUpdate(0.3);local stopped=#calls;events.onUpdate(1)
assert(#calls==stopped and not files['status.json'].enabled)
files['command.json']={id=4,action='replay',distance=3}
calls={};events.onUpdate(0.3)
assert(calls[1].a.x==first.x and calls[1].a.y==first.y and calls[1].a.z==first.z,
       'replay did not use original anchor')
playerLoaded=false;events.onUpdate(0.1)
assert(not files['status.json'].enabled and files['status.json'].error)
playerLoaded=true
files['command.json']={id=5,action='arm',distance=100}
events.onUpdate(0.3)
assert(not files['status.json'].enabled and files['status.json'].error)
drawFailure=true
files['command.json']={id=6,action='arm',distance=3}
events.onUpdate(0.3)
assert(not files['status.json'].enabled and files['status.json'].error)
''')
print('PASS: LuaJIT syntax, explicit activation, stale-command suppression, fixed world anchor, replay, stop, unload, invalid distance, draw failure')
print('NOT VERIFIED: native DrawLine3D visibility, lifetime=0 behavior, per-eye inclusion, position in engine post-processing')
