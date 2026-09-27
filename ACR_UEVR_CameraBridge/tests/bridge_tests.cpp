#include <iostream>
#include <stdexcept>
#include <thread>
#define BRIDGE_TEST
#include "../src/ACRCameraBridge.cpp"

void require(bool b, const char *message) {
    if (!b)
        throw std::runtime_error(message);
}
bool close_degrees(double a, double b) { return std::abs(bridge::wrap(a - b)) < 0.001; }
struct FakeCamera {
    RotatorD rotation;
    float fov{100};
};
FakeCamera camera{{0, 10, 0}}, other{{0, 80, 0}};
API::UObject *current_pcm = reinterpret_cast<API::UObject *>(&camera);
bool headset = true;
std::string last_json;
void log_stub(const char *, ...) {}
void original_camera(API::UObject *self, float) {
    require(close_degrees(reinterpret_cast<FakeCamera *>(self)->rotation.yaw, 10),
            "Original camera received modified yaw");
    reinterpret_cast<FakeCamera *>(self)->rotation = {0, 20, 0};
}
int main() try {
    // Math: identities, recentering, wrap-around, decoupled pitch and invalid poses.
    for (double p : {-70., 0., 50.})
        for (double y : {-179., -30., 0., 179.})
            for (double r : {-40., 0., 20.}) {
                auto result = bridge::head_center({p, y, r}, {}, {}, false);
                require(result && bridge::same(*result, {p, y, r}), "Identity headset changed base orientation");
            }
    auto turn = bridge::head_center({0, 170, 0}, {}, bridge::axis(1, -30), false);
    require(turn && close_degrees(turn->yaw, -160), "Head yaw composition or wrapping failed");
    auto recentered = bridge::head_center({0, 25, 0}, bridge::axis(1, 30), bridge::axis(1, -30), false);
    require(recentered && close_degrees(recentered->yaw, 25), "Recenter offset failed");
    auto flat = bridge::head_center({25, 45, 18}, {}, {}, true);
    require(flat && bridge::same(*flat, {0, 45, 0}), "Decoupled base failed");
    require(!bridge::head_center({0, 0, 0}, {}, {0, 0, 0, 0}, false), "Zero quaternion accepted");
    require(!bridge::head_center({NAN, 0, 0}, {}, {}, false), "Nonfinite camera accepted");
    auto relative = bridge::relative_head(bridge::axis(1, 30), bridge::axis(1, -30));
    require(relative && bridge::same(bridge::to_rotation(*relative), {}), "Relative head recenter failed");
    require(!bridge::relative_head({}, {0, 0, 0, 0}), "Invalid relative head accepted");
    // Resolver: wrong controller, truncation, jump shape and ambiguous slots.
    std::vector<std::uint8_t> code = {0x48, 0x8b, 0x89, 0x60, 0x03, 0,    0,    0x48, 0x85, 0xc9, 0x74,
                                      0x0a, 0x48, 0x8b, 0x01, 0x48, 0xff, 0xa0, 0xb0, 0x07, 0,    0};
    auto hits = bridge::forwarders(code, 0x360);
    require(hits.size() == 1 && hits[0].slot == 246, "Known forwarder rejected");
    require(bridge::forwarders(code, 0x368).empty(), "Wrong controller field accepted");
    require(bridge::forwarders(std::span(code).first(21), 0x360).empty(), "Truncated forwarder accepted");
    code[11] = 0x09;
    require(bridge::forwarders(code, 0x360).empty(), "Wrong branch target accepted");
    require(!bridge::unique_slot({240, 246}) && bridge::unique_slot({246, 246}) == 246,
            "Resolver ambiguity handling failed");
    bridge::Settings settings;
    require(!bridge::change(settings, "fov", "170") && !bridge::change(settings, "timing", "0") &&
                !bridge::change(settings, "yaw_gain", "1") && !bridge::change(settings, "enabled", "true"),
            "Invalid controls accepted");

    // Mock only the SDK boundary; exercise the actual plugin callbacks and memory writes.
    UEVR_PluginFunctions functions{};
    functions.log_info = functions.log_warn = functions.log_error = &log_stub;
    functions.dispatch_lua_event = [](const char *, const char *data) { last_json = data; };
    UEVR_SDKFunctions sdk_functions{};
    sdk_functions.get_player_controller = [](int) { return reinterpret_cast<UEVR_UObjectHandle>(0x2220); };
    UEVR_UObjectFunctions objects{};
    objects.get_property_data = [](UEVR_UObjectHandle, const wchar_t *name) -> void * {
        return std::wcscmp(name, L"PlayerCameraManager") == 0 ? static_cast<void *>(&current_pcm) : nullptr;
    };
    UEVR_SDKData sdk{};
    sdk.functions = &sdk_functions;
    sdk.uobject = &objects;
    UEVR_VRData vr{};
    vr.is_runtime_ready = []() { return true; };
    vr.is_hmd_active = []() { return headset; };
    vr.is_decoupled_pitch_enabled = []() { return false; };
    vr.get_hmd_index = []() { return 0; };
    vr.get_mod_value = [](const char *, char *out, unsigned int size) { strncpy_s(out, size, "false", _TRUNCATE); };
    vr.get_rotation_offset = [](UEVR_Quaternionf *out) { *out = {1, 0, 0, 0}; };
    vr.get_pose = [](int, UEVR_Vector3f *pos, UEVR_Quaternionf *rot) {
        *pos = {};
        auto q = bridge::axis(1, -30);
        *rot = {float(q.w), float(q.x), float(q.y), float(q.z)};
    };
    UEVR_PluginInitializeParam param{};
    param.functions = &functions;
    param.sdk = &sdk;
    param.vr = &vr;
    API::initialize(&param);
    auto &plugin = *g_plugin;
    plugin.m_acr = true;
    plugin.m_pcm = current_pcm;
    plugin.m_layout = {0, true, true};
    plugin.m_resolved = true;
    plugin.m_resolve_attempted = true;
    plugin.m_original = reinterpret_cast<void *>(&original_camera);
    plugin.m_ini = std::filesystem::current_path() / "bridge-test-settings.ini";
    plugin.on_pre_engine_tick(nullptr, 0.01f);
    ACRCameraBridge::update_camera(current_pcm, 0.01f);
    require(close_degrees(camera.rotation.yaw, 50) && camera.fov == 100,
            "Native hook did not publish fresh clean+30 yaw");
    require(std::string_view(plugin.state_locked()) == "Waiting", "Reported Active before stereo compensation");
    // Regression: actual ACR had zero accepted viewport callbacks. Match our
    // injected pose directly, including when rendering occurs on another thread.
    bool render_ok = false;
    double rendered_yaw{};
    std::thread render_thread([&] {
        UEVR_Rotatord view{0, 50, 0};
        auto *view_ptr = reinterpret_cast<UEVR_Rotatorf *>(&view);
        plugin.on_early_stereo(nullptr, 0, 1, nullptr, view_ptr, true);
        view = {0, -130, 0}; // UEVR Freeze Rotation held an old camera orientation.
        plugin.on_pre_calculate_stereo_view_offset(nullptr, 0, 1, nullptr, view_ptr, true);
        render_ok = close_degrees(view.yaw, 20);
        view.yaw += 30; // Model normal UEVR tracking applied once to the clean base.
        plugin.on_post_calculate_stereo_view_offset(nullptr, 0, 1, nullptr, view_ptr, true);
        rendered_yaw = view.yaw;
    });
    render_thread.join();
    require(render_ok && close_degrees(rendered_yaw, 50), "Zero-viewport/different-thread/frozen-base regression");
    require(std::string_view(plugin.state_locked()) == "Active", "Matched stereo did not report Active");
    plugin.m_render_ms = GetTickCount64() - 3000;
    require(std::string_view(plugin.state_locked()) == "Waiting", "Stale stereo still reports Active");
    // A changed camera under the SAME manager must use a new clean base.
    plugin.restore_locked();
    camera.rotation = {0, -60, 0};
    plugin.capture_clean_locked();
    plugin.publish_locked();
    UEVR_Rotatord switched{0, -30, 0};
    auto *switched_ptr = reinterpret_cast<UEVR_Rotatorf *>(&switched);
    plugin.on_early_stereo(nullptr, 0, 1, nullptr, switched_ptr, true);
    switched.yaw = -130;
    plugin.on_pre_calculate_stereo_view_offset(nullptr, 0, 1, nullptr, switched_ptr, true);
    require(close_degrees(switched.yaw, -60), "Camera switch retained old frozen base");
    plugin.on_post_calculate_stereo_view_offset(nullptr, 0, 1, nullptr, switched_ptr, true);
    plugin.restore_locked();
    camera.rotation = {0, 20, 0};
    plugin.capture_clean_locked();
    plugin.publish_locked();
    UEVR_Rotatord rendered{0, 50, 0};
    plugin.on_pre_calculate_stereo_view_offset(nullptr, 0, 1, nullptr, reinterpret_cast<UEVR_Rotatorf *>(&rendered),
                                               true);
    require(close_degrees(rendered.yaw, 20), "Stereo input still contained override (double rotation)");
    UEVR_Rotatord mirror{2, -50, 3};
    plugin.on_pre_calculate_stereo_view_offset(nullptr, 1, 1, nullptr, reinterpret_cast<UEVR_Rotatorf *>(&mirror),
                                               true);
    require(close_degrees(mirror.yaw, -50), "Unrelated view was overwritten");
    plugin.on_pre_engine_tick(nullptr, 0);
    require(close_degrees(camera.rotation.yaw, 20) && camera.fov == 100, "Next tick failed to restore base/FOV");
    plugin.capture_clean_locked();
    plugin.publish_locked();
    camera.rotation.yaw = 123;
    camera.fov = 110;
    plugin.restore_locked();
    require(camera.rotation.yaw == 123 && camera.fov == 110, "Restore overwrote external camera change");
    camera.rotation = {0, 10, 0};
    camera.fov = 100;
    plugin.capture_clean_locked();
    plugin.publish_locked();
    current_pcm = reinterpret_cast<API::UObject *>(&other);
    plugin.restore_locked();
    require(close_degrees(other.rotation.yaw, 80), "Manager transition modified a different camera");
    current_pcm = reinterpret_cast<API::UObject *>(&camera);
    camera.rotation = {0, 10, 0};
    plugin.capture_clean_locked();
    plugin.publish_locked();
    headset = false;
    plugin.on_pre_engine_tick(nullptr, 0);
    require(close_degrees(camera.rotation.yaw, 10) && !plugin.m_override_written,
            "HMD inactive failed to restore camera");
    plugin.on_custom_event("acr.camera.command.v2", "2\n17\nenabled\n0");
    require(plugin.m_settings.enabled, "Lua command mutated settings off the game thread");
    plugin.on_pre_engine_tick(nullptr, 0);
    require(!plugin.m_settings.enabled && plugin.m_applied == 17, "Lua queued setting was not applied");
    require(std::filesystem::exists(plugin.m_ini), "Settings persistence failed");
    headset = true;
    plugin.m_settings.enabled = true;
    plugin.on_pre_engine_tick(nullptr, 0);
    camera.rotation = {0, 10, 0};
    plugin.capture_clean_locked();
    plugin.publish_locked();
    require(close_degrees(camera.rotation.yaw, 40), "Live HMD API pose/order did not compose head direction");
    plugin.restore_locked();
    plugin.on_message(nullptr, WM_KEYDOWN, VK_F9, 0);
    plugin.on_message(nullptr, WM_KEYDOWN, VK_F9, 0); // Duplicated delivery, not another toggle.
    plugin.on_message(nullptr, WM_KEYUP, VK_F9, 0);
    plugin.on_pre_engine_tick(nullptr, 0);
    require(!plugin.m_settings.enabled && close_degrees(camera.rotation.yaw, 10),
            "Hotkey de-duplication or disable restore failed");
    // Removed controls/hotkeys must not revive experimental camera behavior.
    plugin.on_message(nullptr, WM_KEYDOWN, VK_F10, 0);
    plugin.on_message(nullptr, WM_KEYUP, VK_F10, 0);
    plugin.on_pre_engine_tick(nullptr, 0);
    require(!plugin.m_settings.enabled, "Removed F10 hotkey changed state");
    plugin.on_custom_event("acr.camera.command.v2", "2\n18\nfov\nnan");
    require(plugin.m_rejected == 18, "Malformed UI setting was accepted");
    plugin.on_custom_event("acr.camera.command.v2", "2\n19\nget\n");
    require(last_json.find("\"protocol\":2") != std::string::npos &&
                last_json.find("\"enabled\":false") != std::string::npos,
            "Lua status is wrong");
    require(std::string_view(plugin.state_locked()) == "Disabled", "Disabled status is wrong");
    std::cout << "PASS: math, resolver, native update, clean stereo, restoration, lifecycle, controls, persistence and "
                 "status\n";
    return 0;
} catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
}
