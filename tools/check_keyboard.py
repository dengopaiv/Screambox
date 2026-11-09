# -*- coding: utf-8 -*-
"""Keyboard regression checks. Every failure this catches is SILENT in use.

Four ways a key can quietly do nothing in this window, all of them shipped here
at some point:

  1. Two accelerators on one chord - the first entry wins and the second can
     never fire. (Ctrl+Shift+N was bound twice.)
  2. An unmodified key bound as an accelerator - TranslateAccelerator runs
     before IsDialogMessage, so the key goes dead on every control in the
     window. (Space was bound to run/stop.)
  3. Two controls with the same mnemonic - Win32 cycles focus between them
     instead of activating either. (Alt+V, between Volume and a checkbox.)
  4. A mnemonic shadowed by an accelerator on the same chord - the accelerator
     is consulted first, so the underline is a promise that never fires.
     (Alt+A, Alt+S and Alt+R, which is what finding F was.)
  5. Two items in the same menu popup with the same mnemonic - cycles exactly
     like case 3, one namespace down. ("&Next" against "&New pattern".)
  6. A key bound to a command NOTHING HANDLES - the accelerator fires, the
     WM_COMMAND is sent, the switch has no case for it, and the message falls
     into DefWindowProc. The menu item is enabled, the accelerator text is
     printed beside it and F1 documents it, and pressing it does nothing at
     all. (Copy / Cut / Paste pattern, all three, from the beginning.)

  7. Two controls in one DIALOG with the same mnemonic - case 3 again, inside a
     modal dialog the other checks never looked at. (The sweep editor had two.)

Checks 1-5 and 7 all ask whether a keystroke ARRIVES. Check 6 is the one that asks
whether anything is waiting for it when it does, which is why the first five
passed for as long as they did while three menu items were dead.

None of these produce an error, a warning, or a log line. Run this after any
edit to resource.rc or to a control's label:

    python tools/check_keyboard.py
"""
import io
import re
import sys
import os
_HERE = os.path.dirname(os.path.abspath(__file__))
_SRC = os.path.join(os.path.dirname(_HERE), "src")


RC = os.path.join(_SRC, "resource.rc")
MW = os.path.join(_SRC, "MainWindow.cpp")

rc = io.open(RC, encoding="utf-8", errors="replace").read()
mw = io.open(MW, encoding="utf-8").read()

fails = []


def check(ok, what, detail=""):
    print("  %s  %s%s" % ("PASS" if ok else "FAIL", what,
                          ("\n          " + detail) if detail else ""))
    if not ok:
        fails.append(what)


# --------------------------------------------------------------- accelerators
accel = rc[rc.find("ACCELERATORS"):]
accel = accel[:accel.find("\nEND")]
chords = {}
for line in accel.splitlines():
    line = line.strip()
    if not line or line.startswith("//") or "VIRTKEY" not in line:
        continue
    m = re.match(r'"?([A-Za-z0-9]|VK_\w+)"?\s*,\s*(\w+)\s*,(.*)', line)
    if not m:
        continue
    key, cmd, rest = m.group(1).upper(), m.group(2), m.group(3)
    mods = frozenset(x for x in ("CONTROL", "SHIFT", "ALT") if x in rest)
    chords.setdefault((key, mods), []).append(cmd)


def chord_name(key, mods):
    return "+".join([m.title() for m in sorted(mods)] + [key])


dupes = {c: v for c, v in chords.items() if len(v) > 1}
check(not dupes, "no accelerator chord is bound twice (%d chords)" % len(chords),
      "" if not dupes else "; ".join("%s -> %s" % (chord_name(*c), ", ".join(v))
                                     for c, v in dupes.items()))

# An accelerator with NO modifier is only safe on keys no control wants: the
# function keys. Anything else steals the key from every control in the window.
bare = [c for c in chords if not c[1] and not re.match(r"VK_F\d+$", c[0])]
check(not bare, "no unmodified key is bound as a global accelerator",
      "" if not bare else ", ".join(chord_name(*c) for c in bare)
      + " - TranslateAccelerator runs before IsDialogMessage, so this key is "
        "dead on every control")

# Plain Alt+letter is the chord class that shadows mnemonics, since a mnemonic
# IS Alt+letter. Keeping the set empty makes finding F unrepeatable.
alt_letter = [c for c in chords if c[1] == frozenset({"ALT"}) and len(c[0]) == 1]
check(not alt_letter, "no accelerator uses a plain Alt+letter chord",
      "" if not alt_letter else ", ".join(chord_name(*c) for c in alt_letter)
      + " - this shadows the identically-lettered mnemonic")

# ----------------------------------------------------------------- mnemonics
start = mw.find("void MainWindow::buildLayout")
# To buildLayout's own closing brace. This used to run to the next
# "void MainWindow::", which swept in every free function in between - and the
# sweep editor's helpers, whose labels are that DIALOG's mnemonics, sit there.
body = mw[start:mw.find("\n}", start)]
mnemonics = {}
for m in re.finditer(r'L"([^"]*&[A-Za-z][^"]*)"', body):
    for k in re.finditer(r"&([A-Za-z])", m.group(1)):
        mnemonics.setdefault(k.group(1).upper(), []).append(m.group(1)[:44])

dupm = {k: v for k, v in mnemonics.items() if len(v) > 1}
check(not dupm, "every main-window mnemonic is unique (%d in use)" % len(mnemonics),
      "" if not dupm else "; ".join("Alt+%s -> %s" % (k, " | ".join(v))
                                    for k, v in dupm.items()))

shadowed = sorted(k for k in mnemonics if (k, frozenset({"ALT"})) in chords)
check(not shadowed, "no mnemonic is shadowed by an accelerator on the same chord",
      "" if not shadowed else ", ".join("Alt+" + k for k in shadowed))

# The menu bar owns these, and a main-window mnemonic on one of them competes
# with the menu rather than with another control.
# File, Edit, Pattern, pLay, Help. The menu bar is consulted before any
# control, so a main-window mnemonic on one of these can never fire.
MENU = set("FEPHL")
clash = sorted(k for k in mnemonics if k in MENU)
check(not clash, "no main-window mnemonic collides with the menu bar (F, E, P, H, L)",
      "" if not clash else ", ".join("Alt+" + k for k in clash))

# ------------------------------------------------------- menu popups
# Each popup is its own mnemonic namespace: the letters only have to be unique
# among the items of that one menu, and the menu-bar row is a namespace of its
# own. Parsed by tracking BEGIN/END depth so nested popups are scored inside
# their parent, which is where Windows actually resolves them.
menu = rc[rc.find("IDR_MAIN_MENU MENU"):]
menu = menu[:menu.find("IDR_MAIN_MENU ACCELERATORS")]
scopes = {}          # name -> {letter: [labels]}
stack = ["menu bar"]
for line in menu.splitlines():
    t = line.strip()
    if t.startswith("BEGIN"):
        continue
    if t.startswith("END"):
        if len(stack) > 1:
            stack.pop()
        continue
    m = re.match(r'(POPUP|MENUITEM)\s+"([^"]*)"', t)
    if not m:
        continue
    kind, label = m.group(1), m.group(2)
    for k in re.finditer(r"&([A-Za-z])", label):
        scopes.setdefault(stack[-1], {}).setdefault(
            k.group(1).upper(), []).append(label.split(chr(92))[0])
    if kind == "POPUP":
        stack.append(label.replace("&", ""))

menu_dupes = []
for scope, letters in scopes.items():
    for k, labels in letters.items():
        if len(labels) > 1:
            menu_dupes.append("%s: Alt+%s -> %s" % (scope, k, " | ".join(labels)))
check(not menu_dupes,
      "every menu's mnemonics are unique within that menu (%d menus)" % len(scopes),
      "; ".join(menu_dupes))

# ------------------------------------------------- commands with no handler
# The first five checks all verify that a keystroke reaches the window. This
# one verifies that something is listening: every IDM_ named by a menu item or
# an accelerator must have a `case` in MainWindow's WM_COMMAND switch. Without
# it the command is silently swallowed by DefWindowProc.
# `.` does not match a newline, so this strips a // comment from every line
# without touching the line structure - commented-out ids must not count.
rc_live = re.sub(r"//.*", "", rc)
menu_ids = set(re.findall(r'MENUITEM\s+"[^"]*"\s*,\s*(IDM_\w+)', rc_live))
accel_ids = set(re.findall(r'^\s*(?:"\w"|VK_\w+)\s*,\s*(IDM_\w+)', rc_live, re.M))
handled = set(re.findall(r"case\s+(IDM_\w+)\s*:", mw))
dead = sorted((menu_ids | accel_ids) - handled)
check(not dead,
      "every menu item and accelerator reaches a handler (%d commands)"
      % len(menu_ids | accel_ids),
      ("no case in the WM_COMMAND switch for: " + ", ".join(dead)) if dead else "")

# ------------------------------------------------------- dialog mnemonics
# Every modal dialog is its own namespace, and the same collision as check 3
# happens inside one: Win32 cycles focus between the matches. The checks above
# only ever looked at the main window, which is how the sweep editor shipped
# with two collisions (Alt+N on "Sweep number" / "End pattern", Alt+T on "Start
# step" / "Paste") while this script said KEYBOARD OK. Modal dialogs have no
# menu bar and no accelerator table, so uniqueness is the whole rule.
dialog_dupes = []
dialogs = re.findall(r"^(\w+)\s+DIALOGEX\b.*?^BEGIN(.*?)^END", rc, re.S | re.M)
for name, dbody in dialogs:
    letters = {}
    # Comments are not controls; the resource script explains its letter
    # choices in them, quoting the labels it is steering around.
    for label in re.findall(r'"((?:[^"]|"")*)"', re.sub(r"//.*", "", dbody)):
        for k in re.finditer(r"&([A-Za-z])", label.replace("&&", "")):
            letters.setdefault(k.group(1).upper(), []).append(label)
    for k, labels in letters.items():
        if len(labels) > 1:
            dialog_dupes.append("%s: Alt+%s -> %s" % (name, k, " | ".join(labels)))
check(not dialog_dupes,
      "every dialog's mnemonics are unique within that dialog (%d dialogs)" % len(dialogs),
      "; ".join(dialog_dupes))

print()
print("  in use:", " ".join("Alt+" + k for k in sorted(mnemonics)))
print("  free:  ", " ".join("Alt+" + c for c in "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                            if c not in mnemonics and c not in MENU))

print("=" * 74)
print("KEYBOARD OK" if not fails else "KEYBOARD FAILURES: " + ", ".join(fails))
print("=" * 74)
sys.exit(1 if fails else 0)
