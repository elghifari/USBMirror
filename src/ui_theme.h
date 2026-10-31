// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Paint native controls without replacing their keyboard or accessibility behavior.
namespace usbmirror::ui {
constexpr COLORREF background = RGB(14, 18, 25);
constexpr COLORREF surface = background;
constexpr COLORREF field = RGB(15, 21, 30);
constexpr COLORREF border = RGB(44, 55, 71);
constexpr COLORREF text = RGB(231, 237, 245);
constexpr COLORREF muted = RGB(151, 165, 184);
constexpr COLORREF disabled = RGB(105, 118, 136);
constexpr COLORREF accent = RGB(88, 221, 192);
constexpr COLORREF ink = RGB(10, 36, 32);

inline int px(HWND window, int value) { return MulDiv(value, static_cast<int>(GetDpiForWindow(window)), 96); }
inline void box(HDC dc, RECT rectangle, COLORREF fill, COLORREF stroke) {
    const auto brush = CreateSolidBrush(fill); const auto pen = CreatePen(PS_SOLID, 1, stroke);
    const auto old_brush = SelectObject(dc, brush); const auto old_pen = SelectObject(dc, pen);
    Rectangle(dc, rectangle.left, rectangle.top, rectangle.right, rectangle.bottom);
    SelectObject(dc, old_pen); SelectObject(dc, old_brush); DeleteObject(pen); DeleteObject(brush);
}
inline void fill(HDC dc, RECT rectangle, COLORREF color) {
    const auto brush = CreateSolidBrush(color); FillRect(dc, &rectangle, brush); DeleteObject(brush);
}
inline void line(HDC dc, int x1, int y1, int x2, int y2, COLORREF color, int width = 1) {
    const auto pen = CreatePen(PS_SOLID, width, color); const auto previous = SelectObject(dc, pen);
    MoveToEx(dc, x1, y1, nullptr); LineTo(dc, x2, y2); SelectObject(dc, previous); DeleteObject(pen);
}
inline void label(HDC dc, const wchar_t* value, RECT rectangle, HFONT font, COLORREF color,
    UINT flags = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX) {
    const auto previous = SelectObject(dc, font); SetBkMode(dc, TRANSPARENT); SetTextColor(dc, color);
    DrawTextW(dc, value, -1, &rectangle, flags); SelectObject(dc, previous);
}
inline bool hovered(HWND window) { return GetPropW(window, L"USBMirror.Hover") != nullptr; }
inline std::wstring combo_text(HWND window, WPARAM index) {
    const auto length = SendMessageW(window, CB_GETLBTEXTLEN, index, 0);
    if (length < 0 || length > 4096) return {};
    std::wstring value(static_cast<std::size_t>(length) + 1, L'\0');
    SendMessageW(window, CB_GETLBTEXT, index, reinterpret_cast<LPARAM>(value.data()));
    value.resize(static_cast<std::size_t>(length)); return value;
}
inline void focus(HDC dc, RECT rectangle, HWND window) {
    if (GetFocus() == window && !(SendMessageW(window, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS)) {
        InflateRect(&rectangle, -px(window, 4), -px(window, 4));
        SetTextColor(dc, text); DrawFocusRect(dc, &rectangle);
    }
}
inline void button(const DRAWITEMSTRUCT& item) {
    const auto window = item.hwndItem; const auto dc = item.hDC; const auto rectangle = item.rcItem;
    const bool enabled = IsWindowEnabled(window) != FALSE;
    const bool primary = GetDlgCtrlID(window) == 102;
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const bool hover = hovered(window);
    COLORREF color = !enabled ? surface : primary ? (pressed ? RGB(57, 175, 151) : hover ? RGB(120, 235, 210) : accent) :
        pressed ? RGB(41, 55, 72) : hover ? RGB(43, 55, 72) : RGB(32, 41, 55);
    fill(dc, rectangle, surface);
    box(dc, rectangle, color, !enabled ? border : primary ? color : hover ? muted : border);
    wchar_t value[160]{}; GetWindowTextW(window, value, 160);
    auto text_rectangle = rectangle; InflateRect(&text_rectangle, -px(window, 8), 0);
    label(dc, value, text_rectangle, reinterpret_cast<HFONT>(SendMessageW(window, WM_GETFONT, 0, 0)),
        !enabled ? disabled : primary ? ink : text, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
        ((SendMessageW(window, WM_QUERYUISTATE, 0, 0) & UISF_HIDEACCEL) ? DT_HIDEPREFIX : 0));
    focus(dc, rectangle, window);
}
inline void checkbox(HWND window, HDC dc) {
    RECT rectangle{}; GetClientRect(window, &rectangle); fill(dc, rectangle, surface);
    const bool enabled = IsWindowEnabled(window) != FALSE;
    const bool checked = SendMessageW(window, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const auto size = px(window, 16); const auto top = (rectangle.bottom - size) / 2;
    RECT square{1, top, size + 1, top + size};
    box(dc, square, checked && enabled ? accent : field, !enabled ? border : checked ? accent : hovered(window) ? muted : border);
    if (checked) {
        const auto color = enabled ? ink : disabled;
        line(dc, px(window, 4), top + px(window, 8), px(window, 7), top + px(window, 11), color, px(window, 2));
        line(dc, px(window, 7), top + px(window, 11), px(window, 13), top + px(window, 5), color, px(window, 2));
    }
    rectangle.left = px(window, 26);
    wchar_t value[180]{}; GetWindowTextW(window, value, 180);
    label(dc, value, rectangle, reinterpret_cast<HFONT>(SendMessageW(window, WM_GETFONT, 0, 0)), enabled ? text : disabled,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    focus(dc, rectangle, window);
}
inline void combo(HWND window, HDC dc) {
    RECT rectangle{}; GetClientRect(window, &rectangle); fill(dc, rectangle, surface);
    const bool enabled = IsWindowEnabled(window) != FALSE;
    box(dc, rectangle, field, !enabled ? border : GetFocus() == window ? accent : hovered(window) ? muted : border);
    const auto selected = SendMessageW(window, CB_GETCURSEL, 0, 0);
    const auto value = selected >= 0 ? combo_text(window, selected) : std::wstring{};
    auto text_rectangle = rectangle; text_rectangle.left += px(window, 12); text_rectangle.right -= px(window, 30);
    label(dc, value.c_str(), text_rectangle, reinterpret_cast<HFONT>(SendMessageW(window, WM_GETFONT, 0, 0)), enabled ? text : disabled);
    const auto x = rectangle.right - px(window, 18); const auto y = rectangle.bottom / 2;
    line(dc, x - px(window, 4), y - px(window, 2), x, y + px(window, 2), enabled ? muted : disabled);
    line(dc, x, y + px(window, 2), x + px(window, 4), y - px(window, 2), enabled ? muted : disabled);
}
inline void combo_item(const DRAWITEMSTRUCT& item) {
    const bool selected = (item.itemState & ODS_SELECTED) != 0;
    fill(item.hDC, item.rcItem, selected ? RGB(34, 66, 64) : field);
    if (item.itemID == static_cast<UINT>(-1)) return;
    const auto value = combo_text(item.hwndItem, item.itemID);
    auto rectangle = item.rcItem; rectangle.left += px(item.hwndItem, 12); rectangle.right -= px(item.hwndItem, 6);
    label(item.hDC, value.c_str(), rectangle, reinterpret_cast<HFONT>(SendMessageW(item.hwndItem, WM_GETFONT, 0, 0)),
        IsWindowEnabled(item.hwndItem) ? text : disabled);
}
inline void slider(HWND window, HDC dc) {
    RECT rectangle{}; GetClientRect(window, &rectangle); fill(dc, rectangle, surface);
    RECT channel{}, thumb{}; SendMessageW(window, TBM_GETCHANNELRECT, 0, reinterpret_cast<LPARAM>(&channel));
    SendMessageW(window, TBM_GETTHUMBRECT, 0, reinterpret_cast<LPARAM>(&thumb));
    const auto y = rectangle.bottom / 2; const auto x = (thumb.left + thumb.right) / 2;
    RECT rail{channel.left, y - px(window, 2), channel.right, y + px(window, 2)};
    box(dc, rail, border, border); rail.right = x;
    if (rail.right > rail.left) box(dc, rail, accent, accent);
    const auto radius = px(window, 6); const auto brush = CreateSolidBrush(IsWindowEnabled(window) ? text : disabled);
    const auto old_brush = SelectObject(dc, brush); const auto old_pen = SelectObject(dc, GetStockObject(NULL_PEN));
    Ellipse(dc, x - radius, y - radius, x + radius, y + radius);
    SelectObject(dc, old_pen); SelectObject(dc, old_brush); DeleteObject(brush); focus(dc, rectangle, window);
}
inline LRESULT CALLBACK control_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR kind) {
    if (message == WM_MOUSEMOVE && !hovered(window)) {
        SetPropW(window, L"USBMirror.Hover", reinterpret_cast<HANDLE>(1));
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window, 0}; TrackMouseEvent(&track); InvalidateRect(window, nullptr, FALSE);
    }
    if (message == WM_MOUSELEAVE) { RemovePropW(window, L"USBMirror.Hover"); InvalidateRect(window, nullptr, FALSE); }
    if ((message == WM_PAINT || message == WM_PRINTCLIENT) && kind) {
        PAINTSTRUCT paint{}; const auto dc = message == WM_PAINT ? BeginPaint(window, &paint) : reinterpret_cast<HDC>(wparam);
        if (kind == 1) checkbox(window, dc); else if (kind == 2) combo(window, dc); else slider(window, dc);
        if (message == WM_PAINT) EndPaint(window, &paint); return 0;
    }
    if (message == WM_ERASEBKGND && kind) return 1;
    const auto result = DefSubclassProc(window, message, wparam, lparam);
    if (message == WM_SETFOCUS || message == WM_KILLFOCUS || message == WM_ENABLE || message == BM_SETCHECK ||
        message == WM_UPDATEUISTATE || message == CB_SETCURSEL || message == TBM_SETPOS) InvalidateRect(window, nullptr, FALSE);
    if (message == WM_NCDESTROY) RemovePropW(window, L"USBMirror.Hover");
    return result;
}
inline void style(HWND window, DWORD_PTR kind = 0) { SetWindowSubclass(window, control_proc, 1, kind); }
}
