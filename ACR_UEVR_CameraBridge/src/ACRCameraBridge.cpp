#include <memory>
#include <mutex>
#include <deque>
#include <sstream>
#include <filesystem>
#include <array>
#include <windows.h>
#include "uevr/Plugin.hpp"
#include "BridgeCore.hpp"
#include "BridgeSettings.hpp"
#include "CameraResolver.hpp"

using namespace uevr;
namespace {
using RotatorD = bridge::Rotation;
struct CameraLayout {
    std::ptrdiff_t rotation_offset{-1};
    bool rotation_is_double{false}, valid{false};
};
bool field_class_is(API::FProperty *prop, const wchar_t *name) {
    return prop && prop->get_class() && prop->get_class()->get_name() == name;
}
class ACRCameraBridge final : public Plugin {
  public:
    ACRCameraBridge() { s_self = this; }
    void on_initialize() override {
        wchar_t exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        m_acr = _wcsicmp(std::filesystem::path(exe).filename().c_str(), L"acr.exe") == 0;
        // Use the API's buffer contract directly (room for the terminator).
        auto fn = API::get()->param()->functions->get_persistent_dir;
        std::wstring dir(fn(nullptr, 0) + 1, L'\0');
        fn(dir.data(), static_cast<unsigned>(dir.size()));
        dir.resize(std::wcslen(dir.c_str()));
        m_ini = std::filesystem::path(dir) / L"ACR_CameraBridge.ini";
        char value[64]{};
        GetPrivateProfileStringA("Bridge", "enabled", "", value, sizeof(value), m_ini.string().c_str());
        if (*value)
            bridge::change(m_settings, "enabled", value);
        m_message = m_acr ? "Waiting for the driving camera" : "Inactive: this plugin is for acr.exe";
        m_early_registered = API::get()->sdk()->callbacks->on_early_calculate_stereo_view_offset(
            [](UEVR_StereoRenderingDeviceHandle device, int index, float meters, UEVR_Vector3f *position,
               UEVR_Rotatorf *rotation,
               bool dbl) { s_self->on_early_stereo(device, index, meters, position, rotation, dbl); });
        API::get()->log_info("[ACR CameraBridge v2.3] loaded; early stereo callback=%d", int(m_early_registered));
    }
    bool on_message(HWND, UINT msg, WPARAM key, LPARAM param) override {
        if (key != VK_F9)
            return true;
        std::scoped_lock lock(m_mutex);
        // Ignore auto-repeat and duplicate delivery of a key-down event.
        bool &down = m_f9_down;
        if (msg == WM_KEYUP || msg == WM_SYSKEYUP) {
            down = false;
            return true;
        }
        if (msg != WM_KEYDOWN && msg != WM_SYSKEYDOWN)
            return true;
        if (down || (static_cast<std::uint64_t>(param) & (1ull << 30)))
            return true;
        down = true;
        m_toggle_bridge = !m_toggle_bridge;
        return true;
    }
    void on_custom_event(const char *name, const char *data) override {
        if (!name || std::string_view(name) != "acr.camera.command.v2" || !data || strnlen_s(data, 257) > 256)
            return;
        std::istringstream in(data);
        std::string protocol, id, key, value;
        std::getline(in, protocol);
        std::getline(in, id);
        std::getline(in, key);
        std::getline(in, value);
        double request{};
        if (protocol != "2" || !bridge::number(id, request) || request < 0 || request > 1000000000 ||
            std::floor(request) != request)
            return;
        std::string snapshot;
        {
            std::scoped_lock lock(m_mutex);
            if (key != "get") {
                auto candidate = m_settings;
                if (bridge::change(candidate, key, value) && m_commands.size() < 32) {
                    m_commands.push_back({static_cast<unsigned>(request), key, value});
                    m_error.clear();
                } else {
                    m_rejected = static_cast<unsigned>(request);
                    m_error = "Invalid setting or command queue full";
                }
            }
            snapshot = status_locked();
        }
        // Never enter Lua while holding the camera/state mutex.
        API::get()->dispatch_lua_event("acr.camera.status.v2", snapshot);
    }
    void on_pre_engine_tick(API::UGameEngine *, float) override {
        std::scoped_lock lock(m_mutex);
        if (!m_acr)
            return;
        m_game_thread = GetCurrentThreadId();
        ++m_frame;
        restore_locked();
        apply_commands_locked();
        auto *current = get_player_camera_manager();
        if (current != m_pcm) {
            m_pcm = current;
            m_layout = {};
            m_clean_valid = false;
            m_override_written = false;
            m_resolved = false;
            m_resolve_attempted = false;
            m_logged_layout_failure = false;
            ++m_generation;
            m_snapshots = {};
            m_render_valid = false;
            m_runtime_error = false;
            API::get()->log_info("[ACR CameraBridge] camera manager -> %p", current);
        }
        m_hmd = API::VR::is_runtime_ready() && API::VR::is_hmd_active();
        m_screen = API::VR::get_mod_value<bool>("VR_2DScreenMode");
        if (!m_pcm) {
            m_message = "Waiting for PlayerCameraManager";
            return;
        }
        if (!m_layout.valid && !resolve_layout_locked()) {
            m_message = "Camera layout unavailable; no writes";
            return;
        }
        if (!m_resolve_attempted)
            install_hook_locked();
        if (!m_settings.enabled)
            m_message = "Disabled; original camera restored";
        else if (!m_hmd || m_screen)
            m_message = "Waiting for active VR driving view";
        // Preserve the resolver failure message for the status panel.
    }
    void on_early_stereo(UEVR_StereoRenderingDeviceHandle device, int index, float, UEVR_Vector3f *,
                         UEVR_Rotatorf *rotation, bool is_double) {
        std::scoped_lock lock(m_mutex);
        auto &pair = view_pair();
        pair = {};
        if (!stereo_candidate(index, rotation) || !m_settings.enabled)
            return;
        const auto input = read_view(rotation, is_double);
        const auto snapshot = match_snapshot_locked(input, is_double);
        if (!snapshot)
            return;
        pair.device = device;
        pair.pointer = rotation;
        pair.index = index;
        pair.is_double = is_double;
        pair.snapshot = *snapshot;
        pair.matched = true;
    }
    void on_pre_calculate_stereo_view_offset(UEVR_StereoRenderingDeviceHandle device, int index, float, UEVR_Vector3f *,
                                             UEVR_Rotatorf *rotation, bool is_double) override {
        std::scoped_lock lock(m_mutex);
        if (!stereo_candidate(index, rotation))
            return;
        if (!m_settings.enabled)
            return;
        const auto input = read_view(rotation, is_double);
        auto &pair = view_pair();
        // The early callback identifies our pose before UEVR's camera freeze/lerp
        // can change it. The pre callback runs after those modifiers and supplies
        // the matching fresh game base. It does not depend on viewport hook scope.
        bool paired = pair.matched && pair.device == device && pair.pointer == rotation && pair.index == index &&
                      pair.is_double == is_double && snapshot_current(pair.snapshot);
        if (!paired) {
            pair = {};
            const auto snapshot = match_snapshot_locked(input, is_double);
            if (!snapshot)
                return;
            pair.device = device;
            pair.pointer = rotation;
            pair.index = index;
            pair.is_double = is_double;
            pair.snapshot = *snapshot;
        }
        pair.matched = false;
        pair.render_pending = true;
        write_view(rotation, is_double, pair.snapshot.clean);
    }
    void on_post_calculate_stereo_view_offset(UEVR_StereoRenderingDeviceHandle device, int index, float,
                                              UEVR_Vector3f *, UEVR_Rotatorf *rotation, bool is_double) override {
        std::scoped_lock lock(m_mutex);
        auto &pair = view_pair();
        if (pair.render_pending && pair.device == device && pair.pointer == rotation && pair.index == index &&
            pair.is_double == is_double && snapshot_current(pair.snapshot)) {
            const auto r = read_view(rotation, is_double);
            if (bridge::finite(r)) {
                m_render_valid = true;
                m_render_ms = GetTickCount64();
            }
        }
        pair = {};
    }
#ifdef BRIDGE_TEST
  public:
#else
  private:
#endif
    using UpdateFn = void (*)(API::UObject *, float);
    static inline ACRCameraBridge *s_self{};
    static void update_camera(API::UObject *pcm, float dt) {
        auto *self = s_self;
        const auto original = reinterpret_cast<UpdateFn>(self->m_original);
        bool ours = false;
        {
            std::scoped_lock lock(self->m_mutex);
            ours = self->m_resolved && pcm == self->m_pcm && GetCurrentThreadId() == self->m_game_thread;
            if (ours)
                self->restore_locked();
        }
        // UEVR fills the trampoline before enabling its hook. No plugin mutex across game code.
        original(pcm, dt);
        if (!ours)
            return;
        std::scoped_lock lock(self->m_mutex);
        if (pcm != self->m_pcm || !self->valid_manager_locked())
            return;
        self->capture_clean_locked();
        self->publish_locked();
    }
    void install_hook_locked() {
        m_resolve_attempted = true;
        auto *controller = API::get()->get_player_controller(0);
        auto **field = controller ? controller->get_property_data<API::UObject *>(L"PlayerCameraManager") : nullptr;
        if (!field || *field != m_pcm) {
            m_message = "Controller-camera link unavailable";
            return;
        }
        const auto offset = reinterpret_cast<std::uintptr_t>(field) - reinterpret_cast<std::uintptr_t>(controller);
        if (offset > 0x10000) {
            m_message = "Controller field failed validation";
            return;
        }
        const auto target = bridge::resolve(m_pcm, static_cast<std::uint32_t>(offset), m_target);
        if (!target) {
            m_message = "UpdateCamera resolver failed or was ambiguous";
            return;
        }
        if (m_target && target->function != m_target) {
            m_message = "Camera implementation changed; restart required";
            return;
        }
        m_slot = target->slot;
        if (!m_target) {
            m_hook_id = API::get()->param()->functions->register_inline_hook(
                target->function, reinterpret_cast<void *>(&update_camera), &m_original);
            if (m_hook_id < 0) {
                m_original = nullptr;
                m_message = "UEVR could not install camera hook";
                return;
            }
            m_target = target->function;
        }
        m_resolved = true;
        m_message = "Camera hook ready; waiting for camera update";
        API::get()->log_info("[ACR CameraBridge] UpdateCamera hooked: slot %d, target %p", m_slot, m_target);
    }
    bool valid_manager_locked() { return m_pcm && m_layout.valid && get_player_camera_manager() == m_pcm; }
    struct Snapshot {
        RotatorD clean{}, written{};
        std::uint64_t frame{}, generation{};
        bool is_double{}, valid{};
    };
    struct ViewPair {
        UEVR_StereoRenderingDeviceHandle device{};
        UEVR_Rotatorf *pointer{};
        int index{};
        bool is_double{}, matched{}, render_pending{};
        Snapshot snapshot{};
    };
    static ViewPair &view_pair() {
        static thread_local ViewPair pair;
        return pair;
    }
    bool stereo_candidate(int index, const void *rotation) const {
        return m_acr && rotation && index >= 0 && index <= 2;
    }
    bool snapshot_current(const Snapshot &s) const {
        return s.valid && s.generation == m_generation && s.frame <= m_frame && m_frame - s.frame <= 3;
    }
    std::optional<Snapshot> match_snapshot_locked(RotatorD input, bool dbl) const {
        for (std::size_t n = 0; n < m_snapshots.size(); ++n) {
            // Do not mistake the new camera's already-clean input for an older
            // head-facing snapshot that happens to have the same orientation.
            if (n > 0 && m_clean_valid && m_clean_frame == m_frame && bridge::same(input, m_clean_rotation, 0.05))
                return {};
            const auto &s = m_snapshots[(m_snapshot_cursor + m_snapshots.size() - 1 - n) % m_snapshots.size()];
            if (snapshot_current(s) && s.is_double == dbl && bridge::same(input, s.written, 0.05))
                return s;
        }
        return {};
    }
    static RotatorD read_view(UEVR_Rotatorf *r, bool dbl) {
        if (dbl) {
            auto *d = reinterpret_cast<UEVR_Rotatord *>(r);
            return {d->pitch, d->yaw, d->roll};
        }
        return {r->pitch, r->yaw, r->roll};
    }
    static void write_view(UEVR_Rotatorf *r, bool dbl, RotatorD v) {
        if (dbl)
            *reinterpret_cast<UEVR_Rotatord *>(r) = {v.pitch, v.yaw, v.roll};
        else
            *r = {static_cast<float>(v.pitch), static_cast<float>(v.yaw), static_cast<float>(v.roll)};
    }
    void capture_clean_locked() {
        m_clean_valid = read_rotation_locked(m_clean_rotation) && bridge::finite(m_clean_rotation);
        m_clean_frame = m_frame;
    }
    void publish_locked() {
        if (!m_settings.enabled || !m_hmd || m_screen || !m_clean_valid)
            return;
        const auto offset = API::VR::get_rotation_offset();
        const auto pose = API::VR::get_pose(API::VR::get_hmd_index());
        const auto q = pose.rotation;
        const auto relative = bridge::relative_head({offset.x, offset.y, offset.z, offset.w}, {q.x, q.y, q.z, q.w});
        const auto head =
            relative ? bridge::head_center(m_clean_rotation, {}, *relative, API::VR::is_decoupled_pitch_enabled())
                     : std::nullopt;
        if (!head) {
            m_message = "Head pose invalid; no camera override";
            m_runtime_error = true;
            return;
        }
        const auto tracked = *head;
        if (!write_rotation_locked(tracked)) {
            m_message = "Camera write failed";
            m_runtime_error = true;
            return;
        }
        m_written_rotation = tracked;
        if (!m_layout.rotation_is_double)
            m_written_rotation = {static_cast<float>(tracked.pitch), static_cast<float>(tracked.yaw),
                                  static_cast<float>(tracked.roll)};
        m_override_written = true;
        m_snapshots[m_snapshot_cursor] = {m_clean_rotation, m_written_rotation,          m_frame,
                                          m_generation,     m_layout.rotation_is_double, true};
        m_snapshot_cursor = (m_snapshot_cursor + 1) % m_snapshots.size();
        m_runtime_error = false;
        m_message = "Waiting for stereo view";
    }
    void restore_locked() {
        if (!m_override_written)
            return;
        if (valid_manager_locked()) {
            RotatorD current{};
            if (read_rotation_locked(current) && current.pitch == m_written_rotation.pitch &&
                current.yaw == m_written_rotation.yaw && current.roll == m_written_rotation.roll)
                write_rotation_locked(m_clean_rotation);
        }
        m_override_written = false;
    }
    template <class T> static bool safe_read(const void *p, T &value) { return bridge::read(p, value); }
    template <class T> static bool safe_write(void *p, const T &value) { return bridge::write(p, value); }
    static void log_once(bool &flag, const char *message) {
        if (!flag) {
            flag = true;
            API::get()->log_warn("%s", message);
        }
    }
    API::UObject *get_player_camera_manager() {
        auto *pc = API::get()->get_player_controller(0);
        if (pc == nullptr) {
            return nullptr;
        }

        auto **pcm_slot = pc->get_property_data<API::UObject *>(L"PlayerCameraManager");
        if (pcm_slot == nullptr) {
            return nullptr;
        }

        return *pcm_slot;
    }

    bool resolve_layout_locked() {
        if (m_pcm == nullptr || m_pcm->get_class() == nullptr) {
            return false;
        }

        auto *cache_base_prop = m_pcm->get_class()->find_property(L"CameraCachePrivate");
        if (!field_class_is(cache_base_prop, L"StructProperty")) {
            log_once(m_logged_layout_failure, "[ACR CameraBridge] CameraCachePrivate is missing/not a StructProperty");
            return false;
        }

        auto *cache_prop = reinterpret_cast<API::FStructProperty *>(cache_base_prop);
        auto *cache_struct = cache_prop->get_struct();
        if (cache_struct == nullptr) {
            log_once(m_logged_layout_failure, "[ACR CameraBridge] couldn't resolve FCameraCacheEntry");
            return false;
        }

        auto *pov_base_prop = cache_struct->find_property(L"POV");
        if (!field_class_is(pov_base_prop, L"StructProperty")) {
            log_once(m_logged_layout_failure,
                     "[ACR CameraBridge] CameraCachePrivate.POV is missing/not a StructProperty");
            return false;
        }

        auto *pov_prop = reinterpret_cast<API::FStructProperty *>(pov_base_prop);
        auto *view_struct = pov_prop->get_struct();
        if (view_struct == nullptr) {
            log_once(m_logged_layout_failure, "[ACR CameraBridge] couldn't resolve FMinimalViewInfo");
            return false;
        }

        auto *rotation_base_prop = view_struct->find_property(L"Rotation");
        if (!field_class_is(rotation_base_prop, L"StructProperty")) {
            log_once(m_logged_layout_failure,
                     "[ACR CameraBridge] FMinimalViewInfo.Rotation is missing/not a StructProperty");
            return false;
        }

        auto *rotation_prop = reinterpret_cast<API::FStructProperty *>(rotation_base_prop);
        auto *rotation_struct = rotation_prop->get_struct();
        if (rotation_struct == nullptr) {
            return false;
        }

        const int rotation_bytes = rotation_struct->get_struct_size();
        if (rotation_bytes != static_cast<int>(sizeof(UEVR_Rotatord)) &&
            rotation_bytes != static_cast<int>(sizeof(UEVR_Rotatorf))) {
            API::get()->log_error(
                "[ACR CameraBridge] unexpected Rotation struct size %d (expected %zu or %zu); refusing to write",
                rotation_bytes, sizeof(UEVR_Rotatord), sizeof(UEVR_Rotatorf));
            return false;
        }

        const auto pov_offset = cache_prop->get_offset() + pov_prop->get_offset();
        m_layout.rotation_offset = pov_offset + rotation_prop->get_offset();
        if (m_layout.rotation_offset < 0 || m_layout.rotation_offset > 0x10000)
            return false;
        m_layout.rotation_is_double = rotation_bytes == static_cast<int>(sizeof(UEVR_Rotatord));

        m_layout.valid = true;
        API::get()->log_info("[ACR CameraBridge] Rotation +0x%llX (%s)",
                             static_cast<unsigned long long>(m_layout.rotation_offset),
                             m_layout.rotation_is_double ? "double" : "float");
        return true;
    }

    bool read_rotation_locked(RotatorD &out) {
        if (!m_layout.valid || m_pcm == nullptr) {
            return false;
        }

        const auto *p = reinterpret_cast<const std::uint8_t *>(m_pcm) + m_layout.rotation_offset;
        if (m_layout.rotation_is_double) {
            UEVR_Rotatord r{};
            if (!safe_read(p, r))
                return false;
            out = {r.pitch, r.yaw, r.roll};
        } else {
            UEVR_Rotatorf r{};
            if (!safe_read(p, r))
                return false;
            out = {r.pitch, r.yaw, r.roll};
        }
        return true;
    }

    bool write_rotation_locked(const RotatorD &in) {
        if (!m_layout.valid || m_pcm == nullptr) {
            return false;
        }

        auto *p = reinterpret_cast<std::uint8_t *>(m_pcm) + m_layout.rotation_offset;
        if (m_layout.rotation_is_double) {
            const UEVR_Rotatord r{in.pitch, in.yaw, in.roll};
            return safe_write(p, r);
        }

        const UEVR_Rotatorf r{static_cast<float>(in.pitch), static_cast<float>(in.yaw), static_cast<float>(in.roll)};
        return safe_write(p, r);
    }

    struct Command {
        unsigned id;
        std::string key, value;
    };
    void apply_commands_locked() {
        bool changed = false;
        while (!m_commands.empty()) {
            auto cmd = std::move(m_commands.front());
            m_commands.pop_front();
            if (!bridge::change(m_settings, cmd.key, cmd.value)) {
                m_rejected = cmd.id;
                m_error = "Setting was rejected";
                continue;
            }
            m_applied = cmd.id;
            changed = true;
        }
        if (m_toggle_bridge) {
            m_settings.enabled = !m_settings.enabled;
            m_toggle_bridge = false;
            changed = true;
        }
        if (!changed)
            return;
        m_clean_valid = false;
        m_render_valid = false;
        m_snapshots = {};
        ++m_generation;
        std::ostringstream ini;
        ini << "[Bridge]\nenabled=" << int(m_settings.enabled) << "\n";
        // A small atomic replacement; an interrupted save keeps the prior file intact.
        auto temp = m_ini;
        temp += L".tmp";
        const auto content = ini.str();
        HANDLE file =
            CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        bool saved = false;
        if (file != INVALID_HANDLE_VALUE) {
            DWORD written{};
            saved = WriteFile(file, content.data(), static_cast<DWORD>(content.size()), &written, nullptr) &&
                    written == content.size();
            if (saved)
                saved = FlushFileBuffers(file) != FALSE;
            CloseHandle(file);
            if (saved)
                saved = MoveFileExW(temp.c_str(), m_ini.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) !=
                        FALSE;
        }
        m_error = saved ? "" : "Settings applied, but saving the INI failed";
        API::get()->log_info("[ACR CameraBridge v2.3] enabled=%d", int(m_settings.enabled));
    }
    const char *state_locked() const {
        if (!m_settings.enabled)
            return "Disabled";
        if (!m_acr || !m_error.empty() || m_runtime_error ||
            (m_pcm && (!m_layout.valid || (m_resolve_attempted && !m_resolved))))
            return "Error";
        if (m_hmd && !m_screen && m_resolved && m_render_valid && GetTickCount64() - m_render_ms < 2000)
            return "Active";
        return "Waiting";
    }
    std::string status_locked() const {
        std::ostringstream s;
        s.imbue(std::locale::classic());
        s << std::boolalpha;
        s << "{\"protocol\":2,\"version\":\"2.3.0\",\"applied\":" << m_applied << ",\"rejected\":" << m_rejected
          << ",\"state\":\"" << state_locked() << "\",\"message\":\"" << m_message << "\",\"error\":\"" << m_error
          << "\",\"settings\":{\"enabled\":" << m_settings.enabled << "}}";
        return s.str();
    }
    std::mutex m_mutex;
    bridge::Settings m_settings;
    std::deque<Command> m_commands;
    std::filesystem::path m_ini;
    std::string m_message{"Starting"}, m_error;
    unsigned m_applied{}, m_rejected{};
    bool m_acr{}, m_hmd{}, m_screen{}, m_toggle_bridge{}, m_f9_down{};
    API::UObject *m_pcm{};
    CameraLayout m_layout{};
    RotatorD m_clean_rotation{}, m_written_rotation{};
    bool m_clean_valid{}, m_override_written{}, m_logged_layout_failure{};
    void *m_target{};
    void *m_original{};
    int m_hook_id{-1}, m_slot{-1};
    bool m_resolved{}, m_resolve_attempted{};
    DWORD m_game_thread{};
    std::uint64_t m_frame{}, m_clean_frame{};
    std::array<Snapshot, 8> m_snapshots{};
    std::size_t m_snapshot_cursor{};
    std::uint64_t m_generation{}, m_render_ms{};
    bool m_early_registered{}, m_render_valid{}, m_runtime_error{};
};
std::unique_ptr<ACRCameraBridge> g_plugin = std::make_unique<ACRCameraBridge>();
} // namespace
