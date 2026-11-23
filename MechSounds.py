"""MechSounds.py — system-wide mechanical keyboard sounds for Windows.
No AutoHotkey needed. Needs Python 3 (python.org) — standard library only.

Plays a switch sound on every keypress, in every app, through your
default audio device (so it comes through your headphones). Works with
ANY keyboard — USB, Bluetooth, laptop — because it listens to key
events, not the hardware.

Run:  python MechSounds.py
       (drop this file in the MechSounds folder, next to sounds/)
Stop:  Ctrl+Alt+Q, or Ctrl+C in this window.

Controls: Ctrl+Alt+M toggle sounds, Ctrl+Alt+P cycle switch profile.

LATENCY DESIGN (so it never feels laggy or disorienting):
 - All WAVs are opened once at startup as MCI aliases and kept ready.
   A keypress never touches the disk.
 - The keyboard hook does ONE thing: PostMessage() and return. It never
   plays audio, never reads files, never blocks — so the keystroke
   itself is never delayed by the sound.
 - Playback is two tiny MCI commands (seek to start, play) on a
   pre-opened alias, ~1 ms. Sounds overlap via a pool of aliases, so
   fast typing never cuts off the previous key's tail.
 - Every 100 keys this window prints the measured hook-to-play
   dispatch time (avg/max ms). The rest is Windows audio output,
   typically 10-30 ms — total stays well under the ~50 ms where
   typing starts to feel disconnected from its sound.
"""
import ctypes
import os
import random
import sys
import time
from ctypes import wintypes

if os.name == "nt":
    user32 = ctypes.windll.user32
    winmm = ctypes.windll.winmm
    kernel32 = ctypes.windll.kernel32
    WINFUNCTYPE = ctypes.WINFUNCTYPE
else:
    user32 = winmm = kernel32 = None
    WINFUNCTYPE = ctypes.CFUNCTYPE

WH_KEYBOARD_LL = 13
WM_KEYDOWN = 0x0100
WM_KEYUP = 0x0101
WM_SYSKEYDOWN = 0x0104
WM_SYSKEYUP = 0x0105
WM_HOTKEY = 0x0312
WM_USER = 0x0400
WM_PLAY_DOWN = WM_USER + 2
WM_PLAY_UP = WM_USER + 3
WM_DESTROY = 0x0002
HWND_MESSAGE = -3
MOD_ALT = 0x0001
MOD_CONTROL = 0x0002
VK_SPACE = 0x20
VK_RETURN = 0x0D
VK_BACK = 0x08
VK_TAB = 0x09
VK_LCONTROL = 0xA2
VK_RCONTROL = 0xA3
VK_LMENU = 0xA4
VK_RMENU = 0xA5
VK_LSHIFT = 0xA0
VK_RSHIFT = 0xA1
VK_LWIN = 0x5B
VK_RWIN = 0x5C

PROFILES = ["Rainy75", "NKCream", "CrystalPanda", "Oreo", "RealTopre", "ModelM", "Reaper"]
PROFILE_NAMES = {
    "Rainy75": "Rainy 75 Pro - REAL Kailh Cocoa recording",
    "NKCream": "NK Cream - REAL NovelKeys Cream recording",
    "CrystalPanda": "Crystal Panda - REAL Keychron tactile recording",
    "Oreo": "Everglide Oreo - REAL linear recording",
    "RealTopre": "HHKB Topre 45g - REAL Topre recording",
    "ModelM": "IBM Model M - REAL buckling spring",
    "Reaper": "Leobog Reaper - REAL creamy linear",
}
KIND_DOWN, KIND_SPACE = 0, 1

BASE = os.path.dirname(os.path.abspath(__file__))
SOUNDS = os.path.join(BASE, "sounds")

cur_profile = 0
enabled = True
aliases = []          # list of (name, kind, duration_ms, free_at_tick)
pressed = set()       # keys currently held (auto-repeat filter)
dispatch_sum = 0
dispatch_max = 0
dispatch_n = 0
hwnd = None
hook = None


class KBDLLHOOKSTRUCT(ctypes.Structure):
    _fields_ = [("vkCode", wintypes.DWORD),
                ("scanCode", wintypes.DWORD),
                ("flags", wintypes.DWORD),
                ("time", wintypes.DWORD),
                ("dwExtraInfo", ctypes.c_void_p)]


def mci(cmd):
    return winmm.mciSendStringA(cmd.encode(), None, 0, None)


def close_aliases():
    global aliases
    for name, _kind, _dur, _free, _var in aliases:
        mci(f"close {name}")
    aliases = []


def open_aliases():
    global aliases
    close_aliases()
    prof = PROFILES[cur_profile]
    idx = 0
    for v in (1, 2, 3, 4, 5, 6):
        for _ in range(4):
            name = f"kbd{idx}"
            path = os.path.join(SOUNDS, prof, f"down{v}.wav")
            if mci(f'open "{path}" alias {name}') == 0:
                dur = mci_length(name)
                aliases.append((name, KIND_DOWN, dur, 0, v - 1))
            idx += 1
    for _ in range(4):
        name = f"kbd{idx}"
        path = os.path.join(SOUNDS, prof, "space1.wav")
        if mci(f'open "{path}" alias {name}') == 0:
            dur = mci_length(name)
            aliases.append((name, KIND_SPACE, dur, 0, 0))
        idx += 1
    print(f"[MechSounds] profile: {PROFILE_NAMES[prof]} "
          f"({len(aliases)} sounds ready)")


def mci_length(name):
    buf = ctypes.create_string_buffer(32)
    if winmm.mciSendStringA(f"status {name} length".encode(),
                            buf, 32, None) == 0:
        try:
            ms = int(buf.value)
            return ms if 20 < ms < 500 else 150
        except ValueError:
            return 150
    return 150


def key_variant(vk):
    """Which down variant a key plays: same key, same recording."""
    return ((vk * 2654435761) >> 29) % 6


def play_kind(kind, vk=0):
    global aliases
    now = kernel32.GetTickCount64()
    want = key_variant(vk) if kind == KIND_DOWN else 0
    cands = [i for i, (_n, k, _d, _f, var) in enumerate(aliases)
             if k == kind and (kind != KIND_DOWN or var == want)]
    if not cands:
        return
    random.shuffle(cands)
    for i in cands:
        name, _k, dur, free_at, var = aliases[i]
        if now >= free_at:
            aliases[i] = (name, _k, dur, now + dur, var)
            mci(f"play {name} from 0")
            return
    name, _k, dur, _f, var = aliases[cands[0]]
    aliases[cands[0]] = (name, _k, dur, now + dur, var)
    mci(f"play {name} from 0")


def note_dispatch(start_ms):
    global dispatch_sum, dispatch_max, dispatch_n
    dt = kernel32.GetTickCount64() - start_ms
    dispatch_sum += dt
    dispatch_n += 1
    dispatch_max = max(dispatch_max, dt)
    if dispatch_n % 100 == 0:
        avg = dispatch_sum // dispatch_n
        print(f"[MechSounds] dispatch latency: avg {avg}ms "
              f"max {dispatch_max}ms over {dispatch_n} keys")


MODIFIERS = {VK_LCONTROL, VK_RCONTROL, VK_LMENU, VK_RMENU,
             VK_LSHIFT, VK_RSHIFT, VK_LWIN, VK_RWIN}

HOOKPROC = WINFUNCTYPE(ctypes.c_long, ctypes.c_int,
                       wintypes.WPARAM, wintypes.LPARAM)


@HOOKPROC
def hook_proc(n_code, w_param, l_param):
    if n_code == 0:
        ks = ctypes.cast(l_param, ctypes.POINTER(KBDLLHOOKSTRUCT)).contents
        if ks.vkCode not in MODIFIERS:
            now = kernel32.GetTickCount64()
            if w_param in (WM_KEYDOWN, WM_SYSKEYDOWN):
                user32.PostMessageA(hwnd, WM_PLAY_DOWN, ks.vkCode, now)
            elif w_param in (WM_KEYUP, WM_SYSKEYUP):
                user32.PostMessageA(hwnd, WM_PLAY_UP, ks.vkCode, now)
    return user32.CallNextHookEx(hook, n_code, w_param, l_param)


WNDPROC = WINFUNCTYPE(ctypes.c_long, wintypes.HWND, wintypes.UINT,
                     wintypes.WPARAM, wintypes.LPARAM)


@WNDPROC
def wnd_proc(h, msg, wp, lp):
    global cur_profile, enabled
    if msg == WM_PLAY_DOWN:
        note_dispatch(lp)
        if wp in pressed:
            return 0  # auto-repeat of a held key: one click per press
        pressed.add(wp)
        if enabled:
            if wp in (VK_SPACE, VK_RETURN, VK_BACK, VK_TAB):
                play_kind(KIND_SPACE, wp)
            else:
                play_kind(KIND_DOWN, wp)
        return 0
    if msg == WM_PLAY_UP:
        pressed.discard(wp)
        return 0
    if msg == WM_HOTKEY:
        if wp == 1:
            enabled = not enabled
            print("[MechSounds]", "ON" if enabled else "OFF")
        elif wp == 2:
            cur_profile = (cur_profile + 1) % len(PROFILES)
            open_aliases()
        elif wp == 3:
            user32.PostMessageA(h, WM_DESTROY, 0, 0)
        return 0
    if msg == WM_DESTROY:
        user32.PostQuitMessage(0)
        return 0
    return user32.DefWindowProcA(h, msg, wp, lp)


class WNDCLASSA(ctypes.Structure):
    _fields_ = [("style", wintypes.UINT),
                ("lpfnWndProc", WNDPROC),
                ("cbClsExtra", ctypes.c_int),
                ("cbWndExtra", ctypes.c_int),
                ("hInstance", ctypes.c_void_p),
                ("hIcon", ctypes.c_void_p),
                ("hCursor", ctypes.c_void_p),
                ("hbrBackground", ctypes.c_void_p),
                ("lpszMenuName", wintypes.LPCSTR),
                ("lpszClassName", wintypes.LPCSTR)]


def main():
    global hwnd, hook
    if os.name != "nt":
        print("MechSounds.py only runs on Windows.")
        return 1
    if not os.path.isdir(os.path.join(SOUNDS, "Rainy75")):
        print("sounds folder not found next to MechSounds.py.")
        print("Drop this file in the MechSounds folder, next to sounds/.")
        return 1
    random.seed(time.time())

    wc = WNDCLASSA()
    wc.lpfnWndProc = wnd_proc
    wc.hInstance = kernel32.GetModuleHandleA(None)
    wc.lpszClassName = b"MechSoundsWnd"
    user32.RegisterClassA(ctypes.byref(wc))
    hwnd = user32.CreateWindowExA(0, b"MechSoundsWnd", b"MechSounds",
                                  0, 0, 0, 0, 0, HWND_MESSAGE, None,
                                  wc.hInstance, None)
    if not hwnd:
        print("Could not create message window.")
        return 1

    user32.RegisterHotKey(hwnd, 1, MOD_CONTROL | MOD_ALT, ord("M"))
    user32.RegisterHotKey(hwnd, 2, MOD_CONTROL | MOD_ALT, ord("P"))
    user32.RegisterHotKey(hwnd, 3, MOD_CONTROL | MOD_ALT, ord("Q"))

    open_aliases()
    hook = user32.SetWindowsHookExA(WH_KEYBOARD_LL, hook_proc,
                                    wc.hInstance, 0)
    if not hook:
        print("Failed to install the keyboard hook.")
        return 1

    print("[MechSounds] running. Ctrl+Alt+M toggle, Ctrl+Alt+P profile, "
          "Ctrl+Alt+Q quit.")
    msg = wintypes.MSG()
    while user32.GetMessageA(ctypes.byref(msg), None, 0, 0) != 0:
        user32.TranslateMessage(ctypes.byref(msg))
        user32.DispatchMessageA(ctypes.byref(msg))

    close_aliases()
    user32.UnhookWindowsHookEx(hook)
    return 0


if __name__ == "__main__":
    sys.exit(main())
