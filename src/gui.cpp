// DualSenseMusic - GUI front end for the DualSense Bluetooth audio player.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cwchar>
#include <cmath>
#include <string>
#include <vector>

#include "player.h"

namespace {

constexpr wchar_t kWndClass[] = L"DualSenseMusicWindow";
constexpr UINT_PTR kTickTimer = 1;

const COLORREF kBg          = RGB(14, 16, 20);
const COLORREF kCard        = RGB(23, 26, 32);
const COLORREF kCardBorder  = RGB(38, 43, 52);
const COLORREF kText        = RGB(234, 238, 244);
const COLORREF kMuted       = RGB(138, 148, 164);
const COLORREF kAccent      = RGB(79, 140, 255);
const COLORREF kAccentHover = RGB(110, 162, 255);
const COLORREF kTrack       = RGB(41, 47, 57);
const COLORREF kTrackHover  = RGB(52, 59, 71);
const COLORREF kGreen       = RGB(64, 200, 120);
const COLORREF kRed         = RGB(232, 96, 96);

enum class State { Idle, Playing, Paused };
enum class Control { None, Play, Stop, Test, Loop, Open, Source, SourceMenu, Progress, Slider0, Slider1, Slider2, Slider3, Pill };

struct Layout {
    RECT card;
    RECT progress;
    RECT play;
    RECT stop;
    RECT test;
    RECT loop;
    RECT source;
    RECT source_menu;
    RECT pill;
    RECT slider[4];
    RECT slider_track[4];
    RECT status;
    int  track_name_y = 0;
};

std::wstring ToWide(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0);
    std::wstring out(static_cast<size_t>(size), 0);
    if (size > 0) {
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(),
                            size);
    }
    return out;
}

std::wstring FormatTime(double seconds) {
    if (!(seconds >= 0.0) || !std::isfinite(seconds)) {
        seconds = 0.0;
    }
    const int total = static_cast<int>(seconds + 0.5);
    wchar_t   text[32];
    std::swprintf(text, 32, L"%d:%02d", total / 60, total % 60);
    return text;
}

const wchar_t* SliderLabel(int index) {
    switch (index) {
    case 0: return L"Haptics intensity";
    case 1: return L"Haptic tone";
    case 2: return L"Speaker volume";
    default: return L"Speaker level";
    }
}

class App {
public:
    explicit App(HINSTANCE instance) : instance_(instance) {}
    ~App() { ReleaseGdi(); }

    bool Create();
    int  Run();
    void SetStartupFile(const std::wstring& path) { startup_file_ = path; }

private:
    static LRESULT CALLBACK Thunk(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT                 Handle(UINT message, WPARAM wparam, LPARAM lparam);

    int  S(int value) const { return MulDiv(value, dpi_, 96); }
    void BuildFonts();
    void ReleaseGdi();
    void BuildLayout(int width, int height);

    void Paint();
    void EnsureBuffer(int width, int height);

    void OnTimer();
    void RefreshDevices();
    void RefreshStatus();
    void OpenFileDialog();
    void LoadFile(const std::wstring& path, bool autoplay);
    void StartPlayback(bool test, double start);
    void Pause();
    void StopPlayback();
    void SeekTo(double position);

    Control HitTest(POINT point) const;
    void    SetSliderFromPoint(int index, int x);
    void    ApplySlider(int index);

    void DrawButton(HDC dc, RECT r, const wchar_t* label, bool primary, bool hover, bool down);
    void DrawSlider(HDC dc, int index);
    void DrawTextItem(HDC dc, const std::wstring& text, RECT r, HFONT font, COLORREF color,
                      UINT flags);
    void DrawRounded(HDC dc, RECT r, int radius, COLORREF fill, COLORREF border);

private:
    HINSTANCE instance_ = nullptr;
    HWND      hwnd_     = nullptr;
    int       dpi_      = 96;
    bool      running_  = true;

    HDC     mem_dc_     = nullptr;
    HBITMAP mem_bitmap_ = nullptr;
    int     mem_width_  = 0;
    int     mem_height_ = 0;
    HGDIOBJ old_bitmap_ = nullptr;

    HFONT font_title_ = nullptr;
    HFONT font_h2_    = nullptr;
    HFONT font_body_  = nullptr;
    HFONT font_small_ = nullptr;

    Layout layout_ {};

    ds::Player                player_;
    std::vector<HidDeviceInfo> pads_;
    std::wstring              file_;
    std::wstring              startup_file_;
    ds::Settings              settings_;
    State                     state_      = State::Idle;
    double                    pause_pos_  = 0.0;
    double                    duration_   = 0.0;
    bool                      test_mode_  = false;
    bool                      system_audio_ = false;
    int                       audio_device_ = -1;
    std::vector<ds::AudioOutputInfo> audio_outputs_;
    bool                      loop_       = false;
    bool                      dragging_slider_ = false;
    int                       drag_slider_     = -1;
    bool                      seeking_    = false;
    double                    seek_pos_   = 0.0;
    std::wstring              status_;
    std::wstring              controller_name_;

    Control hover_   = Control::None;
    Control pressed_ = Control::None;

    float slider_value_[4] = {0.5f, 0.9f, 1.0f, 0.6667f};
};

bool App::Create() {
    SetProcessDPIAware();
    HDC screen = GetDC(nullptr);
    dpi_       = screen != nullptr ? GetDeviceCaps(screen, LOGPIXELSX) : 96;
    if (screen != nullptr) {
        ReleaseDC(nullptr, screen);
    }

    WNDCLASSEXW wc {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = &App::Thunk;
    wc.hInstance     = instance_;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kWndClass;
    wc.hbrBackground = nullptr;
    if (RegisterClassExW(&wc) == 0) {
        return false;
    }

    hwnd_ = CreateWindowExW(0, kWndClass, L"DualSense Music", WS_OVERLAPPEDWINDOW,
                            CW_USEDEFAULT, CW_USEDEFAULT, S(840), S(620), nullptr, nullptr,
                            instance_, this);
    if (hwnd_ == nullptr) {
        return false;
    }

    // Dark title bar where the OS supports it.
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd_, 20, &dark, sizeof(dark));

    DragAcceptFiles(hwnd_, TRUE);
    BuildFonts();

    settings_.loop = false;
    slider_value_[0] = settings_.haptics_gain / 2.0f;
    slider_value_[1] = settings_.tone;
    slider_value_[2] = static_cast<float>(settings_.speaker_volume) / 100.0f;
    slider_value_[3] = settings_.speaker_gain / 2.0f;

    RefreshDevices();
    RefreshStatus();
    if (!startup_file_.empty()) {
        LoadFile(startup_file_, true);
    }

    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
    SetTimer(hwnd_, kTickTimer, 33, nullptr);
    return true;
}

int App::Run() {
    MSG message {};
    while (running_ && GetMessageW(&message, nullptr, 0, 0)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return 0;
}

void App::BuildFonts() {
    auto make = [&](int points, int weight) {
        return CreateFontW(-S(points), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                           OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           VARIABLE_PITCH | FF_SWISS, L"Segoe UI");
    };
    font_title_ = make(26, FW_SEMIBOLD);
    font_h2_    = make(15, FW_SEMIBOLD);
    font_body_  = make(12, FW_NORMAL);
    font_small_ = make(11, FW_NORMAL);
}

void App::ReleaseGdi() {
    if (mem_dc_ != nullptr) {
        if (old_bitmap_ != nullptr) {
            SelectObject(mem_dc_, old_bitmap_);
        }
        DeleteDC(mem_dc_);
        mem_dc_ = nullptr;
    }
    if (mem_bitmap_ != nullptr) {
        DeleteObject(mem_bitmap_);
        mem_bitmap_ = nullptr;
    }
    for (HFONT* font: {&font_title_, &font_h2_, &font_body_, &font_small_}) {
        if (*font != nullptr) {
            DeleteObject(*font);
            *font = nullptr;
        }
    }
}

void App::BuildLayout(int width, int height) {
    const int margin = S(26);
    const int card_h = S(150);

    layout_.card = {margin, S(94), width - margin, S(94) + card_h};
    layout_.progress = {layout_.card.left + S(18), layout_.card.bottom - S(46),
                        layout_.card.right - S(18), layout_.card.bottom - S(38)};
    layout_.track_name_y = layout_.card.top + S(18);

    int y = layout_.card.bottom + S(24);
    layout_.play  = {margin, y, margin + S(124), y + S(42)};
    layout_.stop  = {layout_.play.right + S(10), y, layout_.play.right + S(10) + S(92),
                     y + S(42)};
    layout_.test  = {layout_.stop.right + S(10), y, layout_.stop.right + S(10) + S(92), y + S(42)};
    layout_.loop  = {width - margin - S(96), y + S(12), width - margin, y + S(30)};
    layout_.source = {layout_.loop.left - S(180), y + S(12), layout_.loop.left - S(16),
                      y + S(30)};
    layout_.source_menu = {layout_.source.right - S(18), layout_.source.top + S(2),
                           layout_.source.right, layout_.source.bottom - S(2)};

    y += S(42) + S(26);
    for (int i = 0; i < 4; i++) {
        layout_.slider[i] = {margin, y, width - margin, y + S(40)};
        const int left    = margin + S(168);
        const int right   = width - margin - S(74);
        const int center  = y + S(20);
        layout_.slider_track[i] = {left, center - S(3), right, center + S(3)};
        y += S(46);
    }

    layout_.status = {margin, height - S(40), width - margin, height - S(14)};
    layout_.pill   = {width - margin - S(168), S(24), width - margin, S(24) + S(30)};
}

void App::EnsureBuffer(int width, int height) {
    if (mem_dc_ != nullptr && mem_width_ == width && mem_height_ == height) {
        return;
    }
    if (mem_dc_ != nullptr) {
        if (old_bitmap_ != nullptr) {
            SelectObject(mem_dc_, old_bitmap_);
            old_bitmap_ = nullptr;
        }
        DeleteDC(mem_dc_);
        mem_dc_ = nullptr;
    }
    if (mem_bitmap_ != nullptr) {
        DeleteObject(mem_bitmap_);
        mem_bitmap_ = nullptr;
    }
    HDC screen = GetDC(hwnd_);
    mem_dc_    = CreateCompatibleDC(screen);
    mem_bitmap_ = CreateCompatibleBitmap(screen, width, height);
    old_bitmap_ = SelectObject(mem_dc_, mem_bitmap_);
    ReleaseDC(hwnd_, screen);
    mem_width_  = width;
    mem_height_ = height;
}

void App::DrawTextItem(HDC dc, const std::wstring& text, RECT r, HFONT font, COLORREF color,
                       UINT flags) {
    SelectObject(dc, font);
    SetTextColor(dc, color);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, text.c_str(), -1, &r, flags | DT_NOPREFIX);
}

void App::DrawRounded(HDC dc, RECT r, int radius, COLORREF fill, COLORREF border) {
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN   pen   = CreatePen(PS_SOLID, 1, border == CLR_INVALID ? fill : border);
    HGDIOBJ old_brush = SelectObject(dc, brush);
    HGDIOBJ old_pen   = SelectObject(dc, pen);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius * 2, radius * 2);
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(brush);
    DeleteObject(pen);
}

void App::DrawButton(HDC dc, RECT r, const wchar_t* label, bool primary, bool hover, bool down) {
    COLORREF fill;
    COLORREF border;
    COLORREF text;
    if (primary) {
        fill   = hover ? kAccentHover : kAccent;
        border = fill;
        text   = RGB(255, 255, 255);
    } else {
        fill   = down ? kTrack : (hover ? kTrackHover : kCard);
        border = hover ? RGB(62, 70, 84) : kCardBorder;
        text   = kText;
    }
    if (down) {
        fill = primary ? RGB(62, 118, 222) : kTrack;
    }
    DrawRounded(dc, r, S(8), fill, border);
    DrawTextItem(dc, label, r, font_body_, text, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void App::DrawSlider(HDC dc, int index) {
    const RECT row   = layout_.slider[index];
    const RECT track = layout_.slider_track[index];
    const bool hover = hover_ == static_cast<Control>(
                                    static_cast<int>(Control::Slider0) + index);

    RECT label_rect = {row.left, row.top, track.left - S(10), row.bottom};
    DrawTextItem(dc, SliderLabel(index), label_rect, font_body_, kMuted,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    RECT track_full = {track.left, track.top, track.right, track.bottom};
    DrawRounded(dc, track_full, S(3), hover || dragging_slider_ ? kTrackHover : kTrack, CLR_INVALID);

    const float value = std::clamp(slider_value_[index], 0.0f, 1.0f);
    RECT filled = track_full;
    filled.right = track.left + static_cast<int>((track.right - track.left) * value);
    if (filled.right > filled.left + S(2)) {
        DrawRounded(dc, filled, S(3), kAccent, CLR_INVALID);
    }

    const int knob_x = track.left + static_cast<int>((track.right - track.left) * value);
    const int knob_r = S(7);
    RECT      knob  = {knob_x - knob_r, track.top - knob_r + S(3), knob_x + knob_r,
                       track.bottom + knob_r - S(3)};
    DrawRounded(dc, knob, knob_r, RGB(240, 244, 250), CLR_INVALID);

    wchar_t text[32];
    switch (index) {
    case 0:
        std::swprintf(text, 32, L"%.2fx", value * 2.0f);
        break;
    case 1:
        std::swprintf(text, 32, L"%d%%", static_cast<int>(std::lround(value * 100.0f)));
        break;
    case 2:
        std::swprintf(text, 32, L"%d", static_cast<int>(std::lround(value * 100.0f)));
        break;
    default:
        std::swprintf(text, 32, L"%.2fx", value * 2.0f);
        break;
    }
    RECT value_rect = {track.right + S(12), row.top, row.right, row.bottom};
    DrawTextItem(dc, text, value_rect, font_body_, kText,
                 DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
}

void App::Paint() {
    RECT client {};
    GetClientRect(hwnd_, &client);
    const int width  = client.right;
    const int height = client.bottom;
    if (width <= 0 || height <= 0) {
        return;
    }
    EnsureBuffer(width, height);
    const HDC dc = mem_dc_;

    HBRUSH bg = CreateSolidBrush(kBg);
    FillRect(dc, &client, bg);
    DeleteObject(bg);

    BuildLayout(width, height);

    // Header
    RECT title = {S(26), S(20), width - S(200), S(58)};
    DrawTextItem(dc, L"DualSense Music", title, font_title_, kText,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    RECT subtitle = {S(26), S(56), width - S(200), S(80)};
    std::wstring sub = controller_name_.empty()
                           ? L"No controller found - pair the DualSense over Bluetooth"
                           : controller_name_ + L"  \x00B7  Bluetooth";
    DrawTextItem(dc, sub, subtitle, font_small_, kMuted,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    // Connection pill
    const bool connected = !pads_.empty();
    DrawRounded(dc, layout_.pill, S(15), connected ? RGB(24, 40, 31) : RGB(44, 26, 28),
                connected ? RGB(38, 66, 50) : RGB(70, 38, 40));
    const int dot = S(4);
    const int cx  = layout_.pill.left + S(16);
    const int cy  = (layout_.pill.top + layout_.pill.bottom) / 2;
    HBRUSH dot_brush = CreateSolidBrush(connected ? kGreen : kRed);
    HPEN   dot_pen   = CreatePen(PS_SOLID, 1, connected ? kGreen : kRed);
    HGDIOBJ ob = SelectObject(dc, dot_brush);
    HGDIOBJ op = SelectObject(dc, dot_pen);
    Ellipse(dc, cx - dot, cy - dot, cx + dot, cy + dot);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(dot_brush);
    DeleteObject(dot_pen);

    RECT pill_text = layout_.pill;
    pill_text.left += S(28);
    DrawTextItem(dc, connected ? L"Connected" : L"No controller", pill_text, font_small_,
                 connected ? RGB(150, 220, 175) : RGB(235, 160, 160),
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // Track card
    DrawRounded(dc, layout_.card, S(12), kCard, kCardBorder);

    RECT name_rect = {layout_.card.left + S(18), layout_.track_name_y,
                      layout_.card.right - S(140), layout_.track_name_y + S(30)};
    std::wstring track_name;
    if (test_mode_) {
        track_name = L"Test loop";
    } else if (system_audio_) {
        track_name = L"System audio (your PC's output)";
    } else if (!file_.empty()) {
        const size_t slash = file_.find_last_of(L"\\/");
        track_name = slash == std::wstring::npos ? file_ : file_.substr(slash + 1);
    } else {
        track_name = L"Drag an audio file here";
    }
    DrawTextItem(dc, track_name, name_rect, font_h2_,
                 file_.empty() && !test_mode_ ? kMuted : kText,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    RECT open_rect = {layout_.card.right - S(126), layout_.track_name_y - S(4),
                      layout_.card.right - S(18), layout_.track_name_y + S(30)};
    DrawButton(dc, open_rect, L"Open file...", false, hover_ == Control::Open,
               pressed_ == Control::Open);

    // Progress
    const double total = duration_;
    double       position = pause_pos_;
    if (state_ == State::Playing) {
        position = seeking_ ? seek_pos_ : player_.Position();
    } else if (seeking_) {
        position = seek_pos_;
    }
    const float progress = total > 0.0 ? static_cast<float>(std::clamp(position / total, 0.0, 1.0))
                                       : 0.0f;

    RECT bar_bg = layout_.progress;
    DrawRounded(dc, bar_bg, S(4), kTrack, CLR_INVALID);
    RECT bar_fill = bar_bg;
    bar_fill.right = bar_bg.left + static_cast<int>((bar_bg.right - bar_bg.left) * progress);
    if (bar_fill.right > bar_fill.left + S(2)) {
        DrawRounded(dc, bar_fill, S(4), kAccent, CLR_INVALID);
    }
    if (total > 0.0) {
        const int knob_x = bar_bg.left + static_cast<int>((bar_bg.right - bar_bg.left) * progress);
        RECT      knob   = {knob_x - S(6), bar_bg.top - S(4), knob_x + S(6), bar_bg.bottom + S(4)};
        DrawRounded(dc, knob, S(6), RGB(240, 244, 250), CLR_INVALID);
    }

    RECT left_time  = {layout_.card.left + S(18), layout_.progress.bottom + S(4),
                       layout_.card.left + S(120), layout_.progress.bottom + S(24)};
    RECT right_time = {layout_.card.right - S(120), layout_.progress.bottom + S(4),
                       layout_.card.right - S(18), layout_.progress.bottom + S(24)};
    DrawTextItem(dc, FormatTime(position), left_time, font_body_, kMuted,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DrawTextItem(dc, total > 0.0 ? FormatTime(total) : L"--:--", right_time, font_body_, kMuted,
                 DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

    // Transport
    const bool playing = state_ == State::Playing;
    DrawButton(dc, layout_.play, playing ? L"Pause" : L"Play", true, hover_ == Control::Play,
               pressed_ == Control::Play);
    DrawButton(dc, layout_.stop, L"Stop", false, hover_ == Control::Stop, pressed_ == Control::Stop);
    DrawButton(dc, layout_.test, L"Test", false, hover_ == Control::Test, pressed_ == Control::Test);

    RECT source_box = {layout_.source.left, layout_.source.top + S(3),
                       layout_.source.left + S(14), layout_.source.top + S(17)};
    DrawRounded(dc, source_box, S(3), system_audio_ ? kAccent : kCard,
                system_audio_ ? kAccent : RGB(70, 78, 92));
    if (system_audio_) {
        HPEN pen = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
        HGDIOBJ op = SelectObject(dc, pen);
        MoveToEx(dc, source_box.left + S(3), source_box.top + S(7), nullptr);
        LineTo(dc, source_box.left + S(6), source_box.top + S(11));
        LineTo(dc, source_box.left + S(11), source_box.top + S(4));
        SelectObject(dc, op);
        DeleteObject(pen);
    }
    RECT source_text = {source_box.right + S(8), layout_.source.top,
                        layout_.source_menu.left - S(2), layout_.source.bottom};
    DrawTextItem(dc, L"System audio", source_text, font_body_, kText,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DrawTextItem(dc, L"\x25BE", layout_.source_menu, font_small_,
                 system_audio_ ? kText : kMuted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    RECT loop_box = {layout_.loop.left, layout_.loop.top + S(3), layout_.loop.left + S(14),
                     layout_.loop.top + S(17)};
    DrawRounded(dc, loop_box, S(3), loop_ ? kAccent : kCard,
                loop_ ? kAccent : RGB(70, 78, 92));
    if (loop_) {
        HPEN pen = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
        HGDIOBJ op = SelectObject(dc, pen);
        MoveToEx(dc, loop_box.left + S(3), loop_box.top + S(7), nullptr);
        LineTo(dc, loop_box.left + S(6), loop_box.top + S(11));
        LineTo(dc, loop_box.left + S(11), loop_box.top + S(4));
        SelectObject(dc, op);
        DeleteObject(pen);
    }
    RECT loop_text = {loop_box.right + S(8), layout_.loop.top, layout_.loop.right,
                      layout_.loop.bottom};
    DrawTextItem(dc, L"Loop", loop_text, font_body_, kText,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    for (int i = 0; i < 4; i++) {
        DrawSlider(dc, i);
    }

    DrawTextItem(dc, status_, layout_.status, font_small_, kMuted,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    HDC target = GetDC(hwnd_);
    BitBlt(target, 0, 0, width, height, mem_dc_, 0, 0, SRCCOPY);
    ReleaseDC(hwnd_, target);
}

Control App::HitTest(POINT p) const {
    if (PtInRect(&layout_.play, p)) return Control::Play;
    if (PtInRect(&layout_.stop, p)) return Control::Stop;
    if (PtInRect(&layout_.test, p)) return Control::Test;
    if (PtInRect(&layout_.loop, p)) return Control::Loop;
    if (PtInRect(&layout_.source_menu, p)) return Control::SourceMenu;
    if (PtInRect(&layout_.source, p)) return Control::Source;
    if (PtInRect(&layout_.pill, p)) return Control::Pill;

    RECT progress = layout_.progress;
    progress.top -= S(8);
    progress.bottom += S(8);
    if (PtInRect(&progress, p)) return Control::Progress;

    // The "Open file" button inside the card.
    RECT open_rect = {layout_.card.right - S(126), layout_.track_name_y - S(4),
                      layout_.card.right - S(18), layout_.track_name_y + S(30)};
    if (PtInRect(&open_rect, p)) return Control::Open;

    for (int i = 0; i < 4; i++) {
        RECT row = layout_.slider[i];
        row.top -= S(6);
        row.bottom += S(6);
        if (PtInRect(&row, p)) {
            return static_cast<Control>(static_cast<int>(Control::Slider0) + i);
        }
    }
    return Control::None;
}

void App::SetSliderFromPoint(int index, int x) {
    const RECT track = layout_.slider_track[index];
    const int  width = std::max(1, static_cast<int>(track.right - track.left));
    slider_value_[index] =
        std::clamp(static_cast<float>(x - track.left) / static_cast<float>(width), 0.0f, 1.0f);
    ApplySlider(index);
}

void App::ApplySlider(int index) {
    switch (index) {
    case 0:
        settings_.haptics_gain = slider_value_[0] * 2.0f;
        player_.SetHapticsGain(settings_.haptics_gain);
        break;
    case 1:
        settings_.tone = slider_value_[1];
        player_.SetTone(settings_.tone);
        break;
    case 2:
        settings_.speaker_volume = static_cast<int>(std::lround(slider_value_[2] * 100.0f));
        player_.SetSpeakerVolume(settings_.speaker_volume);
        break;
    default:
        settings_.speaker_gain = slider_value_[3] * 2.0f;
        player_.SetSpeakerGain(settings_.speaker_gain);
        break;
    }
}

void App::RefreshDevices() {
    pads_ = ds::FindDualSensePads();
    controller_name_.clear();
    if (!pads_.empty()) {
        controller_name_ = pads_.front().name.empty() ? L"Wireless Controller"
                                                      : ToWide(pads_.front().name);
    }
}

void App::RefreshStatus() {
    if (pads_.empty()) {
        status_ = L"No controller. Pair the DualSense over Bluetooth, then click the status pill.";
        return;
    }
    if (!pads_.front().bluetooth) {
        status_ = L"Controller is connected over USB. Unplug it and connect over Bluetooth.";
        return;
    }
    const std::string error = player_.Error();
    if (!error.empty()) {
        status_ = ToWide(error);
        return;
    }
    switch (state_) {
    case State::Playing:
        if (test_mode_) {
            status_ = L"Playing the test loop.";
        } else if (system_audio_) {
            status_ = settings_.loopback_device.empty()
                          ? L"Playing your PC's audio on the controller."
                          : L"Playing " + ToWide(settings_.loopback_device) +
                                L" on the controller.";
        } else {
            status_ = L"Playing on the controller speaker and haptics.";
        }
        break;
    case State::Paused:
        status_ = L"Paused.";
        break;
    default:
        status_ = system_audio_
                      ? L"System audio ready. Press Play, then start your game."
                      : L"Ready. Drag a file here or press Play.";
        break;
    }
}

void App::OpenFileDialog() {
    wchar_t file[MAX_PATH] = {};
    OPENFILENAMEW ofn {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = hwnd_;
    ofn.lpstrFilter = L"Audio files\0*.wav;*.mp3;*.flac;*.ogg\0All files\0*.*\0";
    ofn.lpstrFile   = file;
    ofn.nMaxFile    = MAX_PATH;
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    ofn.lpstrTitle  = L"Open audio file";
    if (GetOpenFileNameW(&ofn)) {
        LoadFile(file, true);
    }
}

void App::LoadFile(const std::wstring& path, bool autoplay) {
    file_      = path;
    test_mode_ = false;
    pause_pos_ = 0.0;
    duration_  = 0.0;
    state_     = State::Idle;
    if (autoplay) {
        StartPlayback(false, 0.0);
    } else {
        RefreshStatus();
    }
}

void App::StartPlayback(bool test, double start) {
    if (pads_.empty()) {
        MessageBoxW(hwnd_, L"No DualSense found.\n\nPair the controller over Bluetooth first.",
                    L"DualSense Music", MB_ICONINFORMATION);
        return;
    }
    if (!pads_.front().bluetooth) {
        MessageBoxW(hwnd_,
                    L"The controller is connected over USB.\n\nThis program uses the "
                    L"Bluetooth audio path, so unplug the cable and connect wirelessly.",
                    L"DualSense Music", MB_ICONINFORMATION);
        return;
    }
    if (!test && !system_audio_ && file_.empty()) {
        OpenFileDialog();
        return;
    }

    player_.Stop();
    settings_.test         = test;
    settings_.system_audio = system_audio_ && !test;
    // loopback_device is kept in settings_ by the menu
    settings_.loop     = loop_;
    settings_.start    = start;
    settings_.duration = 0.0;

    std::string error;
    if (!player_.Start(file_, pads_.front(), settings_, &error)) {
        status_ = ToWide(error);
        state_  = State::Idle;
        return;
    }
    test_mode_ = test;
    state_     = State::Playing;
    pause_pos_ = start;
    duration_  = player_.Duration();
    RefreshStatus();
}

void App::Pause() {
    if (state_ != State::Playing) {
        return;
    }
    pause_pos_ = player_.Position();
    player_.Stop();
    state_ = State::Paused;
    RefreshStatus();
}

void App::StopPlayback() {
    player_.Stop();
    state_     = State::Idle;
    pause_pos_ = 0.0;
    RefreshStatus();
}

void App::SeekTo(double position) {
    if (duration_ <= 0.0) {
        return;
    }
    position = std::clamp(position, 0.0, duration_ - 0.05);
    if (state_ == State::Playing) {
        StartPlayback(test_mode_, position);
    } else {
        pause_pos_ = position;
        RefreshStatus();
    }
}

void App::OnTimer() {
    if (state_ == State::Playing && !player_.Running()) {
        const std::string error = player_.Error();
        if (!error.empty()) {
            status_ = ToWide(error);
        }
        state_     = State::Idle;
        pause_pos_ = 0.0;
        if (error.empty()) {
            RefreshStatus();
        }
    }
    if (state_ == State::Playing) {
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

LRESULT App::Handle(UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd_, &ps);
        Paint();
        EndPaint(hwnd_, &ps);
        return 0;
    }

    case WM_SIZE:
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;

    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
        info->ptMinTrackSize.x = S(720);
        info->ptMinTrackSize.y = S(600);
        return 0;
    }

    case WM_COMMAND: {
        const int id = LOWORD(wparam);
        if (id == 1) {
            audio_device_ = -1;
            settings_.loopback_device.clear();
        } else if (id >= 100) {
            const size_t index = static_cast<size_t>(id - 100);
            if (index < audio_outputs_.size()) {
                audio_device_ = static_cast<int>(index);
                settings_.loopback_device = audio_outputs_[index].name;
            }
        } else {
            break;
        }
        if (state_ == State::Playing && system_audio_) {
            player_.Stop();
            StartPlayback(false, 0.0);
        }
        RefreshStatus();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }

    case WM_TIMER:
        if (wparam == kTickTimer) {
            OnTimer();
        }
        return 0;

    case WM_MOUSEMOVE: {
        POINT p {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        if (dragging_slider_) {
            SetSliderFromPoint(drag_slider_, p.x);
        } else if (seeking_) {
            const RECT bar = layout_.progress;
            const double ratio = std::clamp(
                static_cast<double>(p.x - bar.left) / std::max(1, static_cast<int>(bar.right - bar.left)),
                0.0, 1.0);
            seek_pos_ = ratio * duration_;
        } else {
            const Control hit = HitTest(p);
            if (hit != hover_) {
                hover_ = hit;
                TRACKMOUSEEVENT track {sizeof(track), TME_LEAVE, hwnd_, 0};
                TrackMouseEvent(&track);
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        hover_ = Control::None;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;

    case WM_SETCURSOR: {
        if (LOWORD(lparam) == HTCLIENT) {
            POINT p;
            GetCursorPos(&p);
            ScreenToClient(hwnd_, &p);
            const Control hit = HitTest(p);
            if (hit != Control::None) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            return TRUE;
        }
        break;
    }

    case WM_LBUTTONDOWN: {
        SetCapture(hwnd_);
        POINT p {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        const Control hit = HitTest(p);
        pressed_          = hit;

        if (hit == Control::Progress && duration_ > 0.0) {
            seeking_  = true;
            const RECT bar = layout_.progress;
            const double ratio = std::clamp(
                static_cast<double>(p.x - bar.left) / std::max(1, static_cast<int>(bar.right - bar.left)),
                0.0, 1.0);
            seek_pos_ = ratio * duration_;
        } else if (hit >= Control::Slider0 && hit <= Control::Slider3) {
            dragging_slider_ = true;
            drag_slider_     = static_cast<int>(hit) - static_cast<int>(Control::Slider0);
            SetSliderFromPoint(drag_slider_, p.x);
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }

    case WM_LBUTTONUP: {
        ReleaseCapture();
        POINT p {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        const Control hit = HitTest(p);

        if (dragging_slider_) {
            dragging_slider_ = false;
            drag_slider_     = -1;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        if (seeking_) {
            seeking_ = false;
            SeekTo(seek_pos_);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }

        if (hit == pressed_ && hit != Control::None) {
            switch (hit) {
            case Control::Play:
                if (state_ == State::Playing) {
                    Pause();
                } else {
                    StartPlayback(test_mode_, pause_pos_);
                }
                break;
            case Control::Open:
                OpenFileDialog();
                break;
            case Control::Stop:
                StopPlayback();
                break;
            case Control::Test:
                StartPlayback(true, 0.0);
                break;
            case Control::Loop:
                loop_ = !loop_;
                player_.SetLoop(loop_);
                settings_.loop = loop_;
                break;
            case Control::SourceMenu: {
                audio_outputs_ = ds::ListAudioOutputs();
                HMENU menu = CreatePopupMenu();
                AppendMenuW(menu, MF_STRING, 1, L"Default output device");
                AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                for (size_t i = 0; i < audio_outputs_.size(); i++) {
                    std::wstring label = ToWide(audio_outputs_[i].name);
                    AppendMenuW(menu, MF_STRING, 100 + static_cast<UINT>(i), label.c_str());
                }
                POINT origin {layout_.source_menu.left, layout_.source_menu.bottom};
                ClientToScreen(hwnd_, &origin);
                SetForegroundWindow(hwnd_);
                TrackPopupMenu(menu, TPM_LEFTALIGN | TPM_TOPALIGN, origin.x, origin.y, 0, hwnd_,
                               nullptr);
                DestroyMenu(menu);
                PostMessageW(hwnd_, WM_NULL, 0, 0);
                break;
            }
            case Control::Source:
                system_audio_ = !system_audio_;
                if (state_ == State::Playing) {
                    player_.Stop();
                    state_     = State::Idle;
                    pause_pos_ = 0.0;
                }
                RefreshStatus();
                break;
            case Control::Pill:
                RefreshDevices();
                RefreshStatus();
                break;
            default:
                break;
            }
        }
        pressed_ = Control::None;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }

    case WM_DROPFILES: {
        auto drop = reinterpret_cast<HDROP>(wparam);
        wchar_t path[MAX_PATH] = {};
        if (DragQueryFileW(drop, 0, path, MAX_PATH) > 0) {
            LoadFile(path, true);
        }
        DragFinish(drop);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }

    case WM_KEYDOWN:
        if (wparam == VK_SPACE) {
            if (state_ == State::Playing) {
                Pause();
            } else {
                StartPlayback(test_mode_, pause_pos_);
            }
            return 0;
        }
        if (wparam == VK_ESCAPE) {
            StopPlayback();
            return 0;
        }
        break;

    case WM_DESTROY:
        KillTimer(hwnd_, kTickTimer);
        player_.Stop();
        running_ = false;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd_, message, wparam, lparam);
}

LRESULT CALLBACK App::Thunk(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    App*  self = nullptr;
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        self         = static_cast<App*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        if (self != nullptr) {
            self->hwnd_ = hwnd;
        }
    } else {
        self = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    return self != nullptr ? self->Handle(message, wparam, lparam)
                           : DefWindowProcW(hwnd, message, wparam, lparam);
}

} // namespace

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int) {
    App app(instance);

    int       argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv != nullptr) {
        if (argc > 1) {
            app.SetStartupFile(argv[1]);
        }
        LocalFree(argv);
    }
    if (!app.Create()) {
        MessageBoxW(nullptr, L"Could not create the window.", L"DualSense Music", MB_ICONERROR);
        return 1;
    }
    return app.Run();
}
