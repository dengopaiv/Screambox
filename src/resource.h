#pragma once

#define IDC_STATIC                  -1

// Menu
#define IDR_MAIN_MENU               100

// Menu commands
#define IDM_FILE_NEW                200
#define IDM_FILE_OPEN_CB            201
#define IDM_FILE_SAVE_CB            203
#define IDM_FILE_SAVE_WAV           207
#define IDM_FILE_EXIT               205
#define IDM_FILE_NEW_TRACK          206
#define IDM_FILE_OPEN_ROQ           208
#define IDM_FILE_SAVE_ROQ           209
#define IDM_EDIT_COPY               210
#define IDM_EDIT_CUT                211
#define IDM_EDIT_PASTE              212
#define IDM_EDIT_CLEAR              217
#define IDM_EDIT_RANDOM_NICELY      270
#define IDM_EDIT_RANDOM_RETRO       271
#define IDM_EDIT_RANDOM_VST         272
#define IDM_EDIT_RANDOM_CHROMATIC   273
#define IDM_PATTERN_TRIPLET         275
#define IDM_EDIT_RANDOM_STEP        274
#define IDM_EDIT_TRANSPOSE_UP       250
#define IDM_EDIT_TRANSPOSE_DOWN     251
#define IDM_EDIT_TRANSPOSE_OCT_UP   252
#define IDM_EDIT_TRANSPOSE_OCT_DOWN 253
#define IDM_EDIT_TRANSPOSE_SONG_UP       254
#define IDM_EDIT_TRANSPOSE_SONG_DOWN     255
#define IDM_EDIT_TRANSPOSE_SONG_OCT_UP   256
#define IDM_EDIT_TRANSPOSE_SONG_OCT_DOWN 257
#define IDM_STEP_TOGGLE_ACCENT      260
#define IDM_STEP_TOGGLE_SLIDE       261
#define IDM_STEP_TOGGLE_REVERSE      262
#define IDM_TRANSPORT_RUN_STOP      220
#define IDM_TRANSPORT_SWEEPS        221
#define IDM_TRANSPORT_PLAY_CURRENT  222
#define IDM_TRANSPORT_PLAY_SEQUENCE 223
#define IDM_TRANSPORT_PLAY_SONG     224
#define IDM_TRANSPORT_STOP          225
#define IDM_PATTERN_PREV            240
#define IDM_PATTERN_NEXT            241
#define IDM_PATTERN_NEW             242
#define IDM_PATTERN_DELETE          243
#define IDM_OPEN_DISTORTION         226
#define IDM_OPEN_MODULATION         227
#define IDM_HELP_KEYS               230
#define IDM_HELP_ABOUT              231

// Main-window control IDs
#define IDC_WAVEFORM                300
#define IDC_OSC_BUTTON              305
#define IDC_DIST_BUTTON             306
#define IDC_MOD_BUTTON              307
#define IDC_TEMPO                   301
#define IDC_OCTAVE                  302
#define IDC_RANDOM                  303
#define IDC_TRANSPORT               304
#define IDC_TEMPO_LABEL             310
#define IDC_OCTAVE_LABEL            311
#define IDC_WAVEFORM_LABEL          312
#define IDC_RANDOM_LABEL            313
#define IDC_TRANSPORT_LABEL         314

#define IDC_KNOB_TAB                320
#define IDC_KNOB_LIST               321

#define IDC_STEP_LIST               330
#define IDC_STEP_LIST_LABEL         331

#define IDC_PATTERN_LIST            340
#define IDC_PATTERN_LIST_LABEL      341
#define IDC_PATTERN_COMBO           346
#define IDC_ORDER_ADD               342
#define IDC_ORDER_DEL               343
#define IDC_PATTERN_PLUS            344
#define IDC_PATTERN_MINUS           345

#define IDC_STATUS_LABEL            350

#define IDC_WILDNESS                370
#define IDC_WILDNESS_LABEL          371
#define IDC_WILDNESS_VALUE          372
#define IDC_ACCENT_SQUELCH          380
#define IDC_ACCENT_SQUELCH_LABEL    381
#define IDC_ACCENT_SQUELCH_VALUE    382
#define IDC_RESO_BOOST              383

// Oscillator dialog
#define IDD_OSCILLATOR              800
#define IDC_OSC_PWM                 801
#define IDC_OSC_PWM_LABEL           802
#define IDC_OSC_PWM_VALUE           803
#define IDC_OSC_SUB                 804
#define IDC_OSC_SUB_LABEL           805
#define IDC_OSC_SUB_VALUE           806
#define IDC_OSC_DUP_WAVEFORM        807
#define IDC_OSC_DUP_WAVEFORM_LABEL  808
#define IDC_OSC_DUP_DETUNE          809
#define IDC_OSC_DUP_DETUNE_LABEL    810
#define IDC_OSC_DUP_DETUNE_VALUE    811
#define IDC_OSC_DUP_LEVEL           812
#define IDC_OSC_DUP_LEVEL_LABEL     813
#define IDC_OSC_DUP_LEVEL_VALUE     814
#define IDC_OSC_RESET_303           815
#define IDC_OSC_DUP_PWM             816
#define IDC_OSC_DUP_PWM_LABEL       817
#define IDC_OSC_DUP_PWM_VALUE       818
// 819..827 were the PWM LFO's controls, retired in v15 (TODO Y3).
#define IDC_OSC_NOISE               828
#define IDC_OSC_NOISE_LABEL         829
#define IDC_OSC_NOISE_VALUE         830

// Distortion dialog
#define IDD_DISTORTION              900
#define IDC_DIST_TYPE               901
#define IDC_DIST_TYPE_LABEL         902
#define IDC_DIST_GAIN               903
#define IDC_DIST_GAIN_LABEL         904
#define IDC_DIST_GAIN_VALUE         905
#define IDC_DIST_TONE               906
#define IDC_DIST_TONE_LABEL         907
#define IDC_DIST_TONE_VALUE         908
#define IDC_DIST_LEVEL              909
#define IDC_DIST_LEVEL_LABEL        910
#define IDC_DIST_LEVEL_VALUE        911
#define IDC_FX_COMP_THRESH          912
#define IDC_FX_COMP_THRESH_LABEL    913
#define IDC_FX_COMP_THRESH_VALUE    914
#define IDC_FX_COMP_DEPTH           915
#define IDC_FX_COMP_DEPTH_LABEL     916
#define IDC_FX_COMP_DEPTH_VALUE     917
#define IDC_FX_RING_HZ              918
#define IDC_FX_RING_HZ_LABEL        919
#define IDC_FX_RING_HZ_VALUE        920
#define IDC_FX_RING_AMOUNT          921
#define IDC_FX_RING_AMOUNT_LABEL    922
#define IDC_FX_RING_AMOUNT_VALUE    923

// Sweep editor dialog
#define IDD_SWEEP_EDIT              700
#define IDC_SWEEP_PARAM_LABEL       701
#define IDC_SWEEP_NUMBER            702
#define IDC_SWEEP_START_PATTERN     703
#define IDC_SWEEP_START_STEP        704
#define IDC_SWEEP_START_VALUE       705
#define IDC_SWEEP_END_PATTERN       706
#define IDC_SWEEP_END_STEP          707
#define IDC_SWEEP_END_VALUE         708
#define IDC_SWEEP_ADD               710
#define IDC_SWEEP_REMOVE            711
#define IDC_SWEEP_COPY              712
#define IDC_SWEEP_PASTE             713
#define IDC_SWEEP_SAVE              714
#define IDC_SWEEP_LOAD              715
#define IDC_SWEEP_UPDATE            716
#define IDC_SWEEP_HOLD              717
#define IDC_SWEEP_COUNT_LABEL       720
#define IDC_SWEEP_STATUS            721
#define IDC_SWEEP_START_VALUE_LBL   722
#define IDC_SWEEP_END_VALUE_LBL     723
#define IDC_SWEEP_SHAPE             724
#define IDC_SWEEP_CYCLES            725
#define IDC_SWEEP_GENERATE          726
// Oscillation (v15): an LFO is a sweep with a wave.
#define IDC_SWEEP_START_VALUE_CAP   727
#define IDC_SWEEP_END_VALUE_CAP     728
#define IDC_SWEEP_WAVE              729
#define IDC_SWEEP_PHASE             730
#define IDC_SWEEP_RATE              731
#define IDC_SWEEP_RATE_UNIT         732
#define IDC_SWEEP_APPLY             733
#define IDC_SWEEP_HELP              734

// Modulation dialog (TODO Y3)
#define IDD_MODULATION              1000
#define IDC_MOD_LIST                1001
#define IDC_MOD_EDIT                1002
#define IDC_MOD_PRESET              1003
#define IDC_MOD_ADD_LFO             1004
#define IDC_MOD_STATUS              1005
#define IDC_MOD_HELP                1006

// Step-edit dialog
#define IDD_STEP_EDIT               500
#define IDC_STEP_EDIT_LABEL         501
#define IDC_STEP_EDIT_EDIT          502
#define IDC_STEP_EDIT_ACCENT        503
#define IDC_STEP_EDIT_SLIDE         504
#define IDC_STEP_EDIT_HELP          505

// Step editor: one step selector + one pitch combo + one slide checkbox
// + one accent checkbox. The three value controls reflect the state of
// whichever step is currently selected; edits write back to that step.
#define IDC_STEP_SELECTOR           600
#define IDC_STEP_PITCH              601
#define IDC_STEP_SLIDE              602
#define IDC_STEP_ACCENT             603
#define IDC_STEP_REVERSE             604
#define IDC_PATTERN_LENGTH          605
#define IDC_PATTERN_LENGTH_LABEL    606
#define IDC_PATTERN_TRIPLET         607

// Parameter trackbars
#define IDC_SLIDER_VOLUME           400
#define IDC_SLIDER_CUTOFF           401
#define IDC_SLIDER_RESONANCE        402
#define IDC_SLIDER_ENVMOD           403
#define IDC_SLIDER_DECAY            404
#define IDC_SLIDER_ACCENT           405
#define IDC_SLIDER_TUNING           406
#define IDC_SLIDER_BASE             IDC_SLIDER_VOLUME
#define IDC_SLIDER_COUNT            7
