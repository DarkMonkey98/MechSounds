"""Synthesize mechanical keyboard switch sounds as WAV files.

Each profile is a mix of:
  - thump: low sine (bottom-out), fast attack, exp decay
  - resonance: mid sine (woody/metallic body), slower decay
  - noise: bandpassed noise burst (clack/click character)
  - click: very short high-freq burst (clicky switches only)

Key-down = full sound, key-up = quieter/shorter/higher (top-out),
spacebar = deeper/heavier/longer (stabilizer acoustics).
3 down variants + 2 up variants per profile so fast typing doesn't
sound like a machine gun.
"""
import os
import numpy as np
from scipy.io import wavfile

SR = 44100
OUT = os.path.expanduser('~/workspace/mechsounds/sounds')


def sine(f, dur, amp, decay, sr=SR):
    t = np.arange(int(dur * sr)) / sr
    return amp * np.sin(2 * np.pi * f * t) * np.exp(-t / decay)


def noise_burst(dur, amp, decay, center, bw, rng, sr=SR):
    t = np.arange(int(dur * sr)) / sr
    n = rng.standard_normal(len(t))
    F = np.fft.rfft(n)
    freqs = np.fft.rfftfreq(len(t), 1 / sr)
    H = np.exp(-0.5 * ((freqs - center) / bw) ** 2)
    y = np.fft.irfft(F * H, len(t))
    peak = np.max(np.abs(y)) + 1e-9
    return amp * (y / peak) * np.exp(-t / decay)


def finish(y, peak=0.4):
    # 0.4 peak (-8 dBFS) leaves headroom: fast typing overlaps
    # 2-3 sounds at once, and 0.85 + 0.85 summed past 0 dBFS,
    # which is the clipping Matt heard. Files themselves were clean.
    # 1.5 ms attack ramp (no onset click), 3 ms fade-out (no end click)
    n = len(y)
    attack = int(0.0015 * SR)
    fade = int(0.003 * SR)
    ramp = np.ones(n)
    ramp[:attack] = 0.5 - 0.5 * np.cos(np.pi * np.arange(attack) / attack)
    ramp[-fade:] = 0.5 + 0.5 * np.cos(np.pi * np.arange(fade) / fade)
    y = y * ramp
    m = np.max(np.abs(y)) + 1e-9
    return np.int16(np.clip(y / m * peak * 32767, -32767, 32767))


def render(p, rng, kind='down'):
    if kind == 'up':
        p = dict(p)
        p['thump_f'] *= 1.4
        p['thump_amp'] *= 0.5
        p['thump_decay'] *= 0.6
        p['res_amp'] *= 0.6
        p['res_decay'] *= 0.6
        p['noise_amp'] *= 0.55
        p['noise_decay'] *= 0.6
        p['dur'] *= 0.6
        p.pop('click', None)
    elif kind == 'space':
        p = dict(p)
        p['thump_f'] *= 0.7
        p['thump_amp'] *= 1.15
        p['thump_decay'] *= 1.3
        p['res_f'] *= 0.8
        p['res_decay'] *= 1.3
        p['dur'] *= 1.25
    dur = p['dur']
    jf = 1 + rng.uniform(-0.04, 0.04)   # per-variant pitch jitter
    ja = 1 + rng.uniform(-0.08, 0.08)   # per-variant level jitter
    # Broadband by design: the pure resonance sine is kept quiet and
    # short (a loud sustained sine is what made HolyPanda sound like
    # a Windows error ding — 44% of its attack energy was one tone,
    # vs 2% for a real recording). The noise burst carries the clack.
    y = (
        sine(p['thump_f'] * jf, dur, p['thump_amp'] * ja, p['thump_decay'])
        + sine(p['res_f'] * jf, dur, p['res_amp'] * ja, p['res_decay'])
        + noise_burst(dur, p['noise_amp'] * ja, p['noise_decay'],
                      p['noise_c'] * jf, p['noise_bw'], rng)
    )
    crack = noise_burst(0.008, 0.55 * ja, 0.003, 3000, 2500, rng)  # attack crack
    y = y.copy()
    y[:len(crack)] += crack
    if p.get('click'):
        click = noise_burst(0.012, 0.9 * ja, 0.004, 3500, 800, rng)
        y = y.copy()
        y[:len(click)] += click
    return finish(y)


PROFILES = {
    'HolyPanda': dict(thump_f=120, thump_amp=0.65, thump_decay=0.045,
                      res_f=480, res_amp=0.15, res_decay=0.035,
                      noise_c=750, noise_bw=650, noise_amp=1.00,
                      noise_decay=0.035, dur=0.13),
    'Topre': dict(thump_f=95, thump_amp=0.60, thump_decay=0.040,
                  res_f=300, res_amp=0.12, res_decay=0.035,
                  noise_c=600, noise_bw=500, noise_amp=0.90,
                  noise_decay=0.025, dur=0.10),
    'OilKing': dict(thump_f=140, thump_amp=0.62, thump_decay=0.040,
                    res_f=600, res_amp=0.15, res_decay=0.035,
                    noise_c=700, noise_bw=450, noise_amp=0.95,
                    noise_decay=0.030, dur=0.11),
    'MXBrown': dict(thump_f=160, thump_amp=0.55, thump_decay=0.030,
                    res_f=800, res_amp=0.15, res_decay=0.030,
                    noise_c=1500, noise_bw=900, noise_amp=1.00,
                    noise_decay=0.025, dur=0.09),
    'MXRed': dict(thump_f=150, thump_amp=0.50, thump_decay=0.030,
                  res_f=700, res_amp=0.12, res_decay=0.030,
                  noise_c=1200, noise_bw=800, noise_amp=0.95,
                  noise_decay=0.022, dur=0.08),
    'MXBlue': dict(thump_f=170, thump_amp=0.55, thump_decay=0.030,
                   res_f=900, res_amp=0.15, res_decay=0.030,
                   noise_c=2200, noise_bw=1100, noise_amp=1.00,
                   noise_decay=0.025, dur=0.09, click=True),
}

if __name__ == '__main__':
    rng = np.random.default_rng(7)
    total = 0
    for name, p in PROFILES.items():
        d = os.path.join(OUT, name)
        os.makedirs(d, exist_ok=True)
        for i in range(1, 4):
            wavfile.write(os.path.join(d, f'down{i}.wav'), SR, render(p, rng, 'down'))
            total += 1
        for i in range(1, 3):
            wavfile.write(os.path.join(d, f'up{i}.wav'), SR, render(p, rng, 'up'))
            total += 1
        wavfile.write(os.path.join(d, 'space1.wav'), SR, render(p, rng, 'space'))
        total += 1
        print(f'{name}: 3 down, 2 up, 1 space')
    print(f'{total} WAVs written to {OUT}')
