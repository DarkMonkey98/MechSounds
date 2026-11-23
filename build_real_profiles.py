"""Build MechSounds profiles from real Mechvibes recordings.

Mechvibes packs come in three shapes:
  single + one audio file (wav or ogg) + timing windows per key
  multi  + one ogg file per key
Each key's recording is one full keypress, so one sound per press is
exactly right: down1-3 are three distinct key recordings, space1 is
the Space key's own recording. No release (up) sounds — MechSounds
plays one click per keypress.

All output: 44.1 kHz mono WAV, 0.4 peak (-8 dBFS), short attack ramp
and fade-out so nothing clicks. Windows trimmed to 200 ms max
(300 ms for space) so the sound stays snappy.
"""
import json
import os
import subprocess
import sys
import tempfile

import numpy as np
from scipy.io import wavfile
from scipy.signal import resample_poly

BASE = os.path.expanduser('~/workspace/mechsounds')
PACKS_DIR = os.path.join(BASE, 'real-packs')
OUT_DIR = os.path.join(BASE, 'sounds')
SR_OUT = 44100

# (pack dir, profile folder name)
PACKS = [
    ('wob', 'Rainy75'),
    ('creams', 'NKCream'),
    ('oreo', 'Oreo'),
    ('panda', 'CrystalPanda'),
    ('hhkb', 'RealTopre'),
    ('modelm', 'ModelM'),
    ('reaper', 'Reaper'),
    # Top 10 most-downloaded keyboard packs (Nov 2026), new in v10
    ('keychron-q3', 'KeychronQ3'),
    ('orange-deluxe', 'OrangeDeluxe'),
    ('aula-f75', 'AulaF75'),
    ('creamy-thingy', 'CreamyThingy'),
    ('gravastar-v60', 'GravastarV60'),
    ('hyperx-alloy', 'HyperXAlloy'),
    ('wooting-60he', 'Wooting60HE'),
    ('epomaker-th85', 'EpomakerTH85'),
]


def finish(y, peak=0.4, sr=SR_OUT):
    n = len(y)
    attack = int(0.0015 * sr)
    fade = int(0.003 * sr)
    ramp = np.ones(n)
    ramp[:attack] = 0.5 - 0.5 * np.cos(np.pi * np.arange(attack) / attack)
    ramp[-fade:] = 0.5 + 0.5 * np.cos(np.pi * np.arange(fade) / fade)
    y = y * ramp
    m = np.max(np.abs(y)) + 1e-9
    return np.int16(np.clip(y / m * peak * 32767, -32767, 32767))


def to_mono(x):
    if x.ndim == 2:
        return x.astype(np.float64).mean(axis=1)
    return x.astype(np.float64)


def decode_any(path):
    """Decode any non-WAV audio (ogg, mp3, ...) to mono float via ffmpeg."""
    tmp = tempfile.mktemp(suffix='.wav')
    subprocess.run(['ffmpeg', '-v', 'error', '-y', '-i', path,
                    '-ac', '1', '-ar', '48000', tmp], check=True)
    sr, x = wavfile.read(tmp)
    os.unlink(tmp)
    return sr, to_mono(x) / 32768.0


def load_audio(path):
    if path.lower().endswith('.wav'):
        sr, x = wavfile.read(path)
        return sr, to_mono(x) / 32768.0
    return decode_any(path)


def slice_around_peak(y, sr, t0, t1, pre_ms=30, total_ms=200):
    """Slice a window around its loudest point.

    The click isn't always at the start of a pack's window — the
    HHKB pack's windows have 140-210 ms of silence before the
    actual keypress, so slicing from the window start gave amplified
    room tone. Find the peak first, then take total_ms around it.
    """
    a = int(round(t0 / 1000 * sr))
    b = int(round(t1 / 1000 * sr))
    seg = y[a:b]
    if len(seg) == 0:
        return seg
    pk = int(np.argmax(np.abs(seg)))
    pre = int(pre_ms / 1000 * sr)
    total = int(total_ms / 1000 * sr)
    s = max(0, pk - pre)
    e = min(len(seg), s + total)
    return seg[s:e]


def pitch(y, sr, factor):
    """Shift pitch by factor (1.03 = 3% higher). Duration changes too."""
    up = 100
    down = int(round(100 * factor))
    return resample_poly(y, up, down)


def slice_around_peak_file(y, sr, total_ms=200, pre_ms=30):
    """Peak-centered slice of a whole file (multi-file packs)."""
    if len(y) == 0:
        return y
    pk = int(np.argmax(np.abs(y)))
    pre = int(pre_ms / 1000 * sr)
    total = int(total_ms / 1000 * sr)
    s = max(0, pk - pre)
    e = min(len(y), s + total)
    return y[s:e]


def to_out(y, sr):
    if sr != SR_OUT:
        y = resample_poly(y, SR_OUT, sr)
    return y


def build_single(src, profile, audio_file):
    sr, y = load_audio(os.path.join(src, audio_file))
    cfg = json.load(open(os.path.join(src, 'config.json')))
    defs = cfg['definitions']
    windows = {}
    for k, v in defs.items():
        windows.setdefault(tuple(v['timing'][0]), []).append(k)

    # space: union of the Space key's windows
    space_t = defs['Space']['timing']
    s0 = min(t[0] for t in space_t)
    s1 = max(t[1] for t in space_t)

    # down variants: up to 6 distinct windows, letter keys first.
    # Each key is assigned one variant by hash, so the same key
    # always plays the same recording.
    letter = [w for w in windows
              if any(k.startswith('Key') for k in windows[w])]
    others = [w for w in windows if w not in letter]
    picks = (letter + others)[:6]
    if len(picks) < 6:
        picks = (picks + sorted(windows))[:6]

    # single-recording pack (creams): one window for everything —
    # make variants by pitch-shifting the one clip.
    out = os.path.join(OUT_DIR, profile)
    os.makedirs(out, exist_ok=True)
    if len(windows) == 1:
        w = picks[0]
        base = slice_around_peak(y, sr, *w, total_ms=200)
        for i, f in enumerate([1.0, 0.97, 1.03, 0.94, 1.06, 0.91], 1):
            seg = pitch(base, sr, f) if f != 1.0 else base
            wavfile.write(os.path.join(out, f'down{i}.wav'), SR_OUT,
                          finish(to_out(seg, sr)))
        seg = pitch(base, sr, 0.8)
        wavfile.write(os.path.join(out, 'space1.wav'), SR_OUT,
                      finish(to_out(seg, sr)))
    else:
        for i, w in enumerate(picks, 1):
            seg = slice_around_peak(y, sr, *w, total_ms=200)
            wavfile.write(os.path.join(out, f'down{i}.wav'), SR_OUT,
                          finish(to_out(seg, sr)))
        seg = slice_around_peak(y, sr, s0, s1, total_ms=300)
        wavfile.write(os.path.join(out, 'space1.wav'), SR_OUT,
                      finish(to_out(seg, sr)))
    print(f'{profile}: {len(windows)} recordings in pack, '
          f'down windows {[(round(w[1]-w[0]), windows[w][:1]) for w in picks]}')


def build_multi(src, profile):
    cfg = json.load(open(os.path.join(src, 'config.json')))
    defs = cfg['definitions']
    out = os.path.join(OUT_DIR, profile)
    os.makedirs(out, exist_ok=True)
    # six distinct letter/digit recordings + the spacebar file
    downs = []
    for k in sorted(defs):
        f = defs[k].get('audio_file')
        if f and f not in downs and k != 'Space':
            downs.append(f)
        if len(downs) == 6:
            break
    for i, f in enumerate(downs, 1):
        sr, y = load_audio(os.path.join(src, f))
        seg = slice_around_peak_file(y, sr, total_ms=200)
        wavfile.write(os.path.join(out, f'down{i}.wav'), SR_OUT,
                      finish(to_out(seg, sr)))
    space_f = defs['Space']['audio_file']
    sr, y = load_audio(os.path.join(src, space_f))
    seg = slice_around_peak_file(y, sr, total_ms=300)
    wavfile.write(os.path.join(out, 'space1.wav'), SR_OUT,
                  finish(to_out(seg, sr)))
    print(f'{profile}: multi-file pack, downs {downs}, space {space_f}')


def main():
    for pack_dir, profile in PACKS:
        src = os.path.join(PACKS_DIR, pack_dir)
        cfg = json.load(open(os.path.join(src, 'config.json')))
        if cfg.get('definition_method') == 'multi':
            build_multi(src, profile)
        else:
            build_single(src, profile, cfg['audio_file'])
    # verify: duration, peak, and that the click isn't buried in
    # leading silence (first 60 ms must carry real signal)
    for _, profile in PACKS:
        d = os.path.join(OUT_DIR, profile)
        for f in sorted(os.listdir(d)):
            sr, x = wavfile.read(os.path.join(d, f))
            y = x.astype(np.float64) / 32768.0
            peak = np.abs(y).max()
            head = np.abs(y[:int(0.06 * sr)]).max()
            flag = '  <-- SILENT HEAD' if head < 0.05 else ''
            print(f'  {profile}/{f}: {len(x)/sr*1000:.0f} ms peak {peak:.3f} '
                  f'head60ms {head:.3f}{flag}')


if __name__ == '__main__':
    main()
