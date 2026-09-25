#include "engine.hpp"
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace {
constexpr wchar_t defaultStation[] = L"https://0nlineradio.radioho.st/technolovers-trance";
constexpr COLORREF background = RGB(16, 21, 29), panel = RGB(26, 33, 44), muted = RGB(151, 168, 189),
    foreground = RGB(236, 243, 249), accent = RGB(95, 226, 183);
enum { Start = 101, Evolve, Reject, Radio, Folder, Url, Volume, Playful, Split, Reverb, Monitor };
std::unique_ptr<Engine> engine;
HWND urlBox, startButton, evolveButton, rejectButton, radioButton, folderButton, slider, playfulButton, splitButton, reverbButton, monitorButton;
HFONT bodyFont, titleFont, numberFont, smallFont;
HBRUSH editBrush;
float scale = 1;
bool active = false;
bool arrangementPreview = false;
std::wstring wide(const std::string& value) { return std::wstring(value.begin(), value.end()); }
int px(int x) { return int(x * scale); }
HFONT font(int size, int weight = FW_NORMAL) {
    return CreateFontW(-px(size), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
}
void text(HDC dc, const std::wstring& value, int x, int y, int w, int h, HFONT f, COLORREF color,
          UINT flags = DT_LEFT | DT_WORDBREAK | DT_NOPREFIX) {
    RECT rect{px(x), px(y), px(x + w), px(y + h)};
    SelectObject(dc, f); SetTextColor(dc, color); SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, value.c_str(), -1, &rect, flags);
}
void box(HDC dc, int x, int y, int w, int h, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color); auto old = SelectObject(dc, brush);
    auto pen = SelectObject(dc, GetStockObject(NULL_PEN));
    RoundRect(dc, px(x), px(y), px(x + w), px(y + h), px(14), px(14));
    SelectObject(dc, old); SelectObject(dc, pen); DeleteObject(brush);
}
COLORREF sourceColor(const std::string& id) {
    double h = (music::sourceColorId(id) % 360) / 60.0;
    double c = 0.48, x = c * (1 - std::abs(std::fmod(h, 2.0) - 1)), m = 0.40;
    double r = 0, g = 0, b = 0;
    if (h < 1) { r = c; g = x; } else if (h < 2) { r = x; g = c; }
    else if (h < 3) { g = c; b = x; } else if (h < 4) { g = x; b = c; }
    else if (h < 5) { r = x; b = c; } else { r = c; b = x; }
    return RGB(int((r + m) * 255), int((g + m) * 255), int((b + m) * 255));
}
void drawBars(HDC dc, const std::array<music::BarInfo, 8>& bars, int y, int width, int playingBeat, bool showCursor) {
    int bw = (width - 77) / 8;
    for (int i = 0; i < 8; ++i) {
        int x = 28 + i * (bw + 3);
        const auto& bar = bars[i]; bool exists = !bar.clipId.empty();
        auto color = exists ? sourceColor(bar.captureId) : panel;
        box(dc, x, y, bw, 82, panel);
        if (!exists) { text(dc, std::to_wstring(i + 1), x + 8, y + 17, bw - 16, 24, bodyFont, muted); continue; }
        box(dc, x, y, bw, 6, color);
        auto label = (bar.split ? L"H " : L"") + wide(music::sourceLabel(bar.captureId)) + L"/" + std::to_wstring(bar.sourceBar);
        text(dc, label, x + 6, y + 10, bw - 10, 20, smallFont, color, DT_SINGLELINE | DT_NOPREFIX);
        int beatWidth = (bw - 12) / 4;
        for (int beat = 0; beat < 4; ++beat) {
            bool selected = showCursor && playingBeat == i * 4 + beat;
            box(dc, x + 6 + beat * beatWidth, y + 35, beatWidth - 2, 23, selected ? foreground : RGB(42, 51, 64));
            text(dc, std::to_wstring(bar.beats[beat] + 1), x + 6 + beat * beatWidth, y + 36, beatWidth - 2, 20,
                 smallFont, selected ? background : color, DT_CENTER | DT_SINGLELINE);
        }
        if (bar.changed) box(dc, x + bw - 10, y + 7, 5, 5, RGB(255, 196, 105));
        if (bar.split) {
            auto lowColor = sourceColor(bar.lowCaptureId);
            text(dc, L"L " + wide(music::sourceLabel(bar.lowCaptureId)) + L"/" + std::to_wstring(bar.lowSourceBar),
                 x + 6, y + 59, bw - 10, 18, smallFont, lowColor, DT_SINGLELINE | DT_NOPREFIX);
            box(dc, x, y + 79, bw, 3, lowColor);
        }
    }
}
HWND button(HWND parent, const wchar_t* label, int id) {
    auto hwnd = CreateWindowW(L"BUTTON", label, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                              0, 0, 0, 0, parent, reinterpret_cast<HMENU>(INT_PTR(id)), nullptr, nullptr);
    SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(bodyFont), TRUE); return hwnd;
}
void layout(HWND hwnd) {
    RECT rc; GetClientRect(hwnd, &rc); int w = int(rc.right / scale);
    MoveWindow(urlBox, px(28), px(136), px(w - 240), px(36), TRUE);
    MoveWindow(startButton, px(w - 196), px(134), px(168), px(40), TRUE);
    MoveWindow(evolveButton, px(28), px(576), px(130), px(40), TRUE);
    MoveWindow(rejectButton, px(170), px(576), px(145), px(40), TRUE);
    MoveWindow(radioButton, px(327), px(576), px(155), px(40), TRUE);
    MoveWindow(splitButton, px(494), px(576), px(165), px(40), TRUE);
    MoveWindow(monitorButton, px(671), px(576), px(185), px(40), TRUE);
    MoveWindow(playfulButton, px(28), px(628), px(208), px(36), TRUE);
    MoveWindow(folderButton, px(w - 190), px(628), px(162), px(36), TRUE);
    MoveWindow(reverbButton, px(244), px(628), px(150), px(36), TRUE);
    MoveWindow(slider, px(480), px(631), px(140), px(30), TRUE);
}
void refreshButtons() {
    EnableWindow(urlBox, !active);
    SetWindowTextW(startButton, active ? L"Stop" : L"Start listening");
    EnableWindow(evolveButton, active); EnableWindow(rejectButton, active); EnableWindow(radioButton, active);
    SetWindowTextW(radioButton, engine->radio() ? L"Return to remix" : L"Hear live radio");
    SetWindowTextW(playfulButton, engine->playful() ? L"Rearrangement: on" : L"Rearrangement: off");
    SetWindowTextW(splitButton, engine->split() ? L"220 Hz split: on" : L"220 Hz split: off");
    SetWindowTextW(monitorButton, engine->monitor() == 1 ? L"Hear: upper only" : engine->monitor() == 2 ? L"Hear: bass only" : L"Hear: both");
    SetWindowTextW(reverbButton, engine->reverb() ? L"Reverb: on" : L"Reverb: off");
}
void paint(HWND hwnd, HDC target) {
    RECT rc; GetClientRect(hwnd, &rc);
    auto dc = CreateCompatibleDC(target); auto bitmap = CreateCompatibleBitmap(target, rc.right, rc.bottom);
    auto oldBitmap = SelectObject(dc, bitmap);
    HBRUSH brush = CreateSolidBrush(background); FillRect(dc, &rc, brush); DeleteObject(brush);
    int w = int(rc.right / scale);
    auto s = engine->snapshot();
    if (arrangementPreview) {
        s.mixing = true; s.transitioning = true; s.bpm = 138; s.key = 9 + 12; s.beat = 18; s.fade = 0.45;
        for (int i = 0; i < 8; ++i) {
            s.bars[i].captureId = "example-capture-A"; s.bars[i].clipId = "preview"; s.bars[i].sourceBar = i + 1;
            s.bars[i].split = true; s.bars[i].lowCaptureId = "example-bass-C"; s.bars[i].lowClipId = "preview-bass";
            s.bars[i].lowSourceBar = i % 2 + 9; s.bars[i].beats = {0, 1, 0, 1}; s.bars[i].changed = true;
            s.incomingBars[i] = s.bars[i];
            if (i >= 4) { s.incomingBars[i].captureId = "example-capture-B"; s.incomingBars[i].sourceBar = 17 + (i - 4) % 2; s.incomingBars[i].changed = true; }
        }
        s.incomingBars[7].beats = {0, 1, 0, 1}; s.activity = L"Illustration of a transition: source bars and beat repeats are shown below.";
    }
    text(dc, L"RADIOMUSIC", 28, 22, 300, 40, titleFont, foreground);
    text(dc, L"A radio stream that slowly becomes something of its own.", 28, 67, w - 56, 26, bodyFont, muted);
    text(dc, L"SOURCE  /  DIRECT MP3 STREAM", 28, 110, w - 56, 20, smallFont, muted);
    int cardWidth = (w - 80) / 3;
    for (int i = 0; i < 3; ++i) box(dc, 28 + i * (cardWidth + 12), 198, cardWidth, 112, panel);
    std::wostringstream bpm; if (s.bpm > 0) bpm << std::fixed << std::setprecision(1) << s.bpm; else bpm << L"—";
    text(dc, L"MIX TEMPO", 46, 213, cardWidth - 30, 20, smallFont, muted);
    text(dc, bpm.str(), 46, 238, cardWidth - 30, 50, numberFont, accent);
    int x = 46 + cardWidth + 12;
    text(dc, L"USED / SAVED LOOPS", x, 213, cardWidth - 30, 20, smallFont, muted);
    text(dc, std::to_wstring(s.usedLoops) + L" / " + std::to_wstring(s.librarySize), x, 238, cardWidth - 30, 50, numberFont, foreground);
    x += cardWidth + 12;
    text(dc, L"NEXT CAPTURE", x, 213, cardWidth - 30, 20, smallFont, muted);
    text(dc, active ? std::to_wstring(int(s.captureSeconds)) + L" / 48 s" : L"Paused", x, 238, cardWidth - 30, 50, numberFont, foreground);
    std::wstring mode = !active ? L"READY WHEN YOU ARE" : engine->radio() ? L"MONITORING LIVE RADIO" :
        s.transitioning ? L"CROSSFADING OVER ONE PASS" : s.mixing ? L"SEQUENCE PASS " + std::to_wstring(std::min(2, s.passes + 1)) + L" OF 2" : L"RADIO NOW · GATHERING THE FIRST LOOPS";
    text(dc, mode, 28, 323, w - 320, 25, smallFont, accent);
    text(dc, L"Estimated key: " + wide(music::keyName(s.key)), w - 285, 323, 257, 25, smallFont, muted);
    bool cursor = (active || arrangementPreview) && s.mixing && !engine->radio();
    drawBars(dc, s.bars, 348, w, int(s.beat), cursor);
    text(dc, L"H = upper frequencies · L = bass · color = capture · label = source/bar · digits = source beats", 28, 436, w - 56, 22, smallFont, muted);
    text(dc, s.transitioning ? L"INCOMING ARRANGEMENT  /  " + std::to_wstring(int(s.fade * 100)) + L"% blended" :
         s.waiting ? L"EVOLUTION QUEUED FOR THE NEXT PHRASE" : L"NEXT ARRANGEMENT APPEARS DURING A TRANSITION", 28, 464, w - 56, 22, smallFont, accent);
    drawBars(dc, s.incomingBars, 489, w, int(s.beat), cursor && s.transitioning);
    text(dc, s.activity, 28, 678, w - 56, 42, bodyFont, foreground);
    text(dc, L"Volume", 410, 635, 65, 25, smallFont, muted);
    text(dc, std::to_wstring(s.residentClips) + L" loaded", 628, 635, w - 820, 25, smallFont, muted);
    text(dc, s.connection, 28, 727, w - 56, 42, smallFont, muted);
    BitBlt(target, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBitmap); DeleteObject(bitmap); DeleteDC(dc);
}
LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    switch (message) {
    case WM_CREATE: {
        scale = GetDpiForWindow(hwnd) / 96.0f;
        bodyFont = font(16); titleFont = font(30, FW_SEMIBOLD); numberFont = font(34, FW_SEMIBOLD); smallFont = font(13);
        editBrush = CreateSolidBrush(panel);
        urlBox = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", defaultStation,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(Url), nullptr, nullptr);
        SendMessageW(urlBox, WM_SETFONT, reinterpret_cast<WPARAM>(bodyFont), TRUE);
        startButton = button(hwnd, L"Start listening", Start); evolveButton = button(hwnd, L"Evolve   >", Evolve);
        rejectButton = button(hwnd, L"Ban sources   −", Reject); radioButton = button(hwnd, L"Hear live radio", Radio);
        folderButton = button(hwnd, L"Open library", Folder);
        playfulButton = button(hwnd, L"Rearrangement: on", Playful);
        splitButton = button(hwnd, L"220 Hz split: on", Split);
        monitorButton = button(hwnd, L"Hear: both", Monitor);
        reverbButton = button(hwnd, L"Reverb: off", Reverb);
        slider = CreateWindowW(TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_HORZ | TBS_NOTICKS,
                               0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(Volume), nullptr, nullptr);
        SendMessageW(slider, TBM_SETRANGE, TRUE, MAKELONG(0, 100)); SendMessageW(slider, TBM_SETPOS, TRUE, 55);
        layout(hwnd); refreshButtons(); SetTimer(hwnd, 1, 150, nullptr); return 0;
    }
    case WM_SIZE: layout(hwnd); InvalidateRect(hwnd, nullptr, FALSE); return 0;
    case WM_GETMINMAXINFO: {
        auto info = reinterpret_cast<MINMAXINFO*>(lp); info->ptMinTrackSize = {px(900), px(810)}; return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_CTLCOLOREDIT:
        SetTextColor(reinterpret_cast<HDC>(wp), foreground); SetBkColor(reinterpret_cast<HDC>(wp), panel);
        return reinterpret_cast<LRESULT>(editBrush);
    case WM_PAINT: { PAINTSTRUCT p; HDC dc = BeginPaint(hwnd, &p); paint(hwnd, dc); EndPaint(hwnd, &p); return 0; }
    case WM_DRAWITEM: {
        auto d = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
        bool enabled = !(d->itemState & ODS_DISABLED), primary = d->CtlID == Start;
        COLORREF color = primary ? accent : panel;
        auto brush = CreateSolidBrush(color); FillRect(d->hDC, &d->rcItem, brush); DeleteObject(brush);
        wchar_t label[128]; GetWindowTextW(d->hwndItem, label, 128);
        SelectObject(d->hDC, bodyFont); SetBkMode(d->hDC, TRANSPARENT);
        SetTextColor(d->hDC, !enabled ? muted : primary ? background : foreground);
        DrawTextW(d->hDC, label, -1, &d->rcItem, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (d->itemState & ODS_FOCUS) { RECT r = d->rcItem; InflateRect(&r, -3, -3); DrawFocusRect(d->hDC, &r); }
        return TRUE;
    }
    case WM_HSCROLL: engine->setVolume(float(SendMessageW(slider, TBM_GETPOS, 0, 0)) / 100); return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case Start:
            if (active) { SetWindowTextW(startButton, L"Stopping..."); UpdateWindow(hwnd); engine->stop(); active = false; }
            else {
                wchar_t url[4096]; GetWindowTextW(urlBox, url, 4096);
                active = engine->start(url);
            }
            refreshButtons(); break;
        case Evolve: if (active) engine->evolve(); break;
        case Reject: if (active) engine->reject(); break;
        case Radio: engine->setRadio(!engine->radio()); refreshButtons(); break;
        case Playful: engine->setPlayful(!engine->playful()); refreshButtons(); break;
        case Split: engine->setSplit(!engine->split()); refreshButtons(); break;
        case Monitor: engine->setMonitor((engine->monitor() + 1) % 3); refreshButtons(); break;
        case Reverb: engine->setReverb(!engine->reverb()); refreshButtons(); break;
        case Folder: {
            std::error_code ec; std::filesystem::create_directories(engine->libraryPath(), ec);
            ShellExecuteW(hwnd, L"open", engine->libraryPath().c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
        }
        }
        InvalidateRect(hwnd, nullptr, FALSE); return 0;
    case WM_LBUTTONUP: {
        int x = int(short(LOWORD(lp)) / scale), y = int(short(HIWORD(lp)) / scale);
        RECT r; GetClientRect(hwnd, &r); int width = int(r.right / scale), bw = (width - 77) / 8;
        bool incoming = y >= 489 && y < 571;
        if (((y >= 348 && y < 430) || incoming) && x >= 28) {
            int index = (x - 28) / (bw + 3);
            if (index >= 0 && index < 8) {
                auto s = engine->snapshot(); auto bar = incoming ? s.incomingBars[index] : s.bars[index];
                if (!bar.clipId.empty()) {
                    auto details = L"Playing position: bar " + std::to_wstring(index + 1) + L"\nOriginal capture: " + wide(bar.captureId)
                        + L"\nSaved loop: " + wide(bar.clipId) + L"\nOriginal bar: " + std::to_wstring(bar.sourceBar)
                        + L"\nEstimated key: " + wide(music::keyName(bar.key)) + L"\nSource beats: ";
                    for (int beat : bar.beats) details += std::to_wstring(beat + 1) + L" ";
                    if (bar.split) {
                        details += L"\n\nBASS BELOW 220 HZ\nCapture: " + wide(bar.lowCaptureId) + L"\nSaved loop: " + wide(bar.lowClipId)
                            + L"\nOriginal bar: " + std::to_wstring(bar.lowSourceBar) + L"\nBeats: ";
                        for (int beat : bar.lowBeats) details += std::to_wstring(beat + 1) + L" ";
                    }
                    details += L"\n\nBar numbers refer to the estimated grid in the capture, not the original song's annotated bars.";
                    MessageBoxW(hwnd, details.c_str(), L"Where this bar comes from", MB_OK);
                }
            }
        }
        return 0;
    }
    case WM_TIMER:
        if (active && !engine->snapshot().running) { engine->stop(); active = false; refreshButtons(); }
        InvalidateRect(hwnd, nullptr, FALSE); return 0;
    case WM_CLOSE: DestroyWindow(hwnd); return 0;
    case WM_DESTROY:
        KillTimer(hwnd, 1); engine->stop();
        DeleteObject(bodyFont); DeleteObject(titleFont); DeleteObject(numberFont); DeleteObject(smallFont);
        DeleteObject(editBrush);
        PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, message, wp, lp);
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    int argc = 0; auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    auto root = std::filesystem::path(exe).parent_path();
    // The normal build lives in build/Release; data belongs beside the project launcher.
    if (root.filename() == L"Release" || root.filename() == L"Debug") root = root.parent_path();
    if (root.filename() == L"build") root = root.parent_path();
    bool offlineCheck = argc > 1 && std::wstring(argv[1]) == L"--offline-check";
    bool smoke = argc > 1 && (std::wstring(argv[1]) == L"--smoke" || offlineCheck);
    bool uiCheck = argc > 1 && std::wstring(argv[1]) == L"--ui-check";
    arrangementPreview = uiCheck && argc > 2 && std::wstring(argv[2]) == L"--arrangement";
    if (smoke) {
        int seconds = offlineCheck ? 20 : argc > 2 ? std::clamp(_wtoi(argv[2]), 2, 300) : 85;
        auto dir = root / "test-output"; std::filesystem::create_directories(dir);
        Engine test(dir / "library");
        std::ofstream log(dir / (offlineCheck ? "offline.log" : "smoke.log"));
        test.start(offlineCheck ? L"http://127.0.0.1:1/unavailable.mp3" : argc > 3 ? argv[3] : defaultStation, true);
        Snapshot s;
        bool sawArrangement = false, sawSplit = false;
        std::vector<std::string> rejected;
        for (int i = 0; i < seconds; ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            s = test.snapshot();
            if (i % 5 == 0) { log << i << "s decoded=" << s.decodedFrames << " loops=" << s.librarySize << " bpm=" << s.bpm << '\n'; log.flush(); }
            if (i == 5 || i == 65) test.evolve();
            for (const auto& bar : s.bars) sawArrangement = sawArrangement || bar.changed;
            for (const auto& bar : s.incomingBars) sawArrangement = sawArrangement || bar.changed;
            for (const auto& bar : s.bars) sawSplit = sawSplit || (bar.split && bar.clipId != bar.lowClipId);
            if (offlineCheck && i == 12) { rejected = s.audible; test.reject(); }
        }
        s = test.snapshot(); test.stop();
        if (!offlineCheck) test.savePreview(dir / "remix-preview.wav");
        std::ofstream report(dir / (offlineCheck ? "offline-report.txt" : "smoke-report.txt"));
        report << "Decoded frames: " << s.decodedFrames << "\nRendered frames: " << s.renderedFrames
               << "\nSaved loops: " << s.librarySize << "\nMix BPM: " << s.bpm << "\nPeak before safety clamp: " << s.peak
               << "\nLive buffer underruns: " << s.liveUnderruns << "\nEstimated mix key: " << music::keyName(s.key)
               << "\nRearrangement observed: " << sawArrangement << "\nSplit sources observed: " << sawSplit
               << "\nResident sources: " << s.residentClips << '\n';
        bool offlineGood = !rejected.empty() && s.librarySize > 0 && s.peak > 0;
        for (const auto& id : rejected) {
            bool removed = !std::filesystem::exists(dir / "library" / (id + ".wav")) &&
                !music::loadClip(dir / "library" / (id + ".wav"));
            report << "Rejected loop removed: " << id << " = " << removed << '\n';
            offlineGood = offlineGood && removed;
        }
        LocalFree(argv);
        if (offlineCheck) return offlineGood ? 0 : 1;
        return s.decodedFrames > 0 && s.librarySize > 0 && s.peak > 0 && s.peak < 1 ? 0 : 1;
    }
    LocalFree(argv);
    engine = std::make_unique<Engine>(root / "library");
    SetProcessDPIAware(); INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_BAR_CLASSES}; InitCommonControlsEx(&controls);
    WNDCLASSW wc{}; wc.hInstance = instance; wc.lpfnWndProc = windowProc; wc.lpszClassName = L"RadiomusicWindow";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    RegisterClassW(&wc);
    auto dc = GetDC(nullptr); scale = GetDeviceCaps(dc, LOGPIXELSX) / 96.0f; ReleaseDC(nullptr, dc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"Radiomusic — evolving radio loops", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                              CW_USEDEFAULT, CW_USEDEFAULT, px(960), px(830), nullptr, nullptr, instance, nullptr);
    if (!hwnd) return 1;
    if (uiCheck) {
        // Render our own window to an artifact without capturing the user's desktop.
        SetWindowPos(hwnd, nullptr, -32000, -32000, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        ShowWindow(hwnd, SW_SHOWNOACTIVATE); UpdateWindow(hwnd);
        RECT r; GetClientRect(hwnd, &r);
        BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = r.right; bi.bmiHeader.biHeight = -r.bottom;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr; HDC screen = GetDC(hwnd); HDC memory = CreateCompatibleDC(screen);
        auto bitmap = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        auto previous = SelectObject(memory, bitmap);
        paint(hwnd, memory);
        for (HWND child : {urlBox, startButton, evolveButton, rejectButton, radioButton, folderButton, slider, playfulButton, splitButton, reverbButton, monitorButton}) {
            RECT cr; GetWindowRect(child, &cr); MapWindowPoints(nullptr, hwnd, reinterpret_cast<POINT*>(&cr), 2);
            SetViewportOrgEx(memory, cr.left, cr.top, nullptr);
            SendMessageW(child, WM_PRINT, reinterpret_cast<WPARAM>(memory), PRF_CLIENT | PRF_NONCLIENT | PRF_ERASEBKGND);
            if (child == urlBox) SendMessageW(child, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(memory), PRF_CLIENT | PRF_ERASEBKGND);
        }
        SetViewportOrgEx(memory, 0, 0, nullptr); GdiFlush();
        auto dir = root / "test-output"; std::filesystem::create_directories(dir);
        BITMAPFILEHEADER bf{}; bf.bfType = 0x4d42; bf.bfOffBits = sizeof(bf) + sizeof(BITMAPINFOHEADER);
        bf.bfSize = bf.bfOffBits + r.right * r.bottom * 4;
        std::ofstream file(dir / "interface.bmp", std::ios::binary);
        file.write(reinterpret_cast<const char*>(&bf), sizeof(bf));
        file.write(reinterpret_cast<const char*>(&bi.bmiHeader), sizeof(BITMAPINFOHEADER));
        file.write(static_cast<const char*>(bits), r.right * r.bottom * 4); file.close();
        SelectObject(memory, previous); DeleteObject(bitmap); DeleteDC(memory); ReleaseDC(hwnd, screen);
        wchar_t savedUrl[4096]; GetWindowTextW(urlBox, savedUrl, 4096);
        bool good = file.good() && IsWindowEnabled(startButton) && !IsWindowEnabled(rejectButton) && std::wstring(savedUrl) == defaultStation;
        DestroyWindow(hwnd); engine.reset(); return good ? 0 : 1;
    }
    ShowWindow(hwnd, show); UpdateWindow(hwnd);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        // Keep typed URL characters intact; shortcuts work when focus is elsewhere.
        if (GetFocus() != urlBox && msg.message == WM_CHAR && (msg.wParam == L'-' || msg.wParam == L'>')) {
            SendMessageW(hwnd, WM_COMMAND, msg.wParam == L'-' ? Reject : Evolve, 0); continue;
        }
        if (!IsDialogMessageW(hwnd, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    engine.reset(); return 0;
}
