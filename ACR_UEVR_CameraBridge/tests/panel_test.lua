local script=assert(arg[1])
local callbacks, panel, commands, snapshot = {}, nil, {}, nil
uevr={api={},sdk={callbacks={}},lua={}}
function uevr.api:dispatch_custom_event(event,data)
    assert(event=="acr.camera.command.v2")
    commands[#commands+1]=data
end
function uevr.sdk.callbacks.on_lua_event(fn) callbacks.event=fn end
function uevr.sdk.callbacks.on_frame(fn) callbacks.frame=fn end
function uevr.lua.add_script_panel(name,fn) assert(name=="ACR Camera Bridge");panel=fn end
json={load_string=function()return snapshot end}
local click, disabled, checkbox_label=nil,false,nil
local displayed={}
imgui={}
function imgui.tree_node()return true end
function imgui.tree_pop()end
function imgui.text(value)displayed[#displayed+1]=value end
function imgui.begin_disabled(value)disabled=value end
function imgui.end_disabled()disabled=false end
function imgui.checkbox(label,value)
    assert(label=="Enable camera bridge","Unexpected experimental control")
    if label==checkbox_label and not disabled then checkbox_label=nil;return true,not value end
    return false,value
end
assert(loadfile(script))()
assert(panel and callbacks.event and callbacks.frame)
panel() -- Safe while native DLL is missing.
callbacks.frame();assert(commands[#commands]:find("\nget\n",1,true))
snapshot={protocol=2,version="2.3.0",applied=0,rejected=0,error="",message="Ready",
    state="Active",settings={enabled=true}}
callbacks.event("acr.camera.status.v2","mock")
panel();assert(table.concat(displayed,"\n"):find("Status: Active",1,true),"Active status missing")
checkbox_label="Enable camera bridge";panel()
local sent=commands[#commands]
assert(sent:find("\nenabled\n0",1,true),"Checkbox did not send disabled value")
local count=#commands
checkbox_label="Enable camera bridge";panel()
assert(#commands==count,"Pending command allowed a second change")
local id=tonumber(sent:match("^2\n(%d+)"))
snapshot.applied=id;snapshot.settings.enabled=false;snapshot.state="Disabled"
callbacks.event("acr.camera.status.v2","mock");panel()
sent=commands[#commands]
assert(sent:find("\nenabled\n1",1,true),"Acknowledgement did not re-enable controls")
id=tonumber(sent:match("^2\n(%d+)"))
snapshot.rejected=id;snapshot.error="Rejected test"
callbacks.event("acr.camera.status.v2","mock");displayed={};panel()
assert(table.concat(displayed,"\n"):find("Rejected test",1,true))
for i=1,650 do callbacks.frame() end
displayed={};panel()
assert(table.concat(displayed,"\n"):find("stopped responding",1,true),"Stale status did not report error")
snapshot={protocol=2,settings={enabled=true}}
callbacks.event("acr.camera.status.v2","mock");displayed={};panel()
assert(table.concat(displayed,"\n"):find("mismatch",1,true),"Old DLL status was accepted")
print("PASS: Lua status, enable control, polling, acknowledgements, rejection, timeout and version mismatch")
