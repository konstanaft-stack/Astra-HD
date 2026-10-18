-- Opt-in world-space geometry fixture. It changes no camera, PPD, FG or graphics
-- settings and creates no persistent entities. No hotkey is reserved.
local pattern = require('pattern')
local state = { enabled = false, moving = false, distance = 3, elapsed = 0,
    updates = 0, draws = 0, lastCommand = nil, error = nil }
local anchor, visualizer
local pollElapsed, statusElapsed = 0, 0
local cachedLines = pattern.build(3, 0)
local colors

local function xyz(v) return {x = v.x, y = v.y, z = v.z} end
local function v4(v) return Vector4.new(v.x, v.y, v.z, 1) end
local function writeJson(name, value)
    local f = assert(io.open(name, 'w'))
    f:write(json.encode(value))
    f:close()
end
local function readJson(name)
    local f = io.open(name, 'r')
    if not f then return nil end
    local value = f:read('*a')
    f:close()
    return json.decode(value)
end
local function status()
    writeJson('status.json', {
        enabled = state.enabled, moving = state.moving, distance = state.distance,
        elapsed = state.elapsed, updates = state.updates, drawCalls = state.draws,
        command = state.lastCommand, error = state.error, anchor = anchor,
        scope = 'Unlit engine debug geometry. Both-eye visibility and processing stage unverified.',
        materialReflectionsTested = false
    })
end
local function arm(replay)
    local player = assert(Game.GetPlayer(), 'No loaded player')
    visualizer = assert(GameInstance.GetDebugVisualizerSystem(player:GetGame()), 'No debug visualizer')
    if replay then
        anchor = assert(readJson('anchor.json'), 'No saved anchor')
        assert(anchor.position and anchor.forward and anchor.right and anchor.up, 'Invalid anchor')
    else
        local camera = assert(player:GetFPPCameraComponent(), 'No player camera')
        local transform = camera:GetLocalToWorld()
        local matrix, quat = GetSingleton('Matrix'), GetSingleton('Quaternion')
        local q = matrix:ToQuat(transform)
        anchor = {position = xyz(matrix:GetTranslation(transform)),
            forward = xyz(quat:GetForward(q)), right = xyz(quat:GetRight(q)),
            up = xyz(quat:GetUp(q))}
        writeJson('anchor.json', anchor)
    end
    colors = {
        white = Color.new(255,255,255,255), gray = Color.new(120,120,120,255),
        red = Color.new(255,50,50,255), green = Color.new(50,255,50,255),
        cyan = Color.new(50,255,255,255)
    }
    state.enabled, state.elapsed = true, 0
end
local function command()
    local c = readJson('command.json')
    if not c or c.id == nil or c.id == state.lastCommand then return end
    state.lastCommand, state.error = c.id, nil
    if c.action == 'stop' then
        state.enabled, state.moving = false, false
    elseif c.action == 'arm' or c.action == 'replay' then
        local d = tonumber(c.distance or 3)
        assert(d and d >= 1 and d <= 12, 'Distance must be 1..12 metres')
        state.distance, state.moving = d, c.moving == true
        cachedLines = pattern.build(d, 0)
        arm(c.action == 'replay')
    else
        error('Unknown probe command')
    end
    status()
end

registerForEvent('onInit', function()
    -- An old arm request must not silently enable diagnostics in a later run.
    local ok, previous = pcall(readJson, 'command.json')
    if ok and previous then state.lastCommand = previous.id end
    status()
end)
registerForEvent('onUpdate', function(dt)
    dt = math.max(0, tonumber(dt) or 0)
    pollElapsed, statusElapsed = pollElapsed + dt, statusElapsed + dt
    if pollElapsed >= 0.25 then
        pollElapsed = 0
        local ok, err = pcall(command)
        if not ok then
            state.enabled, state.error = false, tostring(err)
            pcall(status)
        end
    end
    if not state.enabled then return end
    if not Game.GetPlayer() then
        state.enabled, state.error = false, 'Player unloaded; replay explicitly after load'
        pcall(status)
        return
    end
    state.elapsed, state.updates = state.elapsed + dt, state.updates + 1
    local lines = state.moving and pattern.build(state.distance, state.elapsed * math.pi / 2) or cachedLines
    local ok, err = pcall(function()
        for _, line in ipairs(lines) do
            visualizer:DrawLine3D(v4(pattern.world(anchor, line.a)),
                v4(pattern.world(anchor, line.b)), colors[line.color], 0)
            state.draws = state.draws + 1
        end
    end)
    if not ok then state.enabled, state.error = false, tostring(err) end
    if statusElapsed >= 1 or not ok then statusElapsed = 0; pcall(status) end
end)
registerForEvent('onShutdown', function()
    state.enabled = false
    pcall(status)
end)
