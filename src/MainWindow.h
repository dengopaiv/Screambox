#pragma once

#include <windows.h>
#include <string>
#include <vector>

#include "CBEngine.h"

class AudioEngine;

// Accessible main window. Every control is a system control with a preceding
// STATIC label; screen readers announce control name + value out of the box.
class MainWindow {
public:
    MainWindow(CBEngine& engine, AudioEngine& audio, HINSTANCE hInst);

    // Sweep data model: per-parameter list of sweeps. Exposed publicly
    // so sweepEditorProc helpers can reference the type.
    struct Sweep {
        int startPattern = 0, startStep = 0, startValue = 0;
        int endPattern   = 0, endStep   = 15, endValue  = 0;
        bool hold = false;   // keep the end value until the next sweep starts
        // A wave (v15) makes it an LFO: it moves the value instead of setting
        // it, and the two values are its depth. See CBEngine::SweepOsc.
        CBEngine::SweepOsc osc;
    };

    bool create();
    int  runMessageLoop();

private:
    static LRESULT CALLBACK staticProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT proc(HWND, UINT, WPARAM, LPARAM);

    void buildLayout();
    void syncControlsFromEngine();
    void onCommand(WPARAM wp);
    void announceValue(HWND control, const std::string& text);

    // Handlers
    void loadCbFile();
    void saveCbFile();
    void loadRoqFile();
    void saveRoqFile();
    void startNewTrack();
    void toggleTransport();
    void showKeyboardReference();
    void showAbout();

    // Pattern navigation
    void gotoPrevPattern();
    void gotoNextPattern();
    void addPatternAfterCurrent();
    void deleteCurrentPattern();
    void refreshPatternLabel();
    void onPatternLengthChange();
    void toggleCurrentPatternTriplet();
    // Step count of every pattern, in order; never empty. Used by the sweep
    // editor, whose step combos have to offer the steps a pattern actually has.
    std::vector<int> patternLengths() const;

    // Pattern mutation
    void clearCurrentPattern();
    void copyCurrentPattern();
    void cutCurrentPattern();
    void pasteCurrentPattern();
    void newRandomPattern();
    void randomCurrentPattern(CBEngine::RandomFlavor flavor);
    void randomFocusedStep();
    float readWildness() const;
    void transposeCurrentPattern(int semitones);
    void transposeSong(int semitones);
    void toggleFocusedStepAccent();
    void toggleFocusedStepSlide();
    void toggleFocusedStepReverse();

    // Transport modes. F5 song / F6 pattern / F7 from-current / F8 stop;
    // toggleTransport() backs the on-screen button and the menu toggle.
    void playSong();
    void stopTransport();
    void playCurrentPatternOnly();
    void playSequenceFromCurrent();

    // Save to WAV file
    void saveWavFile();

    // Step editor
    void populateStepList();             // sync selector contents
    void loadFocusedStepIntoEditor();    // refresh pitch/slide/accent widgets
    void writeFocusedStepFromEditor();   // commit pitch/slide/accent back to engine
    void onStepSelectorChange();

    CBEngine& engine_;
    AudioEngine& audio_;
    HINSTANCE hInst_ = nullptr;
    HWND hwnd_ = nullptr;
    HACCEL accel_ = nullptr;

    // Named child windows
    HWND hWaveform_ = nullptr;
    HWND hWaveformLabel_ = nullptr;
    HWND hTempo_ = nullptr;
    HWND hTempoLabel_ = nullptr;
    HWND hTransport_ = nullptr;
    HWND hPatternCombo_ = nullptr;   // pattern-selector combobox
    HWND hPatternLength_ = nullptr;  // pattern-length combobox (1..64 steps)
    HWND hPatternTriplet_ = nullptr; // triplet-feel checkbox, same pattern
    HWND hWildness_ = nullptr;       // density slider for random-pattern actions
    HWND hWildnessLabel_ = nullptr;
    HWND hWildnessValue_ = nullptr;
    HWND hAccentSquelch_ = nullptr;  // residual-stacking amount for accent buildup
    HWND hAccentSquelchLabel_ = nullptr;
    HWND hAccentSquelchValue_ = nullptr;
    HWND hResoBoost_ = nullptr;      // Devilfish-style resonance boost, under the filter
    CBEngine::RandomFlavor lastRandomFlavor_ = CBEngine::RandomFlavor::Nicely;
    HWND hStepListLabel_ = nullptr;
    // Focused step editor: one selector + one set of value controls.
    HWND hStepSelector_ = nullptr;  // ListBox with 16 items "Step 1".."Step 16"
    HWND hStepPitch_ = nullptr;     // Combobox: 61 items (Rest + C0..B4)
    HWND hStepSlide_ = nullptr;     // Checkbox
    HWND hStepAccent_ = nullptr;    // Checkbox
    HWND hStepReverse_ = nullptr;   // Checkbox: per-step reverse (filter envelope backwards)
    int  focusedStep_ = 0;          // which step the value controls reflect
    HWND hStatus_ = nullptr;
    // Last AudioEngine::deviceGeneration() announced. The audio thread bumps
    // it when a device is lost or reopened; the 10 Hz timer says so once.
    unsigned audioGen_ = 0;
    void announceAudioDeviceChange();

    // Parameter trackbars (Volume, Cutoff, Resonance, EnvMod, Decay, Accent, Tuning)
    HWND hSliders_[7]{};
    HWND hSliderLabels_[7]{};
    HWND hSliderValues_[7]{};
    // The label and trackbar names as built, before any "(2 sweeps)" suffix.
    std::wstring sliderLabelBase_[7];
    std::wstring sliderPlainBase_[7];
    // Says in each slider's NAME how many sweeps it carries - the name is what
    // a screen reader reads on focus, and without it nothing tells you that
    // moving a swept slider will be overridden at the next step.
    void refreshSweptLabels();
    void updateSliderValue(int idx);
    void onSliderScroll(HWND slider);

    // Pattern the *editor* is focused on. Decoupled from the engine's
    // playback index: during full-sequence playback the engine advances
    // through patterns, but step edits and the pattern combobox stick
    // to whichever pattern the user selected for editing.
    int editingIdx_ = 0;
    void clampEditingIdx();
    std::wstring currentFile_;

    // Per-target list of sweeps (see public Sweep struct). Indexed by sweep
    // target - the seven main sliders, then squelch and the dialog sliders;
    // the numbering is at CBEngine::ParamSweep and is what a .roq stores.
    std::vector<Sweep> paramSweeps_[CBEngine::kSweepTargets];
    // The one place a UI sweep list becomes an engine sweep list. Used for the
    // live engine and for the WAV export's private engine alike - the export
    // once forgot to copy sweeps at all, because every call site had its own
    // hand-written conversion loop.
    void pushSweeps(CBEngine& target, int sliderIdx) const;
    void pushAllSweeps(CBEngine& target) const;
    // Empties every slider's sweep list, in the UI and the engine. Returns how
    // many sweeps there were, so the caller can say so.
    size_t clearAllSweeps();
    // The list as it was when the sweep editor opened, restored on Cancel.
    std::vector<Sweep> sweepSnapshot_;
    Sweep sweepClipboard_;
    bool  sweepClipboardValid_ = false;

    // Whole-pattern clipboard: steps, length and triplet flag together, since
    // those three are what "a pattern" means to the user. In-process only,
    // like sweepClipboard_ above - nothing here touches the system clipboard.
    CbPattern patternClipboard_;
    bool      patternClipboardValid_ = false;
    int   editingSweepParam_ = -1;  // which slider opened the dialog
    int   editingSweepIndex_ = 0;   // currently-viewed sweep in the dialog

    // `owner` is the window the editor is modal to: the main window, or the
    // Oscillator or Acid FX dialog when Enter was pressed on one of theirs.
    void openSweepEditor(int sliderIdx, HWND owner = nullptr);
    // A label as it should read with the target's sweep count in it:
    // "&Gain:" becomes "&Gain (2 sweeps):". Empty count, label unchanged.
    std::wstring sweptLabel(const std::wstring& base, int target) const;
    std::wstring sweepCountSuffix(int target) const;
    static INT_PTR CALLBACK sweepEditorProc(HWND, UINT, WPARAM, LPARAM);
    INT_PTR sweepEditorHandler(HWND, UINT, WPARAM, LPARAM);

    // Distortion dialog (type + gain / tone / level).
    HWND hDistButton_ = nullptr;
    void openDistortionDialog();
    static INT_PTR CALLBACK distDialogProc(HWND, UINT, WPARAM, LPARAM);
    INT_PTR distDialogHandler(HWND, UINT, WPARAM, LPARAM);
    // Sets every slider label in the dialog: gain/tone/level say when Classic
    // makes them inert, and each says how many sweeps its slider carries.
    void refreshDistLabels(HWND dlg);

    // Modulation dialog (TODO Y3): every sweep target in one list with its
    // sweep and LFO counts, Enter to edit one, and LFO presets.
    HWND hModButton_ = nullptr;
    void openModulationDialog();
    static INT_PTR CALLBACK modDialogProc(HWND, UINT, WPARAM, LPARAM);
    INT_PTR modDialogHandler(HWND, UINT, WPARAM, LPARAM);
    void refreshModList(HWND dlg);

    // Oscillator parameter dialog (PWM, sub, duplet, reset-to-303).
    HWND hOscButton_ = nullptr;
    void openOscillatorDialog();
    static INT_PTR CALLBACK oscDialogProc(HWND, UINT, WPARAM, LPARAM);
    INT_PTR oscDialogHandler(HWND, UINT, WPARAM, LPARAM);
    void resetTo303();
    // Sets every slider label in the dialog: the two LFO depth labels say when
    // they are inert (saw), and each says how many sweeps its slider carries.
    void refreshOscLabels(HWND dlg);
};
