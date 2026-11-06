#include "MainWindow.h"
#include "AudioEngine.h"
#include "CBEngine.h"
#include "CbPresetIO.h"
#include "RoqIO.h"
#include "MidiInput.h"
#include "resource.h"

#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif

#include <commctrl.h>
#include <commdlg.h>
#include <oleacc.h>
#include <shlwapi.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <cwchar>
#include <cstdio>
#include <vector>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "oleacc.lib")

namespace {
constexpr wchar_t kClassName[] = L"CreakboxMainWnd";

std::wstring toWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(static_cast<size_t>(n ? n - 1 : 0), L'\0');
    if (n) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}
std::string fromWide(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n ? n - 1 : 0), '\0');
    if (n) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

HWND createLabel(HWND parent, HINSTANCE hInst, int id, int x, int y, int w, int h, const wchar_t* text) {
    return CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOTIFY,
        x, y, w, h, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), hInst, nullptr);
}
HWND createButton(HWND parent, HINSTANCE hInst, int id, int x, int y, int w, int h, const wchar_t* text) {
    return CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        x, y, w, h, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), hInst, nullptr);
}
HWND createEdit(HWND parent, HINSTANCE hInst, int id, int x, int y, int w, int h, const wchar_t* initial) {
    return CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", initial,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        x, y, w, h, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), hInst, nullptr);
}
HWND createCombo(HWND parent, HINSTANCE hInst, int id, int x, int y, int w, int h) {
    return CreateWindowExW(0, L"COMBOBOX", nullptr,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
        x, y, w, h, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), hInst, nullptr);
}

// Subclass for parameter sliders: Enter opens the sweep editor for that
// slider's parameter. The old wndproc is captured per-slider and stored
// in the window's user data so each slider chains to its own original.
static WNDPROC g_oldSliderProc = nullptr;
static LRESULT CALLBACK sliderWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

// Replacement wndproc for the note combobox: Page Down / Page Up jump one
// octave (12 items). Stored as the static "old proc" so we can chain.
static WNDPROC g_oldComboProc = nullptr;
LRESULT CALLBACK noteComboWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_KEYDOWN && (wp == VK_NEXT || wp == VK_PRIOR)) {
        const int count = static_cast<int>(SendMessageW(hwnd, CB_GETCOUNT, 0, 0));
        int cur = static_cast<int>(SendMessageW(hwnd, CB_GETCURSEL, 0, 0));
        if (cur == CB_ERR) cur = 0;
        int next = cur + (wp == VK_NEXT ? 12 : -12);
        if (next < 0) next = 0;
        if (next >= count) next = count - 1;
        if (next != cur) {
            SendMessageW(hwnd, CB_SETCURSEL, next, 0);
            HWND parent = GetParent(hwnd);
            const LONG id = GetWindowLongW(hwnd, GWL_ID);
            SendMessageW(parent, WM_COMMAND,
                         MAKEWPARAM(id, CBN_SELCHANGE),
                         reinterpret_cast<LPARAM>(hwnd));
            NotifyWinEvent(EVENT_OBJECT_VALUECHANGE, hwnd, OBJID_CLIENT, CHILDID_SELF);
        }
        return 0;
    }
    return CallWindowProcW(g_oldComboProc, hwnd, msg, wp, lp);
}

LRESULT CALLBACK sliderWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_GETDLGCODE) {
        // Base trackbar dlg code (typically DLGC_WANTARROWS). Only claim
        // the pending Enter keystroke; everything else (Tab, arrows, etc.)
        // stays with default handling so focus navigation still works.
        LRESULT base = CallWindowProcW(g_oldSliderProc, hwnd, msg, wp, lp);
        if (lp != 0) {
            MSG* pmsg = reinterpret_cast<MSG*>(lp);
            if (pmsg->message == WM_KEYDOWN && pmsg->wParam == VK_RETURN) {
                return base | DLGC_WANTMESSAGE;
            }
        }
        return base;
    }
    if (msg == WM_KEYDOWN && wp == VK_RETURN) {
        SendMessageW(GetParent(hwnd), WM_APP + 1,
                     GetWindowLongW(hwnd, GWL_ID), 0);
        return 0;
    }
    return CallWindowProcW(g_oldSliderProc, hwnd, msg, wp, lp);
}

// Replacement wndproc for the checkboxes. Enter already activates the
// sliders (opens their sweep editor), so a checkbox that ignores Enter and
// answers only to Space is an inconsistency you discover by pressing Enter
// and getting silence. IsDialogMessage routes Enter to the default button,
// and this window has none, so the keystroke otherwise goes nowhere.
// Space is handled by BS_AUTOCHECKBOX itself and is not touched here. Note
// that Space only reaches these controls at all because transport moved to
// F5-F8; while VK_SPACE was a global accelerator it was consumed before
// IsDialogMessage ever ran, so no checkbox in the window ever saw it.
static WNDPROC g_oldCheckProc = nullptr;
static LRESULT CALLBACK checkboxWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_GETDLGCODE) {
        // Claim only the pending Enter keystroke; Tab, Space and arrows stay
        // with default handling so focus navigation is untouched.
        LRESULT base = CallWindowProcW(g_oldCheckProc, hwnd, msg, wp, lp);
        if (lp != 0) {
            MSG* pmsg = reinterpret_cast<MSG*>(lp);
            if (pmsg->message == WM_KEYDOWN && pmsg->wParam == VK_RETURN)
                return base | DLGC_WANTMESSAGE;
        }
        return base;
    }
    if (msg == WM_KEYDOWN && wp == VK_RETURN) {
        // Toggle, then tell the parent, so the BN_CLICKED handler commits the
        // change and announces it exactly as a Space press or a click would.
        const bool on = SendMessageW(hwnd, BM_GETCHECK, 0, 0) == BST_CHECKED;
        SendMessageW(hwnd, BM_SETCHECK, on ? BST_UNCHECKED : BST_CHECKED, 0);
        SendMessageW(GetParent(hwnd), WM_COMMAND,
                     MAKEWPARAM(GetWindowLongW(hwnd, GWL_ID), BN_CLICKED),
                     reinterpret_cast<LPARAM>(hwnd));
        NotifyWinEvent(EVENT_OBJECT_VALUECHANGE, hwnd, OBJID_CLIENT, CHILDID_SELF);
        return 0;
    }
    return CallWindowProcW(g_oldCheckProc, hwnd, msg, wp, lp);
}

// All checkboxes share one window class, so the first subclass captures the
// original wndproc for every one of them.
static void makeCheckboxEnterActivatable(HWND box) {
    WNDPROC prev = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(box, GWLP_WNDPROC,
                          reinterpret_cast<LONG_PTR>(checkboxWndProc)));
    if (!g_oldCheckProc) g_oldCheckProc = prev;
}
} // namespace

MainWindow::MainWindow(CBEngine& engine, AudioEngine& audio, MidiInput& midi, HINSTANCE hInst)
    : engine_(engine), audio_(audio), midi_(midi), hInst_(hInst) {}

LRESULT CALLBACK MainWindow::staticProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = static_cast<MainWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    return self->proc(hwnd, msg, wp, lp);
}

bool MainWindow::create() {
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES | ICC_BAR_CLASSES };
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = &MainWindow::staticProc;
    wc.hInstance = hInst_;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = kClassName;
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    if (!RegisterClassExW(&wc)) return false;

    accel_ = LoadAcceleratorsW(hInst_, MAKEINTRESOURCEW(IDR_MAIN_MENU));

    HMENU menu = LoadMenuW(hInst_, MAKEINTRESOURCEW(IDR_MAIN_MENU));

    HWND h = CreateWindowExW(0, kClassName,
        L"Creakbox — accessible 303 bassline",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, 780, 672,
        nullptr, menu, hInst_, this);
    return h != nullptr;
}

void MainWindow::buildLayout() {
    int y = 10;

    // Row 1: Waveform combo + "Oscillator..." button opens the extras dialog
    // (PWM, sub sine, duplet voice, "Everybody needs a 303" reset).
    hWaveformLabel_ = createLabel(hwnd_, hInst_, IDC_WAVEFORM_LABEL, 10, y + 3, 90, 20, L"&Waveform:");
    hWaveform_ = createCombo(hwnd_, hInst_, IDC_WAVEFORM, 110, y, 120, 200);
    SendMessageW(hWaveform_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Saw"));
    SendMessageW(hWaveform_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Square"));
    SendMessageW(hWaveform_, CB_SETCURSEL, engine_.getParam(CbParam::Waveform) > 0.5f ? 1 : 0, 0);
    hOscButton_ = createButton(hwnd_, hInst_, IDC_OSC_BUTTON, 240, y, 100, 22, L"&Oscillator...");
    // Renamed from "Distortion..." when the compressor and ring modulator
    // joined it. The rename paid for a mnemonic: "Distortion" had no free
    // letter left, but X was still spare, so this control went from
    // keyboard-reachable-by-Tab-only to Alt+X. Ctrl+D still opens it too.
    hDistButton_ = createButton(hwnd_, hInst_, IDC_DIST_BUTTON, 346, y, 100, 22, L"Acid F&X...");
    y += 30;

    // Row 2: Tempo edit
    // Mnemonic is &m, not &T: "S&tep:" already owns Alt+T, and a duplicated
    // mnemonic cycles focus between the two matches instead of activating
    // either. Every mnemonic on this window has to be unique.
    hTempoLabel_ = createLabel(hwnd_, hInst_, IDC_TEMPO_LABEL, 10, y + 3, 90, 20, L"Te&mpo (BPM):");
    wchar_t tempoText[16]; swprintf(tempoText, 16, L"%d", static_cast<int>(engine_.getParam(CbParam::Tempo)));
    hTempo_ = createEdit(hwnd_, hInst_, IDC_TEMPO, 110, y, 80, 22, tempoText);
    y += 30;

    // Row 3: Transport button + Pattern selector combobox
    hTransport_ = createButton(hwnd_, hInst_, IDC_TRANSPORT, 10, y, 180, 28, L"&Run (F5)");
    // No mnemonic. Every letter of "Pattern" is taken (P by the menu bar, A by
    // the Accent slider, T by "S&tep:", E by EnvMod, R by Run, N by "&Note:"),
    // and a duplicate cycles focus between the matches rather than activating
    // anything. The combo is reached by Tab, and Ctrl+Left/Right changes
    // pattern directly without focusing it at all - which is the route the
    // step-sequencer heading already advertises.
    createLabel(hwnd_, hInst_, IDC_STATIC, 200, y + 6, 60, 18, L"Pattern:");
    hPatternCombo_ = CreateWindowExW(0, L"COMBOBOX", nullptr,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
        260, y + 4, 120, 200,
        hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_PATTERN_COMBO)), hInst_, nullptr);

    // Accent squelch: scales the residual-stacking amount for the TB-303
    // accent buildup. 100% = Frank's original behavior; 0 = no buildup;
    // >100% amplifies the squelch on successive accents.
    hAccentSquelchLabel_ = createLabel(hwnd_, hInst_, IDC_ACCENT_SQUELCH_LABEL,
        400, y + 2, 110, 16, L"Accent s&quelch:");
    hAccentSquelch_ = CreateWindowExW(0, TRACKBAR_CLASSW, L"Accent squelch",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_HORZ | TBS_AUTOTICKS,
        400, y + 16, 130, 22,
        hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_ACCENT_SQUELCH)), hInst_, nullptr);
    // 0 = Frank's original TB-303 accent (sq=1.0). 100 = double squelch
    // (sq=2.0) for progressively harder buildup on consecutive accents.
    SendMessageW(hAccentSquelch_, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
    SendMessageW(hAccentSquelch_, TBM_SETLINESIZE, 0, 1);
    SendMessageW(hAccentSquelch_, TBM_SETPAGESIZE, 0, 10);
    SendMessageW(hAccentSquelch_, TBM_SETPOS, TRUE, 0);
    hAccentSquelchValue_ = createLabel(hwnd_, hInst_, IDC_ACCENT_SQUELCH_VALUE,
        535, y + 18, 50, 16, L"0%");

    // Wildness slider: drives density (rest / slide / accent probability)
    // for all random-pattern and per-step randomize operations. Live slider,
    // not persisted — it's a working parameter the user tweaks as they go.
    hWildnessLabel_ = createLabel(hwnd_, hInst_, IDC_WILDNESS_LABEL,
        590, y + 2, 80, 16, L"W&ildness:");
    hWildness_ = CreateWindowExW(0, TRACKBAR_CLASSW, L"Wildness",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_HORZ | TBS_AUTOTICKS,
        590, y + 16, 120, 22,
        hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_WILDNESS)), hInst_, nullptr);
    SendMessageW(hWildness_, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
    SendMessageW(hWildness_, TBM_SETLINESIZE, 0, 1);
    SendMessageW(hWildness_, TBM_SETPAGESIZE, 0, 10);
    SendMessageW(hWildness_, TBM_SETPOS, TRUE, 50);
    hWildnessValue_ = createLabel(hwnd_, hInst_, IDC_WILDNESS_VALUE,
        715, y + 18, 50, 16, L"50%");
    y += 40;

    // Row 4: Parameter trackbars. Bipolar params (Cutoff, Rezo, EnvMod,
    // Tuning) get a -100..+100 range with 0 at centre so negative values
    // can be dialled in from the slider — required for the "inverted"
    // filter character. Unipolar (Volume, Decay, Accent) stay 0..100.
    // Mnemonics here must not repeat any other on this window — a duplicate
    // cycles focus between the matches rather than activating either. Hence
    // Deca&y rather than &Decay (D belongs to Wil&dness) and T&uning rather
    // than Tu&ning (N belongs to &Note:).
    static const wchar_t* kParamNames[7] = {
        L"&Volume", L"&Cutoff", L"Re&sonance", L"EnvMo&d", L"Deca&y", L"&Accent", L"T&uning"
    };
    // The same names WITHOUT the ampersand, for the trackbars' own window text.
    // The trackbar carries its name so a screen reader has one even before it
    // looks at the preceding STATIC - but if it carried the ampersand too, every
    // slider's mnemonic would match two controls and Win32 would cycle between
    // them instead of activating. Both matches happen to land on the same
    // trackbar here, so it was harmless, but "harmless duplicate mnemonic" is
    // exactly the shape of bug this window has been bitten by twice.
    static const wchar_t* kParamPlain[7] = {
        L"Volume", L"Cutoff", L"Resonance", L"EnvMod", L"Decay", L"Accent", L"Tuning"
    };
    static const CbParam kParamIds[7] = {
        CbParam::Volume, CbParam::Cutoff, CbParam::Resonance, CbParam::EnvMod,
        CbParam::Decay, CbParam::Accent, CbParam::Tuning
    };
    // Volume/Cutoff/Decay/Accent are unipolar 0..100. (The comment above
    // predates the table and wrongly lists Cutoff as bipolar; kBipolar is
    // the authority.)
    // Resonance/EnvMod/Tuning are bipolar -100..+100 (negatives produce
    // Frank's "inverted" filter character).
    static const bool kBipolar[7] = {
        false, false, true, true, false, false, true
    };
    // Slider: ~160 px wide each. Row is 7 * 168 ≈ 1176 — too wide. Make it
    // narrower and let them overflow into a scrollable area if needed; for
    // now keep 100 px wide so tabbing still reveals all.
    const int sliderW = 100;
    const int sliderH = 30;
    const int gap = 8;
    for (int i = 0; i < 7; ++i) {
        const int x = 10 + i * (sliderW + gap);
        hSliderLabels_[i] = createLabel(hwnd_, hInst_, IDC_STATIC, x, y, sliderW, 16, kParamNames[i]);
        hSliders_[i] = CreateWindowExW(0, TRACKBAR_CLASSW, kParamPlain[i],
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_HORZ | TBS_AUTOTICKS | TBS_NOTIFYBEFOREMOVE,
            x, y + 16, sliderW, sliderH,
            hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SLIDER_BASE + i)), hInst_, nullptr);
        const int rangeMin = kBipolar[i] ? -100 : 0;
        const int rangeMax = 100;
        SendMessageW(hSliders_[i], TBM_SETRANGE, TRUE, MAKELPARAM(rangeMin, rangeMax));
        // Arrow keys move by 1, PgUp/PgDn by 10, Home/End jumps to min/max.
        SendMessageW(hSliders_[i], TBM_SETLINESIZE, 0, 1);
        SendMessageW(hSliders_[i], TBM_SETPAGESIZE, 0, 10);
        // Initial position: engine stores 0..1 for unipolar params. For
        // bipolar params, the engine stores the bipolar value directly
        // (range -1..+1, centre 0 = neutral).
        int initPos = static_cast<int>(engine_.getParam(kParamIds[i]) * 100.0f);
        if (initPos < rangeMin) initPos = rangeMin;
        if (initPos > rangeMax) initPos = rangeMax;
        SendMessageW(hSliders_[i], TBM_SETPOS, TRUE, static_cast<LPARAM>(initPos));
        hSliderValues_[i] = createLabel(hwnd_, hInst_, IDC_STATIC, x, y + 16 + sliderH + 2, sliderW, 16, L"");
        updateSliderValue(i);
        // Subclass slider so Enter opens its sweep editor (only once;
        // all trackbars use the same class wndproc so g_oldSliderProc
        // captures the shared original).
        WNDPROC prev = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(hSliders_[i], GWLP_WNDPROC,
                              reinterpret_cast<LONG_PTR>(sliderWndProc)));
        if (!g_oldSliderProc) g_oldSliderProc = prev;
    }
    // Resonance boost sits directly under the Resonance slider (index 2) -
    // "under the filter", where the filter controls actually live. It lifts
    // the top of the resonance range past the point where the filter starts
    // to self-oscillate; see wwelti_IIR_IIRlp3pole_boost in IIR.cpp.
    {
        const int resoX = 10 + 2 * (sliderW + gap);
        hResoBoost_ = CreateWindowExW(0, L"BUTTON", L"&Boost resonance",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
            resoX, y + 16 + sliderH + 2 + 18, 140, 20,
            hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_RESO_BOOST)), hInst_, nullptr);
        SendMessageW(hResoBoost_, BM_SETCHECK,
                     engine_.getParam(CbParam::ResoBoost) > 0.5f ? BST_CHECKED : BST_UNCHECKED, 0);
        makeCheckboxEnterActivatable(hResoBoost_);
    }
    y += 16 + sliderH + 2 + 18 + 20 + 8;  // label + slider + value + boost + pad

    // Row 5: Step list header
    hStepListLabel_ = createLabel(hwnd_, hInst_, IDC_STEP_LIST_LABEL, 10, y, 720, 20,
        L"Step sequencer — Pattern 1 of 1  (pick a step, then set its note / slide / accent. Ctrl+Left/Right switches pattern.)");
    y += 24;

    // Pattern length. The sequencer has always honoured `lastStep` — it loops
    // there and derives song position from it — but nothing could set it to
    // anything but 15, and until .roq v10 nothing could save it either.
    // Created here, before the step selector, so tab order asks "how long is
    // this pattern" before "which step", which is the order it makes sense in
    // when you cannot see the window.
    createLabel(hwnd_, hInst_, IDC_PATTERN_LENGTH_LABEL, 520, y, 100, 16, L"Pattern len&gth:");
    hPatternLength_ = CreateWindowExW(0, L"COMBOBOX", nullptr,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
        520, y + 18, 100, 300,
        hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_PATTERN_LENGTH)), hInst_, nullptr);
    for (int n = 1; n <= 64; ++n) {
        wchar_t item[24];
        swprintf(item, 24, (n == 1) ? L"%d step" : L"%d steps", n);
        SendMessageW(hPatternLength_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item));
    }
    SendMessageW(hPatternLength_, CB_SETCURSEL, 15, 0);

    // Triplet feel, for THIS pattern. Beside the length control because the two
    // are read together: 12 steps straight is three quarters of a bar and loops
    // as a polyrhythm, 12 steps triplet fills the bar and swings.
    //
    // No mnemonic - every letter of "Triplet feel" is already taken (T by
    // "S&tep:", R by "&Run", I by "W&ildness", P and E and F and L by the menu
    // bar) and a duplicate cycles focus instead of activating. Ctrl+T toggles it
    // from anywhere, which is more than a mnemonic would have given.
    hPatternTriplet_ = CreateWindowExW(0, L"BUTTON", L"Triplet feel (Ctrl+T)",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
        628, y + 20, 130, 20,
        hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_PATTERN_TRIPLET)), hInst_, nullptr);
    makeCheckboxEnterActivatable(hPatternTriplet_);

    // Focused-step editor: one selector + one set of pitch/slide/accent
    // value controls. The value controls reflect whatever step the
    // selector has focused; editing writes back to that step.
    static const wchar_t* kNoteNames[12] = {
        L"C", L"C#", L"D", L"D#", L"E", L"F", L"F#", L"G", L"G#", L"A", L"A#", L"B"
    };

    // Step selector (ListBox: 16 items)
    createLabel(hwnd_, hInst_, IDC_STATIC, 10, y, 60, 16, L"S&tep:");
    hStepSelector_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", nullptr,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY,
        75, y, 90, 280,
        hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_STEP_SELECTOR)), hInst_, nullptr);
    // Items populated by populateStepList() so each row shows the step's
    // current state (note + slide/accent) for screen readers.

    // Pitch combobox (right of selector)
    createLabel(hwnd_, hInst_, IDC_STATIC, 180, y, 60, 16, L"&Note:");
    hStepPitch_ = CreateWindowExW(0, L"COMBOBOX", nullptr,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
        180, y + 18, 130, 280,
        hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_STEP_PITCH)), hInst_, nullptr);
    SendMessageW(hStepPitch_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Rest"));
    for (int p = 1; p <= 60; ++p) {
        wchar_t buf[8];
        const int n = p - 1;
        swprintf(buf, 8, L"%s%d", kNoteNames[n % 12], n / 12);
        SendMessageW(hStepPitch_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(buf));
    }
    // Replace the wndproc so Page Up / Page Down jump one octave.
    g_oldComboProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(hStepPitch_, GWLP_WNDPROC,
                          reinterpret_cast<LONG_PTR>(noteComboWndProc)));

    // These three carry no mnemonic, and every letter that could serve as one
    // is taken: R by "&Run", E by EnvMod, V by "&Volume", S by "Re&sonance",
    // A by "&Accent", T by "S&tep:", and P/E/F/H/L by the menu bar. A
    // duplicated mnemonic does not activate anything — Win32 cycles focus
    // between the matches — so a label with no underline is honest and one
    // with a colliding underline is not.
    //
    // A mnemonic would be the wrong shape here even if a letter were free.
    // These three are used WHILE arrowing through the step list, and a
    // mnemonic moves focus to the control it names, so every toggle would
    // throw you out of the list and cost a Tab to get back. An accelerator
    // fires without touching focus, which is the behaviour this job wants.
    //
    // The chord therefore goes in the LABEL, the same way "Triplet feel
    // (Ctrl+T)" does it above. Without that the key is real but undiscoverable:
    // nothing on screen or in the announcement says it exists.
    hStepSlide_ = CreateWindowExW(0, L"BUTTON", L"Slide this step (Ctrl+Shift+S)",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
        318, y + 4, 198, 22,
        hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_STEP_SLIDE)), hInst_, nullptr);

    hStepAccent_ = CreateWindowExW(0, L"BUTTON", L"Accent this step (Ctrl+Shift+A)",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
        318, y + 30, 198, 22,
        hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_STEP_ACCENT)), hInst_, nullptr);

    // This row clears the pattern-length combo's y range, so it may run wider
    // than the two above without colliding with it.
    hStepReverse_ = CreateWindowExW(0, L"BUTTON", L"Reverse step (Ctrl+Shift+I)",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
        318, y + 56, 198, 22,
        hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_STEP_REVERSE)), hInst_, nullptr);

    // Enter toggles these as well as Space, matching the sliders.
    makeCheckboxEnterActivatable(hStepSlide_);
    makeCheckboxEnterActivatable(hStepAccent_);
    makeCheckboxEnterActivatable(hStepReverse_);

    focusedStep_ = 0;
    populateStepList();
    y += 290;

    // Status label (screen-reader announce target)
    hStatus_ = createLabel(hwnd_, hInst_, IDC_STATUS_LABEL, 10, y, 720, 20, L"Ready.");
    // Show the audio sample rate so the user knows what WASAPI handed us.
    wchar_t initMsg[128];
    swprintf(initMsg, 128, L"Ready. Audio: %.0f Hz, WASAPI shared.  Sweeps off.",
             audio_.sampleRate());
    SetWindowTextW(hStatus_, initMsg);

    refreshPatternLabel();
}

// -------------------- Sweep editor --------------------

static const wchar_t* kSliderNames[7] = {
    L"Volume", L"Cutoff", L"Resonance", L"EnvMod", L"Decay", L"Accent", L"Tuning"
};
static bool isBipolarSlider(int idx) {
    // Indexes match the main-UI slider order: 0=Vol, 1=Cut, 2=Rez, 3=Env,
    // 4=Dec, 5=Acc, 6=Tune. Bipolar = can go negative.
    return idx == 2 || idx == 3 || idx == 6;
}
static void sweepValueLabelFor(int pos, wchar_t* out, size_t n) {
    if (pos >= 0) swprintf(out, n, L"+%d%%", pos);
    else          swprintf(out, n, L"%d%%", pos);
}

namespace { bool sweepsOverlap(const MainWindow::Sweep& a, const MainWindow::Sweep& b) {
    // Map (pattern, step) into a single integer along the song timeline so
    // overlap reduces to interval-vs-interval. The stride is the *maximum*
    // pattern length rather than the actual one: it only has to be big enough
    // that no two (pattern, step) pairs collide and that the mapping stays
    // increasing. A stride of 16 did both while every pattern was 16 steps, and
    // neither once one is longer - pattern 0 step 20 and pattern 1 step 4 would
    // both map to 20 and overlap checks would compare the wrong intervals.
    auto at = [](int pat, int step) { return pat * 64 + step; };
    int a0 = at(a.startPattern, a.startStep), a1 = at(a.endPattern, a.endStep);
    int b0 = at(b.startPattern, b.startStep), b1 = at(b.endPattern, b.endStep);
    if (a0 > a1) std::swap(a0, a1);
    if (b0 > b1) std::swap(b0, b1);
    // Intervals are inclusive. They overlap if neither ends before the other starts.
    return !(a1 < b0 || b1 < a0);
} }

INT_PTR CALLBACK MainWindow::sweepEditorProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(dlg, GWLP_USERDATA));
    if (msg == WM_INITDIALOG) {
        self = reinterpret_cast<MainWindow*>(lp);
        SetWindowLongPtrW(dlg, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return FALSE;
    return self->sweepEditorHandler(dlg, msg, wp, lp);
}

static void configureSweepValueTrackbars(HWND dlg, int paramIdx) {
    const int lo = isBipolarSlider(paramIdx) ? -100 : 0;
    const int hi = 100;
    for (int id : {IDC_SWEEP_START_VALUE, IDC_SWEEP_END_VALUE}) {
        HWND t = GetDlgItem(dlg, id);
        SendMessageW(t, TBM_SETRANGE, TRUE, MAKELPARAM(lo, hi));
        SendMessageW(t, TBM_SETLINESIZE, 0, 1);
        SendMessageW(t, TBM_SETPAGESIZE, 0, 10);
    }
}

// Steps offered for one endpoint = the steps its pattern actually has.
// Offering 16 everywhere would let a sweep be aimed at a step that does not
// exist in a short pattern, and hide steps 17+ of a long one.
static void fillStepCombo(HWND dlg, int comboId, int stepCount, int keepSel) {
    HWND h = GetDlgItem(dlg, comboId);
    if (!h) return;
    if (stepCount < 1)  stepCount = 1;
    if (stepCount > 64) stepCount = 64;
    SendMessageW(h, CB_RESETCONTENT, 0, 0);
    for (int s = 0; s < stepCount; ++s) {
        wchar_t buf[16]; swprintf(buf, 16, L"%d", s + 1);
        SendMessageW(h, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(buf));
    }
    if (keepSel < 0) keepSel = 0;
    if (keepSel >= stepCount) keepSel = stepCount - 1;
    SendMessageW(h, CB_SETCURSEL, keepSel, 0);
    NotifyWinEvent(EVENT_OBJECT_VALUECHANGE, h, OBJID_CLIENT, CHILDID_SELF);
}

// Refill one endpoint's step combo after its pattern combo changed.
static void refillStepsForPattern(HWND dlg, int patternId, int stepId,
                                  const std::vector<int>& patLens) {
    const int p = (int)SendMessageW(GetDlgItem(dlg, patternId), CB_GETCURSEL, 0, 0);
    if (p < 0 || p >= (int)patLens.size()) return;
    const int keep = (int)SendMessageW(GetDlgItem(dlg, stepId), CB_GETCURSEL, 0, 0);
    fillStepCombo(dlg, stepId, patLens[p], keep);
}

static void populateSweepDialog(HWND dlg, const std::vector<int>& patLens,
                                const MainWindow::Sweep& sw,
                                const std::vector<MainWindow::Sweep>& all, int curIdx) {
    const int numPatterns = (int)patLens.size();
    // Fill sweep-number combo
    HWND num = GetDlgItem(dlg, IDC_SWEEP_NUMBER);
    SendMessageW(num, CB_RESETCONTENT, 0, 0);
    for (int i = 0; i < (int)all.size(); ++i) {
        wchar_t buf[16]; swprintf(buf, 16, L"%d", i + 1);
        SendMessageW(num, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(buf));
    }
    if (all.empty()) SendMessageW(num, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"(new)"));
    SendMessageW(num, CB_SETCURSEL, curIdx, 0);

    auto fillPatterns = [&](int id) {
        HWND h = GetDlgItem(dlg, id);
        SendMessageW(h, CB_RESETCONTENT, 0, 0);
        for (int p = 0; p < numPatterns; ++p) {
            wchar_t buf[16]; swprintf(buf, 16, L"%d", p + 1);
            SendMessageW(h, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(buf));
        }
    };
    fillPatterns(IDC_SWEEP_START_PATTERN);
    fillPatterns(IDC_SWEEP_END_PATTERN);

    auto lenOf = [&](int p) { return (p >= 0 && p < numPatterns) ? patLens[p] : 16; };
    SendMessageW(GetDlgItem(dlg, IDC_SWEEP_START_PATTERN), CB_SETCURSEL, sw.startPattern, 0);
    SendMessageW(GetDlgItem(dlg, IDC_SWEEP_END_PATTERN),   CB_SETCURSEL, sw.endPattern,   0);
    // After the pattern combos, so each step list is sized to its own pattern.
    fillStepCombo(dlg, IDC_SWEEP_START_STEP, lenOf(sw.startPattern), sw.startStep);
    fillStepCombo(dlg, IDC_SWEEP_END_STEP,   lenOf(sw.endPattern),   sw.endStep);
    SendMessageW(GetDlgItem(dlg, IDC_SWEEP_START_VALUE), TBM_SETPOS, TRUE, sw.startValue);
    SendMessageW(GetDlgItem(dlg, IDC_SWEEP_END_VALUE),   TBM_SETPOS, TRUE, sw.endValue);
    wchar_t sv[16], ev[16];
    sweepValueLabelFor(sw.startValue, sv, 16);
    sweepValueLabelFor(sw.endValue,   ev, 16);
    SetWindowTextW(GetDlgItem(dlg, IDC_SWEEP_START_VALUE_LBL), sv);
    SetWindowTextW(GetDlgItem(dlg, IDC_SWEEP_END_VALUE_LBL),   ev);

    wchar_t cnt[32]; swprintf(cnt, 32, L"Sweeps: %d", (int)all.size());
    SetWindowTextW(GetDlgItem(dlg, IDC_SWEEP_COUNT_LABEL), cnt);
}

static MainWindow::Sweep readSweepFromDialog(HWND dlg) {
    MainWindow::Sweep sw;
    sw.startPattern = (int)SendMessageW(GetDlgItem(dlg, IDC_SWEEP_START_PATTERN), CB_GETCURSEL, 0, 0);
    sw.startStep    = (int)SendMessageW(GetDlgItem(dlg, IDC_SWEEP_START_STEP),    CB_GETCURSEL, 0, 0);
    sw.endPattern   = (int)SendMessageW(GetDlgItem(dlg, IDC_SWEEP_END_PATTERN),   CB_GETCURSEL, 0, 0);
    sw.endStep      = (int)SendMessageW(GetDlgItem(dlg, IDC_SWEEP_END_STEP),      CB_GETCURSEL, 0, 0);
    sw.startValue   = (int)SendMessageW(GetDlgItem(dlg, IDC_SWEEP_START_VALUE), TBM_GETPOS, 0, 0);
    sw.endValue     = (int)SendMessageW(GetDlgItem(dlg, IDC_SWEEP_END_VALUE),   TBM_GETPOS, 0, 0);
    return sw;
}

INT_PTR MainWindow::sweepEditorHandler(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_INITDIALOG: {
            wchar_t hdr[128];
            swprintf(hdr, 128, L"Editing sweeps for: %s", kSliderNames[editingSweepParam_]);
            SetWindowTextW(GetDlgItem(dlg, IDC_SWEEP_PARAM_LABEL), hdr);

            configureSweepValueTrackbars(dlg, editingSweepParam_);

            const auto& list = paramSweeps_[editingSweepParam_];
            Sweep current = list.empty() ? Sweep{} : list[editingSweepIndex_];
            populateSweepDialog(dlg, patternLengths(),
                                current, list, editingSweepIndex_);
            SetFocus(GetDlgItem(dlg, IDC_SWEEP_NUMBER));
            return FALSE;
        }
        case WM_HSCROLL: {
            // Refresh both value labels on any trackbar scroll — cheap and
            // keeps them in sync with the current slider positions.
            int startPos = (int)SendMessageW(GetDlgItem(dlg, IDC_SWEEP_START_VALUE), TBM_GETPOS, 0, 0);
            int endPos   = (int)SendMessageW(GetDlgItem(dlg, IDC_SWEEP_END_VALUE),   TBM_GETPOS, 0, 0);
            wchar_t s[16], e[16];
            sweepValueLabelFor(startPos, s, 16);
            sweepValueLabelFor(endPos,   e, 16);
            SetWindowTextW(GetDlgItem(dlg, IDC_SWEEP_START_VALUE_LBL), s);
            SetWindowTextW(GetDlgItem(dlg, IDC_SWEEP_END_VALUE_LBL),   e);
            HWND which = reinterpret_cast<HWND>(lp);  // which trackbar moved
            if (which) {
                NotifyWinEvent(EVENT_OBJECT_VALUECHANGE, which, OBJID_CLIENT, CHILDID_SELF);
            }
            return TRUE;
        }
        case WM_COMMAND: {
            const int id = LOWORD(wp);
            const int notif = HIWORD(wp);
            auto setStatus = [&](const wchar_t* s) {
                SetWindowTextW(GetDlgItem(dlg, IDC_SWEEP_STATUS), s);
                NotifyWinEvent(EVENT_OBJECT_NAMECHANGE, GetDlgItem(dlg, IDC_SWEEP_STATUS),
                               OBJID_CLIENT, CHILDID_SELF);
            };
            auto& list = paramSweeps_[editingSweepParam_];
            if ((id == IDC_SWEEP_START_PATTERN || id == IDC_SWEEP_END_PATTERN)
                && notif == CBN_SELCHANGE) {
                const std::vector<int> lens = patternLengths();
                refillStepsForPattern(dlg,
                    (id == IDC_SWEEP_START_PATTERN) ? IDC_SWEEP_START_PATTERN : IDC_SWEEP_END_PATTERN,
                    (id == IDC_SWEEP_START_PATTERN) ? IDC_SWEEP_START_STEP    : IDC_SWEEP_END_STEP,
                    lens);
                return TRUE;
            }
            if (id == IDC_SWEEP_NUMBER && notif == CBN_SELCHANGE) {
                int sel = (int)SendMessageW(GetDlgItem(dlg, IDC_SWEEP_NUMBER), CB_GETCURSEL, 0, 0);
                if (sel >= 0 && sel < (int)list.size()) {
                    editingSweepIndex_ = sel;
                    populateSweepDialog(dlg, patternLengths(),
                                        list[sel], list, sel);
                }
                return TRUE;
            }
            if (id == IDC_SWEEP_ADD) {
                Sweep sw = readSweepFromDialog(dlg);
                for (size_t k = 0; k < list.size(); ++k) {
                    if (sweepsOverlap(list[k], sw)) {
                        wchar_t m[128];
                        swprintf(m, 128, L"Overlap with sweep %zu. Adjust ranges.", k + 1);
                        setStatus(m);
                        return TRUE;
                    }
                }
                list.push_back(sw);
                editingSweepIndex_ = (int)list.size() - 1;
                populateSweepDialog(dlg, patternLengths(),
                                    list.back(), list, editingSweepIndex_);
                setStatus(L"Sweep added.");
                return TRUE;
            }
            if (id == IDC_SWEEP_REMOVE) {
                if (list.empty()) { setStatus(L"No sweep to remove."); return TRUE; }
                list.erase(list.begin() + editingSweepIndex_);
                if (editingSweepIndex_ >= (int)list.size()) editingSweepIndex_ = (list.empty() ? 0 : (int)list.size() - 1);
                Sweep show = list.empty() ? Sweep{} : list[editingSweepIndex_];
                populateSweepDialog(dlg, patternLengths(),
                                    show, list, editingSweepIndex_);
                setStatus(L"Sweep removed.");
                return TRUE;
            }
            if (id == IDC_SWEEP_COPY) {
                sweepClipboard_ = readSweepFromDialog(dlg);
                sweepClipboardValid_ = true;
                setStatus(L"Sweep copied to clipboard.");
                return TRUE;
            }
            if (id == IDC_SWEEP_PASTE) {
                if (!sweepClipboardValid_) { setStatus(L"Clipboard empty."); return TRUE; }
                populateSweepDialog(dlg, patternLengths(),
                                    sweepClipboard_, list, editingSweepIndex_);
                setStatus(L"Pasted — press Add to commit.");
                return TRUE;
            }
            if (id == IDC_SWEEP_SAVE) {
                // Save the current slider's sweep list to a .hape file.
                wchar_t path[MAX_PATH]{};
                OPENFILENAMEW ofn{ sizeof(ofn) };
                ofn.hwndOwner = dlg;
                ofn.lpstrFilter = L"Creakbox sweeps (*.hape)\0*.hape\0";
                ofn.lpstrDefExt = L"hape";
                ofn.lpstrFile = path;
                ofn.nMaxFile = MAX_PATH;
                ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
                if (!GetSaveFileNameW(&ofn)) { setStatus(L"Save cancelled."); return TRUE; }

                std::vector<RoqSweep> out;
                out.reserve(list.size());
                for (const auto& s : list) {
                    RoqSweep sw;
                    sw.sliderIdx = editingSweepParam_;
                    sw.startPat = s.startPattern; sw.startStep = s.startStep; sw.startVal = s.startValue;
                    sw.endPat   = s.endPattern;   sw.endStep   = s.endStep;   sw.endVal   = s.endValue;
                    out.push_back(sw);
                }
                std::string err;
                if (!saveHape(fromWide(path), out, err)) {
                    wchar_t m[256]; swprintf(m, 256, L"Save failed: %hs", err.c_str());
                    setStatus(m);
                } else {
                    wchar_t m[128]; swprintf(m, 128, L"Saved %zu sweep(s) to file.", out.size());
                    setStatus(m);
                }
                return TRUE;
            }
            if (id == IDC_SWEEP_LOAD) {
                // Load sweeps from a .hape file, replacing the current slider's list.
                wchar_t path[MAX_PATH]{};
                OPENFILENAMEW ofn{ sizeof(ofn) };
                ofn.hwndOwner = dlg;
                ofn.lpstrFilter = L"Creakbox sweeps (*.hape)\0*.hape\0All files\0*.*\0";
                ofn.lpstrFile = path;
                ofn.nMaxFile = MAX_PATH;
                ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
                if (!GetOpenFileNameW(&ofn)) { setStatus(L"Load cancelled."); return TRUE; }

                std::string err;
                auto loaded = loadHape(fromWide(path), err);
                if (!loaded) {
                    wchar_t m[256]; swprintf(m, 256, L"Load failed: %hs", err.c_str());
                    setStatus(m);
                    return TRUE;
                }
                // Keep only sweeps that match the currently-edited slider.
                // Others are dropped with a note so the user knows why the
                // count is smaller than the file claimed.
                list.clear();
                int skipped = 0;
                for (const auto& sw : *loaded) {
                    if (sw.sliderIdx != editingSweepParam_) { ++skipped; continue; }
                    Sweep ui;
                    ui.startPattern = sw.startPat; ui.startStep = sw.startStep; ui.startValue = sw.startVal;
                    ui.endPattern   = sw.endPat;   ui.endStep   = sw.endStep;   ui.endValue   = sw.endVal;
                    list.push_back(ui);
                }
                editingSweepIndex_ = 0;
                Sweep show = list.empty() ? Sweep{} : list.front();
                populateSweepDialog(dlg, patternLengths(),
                                    show, list, editingSweepIndex_);
                wchar_t m[192];
                if (skipped > 0) {
                    swprintf(m, 192, L"Loaded %zu sweep(s). Skipped %d for other sliders.",
                             list.size(), skipped);
                } else {
                    swprintf(m, 128, L"Loaded %zu sweep(s).", list.size());
                }
                setStatus(m);
                return TRUE;
            }
            if (id == IDOK || id == IDCANCEL) {
                EndDialog(dlg, id);
                return TRUE;
            }
            break;
        }
    }
    return FALSE;
}

// -------------------- Oscillator dialog --------------------

INT_PTR CALLBACK MainWindow::oscDialogProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    MainWindow* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(dlg, GWLP_USERDATA));
    if (msg == WM_INITDIALOG) {
        self = reinterpret_cast<MainWindow*>(lp);
        SetWindowLongPtrW(dlg, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return FALSE;
    return self->oscDialogHandler(dlg, msg, wp, lp);
}

static void setTrackAndLabel(HWND dlg, int tbId, int lblId, int pos) {
    HWND t = GetDlgItem(dlg, tbId);
    SendMessageW(t, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
    SendMessageW(t, TBM_SETLINESIZE, 0, 1);
    SendMessageW(t, TBM_SETPAGESIZE, 0, 10);
    SendMessageW(t, TBM_SETPOS, TRUE, pos);
    wchar_t buf[16]; swprintf(buf, 16, L"%d%%", pos);
    SetWindowTextW(GetDlgItem(dlg, lblId), buf);
}

// Detune slider: bipolar ±1200 cents. Arrow keys step 10 cents; Page Up/Down
// step 100 cents (one semitone); Home/End jump to ±1 octave from unison.
static void setDetuneTrack(HWND dlg, int cents) {
    HWND t = GetDlgItem(dlg, IDC_OSC_DUP_DETUNE);
    SendMessageW(t, TBM_SETRANGE, TRUE, MAKELPARAM(-1200, 1200));
    SendMessageW(t, TBM_SETLINESIZE, 0, 10);
    SendMessageW(t, TBM_SETPAGESIZE, 0, 100);
    SendMessageW(t, TBM_SETPOS, TRUE, cents);
    wchar_t buf[24];
    if (cents >= 0) swprintf(buf, 24, L"+%d c", cents);
    else            swprintf(buf, 24, L"%d c", cents);
    SetWindowTextW(GetDlgItem(dlg, IDC_OSC_DUP_DETUNE_VALUE), buf);
}

// PWM LFO rate taper. The slider is 0..100; position 0 is a detent meaning
// off, and 1..100 map exponentially onto 0.05..100 Hz. A linear 0..100 Hz
// slider would bury the slow region where PWM is actually musical in the
// first few percent of travel - and, since trackbars are integer-valued,
// would put nothing at all below 1 Hz. Exponential gives the slow end about
// half the slider.
//
// The engine and the .roq file both store Hz, never slider position, so
// changing this taper later cannot repitch already-saved songs.
constexpr double kLfoMinHz = 0.05;
constexpr double kLfoMaxHz = 100.0;

static double lfoRateFromPos(int pos) {
    if (pos <= 0) return 0.0;
    return kLfoMinHz * std::pow(kLfoMaxHz / kLfoMinHz, (pos - 1) / 99.0);
}

static int lfoPosFromRate(double hz) {
    if (hz <= 0.0) return 0;
    const double p = 1.0 + 99.0 * std::log(hz / kLfoMinHz) / std::log(kLfoMaxHz / kLfoMinHz);
    return std::clamp(static_cast<int>(std::lround(p)), 1, 100);
}

static void formatLfoRate(double hz, wchar_t* out, size_t n) {
    if (hz <= 0.0)      swprintf(out, n, L"off");
    else if (hz < 10.0) swprintf(out, n, L"%.2f Hz", hz);
    else                swprintf(out, n, L"%.1f Hz", hz);
}

// Rate needs its own setter: the position/value mapping is not the plain
// percentage the other oscillator sliders use.
static void setLfoRateTrack(HWND dlg, double hz) {
    HWND t = GetDlgItem(dlg, IDC_OSC_LFO_RATE);
    SendMessageW(t, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
    SendMessageW(t, TBM_SETLINESIZE, 0, 1);
    SendMessageW(t, TBM_SETPAGESIZE, 0, 10);
    SendMessageW(t, TBM_SETPOS, TRUE, lfoPosFromRate(hz));
    wchar_t buf[24]; formatLfoRate(hz, buf, 24);
    SetWindowTextW(GetDlgItem(dlg, IDC_OSC_LFO_RATE_VALUE), buf);
}

// PWM bends only the square path - both saw branches in CBEngine ignore the
// duty threshold entirely, so a depth control is inert whenever its
// oscillator is on saw. Say so in the label instead of disabling the control:
// the MAIN waveform is chosen on the main window, not in this dialog, so a
// greyed slider would announce "unavailable" to a screen reader with no
// discoverable cause.
void MainWindow::refreshLfoDepthLabels(HWND dlg) {
    const bool mainSaw = engine_.getParam(CbParam::Waveform)       <= 0.5f;
    const bool dupSaw  = engine_.getParam(CbParam::DupletWaveform) <= 0.5f;
    SetWindowTextW(GetDlgItem(dlg, IDC_OSC_LFO_MAIN_LABEL),
        mainSaw ? L"&Main depth (saw - no effect):" : L"&Main depth:");
    SetWindowTextW(GetDlgItem(dlg, IDC_OSC_LFO_DUP_LABEL),
        dupSaw ? L"D&uplet depth (saw - no effect):" : L"D&uplet depth:");
}

INT_PTR MainWindow::oscDialogHandler(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_INITDIALOG: {
            setTrackAndLabel(dlg, IDC_OSC_PWM, IDC_OSC_PWM_VALUE,
                static_cast<int>(engine_.getParam(CbParam::PWM) * 100.0f + 0.5f));
            setTrackAndLabel(dlg, IDC_OSC_SUB, IDC_OSC_SUB_VALUE,
                static_cast<int>(engine_.getParam(CbParam::SubOscLevel) * 100.0f + 0.5f));
            setTrackAndLabel(dlg, IDC_OSC_NOISE, IDC_OSC_NOISE_VALUE,
                static_cast<int>(engine_.getParam(CbParam::NoiseLevel) * 100.0f + 0.5f));
            setTrackAndLabel(dlg, IDC_OSC_DUP_PWM, IDC_OSC_DUP_PWM_VALUE,
                static_cast<int>(engine_.getParam(CbParam::DupletPWM) * 100.0f + 0.5f));
            setDetuneTrack(dlg,
                static_cast<int>(engine_.getParam(CbParam::DupletDetune) * 1200.0f + 0.5f));
            setTrackAndLabel(dlg, IDC_OSC_DUP_LEVEL, IDC_OSC_DUP_LEVEL_VALUE,
                static_cast<int>(engine_.getParam(CbParam::DupletLevel) * 100.0f + 0.5f));
            HWND wv = GetDlgItem(dlg, IDC_OSC_DUP_WAVEFORM);
            SendMessageW(wv, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Saw"));
            SendMessageW(wv, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Square"));
            SendMessageW(wv, CB_SETCURSEL,
                engine_.getParam(CbParam::DupletWaveform) > 0.5f ? 1 : 0, 0);
            setLfoRateTrack(dlg, engine_.getParam(CbParam::PwmLfoRate));
            setTrackAndLabel(dlg, IDC_OSC_LFO_MAIN, IDC_OSC_LFO_MAIN_VALUE,
                static_cast<int>(engine_.getParam(CbParam::PwmLfoDepth) * 100.0f + 0.5f));
            setTrackAndLabel(dlg, IDC_OSC_LFO_DUP, IDC_OSC_LFO_DUP_VALUE,
                static_cast<int>(engine_.getParam(CbParam::PwmLfoDupletDepth) * 100.0f + 0.5f));
            refreshLfoDepthLabels(dlg);
            SetFocus(GetDlgItem(dlg, IDC_OSC_PWM));
            return FALSE;
        }
        case WM_HSCROLL: {
            HWND which = reinterpret_cast<HWND>(lp);
            int pos = static_cast<int>(SendMessageW(which, TBM_GETPOS, 0, 0));
            // Detune is the odd one out: bipolar ±1200 cents, shown in cents.
            if (which == GetDlgItem(dlg, IDC_OSC_DUP_DETUNE)) {
                engine_.setParam(CbParam::DupletDetune, pos / 1200.0f);
                wchar_t buf[24];
                if (pos >= 0) swprintf(buf, 24, L"+%d c", pos);
                else          swprintf(buf, 24, L"%d c", pos);
                SetWindowTextW(GetDlgItem(dlg, IDC_OSC_DUP_DETUNE_VALUE), buf);
                NotifyWinEvent(EVENT_OBJECT_VALUECHANGE, which, OBJID_CLIENT, CHILDID_SELF);
                return TRUE;
            }
            // Rate is the other odd one out: exponential Hz, shown in Hz.
            if (which == GetDlgItem(dlg, IDC_OSC_LFO_RATE)) {
                const double hz = lfoRateFromPos(pos);
                engine_.setParam(CbParam::PwmLfoRate, static_cast<float>(hz));
                wchar_t buf[24]; formatLfoRate(hz, buf, 24);
                SetWindowTextW(GetDlgItem(dlg, IDC_OSC_LFO_RATE_VALUE), buf);
                NotifyWinEvent(EVENT_OBJECT_VALUECHANGE, which, OBJID_CLIENT, CHILDID_SELF);
                return TRUE;
            }
            int lblId = 0; CbParam pm = CbParam::NumParams;
            if (which == GetDlgItem(dlg, IDC_OSC_PWM))        { pm = CbParam::PWM;         lblId = IDC_OSC_PWM_VALUE; }
            else if (which == GetDlgItem(dlg, IDC_OSC_SUB))   { pm = CbParam::SubOscLevel; lblId = IDC_OSC_SUB_VALUE; }
            else if (which == GetDlgItem(dlg, IDC_OSC_NOISE)) { pm = CbParam::NoiseLevel;  lblId = IDC_OSC_NOISE_VALUE; }
            else if (which == GetDlgItem(dlg, IDC_OSC_DUP_PWM))    { pm = CbParam::DupletPWM;    lblId = IDC_OSC_DUP_PWM_VALUE; }
            else if (which == GetDlgItem(dlg, IDC_OSC_DUP_LEVEL))  { pm = CbParam::DupletLevel;  lblId = IDC_OSC_DUP_LEVEL_VALUE; }
            else if (which == GetDlgItem(dlg, IDC_OSC_LFO_MAIN))   { pm = CbParam::PwmLfoDepth;       lblId = IDC_OSC_LFO_MAIN_VALUE; }
            else if (which == GetDlgItem(dlg, IDC_OSC_LFO_DUP))    { pm = CbParam::PwmLfoDupletDepth; lblId = IDC_OSC_LFO_DUP_VALUE; }
            if (pm != CbParam::NumParams) {
                engine_.setParam(pm, pos / 100.0f);
                wchar_t buf[16]; swprintf(buf, 16, L"%d%%", pos);
                SetWindowTextW(GetDlgItem(dlg, lblId), buf);
                NotifyWinEvent(EVENT_OBJECT_VALUECHANGE, which, OBJID_CLIENT, CHILDID_SELF);
            }
            return TRUE;
        }
        case WM_COMMAND: {
            const int id = LOWORD(wp);
            const int notif = HIWORD(wp);
            if (id == IDC_OSC_DUP_WAVEFORM && notif == CBN_SELCHANGE) {
                int sel = (int)SendMessageW(GetDlgItem(dlg, IDC_OSC_DUP_WAVEFORM), CB_GETCURSEL, 0, 0);
                engine_.setParam(CbParam::DupletWaveform, sel == 1 ? 1.0f : 0.0f);
                refreshLfoDepthLabels(dlg);
                return TRUE;
            }
            if (id == IDC_OSC_RESET_303) {
                resetTo303();
                // Reload dialog controls to reflect reset defaults.
                setTrackAndLabel(dlg, IDC_OSC_PWM, IDC_OSC_PWM_VALUE, 50);
                setTrackAndLabel(dlg, IDC_OSC_SUB, IDC_OSC_SUB_VALUE, 0);
                setTrackAndLabel(dlg, IDC_OSC_NOISE, IDC_OSC_NOISE_VALUE, 0);
                setTrackAndLabel(dlg, IDC_OSC_DUP_PWM, IDC_OSC_DUP_PWM_VALUE, 50);
                setDetuneTrack(dlg, 0);
                setTrackAndLabel(dlg, IDC_OSC_DUP_LEVEL, IDC_OSC_DUP_LEVEL_VALUE, 0);
                SendMessageW(GetDlgItem(dlg, IDC_OSC_DUP_WAVEFORM), CB_SETCURSEL, 1, 0);
                setLfoRateTrack(dlg, 0.0);
                setTrackAndLabel(dlg, IDC_OSC_LFO_MAIN, IDC_OSC_LFO_MAIN_VALUE, 0);
                setTrackAndLabel(dlg, IDC_OSC_LFO_DUP, IDC_OSC_LFO_DUP_VALUE, 0);
                refreshLfoDepthLabels(dlg);
                return TRUE;
            }
            if (id == IDOK || id == IDCANCEL) { EndDialog(dlg, id); return TRUE; }
            break;
        }
    }
    return FALSE;
}

// -------------------- Distortion dialog --------------------
//
// The stage sits after the filter and the volume envelope, which is where a
// pedal sits relative to a real 303's output jack. Type "Classic" is the stock
// 2004 clipper and the default, so an untouched song sounds exactly as before.
static const wchar_t* kDistNames[] = {
    L"Classic (stock 303 clipper)",
    L"RAT (hard clip, dark filter)",
    L"Screamer (soft clip, mid push)",
    L"Dist+ (hard clip, bright)",
    L"Fuzz (asymmetric, germanium)",
};

void MainWindow::openDistortionDialog() {
    DialogBoxParamW(hInst_, MAKEINTRESOURCEW(IDD_DISTORTION), hwnd_,
                    &MainWindow::distDialogProc,
                    reinterpret_cast<LPARAM>(this));
}

INT_PTR CALLBACK MainWindow::distDialogProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    MainWindow* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(dlg, GWLP_USERDATA));
    if (msg == WM_INITDIALOG) {
        self = reinterpret_cast<MainWindow*>(lp);
        SetWindowLongPtrW(dlg, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return FALSE;
    return self->distDialogHandler(dlg, msg, wp, lp);
}

// Ring carrier taper. 5 Hz to 2 kHz, exponential, because the interesting
// range is enormous and linear would put everything musical in the bottom
// centimetre of the slider: below ~30 Hz it is a per-note tremolo, the low
// hundreds are where a bassline turns metallic, and the top is bell territory.
// Hz is what gets STORED - the engine and the file both hold a real frequency,
// so this taper can be retuned later without repitching saved songs.
static const double kRingMinHz = 5.0;
static const double kRingMaxHz = 2000.0;

static double ringHzFromPos(int pos) {
    if (pos < 0) pos = 0;
    if (pos > 100) pos = 100;
    return kRingMinHz * std::pow(kRingMaxHz / kRingMinHz, pos / 100.0);
}

static int ringPosFromHz(double hz) {
    if (hz <= kRingMinHz) return 0;
    const double p = 100.0 * std::log(hz / kRingMinHz) / std::log(kRingMaxHz / kRingMinHz);
    return std::clamp(static_cast<int>(std::lround(p)), 0, 100);
}

static void formatRingHz(double hz, wchar_t* out, size_t n) {
    if (hz < 10.0) swprintf(out, n, L"%.1f Hz", hz);
    else           swprintf(out, n, L"%.0f Hz", hz);
}

// "off" rather than "0%" at zero, for both amount controls. A screen reader
// reading "0 percent" leaves the user to infer that the effect is disabled;
// saying so is one word and removes the inference.
static void setAmountLabel(HWND dlg, int lblId, int pos) {
    wchar_t buf[16];
    if (pos <= 0) wcscpy_s(buf, L"off");
    else          swprintf(buf, 16, L"%d%%", pos);
    SetWindowTextW(GetDlgItem(dlg, lblId), buf);
}

INT_PTR MainWindow::distDialogHandler(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_INITDIALOG: {
        HWND cb = GetDlgItem(dlg, IDC_DIST_TYPE);
        for (const wchar_t* n : kDistNames)
            SendMessageW(cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(n));
        int t = (int)(engine_.getParam(CbParam::DistType) + 0.5f);
        const int count = (int)(sizeof(kDistNames) / sizeof(kDistNames[0]));
        if (t < 0 || t >= count) t = 0;
        SendMessageW(cb, CB_SETCURSEL, t, 0);
        setTrackAndLabel(dlg, IDC_DIST_GAIN,  IDC_DIST_GAIN_VALUE,
                         (int)(engine_.getParam(CbParam::DistGain)  * 100.0f + 0.5f));
        setTrackAndLabel(dlg, IDC_DIST_TONE,  IDC_DIST_TONE_VALUE,
                         (int)(engine_.getParam(CbParam::DistTone)  * 100.0f + 0.5f));
        setTrackAndLabel(dlg, IDC_DIST_LEVEL, IDC_DIST_LEVEL_VALUE,
                         (int)(engine_.getParam(CbParam::DistLevel) * 100.0f + 0.5f));

        setTrackAndLabel(dlg, IDC_FX_COMP_THRESH, IDC_FX_COMP_THRESH_VALUE,
                         (int)(engine_.getParam(CbParam::CompThreshold) * 100.0f + 0.5f));
        const int depthPos = (int)(engine_.getParam(CbParam::CompDepth) * 100.0f + 0.5f);
        setTrackAndLabel(dlg, IDC_FX_COMP_DEPTH, IDC_FX_COMP_DEPTH_VALUE, depthPos);
        setAmountLabel(dlg, IDC_FX_COMP_DEPTH_VALUE, depthPos);

        {
            HWND t = GetDlgItem(dlg, IDC_FX_RING_HZ);
            SendMessageW(t, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
            SendMessageW(t, TBM_SETLINESIZE, 0, 1);
            SendMessageW(t, TBM_SETPAGESIZE, 0, 10);
            const double hz = engine_.getParam(CbParam::RingHz);
            SendMessageW(t, TBM_SETPOS, TRUE, ringPosFromHz(hz));
            wchar_t buf[24]; formatRingHz(hz, buf, 24);
            SetWindowTextW(GetDlgItem(dlg, IDC_FX_RING_HZ_VALUE), buf);
        }
        const int ringPos = (int)(engine_.getParam(CbParam::RingAmount) * 100.0f + 0.5f);
        setTrackAndLabel(dlg, IDC_FX_RING_AMOUNT, IDC_FX_RING_AMOUNT_VALUE, ringPos);
        setAmountLabel(dlg, IDC_FX_RING_AMOUNT_VALUE, ringPos);

        refreshDistLabels(dlg);
        return TRUE;
    }
    case WM_HSCROLL: {
        HWND t = reinterpret_cast<HWND>(lp);
        const int id  = GetDlgCtrlID(t);
        const int pos = (int)SendMessageW(t, TBM_GETPOS, 0, 0);
        wchar_t buf[16]; swprintf(buf, 16, L"%d%%", pos);
        if (id == IDC_DIST_GAIN) {
            engine_.setParam(CbParam::DistGain, pos / 100.0f);
            SetWindowTextW(GetDlgItem(dlg, IDC_DIST_GAIN_VALUE), buf);
        } else if (id == IDC_DIST_TONE) {
            engine_.setParam(CbParam::DistTone, pos / 100.0f);
            SetWindowTextW(GetDlgItem(dlg, IDC_DIST_TONE_VALUE), buf);
        } else if (id == IDC_DIST_LEVEL) {
            engine_.setParam(CbParam::DistLevel, pos / 100.0f);
            SetWindowTextW(GetDlgItem(dlg, IDC_DIST_LEVEL_VALUE), buf);
        } else if (id == IDC_FX_COMP_THRESH) {
            engine_.setParam(CbParam::CompThreshold, pos / 100.0f);
            SetWindowTextW(GetDlgItem(dlg, IDC_FX_COMP_THRESH_VALUE), buf);
        } else if (id == IDC_FX_COMP_DEPTH) {
            engine_.setParam(CbParam::CompDepth, pos / 100.0f);
            setAmountLabel(dlg, IDC_FX_COMP_DEPTH_VALUE, pos);
            refreshDistLabels(dlg);
        } else if (id == IDC_FX_RING_HZ) {
            const double hz = ringHzFromPos(pos);
            engine_.setParam(CbParam::RingHz, (float)hz);
            wchar_t hzBuf[24]; formatRingHz(hz, hzBuf, 24);
            SetWindowTextW(GetDlgItem(dlg, IDC_FX_RING_HZ_VALUE), hzBuf);
        } else if (id == IDC_FX_RING_AMOUNT) {
            engine_.setParam(CbParam::RingAmount, pos / 100.0f);
            setAmountLabel(dlg, IDC_FX_RING_AMOUNT_VALUE, pos);
            refreshDistLabels(dlg);
        }
        NotifyWinEvent(EVENT_OBJECT_VALUECHANGE, t, OBJID_CLIENT, CHILDID_SELF);
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_DIST_TYPE && HIWORD(wp) == CBN_SELCHANGE) {
            int t = (int)SendMessageW(GetDlgItem(dlg, IDC_DIST_TYPE), CB_GETCURSEL, 0, 0);
            if (t < 0) t = 0;
            engine_.setParam(CbParam::DistType, (float)t);
            refreshDistLabels(dlg);
            return TRUE;
        }
        if (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL) {
            EndDialog(dlg, LOWORD(wp));
            return TRUE;
        }
        break;
    }
    return FALSE;
}

// Classic ignores gain, tone and level entirely — it is the fixed 2004 clipper.
// Rather than disabling three controls (which announces "unavailable" with no
// discoverable cause), say so in their labels, the same way the PWM LFO depth
// labels do when the oscillator is a saw.
void MainWindow::refreshDistLabels(HWND dlg) {
    const bool classic = (int)(engine_.getParam(CbParam::DistType) + 0.5f) == 0;
    SetWindowTextW(GetDlgItem(dlg, IDC_DIST_GAIN_LABEL),
                   classic ? L"&Gain (no effect - Classic):" : L"&Gain:");
    SetWindowTextW(GetDlgItem(dlg, IDC_DIST_TONE_LABEL),
                   classic ? L"T&one (no effect - Classic):" : L"T&one:");
    SetWindowTextW(GetDlgItem(dlg, IDC_DIST_LEVEL_LABEL),
                   classic ? L"&Level (no effect - Classic):" : L"&Level:");

    // Same treatment for the two knobs that do nothing while their effect is
    // switched off. Threshold with depth at zero, and carrier frequency with
    // amount at zero, are both live controls with no audible result, which is
    // exactly the situation that needs saying out loud rather than greying out.
    const bool noComp = engine_.getParam(CbParam::CompDepth)  <= 0.0f;
    const bool noRing = engine_.getParam(CbParam::RingAmount) <= 0.0f;
    SetWindowTextW(GetDlgItem(dlg, IDC_FX_COMP_THRESH_LABEL),
                   noComp ? L"Th&reshold (no effect - depth off):" : L"Th&reshold:");
    SetWindowTextW(GetDlgItem(dlg, IDC_FX_RING_HZ_LABEL),
                   noRing ? L"&Frequency (no effect - amount off):" : L"&Frequency:");
    NotifyWinEvent(EVENT_OBJECT_NAMECHANGE, GetDlgItem(dlg, IDC_FX_COMP_THRESH_LABEL),
                   OBJID_CLIENT, CHILDID_SELF);
    NotifyWinEvent(EVENT_OBJECT_NAMECHANGE, GetDlgItem(dlg, IDC_FX_RING_HZ_LABEL),
                   OBJID_CLIENT, CHILDID_SELF);
}

void MainWindow::openOscillatorDialog() {
    DialogBoxParamW(hInst_, MAKEINTRESOURCEW(IDD_OSCILLATOR), hwnd_,
                    &MainWindow::oscDialogProc,
                    reinterpret_cast<LPARAM>(this));
}

void MainWindow::resetTo303() {
    // Factory-reset every engine parameter to the clean-303 defaults. Does
    // not touch patterns, sweeps, or transport state — only sound params.
    engine_.setParam(CbParam::Volume,        1.0f);
    engine_.setParam(CbParam::Waveform,      1.0f);   // square
    engine_.setParam(CbParam::Tuning,        0.0f);
    engine_.setParam(CbParam::Cutoff,        0.15f);
    engine_.setParam(CbParam::Resonance,     1.0f);
    engine_.setParam(CbParam::EnvMod,        0.10f);
    engine_.setParam(CbParam::Decay,         0.90f);
    engine_.setParam(CbParam::Accent,        1.0f);
    engine_.setParam(CbParam::Tempo,         140.0f);
    engine_.setParam(CbParam::AccentSquelch, 1.0f);
    engine_.setParam(CbParam::PWM,           0.5f);
    engine_.setParam(CbParam::SubOscLevel,   0.0f);
    engine_.setParam(CbParam::DupletWaveform,1.0f);
    engine_.setParam(CbParam::DupletPWM,     0.5f);
    engine_.setParam(CbParam::DupletDetune,  0.0f);
    engine_.setParam(CbParam::DupletLevel,   0.0f);
    engine_.setParam(CbParam::PwmLfoRate,        0.0f);   // Hz; 0 = LFO off
    engine_.setParam(CbParam::PwmLfoDepth,       0.0f);
    engine_.setParam(CbParam::PwmLfoDupletDepth, 0.0f);
    engine_.setParam(CbParam::NoiseLevel,        0.0f);
    engine_.setParam(CbParam::ResoBoost,         0.0f);
    // Back to Classic, the stock 2004 clipper. Gain/tone/level are inert
    // while Classic is selected, but they are reset anyway so the dialog
    // shows neutral values rather than whatever the last pedal was left at.
    engine_.setParam(CbParam::DistType,  0.0f);
    engine_.setParam(CbParam::DistGain,  0.0f);
    engine_.setParam(CbParam::DistTone,  0.5f);
    engine_.setParam(CbParam::DistLevel, 1.0f);
    engine_.setParam(CbParam::CompThreshold, 0.5f);
    engine_.setParam(CbParam::CompDepth,     0.0f);   // compressor off
    engine_.setParam(CbParam::RingHz,      100.0f);   // Hz
    engine_.setParam(CbParam::RingAmount,    0.0f);   // ring modulator off
    // Deliberately does NOT touch per-step reverse - that is pattern
    // data, and a sound-parameter reset must not silently edit patterns.
    syncControlsFromEngine();
    announceValue(hStatus_, "Everybody needs a 303 \u2014 engine reset to defaults.");
}

void MainWindow::openSweepEditor(int sliderIdx) {
    if (sliderIdx < 0 || sliderIdx >= 7) return;
    editingSweepParam_ = sliderIdx;
    editingSweepIndex_ = 0;
    DialogBoxParamW(hInst_, MAKEINTRESOURCEW(IDD_SWEEP_EDIT), hwnd_,
                    &MainWindow::sweepEditorProc,
                    reinterpret_cast<LPARAM>(this));

    // Push the UI's sweep list into the engine so it modulates params live.
    std::vector<CBEngine::ParamSweep> engineSweeps;
    engineSweeps.reserve(paramSweeps_[sliderIdx].size());
    for (const auto& s : paramSweeps_[sliderIdx]) {
        CBEngine::ParamSweep es;
        es.startPat = s.startPattern; es.startStep = s.startStep; es.startVal = s.startValue;
        es.endPat   = s.endPattern;   es.endStep   = s.endStep;   es.endVal   = s.endValue;
        engineSweeps.push_back(es);
    }
    engine_.setParamSweeps(sliderIdx, engineSweeps);

    char sb[128];
    _snprintf_s(sb, _TRUNCATE, "Sweep editor closed for %s. %zu sweep(s) stored.",
        fromWide(kSliderNames[sliderIdx]).c_str(), paramSweeps_[sliderIdx].size());
    announceValue(hStatus_, sb);
}

void MainWindow::refreshPatternLabel() {
    clampEditingIdx();
    int total = engine_.patternCount();
    int editing = editingIdx_ + 1;

    // Step-sequencer heading.
    if (hStepListLabel_) {
        wchar_t buf[256];
        swprintf(buf, 256,
            L"Step sequencer — Editing pattern %d of %d  (pick a step, then set its note / slide / accent. Ctrl+Left/Right switches pattern.)",
            editing, total);
        SetWindowTextW(hStepListLabel_, buf);
        NotifyWinEvent(EVENT_OBJECT_NAMECHANGE, hStepListLabel_, OBJID_CLIENT, CHILDID_SELF);
    }

    // Title bar.
    if (hwnd_) {
        wchar_t title[160];
        swprintf(title, 160, L"Creakbox — editing pattern %d of %d", editing, total);
        SetWindowTextW(hwnd_, title);
    }

    // Pattern-selector combobox.
    if (hPatternCombo_) {
        SendMessageW(hPatternCombo_, WM_SETREDRAW, FALSE, 0);
        SendMessageW(hPatternCombo_, CB_RESETCONTENT, 0, 0);
        for (int i = 0; i < total; ++i) {
            wchar_t item[32]; swprintf(item, 32, L"Pattern %d", i + 1);
            SendMessageW(hPatternCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item));
        }
        SendMessageW(hPatternCombo_, CB_SETCURSEL, editingIdx_, 0);
        SendMessageW(hPatternCombo_, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(hPatternCombo_, nullptr, FALSE);
    }
}

std::vector<int> MainWindow::patternLengths() const {
    std::vector<int> lens;
    const int total = engine_.patternCount();
    lens.reserve(total > 0 ? total : 1);
    for (int i = 0; i < total; ++i) lens.push_back(engine_.getPatternAt(i).lastStep + 1);
    if (lens.empty()) lens.push_back(16);
    return lens;
}

void MainWindow::onPatternLengthChange() {
    if (!hPatternLength_) return;
    const int sel = static_cast<int>(SendMessageW(hPatternLength_, CB_GETCURSEL, 0, 0));
    if (sel < 0 || sel > 63) return;
    CbPattern p = engine_.getPatternAt(editingIdx_);
    if (p.lastStep == sel) return;
    // Shortening does not erase the steps past the new end - setPatternAt
    // copies the whole 64-step array either way - so lengthening again brings
    // them back within the session. Saving does not: .roq only writes the live
    // steps, which is what makes a short pattern a short pattern on disk.
    p.lastStep = sel;
    engine_.setPatternAt(editingIdx_, p);
    if (focusedStep_ > p.lastStep) focusedStep_ = p.lastStep;
    populateStepList();
    const int len = sel + 1;
    char sb[128];
    _snprintf_s(sb, _TRUNCATE, "Pattern %d is now %d step%s.",
                editingIdx_ + 1, len, (len == 1) ? "" : "s");
    announceValue(hStatus_, sb);
}

void MainWindow::toggleCurrentPatternTriplet() {
    CbPattern p = engine_.getPatternAt(editingIdx_);
    p.triplet = !p.triplet;
    engine_.setPatternAt(editingIdx_, p);
    if (hPatternTriplet_)
        SendMessageW(hPatternTriplet_, BM_SETCHECK,
                     p.triplet ? BST_CHECKED : BST_UNCHECKED, 0);
    const int len = p.lastStep + 1;
    // Say what it MEANS, not just that it changed: the whole point of the
    // control is that 12 or 24 steps stop needing tempo arithmetic, and the
    // announcement is the only place that can be pointed out.
    char sb[192];
    if (p.triplet) {
        // A step is a quarter/3, so a 4/4 bar holds twelve of them.
        const int whole = len / 12, rem = len % 12;
        if (rem == 0 && whole >= 1)
            _snprintf_s(sb, _TRUNCATE, "Pattern %d triplet on - %d steps fill %d bar%s.",
                        editingIdx_ + 1, len, whole, whole == 1 ? "" : "s");
        else
            _snprintf_s(sb, _TRUNCATE,
                        "Pattern %d triplet on - %d steps, %d needed to fill a bar.",
                        editingIdx_ + 1, len, 12);
    } else {
        const int whole = len / 16, rem = len % 16;
        if (rem == 0 && whole >= 1)
            _snprintf_s(sb, _TRUNCATE, "Pattern %d triplet off - %d steps fill %d bar%s.",
                        editingIdx_ + 1, len, whole, whole == 1 ? "" : "s");
        else
            _snprintf_s(sb, _TRUNCATE,
                        "Pattern %d triplet off - %d steps, %d needed to fill a bar.",
                        editingIdx_ + 1, len, 16);
    }
    announceValue(hStatus_, sb);
}

void MainWindow::syncControlsFromEngine() {
    // Push engine parameter values back out to the UI trackbars. Called
    // after loading a song so the sliders reflect stored values rather
    // than whatever was on screen before.
    static const CbParam kIds[7] = {
        CbParam::Volume, CbParam::Cutoff, CbParam::Resonance, CbParam::EnvMod,
        CbParam::Decay, CbParam::Accent, CbParam::Tuning
    };
    for (int i = 0; i < 7; ++i) {
        if (!hSliders_[i]) continue;
        int pos = static_cast<int>(engine_.getParam(kIds[i]) * 100.0f);
        SendMessageW(hSliders_[i], TBM_SETPOS, TRUE, static_cast<LPARAM>(pos));
        updateSliderValue(i);
    }
    if (hWaveform_) {
        SendMessageW(hWaveform_, CB_SETCURSEL,
                     engine_.getParam(CbParam::Waveform) > 0.5f ? 1 : 0, 0);
    }
    if (hResoBoost_) {
        SendMessageW(hResoBoost_, BM_SETCHECK,
                     engine_.getParam(CbParam::ResoBoost) > 0.5f ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    if (hAccentSquelch_ && hAccentSquelchValue_) {
        // Inverse of the slider map: sq 1.0..2.0 → slider 0..100.
        float sq = engine_.getParam(CbParam::AccentSquelch);
        if (sq < 1.0f) sq = 1.0f;
        if (sq > 2.0f) sq = 2.0f;
        int pos = static_cast<int>((sq - 1.0f) * 100.0f + 0.5f);
        SendMessageW(hAccentSquelch_, TBM_SETPOS, TRUE, pos);
        wchar_t buf[16]; swprintf(buf, 16, L"%d%%", pos);
        SetWindowTextW(hAccentSquelchValue_, buf);
    }
}

void MainWindow::clampEditingIdx() {
    int total = engine_.patternCount();
    if (total <= 0) { editingIdx_ = 0; return; }
    if (editingIdx_ < 0) editingIdx_ = 0;
    if (editingIdx_ >= total) editingIdx_ = total - 1;
}

void MainWindow::gotoPrevPattern() {
    int total = engine_.patternCount();
    if (total <= 0) return;
    editingIdx_ = (editingIdx_ - 1 + total) % total;
    populateStepList();
    refreshPatternLabel();
    char sb[64]; _snprintf_s(sb, _TRUNCATE, "Editing pattern %d of %d.", editingIdx_ + 1, total);
    announceValue(hStatus_, sb);
}

void MainWindow::gotoNextPattern() {
    int total = engine_.patternCount();
    if (total <= 0) return;
    editingIdx_ = (editingIdx_ + 1) % total;
    populateStepList();
    refreshPatternLabel();
    char sb[64]; _snprintf_s(sb, _TRUNCATE, "Editing pattern %d of %d.", editingIdx_ + 1, total);
    announceValue(hStatus_, sb);
}

void MainWindow::addPatternAfterCurrent() {
    // Engine's insertPatternAfterCurrent uses its own patternIdx_ — align it
    // to the editor's selection so the new slot lands where the user expects.
    engine_.selectPattern(editingIdx_);
    int newIdx = engine_.insertPatternAfterCurrent();
    editingIdx_ = newIdx;
    populateStepList();
    refreshPatternLabel();
    char sb[96]; _snprintf_s(sb, _TRUNCATE, "New empty pattern inserted at position %d of %d.",
                              newIdx + 1, engine_.patternCount());
    announceValue(hStatus_, sb);
}

void MainWindow::clearCurrentPattern() {
    CbPattern p;  // default-constructed = all rests, no slides, no accents
    engine_.setPatternAt(editingIdx_, p);
    populateStepList();
    announceValue(hStatus_, "Current pattern cleared to rests.");
}

// Copy / cut / paste move a WHOLE pattern - its steps, its length and its
// triplet flag - because those three together are what the user means by "this
// pattern". Pasting only the steps would leave a 12-step triplet pattern
// pasted into a 16-step slot sounding like neither.
//
// Cut clears in place rather than removing the pattern from the song. That is
// what its position in the Edit menu promises (it sits with "Clear pattern",
// not with Pattern > Delete), and it is also the safe reading: sweeps address
// their endpoints by pattern INDEX, so removing a slot would silently relocate
// every sweep after it. Delete already exists on Ctrl+Shift+Del for when that
// is really wanted.
void MainWindow::copyCurrentPattern() {
    patternClipboard_ = engine_.getPatternAt(editingIdx_);
    patternClipboardValid_ = true;
    char sb[96]; _snprintf_s(sb, _TRUNCATE, "Copied pattern %d, %d step%s%s.",
                              editingIdx_ + 1, patternClipboard_.lastStep + 1,
                              patternClipboard_.lastStep == 0 ? "" : "s",
                              patternClipboard_.triplet ? ", triplet feel" : "");
    announceValue(hStatus_, sb);
}

void MainWindow::cutCurrentPattern() {
    // Deliberately not calling copyCurrentPattern(): two announceValue() calls
    // in a row means the second interrupts the first, and a screen reader then
    // reads half of "Copied..." before jumping to "Cut...". One action, one
    // announcement.
    patternClipboard_ = engine_.getPatternAt(editingIdx_);
    patternClipboardValid_ = true;
    CbPattern p;  // default-constructed = all rests, 16 steps, no triplet
    engine_.setPatternAt(editingIdx_, p);
    populateStepList();
    char sb[96]; _snprintf_s(sb, _TRUNCATE,
                              "Cut pattern %d, %d step%s. It is now empty.",
                              editingIdx_ + 1, patternClipboard_.lastStep + 1,
                              patternClipboard_.lastStep == 0 ? "" : "s");
    announceValue(hStatus_, sb);
}

void MainWindow::pasteCurrentPattern() {
    // Deliberately NOT a disabled menu item when the clipboard is empty: a
    // greyed command announces "unavailable" with no discoverable cause. Say
    // what is missing instead.
    if (!patternClipboardValid_) {
        announceValue(hStatus_, "Nothing to paste. Copy or cut a pattern first.");
        return;
    }
    engine_.setPatternAt(editingIdx_, patternClipboard_);
    populateStepList();
    // Length and triplet are announced explicitly: populateStepList moves the
    // length combo and the triplet checkbox, and a paste that silently changed
    // the bar length is exactly the kind of thing you cannot see coming.
    char sb[112]; _snprintf_s(sb, _TRUNCATE, "Pasted into pattern %d. %d step%s%s.",
                              editingIdx_ + 1, patternClipboard_.lastStep + 1,
                              patternClipboard_.lastStep == 0 ? "" : "s",
                              patternClipboard_.triplet ? ", triplet feel" : ", straight");
    announceValue(hStatus_, sb);
}

static const char* flavorName(CBEngine::RandomFlavor f) {
    using RF = CBEngine::RandomFlavor;
    switch (f) {
        case RF::Nicely:    return "Nicely";
        case RF::Retro:     return "Retro 2002";
        case RF::VST:       return "VST";
        case RF::Chromatic: return "Chromatic chaos";
    }
    return "Nicely";
}

float MainWindow::readWildness() const {
    if (!hWildness_) return 0.5f;
    int pos = static_cast<int>(SendMessageW(hWildness_, TBM_GETPOS, 0, 0));
    return static_cast<float>(pos) / 100.0f;
}

void MainWindow::randomCurrentPattern(CBEngine::RandomFlavor flavor) {
    lastRandomFlavor_ = flavor;
    const float wild = readWildness();
    // Keep the pattern's length: the randomiser replaces its notes, not its
    // shape. Passing the default would silently snap a 12-step pattern to 16.
    engine_.setPatternAt(editingIdx_,
        CBEngine::makeRandomPattern(flavor, wild, engine_.getPatternAt(editingIdx_).lastStep));
    populateStepList();
    char sb[128];
    _snprintf_s(sb, _TRUNCATE, "%s pattern applied (wildness %d%%).",
                flavorName(flavor), static_cast<int>(wild * 100.0f));
    announceValue(hStatus_, sb);
}

void MainWindow::randomFocusedStep() {
    const float wild = readWildness();
    CbPattern pat = engine_.getPatternAt(editingIdx_);
    if (focusedStep_ < 0 || focusedStep_ > pat.lastStep) return;
    pat.steps[focusedStep_] = CBEngine::makeRandomStep(lastRandomFlavor_, wild);
    engine_.setPatternAt(editingIdx_, pat);
    populateStepList();
    char sb[128];
    _snprintf_s(sb, _TRUNCATE, "Step %d randomized (%s, wildness %d%%).",
                focusedStep_ + 1, flavorName(lastRandomFlavor_),
                static_cast<int>(wild * 100.0f));
    announceValue(hStatus_, sb);
}

void MainWindow::newRandomPattern() {
    // Fresh preset: one random pattern replaces everything. Uses the most
    // recently chosen flavor so Ctrl+N after Ctrl+R keeps the Retro palette.
    const float wild = readWildness();
    std::vector<CbPattern> ps{ CBEngine::makeRandomPattern(lastRandomFlavor_, wild) };
    engine_.setPatterns(ps);
    editingIdx_ = 0;
    populateStepList();
    refreshPatternLabel();
    char sb[128];
    _snprintf_s(sb, _TRUNCATE, "New random pattern created (%s).",
                flavorName(lastRandomFlavor_));
    announceValue(hStatus_, sb);
}

void MainWindow::startNewTrack() {
    int resp = MessageBoxW(hwnd_,
        L"Clear all patterns and sweeps, and start a fresh track?",
        L"Creakbox", MB_OKCANCEL | MB_ICONQUESTION);
    if (resp != IDOK) return;

    engine_.setPatterns({ CbPattern{} });
    editingIdx_ = 0;
    for (int k = 0; k < 7; ++k) {
        paramSweeps_[k].clear();
        engine_.setParamSweeps(k, {});
    }
    currentFile_.clear();
    populateStepList();
    refreshPatternLabel();
    announceValue(hStatus_, "New track started \u2014 1 empty pattern.");
}

static void transposePatternInPlace(CbPattern& p, int semitones) {
    // Only the steps the pattern actually plays. Steps past `lastStep` still
    // hold whatever they held before the pattern was shortened, and transposing
    // them would quietly rewrite data the user cannot see.
    for (int s = 0; s <= p.lastStep; ++s) {
        int pitch = p.steps[s].pitch;
        if (pitch <= 0) continue;              // rests stay rests
        int np = pitch + semitones;
        if (np < 1)  np = 1;
        if (np > 60) np = 60;
        p.steps[s].pitch = np;
    }
}

void MainWindow::transposeCurrentPattern(int semitones) {
    CbPattern p = engine_.getPatternAt(editingIdx_);
    transposePatternInPlace(p, semitones);
    engine_.setPatternAt(editingIdx_, p);
    populateStepList();
    char sb[96]; _snprintf_s(sb, _TRUNCATE, "Transposed pattern %d by %+d semitone%s.",
                              editingIdx_ + 1, semitones, (semitones == 1 || semitones == -1) ? "" : "s");
    announceValue(hStatus_, sb);
}

void MainWindow::transposeSong(int semitones) {
    int total = engine_.patternCount();
    for (int i = 0; i < total; ++i) {
        CbPattern p = engine_.getPatternAt(i);
        transposePatternInPlace(p, semitones);
        engine_.setPatternAt(i, p);
    }
    populateStepList();
    char sb[96]; _snprintf_s(sb, _TRUNCATE, "Transposed all %d pattern%s by %+d semitone%s.",
                              total, total == 1 ? "" : "s",
                              semitones, (semitones == 1 || semitones == -1) ? "" : "s");
    announceValue(hStatus_, sb);
}

void MainWindow::toggleFocusedStepReverse() {
    CbPattern p = engine_.getPatternAt(editingIdx_);
    if (focusedStep_ < 0 || focusedStep_ > p.lastStep) return;
    p.steps[focusedStep_].reverseStep = !p.steps[focusedStep_].reverseStep;
    engine_.setPatternAt(editingIdx_, p);
    SendMessageW(hStepReverse_, BM_SETCHECK,
                 p.steps[focusedStep_].reverseStep ? BST_CHECKED : BST_UNCHECKED, 0);
    populateStepList();
    char sb[64]; _snprintf_s(sb, _TRUNCATE, "Step %d reverse %s.", focusedStep_ + 1,
                              p.steps[focusedStep_].reverseStep ? "on" : "off");
    announceValue(hStatus_, sb);
}

void MainWindow::toggleFocusedStepAccent() {
    CbPattern p = engine_.getPatternAt(editingIdx_);
    if (focusedStep_ < 0 || focusedStep_ > p.lastStep) return;
    p.steps[focusedStep_].accent = !p.steps[focusedStep_].accent;
    engine_.setPatternAt(editingIdx_, p);
    SendMessageW(hStepAccent_, BM_SETCHECK,
                 p.steps[focusedStep_].accent ? BST_CHECKED : BST_UNCHECKED, 0);
    populateStepList();
    char sb[64]; _snprintf_s(sb, _TRUNCATE, "Step %d accent %s.", focusedStep_ + 1,
                              p.steps[focusedStep_].accent ? "on" : "off");
    announceValue(hStatus_, sb);
}

void MainWindow::toggleFocusedStepSlide() {
    CbPattern p = engine_.getPatternAt(editingIdx_);
    if (focusedStep_ < 0 || focusedStep_ > p.lastStep) return;
    p.steps[focusedStep_].slide = !p.steps[focusedStep_].slide;
    engine_.setPatternAt(editingIdx_, p);
    SendMessageW(hStepSlide_, BM_SETCHECK,
                 p.steps[focusedStep_].slide ? BST_CHECKED : BST_UNCHECKED, 0);
    populateStepList();
    char sb[64]; _snprintf_s(sb, _TRUNCATE, "Step %d slide %s.", focusedStep_ + 1,
                              p.steps[focusedStep_].slide ? "on" : "off");
    announceValue(hStatus_, sb);
}

// F5. The whole song from the top, regardless of which pattern is being
// edited. Unconditional, not a toggle: pressing it while already playing
// restarts from pattern 1, which is what "play the song" should mean.
void MainWindow::playSong() {
    engine_.setPlayMode(CBEngine::PlayMode::FullSequence);
    engine_.setRunning(true, /*restartFromFirstPattern=*/true);
    SetWindowTextW(hTransport_, L"&Stop (F8)");
    char sb[96]; _snprintf_s(sb, _TRUNCATE,
        "Playing song from pattern 1 through pattern %d.", engine_.patternCount());
    announceValue(hStatus_, sb);
}

// F8. Unconditional stop, so it is safe to press when already stopped.
void MainWindow::stopTransport() {
    engine_.setRunning(false);
    SetWindowTextW(hTransport_, L"&Run (F5)");
    announceValue(hStatus_, "Transport: stopped.");
}

void MainWindow::playCurrentPatternOnly() {
    engine_.setPlayMode(CBEngine::PlayMode::CurrentPatternOnly);
    engine_.selectPattern(editingIdx_);  // playback starts at edited pattern
    engine_.setRunning(true, /*restartFromFirstPattern=*/false);
    SetWindowTextW(hTransport_, L"&Stop (F8)");
    char sb[96]; _snprintf_s(sb, _TRUNCATE, "Looping pattern %d only.", editingIdx_ + 1);
    announceValue(hStatus_, sb);
}

void MainWindow::playSequenceFromCurrent() {
    engine_.setPlayMode(CBEngine::PlayMode::FullSequence);
    engine_.selectPattern(editingIdx_);
    engine_.setRunning(true, /*restartFromFirstPattern=*/false);
    SetWindowTextW(hTransport_, L"&Stop (F8)");
    char sb[128]; _snprintf_s(sb, _TRUNCATE,
        "Playing sequence from pattern %d through pattern %d.",
        editingIdx_ + 1, engine_.patternCount());
    announceValue(hStatus_, sb);
}

void MainWindow::deleteCurrentPattern() {
    int cur = editingIdx_ + 1;
    engine_.selectPattern(editingIdx_);
    if (!engine_.deleteCurrentPattern()) {
        announceValue(hStatus_, "Can't delete — only one pattern remains.");
        return;
    }
    clampEditingIdx();
    populateStepList();
    refreshPatternLabel();
    char sb[96]; _snprintf_s(sb, _TRUNCATE, "Deleted pattern %d. Now editing pattern %d of %d.",
                              cur, editingIdx_ + 1, engine_.patternCount());
    announceValue(hStatus_, sb);
}

void MainWindow::populateStepList() {
    // Refresh both the step-selector listbox items (so each row describes
    // its step's current state for screen readers) and the value controls.
    if (!hStepSelector_) return;
    static const wchar_t* kNoteNames[12] = {
        L"C", L"C#", L"D", L"D#", L"E", L"F", L"F#", L"G", L"G#", L"A", L"A#", L"B"
    };
    CbPattern pat = engine_.getPatternAt(editingIdx_);
    // Shortening a pattern can strand the focus past its end.
    if (focusedStep_ > pat.lastStep) focusedStep_ = pat.lastStep;
    if (focusedStep_ < 0) focusedStep_ = 0;
    const int prevSel = focusedStep_;
    if (hPatternLength_) {
        SendMessageW(hPatternLength_, CB_SETCURSEL, pat.lastStep, 0);
        NotifyWinEvent(EVENT_OBJECT_VALUECHANGE, hPatternLength_, OBJID_CLIENT, CHILDID_SELF);
    }
    if (hPatternTriplet_) {
        SendMessageW(hPatternTriplet_, BM_SETCHECK,
                     pat.triplet ? BST_CHECKED : BST_UNCHECKED, 0);
        NotifyWinEvent(EVENT_OBJECT_VALUECHANGE, hPatternTriplet_, OBJID_CLIENT, CHILDID_SELF);
    }
    SendMessageW(hStepSelector_, WM_SETREDRAW, FALSE, 0);
    SendMessageW(hStepSelector_, LB_RESETCONTENT, 0, 0);
    for (int i = 0; i <= pat.lastStep; ++i) {
        const CbStep& s = pat.steps[i];
        wchar_t noteBuf[12];
        if (s.pitch <= 0) {
            wcscpy_s(noteBuf, L"Rest");
        } else {
            const int n = s.pitch - 1;
            swprintf(noteBuf, 12, L"%s%d", kNoteNames[n % 12], n / 12);
        }
        wchar_t line[64];
        swprintf(line, 64, L"Step %2d: %-5s%s%s%s",
                 i + 1, noteBuf,
                 s.slide  ? L" slide"  : L"",
                 s.accent ? L" accent" : L"",
                 s.reverseStep ? L" reverse" : L"");
        SendMessageW(hStepSelector_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(line));
    }
    SendMessageW(hStepSelector_, LB_SETCURSEL, prevSel, 0);
    SendMessageW(hStepSelector_, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(hStepSelector_, nullptr, TRUE);
    // Make NVDA re-read the currently-selected item so users notice the
    // text changed after an edit.
    NotifyWinEvent(EVENT_OBJECT_NAMECHANGE, hStepSelector_, OBJID_CLIENT, CHILDID_SELF);
    loadFocusedStepIntoEditor();
}

void MainWindow::loadFocusedStepIntoEditor() {
    if (!hStepPitch_) return;
    CbPattern pat = engine_.getPatternAt(editingIdx_);
    if (focusedStep_ < 0) focusedStep_ = 0;
    if (focusedStep_ > pat.lastStep) focusedStep_ = pat.lastStep;
    const CbStep& s = pat.steps[focusedStep_];
    int idx = s.pitch; if (idx < 0) idx = 0; if (idx > 60) idx = 60;
    SendMessageW(hStepPitch_,  CB_SETCURSEL, idx, 0);
    SendMessageW(hStepSlide_,  BM_SETCHECK, s.slide  ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(hStepAccent_, BM_SETCHECK, s.accent ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(hStepReverse_, BM_SETCHECK, s.reverseStep ? BST_CHECKED : BST_UNCHECKED, 0);
    if (hStepSelector_) SendMessageW(hStepSelector_, LB_SETCURSEL, focusedStep_, 0);
}

void MainWindow::writeFocusedStepFromEditor() {
    if (!hStepPitch_) return;
    CbPattern pat = engine_.getPatternAt(editingIdx_);
    const int pitchIdx = static_cast<int>(SendMessageW(hStepPitch_, CB_GETCURSEL, 0, 0));
    pat.steps[focusedStep_].pitch  = (pitchIdx == CB_ERR) ? 0 : pitchIdx;
    pat.steps[focusedStep_].slide  = SendMessageW(hStepSlide_,  BM_GETCHECK, 0, 0) == BST_CHECKED;
    pat.steps[focusedStep_].accent = SendMessageW(hStepAccent_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    pat.steps[focusedStep_].reverseStep = SendMessageW(hStepReverse_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    engine_.setPatternAt(editingIdx_, pat);
    // Update the corresponding listbox row so its description reflects the
    // new state. Delete + re-insert at the same index because ListBox has
    // no API to edit an existing item's text in place.
    if (hStepSelector_) {
        static const wchar_t* kNoteNames[12] = {
            L"C", L"C#", L"D", L"D#", L"E", L"F", L"F#", L"G", L"G#", L"A", L"A#", L"B"
        };
        const CbStep& s = pat.steps[focusedStep_];
        wchar_t noteBuf[12];
        if (s.pitch <= 0) {
            wcscpy_s(noteBuf, L"Rest");
        } else {
            const int n = s.pitch - 1;
            swprintf(noteBuf, 12, L"%s%d", kNoteNames[n % 12], n / 12);
        }
        wchar_t line[64];
        swprintf(line, 64, L"Step %2d: %-5s%s%s%s",
                 focusedStep_ + 1, noteBuf,
                 s.slide  ? L" slide"  : L"",
                 s.accent ? L" accent" : L"",
                 s.reverseStep ? L" reverse" : L"");
        SendMessageW(hStepSelector_, WM_SETREDRAW, FALSE, 0);
        SendMessageW(hStepSelector_, LB_DELETESTRING, focusedStep_, 0);
        SendMessageW(hStepSelector_, LB_INSERTSTRING, focusedStep_, reinterpret_cast<LPARAM>(line));
        SendMessageW(hStepSelector_, LB_SETCURSEL, focusedStep_, 0);
        SendMessageW(hStepSelector_, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(hStepSelector_, nullptr, FALSE);
    }
}

void MainWindow::onStepSelectorChange() {
    int sel = static_cast<int>(SendMessageW(hStepSelector_, LB_GETCURSEL, 0, 0));
    // The listbox only ever holds the pattern's live steps, so `sel` is already
    // in range; the guard is against CB_ERR and a stale selection after a resize.
    if (sel < 0 || sel >= 64) return;
    focusedStep_ = sel;
    loadFocusedStepIntoEditor();
}

void MainWindow::updateSliderValue(int idx) {
    if (idx < 0 || idx >= 7 || !hSliders_[idx]) return;
    static const CbParam kParamIds[7] = {
        CbParam::Volume, CbParam::Cutoff, CbParam::Resonance, CbParam::EnvMod,
        CbParam::Decay, CbParam::Accent, CbParam::Tuning
    };
    const int pos = static_cast<int>(SendMessageW(hSliders_[idx], TBM_GETPOS, 0, 0));
    const float v = pos / 100.0f;
    engine_.setParam(kParamIds[idx], v);
    wchar_t buf[32];
    if (pos >= 0) swprintf(buf, 32, L"+%d%%", pos);
    else          swprintf(buf, 32, L"%d%%", pos);  // "-100%"
    SetWindowTextW(hSliderValues_[idx], buf);
    NotifyWinEvent(EVENT_OBJECT_VALUECHANGE, hSliders_[idx], OBJID_CLIENT, CHILDID_SELF);
}

void MainWindow::onSliderScroll(HWND slider) {
    for (int i = 0; i < 7; ++i) {
        if (hSliders_[i] == slider) { updateSliderValue(i); return; }
    }
    if (slider == hWildness_ && hWildnessValue_) {
        int pos = static_cast<int>(SendMessageW(hWildness_, TBM_GETPOS, 0, 0));
        wchar_t buf[16]; swprintf(buf, 16, L"%d%%", pos);
        SetWindowTextW(hWildnessValue_, buf);
        NotifyWinEvent(EVENT_OBJECT_VALUECHANGE, hWildness_, OBJID_CLIENT, CHILDID_SELF);
    }
    if (slider == hAccentSquelch_ && hAccentSquelchValue_) {
        int pos = static_cast<int>(SendMessageW(hAccentSquelch_, TBM_GETPOS, 0, 0));
        // Slider 0..100 → sq 1.0..2.0. 0 is Frank's original TB-303 accent
        // behavior; 100 doubles the squelch buildup on successive accents.
        float sq = 1.0f + pos * 0.01f;
        engine_.setParam(CbParam::AccentSquelch, sq);
        wchar_t buf[16]; swprintf(buf, 16, L"%d%%", pos);
        SetWindowTextW(hAccentSquelchValue_, buf);
        NotifyWinEvent(EVENT_OBJECT_VALUECHANGE, hAccentSquelch_, OBJID_CLIENT, CHILDID_SELF);
    }
}

void MainWindow::announceValue(HWND control, const std::string& text) {
    auto w = toWide(text);
    SetWindowTextW(control, w.c_str());
    NotifyWinEvent(EVENT_OBJECT_NAMECHANGE, control, OBJID_CLIENT, CHILDID_SELF);
    NotifyWinEvent(EVENT_OBJECT_VALUECHANGE, control, OBJID_CLIENT, CHILDID_SELF);
}

void MainWindow::toggleTransport() {
    bool running = !engine_.isRunning();
    if (running) {
        // The toggle always restarts from pattern 1 in full-sequence mode,
        // i.e. it is the button-shaped equivalent of F5.
        engine_.setPlayMode(CBEngine::PlayMode::FullSequence);
        engine_.setRunning(true, /*restartFromFirstPattern=*/true);
    } else {
        engine_.setRunning(false);
    }
    SetWindowTextW(hTransport_, running ? L"&Stop (F8)" : L"&Run (F5)");
    announceValue(hStatus_, running ? "Transport: running." : "Transport: stopped.");
}

void MainWindow::loadCbFile() {
    wchar_t path[MAX_PATH]{};
    OPENFILENAMEW ofn{ sizeof(ofn) };
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFilter = L"CreakBox module (*.cb)\0*.cb\0All files\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&ofn)) return;

    std::string err;
    auto preset = loadCb(fromWide(path), err);
    if (!preset) {
        MessageBoxW(hwnd_, toWide("Failed to load: " + err).c_str(), L"Creakbox", MB_OK | MB_ICONERROR);
        return;
    }
    if (!preset->patterns.empty()) {
        engine_.setPatterns(preset->patterns);
    }
    editingIdx_ = 0;
    engine_.setParam(CbParam::Tempo, preset->tempo);
    // Apply the sweeps — decoded via Ghidra of CREAKBOX.EXE.
    CBEngine::Sweep sweeps[7];
    for (int i = 0; i < 7; ++i) {
        sweeps[i].order0 = preset->bp0[i].order;
        sweeps[i].off0   = preset->bp0[i].offset;
        sweeps[i].val0   = preset->bp0[i].value;
        sweeps[i].order1 = preset->bp1[i].order;
        sweeps[i].off1   = preset->bp1[i].offset;
        sweeps[i].val1   = preset->bp1[i].value;
    }
    // Sweeps preserved on the preset for round-trip fidelity but OFF during
    // playback so the UI sliders stay authoritative. Future work: apply
    // sweeps as a *modulation* on top of slider values rather than as a
    // replacement, or add a per-tab "enable" toggle so the user can decide.
    engine_.setSweeps(sweeps, /*enabled=*/false);

    wchar_t tbuf[16]; swprintf(tbuf, 16, L"%d", static_cast<int>(preset->tempo));
    SetWindowTextW(hTempo_, tbuf);
    currentFile_ = path;
    populateStepList();
    refreshPatternLabel();
    char sb[256];
    _snprintf_s(sb, _TRUNCATE, "Loaded %s (%zu patterns, tempo %d).",
        fromWide(PathFindFileNameW(path)).c_str(),
        preset->patterns.size(), static_cast<int>(preset->tempo));
    announceValue(hStatus_, sb);
}


// Write a 16-bit mono PCM WAV to `path`. Input is the engine's stereo
// interleaved float buffer; since both channels always carry identical
// samples, we write the left channel only — mono is half the file size
// with no audible difference.
static bool writeWavFile16(const std::wstring& path, const std::vector<float>& interleaved,
                            int sampleRate, std::string& err) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { err = "cannot create file"; return false; }
    const uint16_t channels = 1;
    const uint16_t bitsPerSample = 16;
    const uint32_t byteRate = sampleRate * channels * (bitsPerSample / 8);
    const uint16_t blockAlign = channels * (bitsPerSample / 8);
    const size_t   monoSamples = interleaved.size() / 2;
    const uint32_t dataBytes = static_cast<uint32_t>(monoSamples * sizeof(int16_t));
    const uint32_t fileBytes = 36 + dataBytes;
    DWORD written = 0;
    auto w = [&](const void* p, DWORD n) { WriteFile(h, p, n, &written, nullptr); };
    w("RIFF", 4); w(&fileBytes, 4); w("WAVE", 4);
    w("fmt ", 4);
    uint32_t fmtSize = 16; uint16_t fmtTag = 1; // PCM
    w(&fmtSize, 4); w(&fmtTag, 2); w(&channels, 2);
    uint32_t sr = static_cast<uint32_t>(sampleRate);
    w(&sr, 4); w(&byteRate, 4); w(&blockAlign, 2); w(&bitsPerSample, 2);
    w("data", 4); w(&dataBytes, 4);
    // Convert floats to int16 and write in chunks to keep memory bounded.
    // Take the left channel only (index i*2); right is identical.
    std::vector<int16_t> chunk;
    chunk.reserve(4096);
    for (size_t i = 0; i < monoSamples; ++i) {
        float v = interleaved[i * 2];
        if (v > 1.0f) v = 1.0f;
        if (v < -1.0f) v = -1.0f;
        chunk.push_back(static_cast<int16_t>(v * 32767.0f));
        if (chunk.size() >= 4096) {
            w(chunk.data(), static_cast<DWORD>(chunk.size() * sizeof(int16_t)));
            chunk.clear();
        }
    }
    if (!chunk.empty()) w(chunk.data(), static_cast<DWORD>(chunk.size() * sizeof(int16_t)));
    CloseHandle(h);
    return true;
}

void MainWindow::saveWavFile() {
    wchar_t path[MAX_PATH]{ L'c', ':', '\\', 'c', 'b', '3', '0', '3', '.', 'w', 'a', 'v', 0 };
    OPENFILENAMEW ofn{ sizeof(ofn) };
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFilter = L"Wave audio (*.wav)\0*.wav\0";
    ofn.lpstrDefExt = L"wav";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&ofn)) return;

    // Render the full sequence once through at the current audio sample rate.
    // Use a detached CBEngine so the main realtime engine keeps playing.
    CBEngine renderEngine;
    renderEngine.setSampleRate(audio_.sampleRate());
    renderEngine.setPatterns({engine_.getPatternAt(editingIdx_)});  // seed; overwritten below
    // Actually copy all patterns from main engine:
    std::vector<CbPattern> pats;
    for (int i = 0; i < engine_.patternCount(); ++i) pats.push_back(engine_.getPatternAt(i));
    renderEngine.setPatterns(pats);
    // Copy slider parameter values
    for (int p = 0; p < int(CbParam::NumParams); ++p)
        renderEngine.setParam(static_cast<CbParam>(p), engine_.getParam(static_cast<CbParam>(p)));

    // Length: play the full song order once through.
    const float bpm = engine_.getParam(CbParam::Tempo);
    const int samplesPerStep = static_cast<int>((audio_.sampleRate() * 60.0) / (bpm * 4.0));
    const int totalSamples = samplesPerStep * 16 * engine_.patternCount();
    std::vector<float> buf(totalSamples * 2, 0.0f);

    renderEngine.setPlayMode(CBEngine::PlayMode::FullSequence);
    renderEngine.setRunning(true, /*restartFromFirstPattern=*/true);
    // Render in blocks of 512 samples
    const int blockSize = 512;
    int pos = 0;
    while (pos < totalSamples) {
        int n = ((blockSize) < (totalSamples - pos) ? (blockSize) : (totalSamples - pos));
        renderEngine.process(buf.data() + pos * 2, n);
        pos += n;
    }
    renderEngine.setRunning(false);

    std::string err;
    if (!writeWavFile16(path, buf, static_cast<int>(audio_.sampleRate()), err)) {
        MessageBoxW(hwnd_, toWide("WAV save failed: " + err).c_str(),
                    L"Creakbox", MB_OK | MB_ICONERROR);
        return;
    }
    char sb[256];
    _snprintf_s(sb, _TRUNCATE, "Rendered %d samples (%0.1f sec) to %s.",
        totalSamples, totalSamples / audio_.sampleRate(),
        fromWide(PathFindFileNameW(path)).c_str());
    announceValue(hStatus_, sb);
}

void MainWindow::saveCbFile() {
    wchar_t path[MAX_PATH]{};
    if (!currentFile_.empty()) wcscpy_s(path, currentFile_.c_str());
    OPENFILENAMEW ofn{ sizeof(ofn) };
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFilter = L"CreakBox module (*.cb)\0*.cb\0";
    ofn.lpstrDefExt = L"cb";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&ofn)) return;

    CbPreset p;
    p.tempo = engine_.getParam(CbParam::Tempo);
    p.patterns.push_back(engine_.getPatternAt(editingIdx_));
    p.trailingPatternMetadata.push_back(0.0f);
    for (int t = 0; t < 7; ++t) {
        p.bp0[t] = CbBreakpoint{0, 0, 100};
        p.bp1[t] = CbBreakpoint{9999, 0, 100};
    }

    std::string err;
    if (!saveCb(fromWide(path), p, err)) {
        MessageBoxW(hwnd_, toWide("Save failed: " + err).c_str(), L"Creakbox", MB_OK | MB_ICONERROR);
        return;
    }
    currentFile_ = path;
    announceValue(hStatus_, "Saved.");
}

void MainWindow::loadRoqFile() {
    wchar_t path[MAX_PATH]{};
    OPENFILENAMEW ofn{ sizeof(ofn) };
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFilter = L"Creakbox song (*.roq)\0*.roq\0All files\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&ofn)) return;

    std::string err;
    auto song = loadRoq(fromWide(path), err);
    if (!song) {
        MessageBoxW(hwnd_, toWide("Failed to load: " + err).c_str(), L"Creakbox", MB_OK | MB_ICONERROR);
        return;
    }

    if (!song->patterns.empty()) engine_.setPatterns(song->patterns);
    editingIdx_ = 0;
    engine_.setParam(CbParam::Volume,    song->volume);
    engine_.setParam(CbParam::Waveform,  song->waveform);
    engine_.setParam(CbParam::Tuning,    song->tuning);
    engine_.setParam(CbParam::Cutoff,    song->cutoff);
    engine_.setParam(CbParam::Resonance, song->resonance);
    engine_.setParam(CbParam::EnvMod,    song->envmod);
    engine_.setParam(CbParam::Decay,     song->decay);
    engine_.setParam(CbParam::Accent,    song->accent);
    engine_.setParam(CbParam::Tempo,     song->tempo);
    engine_.setParam(CbParam::AccentSquelch, song->accentSquelch);
    engine_.setParam(CbParam::PWM,             song->pwm);
    engine_.setParam(CbParam::SubOscLevel,     song->subOscLevel);
    engine_.setParam(CbParam::DupletWaveform,  song->dupletWaveform);
    engine_.setParam(CbParam::DupletPWM,        song->dupletPWM);
    engine_.setParam(CbParam::DupletDetune,    song->dupletDetune);
    engine_.setParam(CbParam::DupletLevel,     song->dupletLevel);
    engine_.setParam(CbParam::PwmLfoRate,        song->pwmLfoRate);
    engine_.setParam(CbParam::PwmLfoDepth,       song->pwmLfoDepth);
    engine_.setParam(CbParam::PwmLfoDupletDepth, song->pwmLfoDupletDepth);
    engine_.setParam(CbParam::NoiseLevel,        song->noiseLevel);
    engine_.setParam(CbParam::ResoBoost,         song->resoBoost);
    engine_.setParam(CbParam::DistType,          song->distType);
    engine_.setParam(CbParam::DistGain,          song->distGain);
    engine_.setParam(CbParam::DistTone,          song->distTone);
    engine_.setParam(CbParam::DistLevel,         song->distLevel);
    engine_.setParam(CbParam::CompThreshold, song->compThreshold);
    engine_.setParam(CbParam::CompDepth,     song->compDepth);
    engine_.setParam(CbParam::RingHz,        song->ringHz);
    engine_.setParam(CbParam::RingAmount,    song->ringAmount);
    // song->impulsive is a deprecated no-op field — read from old .roq files
    // but no longer applied (the engine always uses Frank's TB-303 buildup).

    // Distribute sweeps back into per-slider buckets and push to engine.
    for (int k = 0; k < 7; ++k) paramSweeps_[k].clear();
    for (const auto& sw : song->sweeps) {
        if (sw.sliderIdx < 0 || sw.sliderIdx >= 7) continue;
        Sweep ui;
        ui.startPattern = sw.startPat; ui.startStep = sw.startStep; ui.startValue = sw.startVal;
        ui.endPattern   = sw.endPat;   ui.endStep   = sw.endStep;   ui.endValue   = sw.endVal;
        paramSweeps_[sw.sliderIdx].push_back(ui);
    }
    for (int k = 0; k < 7; ++k) {
        std::vector<CBEngine::ParamSweep> es;
        es.reserve(paramSweeps_[k].size());
        for (const auto& s : paramSweeps_[k]) {
            CBEngine::ParamSweep e;
            e.startPat = s.startPattern; e.startStep = s.startStep; e.startVal = s.startValue;
            e.endPat   = s.endPattern;   e.endStep   = s.endStep;   e.endVal   = s.endValue;
            es.push_back(e);
        }
        engine_.setParamSweeps(k, es);
    }

    wchar_t tbuf[16]; swprintf(tbuf, 16, L"%d", static_cast<int>(song->tempo));
    SetWindowTextW(hTempo_, tbuf);
    currentFile_ = path;
    syncControlsFromEngine();
    populateStepList();
    refreshPatternLabel();

    char sb[256];
    _snprintf_s(sb, _TRUNCATE, "Loaded %s (%zu patterns, %zu sweeps, tempo %d).",
        fromWide(PathFindFileNameW(path)).c_str(),
        song->patterns.size(), song->sweeps.size(), static_cast<int>(song->tempo));
    announceValue(hStatus_, sb);
}

void MainWindow::saveRoqFile() {
    wchar_t path[MAX_PATH]{};
    if (!currentFile_.empty()) {
        // Reuse current file only if it's already a .roq; .cb stays untouched.
        const wchar_t* ext = PathFindExtensionW(currentFile_.c_str());
        if (ext && _wcsicmp(ext, L".roq") == 0) wcscpy_s(path, currentFile_.c_str());
    }
    OPENFILENAMEW ofn{ sizeof(ofn) };
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFilter = L"Creakbox song (*.roq)\0*.roq\0";
    ofn.lpstrDefExt = L"roq";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&ofn)) return;

    RoqSong song;
    song.volume    = engine_.getParam(CbParam::Volume);
    song.waveform  = engine_.getParam(CbParam::Waveform);
    song.tuning    = engine_.getParam(CbParam::Tuning);
    song.cutoff    = engine_.getParam(CbParam::Cutoff);
    song.resonance = engine_.getParam(CbParam::Resonance);
    song.envmod    = engine_.getParam(CbParam::EnvMod);
    song.decay     = engine_.getParam(CbParam::Decay);
    song.accent    = engine_.getParam(CbParam::Accent);
    song.tempo     = engine_.getParam(CbParam::Tempo);
    song.accentSquelch = engine_.getParam(CbParam::AccentSquelch);
    song.pwm            = engine_.getParam(CbParam::PWM);
    song.subOscLevel    = engine_.getParam(CbParam::SubOscLevel);
    song.dupletWaveform = engine_.getParam(CbParam::DupletWaveform);
    song.dupletPWM      = engine_.getParam(CbParam::DupletPWM);
    song.dupletDetune   = engine_.getParam(CbParam::DupletDetune);
    song.dupletLevel    = engine_.getParam(CbParam::DupletLevel);
    song.pwmLfoRate        = engine_.getParam(CbParam::PwmLfoRate);
    song.pwmLfoDepth       = engine_.getParam(CbParam::PwmLfoDepth);
    song.pwmLfoDupletDepth = engine_.getParam(CbParam::PwmLfoDupletDepth);
    song.noiseLevel        = engine_.getParam(CbParam::NoiseLevel);
    song.resoBoost         = engine_.getParam(CbParam::ResoBoost);
    song.distType          = engine_.getParam(CbParam::DistType);
    song.distGain          = engine_.getParam(CbParam::DistGain);
    song.distTone          = engine_.getParam(CbParam::DistTone);
    song.distLevel         = engine_.getParam(CbParam::DistLevel);
    song.compThreshold = engine_.getParam(CbParam::CompThreshold);
    song.compDepth     = engine_.getParam(CbParam::CompDepth);
    song.ringHz        = engine_.getParam(CbParam::RingHz);
    song.ringAmount    = engine_.getParam(CbParam::RingAmount);
    // song.impulsive stays at its default 0 — deprecated field, written for
    // v2 schema compatibility but no longer meaningful.

    int n = engine_.patternCount();
    song.patterns.reserve(n);
    for (int i = 0; i < n; ++i) song.patterns.push_back(engine_.getPatternAt(i));

    for (int k = 0; k < 7; ++k) {
        for (const auto& s : paramSweeps_[k]) {
            RoqSweep sw;
            sw.sliderIdx = k;
            sw.startPat = s.startPattern; sw.startStep = s.startStep; sw.startVal = s.startValue;
            sw.endPat   = s.endPattern;   sw.endStep   = s.endStep;   sw.endVal   = s.endValue;
            song.sweeps.push_back(sw);
        }
    }

    std::string err;
    if (!saveRoq(fromWide(path), song, err)) {
        MessageBoxW(hwnd_, toWide("Save failed: " + err).c_str(), L"Creakbox", MB_OK | MB_ICONERROR);
        return;
    }
    currentFile_ = path;
    char sb[256];
    _snprintf_s(sb, _TRUNCATE, "Saved %s (%d patterns, %zu sweeps).",
        fromWide(PathFindFileNameW(path)).c_str(), n, song.sweeps.size());
    announceValue(hStatus_, sb);
}

void MainWindow::showKeyboardReference() {
    MessageBoxW(hwnd_,
        L"Transport:\r\n"
        L"   F5: Play current song (from pattern 1)\r\n"
        L"   F6: Play current pattern, looped\r\n"
        L"   F7: Play from current pattern to the end\r\n"
        L"   F8: Stop\r\n"
        L"\r\nGlobal:\r\n"
        L"   Ctrl+O: Open .roq song\r\n"
        L"   Ctrl+S: Save .roq song\r\n"
        L"   Ctrl+D (or Alt+X): Acid FX - distortion, compressor, ring mod\r\n"
        L"   Ctrl+I: Import legacy .cb module\r\n"
        L"   Ctrl+Shift+N: Start new track (blank slate, prompts for confirmation)\r\n"
        L"   Ctrl+C/X/V: Copy / Cut / Paste pattern\r\n"
        L"   Ctrl+Left / Ctrl+Right: Pattern -1 / +1\r\n"
        L"   Ctrl+Shift+P: New pattern after the current one\r\n"
        L"   Ctrl+Shift+Del: Delete current pattern\r\n"
        L"   Ctrl+E: Random pattern — Nicely (C-minor pentatonic)\r\n"
        L"   Ctrl+R: Random pattern — Retro 2002 (Eb blues)\r\n"
        L"   Ctrl+Shift+R: Random pattern — VST (C-major pentatonic)\r\n"
        L"   Ctrl+Shift+E: Random pattern — Chromatic chaos\r\n"
        L"   Ctrl+Shift+G: Random focused step (uses last-chosen flavor)\r\n"
        L"   Ctrl+Up / Ctrl+Down: Transpose current pattern ±1 semitone\r\n"
        L"   Ctrl+Shift+Up / Down: Transpose current pattern ±1 octave\r\n"
        L"   Ctrl+Shift+A / S / I: Toggle accent / slide / reverse on the\r\n"
        L"      focused step, from anywhere (reverse = filter envelope backwards)\r\n"
        L"   Alt+B: Boost resonance on / off (mnemonic - main window only)\r\n"
        L"   Alt+<underlined letter>: jump to any labelled control\r\n"
        L"   F1: This reference\r\n"
        L"\r\nStep sequencer (inline controls per step):\r\n"
        L"   Tab / Shift+Tab: move between controls\r\n"
        L"   Pitch combobox: Up/Down to pick note, type first letter to jump\r\n"
        L"   Alt+G: Pattern length (1-64 steps, this pattern only)\r\n"
        L"   Ctrl+T: Triplet feel for this pattern (12 steps = one bar)\r\n"
        L"   Space or Enter: toggle focused checkbox (slide / accent / reverse)\r\n"
        L"\r\nSliders: arrow keys ±1, Page Up/Down ±10, Home/End min/max,\r\n"
        L"   Enter opens the sweep editor for that slider.\r\n"
        L"Bipolar sliders (Rezo, EnvMod, Tuning) accept negative values.\r\n"
        L"Screen readers announce every control's value on change.",
        L"Keyboard reference",
        MB_OK | MB_ICONINFORMATION);
}

void MainWindow::showAbout() {
    MessageBoxW(hwnd_,
        L"Creakbox\r\n"
        L"Accessible TB-303 style bassline synth.\r\n\r\n"
        L"Original DSP: Frank Reitz (a.k.a. Bass Assessor), 2002-2004.\r\n"
        L"Published as open source by Bioroid Media Development under BSD-2-Clause.\r\n"
        L"Accessible standalone shell: 2026.",
        L"About Creakbox",
        MB_OK | MB_ICONINFORMATION);
}

void MainWindow::onCommand(WPARAM wp) {
    const WORD id = LOWORD(wp);
    const WORD notif = HIWORD(wp);
    switch (id) {
        case IDM_FILE_NEW:        newRandomPattern(); break;
        case IDM_FILE_NEW_TRACK:  startNewTrack(); break;
        case IDM_FILE_OPEN_CB:    loadCbFile(); break;
        case IDM_FILE_SAVE_CB:    saveCbFile(); break;
        case IDM_FILE_OPEN_ROQ:   loadRoqFile(); break;
        case IDM_FILE_SAVE_ROQ:   saveRoqFile(); break;
        case IDM_FILE_SAVE_WAV:   saveWavFile(); break;
        case IDM_FILE_EXIT:       DestroyWindow(hwnd_); break;
        case IDM_EDIT_CLEAR:      clearCurrentPattern(); break;
        case IDM_EDIT_COPY:       copyCurrentPattern();  break;
        case IDM_EDIT_CUT:        cutCurrentPattern();   break;
        case IDM_EDIT_PASTE:      pasteCurrentPattern(); break;
        case IDM_EDIT_RANDOM_NICELY:    randomCurrentPattern(CBEngine::RandomFlavor::Nicely);    break;
        case IDM_EDIT_RANDOM_RETRO:     randomCurrentPattern(CBEngine::RandomFlavor::Retro);     break;
        case IDM_EDIT_RANDOM_VST:       randomCurrentPattern(CBEngine::RandomFlavor::VST);       break;
        case IDM_EDIT_RANDOM_CHROMATIC: randomCurrentPattern(CBEngine::RandomFlavor::Chromatic); break;
        case IDM_EDIT_RANDOM_STEP:      randomFocusedStep(); break;
        case IDM_EDIT_TRANSPOSE_UP:           transposeCurrentPattern(+1);  break;
        case IDM_EDIT_TRANSPOSE_DOWN:         transposeCurrentPattern(-1);  break;
        case IDM_EDIT_TRANSPOSE_OCT_UP:       transposeCurrentPattern(+12); break;
        case IDM_EDIT_TRANSPOSE_OCT_DOWN:     transposeCurrentPattern(-12); break;
        case IDM_EDIT_TRANSPOSE_SONG_UP:      transposeSong(+1);            break;
        case IDM_EDIT_TRANSPOSE_SONG_DOWN:    transposeSong(-1);            break;
        case IDM_EDIT_TRANSPOSE_SONG_OCT_UP:  transposeSong(+12);           break;
        case IDM_EDIT_TRANSPOSE_SONG_OCT_DOWN: transposeSong(-12);          break;
        case IDM_STEP_TOGGLE_ACCENT:          toggleFocusedStepAccent();    break;
        case IDM_STEP_TOGGLE_SLIDE:           toggleFocusedStepSlide();     break;
        case IDM_STEP_TOGGLE_REVERSE:          toggleFocusedStepReverse();    break;
        case IDM_TRANSPORT_RUN_STOP:
        case IDC_TRANSPORT:       toggleTransport(); break;
        case IDM_TRANSPORT_PLAY_CURRENT:  playCurrentPatternOnly(); break;
        case IDM_TRANSPORT_PLAY_SEQUENCE: playSequenceFromCurrent(); break;
        case IDM_TRANSPORT_PLAY_SONG:     playSong();               break;
        case IDM_TRANSPORT_STOP:          stopTransport();          break;
        case IDM_PATTERN_PREV:    gotoPrevPattern(); break;
        case IDM_PATTERN_NEXT:    gotoNextPattern(); break;
        case IDM_PATTERN_NEW:     addPatternAfterCurrent(); break;
        case IDM_PATTERN_DELETE:  deleteCurrentPattern(); break;
        case IDM_OPEN_DISTORTION: openDistortionDialog();  break;
        case IDM_PATTERN_TRIPLET: toggleCurrentPatternTriplet(); break;
        case IDM_HELP_KEYS:       showKeyboardReference(); break;
        case IDM_HELP_ABOUT:      showAbout(); break;
        case IDC_WAVEFORM:
            if (notif == CBN_SELCHANGE) {
                int sel = ComboBox_GetCurSel(hWaveform_);
                engine_.setParam(CbParam::Waveform, sel == 1 ? 1.0f : 0.0f);
                announceValue(hStatus_, sel == 1 ? "Waveform: Square." : "Waveform: Saw.");
            }
            break;
        case IDC_OSC_BUTTON:
            if (notif == BN_CLICKED) openOscillatorDialog();
            break;
        case IDC_DIST_BUTTON:
            if (notif == BN_CLICKED) openDistortionDialog();
            break;
        case IDC_STEP_SELECTOR:
            if (notif == LBN_SELCHANGE) onStepSelectorChange();
            break;
        case IDC_PATTERN_LENGTH:
            if (notif == CBN_SELCHANGE) onPatternLengthChange();
            break;
        case IDC_PATTERN_TRIPLET:
            if (notif == BN_CLICKED) toggleCurrentPatternTriplet();
            break;
        case IDC_PATTERN_COMBO:
            if (notif == CBN_SELCHANGE) {
                int sel = static_cast<int>(SendMessageW(hPatternCombo_, CB_GETCURSEL, 0, 0));
                if (sel >= 0 && sel < engine_.patternCount()) {
                    editingIdx_ = sel;
                    populateStepList();
                    refreshPatternLabel();
                }
            }
            break;
        case IDC_STEP_PITCH:
            if (notif == CBN_SELCHANGE) writeFocusedStepFromEditor();
            break;
        case IDC_STEP_SLIDE:
        case IDC_STEP_ACCENT:
        case IDC_STEP_REVERSE:
            if (notif == BN_CLICKED) writeFocusedStepFromEditor();
            break;
        case IDC_RESO_BOOST:
            if (notif == BN_CLICKED) {
                const bool on = SendMessageW(hResoBoost_, BM_GETCHECK, 0, 0) == BST_CHECKED;
                engine_.setParam(CbParam::ResoBoost, on ? 1.0f : 0.0f);
                announceValue(hStatus_, on ? "Resonance boost on - the filter self-oscillates near the top of the knob."
                                           : "Resonance boost off.");
            }
            break;
        case IDC_TEMPO:
            if (notif == EN_CHANGE) {
                wchar_t buf[16]{}; GetWindowTextW(hTempo_, buf, 16);
                int bpm = _wtoi(buf);
                if (bpm >= 40 && bpm <= 300) engine_.setParam(CbParam::Tempo, static_cast<float>(bpm));
            }
            break;
    }
}

LRESULT MainWindow::proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE:
            buildLayout();
            SetTimer(hwnd, 1, 100, nullptr);  // 10 Hz transport-position poll
            return 0;
        case WM_TIMER:
            if (wp == 1 && engine_.isRunning()) {
                int pat  = engine_.currentPatternIndex();
                int step = engine_.currentStepIndex();
                int pats = engine_.patternCount();
                char sb[128];
                _snprintf_s(sb, _TRUNCATE,
                            "Playing  pattern %d/%d, step %d/16  (audio %.0f Hz)",
                            pat + 1, pats, step + 1, audio_.sampleRate());
                auto w = toWide(sb);
                SetWindowTextW(hStatus_, w.c_str());
                // deliberately don't fire EVENT_OBJECT_VALUECHANGE here —
                // screen readers would announce every tick.
            }
            return 0;
        case WM_COMMAND: onCommand(wp); return 0;
        case WM_HSCROLL:
            if (lp) onSliderScroll(reinterpret_cast<HWND>(lp));
            return 0;
        case WM_APP + 1: {
            // Enter-on-slider -> open sweep editor for that slider's param.
            int ctrlId = static_cast<int>(wp);
            int sliderIdx = ctrlId - IDC_SLIDER_BASE;
            if (sliderIdx >= 0 && sliderIdx < 7) openSweepEditor(sliderIdx);
            return 0;
        }
        // WM_NOTIFY handling for the old ListView step editor was removed
        // when the inline per-step controls replaced it.
        case WM_DESTROY: PostQuitMessage(0); return 0;
        default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int MainWindow::runMessageLoop() {
    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0) > 0) {
        if (accel_ && TranslateAcceleratorW(hwnd_, accel_, &msg)) continue;
        if (IsDialogMessageW(hwnd_, &msg))                         continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}
