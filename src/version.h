#pragma once
// The one place the version number lives. resource.rc reads it for the exe's
// file properties and MainWindow.cpp for the About box, so the two cannot
// disagree. Plain #defines because rc.exe's preprocessor understands nothing
// else.
//
// This is the program's version, not a file-format version: .roq and .hape
// carry their own (see RoqIO.cpp), and a release does not bump those.

//
// 1.0.0 was released as Creakbox on 2026-10-03 (tag v1.0.0, still on GitHub).
// The numbering carries on across the rename; 2.0.0 is the first Screambox.
// 2.1.0 (test build 2026-11-09): LFOs as sweeps, gliding sweeps, Modulation.

#define SCREAMBOX_VER_MAJOR 2
#define SCREAMBOX_VER_MINOR 1
#define SCREAMBOX_VER_PATCH 0

#define SCREAMBOX_VER_STR   "2.1.0"
#define SCREAMBOX_VER_WSTR  L"2.1.0"
