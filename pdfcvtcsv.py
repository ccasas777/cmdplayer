import re
import sys
import csv
import zlib
import numpy as np

RAW_COLOR = (0.929, 0.490, 0.192)      # 橘色: Raw Frequency Response
TARGET_COLOR = (0.329, 0.510, 0.208)   # 綠色粗線: Harman AE/OE 2018 Target
Y_MIN, Y_MAX = -20.0, 30.0

TOKEN = re.compile(rb"\(.*?\)|<[0-9a-fA-F]*>|/[a-zA-Z0-9_]+|[^ \t\r\n\(\)<>]+")
NUMBER = re.compile(rb"[-+]?\d*\.?\d+")



def page_content(pdf):
    """解壓所有 FlateDecode 串流，回傳含最多繪圖指令的那一個 (頁面內容)"""
    best = b""
    for m in re.finditer(rb"<<(.*?)>>\s*stream\r?\n(.*?)endstream", pdf, re.S):
        if b"FlateDecode" not in m.group(1) or b"/Image" in m.group(1):
            continue
        try:
            data = zlib.decompress(m.group(2))
        except zlib.error:
            continue
        if data.count(b" l ") > best.count(b" l "):
            best = data
    return best


def mat_mul(a, b):
    return (a[0] * b[0] + a[1] * b[2], a[0] * b[1] + a[1] * b[3],
            a[2] * b[0] + a[3] * b[2], a[2] * b[1] + a[3] * b[3],
            a[4] * b[0] + a[5] * b[2] + b[4], a[4] * b[1] + a[5] * b[3] + b[5])


def parse_paths(stream):
    """回傳所有繪製的路徑: op、顏色、線寬 (頁面座標)、頂點陣列; 曲線 (c/v/y) 只取端點"""
    state = {"ctm": (1, 0, 0, 1, 0, 0), "stroke": None, "fill": None, "lw": 1.0}
    stack, nums, paths, subpaths, cur = [], [], [], [], None

    def xy(x, y):
        m = state["ctm"]
        return m[0] * x + m[2] * y + m[4], m[1] * x + m[3] * y + m[5]

    for tok in TOKEN.findall(stream):
        if NUMBER.fullmatch(tok):
            nums.append(float(tok))
            continue
        if tok[:1] in b"/()[<":
            continue
        op = tok.decode()
        if op == "q":
            stack.append(dict(state))
        elif op == "Q":
            state = stack.pop() if stack else state
        elif op == "cm":
            state["ctm"] = mat_mul(tuple(nums[-6:]), state["ctm"])
        elif op == "w":
            state["lw"] = nums[-1]
        elif op in ("SC", "SCN", "RG"):
            state["stroke"] = tuple(nums)
        elif op in ("sc", "scn", "rg"):
            state["fill"] = tuple(nums)
        elif op == "G":
            state["stroke"] = (nums[-1],) * 3
        elif op == "g":
            state["fill"] = (nums[-1],) * 3
        elif op == "m":
            if cur:
                subpaths.append(cur)
            cur = [xy(*nums[-2:])]
        elif op in ("l", "c", "v", "y"):
            cur.append(xy(*nums[-2:]))
        elif op == "re":
            if cur:
                subpaths.append(cur)
            x, y, w, h = nums[-4:]
            cur = [xy(x, y), xy(x + w, y), xy(x + w, y + h), xy(x, y + h)]
        elif op in ("S", "s", "f", "F", "f*", "B", "b", "B*", "b*", "n"):
            if cur:
                subpaths.append(cur)
            if op != "n":
                stroke = op in ("S", "s", "B", "b", "B*", "b*")
                lw = state["lw"] * np.hypot(state["ctm"][0], state["ctm"][1])
                for sp in subpaths:
                    paths.append({"op": op, "color": state["stroke"] if stroke else state["fill"],
                                  "lw": lw, "pts": np.array(sp)})
            subpaths, cur = [], None
        nums = []
    return paths


def has_color(p, color):
    return p["color"] is not None and len(p["color"]) == 3 and np.allclose(p["color"], color, atol=2e-3)


def inside(pts, box, check_y=True):
    """曲線可能超出 y 軸範圍 (圖上被裁切)，判斷曲線所屬圖框時只看 x"""
    (x0, y0), (x1, y1) = box
    in_x = pts[:, 0].min() >= x0 - 1 and pts[:, 0].max() <= x1 + 1
    return in_x and (not check_y or (pts[:, 1].min() >= y0 - 1 and pts[:, 1].max() <= y1 + 1))


def extract(pdf_path):
    paths = parse_paths(page_content(open(pdf_path, "rb").read()))

    # 圖框: 黑色描邊矩形; 包含橘色 raw 曲線、位置最左上的那一個
    frames = [(p["pts"].min(0), p["pts"].max(0)) for p in paths
              if p["op"] in ("S", "s") and has_color(p, (0, 0, 0)) and 4 <= len(p["pts"]) <= 5]
    frames = [b for b in frames if b[1][0] - b[0][0] > 150 and b[1][1] - b[0][1] > 100]
    raw_paths = [p for p in paths if p["op"] == "S" and has_color(p, RAW_COLOR) and len(p["pts"]) > 20]
    in_frame = lambda pts, b: inside(pts, b, check_y=False) and b[0][1] - 50 < pts[:, 1].mean() < b[1][1] + 50
    frames = [b for b in frames if any(in_frame(p["pts"], b) for p in raw_paths)]
    box = min(frames, key=lambda b: (round(b[0][0]), -b[1][1]))

    raw = np.vstack([p["pts"] for p in raw_paths if in_frame(p["pts"], box)])
    target = np.vstack([p["pts"] for p in paths if p["op"] == "S" and has_color(p, TARGET_COLOR)
                        and p["lw"] > 2 and len(p["pts"]) > 20 and in_frame(p["pts"], box)])

    # 格線校正: 灰色兩點直線，垂直線跨滿圖高、水平線跨滿圖寬
    (x0, y0), (x1, y1) = box
    vert, horiz = set(), set()
    for p in paths:
        pts = p["pts"]
        if p["op"] != "S" or len(pts) != 2 or p["color"] is None or min(p["color"]) < 0.7 or not inside(pts, box):
            continue
        (ax, ay), (bx, by) = pts
        if abs(ax - bx) < 0.01 and abs(ay - by) > 0.9 * (y1 - y0):
            vert.add(round(ax, 2))
        elif abs(ay - by) < 0.01 and abs(ax - bx) > 0.9 * (x1 - x0):
            horiz.add(round(ay, 2))
    vert, horiz = np.array(sorted(vert)), np.array(sorted(horiz))
    grid_f = np.unique([m * 10**d for d in range(1, 5) for m in range(1, 10) if m * 10**d <= 20000])
    grid_db = np.arange(Y_MIN, Y_MAX + 1)
    if len(vert) != len(grid_f) or len(horiz) != len(grid_db):
        sys.exit(f"格線數量不符: 垂直 {len(vert)} (預期 {len(grid_f)}), 水平 {len(horiz)} (預期 {len(grid_db)})")
    fx = np.polyfit(vert, np.log10(grid_f), 1)
    fy = np.polyfit(horiz, grid_db, 1)
    print(f"grid fit max residual: {np.max(np.abs(np.polyval(fx, vert) - np.log10(grid_f))) / np.log10(2):.4f} oct, "
          f"{np.max(np.abs(np.polyval(fy, horiz) - grid_db)):.3f} dB")

    def to_curve(pts):
        f, db = 10**np.polyval(fx, pts[:, 0]), np.polyval(fy, pts[:, 1])
        order = np.argsort(f)
        f, db = f[order], db[order]
        keep = np.r_[True, np.diff(f) > 1e-6]
        return f[keep], db[keep]

    return to_curve(raw), to_curve(target)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    (f, raw), (ft, target) = extract(sys.argv[1])
    target_on_f = np.interp(np.log(f), np.log(ft), target)
    with open(sys.argv[2], "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["frequency_Hz", "raw_dBr", "harman_oe2018_dBr"])
        for row in zip(f, raw, target_on_f):
            w.writerow([f"{row[0]:.2f}", f"{row[1]:.3f}", f"{row[2]:.3f}"])
    print(f"{len(f)} points, {f[0]:.1f}-{f[-1]:.0f} Hz -> {sys.argv[2]}")


if __name__ == "__main__":
    main()