-- CET: grant only the three requested vehicles after a playable world is present.
-- The vehicle files themselves must already be installed. This mod only invokes
-- the authors' EnablePlayerVehicle commands; it never edits money or quests.

local vehicles = {
  'Vehicle.toyota_gr_yaris_purchasable',
  'Vehicle.rs6_performance',
  'Vehicle.v_standard25_thorton_merrimac_player'
}

local CHECK_INTERVAL = 2.0
local WORLD_SETTLE = 1.0
local MAX_RECORD_CHECKS = 6
local MAX_SYSTEM_CHECKS = 12

local session = 0
local inWorld = false
local attachPending = false
local waitSeconds = 0.0
local systemChecks = 0
local states = {}

local function log(message)
  print('[CyberpunkVR VehicleGrant] ' .. message)
end

local function resetSession()
  session = session + 1
  inWorld = true
  attachPending = false
  waitSeconds = WORLD_SETTLE
  systemChecks = 0
  states = {}
  for _, id in ipairs(vehicles) do
    states[id] = {attempts = 0, finished = false, lastError = nil}
  end
  log('loaded-world session ' .. session .. ': waiting for VehicleSystem and records')
end

local function playerPresent()
  local ok, player = pcall(function() return Game.GetPlayer() end)
  return ok and player ~= nil
end

local function vehicleSystem()
  local ok, system = pcall(function() return Game.GetVehicleSystem() end)
  if ok then return system end
  return nil
end

local function recordPresent(id)
  local ok, record = pcall(function() return TweakDB:GetRecord(id) end)
  return ok and record ~= nil
end

local function tryVehicle(system, id, state)
  state.attempts = state.attempts + 1
  if not recordPresent(id) then
    state.lastError = 'record unavailable'
  else
    local ok, result = pcall(function()
      return system:EnablePlayerVehicle(id, true, false)
    end)
    if ok and result ~= false then
      state.finished = true
      -- CET's method may return nil. A successful Lua call is not proof that
      -- the garage UI or the replacement vehicle mesh has been verified.
      log(id .. ': EnablePlayerVehicle invoked; return=' .. tostring(result) ..
        '; vehicle menu appearance unverified')
      return
    end
    state.lastError = ok and 'EnablePlayerVehicle returned false' or
      ('EnablePlayerVehicle failed: ' .. tostring(result))
  end
  if state.attempts >= MAX_RECORD_CHECKS then
    state.finished = true
    log(id .. ': ' .. state.lastError .. ' after ' .. state.attempts ..
      ' checks; no grant claimed in this session')
  end
end

registerForEvent('onInit', function()
  local ok = pcall(function()
    ObserveAfter('PlayerPuppet', 'OnGameAttached', function()
      -- This also re-arms after loading another save without restarting CET.
      -- Granting still waits for Game.GetPlayer() and VehicleSystem in onUpdate.
      attachPending = true
    end)
  end)
  log('ready; PlayerPuppet.OnGameAttached observer=' .. tostring(ok))
end)

registerForEvent('onUpdate', function(dt)
  if not playerPresent() then
    inWorld = false
    return
  end
  if not inWorld or attachPending then resetSession() end

  dt = type(dt) == 'number' and math.max(0, math.min(dt, CHECK_INTERVAL)) or 0
  waitSeconds = waitSeconds - dt
  if waitSeconds > 0 then return end
  waitSeconds = CHECK_INTERVAL

  local pending = false
  for _, id in ipairs(vehicles) do
    if not states[id].finished then pending = true; break end
  end
  if not pending then return end

  local system = vehicleSystem()
  if not system then
    systemChecks = systemChecks + 1
    if systemChecks >= MAX_SYSTEM_CHECKS then
      for _, id in ipairs(vehicles) do
        if not states[id].finished then states[id].finished = true end
      end
      log('VehicleSystem unavailable after ' .. systemChecks ..
        ' checks; no grant claimed in this session')
    end
    return
  end
  for _, id in ipairs(vehicles) do
    if not states[id].finished then tryVehicle(system, id, states[id]) end
  end
end)
