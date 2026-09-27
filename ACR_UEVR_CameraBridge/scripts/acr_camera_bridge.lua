-- ACR Camera Bridge v2. Native code owns camera memory, hooks, validation and settings.
local command_event, status_event = "acr.camera.command.v2", "acr.camera.status.v2"
local status, pending, error_text = nil, nil, nil
local frame, last_status, request = 0, 0, 0

local function send(key, value)
    request = request + 1
    if type(value) == "boolean" then value = value and "1" or "0" end
    uevr.api:dispatch_custom_event(command_event,
        "2\n" .. tostring(request) .. "\n" .. key .. "\n" .. tostring(value or ""))
    return request
end
local function apply(key, value)
    if pending then return end
    -- Set the pending id before dispatch; a response may arrive synchronously.
    pending = {id = request + 1, frame = frame}
    send(key, value)
end

uevr.sdk.callbacks.on_lua_event(function(event, text)
    if event ~= status_event then return end
    local ok, result = pcall(json.load_string, text)
    if not ok or type(result) ~= "table" or result.protocol ~= 2 or type(result.settings) ~= "table" or type(result.settings.enabled) ~= "boolean"
        or type(result.state) ~= "string" or type(result.applied) ~= "number" or type(result.rejected) ~= "number" then
        error_text = "UI/runtime mismatch. Install the matching DLL and Lua script."
        return
    end
    status, last_status = result, frame
    request = math.max(request, result.applied or 0, result.rejected or 0)
    error_text = result.error ~= "" and result.error or nil
    if pending and result.rejected == pending.id then
        pending = nil
        error_text = result.error ~= "" and result.error or "The runtime rejected this setting."
    elseif pending and result.applied >= pending.id then
        pending = nil
    end

end)

uevr.sdk.callbacks.on_frame(function()
    frame = frame + 1
    if frame % 30 == 1 then send("get") end
    if status and frame - last_status > 600 then
        status, pending = nil, nil
        error_text = "Native plugin stopped responding. Restart ACR and inject UEVR."
    end
    if pending and frame - pending.frame > 600 then
        pending = nil
        error_text = "Setting acknowledgement timed out. Resume the game, then check the displayed settings."
    end
end)

local function text(value) imgui.text(tostring(value)) end
uevr.lua.add_script_panel("ACR Camera Bridge", function()
    if not imgui.tree_node("ACR Camera Bridge") then return end
    local state = error_text and "Error" or (status and status.state or "Waiting")
    text("Status: " .. state)
    if error_text then
        text(error_text)
    elseif status and state ~= "Active" and state ~= "Disabled" then
        text(status.message)
    elseif not status then
        text("Waiting for the native plugin. Restart ACR after installing an update.")
    end
    if status then
        imgui.begin_disabled(pending ~= nil)
        local changed, value = imgui.checkbox("Enable camera bridge", status.settings.enabled)
        if changed then apply("enabled", value) end
        imgui.end_disabled()
        if pending then text("Applying...") end
        text("F9: toggle camera bridge")
    end
    imgui.tree_pop()
end)
