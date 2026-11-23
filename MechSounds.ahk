#Requires AutoHotkey v2.0
#SingleInstance Force
Persistent

; ============================================================
; MechSounds — system-wide mechanical keyboard sounds for Windows
;
; Plays a switch sound on every keypress, in every app, through
; your default audio device (so it comes through your headphones).
; Works with ANY keyboard — USB, Bluetooth, or laptop — because it
; listens to key events, not the hardware.
;
; Controls:
;   Ctrl+Alt+M  toggle sounds on/off
;   Ctrl+Alt+P  cycle switch profile
;   Right-click the tray icon to pause or exit
;
; Requires: AutoHotkey v2 (free from autohotkey.com)
; Just double-click this file. No install, no admin needed for
; normal apps (run as admin to also hear keys in admin windows).
; ============================================================

Profiles := ["Rainy75", "NKCream", "CrystalPanda", "Oreo", "RealTopre", "ModelM", "Reaper"]
ProfileNames := Map(
    "Rainy75",      "Rainy 75 Pro - REAL Kailh Cocoa recording",
    "NKCream",      "NK Cream - REAL NovelKeys Cream recording",
    "CrystalPanda", "Crystal Panda - REAL Keychron tactile recording",
    "Oreo",         "Everglide Oreo - REAL linear recording",
    "RealTopre",    "HHKB Topre 45g - REAL Topre recording",
    "ModelM",       "IBM Model M - REAL buckling spring",
    "Reaper",       "Leobog Reaper - REAL creamy linear"
)
CurrentProfile := 1   ; starts on Rainy 75 Pro (real recording)
SoundsOn := true
SoundsDir := A_ScriptDir "\sounds"

A_IconTip := "MechSounds - Ctrl+Alt+M toggle, Ctrl+Alt+P switch profile"

if (!DirExist(SoundsDir)) {
    MsgBox("Sounds folder not found next to MechSounds.ahk.`nExpected: " SoundsDir, "MechSounds", "Icon!")
    ExitApp()
}

KeyVariant(key) {
    ; Same key, same recording: hash the key name to one of 6 variants.
    sum := 0
    Loop Parse, key
        sum += Ord(A_LoopField)
    return Mod(sum, 6) + 1
}

SoundFile(kind, key := "") {
    global CurrentProfile, Profiles, SoundsDir
    profile := Profiles[CurrentProfile]
    if (kind = "space")
        return SoundsDir "\" profile "\space1.wav"
    return SoundsDir "\" profile "\down" KeyVariant(key) ".wav"
}

; One sound per keypress: only the press plays. The "~key Up"
; hotkeys only clear the pressed state (so a held key's
; auto-repeat doesn't re-trigger a sound); they play nothing.
PressedKeys := Map()

PlayDown(kind := "key", key := "") {
    global SoundsOn, PressedKeys
    if (!SoundsOn)
        return
    if (key != "" && PressedKeys.Has(key))
        return
    if (key != "")
        PressedKeys[key] := true
    SoundPlay(SoundFile(kind, key))
}

PlayUp(key := "") {
    global PressedKeys
    if (key != "")
        PressedKeys.Delete(key)
}

; --- keys that get the normal key sound ---
NormalKeys := [
    "a","b","c","d","e","f","g","h","i","j","k","l","m",
    "n","o","p","q","r","s","t","u","v","w","x","y","z",
    "1","2","3","4","5","6","7","8","9","0",
    "-","=","[","]","\"",";","'",",",".","/","SC029",
    "F1","F2","F3","F4","F5","F6","F7","F8","F9","F10","F11","F12",
    "Up","Down","Left","Right","Home","End","PgUp","PgDn",
    "Delete","Insert","PrintScreen","ScrollLock","Pause",
    "CapsLock","NumLock","Escape",
    "Numpad0","Numpad1","Numpad2","Numpad3","Numpad4",
    "Numpad5","Numpad6","Numpad7","Numpad8","Numpad9",
    "NumpadDot","NumpadAdd","NumpadSub","NumpadMult","NumpadDiv"
]

; --- big stabilized keys get a deeper, heavier sound ---
HeavyKeys := ["Space", "Enter", "Backspace", "Tab", "NumpadEnter"]

for key in NormalKeys {
    Hotkey("~" key, (*) => PlayDown("key", key))
    Hotkey("~" key " Up", (*) => PlayUp(key))
}
for key in HeavyKeys {
    Hotkey("~" key, (*) => PlayDown("space", key))
    Hotkey("~" key " Up", (*) => PlayUp(key))
}

; --- controls ---
^!m:: {
    global SoundsOn
    SoundsOn := !SoundsOn
    ToolTip(SoundsOn ? "MechSounds ON" : "MechSounds OFF")
    SetTimer(() => ToolTip(), -1500)
}

^!p:: {
    global CurrentProfile, Profiles, ProfileNames
    CurrentProfile := Mod(CurrentProfile, Profiles.Length) + 1
    ToolTip("Profile: " ProfileNames[Profiles[CurrentProfile]])
    SetTimer(() => ToolTip(), -1500)
}
