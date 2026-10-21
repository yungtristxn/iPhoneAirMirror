#pragma once
#include <windows.h>
#include <atomic>
#include <thread>
#include <string>
#include <algorithm>
#include <memory>
class ControlPainter;

// UI-thread-only geometry/input plus atomic handoff to the media thread.
// The owned palette has no taskbar entry and never outlives the receiver.
class PhoneShell {
public:
    HWND window=nullptr;
    std::atomic<bool> quit{false}, disconnect{false}, redraw{true}, chrome_dirty{true};
    std::atomic<bool> fullscreen{false}, has_video{false}, muted{false};
    std::atomic<float> volume{1.0f};
    std::atomic<int> status{0}; // ready, connecting, streaming
    PhoneShell();
    ~PhoneShell();
    HRESULT create(HANDLE wake, bool hidden);
    HRESULT self_test(const wchar_t* render_directory);
    void show();
    void shutdown();
    void video_size(UINT width,UINT height);
    void reset_video();
    void set_status(const std::wstring& title);
    float gain() const {return muted.load()?0.0f:volume.load();}
    float dpi_scale() const;
    static int rim_pixels(float scale){return std::max(2,int(3*scale+.5f));}
    static float corner_radius(UINT width,UINT height,float scale);
    static constexpr UINT WM_SHUTDOWN=WM_APP+1, WM_FORMAT=WM_APP+2, WM_REFRESH=WM_APP+3, WM_SHOW=WM_APP+4, WM_TEST=WM_APP+5;
private:
    HWND palette_=nullptr,tooltip_=nullptr;
    HICON small_icon_=nullptr,large_icon_=nullptr;
    HANDLE wake_=nullptr;
    std::thread thread_;
    bool hidden_=false, popup_=false, tracking_=false, dragging_slider_=false, user_sized_=false,hover_pending_=false;
    bool controls_collapsed_=false,dock_right_=true;
    int hot_=-1, pressed_=-1, keyboard_focus_=-1, rail_x_=0, flyout_x_=0, palette_width_=96,arrow_x_=84;
    float aspect_=498.0f/1080.0f, last_nonzero_=1.0f;
    UINT display_w_=0,display_h_=0; // UI-thread source pixels
    UINT published_w_=0,published_h_=0; // media-thread only
    RECT restore_{},size_start_{};
    ULONG_PTR gdiplus_=0;
    std::unique_ptr<ControlPainter> painter_;
    HRESULT initial_paint_=E_PENDING;
    void default_size(bool center=false);
    RECT fitted_rect(RECT original) const;
    HRESULT test_on_ui_thread(const wchar_t* directory);
    void shape_window();
    void update_icons();
    void place_palette();
    HRESULT paint_palette(const wchar_t* diagnostic_path=nullptr,float diagnostic_scale=0);
    void toggle_fullscreen();
    void toggle_mute();
    void set_volume(float level);
    void invoke(int button);
    void pointer_volume(int x);
    bool volume_zone(POINT point) const;
    bool slider_hit(POINT point) const;
    void dismiss_volume();
    void toggle_controls();
    void update_tooltips();
    int button_at(POINT point) const;
    void signal();
    void menu(POINT screen);
    static LRESULT CALLBACK main_proc(HWND,UINT,WPARAM,LPARAM);
    static LRESULT CALLBACK palette_proc(HWND,UINT,WPARAM,LPARAM);
};
