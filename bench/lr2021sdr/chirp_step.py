"""De-chirp each burst (RTL capture) and find the largest frequency step.
usage: chirp_step.py <capture.npy> <offset_hz> <rate:pairs:sign> [...]
A skipped or repeated stretch of samples shifts the chirp in time, which shows
after de-chirping as a step of slope*shift in frequency."""
import sys
import numpy as np
fs = 2.048e6
z = np.load(sys.argv[1]).astype(np.complex64); z -= z.mean()
z = z * np.exp(-2j * np.pi * float(sys.argv[2]) * np.arange(len(z)) / fs).astype(np.complex64)
D = 16; m = len(z) // D * D; Z = np.fft.fft(z[:m]); f = np.fft.fftfreq(m, 1 / fs); Z[np.abs(f) > 40e3] = 0
y = np.fft.ifft(Z)[::D]; fd = fs / D
p = np.convolve(np.abs(y) ** 2, np.ones(64) / 64, "same"); p[: int(0.1 * fd)] = 0
on = np.r_[False, p > 30 * np.median(p), False]; e = np.flatnonzero(np.diff(on.astype(int)))
B = [(e[i], e[i + 1]) for i in range(0, len(e), 2) if e[i + 1] - e[i] > 0.004 * fd]
for (a, b), spec in zip(B, sys.argv[3:]):
    rate, n, sgn = spec.split(":"); rate, n, sgn = int(rate), int(n), float(sgn); T = n / rate
    seg = y[a:b]; t = np.arange(len(seg)) / fd
    d = seg * np.exp(-2j * np.pi * sgn * (-20e3 * t + 40e3 * t * t / (2 * T)))
    inst = np.angle(d[1:] * d[:-1].conj()) * fd / 2 / np.pi
    k = 64; sm = np.convolve(inst, np.ones(k) / k, "valid")       # 0.5 ms average
    i0, i1 = int(0.04 * len(sm)), int(0.96 * len(sm))
    jump = sm[i0 + k:i1] - sm[i0:i1 - k]; j = int(np.argmax(np.abs(jump))) + i0
    step = jump[j - i0]; slope = sgn * 40e3 / T
    tj = (j + k) / fd
    print(f"rate {rate} sign {sgn:+.0f}: burst {len(seg)/fd*1e3:.2f} ms of {T*1e3:.0f}; largest step {step:+.0f} Hz at "
          f"{tj*1e3:.1f} ms ({tj/T*100:.0f}%) = shift {step/slope*rate:+.0f} samples; capture sample {int((a+j+k)*D)} = {((a+j+k)*D)%262144} past a 262144 block")
