#!/usr/bin/env python3
"""shot_compare.py DIR [DIR...]: the native picture against the emulated GS picture of the same present, for each pair
native_<cycle>.rgba / gs_<cycle>.rgba in each folder (RC_VK_SHOT_DIR with RC_RENDERER=vk RC_VK_PICTURE=native; renderer P1
step 8 batch 4). Colour only (R, G, B; the native picture keeps the GS's raw alpha, the emulated one is shown opaque).
A gs picture twice the native size in either direction (PS2X_GS_HW_SCALE=2) is sampled at every second pixel.
Prints per pair: pixels, the share that differ, the mean and largest channel difference, PSNR; then a total line."""
import math, os, re, sys


def load(path, w, h):
    with open(path, 'rb') as f:
        data = f.read()
    if len(data) != w * h * 4:
        return None
    return data


def main(dirs):
    tot_px = tot_diff = tot_sq = 0
    tot_abs = 0
    pairs = 0
    for d in dirs:
        names = sorted(os.listdir(d))
        for n in names:
            m = re.match(r'native_(\d+)\.rgba$', n)
            if not m:
                continue
            cyc = m.group(1)
            gs = os.path.join(d, f'gs_{cyc}.rgba')
            if not os.path.exists(gs):
                continue
            nat = open(os.path.join(d, n), 'rb').read()
            g = open(gs, 'rb').read()
            nw = 640
            nh = len(nat) // (nw * 4)
            # the gs picture: 640 or 1280 wide, lines = bytes / (w * 4)
            gw = 640 if len(g) % (640 * 4) == 0 and len(g) // (640 * 4) in (nh, 2 * nh) else 1280
            gh = len(g) // (gw * 4)
            sx, sy = gw // nw, gh // nh
            if sx not in (1, 2) or sy not in (1, 2) or gw * gh * 4 != len(g):
                print(f'{d}: cycle {cyc}: sizes {nw}x{nh} / {gw}x{gh} not comparable')
                continue
            diff = 0
            sq = 0
            ab = 0
            mx = 0
            for y in range(nh):
                rn = y * nw * 4
                rg = (y * sy) * gw * 4
                for x in range(nw):
                    a = rn + x * 4
                    b = rg + x * sx * 4
                    d0 = abs(nat[a] - g[b]); d1 = abs(nat[a + 1] - g[b + 1]); d2 = abs(nat[a + 2] - g[b + 2])
                    if d0 or d1 or d2:
                        diff += 1
                        m3 = max(d0, d1, d2)
                        if m3 > mx:
                            mx = m3
                        ab += d0 + d1 + d2
                        sq += d0 * d0 + d1 * d1 + d2 * d2
            px = nw * nh
            mse = sq / (px * 3)
            psnr = float('inf') if mse == 0 else 10 * math.log10(255 * 255 / mse)
            print(f'{os.path.basename(os.path.normpath(d))} cycle {cyc}: {px} pixels ({gw}x{gh} gs), {100.0 * diff / px:.2f}% differ, '
                  f'mean channel difference {ab / (px * 3):.2f}, largest {mx}, PSNR {psnr:.1f} dB')
            tot_px += px; tot_diff += diff; tot_sq += sq; tot_abs += ab; pairs += 1
    if pairs:
        mse = tot_sq / (tot_px * 3)
        psnr = float('inf') if mse == 0 else 10 * math.log10(255 * 255 / mse)
        print(f'total: {pairs} pairs, {tot_px} pixels, {100.0 * tot_diff / tot_px:.2f}% differ, mean channel difference '
              f'{tot_abs / (tot_px * 3):.2f}, PSNR {psnr:.1f} dB')
    else:
        print('no pairs')


if __name__ == '__main__':
    main(sys.argv[1:])
