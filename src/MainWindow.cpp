#include "MainWindow.h"
#include "AudioEngine.h"
#include "CBEngine.h"
#include "CbPresetIO.h"
#include "RoqIO.h"
#include "resource.h"
#include "version.h"

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
constexpr wchar_t kClassName[] = L"ScreamboxMainWnd";

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

// Enter on this trackbar opens the sweep editor for it: the subclass sends
// WM_APP + 1 with the control id to the parent, which looks the id up with
// sweepTargetForCtrl. Every trackbar shares one window class, so the first
// subclass captures the original wndproc for all of them - the dialogs' too.
static void makeSliderSweepable(HWND slider) {
    WNDPROC prev = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(slider, GWLP_WNDPROC,
                          reinterpret_cast<LONG_PTR>(sliderWndProc)));
    if (!g_oldSliderProc) g_oldSliderProc = prev;
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

MainWindow::MainWindow(CBEngine& engine, AudioEngine& audio, HINSTANCE hInst)
    : engine_(engine), audio_(audio), hInst_(hInst) {}

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
        L"Screambox — accessible bassline synth",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, 780, 672,
        nullptr, menu, hInst_, this);
    if (!h) return false;
    // This thread only: the dialogs are all modal on the UI thread.
    hookSelf_   = this;
    dialogHook_ = SetWindowsHookExW(WH_MSGFILTER, &MainWindow::dialogKeyHook,
                                    nullptr, GetCurrentThreadId());
    return true;
}

MainWindow* MainWindow::hookSelf_ = nullptr;

// F5-F8 inside this program's dialogs. Without this the sweep editor's Apply
// could only be heard if the song was started BEFORE the editor opened: a
// modal dialog runs its own message loop, TranslateAccelerator is never
// called for it, and the keys did nothing at all there.
//
// Only in OUR dialogs, recognised by their dialog procedure. The Open and Save
// file dialogs run through the same kind of loop, and in them F5 is the
// Explorer view's Refresh - taking it there would break a key someone knows.
// The keys are free in our dialogs: no standard control uses F5-F8 (F4 opens
// a combobox, which is why the transport never lived there).
//
// The main window says what happened in its status bar, which a screen reader
// in the dialog does not hear, so the text is copied into the dialog's own
// status line where it has one. Where it has none (Oscillator, Acid FX) the
// sound starting or stopping is the answer.
LRESULT CALLBACK MainWindow::dialogKeyHook(int code, WPARAM wp, LPARAM lp) {
    MainWindow* self = hookSelf_;
    if (code == MSGF_DIALOGBOX && self) {
        const MSG* m = reinterpret_cast<const MSG*>(lp);
        // Bit 30: the key was already down - an auto-repeat, which would
        // restart the song over and over while F5 is held.
        const bool fresh = m->message == WM_KEYDOWN && !(m->lParam & (1L << 30));
        const bool plain = !(GetKeyState(VK_CONTROL) & 0x8000) && !(GetKeyState(VK_SHIFT) & 0x8000)
                        && !(GetKeyState(VK_MENU) & 0x8000);
        int cmd = 0;
        if (fresh && plain && m->wParam == VK_F1) {
            // F1 in a dialog explains that dialog. Posted, not sent: a message
            // box opened from inside a message hook is a nested modal loop.
            HWND root = GetAncestor(m->hwnd, GA_ROOT);
            const DLGPROC proc = root ? reinterpret_cast<DLGPROC>(GetWindowLongPtrW(root, DWLP_DLGPROC))
                                      : nullptr;
            const int help = proc == &MainWindow::sweepEditorProc ? IDC_SWEEP_HELP
                           : proc == &MainWindow::modDialogProc   ? IDC_MOD_HELP : 0;
            if (help) {
                PostMessageW(root, WM_COMMAND, help, 0);
                return TRUE;
            }
        }
        if (fresh && plain) {
            switch (m->wParam) {
                case VK_F5: cmd = IDM_TRANSPORT_PLAY_SONG;     break;
                case VK_F6: cmd = IDM_TRANSPORT_PLAY_CURRENT;  break;
                case VK_F7: cmd = IDM_TRANSPORT_PLAY_SEQUENCE; break;
                case VK_F8: cmd = IDM_TRANSPORT_STOP;          break;
                default: break;
            }
        }
        if (cmd) {
            HWND root = GetAncestor(m->hwnd, GA_ROOT);
            const DLGPROC proc = root ? reinterpret_cast<DLGPROC>(GetWindowLongPtrW(root, DWLP_DLGPROC))
                                      : nullptr;
            int statusId = -1;   // -1: not one of ours
            if      (proc == &MainWindow::sweepEditorProc) statusId = IDC_SWEEP_STATUS;
            else if (proc == &MainWindow::modDialogProc)   statusId = IDC_MOD_STATUS;
            else if (proc == &MainWindow::oscDialogProc
                  || proc == &MainWindow::distDialogProc)  statusId = 0;
            if (statusId >= 0) {
                SendMessageW(self->hwnd_, WM_COMMAND, cmd, 0);
                if (HWND st = statusId ? GetDlgItem(root, statusId) : nullptr) {
                    wchar_t said[256]{};
                    GetWindowTextW(self->hStatus_, said, 256);
                    SetWindowTextW(st, said);
                    NotifyWinEvent(EVENT_OBJECT_NAMECHANGE, st, OBJID_CLIENT, CHILDID_SELF);
                }
                return TRUE;   // handled: the dialog does not see the key
            }
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
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
    // Every letter of "Modulation" is taken on this window, so it names its
    // chord instead (the accessibility rule in CLAUDE.md).
    hModButton_ = createButton(hwnd_, hInst_, IDC_MOD_BUTTON, 452, y, 160, 22,
                               L"Modulation (Ctrl+M)...");
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
    // >100% amplifies the squelch on successive accents. The position IS the
    // engine value times 100, so what the label says is what the engine gets.
    hAccentSquelchLabel_ = createLabel(hwnd_, hInst_, IDC_ACCENT_SQUELCH_LABEL,
        400, y + 2, 110, 16, L"Accent s&quelch:");
    hAccentSquelch_ = CreateWindowExW(0, TRACKBAR_CLASSW, L"Accent squelch",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_HORZ | TBS_AUTOTICKS,
        400, y + 16, 130, 22,
        hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_ACCENT_SQUELCH)), hInst_, nullptr);
    // 0..200 = sq 0.0..2.0. It used to be 0..100 mapped onto 1.0..2.0, so the
    // half of the range below Frank's original - down to no stacking at all,
    // which the engine has always supported - was out of reach, and a loaded
    // song with sq under 1 was shown wrong and overwritten on the first touch.
    SendMessageW(hAccentSquelch_, TBM_SETRANGE, TRUE, MAKELPARAM(0, 200));
    SendMessageW(hAccentSquelch_, TBM_SETLINESIZE, 0, 1);
    SendMessageW(hAccentSquelch_, TBM_SETPAGESIZE, 0, 10);
    const int sqPos = static_cast<int>(std::lround(engine_.getParam(CbParam::AccentSquelch) * 100.0f));
    SendMessageW(hAccentSquelch_, TBM_SETPOS, TRUE, sqPos);
    wchar_t sqText[16]; swprintf(sqText, 16, L"%d%%", sqPos);
    hAccentSquelchValue_ = createLabel(hwnd_, hInst_, IDC_ACCENT_SQUELCH_VALUE,
        535, y + 18, 50, 16, sqText);
    makeSliderSweepable(hAccentSquelch_);

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
        sliderLabelBase_[i] = kParamNames[i];
        sliderPlainBase_[i] = kParamPlain[i];
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
        // Rounded, not truncated: 0.53f * 100 is 52.99999, and truncation
        // turned a saved 53 into 52 (and 59 into 58) on every load.
        int initPos = static_cast<int>(std::lround(engine_.getParam(kParamIds[i]) * 100.0f));
        if (initPos < rangeMin) initPos = rangeMin;
        if (initPos > rangeMax) initPos = rangeMax;
        SendMessageW(hSliders_[i], TBM_SETPOS, TRUE, static_cast<LPARAM>(initPos));
        hSliderValues_[i] = createLabel(hwnd_, hInst_, IDC_STATIC, x, y + 16 + sliderH + 2, sliderW, 16, L"");
        updateSliderValue(i);
        // Subclass slider so Enter opens its sweep editor.
        makeSliderSweepable(hSliders_[i]);
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
    swprintf(initMsg, 128, L"Ready. Audio: %.0f Hz, WASAPI shared.",
             audio_.sampleRate());
    SetWindowTextW(hStatus_, initMsg);
    audioGen_ = audio_.deviceGeneration();   // the device we started on is not news

    refreshPatternLabel();
    refreshSweptLabels();
}

// -------------------- Sweep editor --------------------

// The ring's exponential taper lives with its dialog further down; the sweep
// editor needs it too, for the frequency target.
static double ringHzFromPos(int pos);
static int    ringPosFromHz(double hz);
static void   formatRingHz(double hz, wchar_t* out, size_t n);

// How the sweep editor shows and stores each kind of sweep target. A sweep
// STORES real units (CBEngine::ParamSweep lists them); the editor's two value
// trackbars move in slider positions, the same ones the target's own slider
// uses, so a frequency sweep feels like its slider and not like a raw Hz count.
enum class SweepKind {
    MainUni,   // main-window unipolar, 0..100, shown "+50%" as it always was
    MainBi,    // main-window bipolar, -100..+100
    Squelch,   // 0..200 %
    Percent,   // 0..100 %
    Amount,    // 0..100 %, "off" at 0 - the effect is switched off there
    Cents,     // -1200..+1200 cents
    RingHz,    // ring carrier: stored in 1/100 Hz, slider is the dialog's taper
};
struct SweepTargetUi {
    const wchar_t* name;   // in the editor's header and the status line
    SweepKind kind;
    int ctrlId;            // the slider whose Enter opens the editor for it
};
// A control id no control has, for the retired rows below.
constexpr int kNoCtrl = -1000;
// Indexed by sweep target; the order is the file format's (CBEngine.h).
static const SweepTargetUi kSweepUi[CBEngine::kSweepTargets] = {
    {L"Volume",               SweepKind::MainUni, IDC_SLIDER_BASE + 0},
    {L"Cutoff",               SweepKind::MainUni, IDC_SLIDER_BASE + 1},
    {L"Resonance",            SweepKind::MainBi,  IDC_SLIDER_BASE + 2},
    {L"EnvMod",               SweepKind::MainBi,  IDC_SLIDER_BASE + 3},
    {L"Decay",                SweepKind::MainUni, IDC_SLIDER_BASE + 4},
    {L"Accent",               SweepKind::MainUni, IDC_SLIDER_BASE + 5},
    {L"Tuning",               SweepKind::MainBi,  IDC_SLIDER_BASE + 6},
    {L"Accent squelch",       SweepKind::Squelch, IDC_ACCENT_SQUELCH},
    {L"Main PWM",             SweepKind::Percent, IDC_OSC_PWM},
    {L"Sub sine level",       SweepKind::Percent, IDC_OSC_SUB},
    {L"Noise level",          SweepKind::Percent, IDC_OSC_NOISE},
    {L"Duplet PWM",           SweepKind::Percent, IDC_OSC_DUP_PWM},
    {L"Duplet detune",        SweepKind::Cents,   IDC_OSC_DUP_DETUNE},
    {L"Duplet level",         SweepKind::Percent, IDC_OSC_DUP_LEVEL},
    // 14..16 were the PWM LFO's rate and depths, retired with it in v15
    // (CBEngine::sweepTargetRetired). No slider opens them and nothing lists
    // them; the rows only keep the numbering.
    {L"(retired)",            SweepKind::Percent, kNoCtrl},
    {L"(retired)",            SweepKind::Percent, kNoCtrl},
    {L"(retired)",            SweepKind::Percent, kNoCtrl},
    {L"Distortion gain",      SweepKind::Percent, IDC_DIST_GAIN},
    {L"Distortion tone",      SweepKind::Percent, IDC_DIST_TONE},
    {L"Distortion level",     SweepKind::Percent, IDC_DIST_LEVEL},
    {L"Compressor threshold", SweepKind::Percent, IDC_FX_COMP_THRESH},
    {L"Compressor depth",     SweepKind::Amount,  IDC_FX_COMP_DEPTH},
    {L"Ring frequency",       SweepKind::RingHz,  IDC_FX_RING_HZ},
    {L"Ring amount",          SweepKind::Amount,  IDC_FX_RING_AMOUNT},
};

// The sweep target a slider stands for, or -1. Control ids are unique across
// the main window and both dialogs, so one lookup serves all three.
static int sweepTargetForCtrl(int ctrlId) {
    for (int k = 0; k < CBEngine::kSweepTargets; ++k)
        if (kSweepUi[k].ctrlId == ctrlId) return k;
    return -1;
}

static int sweepPosToStored(int target, int pos) {
    switch (kSweepUi[target].kind) {
        case SweepKind::RingHz: return (int)std::lround(ringHzFromPos(pos) * 100.0);
        default:                return pos;
    }
}
static int sweepStoredToPos(int target, int stored) {
    switch (kSweepUi[target].kind) {
        case SweepKind::RingHz: return ringPosFromHz(stored / 100.0);
        default:                return stored;
    }
}
// A parameter value (engine units) as a stored sweep value, for starting a new
// sweep at the slider's current setting.
static int sweepStoredFromParam(int target, float v) {
    switch (kSweepUi[target].kind) {
        case SweepKind::Cents:  return (int)std::lround(v * 1200.0f);
        default:                return (int)std::lround(v * 100.0f);
    }
}
static void sweepValueLabelFor(int target, int stored, wchar_t* out, size_t n) {
    switch (kSweepUi[target].kind) {
        case SweepKind::MainUni:
        case SweepKind::MainBi:
            if (stored >= 0) swprintf(out, n, L"+%d%%", stored);
            else             swprintf(out, n, L"%d%%", stored);
            break;
        case SweepKind::Amount:
            if (stored <= 0) swprintf(out, n, L"off");
            else             swprintf(out, n, L"%d%%", stored);
            break;
        case SweepKind::Cents:
            if (stored >= 0) swprintf(out, n, L"+%d c", stored);
            else             swprintf(out, n, L"%d c", stored);
            break;
        case SweepKind::RingHz: formatRingHz(stored / 100.0, out, n);  break;
        default:                swprintf(out, n, L"%d%%", stored);     break;
    }
}

namespace { bool sweepsOverlap(const MainWindow::Sweep& a, const MainWindow::Sweep& b) {
    // A straight sweep and an LFO never overlap: the engine chooses one of
    // each per step and the LFO moves whatever the straight one sets. Two
    // LFOs, or two straight sweeps, do.
    if (a.osc.on() != b.osc.on()) return false;
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

// A sweep whose end comes before its start passes the overlap test above (it
// orders the two ends itself) and then never plays at all: the engine applies a
// sweep only while start <= position <= end. Refused with a reason instead.
static bool sweepIsBackwards(const MainWindow::Sweep& s) {
    return s.endPattern * 64 + s.endStep < s.startPattern * 64 + s.startStep;
}

// Sets a dialog label and says so to assistive technology - the dialog
// equivalent of announceValue(). A bare SetWindowTextW leaves a screen reader
// holding the old text.
static void setDlgLabel(HWND dlg, int id, const wchar_t* text) {
    HWND h = GetDlgItem(dlg, id);
    SetWindowTextW(h, text);
    NotifyWinEvent(EVENT_OBJECT_NAMECHANGE, h, OBJID_CLIENT, CHILDID_SELF);
}
// The same, but silent when the text is already right: the dialogs re-set all
// their labels whenever any one might have changed, and a name-change event
// for a label that did not change is noise to a screen reader.
static void setDlgLabelIfChanged(HWND dlg, int id, const std::wstring& text) {
    wchar_t cur[160]{};
    GetWindowTextW(GetDlgItem(dlg, id), cur, 160);
    if (text != cur) setDlgLabel(dlg, id, text.c_str());
}

// Oscillator and Acid FX sliders: subclassed so Enter opens the sweep editor,
// as it does on the main window. Without this Enter pressed the dialog's
// default button and closed it - the opposite of what the same key does one
// window away. The dialogs still close with Alt+C and Esc.
static const int kOscSweepSliders[] = {
    IDC_OSC_PWM, IDC_OSC_SUB, IDC_OSC_NOISE, IDC_OSC_DUP_PWM, IDC_OSC_DUP_DETUNE,
    IDC_OSC_DUP_LEVEL,
};
static const int kFxSweepSliders[] = {
    IDC_DIST_GAIN, IDC_DIST_TONE, IDC_DIST_LEVEL, IDC_FX_COMP_THRESH,
    IDC_FX_COMP_DEPTH, IDC_FX_RING_HZ, IDC_FX_RING_AMOUNT,
};

INT_PTR CALLBACK MainWindow::sweepEditorProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(dlg, GWLP_USERDATA));
    if (msg == WM_INITDIALOG) {
        self = reinterpret_cast<MainWindow*>(lp);
        SetWindowLongPtrW(dlg, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return FALSE;
    return self->sweepEditorHandler(dlg, msg, wp, lp);
}

// The value trackbars take the range and step sizes of the target's own
// slider, so the keys do in the editor what they do on the slider.
static void configureSweepValueTrackbars(HWND dlg, int target) {
    int lo = 0, hi = 100, line = 1, page = 10;
    switch (kSweepUi[target].kind) {
        case SweepKind::MainBi:  lo = -100; break;
        case SweepKind::Squelch: hi = 200;  break;
        case SweepKind::Cents:   lo = -1200; hi = 1200; line = 10; page = 100; break;
        default: break;
    }
    for (int id : {IDC_SWEEP_START_VALUE, IDC_SWEEP_END_VALUE}) {
        HWND t = GetDlgItem(dlg, id);
        SendMessageW(t, TBM_SETRANGE, TRUE, MAKELPARAM(lo, hi));
        SendMessageW(t, TBM_SETLINESIZE, 0, line);
        SendMessageW(t, TBM_SETPAGESIZE, 0, page);
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

// The LFO part of the editor (v15). Item 0 says what Off means, and that the
// fields beside it then do nothing - the accessibility rule's way of saying
// "inert" without disabling anything.
static const wchar_t* kWaveNames[] = {
    L"Off: a straight sweep (frequency and phase unused)",
    L"Triangle", L"Sine", L"Square", L"Saw up", L"Saw down",
};
static_assert(sizeof kWaveNames / sizeof kWaveNames[0] == (size_t)CBEngine::OscShape::Count,
              "one name per OscShape");
static const wchar_t* kRateUnitNames[] = {
    L"steps per cycle (follows the tempo)",
    L"Hz (cycles a second, ignores the tempo)",
};
// A new LFO's frequency: one cycle a 16-step bar.
constexpr float kDefaultLfoRate = 16.0f;

// The shortest text that reads back as exactly this float, so that showing a
// rate and pressing Update cannot nudge it - a converted PWM LFO's rate came
// off a taper and has six significant digits.
static void formatShortFloat(float v, wchar_t* out, size_t n) {
    for (int prec = 1; prec <= 9; ++prec) {
        swprintf(out, n, L"%.*g", prec, (double)v);
        if (std::wcstof(out, nullptr) == v) return;
    }
}

// Labels that change with the wave: with one, the two values are a depth and
// Hold keeps the LFO going. Set through setDlgLabelIfChanged so a screen
// reader hears the change once and only when there is one.
static void refreshSweepWaveLabels(HWND dlg) {
    const bool lfo = SendMessageW(GetDlgItem(dlg, IDC_SWEEP_WAVE), CB_GETCURSEL, 0, 0) > 0;
    setDlgLabelIfChanged(dlg, IDC_SWEEP_START_VALUE_CAP, lfo ? L"Start depth (&v):" : L"Start &value:");
    setDlgLabelIfChanged(dlg, IDC_SWEEP_END_VALUE_CAP,   lfo ? L"End depth (&l):"   : L"End va&lue:");
    setDlgLabelIfChanged(dlg, IDC_SWEEP_HOLD,
        lfo ? L"&Hold: keep the LFO going at its end depth until the next LFO"
            : L"&Hold the end value until the next sweep");
}

static void populateSweepDialog(HWND dlg, int target, const std::vector<int>& patLens,
                                const MainWindow::Sweep& sw,
                                const std::vector<MainWindow::Sweep>& all, int curIdx) {
    const int numPatterns = (int)patLens.size();
    // Fill sweep-number combo
    HWND num = GetDlgItem(dlg, IDC_SWEEP_NUMBER);
    SendMessageW(num, CB_RESETCONTENT, 0, 0);
    for (int i = 0; i < (int)all.size(); ++i) {
        wchar_t buf[24]; swprintf(buf, 24, all[i].osc.on() ? L"%d (LFO)" : L"%d", i + 1);
        SendMessageW(num, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(buf));
    }
    // Always there, always last, at index all.size(): the way to start another
    // sweep. It used to appear only while the list was empty, so once one sweep
    // existed the fields always showed an existing one and Add could only
    // collide with it - a second sweep looked impossible.
    SendMessageW(num, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"(new sweep)"));
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

    // A sweep can name a pattern this song does not have - from a .hape made
    // for another song, or one whose patterns were deleted. A combo given an
    // out-of-range index shows nothing and reads back CB_ERR, which Add used
    // to store as pattern -1. Show the nearest real pattern and say so.
    auto clampPat = [&](int p) { return p < 0 ? 0 : (p >= numPatterns ? numPatterns - 1 : p); };
    const int sp = clampPat(sw.startPattern), ep = clampPat(sw.endPattern);
    SendMessageW(GetDlgItem(dlg, IDC_SWEEP_START_PATTERN), CB_SETCURSEL, sp, 0);
    SendMessageW(GetDlgItem(dlg, IDC_SWEEP_END_PATTERN),   CB_SETCURSEL, ep, 0);
    // After the pattern combos, so each step list is sized to its own pattern.
    fillStepCombo(dlg, IDC_SWEEP_START_STEP, patLens[sp], sw.startStep);
    fillStepCombo(dlg, IDC_SWEEP_END_STEP,   patLens[ep], sw.endStep);
    SendMessageW(GetDlgItem(dlg, IDC_SWEEP_START_VALUE), TBM_SETPOS, TRUE,
                 sweepStoredToPos(target, sw.startValue));
    SendMessageW(GetDlgItem(dlg, IDC_SWEEP_END_VALUE),   TBM_SETPOS, TRUE,
                 sweepStoredToPos(target, sw.endValue));
    CheckDlgButton(dlg, IDC_SWEEP_HOLD, sw.hold ? BST_CHECKED : BST_UNCHECKED);
    SendMessageW(GetDlgItem(dlg, IDC_SWEEP_WAVE), CB_SETCURSEL, sw.osc.on() ? sw.osc.shape : 0, 0);
    SendMessageW(GetDlgItem(dlg, IDC_SWEEP_RATE_UNIT), CB_SETCURSEL, sw.osc.hz ? 1 : 0, 0);
    {
        wchar_t rate[32];
        formatShortFloat(sw.osc.rate > 0.0f ? sw.osc.rate : kDefaultLfoRate, rate, 32);
        SetDlgItemTextW(dlg, IDC_SWEEP_RATE, rate);
        const long deg = std::lround((double)sw.osc.phase * 360.0) % 360;
        SetDlgItemInt(dlg, IDC_SWEEP_PHASE, (UINT)(deg < 0 ? deg + 360 : deg), FALSE);
    }
    refreshSweepWaveLabels(dlg);
    wchar_t sv[24], ev[24];
    sweepValueLabelFor(target, sw.startValue, sv, 24);
    sweepValueLabelFor(target, sw.endValue,   ev, 24);
    setDlgLabel(dlg, IDC_SWEEP_START_VALUE_LBL, sv);
    setDlgLabel(dlg, IDC_SWEEP_END_VALUE_LBL,   ev);

    int lfos = 0;
    for (const auto& s : all) lfos += s.osc.on() ? 1 : 0;
    wchar_t cnt[48];
    if (lfos) swprintf(cnt, 48, L"Sweeps: %d, of which %d LFO%s", (int)all.size(), lfos, lfos == 1 ? L"" : L"s");
    else      swprintf(cnt, 48, L"Sweeps: %d", (int)all.size());
    setDlgLabel(dlg, IDC_SWEEP_COUNT_LABEL, cnt);
    if (sp != sw.startPattern || ep != sw.endPattern) {
        wchar_t m[160];
        swprintf(m, 160, L"This sweep names pattern %d, which this song does not have. Showing pattern %d.",
                 (sp != sw.startPattern ? sw.startPattern : sw.endPattern) + 1,
                 (sp != sw.startPattern ? sp : ep) + 1);
        setDlgLabel(dlg, IDC_SWEEP_STATUS, m);
    }
}

// Absolute song step of (pattern, step), counting each pattern's real length -
// the same timeline the engine plays sweeps on (posOf in CBEngine::process).
static long absStepOf(const std::vector<int>& patLens, int pat, int step) {
    long at = 0;
    for (int p = 0; p < pat && p < (int)patLens.size(); ++p) at += patLens[p];
    return at + step;
}
static void patStepOf(const std::vector<int>& patLens, long abs, int& pat, int& step) {
    pat = 0;
    while (pat + 1 < (int)patLens.size() && abs >= patLens[pat]) { abs -= patLens[pat]; ++pat; }
    step = (int)abs;
}

// "pattern 1, step 1 to pattern 1, step 16" - counted from 1, as the combos are.
static std::wstring sweepRangeText(const MainWindow::Sweep& s) {
    wchar_t b[96];
    if (s.startPattern == s.endPattern)
        swprintf(b, 96, L"pattern %d, steps %d to %d", s.startPattern + 1, s.startStep + 1, s.endStep + 1);
    else
        swprintf(b, 96, L"pattern %d step %d to pattern %d step %d",
                 s.startPattern + 1, s.startStep + 1, s.endPattern + 1, s.endStep + 1);
    return b;
}

// Why Add / Update / Apply refused: which sweep is in the way, where, and what
// to do about it. "Overlap with sweep 1. Adjust ranges." was all it used to
// say, and with the first sweep filling the pattern there was no free range to
// adjust TO - a second sweep looked impossible.
static std::wstring overlapText(const MainWindow::Sweep& other, size_t k) {
    wchar_t b[320];
    swprintf(b, 320, L"Overlaps sweep %zu (%ls). Two straight sweeps, or two LFOs, cannot share a "
                     L"step on one slider. Shorten one, or choose (new sweep) in Sweep number to "
                     L"start in a free range.", k + 1, sweepRangeText(other).c_str());
    return b;
}

// The fields for "(new sweep)": the first run of steps that no sweep of the
// same kind covers, up to the end of that step's pattern, at `value` both ends.
// `found` is false when every step is covered - the fields then show pattern
// 1 and the caller says why. Straight sweeps and LFOs are counted apart, as
// sweepsOverlap() does: an LFO never blocks a straight sweep or the reverse.
static MainWindow::Sweep freshSweep(const std::vector<MainWindow::Sweep>& list,
                                    const std::vector<int>& lens, bool lfo, int value,
                                    bool& found) {
    MainWindow::Sweep s;
    s.startValue = s.endValue = value;
    const int first = lens.empty() ? 16 : lens[0];
    s.endStep = first - 1;
    found = false;
    long total = 0;
    for (int n : lens) total += n;
    if (total <= 0) return s;
    std::vector<char> covered((size_t)total, 0);
    for (const auto& o : list) {
        if (o.osc.on() != lfo) continue;
        // Endpoints as the engine clamps them (posOf in CBEngine::process); a
        // pattern this song does not have lies past its end and covers nothing.
        auto at = [&](int p, int st) -> long {
            if (p >= (int)lens.size()) return total;
            if (p < 0) return -1;
            if (st >= lens[p]) st = lens[p] - 1;
            return absStepOf(lens, p, st < 0 ? 0 : st);
        };
        const long a0 = std::max(0L, at(o.startPattern, o.startStep));
        const long a1 = std::min(total - 1, at(o.endPattern, o.endStep));
        for (long i = a0; i <= a1; ++i) covered[(size_t)i] = 1;
    }
    long i = 0;
    while (i < total && covered[(size_t)i]) ++i;
    if (i >= total) return s;
    int p0, st0;
    patStepOf(lens, i, p0, st0);
    long j = i;
    while (j + 1 < total && !covered[(size_t)j + 1] && j + 1 - i < lens[p0] - st0) ++j;
    patStepOf(lens, i, s.startPattern, s.startStep);
    patStepOf(lens, j, s.endPattern, s.endStep);
    found = true;
    return s;
}

// Shown in the status line when the LFO wave changes, so the choice explains
// itself where the user is. The whole story is behind Help (F1).
static const wchar_t* kWaveExplain[] = {
    L"Straight sweep: the slider goes evenly from Start value to End value across the range, "
    L"then back to its own setting - or stays, with Hold.",
    L"LFO: moves the slider up and down by itself, around its own setting or a straight sweep "
    L"on the same steps. Depth is how far it swings. F1 explains more.",
};

static const wchar_t kSweepHelp[] =
    L"A SWEEP moves one slider by itself while the song plays. Each slider can have several, "
    L"at different places in the song.\r\n\r\n"
    L"STRAIGHT SWEEP (LFO wave: Off)\r\n"
    L"From the start pattern and step to the end pattern and step, the slider goes evenly from "
    L"Start value to End value. After the end it returns to the slider's own setting, or keeps "
    L"the end value if Hold is ticked. Two straight sweeps on one slider cannot cover the same "
    L"step.\r\n\r\n"
    L"LFO (any other wave)\r\n"
    L"An LFO swings the slider up and down by itself, over and over. It does not replace the "
    L"slider: it swings around the slider's own setting, or around a straight sweep on the same "
    L"steps, so a ramp and a wobble can run at once.\r\n"
    L"- Wave: triangle, sine, square, saw up or saw down - the shape of each swing.\r\n"
    L"- Start depth and End depth: how far it swings each way. Different values make the swing "
    L"grow or fade across the range.\r\n"
    L"- Frequency, measured in steps per cycle (follows the tempo: 16 is one swing a 16-step bar, "
    L"4 one a beat) or in Hz (swings a second, whatever the tempo).\r\n"
    L"- Starting phase, in degrees: where in its swing it starts. 0 starts in the middle going "
    L"up, 180 in the middle going down.\r\n"
    L"- Hold keeps it swinging after its end, until the next LFO on this slider.\r\n"
    L"An LFO may cover the same steps as a straight sweep; two LFOs on one slider may not. Main "
    L"PWM and Duplet PWM follow an LFO smoothly; every other slider moves once a step, so a fast "
    L"LFO there comes out in steps. The Modulation dialog (Ctrl+M on the main window) adds a "
    L"ready-made LFO to the whole song.\r\n\r\n"
    L"ADDING MORE\r\n"
    L"Sweep number lists the sweeps on this slider, then (new sweep). Choose (new sweep) and the "
    L"fields start in the first range no sweep covers yet; set them and press Add. To change a "
    L"sweep, choose its number, change the fields and press Update.\r\n\r\n"
    L"KEYS\r\n"
    L"Apply (Alt+Y) updates the sweep shown and lets you hear it without closing. F5 to F8 play "
    L"and stop as on the main window. Cancel undoes everything since the editor opened, applied "
    L"or not.";

static const wchar_t kModulationHelp[] =
    L"This dialog lists every slider that can move by itself, each with what it carries - "
    L"Cutoff (2 sweeps, 1 LFO).\r\n\r\n"
    L"A straight sweep moves a slider from one value to another across part of the song. An LFO "
    L"swings it up and down by itself, over and over, around the slider's own setting.\r\n\r\n"
    L"Enter, or Edit sweeps, opens the chosen slider's sweep editor, where both kinds are made "
    L"and changed; press F1 there for the details.\r\n\r\n"
    L"LFO preset and Add LFO to the whole song put a ready-made LFO on the chosen slider, from "
    L"the start of the song to the end and beyond. A preset's depth is a share of the slider's "
    L"range, so the same preset suits any slider. Each slider takes one LFO at a time; edit or "
    L"remove it in the sweep editor.\r\n\r\n"
    L"F5 to F8 play and stop as on the main window, so you can listen while you work.";

// The shapes Generate draws. Each is sampled once per step - the rate sweeps
// run at - and the samples are joined into as few straight sweeps as describe
// them exactly. The result is ordinary sweeps: nothing about a shape reaches
// the engine or the file, so it costs no format change and every one of the
// pieces can be edited afterwards like any other sweep.
enum class SweepShape { Ramp, Triangle, Alternate };
static const wchar_t* kShapeNames[] = {
    L"Ramp (start to end)",
    L"Up and down",
    L"Alternate (start, end)",
};

// One value per step of the range, in the editor's slider positions. The ramp
// reaches the end value on each cycle's last step, so its jump back is the
// sawtooth's edge. The other two are periodic - sampled at j / L, so one cycle
// leads into the next without a doubled step at the seam.
static std::vector<double> shapeValues(SweepShape shape, long n, int cycles,
                                       double a, double b) {
    std::vector<double> v((size_t)n);
    for (int c = 0; c < cycles; ++c) {
        const long c0 = (long)((long long)c * n / cycles);
        const long c1 = (long)((long long)(c + 1) * n / cycles);
        const long len = c1 - c0;
        for (long j = 0; j < len; ++j) {
            double f = 0.0;
            switch (shape) {
                case SweepShape::Ramp:
                    f = (len > 1) ? (double)j / (double)(len - 1) : 0.0;
                    break;
                case SweepShape::Triangle: {
                    const double ph = (double)j / (double)len;
                    f = (ph < 0.5) ? 2.0 * ph : 2.0 - 2.0 * ph;
                    break;
                }
                case SweepShape::Alternate:   // first half at the start value
                    f = (2 * j < len) ? 0.0 : 1.0;
                    break;
            }
            v[(size_t)(c0 + j)] = a + (b - a) * f;
        }
    }
    return v;
}

// The shape as sweeps: the per-step samples joined into straight runs, a run
// continuing while each step changes by the same amount as the run's first.
// `va` / `vb` are slider positions; the sweeps get stored values.
static std::vector<MainWindow::Sweep> shapeSweeps(int target, const std::vector<int>& lens,
                                                  long a0, long n, SweepShape shape,
                                                  int cycles, int va, int vb) {
    const std::vector<double> v = shapeValues(shape, n, cycles, va, vb);
    std::vector<MainWindow::Sweep> made;
    for (long i0 = 0; i0 < n; ) {
        long i1 = i0;
        if (i0 + 1 < n) {
            const double d = v[(size_t)i0 + 1] - v[(size_t)i0];
            i1 = i0 + 1;
            while (i1 + 1 < n && std::fabs((v[(size_t)i1 + 1] - v[(size_t)i1]) - d) < 1e-9) ++i1;
        }
        MainWindow::Sweep s;
        patStepOf(lens, a0 + i0, s.startPattern, s.startStep);
        patStepOf(lens, a0 + i1, s.endPattern,   s.endStep);
        s.startValue = sweepPosToStored(target, (int)std::lround(v[(size_t)i0]));
        s.endValue   = sweepPosToStored(target, (int)std::lround(v[(size_t)i1]));
        made.push_back(s);
        i0 = i1 + 1;
    }
    return made;
}

static MainWindow::Sweep readSweepFromDialog(HWND dlg, int target) {
    // CB_ERR (-1) must never become a pattern or step number.
    auto sel = [&](int id) {
        const int v = (int)SendMessageW(GetDlgItem(dlg, id), CB_GETCURSEL, 0, 0);
        return v < 0 ? 0 : v;
    };
    MainWindow::Sweep sw;
    sw.startPattern = sel(IDC_SWEEP_START_PATTERN);
    sw.startStep    = sel(IDC_SWEEP_START_STEP);
    sw.endPattern   = sel(IDC_SWEEP_END_PATTERN);
    sw.endStep      = sel(IDC_SWEEP_END_STEP);
    sw.startValue   = sweepPosToStored(target,
        (int)SendMessageW(GetDlgItem(dlg, IDC_SWEEP_START_VALUE), TBM_GETPOS, 0, 0));
    sw.endValue     = sweepPosToStored(target,
        (int)SendMessageW(GetDlgItem(dlg, IDC_SWEEP_END_VALUE),   TBM_GETPOS, 0, 0));
    sw.hold         = IsDlgButtonChecked(dlg, IDC_SWEEP_HOLD) == BST_CHECKED;
    const int wave  = sel(IDC_SWEEP_WAVE);
    if (wave > 0 && wave < (int)CBEngine::OscShape::Count) {
        sw.osc.shape = wave;
        sw.osc.hz    = sel(IDC_SWEEP_RATE_UNIT) == 1;
        wchar_t rate[32]{};
        GetDlgItemTextW(dlg, IDC_SWEEP_RATE, rate, 32);
        sw.osc.rate  = std::wcstof(rate, nullptr);   // checked by sweepOscProblem
        const UINT deg = GetDlgItemInt(dlg, IDC_SWEEP_PHASE, nullptr, FALSE);
        sw.osc.phase = (float)(deg % 360) / 360.0f;
    }
    return sw;
}

// Why the LFO fields cannot make a sweep, or nullptr if they can. A frequency
// of 0 is legal for the engine (the wave holds its start phase) but is never
// what someone typing into the field meant.
static const wchar_t* sweepOscProblem(const MainWindow::Sweep& sw) {
    if (!sw.osc.on()) return nullptr;
    const float r = sw.osc.rate;
    if (!(r > 0.0f))
        return L"Type a frequency above 0 for the LFO - for example 16 steps per cycle, or 2 Hz.";
    if (sw.osc.hz ? r > 1000.0f : r > 4096.0f)
        return sw.osc.hz ? L"The LFO frequency is above 1000 Hz. Type a lower one."
                         : L"The LFO cycle is longer than 4096 steps. Type a shorter one.";
    return nullptr;
}

INT_PTR MainWindow::sweepEditorHandler(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_INITDIALOG: {
            wchar_t hdr[128];
            swprintf(hdr, 128, L"Editing sweeps for: %s", kSweepUi[editingSweepParam_].name);
            SetWindowTextW(GetDlgItem(dlg, IDC_SWEEP_PARAM_LABEL), hdr);

            configureSweepValueTrackbars(dlg, editingSweepParam_);
            // Enter on a checkbox would otherwise press the dialog's default
            // button - OK - and close the editor. Same subclass as the main
            // window's checkboxes, so Enter toggles it like Space does.
            makeCheckboxEnterActivatable(GetDlgItem(dlg, IDC_SWEEP_HOLD));
            // Cancel restores this. Add, Update, Remove and Load all edit the
            // list in place, so without a snapshot Cancel did exactly what OK did.
            sweepSnapshot_ = paramSweeps_[editingSweepParam_];

            {
                HWND sh = GetDlgItem(dlg, IDC_SWEEP_SHAPE);
                for (const wchar_t* s : kShapeNames)
                    SendMessageW(sh, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
                SendMessageW(sh, CB_SETCURSEL, 0, 0);
                HWND cy = GetDlgItem(dlg, IDC_SWEEP_CYCLES);
                for (int c = 1; c <= 32; ++c) {
                    wchar_t buf[8]; swprintf(buf, 8, L"%d", c);
                    SendMessageW(cy, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(buf));
                }
                SendMessageW(cy, CB_SETCURSEL, 0, 0);
                HWND wv = GetDlgItem(dlg, IDC_SWEEP_WAVE);
                for (const wchar_t* w : kWaveNames)
                    SendMessageW(wv, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(w));
                HWND un = GetDlgItem(dlg, IDC_SWEEP_RATE_UNIT);
                for (const wchar_t* u : kRateUnitNames)
                    SendMessageW(un, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(u));
                SendMessageW(GetDlgItem(dlg, IDC_SWEEP_PHASE), EM_SETLIMITTEXT, 3, 0);
                SendMessageW(GetDlgItem(dlg, IDC_SWEEP_RATE),  EM_SETLIMITTEXT, 12, 0);
            }

            const auto& list = paramSweeps_[editingSweepParam_];
            Sweep current;
            if (list.empty()) {
                bool found = false;
                current = freshSweep(list, patternLengths(), false, newSweepValue(), found);
            } else {
                current = list[editingSweepIndex_];
            }
            populateSweepDialog(dlg, editingSweepParam_, patternLengths(),
                                current, list, editingSweepIndex_);
            SetFocus(GetDlgItem(dlg, IDC_SWEEP_NUMBER));
            return FALSE;
        }
        case WM_HSCROLL: {
            // Refresh both value labels on any trackbar scroll — cheap and
            // keeps them in sync with the current slider positions.
            const int k = editingSweepParam_;
            int startPos = (int)SendMessageW(GetDlgItem(dlg, IDC_SWEEP_START_VALUE), TBM_GETPOS, 0, 0);
            int endPos   = (int)SendMessageW(GetDlgItem(dlg, IDC_SWEEP_END_VALUE),   TBM_GETPOS, 0, 0);
            wchar_t s[24], e[24];
            sweepValueLabelFor(k, sweepPosToStored(k, startPos), s, 24);
            sweepValueLabelFor(k, sweepPosToStored(k, endPos),   e, 24);
            setDlgLabel(dlg, IDC_SWEEP_START_VALUE_LBL, s);
            setDlgLabel(dlg, IDC_SWEEP_END_VALUE_LBL,   e);
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
            if (id == IDC_SWEEP_WAVE && notif == CBN_SELCHANGE) {
                refreshSweepWaveLabels(dlg);
                const bool lfo = SendMessageW(GetDlgItem(dlg, IDC_SWEEP_WAVE), CB_GETCURSEL, 0, 0) > 0;
                setStatus(kWaveExplain[lfo ? 1 : 0]);
                return TRUE;
            }
            if (id == IDC_SWEEP_HELP) {
                MessageBoxW(dlg, kSweepHelp, L"Sweeps and LFOs", MB_OK | MB_ICONINFORMATION);
                return TRUE;
            }
            if (id == IDC_SWEEP_NUMBER && notif == CBN_SELCHANGE) {
                int sel = (int)SendMessageW(GetDlgItem(dlg, IDC_SWEEP_NUMBER), CB_GETCURSEL, 0, 0);
                if (sel >= 0 && sel < (int)list.size()) {
                    editingSweepIndex_ = sel;
                    populateSweepDialog(dlg, editingSweepParam_, patternLengths(),
                                        list[sel], list, sel);
                } else if (sel == (int)list.size()) {
                    // (new sweep): the first free range, at the slider's setting.
                    editingSweepIndex_ = sel;
                    const std::vector<int> lens = patternLengths();
                    bool found = false;
                    const Sweep fresh = freshSweep(list, lens, false, newSweepValue(), found);
                    populateSweepDialog(dlg, editingSweepParam_, lens, fresh, list, sel);
                    if (found) {
                        const std::wstring m = L"New sweep, in the first free range: "
                            + sweepRangeText(fresh) + L". Set it up and press Add.";
                        setStatus(m.c_str());
                    } else {
                        setStatus(L"Every step already has a straight sweep. An LFO can still go on "
                                  L"top - choose an LFO wave - or shorten a sweep to make room.");
                    }
                }
                return TRUE;
            }
            if (id == IDC_SWEEP_APPLY) {
                // Hear it without closing: the sweep shown takes the fields,
                // as Update does, and this target's whole list goes to the
                // engine - which otherwise hears nothing until the editor
                // closes. Cancel still undoes everything since the editor
                // opened: openSweepEditor() pushes the restored snapshot.
                // With no sweep in the list there is nothing to update - after
                // Remove of the last one, Apply must not bring the fields back
                // as a sweep - so the list is sent as it is.
                const bool any = !list.empty() && editingSweepIndex_ >= 0
                              && editingSweepIndex_ < (int)list.size();
                if (any) {
                    Sweep sw = readSweepFromDialog(dlg, editingSweepParam_);
                    if (sweepIsBackwards(sw)) {
                        setStatus(L"The end is before the start, so this sweep would never play. Adjust the ranges.");
                        return TRUE;
                    }
                    if (const wchar_t* why = sweepOscProblem(sw)) { setStatus(why); return TRUE; }
                    for (size_t k = 0; k < list.size(); ++k) {
                        if ((int)k == editingSweepIndex_) continue;
                        if (sweepsOverlap(list[k], sw)) {
                            setStatus(overlapText(list[k], k).c_str());
                            return TRUE;
                        }
                    }
                    list[editingSweepIndex_] = sw;
                    populateSweepDialog(dlg, editingSweepParam_, patternLengths(),
                                        list[editingSweepIndex_], list, editingSweepIndex_);
                }
                pushSweeps(engine_, editingSweepParam_);
                refreshSweptLabels();
                wchar_t m[256];
                // The two-line status area holds about 170 characters.
                const wchar_t* tail = engine_.isRunning()
                    ? L"Cancel still undoes it all."
                    : L"Nothing is playing: F5 plays the song, F6 this pattern, here as on the main window.";
                if (any && list.size() == 1)
                    swprintf(m, 256, L"Applied: sweep 1 updated, and it is what you hear now. %s", tail);
                else if (any)
                    swprintf(m, 256, L"Applied: sweep %d updated, and all %zu are what you hear now. %s",
                             editingSweepIndex_ + 1, list.size(), tail);
                else if (list.empty())
                    swprintf(m, 256, L"Applied: no sweeps on this slider. Add one first to hear it. %s",
                             tail);
                else
                    swprintf(m, 256, L"Applied the %zu sweeps already here. This new one is not added "
                                     L"yet: press Add, then Apply. %s", list.size(), tail);
                setStatus(m);
                return TRUE;
            }
            if (id == IDC_SWEEP_ADD) {
                Sweep sw = readSweepFromDialog(dlg, editingSweepParam_);
                if (sweepIsBackwards(sw)) {
                    setStatus(L"The end is before the start, so this sweep would never play. Adjust the ranges.");
                    return TRUE;
                }
                if (const wchar_t* why = sweepOscProblem(sw)) { setStatus(why); return TRUE; }
                for (size_t k = 0; k < list.size(); ++k) {
                    if (sweepsOverlap(list[k], sw)) {
                        setStatus(overlapText(list[k], k).c_str());
                        return TRUE;
                    }
                }
                list.push_back(sw);
                editingSweepIndex_ = (int)list.size() - 1;
                populateSweepDialog(dlg, editingSweepParam_, patternLengths(),
                                    list.back(), list, editingSweepIndex_);
                setStatus(sw.osc.on()
                    ? L"LFO added. To add another, choose (new sweep) in Sweep number."
                    : L"Sweep added. To add another, choose (new sweep) in Sweep number.");
                return TRUE;
            }
            if (id == IDC_SWEEP_UPDATE) {
                // Replace the sweep being viewed. Before this existed, changing
                // a sweep meant noting its values, removing it, retyping them
                // and adding it again - Add refused, because the new version
                // overlapped the very sweep it was meant to replace.
                if (list.empty() || editingSweepIndex_ < 0 || editingSweepIndex_ >= (int)list.size()) {
                    setStatus(L"This is a new sweep, not added yet: press Add.");
                    return TRUE;
                }
                Sweep sw = readSweepFromDialog(dlg, editingSweepParam_);
                if (sweepIsBackwards(sw)) {
                    setStatus(L"The end is before the start, so this sweep would never play. Adjust the ranges.");
                    return TRUE;
                }
                if (const wchar_t* why = sweepOscProblem(sw)) { setStatus(why); return TRUE; }
                for (size_t k = 0; k < list.size(); ++k) {
                    if ((int)k == editingSweepIndex_) continue;
                    if (sweepsOverlap(list[k], sw)) {
                        setStatus(overlapText(list[k], k).c_str());
                        return TRUE;
                    }
                }
                list[editingSweepIndex_] = sw;
                populateSweepDialog(dlg, editingSweepParam_, patternLengths(),
                                    list[editingSweepIndex_], list, editingSweepIndex_);
                wchar_t m[64]; swprintf(m, 64, L"Sweep %d updated.", editingSweepIndex_ + 1);
                setStatus(m);
                return TRUE;
            }
            if (id == IDC_SWEEP_REMOVE) {
                if (editingSweepIndex_ < 0 || editingSweepIndex_ >= (int)list.size()) {
                    setStatus(L"This is a new sweep, not added yet: there is nothing to remove.");
                    return TRUE;
                }
                list.erase(list.begin() + editingSweepIndex_);
                if (editingSweepIndex_ >= (int)list.size()) editingSweepIndex_ = (list.empty() ? 0 : (int)list.size() - 1);
                bool found = false;
                Sweep show = list.empty()
                    ? freshSweep(list, patternLengths(), false, newSweepValue(), found)
                    : list[editingSweepIndex_];
                populateSweepDialog(dlg, editingSweepParam_, patternLengths(),
                                    show, list, editingSweepIndex_);
                setStatus(L"Sweep removed.");
                return TRUE;
            }
            if (id == IDC_SWEEP_COPY) {
                // The clipboard holds SLIDER POSITIONS, not stored values. A
                // sweep is pasted between parameters, and since v14 those store
                // different units - percent, cents, fractions of a Hz. In
                // positions, a Cutoff sweep from 10 to 80 pasted onto Ring
                // frequency still travels most of that slider; in stored units
                // it would be 0.1 Hz to 0.8 Hz, clamped to the bottom.
                sweepClipboard_ = readSweepFromDialog(dlg, editingSweepParam_);
                sweepClipboard_.startValue = sweepStoredToPos(editingSweepParam_, sweepClipboard_.startValue);
                sweepClipboard_.endValue   = sweepStoredToPos(editingSweepParam_, sweepClipboard_.endValue);
                sweepClipboardValid_ = true;
                setStatus(L"Sweep copied to clipboard.");
                return TRUE;
            }
            if (id == IDC_SWEEP_PASTE) {
                if (!sweepClipboardValid_) { setStatus(L"Clipboard empty."); return TRUE; }
                Sweep pasted = sweepClipboard_;
                pasted.startValue = sweepPosToStored(editingSweepParam_, pasted.startValue);
                pasted.endValue   = sweepPosToStored(editingSweepParam_, pasted.endValue);
                populateSweepDialog(dlg, editingSweepParam_, patternLengths(),
                                    pasted, list, editingSweepIndex_);
                setStatus(L"Pasted — press Add to commit.");
                return TRUE;
            }
            if (id == IDC_SWEEP_SAVE) {
                // Save the current slider's sweep list to a .hape file.
                wchar_t path[MAX_PATH]{};
                OPENFILENAMEW ofn{ sizeof(ofn) };
                ofn.hwndOwner = dlg;
                ofn.lpstrFilter = L"Screambox sweeps (*.hape)\0*.hape\0";
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
                    sw.hold     = s.hold;
                    sw.osc      = s.osc;
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
                ofn.lpstrFilter = L"Screambox sweeps (*.hape)\0*.hape\0All files\0*.*\0";
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
                    ui.hold         = sw.hold;
                    ui.osc          = sw.osc;
                    list.push_back(ui);
                }
                editingSweepIndex_ = 0;
                Sweep show = list.empty() ? Sweep{} : list.front();
                populateSweepDialog(dlg, editingSweepParam_, patternLengths(),
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
            if (id == IDC_SWEEP_GENERATE) {
                // Fills the range in the fields above with the chosen shape,
                // drawn between the start and end values and repeated Cycles
                // times. What it adds are ordinary sweeps.
                const std::vector<int> lens = patternLengths();
                const Sweep range = readSweepFromDialog(dlg, editingSweepParam_);
                const long a0 = absStepOf(lens, range.startPattern, range.startStep);
                const long a1 = absStepOf(lens, range.endPattern,   range.endStep);
                if (a1 < a0) {
                    setStatus(L"The end is before the start. Set the range the shape should fill, then Generate.");
                    return TRUE;
                }
                int shapeSel = (int)SendMessageW(GetDlgItem(dlg, IDC_SWEEP_SHAPE), CB_GETCURSEL, 0, 0);
                if (shapeSel < 0) shapeSel = 0;
                int cycles = (int)SendMessageW(GetDlgItem(dlg, IDC_SWEEP_CYCLES), CB_GETCURSEL, 0, 0) + 1;
                if (cycles < 1) cycles = 1;
                const SweepShape shape = (SweepShape)shapeSel;
                const long n = a1 - a0 + 1;
                // A ramp needs one step a cycle; the other two need two, or
                // there is no second half to go up-and-down or alternate in.
                const long perCycle = (shape == SweepShape::Ramp) ? 1 : 2;
                if (n < (long)cycles * perCycle) {
                    wchar_t m[200];
                    swprintf(m, 200, L"The range is %ld step%s, too short for %d cycle%s of this shape. "
                                     L"Use fewer cycles or a longer range.",
                             n, n == 1 ? L"" : L"s", cycles, cycles == 1 ? L"" : L"s");
                    setStatus(m);
                    return TRUE;
                }
                const int va = (int)SendMessageW(GetDlgItem(dlg, IDC_SWEEP_START_VALUE), TBM_GETPOS, 0, 0);
                const int vb = (int)SendMessageW(GetDlgItem(dlg, IDC_SWEEP_END_VALUE),   TBM_GETPOS, 0, 0);
                std::vector<Sweep> made = shapeSweeps(editingSweepParam_, lens, a0, n,
                                                      shape, cycles, va, vb);
                // Hold, if ticked, belongs to the shape's end - the pieces in
                // between are followed at once by the next one anyway.
                made.back().hold = range.hold;

                for (const Sweep& s : made) {
                    for (size_t k = 0; k < list.size(); ++k) {
                        if (sweepsOverlap(list[k], s)) {
                            wchar_t m[160];
                            swprintf(m, 160, L"The shape would overlap sweep %zu. Remove it or choose another range.",
                                     k + 1);
                            setStatus(m);
                            return TRUE;
                        }
                    }
                }
                const int first = (int)list.size();
                list.insert(list.end(), made.begin(), made.end());
                editingSweepIndex_ = first;
                populateSweepDialog(dlg, editingSweepParam_, lens,
                                    list[first], list, editingSweepIndex_);
                wchar_t m[200];
                swprintf(m, 200, L"Added %zu sweep%s, numbers %d to %d: %s, %d cycle%s.",
                         made.size(), made.size() == 1 ? L"" : L"s", first + 1, (int)list.size(),
                         kShapeNames[shapeSel], cycles, cycles == 1 ? L"" : L"s");
                setStatus(m);
                return TRUE;
            }
            if (id == IDCANCEL) {
                list = sweepSnapshot_;   // openSweepEditor() pushes whichever list remains
                EndDialog(dlg, id);
                return TRUE;
            }
            if (id == IDOK) {
                EndDialog(dlg, id);
                return TRUE;
            }
            break;
        }
    }
    return FALSE;
}

// A new sweep starts at the slider's current setting, at both ends: a
// frequency target has no sensible zero to start from, and for the rest
// "where the slider is now" beats "the bottom of the range".
int MainWindow::newSweepValue() const {
    return sweepStoredFromParam(editingSweepParam_,
        engine_.getParam(CBEngine::sweepTargetParam(editingSweepParam_)));
}

// -------------------- Sweep plumbing --------------------

void MainWindow::pushSweeps(CBEngine& target, int sliderIdx) const {
    if (sliderIdx < 0 || sliderIdx >= CBEngine::kSweepTargets) return;
    std::vector<CBEngine::ParamSweep> es;
    es.reserve(paramSweeps_[sliderIdx].size());
    for (const auto& s : paramSweeps_[sliderIdx]) {
        CBEngine::ParamSweep e;
        e.startPat = s.startPattern; e.startStep = s.startStep; e.startVal = s.startValue;
        e.endPat   = s.endPattern;   e.endStep   = s.endStep;   e.endVal   = s.endValue;
        e.hold     = s.hold;
        e.osc      = s.osc;
        es.push_back(e);
    }
    target.setParamSweeps(sliderIdx, es);
}

void MainWindow::pushAllSweeps(CBEngine& target) const {
    for (int k = 0; k < CBEngine::kSweepTargets; ++k) pushSweeps(target, k);
}

size_t MainWindow::clearAllSweeps() {
    size_t n = 0;
    for (auto& list : paramSweeps_) { n += list.size(); list.clear(); }
    pushAllSweeps(engine_);
    refreshSweptLabels();
    return n;
}

// " (2 sweeps)", " (1 LFO)", " (2 sweeps, 1 LFO)", or "" - the LFOs counted
// apart, since a slider with an LFO on it is still the centre the LFO moves
// around, while one with a straight sweep is overridden.
std::wstring MainWindow::sweepCountSuffix(int target) const {
    size_t straight = 0, lfo = 0;
    for (const auto& s : paramSweeps_[target]) (s.osc.on() ? lfo : straight)++;
    std::wstring out;
    wchar_t buf[48];
    if (straight) {
        swprintf(buf, 48, L"%zu sweep%s", straight, straight == 1 ? L"" : L"s");
        out = buf;
    }
    if (lfo) {
        swprintf(buf, 48, L"%s%zu LFO%s", straight ? L", " : L"", lfo, lfo == 1 ? L"" : L"s");
        out += buf;
    }
    return out.empty() ? out : L" (" + out + L")";
}

std::wstring MainWindow::sweptLabel(const std::wstring& base, int target) const {
    const std::wstring suffix = sweepCountSuffix(target);
    if (suffix.empty()) return base;
    std::wstring s = base;
    const bool colon = !s.empty() && s.back() == L':';
    if (colon) s.pop_back();
    s += suffix;
    if (colon) s += L':';
    return s;
}

void MainWindow::refreshSweptLabels() {
    // Accent squelch is target 7 and sits apart from the row of seven.
    if (hAccentSquelch_ && hAccentSquelchLabel_) {
        const std::wstring label = sweptLabel(L"Accent s&quelch:", 7);
        const std::wstring plain = sweptLabel(L"Accent squelch", 7);
        SetWindowTextW(hAccentSquelchLabel_, label.c_str());
        NotifyWinEvent(EVENT_OBJECT_NAMECHANGE, hAccentSquelchLabel_, OBJID_CLIENT, CHILDID_SELF);
        SetWindowTextW(hAccentSquelch_, plain.c_str());
        NotifyWinEvent(EVENT_OBJECT_NAMECHANGE, hAccentSquelch_, OBJID_CLIENT, CHILDID_SELF);
    }
    for (int i = 0; i < 7; ++i) {
        if (!hSliders_[i] || !hSliderLabels_[i]) continue;
        const std::wstring suffix = sweepCountSuffix(i);
        const std::wstring label = sliderLabelBase_[i] + suffix;
        const std::wstring plain = sliderPlainBase_[i] + suffix;
        // Both: the STATIC is what sighted users read and what the dialog
        // manager pairs with the trackbar, and the trackbar's own text is the
        // name some screen readers take first.
        SetWindowTextW(hSliderLabels_[i], label.c_str());
        NotifyWinEvent(EVENT_OBJECT_NAMECHANGE, hSliderLabels_[i], OBJID_CLIENT, CHILDID_SELF);
        SetWindowTextW(hSliders_[i], plain.c_str());
        NotifyWinEvent(EVENT_OBJECT_NAMECHANGE, hSliders_[i], OBJID_CLIENT, CHILDID_SELF);
    }
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

// Every label is set here because each says how many sweeps its slider
// carries. The base texts repeat resource.rc's; keep the two in step
// (tools/check_keyboard.py reads the mnemonics from the .rc).
//
// The PWM LFO's group used to live here too, with depth labels that said
// "(saw - no effect)" when their oscillator was a saw. The LFO is an
// oscillating sweep now (TODO Y3), edited in the sweep editor like any other.
void MainWindow::refreshOscLabels(HWND dlg) {
    const struct { int id; const wchar_t* base; int target; } labels[] = {
        {IDC_OSC_PWM_LABEL,        L"Main &PWM (square duty):",  8},
        {IDC_OSC_SUB_LABEL,        L"Sub sine &level (-1 oct):", 9},
        {IDC_OSC_NOISE_LABEL,      L"N&oise level:",             10},
        {IDC_OSC_DUP_PWM_LABEL,    L"P&WM:",                     11},
        {IDC_OSC_DUP_DETUNE_LABEL, L"&Detune:",                  12},
        {IDC_OSC_DUP_LEVEL_LABEL,  L"Le&vel:",                   13},
    };
    for (const auto& l : labels) setDlgLabelIfChanged(dlg, l.id, sweptLabel(l.base, l.target));
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
            // lround: the +0.5-and-truncate the other sliders use is wrong for
            // negative values, and detune is the one that has them.
            setDetuneTrack(dlg,
                static_cast<int>(std::lround(engine_.getParam(CbParam::DupletDetune) * 1200.0f)));
            setTrackAndLabel(dlg, IDC_OSC_DUP_LEVEL, IDC_OSC_DUP_LEVEL_VALUE,
                static_cast<int>(engine_.getParam(CbParam::DupletLevel) * 100.0f + 0.5f));
            HWND wv = GetDlgItem(dlg, IDC_OSC_DUP_WAVEFORM);
            SendMessageW(wv, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Saw"));
            SendMessageW(wv, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Square"));
            SendMessageW(wv, CB_SETCURSEL,
                engine_.getParam(CbParam::DupletWaveform) > 0.5f ? 1 : 0, 0);
            for (int id : kOscSweepSliders) makeSliderSweepable(GetDlgItem(dlg, id));
            refreshOscLabels(dlg);
            SetFocus(GetDlgItem(dlg, IDC_OSC_PWM));
            return FALSE;
        }
        case WM_APP + 1: {
            // Enter on a slider: its sweep editor, modal to this dialog.
            const int target = sweepTargetForCtrl(static_cast<int>(wp));
            if (target >= 0) {
                openSweepEditor(target, dlg);
                refreshOscLabels(dlg);
            }
            return TRUE;
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
            int lblId = 0; CbParam pm = CbParam::NumParams;
            if (which == GetDlgItem(dlg, IDC_OSC_PWM))        { pm = CbParam::PWM;         lblId = IDC_OSC_PWM_VALUE; }
            else if (which == GetDlgItem(dlg, IDC_OSC_SUB))   { pm = CbParam::SubOscLevel; lblId = IDC_OSC_SUB_VALUE; }
            else if (which == GetDlgItem(dlg, IDC_OSC_NOISE)) { pm = CbParam::NoiseLevel;  lblId = IDC_OSC_NOISE_VALUE; }
            else if (which == GetDlgItem(dlg, IDC_OSC_DUP_PWM))    { pm = CbParam::DupletPWM;    lblId = IDC_OSC_DUP_PWM_VALUE; }
            else if (which == GetDlgItem(dlg, IDC_OSC_DUP_LEVEL))  { pm = CbParam::DupletLevel;  lblId = IDC_OSC_DUP_LEVEL_VALUE; }
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
                refreshOscLabels(dlg);
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
                refreshOscLabels(dlg);
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

        for (int id : kFxSweepSliders) makeSliderSweepable(GetDlgItem(dlg, id));
        refreshDistLabels(dlg);
        return TRUE;
    }
    case WM_APP + 1: {
        // Enter on a slider: its sweep editor, modal to this dialog.
        const int target = sweepTargetForCtrl(static_cast<int>(wp));
        if (target >= 0) {
            openSweepEditor(target, dlg);
            refreshDistLabels(dlg);
        }
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
// discoverable cause), say so in their labels - the accessibility rule in
// CLAUDE.md.
//
// Every label is set here, because each also says how many sweeps its slider
// carries. The base texts repeat resource.rc's; keep the two in step.
void MainWindow::refreshDistLabels(HWND dlg) {
    const bool classic = (int)(engine_.getParam(CbParam::DistType) + 0.5f) == 0;
    // Same treatment for the two knobs that do nothing while their effect is
    // switched off. Threshold with depth at zero, and carrier frequency with
    // amount at zero, are both live controls with no audible result, which is
    // exactly the situation that needs saying out loud rather than greying out.
    // (A sweep on the depth or amount can switch the effect on while the
    // slider says off; the label describes the slider, which is what Save
    // stores and what plays wherever no sweep covers.)
    const bool noComp = engine_.getParam(CbParam::CompDepth)  <= 0.0f;
    const bool noRing = engine_.getParam(CbParam::RingAmount) <= 0.0f;
    const struct { int id; const wchar_t* base; int target; } labels[] = {
        {IDC_DIST_GAIN_LABEL,  classic ? L"&Gain (no effect - Classic):"  : L"&Gain:",  17},
        {IDC_DIST_TONE_LABEL,  classic ? L"T&one (no effect - Classic):"  : L"T&one:",  18},
        {IDC_DIST_LEVEL_LABEL, classic ? L"&Level (no effect - Classic):" : L"&Level:", 19},
        {IDC_FX_COMP_THRESH_LABEL,
            noComp ? L"Th&reshold (no effect - depth off):" : L"Th&reshold:", 20},
        {IDC_FX_COMP_DEPTH_LABEL,  L"&Depth:",  21},
        {IDC_FX_RING_HZ_LABEL,
            noRing ? L"&Frequency (no effect - amount off):" : L"&Frequency:", 22},
        {IDC_FX_RING_AMOUNT_LABEL, L"&Amount:", 23},
    };
    for (const auto& l : labels) setDlgLabelIfChanged(dlg, l.id, sweptLabel(l.base, l.target));
}

// -------------------- Modulation dialog --------------------
//
// Dengo's sub-editor (TODO Y3): every sweep target in one place, read with its
// counts - "Cutoff (2 sweeps, 1 LFO)" - so a song's modulation can be found
// without opening three windows and Tabbing to each slider. Enter edits the
// selected target's sweeps in the ordinary sweep editor, and a preset places
// a whole-song LFO on it. An LFO is an oscillating sweep (CBEngine::SweepOsc),
// so what the presets make can be edited, saved in a .hape and removed like
// any other sweep.

// A preset is a shape, a rate and a depth RELATIVE to the target's range, so
// one list serves every target: a quarter of Cutoff's travel and a quarter of
// Duplet detune's are both "a quarter".
struct LfoPreset {
    const wchar_t* name;
    CBEngine::OscShape shape;
    bool  hz;
    float rate;      // steps per cycle, or Hz
    double depth;    // fraction of the target's range
};
static const LfoPreset kLfoPresets[] = {
    {L"Slow wobble: triangle, one cycle every 4 bars, a quarter of the range",
        CBEngine::OscShape::Triangle, false, 64.0f, 0.25},
    {L"Bar wobble: sine, one cycle a bar, a quarter of the range",
        CBEngine::OscShape::Sine,     false, 16.0f, 0.25},
    {L"Half-bar wobble: sine, 8 steps a cycle, a quarter of the range",
        CBEngine::OscShape::Sine,     false, 8.0f,  0.25},
    {L"Beat wobble: triangle, 4 steps a cycle, a fifth of the range",
        CBEngine::OscShape::Triangle, false, 4.0f,  0.2},
    {L"Trill: square, 2 steps a cycle, a tenth of the range",
        CBEngine::OscShape::Square,   false, 2.0f,  0.1},
    {L"Rise every bar: saw up, 16 steps a cycle, a quarter of the range",
        CBEngine::OscShape::SawUp,    false, 16.0f, 0.25},
    {L"Fall every bar: saw down, 16 steps a cycle, a quarter of the range",
        CBEngine::OscShape::SawDown,  false, 16.0f, 0.25},
    // What the Oscillator dialog's PWM LFO did at a typical setting, for
    // anyone who used it there: Main PWM or Duplet PWM, then this.
    {L"The old PWM LFO: triangle, 2 Hz, 30 percent of the range",
        CBEngine::OscShape::Triangle, true,  2.0f,  0.30},
};

// A fraction of the target's range as a depth in its stored units. The ring
// carrier's slider spans 5 Hz to 20 kHz on a taper, which no linear fraction
// suits; its depth is a fraction of 400 Hz instead.
static int lfoPresetDepth(int target, double frac) {
    int span = 100;
    switch (kSweepUi[target].kind) {
        case SweepKind::MainBi:  span = 200;   break;
        case SweepKind::Squelch: span = 200;   break;
        case SweepKind::Cents:   span = 2400;  break;
        case SweepKind::RingHz:  span = 40000; break;   // hundredths of a Hz
        default: break;
    }
    const int d = (int)std::lround(frac * span);
    return d < 1 ? 1 : d;
}

void MainWindow::openModulationDialog() {
    DialogBoxParamW(hInst_, MAKEINTRESOURCEW(IDD_MODULATION), hwnd_,
                    &MainWindow::modDialogProc, reinterpret_cast<LPARAM>(this));
}

INT_PTR CALLBACK MainWindow::modDialogProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(dlg, GWLP_USERDATA));
    if (msg == WM_INITDIALOG) {
        self = reinterpret_cast<MainWindow*>(lp);
        SetWindowLongPtrW(dlg, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return FALSE;
    return self->modDialogHandler(dlg, msg, wp, lp);
}

// Refills the target list, keeping the selection. Item data is the target
// number; the retired targets are not listed.
void MainWindow::refreshModList(HWND dlg) {
    HWND lb = GetDlgItem(dlg, IDC_MOD_LIST);
    int sel = (int)SendMessageW(lb, LB_GETCURSEL, 0, 0);
    if (sel < 0) sel = 0;
    SendMessageW(lb, WM_SETREDRAW, FALSE, 0);
    SendMessageW(lb, LB_RESETCONTENT, 0, 0);
    for (int k = 0; k < CBEngine::kSweepTargets; ++k) {
        if (CBEngine::sweepTargetRetired(k)) continue;
        const std::wstring text = std::wstring(kSweepUi[k].name) + sweepCountSuffix(k);
        const int at = (int)SendMessageW(lb, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
        SendMessageW(lb, LB_SETITEMDATA, at, k);
    }
    SendMessageW(lb, WM_SETREDRAW, TRUE, 0);
    SendMessageW(lb, LB_SETCURSEL, sel, 0);
    InvalidateRect(lb, nullptr, TRUE);
    NotifyWinEvent(EVENT_OBJECT_VALUECHANGE, lb, OBJID_CLIENT, CHILDID_SELF);
}

INT_PTR MainWindow::modDialogHandler(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    (void)lp;
    auto selectedTarget = [&]() -> int {
        HWND lb = GetDlgItem(dlg, IDC_MOD_LIST);
        const int sel = (int)SendMessageW(lb, LB_GETCURSEL, 0, 0);
        return sel < 0 ? -1 : (int)SendMessageW(lb, LB_GETITEMDATA, sel, 0);
    };
    switch (msg) {
        case WM_INITDIALOG: {
            refreshModList(dlg);
            HWND pr = GetDlgItem(dlg, IDC_MOD_PRESET);
            for (const auto& p : kLfoPresets)
                SendMessageW(pr, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(p.name));
            SendMessageW(pr, CB_SETCURSEL, 1, 0);   // bar wobble
            setDlgLabel(dlg, IDC_MOD_STATUS,
                L"An LFO swings a slider up and down by itself. Enter edits the chosen slider's "
                L"sweeps; F1 explains.");
            SetFocus(GetDlgItem(dlg, IDC_MOD_LIST));
            return FALSE;
        }
        case WM_COMMAND: {
            const int id = LOWORD(wp);
            const int notif = HIWORD(wp);
            // Enter on the list presses the default button, Edit sweeps -
            // IDOK is what the dialog manager sends for it.
            if ((id == IDC_MOD_LIST && notif == LBN_DBLCLK) || id == IDC_MOD_EDIT || id == IDOK) {
                const int k = selectedTarget();
                if (k >= 0) {
                    openSweepEditor(k, dlg);
                    refreshModList(dlg);
                    SetFocus(GetDlgItem(dlg, IDC_MOD_LIST));
                }
                return TRUE;
            }
            if (id == IDC_MOD_ADD_LFO) {
                const int k = selectedTarget();
                const int pi = (int)SendMessageW(GetDlgItem(dlg, IDC_MOD_PRESET), CB_GETCURSEL, 0, 0);
                if (k < 0 || pi < 0) return TRUE;
                const LfoPreset& p = kLfoPresets[pi];
                const std::vector<int> lens = patternLengths();
                Sweep sw;
                sw.startPattern = 0;
                sw.startStep    = 0;
                sw.endPattern   = lens.empty() ? 0 : (int)lens.size() - 1;
                sw.endStep      = lens.empty() ? 15 : lens.back() - 1;
                sw.startValue   = sw.endValue = lfoPresetDepth(k, p.depth);
                // Held, so it keeps going through patterns added later.
                sw.hold         = true;
                sw.osc.shape    = (int)p.shape;
                sw.osc.hz       = p.hz;
                sw.osc.rate     = p.rate;
                auto& list = paramSweeps_[k];
                for (size_t i = 0; i < list.size(); ++i) {
                    if (sweepsOverlap(list[i], sw)) {
                        wchar_t m[200];
                        swprintf(m, 200, L"%s already has an LFO (sweep %zu). Edit or remove it first.",
                                 kSweepUi[k].name, i + 1);
                        setDlgLabel(dlg, IDC_MOD_STATUS, m);
                        return TRUE;
                    }
                }
                list.push_back(sw);
                pushSweeps(engine_, k);
                refreshSweptLabels();
                refreshModList(dlg);
                wchar_t dv[24]; sweepValueLabelFor(k, sw.startValue, dv, 24);
                wchar_t m[260];
                swprintf(m, 260, L"Added to %s: %s, depth %s, the whole song. Edit sweeps to change it.",
                         kSweepUi[k].name, p.name, dv);
                setDlgLabel(dlg, IDC_MOD_STATUS, m);
                return TRUE;
            }
            if (id == IDC_MOD_HELP) {
                MessageBoxW(dlg, kModulationHelp, L"Modulation", MB_OK | MB_ICONINFORMATION);
                return TRUE;
            }
            if (id == IDCANCEL) { EndDialog(dlg, id); return TRUE; }
            break;
        }
    }
    return FALSE;
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
    // Retired with the PWM LFO (v15) and read by nothing; zeroed so a song
    // re-saved after this writes what a fresh one does. The LFO itself is
    // an oscillating sweep now, and like every sweep it is left alone here.
    engine_.setParam(CbParam::PwmLfoRate,        0.0f);
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

void MainWindow::openSweepEditor(int sliderIdx, HWND owner) {
    if (sliderIdx < 0 || sliderIdx >= CBEngine::kSweepTargets) return;
    editingSweepParam_ = sliderIdx;
    editingSweepIndex_ = 0;
    DialogBoxParamW(hInst_, MAKEINTRESOURCEW(IDD_SWEEP_EDIT), owner ? owner : hwnd_,
                    &MainWindow::sweepEditorProc,
                    reinterpret_cast<LPARAM>(this));

    // Push the UI's sweep list into the engine so it modulates params live.
    // After Cancel this is the snapshot taken when the dialog opened.
    pushSweeps(engine_, sliderIdx);
    refreshSweptLabels();

    char sb[128];
    _snprintf_s(sb, _TRUNCATE, "Sweep editor closed for %s. %zu sweep(s) stored.",
        fromWide(kSweepUi[sliderIdx].name).c_str(), paramSweeps_[sliderIdx].size());
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
        swprintf(title, 160, L"Screambox — editing pattern %d of %d", editing, total);
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
        int pos = static_cast<int>(std::lround(engine_.getParam(kIds[i]) * 100.0f));
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
        // Inverse of the slider map: sq 0.0..2.0 → slider 0..200.
        float sq = engine_.getParam(CbParam::AccentSquelch);
        if (sq < 0.0f) sq = 0.0f;
        if (sq > 2.0f) sq = 2.0f;
        int pos = static_cast<int>(std::lround(sq * 100.0f));
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
    // Inserted after the EDITED pattern, without touching playback: this used
    // to select the edited pattern as the playback position first, so adding a
    // pattern while the song played threw the playhead onto the new empty one.
    const int newIdx = engine_.insertPatternAfter(editingIdx_);
    // Sweeps address patterns by index, so every endpoint at or after the new
    // slot moves up one - otherwise each sweep after it silently lands one
    // pattern early. A sweep spanning the insertion point stretches over the
    // new pattern, which is what "insert a bar in the middle" means.
    int moved = 0;
    for (auto& list : paramSweeps_) {
        for (auto& s : list) {
            bool m = false;
            if (s.startPattern >= newIdx) { ++s.startPattern; m = true; }
            if (s.endPattern   >= newIdx) { ++s.endPattern;   m = true; }
            moved += m;
        }
    }
    if (moved) pushAllSweeps(engine_);
    editingIdx_ = newIdx;
    populateStepList();
    refreshPatternLabel();
    char sb[160]; _snprintf_s(sb, _TRUNCATE, "New empty pattern inserted at position %d of %d.%s",
                              newIdx + 1, engine_.patternCount(),
                              moved ? " Sweeps after it moved with their patterns." : "");
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
    // A new song, so the old song's sweeps go too - they named patterns this
    // one does not have.
    const size_t cleared = clearAllSweeps();
    editingIdx_ = 0;
    populateStepList();
    refreshPatternLabel();
    char sb[160];
    _snprintf_s(sb, _TRUNCATE, "New random pattern created (%s).%s",
                flavorName(lastRandomFlavor_), cleared ? " Sweeps cleared." : "");
    announceValue(hStatus_, sb);
}

void MainWindow::startNewTrack() {
    int resp = MessageBoxW(hwnd_,
        L"Clear all patterns and sweeps, and start a fresh track?",
        L"Screambox", MB_OKCANCEL | MB_ICONQUESTION);
    if (resp != IDOK) return;

    engine_.setPatterns({ CbPattern{} });
    editingIdx_ = 0;
    clearAllSweeps();
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
    const int d = editingIdx_;
    // Needed after the delete, to clamp an end point into the pattern before.
    const int prevLen = (d > 0) ? engine_.getPatternAt(d - 1).lastStep + 1 : 0;
    if (!engine_.deletePattern(d)) {
        announceValue(hStatus_, "Can't delete — only one pattern remains.");
        return;
    }
    // Sweeps address patterns by index. Endpoints after the deleted pattern
    // move down one, so every sweep stays on the music it was written for. An
    // endpoint INSIDE it clamps to the nearest surviving boundary - a start to
    // the first step of the pattern that took its place, an end to the last
    // step of the one before - and a sweep lying wholly inside it is removed,
    // since there is nothing left for it to sweep.
    int clamped = 0, removed = 0;
    for (auto& list : paramSweeps_) {
        std::vector<Sweep> kept;
        kept.reserve(list.size());
        for (Sweep s : list) {
            const bool startIn = s.startPattern == d, endIn = s.endPattern == d;
            if ((startIn && endIn) || (endIn && d == 0)) { ++removed; continue; }
            if (startIn)                  { s.startStep = 0; ++clamped; }
            else if (s.startPattern > d)  { --s.startPattern; }
            if (endIn)                    { s.endPattern = d - 1; s.endStep = prevLen - 1;
                                            if (!startIn) ++clamped; }
            else if (s.endPattern > d)    { --s.endPattern; }
            kept.push_back(s);
        }
        list.swap(kept);
    }
    pushAllSweeps(engine_);
    refreshSweptLabels();
    clampEditingIdx();
    populateStepList();
    refreshPatternLabel();
    char extra[96] = "";
    if (clamped || removed)
        _snprintf_s(extra, _TRUNCATE, " %d sweep%s clamped to its neighbours, %d removed.",
                    clamped, clamped == 1 ? "" : "s", removed);
    char sb[200]; _snprintf_s(sb, _TRUNCATE, "Deleted pattern %d. Now editing pattern %d of %d.%s",
                               d + 1, editingIdx_ + 1, engine_.patternCount(), extra);
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
        // Slider 0..200 → sq 0.0..2.0. 100 is Frank's original TB-303 accent
        // behavior, 0 no stacking, 200 doubles the buildup on successive accents.
        float sq = pos / 100.0f;
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
        MessageBoxW(hwnd_, toWide("Failed to load: " + err).c_str(), L"Screambox", MB_OK | MB_ICONERROR);
        return;
    }
    if (!preset->patterns.empty()) {
        engine_.setPatterns(preset->patterns);
    }
    editingIdx_ = 0;
    engine_.setParam(CbParam::Tempo, preset->tempo);
    // Importing a module starts a new song: the previous song's interactive
    // sweeps would otherwise keep playing over it, aimed at its patterns.
    const size_t cleared = clearAllSweeps();
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
    _snprintf_s(sb, _TRUNCATE, "Loaded %s (%zu patterns, tempo %d).%s",
        fromWide(PathFindFileNameW(path)).c_str(),
        preset->patterns.size(), static_cast<int>(preset->tempo),
        cleared ? " Sweeps cleared." : "");
    announceValue(hStatus_, sb);
}


// Render `engine` (already set up and running) straight into a 16-bit mono PCM
// WAV, a block at a time. Both output channels always carry identical
// samples, so only the left one is written. Streaming rather than rendering
// the whole song into memory first keeps a long song from needing hundreds of
// megabytes, and every write is checked - a full disk used to produce a
// truncated file and a "Rendered" message.
static bool renderWavFile16(const std::wstring& path, CBEngine& engine,
                            long long totalSamples, int sampleRate, std::string& err) {
    const uint16_t channels = 1;
    const uint16_t bitsPerSample = 16;
    const long long dataBytes64 = totalSamples * (long long)sizeof(int16_t);
    if (totalSamples <= 0 || dataBytes64 + 36 > 0xFFFFFFFFLL) {
        err = "the song is too long for a WAV file (over 4 GB)";
        return false;
    }
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { err = "cannot create file"; return false; }
    const uint32_t byteRate = sampleRate * channels * (bitsPerSample / 8);
    const uint16_t blockAlign = channels * (bitsPerSample / 8);
    const uint32_t dataBytes = static_cast<uint32_t>(dataBytes64);
    const uint32_t fileBytes = 36 + dataBytes;
    bool ok = true;
    auto w = [&](const void* p, DWORD n) {
        DWORD written = 0;
        if (ok && (!WriteFile(h, p, n, &written, nullptr) || written != n)) ok = false;
    };
    w("RIFF", 4); w(&fileBytes, 4); w("WAVE", 4);
    w("fmt ", 4);
    uint32_t fmtSize = 16; uint16_t fmtTag = 1; // PCM
    w(&fmtSize, 4); w(&fmtTag, 2); w(&channels, 2);
    uint32_t sr = static_cast<uint32_t>(sampleRate);
    w(&sr, 4); w(&byteRate, 4); w(&blockAlign, 2); w(&bitsPerSample, 2);
    w("data", 4); w(&dataBytes, 4);
    const int blockSize = 512;
    std::vector<float> buf(blockSize * 2);
    std::vector<int16_t> chunk(blockSize);
    for (long long pos = 0; ok && pos < totalSamples; pos += blockSize) {
        const int n = (int)std::min<long long>(blockSize, totalSamples - pos);
        engine.process(buf.data(), n);
        for (int i = 0; i < n; ++i) {
            float v = buf[i * 2];
            if (v > 1.0f) v = 1.0f;
            if (v < -1.0f) v = -1.0f;
            chunk[i] = static_cast<int16_t>(v * 32767.0f);
        }
        w(chunk.data(), static_cast<DWORD>(n * sizeof(int16_t)));
    }
    CloseHandle(h);
    if (!ok) { err = "write failed (disk full?)"; DeleteFileW(path.c_str()); }
    return ok;
}

void MainWindow::saveWavFile() {
    // Start from the song's own name and folder. The old default was
    // c:\cb303.wav, and the root of C: is not writable without elevation.
    wchar_t path[MAX_PATH]{};
    if (!currentFile_.empty()) {
        wcscpy_s(path, currentFile_.c_str());
        PathRenameExtensionW(path, L".wav");
    } else {
        wcscpy_s(path, L"screambox.wav");
    }
    OPENFILENAMEW ofn{ sizeof(ofn) };
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFilter = L"Wave audio (*.wav)\0*.wav\0";
    ofn.lpstrDefExt = L"wav";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&ofn)) return;

    // Render the full sequence once through at the current audio sample rate,
    // on a private engine so the live one keeps playing. It gets the WHOLE
    // song: patterns, every parameter, and every sweep - it used to get no
    // sweeps at all, so an exported track never moved.
    const double sampleRate = audio_.sampleRate();
    CBEngine renderEngine;
    renderEngine.setSampleRate(sampleRate);
    std::vector<CbPattern> pats;
    for (int i = 0; i < engine_.patternCount(); ++i) pats.push_back(engine_.getPatternAt(i));
    renderEngine.setPatterns(pats);
    for (int p = 0; p < int(CbParam::NumParams); ++p) {
        if (p == int(CbParam::RunStop)) continue;   // transport is set below
        renderEngine.setParam(static_cast<CbParam>(p), engine_.getParam(static_cast<CbParam>(p)));
    }
    pushAllSweeps(renderEngine);

    // Length: the song once through, with the sequencer's own arithmetic, so
    // pattern lengths and triplet feel count. It used to assume every pattern
    // was 16 straight steps: a song of 4-step patterns rendered four times
    // over, and a 64-step one stopped a quarter of the way in.
    const float bpm = engine_.getParam(CbParam::Tempo);
    const long long totalSamples = CBEngine::songLengthSamples(pats, sampleRate, bpm);

    renderEngine.setPlayMode(CBEngine::PlayMode::FullSequence);
    renderEngine.setRunning(true, /*restartFromFirstPattern=*/true);
    std::string err;
    const bool ok = renderWavFile16(path, renderEngine, totalSamples,
                                    static_cast<int>(sampleRate), err);
    renderEngine.setRunning(false);
    if (!ok) {
        MessageBoxW(hwnd_, toWide("WAV save failed: " + err).c_str(),
                    L"Screambox", MB_OK | MB_ICONERROR);
        return;
    }
    char sb[256];
    _snprintf_s(sb, _TRUNCATE, "Rendered %lld samples (%0.1f sec) to %s.",
        totalSamples, totalSamples / sampleRate,
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
        MessageBoxW(hwnd_, toWide("Save failed: " + err).c_str(), L"Screambox", MB_OK | MB_ICONERROR);
        return;
    }
    currentFile_ = path;
    announceValue(hStatus_, "Saved.");
}

void MainWindow::loadRoqFile() {
    wchar_t path[MAX_PATH]{};
    OPENFILENAMEW ofn{ sizeof(ofn) };
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFilter = L"Screambox song (*.roq)\0*.roq\0All files\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&ofn)) return;

    std::string err;
    auto song = loadRoq(fromWide(path), err);
    if (!song) {
        MessageBoxW(hwnd_, toWide("Failed to load: " + err).c_str(), L"Screambox", MB_OK | MB_ICONERROR);
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
    // Retired (v15): loadRoq() has already folded a running LFO into
    // oscillating sweeps and zeroed these. What is left is a rate-0 song's
    // inert depths, carried through untouched so it re-saves to its own bytes.
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
    for (auto& list : paramSweeps_) list.clear();
    for (const auto& sw : song->sweeps) {
        if (sw.sliderIdx < 0 || sw.sliderIdx >= CBEngine::kSweepTargets) continue;
        Sweep ui;
        ui.startPattern = sw.startPat; ui.startStep = sw.startStep; ui.startValue = sw.startVal;
        ui.endPattern   = sw.endPat;   ui.endStep   = sw.endStep;   ui.endValue   = sw.endVal;
        ui.hold         = sw.hold;
        ui.osc          = sw.osc;
        paramSweeps_[sw.sliderIdx].push_back(ui);
    }
    pushAllSweeps(engine_);
    refreshSweptLabels();

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
    ofn.lpstrFilter = L"Screambox song (*.roq)\0*.roq\0";
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

    for (int k = 0; k < CBEngine::kSweepTargets; ++k) {
        for (const auto& s : paramSweeps_[k]) {
            RoqSweep sw;
            sw.sliderIdx = k;
            sw.startPat = s.startPattern; sw.startStep = s.startStep; sw.startVal = s.startValue;
            sw.endPat   = s.endPattern;   sw.endStep   = s.endStep;   sw.endVal   = s.endValue;
            sw.hold     = s.hold;
            sw.osc      = s.osc;
            song.sweeps.push_back(sw);
        }
    }

    std::string err;
    if (!saveRoq(fromWide(path), song, err)) {
        MessageBoxW(hwnd_, toWide("Save failed: " + err).c_str(), L"Screambox", MB_OK | MB_ICONERROR);
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
        L"   These four work inside the sweep editor and the other dialogs too.\r\n"
        L"\r\nGlobal:\r\n"
        L"   Ctrl+O: Open .roq song\r\n"
        L"   Ctrl+S: Save .roq song\r\n"
        L"   Ctrl+D (or Alt+X): Acid FX - distortion, compressor, ring mod\r\n"
        L"   Ctrl+M: Modulation - every sweep target, and LFO presets\r\n"
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
        L"   Enter opens the sweep editor for that slider - on the main window\r\n"
        L"   and in the Oscillator and Acid FX dialogs (Alt+C or Esc closes those).\r\n"
        L"Sweep editor: Alt+K shape, Alt+Y cycles, Alt+G generates the shape\r\n"
        L"   over the range and values above it.\r\n"
        L"Bipolar sliders (Rezo, EnvMod, Tuning) accept negative values.\r\n"
        L"Screen readers announce every control's value on change.",
        L"Keyboard reference",
        MB_OK | MB_ICONINFORMATION);
}

void MainWindow::showAbout() {
    MessageBoxW(hwnd_,
        // The credit is the point of this box. The sound - filter, oscillator,
        // envelope, accent - is the original CreakBox code, kept bit-identical
        // on purpose, and the name changed so that nobody mistakes this program
        // for that one, not to take credit for it.
        L"Screambox " SCREAMBOX_VER_WSTR L"\r\n"
        L"Accessible bassline synth and step sequencer for Windows.\r\n\r\n"
        L"Built on CreakBox by Frank Reitz (Bass Assessor), 2002, and its 2004 VST\r\n"
        L"source, published by bioroid media development under a BSD-style licence\r\n"
        L"(license-bmd.txt), with wwelti's DSP optimisations. The filter, oscillator,\r\n"
        L"envelope and accent are theirs.\r\n\r\n"
        L"Screambox, formerly Creakbox: (c) 2026 dengopaiv, BSD 3-Clause.",
        L"About Screambox",
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
        case IDM_OPEN_MODULATION: openModulationDialog();  break;
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
        case IDC_MOD_BUTTON:
            if (notif == BN_CLICKED) openModulationDialog();
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
            if (wp == 1 && audio_.deviceGeneration() != audioGen_) announceAudioDeviceChange();
            if (wp == 1 && engine_.isRunning() && !audio_.deviceLost()) {
                int pat  = engine_.currentPatternIndex();
                int step = engine_.currentStepIndex();
                int pats = engine_.patternCount();
                const int len = engine_.getPatternAt(pat).lastStep + 1;
                char sb[128];
                _snprintf_s(sb, _TRUNCATE,
                            "Playing  pattern %d/%d, step %d/%d  (audio %.0f Hz)",
                            pat + 1, pats, step + 1, len, audio_.sampleRate());
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
            // Enter-on-slider -> open sweep editor for that slider's param:
            // the seven, and Accent squelch.
            const int target = sweepTargetForCtrl(static_cast<int>(wp));
            if (target >= 0) openSweepEditor(target);
            return 0;
        }
        // WM_NOTIFY handling for the old ListView step editor was removed
        // when the inline per-step controls replaced it.
        case WM_DESTROY:
            if (dialogHook_) { UnhookWindowsHookEx(dialogHook_); dialogHook_ = nullptr; }
            hookSelf_ = nullptr;
            PostQuitMessage(0);
            return 0;
        default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Called from the timer when the audio thread has lost or reopened a device.
// One announcement per change; the status line then keeps saying it while no
// device exists, because the transport poll above stands aside until then.
void MainWindow::announceAudioDeviceChange() {
    audioGen_ = audio_.deviceGeneration();
    if (audio_.deviceLost()) {
        announceValue(hStatus_, "Audio device lost. Waiting for an output device.");
        return;
    }
    const std::wstring name = audio_.deviceName();
    char sb[256];
    _snprintf_s(sb, _TRUNCATE, "Audio device changed, now %s at %.0f Hz.",
                name.empty() ? "the default output" : fromWide(name).c_str(),
                audio_.sampleRate());
    announceValue(hStatus_, sb);
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
