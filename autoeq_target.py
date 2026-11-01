"""由從 AutoEq GitHub 下載的檔案產生 linearFIR.py 需要的目標曲線

下載來源 (https://github.com/jaakkopasanen/AutoEq):
  harman       targets/Harman over-ear 2018.csv
  hypethesonics results/HypetheSonics/over-ear/Sennheiser HD 820/Sennheiser HD 820.csv
  rtings       results/Rtings/HMS II.3 over-ear/Sennheiser HD 820/Sennheiser HD 820.csv
後兩者是 AutoEq 的結果 CSV，取其 target 欄 (AutoEq 對該量測台實際使用的目標曲線)

用法: python3 autoeq_target.py <harman.csv> <hypethesonics 結果.csv> <rtings 結果.csv> [輸出目錄，預設 source]
"""
import os
import sys
import csv
import shutil


def write_target(result_csv, out_path):
    with open(result_csv) as fh:
        rows = list(csv.DictReader(fh))
    with open(out_path, "w", newline="") as fh:
        w = csv.writer(fh, lineterminator="\n")
        w.writerow(["frequency", "raw"])
        for r in rows:
            w.writerow([r["frequency"], r["target"]])
    print(f"{out_path}: {len(rows)} points <- target column of {result_csv}")


def main():
    if len(sys.argv) not in (4, 5):
        sys.exit(__doc__)
    harman, hypethesonics, rtings = sys.argv[1:4]
    out_dir = sys.argv[4] if len(sys.argv) == 5 else "source"
    os.makedirs(out_dir, exist_ok=True)

    out = os.path.join(out_dir, "harman_over-ear_2018.csv")
    if os.path.realpath(harman) == os.path.realpath(out):
        print(f"{out}: already in place")
    else:
        shutil.copyfile(harman, out)
        print(f"{out}: copied from {harman}")
    write_target(hypethesonics, os.path.join(out_dir, "target_hypethesonics_5128.csv"))
    write_target(rtings, os.path.join(out_dir, "target_rtings_hms_ii.3.csv"))


if __name__ == "__main__":
    main()