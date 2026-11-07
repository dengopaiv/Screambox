#pragma once
// The one place the version number lives. resource.rc reads it for the exe's
// file properties and MainWindow.cpp for the About box, so the two cannot
// disagree. Plain #defines because rc.exe's preprocessor understands nothing
// else.
//
// This is the program's version, not a file-format version: .roq and .hape
// carry their own (see RoqIO.cpp), and a release does not bump those.

#define SCREAMBOX_VER_MAJOR 1
#define SCREAMBOX_VER_MINOR 0
#define SCREAMBOX_VER_PATCH 0

#define SCREAMBOX_VER_STR   "1.0.0"
#define SCREAMBOX_VER_WSTR  L"1.0.0"
