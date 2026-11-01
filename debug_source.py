import os
import sys
import csv
import numpy as np

# 兩台電腦各跑一次後比對輸出:
#  [data] 不同 -> hd820_data 內容不同 (只比數值，不受換行字元影響)
#  [data] 相同但 [deviation] 不同 -> linearFIR.py 的程式碼不同
here = os.path.dirname(os.path.abspath(__file__))
data_dir = os.path.join(here, "source")

print(f"python {sys.version.split()[0]}, numpy {np.__version__}")
print("[data] rows, first/last frequency, sum of each column")
for name in sorted(os.listdir(data_dir)):
    rows = []
    with open(os.path.join(data_dir, name)) as fh:
        for r in csv.reader(fh):
            try:
                rows.append([float(v) for v in r if v.strip()])
            except ValueError:
                pass
    width = max(set(map(len, rows)), key=[len(r) for r in rows].count)
    a = np.array([r for r in rows if len(r) == width])
    sums = " ".join(f"{s:.4f}" for s in a.sum(axis=0))
    print(f"  {name:34s} {len(a):5d} {a[0, 0]:g}..{a[-1, 0]:g}  {sums}")
    
src = open(os.path.join(here, "create_FIR.py"), encoding="utf-8").read()
ns = {"__file__": os.path.join(here, "create_FIR.py")}
exec(src[:src.index("MEAS_F, MEAS_EQ_DB = load_measurement_eq()")], ns)

grid = np.geomspace(20.0, 19990.0, int(ns["PTS_PER_OCT"] * np.log2(19990.0 / 20.0)))
devs = ns["load_deviations"](grid)
median = np.median(np.vstack([d for _, d in devs]), axis=0)
check = [30, 60, 100, 200, 450, 1000, 2000, 3000, 5000]
print("[deviation] dB at", check)
for label, d in devs + [("median", median)]:
    vals = " ".join(f"{np.interp(np.log(f), np.log(grid), d):+7.2f}" for f in check)
    print(f"  {label:34s} {vals}")