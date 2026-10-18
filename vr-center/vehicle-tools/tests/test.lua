local IDS = {
  'Vehicle.toyota_gr_yaris_purchasable',
  'Vehicle.rs6_performance',
  'Vehicle.v_standard25_thorton_merrimac_player'
}

local function newHarness(records, enable)
  local events, observers, calls, logs = {}, {}, {}, {}
  local player = nil
  local vehicle = {}
  local activeVehicleSystem = vehicle
  function vehicle:EnablePlayerVehicle(id, owned, active)
    calls[#calls + 1] = {id, owned, active}
    if enable then return enable(id) end
  end
  Game = {
    GetPlayer = function() return player end,
    GetVehicleSystem = function() return activeVehicleSystem end
  }
  TweakDB = {GetRecord = function(_, id) return records[id] end}
  registerForEvent = function(name, fn) events[name] = fn end
  ObserveAfter = function(class, method, fn) observers[class .. '.' .. method] = fn end
  print = function(s) logs[#logs + 1] = s end
  assert(loadstring(VEHICLE_INIT_SOURCE, '@init.lua'))()
  events.onInit()
  return {
    calls = calls, logs = logs,
    player = function(v) player = v end,
    system = function(v) activeVehicleSystem = v and vehicle or nil end,
    tick = function(seconds) events.onUpdate(seconds) end,
    attach = function(v) observers['PlayerPuppet.OnGameAttached'](v) end
  }
end

local function tickMany(h, count, seconds)
  for _ = 1, count do h.tick(seconds) end
end

local all = {}
for _, id in ipairs(IDS) do all[id] = {} end

-- Never grant in the main menu, even if vehicle records and VehicleSystem exist.
local h = newHarness(all)
tickMany(h, 30, 1)
assert(#h.calls == 0, 'main menu caused a grant')
h.player({})
h.attach({})
tickMany(h, 6, 1)
assert(#h.calls == 3, 'first loaded session did not invoke three grants once')
for i, id in ipairs(IDS) do
  assert(h.calls[i][1] == id and h.calls[i][2] == true and h.calls[i][3] == false,
    'author grant arguments changed')
end
tickMany(h, 30, 1)
assert(#h.calls == 3, 'grant repeated within one session')
local observedOnly = 0
for _, line in ipairs(h.logs) do
  if line:find('vehicle menu appearance unverified', 1, true) then observedOnly = observedOnly + 1 end
end
assert(observedOnly == 3, 'nil API result was mislabeled or not logged')

-- A new save/load in the same process must re-arm once.
h.player(nil)
h.tick(1)
local secondPlayer = {}
h.player(secondPlayer)
h.attach(secondPlayer)
tickMany(h, 6, 1)
assert(#h.calls == 6, 'new loaded session was not granted once')
tickMany(h, 30, 1)
assert(#h.calls == 6, 'new session repeated grants')

-- The attach callback itself must re-arm when a load does not expose a nil-player tick.
local reload = newHarness(all)
local retainedPlayer = {}
reload.player(retainedPlayer)
tickMany(reload, 5, 1)
assert(#reload.calls == 3)
reload.attach(retainedPlayer)
tickMany(reload, 5, 1)
assert(#reload.calls == 6, 'save/load attach without nil-player gap was missed')

-- Absent mod record is never reported as a successful invocation; it can appear later.
local partial = {[IDS[2]] = {}, [IDS[3]] = {}}
local m = newHarness(partial)
m.player({})
tickMany(m, 4, 1)
assert(#m.calls == 2, 'absent record was passed to VehicleSystem')
partial[IDS[1]] = {}
tickMany(m, 6, 1)
assert(#m.calls == 3 and m.calls[3][1] == IDS[1], 'late record not retried')

local waiting = newHarness(all)
waiting.player({})
waiting.system(false)
tickMany(waiting, 4, 1)
assert(#waiting.calls == 0, 'VehicleSystem unavailable yet a grant was attempted')
waiting.system(true)
tickMany(waiting, 6, 1)
assert(#waiting.calls == 3, 'VehicleSystem readiness was not retried')

-- Missing record and throwing API are bounded within a session.
local missing = {[IDS[2]] = {}, [IDS[3]] = {}}
local b = newHarness(missing, function(id)
  if id == IDS[2] then error('transient call error') end
end)
b.player({})
tickMany(b, 100, 1)
local audiCalls = 0
for _, c in ipairs(b.calls) do
  assert(c[1] ~= IDS[1], 'missing record was granted')
  if c[1] == IDS[2] then audiCalls = audiCalls + 1 end
end
assert(audiCalls > 1 and audiCalls <= 6, 'API error retry was not bounded: ' .. audiCalls)
local before = #b.calls
tickMany(b, 100, 1)
assert(#b.calls == before, 'bounded retries became endless')
local sawMissing, sawError = false, false
for _, line in ipairs(b.logs) do
  if line:find(IDS[1], 1, true) and line:find('record unavailable', 1, true) then sawMissing = true end
  if line:find(IDS[2], 1, true) and line:find('failed', 1, true) then sawError = true end
end
assert(sawMissing and sawError, 'failure log lacks exact record and result')

local returnedFalse = newHarness(all, function(id)
  if id == IDS[1] then return false end
end)
returnedFalse.player({})
tickMany(returnedFalse, 100, 1)
local falseCalls, falseInvoked, falseFailure = 0, false, false
for _, call in ipairs(returnedFalse.calls) do
  if call[1] == IDS[1] then falseCalls = falseCalls + 1 end
end
for _, line in ipairs(returnedFalse.logs) do
  if line:find(IDS[1], 1, true) and line:find('invoked', 1, true) then falseInvoked = true end
  if line:find(IDS[1], 1, true) and line:find('returned false', 1, true) then falseFailure = true end
end
assert(falseCalls == 6 and not falseInvoked and falseFailure,
  'explicit false was not handled as bounded failure')

print('PASS: menu gate, exact records, session reload, missing record, retry bound')
