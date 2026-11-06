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
    HWND hStepReverse_ = nullptr;   // Checkbox: per-step reverse (EnvMod sign flip)
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
    void updateSliderValue(int idx);
    void onSliderScroll(HWND slider);

    // Pattern the *editor* is focused on. Decoupled from the engine's
    // playback index: during full-sequence playback the engine advances
    // through patterns, but step edits and the pattern combobox stick
    // to whichever pattern the user selected for editing.
    int editingIdx_ = 0;
    void clampEditingIdx();
    std::wstring currentFile_;

    // Per-parameter list of sweeps (see public Sweep struct).
    std::vector<Sweep> paramSweeps_[7];
    Sweep sweepClipboard_;
    bool  sweepClipboardValid_ = false;

    // Whole-pattern clipboard: steps, length and triplet flag together, since
    // those three are what "a pattern" means to the user. In-process only,
    // like sweepClipboard_ above - nothing here touches the system clipboard.
    CbPattern patternClipboard_;
    bool      patternClipboardValid_ = false;
    int   editingSweepParam_ = -1;  // which slider opened the dialog
    int   editingSweepIndex_ = 0;   // currently-viewed sweep in the dialog

    void openSweepEditor(int sliderIdx);
    static INT_PTR CALLBACK sweepEditorProc(HWND, UINT, WPARAM, LPARAM);
    INT_PTR sweepEditorHandler(HWND, UINT, WPARAM, LPARAM);

    // Distortion dialog (type + gain / tone / level).
    HWND hDistButton_ = nullptr;
    void openDistortionDialog();
    static INT_PTR CALLBACK distDialogProc(HWND, UINT, WPARAM, LPARAM);
    INT_PTR distDialogHandler(HWND, UINT, WPARAM, LPARAM);
    // Retitles gain/tone/level to say when Classic makes them inert.
    void refreshDistLabels(HWND dlg);

    // Oscillator parameter dialog (PWM, sub, duplet, reset-to-303).
    HWND hOscButton_ = nullptr;
    void openOscillatorDialog();
    static INT_PTR CALLBACK oscDialogProc(HWND, UINT, WPARAM, LPARAM);
    INT_PTR oscDialogHandler(HWND, UINT, WPARAM, LPARAM);
    void resetTo303();
    // Retitles the two LFO depth labels to say when they are inert (saw).
    void refreshLfoDepthLabels(HWND dlg);
};
