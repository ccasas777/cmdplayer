import os
import sys
import csv
import numpy as np

# 以六份 HD820 量測的 Harman AE/OE 2018 偏差取中位數，平滑後取負值作為 EQ，產生最小相位 FIR
#  - 每個頻率取中位數: 單一量測台或單次佩戴的異常 (如 ASR 450 Hz 的左右錯位) 自動被排除
#  - 平滑寬度隨頻率加寬 (SMOOTHING): 高頻各量測差異大 (標準差 2-7 dB)，只修正大範圍趨勢
#  - 各量測絕對位準不同，在 ALIGN_LO..ALIGN_HI 內以中位數對齊第一份
# 資料都在 hd820_data/:
#   ASR_Sennheiser_HD820     ASR (GRAS 45CA-10, KB5010, RA0402) APx 匯出的左右聲道 Harman Deviation
#   oratory1990_pdf.csv      oratory1990 PDF 向量圖取出的 raw 與 Harman 目標 (GRAS 45BC-10 KEMAR)
#   autoeq_*.csv             AutoEq measurements/*/data/over-ear (已平均左右聲道)
#   harman_over-ear_2018.csv AutoEq targets: GRAS 量測台用的 Harman AE/OE 2018
#   target_*.csv             AutoEq results 中該量測台實際使用的目標曲線

DATA_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "source")
HARMAN_CSV = "harman_over-ear_2018.csv"
TARGET_CSV = "oratory1990_optimum_hifi.csv"
#TARGET_CSV = "Diffuse_field_GRAS_KEMAR.csv"
# 與 Harman 研究同類的 GRAS 量測台，直接以 HARMAN_CSV 為目標
AUTOEQ_GRAS = [
    ("oratory1990 AutoEq (GRAS 45BC)", "oratory1990_Sennheiser_HD820.csv"),
    ("Super Review (GRAS KB006x + 711)", "superreview_Sennheiser_HD820.csv"),
]

# 其他量測台: RIG_TARGET_LO 以下各量測台差異小，仍用 HARMAN_CSV; 往上一個八度內過渡到該量測台的目標曲線
AUTOEQ_OTHER_RIGS = [
    ("HypetheSonics (B&K 5128)", "HypetheSonics_Sennheiser_HD_820.csv", "target_hypethesonics_5128.csv"),
    ("Rtings (HMS II.3)", "Rating_Sennheiser_HD_820.csv", "target_rtings_hms_ii.3.csv"),
]
RIG_TARGET_LO = 500.0

ALIGN_LO, ALIGN_HI = 100.0, 3000.0

# (Hz, 平滑寬度 octave)，相鄰兩點之間在對數頻率上線性過渡
SMOOTHING = [
    (4000, 1.0 / 6),
    (6000, 1.0 / 3),
    (10000, 1.0),
]

# 高於此頻率 EQ 維持定值: Harman 目標在此陡降且各量測差異極大，不做修正
TREBLE_HOLD = 1200.0

UNIT_CORR_CSV = "unit_correction.csv"
UNIT_CORR_WEIGHT = 0.5
# 額外低頻 shelf (Hz, Q, dB): B&K 5128 與 HMS II.3 的低頻比 GRAS 少約 5 dB (真人佩戴漏氣較多)，供聽感調整
BASS_SHELF = (105, 0.71, 0)

# preamp 由 EQ 最大增益自動決定，再多留一點餘量避免削波
HEADROOM_DB = 0.5

# tap 數與取樣率成正比，頻率解析度維持約 0.7 Hz (濾波器長約 1.4 秒，最小相位延遲接近零)
TAPS = {
    44100: 65536,
    48000: 65536,
    88200: 131072,
    96000: 131072,
    176400: 262144,
    192000: 262144,
}

OUT_DIR = sys.argv[1] if len(sys.argv) > 1 else "/home/yijan/camilladsp/coeffs"

PTS_PER_OCT = 384


def analog_lowshelf_mag(f, f0, q, gain_db):
    """直接計算連續時間 s 域 (s = jw) 類比 Low-Shelf 濾波器"""
    A = 10.0**(gain_db / 40.0)
    x = f / float(f0)
    num = (A - x**2)**2 + (np.sqrt(A) * x / q)**2
    den = (1.0 - A * (x**2))**2 + (np.sqrt(A) * x / q)**2
    return A * np.sqrt(num / den)


def read_columns(name, cols):
    rows = []
    with open(os.path.join(DATA_DIR, name)) as fh:
        for r in csv.reader(fh):
            try:
                rows.append([float(r[c]) for c in cols])
            except (ValueError, IndexError):
                pass
    return np.array(rows).T


def hann_smooth(y, octave):
    win = np.hanning(int(2 * octave * PTS_PER_OCT) + 1)
    win /= win.sum()
    pad = len(win) // 2
    return np.convolve(np.pad(y, pad, mode="edge"), win, mode="valid")


def variable_smooth(y, log_grid):
    pos = np.interp(log_grid, np.log([p[0] for p in SMOOTHING]), np.arange(len(SMOOTHING)))
    out = np.zeros_like(y)
    for i, (_, octave) in enumerate(SMOOTHING):
        out += np.clip(1 - np.abs(pos - i), 0, 1) * hann_smooth(y, octave)
    return out


def load_deviations(grid):
    """回傳 (名稱, 對數網格上的 Harman 偏差 dB) 清單"""
    log_grid = np.log(grid)

    def on_grid(name, cols):
        data = read_columns(name, cols)
        return [np.interp(log_grid, np.log(data[0]), c) for c in data[1:]]

    harman, = on_grid(HARMAN_CSV, (0, 1))
    devs = []
    raw, target = on_grid("oratory1990_pdf.csv", (0, 1, 2))
    devs.append(("oratory1990 PDF (GRAS 45BC-10)", raw - target))
    # ASR 原始數據為線性 1.6 Hz 間隔，插值到對數網格後才平滑，低頻不會鋸齒; 左右差視為佩戴誤差而平均
    left, right = on_grid("ASR_Sennheiser_HD820.csv", (0, 1, 3))
    devs.append(("ASR (GRAS 45CA-10, RA0402)", (left + right) / 2))
    for label, name in AUTOEQ_GRAS:
        raw, = on_grid(name, (0, 1))
        devs.append((label, raw - harman))
    w = np.clip(np.log(grid / RIG_TARGET_LO) / np.log(2), 0, 1)
    sel = (grid >= RIG_TARGET_LO * 0.6) & (grid <= RIG_TARGET_LO * 2)
    for label, name, target_name in AUTOEQ_OTHER_RIGS:
        raw, = on_grid(name, (0, 1))
        target, = on_grid(target_name, (0, 1))
        target += np.median(harman[sel] - target[sel])
        devs.append((label, raw - ((1 - w) * harman + w * target)))

    sel = (grid >= ALIGN_LO) & (grid <= ALIGN_HI)
    ref = devs[0][1]
    return [(label, d - np.median(d[sel] - ref[sel])) for label, d in devs]


def load_measurement_eq():
    """六份 Harman 偏差逐頻率取中位數，平滑後取負值，回傳對數頻率網格上的 EQ (dB)"""
    f_lo, f_hi = 20.0, 19990.0
    grid = np.geomspace(f_lo, f_hi, int(PTS_PER_OCT * np.log2(f_hi / f_lo)))
    log_grid = np.log(grid)
    devs = load_deviations(grid)
    stack = np.vstack([d for _, d in devs])
    median = np.median(stack, axis=0)
    for label, d in devs:
        sel = (grid >= 20) & (grid <= 5000)
        print(f"{label:34s} vs median 20 Hz-5 kHz rms {np.sqrt(np.mean((d - median)[sel]**2)):.2f} dB")
    harman = read_columns(HARMAN_CSV, (0, 1))
    target = read_columns(TARGET_CSV, (0, 1))
    median -= np.interp(log_grid, np.log(target[0]), target[1]) -np.interp(log_grid, np.log(harman[0]), harman[1]) 
    print(f" target:{TARGET_CSV}")
    return grid, -variable_smooth(median, log_grid)

def load_unit_corr_eq(grid):
    """回傳對數頻率網格上的單體修正 EQ (dB)"""
    if not UNIT_CORR_WEIGHT:
        return np.zeros_like(grid)
    log_grid = np.log(grid)
    f_band, corr = read_columns(UNIT_CORR_CSV, (0, 3))
    third = 2.0**(1.0 / 3.0)
    f_band = np.concatenate([[f_band[0] / third], f_band, [f_band[-1] * third]])
    corr = np.concatenate([[0.0], corr, [0.0]])
    corr = UNIT_CORR_WEIGHT * np.interp(log_grid, np.log(f_band), corr, left=0.0, right=0.0)
    print(f" unit correction: {UNIT_CORR_CSV} x {UNIT_CORR_WEIGHT}")
    return -variable_smooth(corr, log_grid)
MEAS_F, MEAS_EQ_DB = load_measurement_eq()
UNIT_EQ_DB = load_unit_corr_eq(MEAS_F)


def eq_mag(freqs):    
    log_f = np.log(np.clip(freqs, 1e-3, None))
    meas_db = np.interp(np.log(np.clip(freqs, 1e-3, TREBLE_HOLD)), np.log(MEAS_F), MEAS_EQ_DB)
    meas_db += np.interp(log_f, np.log(MEAS_F), UNIT_EQ_DB)
    return 10.0**(meas_db / 20.0) * analog_lowshelf_mag(freqs, *BASS_SHELF)


def minimum_phase_ir(fs, n_taps):
    """以倒頻譜 (homomorphic) 法由振幅曲線求最小相位脈衝響應"""
    # FFT 長度取 tap 數的 8 倍，避免倒頻譜混疊
    n_fft = 8 * n_taps
    freqs = np.fft.rfftfreq(n_fft, d=1.0 / fs)
    cep = np.fft.irfft(np.log(eq_mag(freqs)), n=n_fft)
    fold = np.zeros(n_fft)
    fold[0] = cep[0]
    fold[1:n_fft // 2] = 2 * cep[1:n_fft // 2]
    fold[n_fft // 2] = cep[n_fft // 2]
    ir = np.fft.irfft(np.exp(np.fft.rfft(fold)), n=n_fft)[:n_taps]
    # 只淡出尾端: 取 Blackman 窗的後半段
    return ir * np.blackman(2 * n_taps)[n_taps:]


def max_error_db(ir, fs, gain_db):
    n_fft = 1 << 20
    f_dense = np.fft.rfftfreq(n_fft, d=1.0 / fs)
    sel = (f_dense >= 20.0) & (f_dense <= 20000.0)
    actual_db = 20 * np.log10(np.abs(np.fft.rfft(ir, n=n_fft)[sel]))
    target_db = 20 * np.log10(eq_mag(f_dense[sel])) + gain_db
    return np.max(np.abs(actual_db - target_db))


# 1. 以 1/96 倍頻程掃描 20 Hz - 20 kHz，找出最大增益並決定 preamp
f_check = np.geomspace(20.0, 20000.0, 96 * 10)
eq_check_db = 20 * np.log10(eq_mag(f_check))
peak_db = np.max(eq_check_db)
gain_db = -peak_db - HEADROOM_DB
print(f"EQ peak: {peak_db:+.2f} dB @ {f_check[np.argmax(eq_check_db)]:.0f} Hz, "
      f"preamp {gain_db:+.2f} dB (headroom {HEADROOM_DB:.1f} dB)")

gain_lin = 10.0**(gain_db / 20.0)

for fs, n_taps in TAPS.items():
    # 2. 最小相位: 沒有前振鈴，延遲接近零
    ir = minimum_phase_ir(fs, n_taps) * gain_lin

    # 3. 驗證實際響應與目標的誤差; 延遲以脈衝峰值位置估計
    max_err = max_error_db(ir, fs, gain_db)
    latency_ms = np.argmax(np.abs(ir)) / fs * 1000
    path = f"{OUT_DIR}/hd820_minphase_{fs}.raw"
    ir.astype(np.float64).tofile(path)
    print(f"{fs:>6} Hz minphase: {n_taps} taps, latency {latency_ms:.1f} ms, "
          f"max error {max_err:.3f} dB -> {path}")