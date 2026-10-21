#pragma once
#include <windows.h>
#include <atomic>
#include <thread>
#include <string>
#include <algorithm>
#include <memory>
#include <mutex>
class ControlPainter;
class PaletteSurface;
class PhoneAccessibility;
struct AccessibilityRequest;

// UI-thread-only geometry/input plus atomic handoff to the media thread.
// The owned palette has no taskbar entry and never outlives the receiver.
class PhoneShell {
public:
    HWND window=nullptr;
    std::atomic<bool> quit{false}, disconnect{false}, redraw{true}, chrome_dirty{true};
    std::atomic<bool> fullscreen{false}, has_video{false}, muted{false};
    std::atomic<float> volume{1.0f};
    std::atomic<int> status{0}; // ready, connecting, streaming
    std::atomic<bool> status_dismissed{false};
    PhoneShell();
    ~PhoneShell();
    HRESULT create(HANDLE wake, bool hidden);
    HRESULT self_test(const wchar_t* render_directory);
    void show();
    void shutdown();
    void video_size(UINT width,UINT height);
    void reset_video();
    void set_title(const std::wstring& title);
    void set_status(int kind,const std::wstring& message);
    std::wstring status_message() const;
    // The independent software status card exclusively owns failure/recovery
    // text. Idle chrome underneath must not duplicate it or expose its icon.
    bool idle_artwork_visible() const {return !has_video.load()&&status.load()<3;}
    float gain() const {return muted.load()?0.0f:volume.load();}
    float dpi_scale() const;
    static int rim_pixels(float scale){return std::max(2,int(3*scale+.5f));}
    static float corner_radius(UINT width,UINT height,float scale);
    static constexpr UINT WM_SHUTDOWN=WM_APP+1, WM_FORMAT=WM_APP+2, WM_REFRESH=WM_APP+3, WM_SHOW=WM_APP+4, WM_TEST=WM_APP+5,
        WM_SOURCE_FORMAT=WM_APP+6, WM_SOURCE_RESET=WM_APP+7, WM_ACCESSIBILITY=WM_APP+8;
private:
    friend class PhoneAccessibility;
    HWND palette_=nullptr,tooltip_=nullptr,status_panel_=nullptr;
    HICON small_icon_=nullptr,large_icon_=nullptr;
    HANDLE wake_=nullptr;
    std::thread thread_;
    bool hidden_=false, popup_=false, tracking_=false, dragging_slider_=false, user_sized_=false,hover_pending_=false,hide_volume_pending_=false;
    bool controls_collapsed_=false,dock_right_=true;
    ULONGLONG hide_volume_deadline_=0;
    int hot_=-1, pressed_=-1, keyboard_focus_=-1, rail_x_=0, flyout_x_=0, palette_width_=96,arrow_x_=84;
    float aspect_=498.0f/1080.0f, last_nonzero_=1.0f;
    float session_shape_=0; // first presented format's orientation-independent aspect
    UINT_PTR source_generation_=1; // media-thread only
    UINT_PTR ui_generation_=0; // UI-thread only; ignores delayed previous-session formats
    UINT display_w_=0,display_h_=0; // UI-thread source pixels
    UINT published_w_=0,published_h_=0; // media-thread only
    RECT restore_{},size_start_{};
    ULONG_PTR gdiplus_=0;
    std::unique_ptr<ControlPainter> painter_;
    std::unique_ptr<PaletteSurface> palette_surface_;
    std::unique_ptr<PaletteSurface> status_surface_;
    std::wstring painted_status_;
    int painted_status_kind_=-1;
    std::unique_ptr<PhoneAccessibility> accessibility_;
    mutable std::mutex status_mutex_;
    std::wstring status_reason_;
    UINT_PTR error_generation_=0;
    HRESULT initial_paint_=E_PENDING;
    void default_size(bool center=false);
    RECT fitted_rect(RECT original) const;
    HRESULT test_on_ui_thread(const wchar_t* directory);
    void shape_window();
    void update_icons();
    void place_palette();
    void paint_status_panel();
    bool dismiss_error();
    LRESULT accessibility_request(AccessibilityRequest& request);
    void accessibility_changed();
    HRESULT paint_palette(const wchar_t* diagnostic_path=nullptr,float diagnostic_scale=0);
    void toggle_fullscreen();
    void toggle_mute();
    void set_volume(float level);
    void invoke(int button);
    void pointer_volume(int x);
    bool volume_zone(POINT point) const;
    bool slider_hit(POINT point) const;
    void dismiss_volume();
    void update_volume_timer(bool in_zone);
    void toggle_controls();
    void update_tooltips();
    int button_at(POINT point) const;
    void signal();
    void menu(POINT screen);
    static LRESULT CALLBACK main_proc(HWND,UINT,WPARAM,LPARAM);
    static LRESULT CALLBACK palette_proc(HWND,UINT,WPARAM,LPARAM);
    static LRESULT CALLBACK status_proc(HWND,UINT,WPARAM,LPARAM);
};
