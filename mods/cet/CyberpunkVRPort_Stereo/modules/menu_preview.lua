-- Default identity only. Opt-in native-authorized local camera diagnostic uses
-- typed REDscript setters once and restores only its own unchanged fingerprint.
local M = {}
local current, activeOwner, pendingAppearance, elapsed, warned = nil, nil, nil, 0, false
local stereoEnabled = false
local function same(a, b)
    if not a or not b then return false end
    if rawequal(a, b) then return true end
    local ok, value = pcall(function() return Game.VRPreviewSameController(a, b) end)
    return ok and value == true
end
local diagnosticRows = 0
local function restoreLocal(state, epoch)
    local ok, result = pcall(function() return Game.VRPreviewLocalRestore(state) end)
    local statusOk, status = pcall(function() return tostring(state.status) end)
    print("[Stereo.MenuPreview] local restore epoch=" .. tostring(epoch) .. " callOk=" .. tostring(ok) ..
        " restored=" .. tostring(result) .. " status=" .. (statusOk and status or "unavailable"))
end
local function isInventory(ctrl)
    local ok, value = pcall(function() return tostring(ctrl:GetClassName()) end)
    if ok and value == "gameuiInventoryPuppetPreviewGameController" then return true end
    local okName, name = pcall(function() return ctrl:GetClassName().value end)
    return okName and name == "gameuiInventoryPuppetPreviewGameController"
end
local function isAppearance(ctrl)
    local ok, value = pcall(function() return tostring(ctrl:GetClassName()) end)
    if ok and value == "gameuiCharacterCreationPuppetPreviewGameController" then return true end
    local okName, name = pcall(function() return ctrl:GetClassName().value end)
    return okName and name == "gameuiCharacterCreationPuppetPreviewGameController"
end
local function appearanceSelection(ctrl)
    local ok, selected = pcall(function() return Game.VRPreviewReadAppearanceSelection(ctrl) end)
    if not ok or not selected then return nil end
    local fieldsOk, setup, slot = pcall(function() return tonumber(selected.setup), tostring(selected.slotName) end)
    if not fieldsOk or not setup or setup < 0 or setup ~= math.floor(setup) or not slot or slot == "" then return nil end
    return { setup = setup, slot = slot }
end
local function appearanceReady(ctrl)
    return appearanceSelection(ctrl) ~= nil
end
local function clear()
    local old = current
    current = nil
    -- Stop retries before restoration can re-enter a script callback. Keep the
    -- native scope alive until fingerprint-guarded restoration has finished.
    if old and old.localDiagnostic then restoreLocal(old.localDiagnostic, old.epoch) end
    if old then pcall(function() Game.VRPreviewScopeClear(old.epoch) end) end
end
local function publish(ready)
    if not current then return false end
    local ok, accepted = pcall(function()
        if current.screen == 2 then return Game.VRPreviewResolveAppearanceAndPublish(current.ctrl, current.epoch, ready) end
        return Game.VRPreviewResolveAndPublish(current.ctrl, current.epoch, 1, ready)
    end)
    if not ok and not warned then
        warned = true
        print("[Stereo.MenuPreview] typed resolver unavailable; preview scope remains disabled")
    end
    if ready and (not ok or accepted ~= true) and diagnosticRows < 16 then
        diagnosticRows = diagnosticRows + 1
        local fieldsOk, fields = pcall(function()
            return "scene=" .. tostring(current.ctrl.sceneName) .. " cameraRef=" .. tostring(current.ctrl.cameraRef)
        end)
        print("[Stereo.MenuPreview] resolve-rejected epoch=" .. tostring(current.epoch) ..
            " callOk=" .. tostring(ok) .. " result=" .. tostring(accepted) ..
            " " .. (fieldsOk and fields or "fields-unavailable"))
    end
    return ok and accepted == true
end
local function refreshAppearance()
    if not current or current.screen ~= 2 then return end
    -- A category changes the producer, not the lifetime of the accepted
    -- complete stereo body/UI display. Restore only our old camera fingerprint.
    if current.localDiagnostic then restoreLocal(current.localDiagnostic, current.epoch) end
    current.localDiagnostic, current.localStarted, current.localLastStatus = nil, nil, nil
    current.widgetScan, current.widgetScanStarted = nil, nil
    current.ready = false
    publish(false)
end
local function begin(ctrl)
    local screen = isInventory(ctrl) and 1 or (isAppearance(ctrl) and 2 or nil)
    if not screen then return end
    local selection
    if screen == 2 then
        pendingAppearance = ctrl
        selection = appearanceSelection(ctrl)
        if not selection then
            if current and not same(current.ctrl, ctrl) then clear() end
            return
        end
    end
    if current and current.screen == screen and same(current.ctrl, ctrl) then return end
    clear()
    local ok, epoch = pcall(function() return Game.VRPreviewScopeBegin() end)
    if ok and epoch and epoch ~= 0 then
        current = { ctrl = ctrl, owner = screen == 1 and activeOwner or nil, screen = screen, epoch = epoch,
            setup = selection and selection.setup or 0, slot = selection and selection.slot or nil, ready = false }
        elapsed = 0
    end
end
local function closeOwner(owner)
    if current and same(current.owner, owner) then clear() end
    if same(activeOwner, owner) then activeOwner = nil end
end
function M.init()
    local stereoOk, stereo = pcall(function() return Game.VRPreviewStereoUiEnabled() end)
    stereoEnabled = stereoOk and stereo == true
    print("[Stereo.MenuPreview] stereo-ui request callOk=" .. tostring(stereoOk) .. " value=" .. tostring(stereo))
    ObserveBefore("gameuiInventoryGameController", "OnInitialize", function(owner)
        activeOwner = owner
    end)
    ObserveBefore("inkPuppetPreviewGameController", "OnInitialize", function(ctrl)
        if isInventory(ctrl) or isAppearance(ctrl) then begin(ctrl) end
    end)
    ObserveAfter("inkPuppetPreviewGameController", "OnPreviewInitialized", function(ctrl)
        if not isInventory(ctrl) and not isAppearance(ctrl) then return end
        if not current or not same(current.ctrl, ctrl) then begin(ctrl) end
        if current and same(current.ctrl, ctrl) then current.ready = publish(true) end
    end)
    ObserveBefore("inkInventoryPuppetPreviewGameController", "OnSetCameraSetupEvent", function(ctrl)
        if current and same(current.ctrl, ctrl) then
            if current.localDiagnostic then
                restoreLocal(current.localDiagnostic, current.epoch)
                current.localStarted = true -- no reapply after an existing test
            end
            current.ready = false; publish(false)
        end
    end)
    ObserveAfter("inkInventoryPuppetPreviewGameController", "OnSetCameraSetupEvent", function(ctrl)
        if current and same(current.ctrl, ctrl) then current.ready = publish(true) end
    end)
    ObserveBefore("inkCharacterCreationPuppetPreviewGameController", "OnSetCameraSetupEvent", function(ctrl)
        pendingAppearance = ctrl
        if current and same(current.ctrl, ctrl) then refreshAppearance() end
    end)
    ObserveAfter("inkCharacterCreationPuppetPreviewGameController", "OnSetCameraSetupEvent", function(ctrl)
        pendingAppearance = ctrl
        if appearanceReady(ctrl) then
            begin(ctrl)
            if current and same(current.ctrl, ctrl) then
                local selected = appearanceSelection(ctrl)
                if selected then current.setup, current.slot = selected.setup, selected.slot end
                current.ready = publish(true)
            end
        end
    end)
    -- BaseMenu.CloseMenu dispatches OnCloseMenu before hiding a cached menu.
    -- OnUninitialize is destruction and is not guaranteed on ordinary close.
    ObserveBefore("gameuiInventoryGameController", "OnCloseMenu", closeOwner)
    ObserveBefore("gameuiInventoryGameController", "OnUninitialize", closeOwner)
    -- Vanilla BaseMenu.OnLeaveScenario closes the current menu. This boundary
    -- does not depend on an Inventory owner callback having arrived first.
    ObserveBefore("MenuScenario_BaseMenu", "OnLeaveScenario", function()
        clear()
        activeOwner = nil
        pendingAppearance = nil
    end)
    ObserveBefore("inkPuppetPreviewGameController", "OnUninitialize", function(ctrl)
        if current and same(current.ctrl, ctrl) then clear() end
        if pendingAppearance and same(pendingAppearance, ctrl) then pendingAppearance = nil end
    end)
    ObserveBefore("inkCharacterCreationPuppetPreviewGameController", "OnUninitialize", function(ctrl)
        if current and same(current.ctrl, ctrl) then clear() end
        if pendingAppearance and same(pendingAppearance, ctrl) then pendingAppearance = nil end
    end)
end
function M.tick(dt)
    if pendingAppearance and appearanceReady(pendingAppearance) and
        (not current or not same(current.ctrl, pendingAppearance)) then begin(pendingAppearance) end
    if not current then return end
    if current.screen == 2 then
        local selected = appearanceSelection(current.ctrl)
        if not selected then refreshAppearance(); return end
        if current.setup ~= selected.setup or current.slot ~= selected.slot then
            refreshAppearance()
            current.setup, current.slot = selected.setup, selected.slot
        end
    end
    local delta = tonumber(dt) or 0
    elapsed = elapsed + math.max(delta, 0)
    if not stereoEnabled and delta > 0 and elapsed < 0.1 then return end
    elapsed = 0
    -- A native readiness callback may precede observer registration or not be
    -- delivered through the observed base function. A typed retry is safe:
    -- native publication validates attached owner,
    -- exactly one enabled RTT camera and a valid actual camera setup.
    current.ready = publish(true)
    if current.ready then
        if not current.widgetScanStarted then
            current.widgetScanStarted = true
            local ok, state = pcall(function() return Game.VRPreviewWidgetAliasStart(current.ctrl, current.epoch) end)
            if ok then current.widgetScan = state end
        end
        if current.widgetScan then
            local ok, err = pcall(function() Game.VRPreviewWidgetAliasStep(current.widgetScan) end)
            if not ok and diagnosticRows < 16 then
                diagnosticRows = diagnosticRows + 1
                print("[Stereo.MenuPreview] typed widget scan failed: " .. tostring(err))
            end
        end
        if not current.localStarted then
            current.localStarted = true
            local setup = current.setup
            local ok, state = pcall(function() return Game.VRPreviewLocalStart(current.ctrl, current.epoch, setup) end)
            if ok then current.localDiagnostic = state end
            current.localStartOk = ok
            current.localStartResult = tostring(state)
            print("[Stereo.MenuPreview] local start callOk=" .. tostring(ok) .. " result=" .. tostring(state))
        end
        if current.localDiagnostic then
            local ok, err = pcall(function() Game.VRPreviewLocalStep(current.localDiagnostic) end)
            if not ok and diagnosticRows < 16 then
                diagnosticRows = diagnosticRows + 1
                print("[Stereo.MenuPreview] local diagnostic callback failed: " .. tostring(err))
            end
            local statusOk, status = pcall(function() return tostring(current.localDiagnostic.status) end)
            if statusOk and status ~= current.localLastStatus then
                current.localLastStatus = status
                print("[Stereo.MenuPreview] local stage epoch=" .. tostring(current.epoch) .. " status=" .. status)
            end
        end
    end
end
-- A fresh typed publication validates this appearance controller, epoch and RTT owner.
-- Readiness is independent of XR pair submission; suspending player VRCAM cannot block it.
function M.ownsAppearanceStereo()
    return stereoEnabled and current ~= nil and current.screen == 2 and current.ready == true and publish(true)
end
function M.getState()
    local pairOk, pairReady = pcall(function() return current and Game.VRPreviewStereoUiPairSubmitted(current.epoch) end)
    local halfOk, half = pcall(function() return Game.VRPreviewLocalHalfIpd() end)
    local diagnostic = current and current.localDiagnostic
    local status, localReady, eye = "disabled", false, -1
    if diagnostic then
        local ok, result = pcall(function() return { status = diagnostic.status, ready = diagnostic.ready == true, eye = diagnostic.eye } end)
        if ok then status, localReady, eye = tostring(result.status), result.ready, tonumber(result.eye) or -1 end
    end
    local poseObservation
    if diagnostic then
        local ok, result = pcall(function()
            local function xyz(value) return { x = value.x, y = value.y, z = value.z } end
            local world = GetSingleton('Matrix'):GetTranslation(diagnostic.camera:GetLocalToWorld())
            local position = diagnostic.camera:GetLocalPosition()
            local baseline, expected, applied = diagnostic.worldBaseline, diagnostic.expectedWorld, diagnostic.appliedPosition
            return { applied = diagnostic.applied == true, published = diagnostic.published == true,
                finished = diagnostic.finished == true, baseline = xyz(baseline), expected = xyz(expected),
                world = xyz(world), local_position = xyz(position), applied_position = xyz(applied),
                residual = { x = world.x - baseline.x - expected.x, y = world.y - baseline.y - expected.y,
                    z = world.z - baseline.z - expected.z } }
        end)
        poseObservation = ok and result or { error = tostring(result) }
    end
    local scan = current and current.widgetScan
    local widgetState = { enabled = false, status = "disabled", passes = 0, visited = 0, images = 0, matches = 0, truncated = false }
    if scan then
        local ok, result = pcall(function() return { enabled = true, status = tostring(scan.status), passes = tonumber(scan.passes) or 0,
            visited = tonumber(scan.visited) or 0, images = tonumber(scan.images) or 0, matches = tonumber(scan.totalMatches) or 0, truncated = scan.truncated == true } end)
        if ok then widgetState = result end
    end
    return { ready = current ~= nil and current.ready == true,
        stereo_ui_enabled = stereoEnabled, stereo_pair_submitted = pairOk and pairReady == true,
        local_start_ok = current and current.localStartOk, local_start_result = current and current.localStartResult,
        local_half_ok = halfOk, local_half_ipd = halfOk and tonumber(half) or 0,
        epoch = current and current.epoch or 0, screen = current and current.screen or 0,
        setup = current and current.setup or -1, slot = current and current.slot or nil,
        local_camera_status = status, local_camera_ready = localReady, local_camera_eye = eye,
        local_pose_observation = poseObservation,
        widget_alias_enabled = widgetState.enabled, widget_alias_status = widgetState.status,
        widget_alias_passes = widgetState.passes, widget_alias_visited = widgetState.visited, widget_alias_images = widgetState.images,
        widget_alias_matches = widgetState.matches, widget_alias_truncated = widgetState.truncated }
end
function M.shutdown() clear(); activeOwner = nil; pendingAppearance = nil end
return M
