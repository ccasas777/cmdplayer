"""量測 CamillaDSP FIR 係數檔 (F64_LE raw) 的頻響，各頻率以 1 kHz 為 0 dB 列出

用法: python3 check_fir.py <hd820_minphase_48000.raw> [取樣率，預設由檔名判斷]
"""
import re
import sys
import numpy as np

FREQS = [30, 60, 100, 150, 280, 500, 1000, 2500, 3600, 8000, 12000]

def main():
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__)
    path = sys.argv[1]
    fs = int(sys.argv[2]) if len(sys.argv) == 3 else int(re.search(r"_(\d+)\.raw$", path).group(1))
    ir = np.fromfile(path, dtype="<f8")
    n_fft = 1 << max(20, int(np.ceil(np.log2(len(ir)))))
    f = np.fft.rfftfreq(n_fft, d=1.0 / fs)
    db = 20 * np.log10(np.abs(np.fft.rfft(ir, n=n_fft)) + 1e-30)
    level = lambda fc: np.interp(np.log(fc), np.log(f[1:]), db[1:])
    ref = level(1000)
    print(f"{path}: {len(ir)} taps @ {fs} Hz, gain @ 1 kHz {ref:+.2f} dB")
    print("".join(f"{fc:>8}" for fc in FREQS))
    print("".join(f"{level(fc) - ref:+8.2f}" for fc in FREQS))

if __name__ == "__main__":
    main()