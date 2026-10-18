-- Read the current game provider; the legacy DLSSFrameGen checkbox alone is
-- not authoritative in CP2077 2.31. No settings writes or disk polling here.
local M = {}
local elapsed, previous = 0, nil
local providers = { Off = 1, DLSS = 2, FSR = 3, FSR3 = 3, XeSS = 3, XESS = 3 }
function M.read()
    local settings = Game.GetSettingsSystem()
    local provider = tostring(settings:GetVar('/graphics/presets', 'FrameGeneration'):GetValue())
    local multiplier = tostring(settings:GetVar('/graphics/presets', 'DLSS_MultiFrameGeneration'):GetValue())
    local count = tonumber(multiplier:match('^x([234])$')) or 0
    return providers[provider] or 0, count, provider
end
function M.sample()
    if PublishVRGameFrameGeneration == nil then return end
    local ok, provider, count, label = pcall(M.read)
    if not ok then provider, count, label = 0, 0, 'unavailable' end
    local published = pcall(PublishVRGameFrameGeneration, provider, count)
    local state = tostring(provider)..':'..tostring(count)..':'..tostring(published)
    if state ~= previous then
        print('[Stereo.GameFG] provider='..tostring(label)..' multiplier='..tostring(count)..' published='..tostring(published))
        previous = state
    end
end
function M.tick(dt)
    local delta = tonumber(dt) or 0
    -- A paused game can report zero simulation delta while CET keeps updating
    -- the menu. Native freshness uses wall time, so keep the heartbeat alive.
    if delta <= 0 then M.sample(); return end
    elapsed = elapsed + delta
    if elapsed < 0.25 and previous ~= nil then return end
    elapsed = 0
    M.sample()
end
function M.shutdown()
    if PublishVRGameFrameGeneration ~= nil then pcall(PublishVRGameFrameGeneration, 0, 0) end
end
return M
