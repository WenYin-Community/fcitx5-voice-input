#!/usr/bin/env python3
"""生成 fcitx5-voice-input 项目海报（中文主版 + 英文版）。

设计取向：以视觉为主体，只做功能简介。不放安装命令、不放配置项清单——
那些内容属于 README 与 Pages 的职责，海报应一眼看清"这是什么、能做什么"。
用 Pillow 直接绘制而非 AI 生图：文案需与仓库严格一致，矢量式绘制保证零错字，
且可随后续版本重新生成。
2 倍超采样后缩放，确保文字锐利。
"""
from PIL import Image, ImageDraw, ImageFont
import os

SS = 2  # 超采样倍数
FONTS = "/usr/share/fonts/google-noto-sans-cjk-fonts"


def font(weight, size):
    # index 2 = Noto Sans CJK SC（简体）
    return ImageFont.truetype(f"{FONTS}/NotoSansCJK-{weight}.ttc", size * SS, index=2)


BG_TOP = (23, 26, 43)
BG_BOT = (32, 38, 62)
TEXT = (240, 243, 250)
MUTED = (152, 162, 188)
CARD = (40, 46, 72)


def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


def gradient(size):
    w, h = size
    base = Image.new("RGB", (1, h))
    d = ImageDraw.Draw(base)
    for y in range(h):
        d.point((0, y), fill=lerp(BG_TOP, BG_BOT, y / max(h - 1, 1)))
    return base.resize((w, h), Image.BILINEAR)


def mic_icon(box, bg, fg, radius):
    """在独立图层上绘制图标后返回，避免与海报坐标系混用。

    box 为设备像素下的方形区域 (x0, y0, size)。
    所有几何量都按区域尺寸的比例计算，与 SS 无关。
    """
    x0, y0, size = box
    layer = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    ld = ImageDraw.Draw(layer)
    ld.rounded_rectangle([0, 0, size - 1, size - 1], radius=radius, fill=bg)

    s = size / 100.0                     # 以 100 为设计基准单位
    cx, cy = size / 2, size / 2
    cap_w, cap_h = 20 * s, 40 * s
    ld.rounded_rectangle(
        [cx - cap_w / 2, cy - 16 * s, cx + cap_w / 2, cy - 16 * s + cap_h],
        radius=cap_w / 2, fill=fg,
    )
    ld.arc([cx - 23 * s, cy + 2 * s, cx + 23 * s, cy + 42 * s],
           start=0, end=180, fill=fg, width=max(2, int(4 * s)))
    ld.line([cx, cy + 38 * s, cx, cy + 46 * s], fill=fg, width=max(2, int(4 * s)))
    ld.line([cx - 14 * s, cy + 46 * s, cx + 14 * s, cy + 46 * s],
            fill=fg, width=max(2, int(4 * s)))
    return layer


def make_poster(lang):
    W, H = 1280, 640
    img = gradient((W * SS, H * SS))
    d = ImageDraw.Draw(img, "RGBA")
    zh = lang == "zh"

    # ── 背景光斑 ──────────────────────────────────────────
    glow = Image.new("RGBA", img.size, (0, 0, 0, 0))
    gd = ImageDraw.Draw(glow)
    for cx, cy, rad, col in [
        (1010, 96, 330, (99, 179, 255, 44)),
        (1180, 560, 250, (168, 130, 255, 34)),
        (90, 600, 230, (99, 179, 255, 22)),
    ]:
        gd.ellipse([(cx - rad) * SS, (cy - rad) * SS,
                    (cx + rad) * SS, (cy + rad) * SS], fill=col)
    img.paste(glow, (0, 0), glow)

    # ── 页头：图标 + 标题 + 一行简介 ──────────────────────
    ix, iy, isz = 92, 88, 84
    icon = mic_icon((ix * SS, iy * SS, isz * SS),
                    bg=(99, 179, 255, 245), fg=(255, 255, 255, 255), radius=22 * SS)
    img.paste(icon, (ix * SS, iy * SS), icon)

    tx = ix + isz + 30
    d.text((tx * SS, (iy + 2) * SS), "fcitx5-voice-input",
           font=font("Bold", 48), fill=TEXT)

    sub = ("Fcitx5 语音输入插件 · 说出即上屏" if zh
           else "Voice input addon for Fcitx5")
    d.text((tx * SS, (iy + 62) * SS), sub, font=font("Medium", 22), fill=(99, 179, 255))

    d.line([(92 * SS, 212 * SS), ((W - 92) * SS, 212 * SS)],
           fill=(255, 255, 255, 32), width=int(SS))

    # ── 三张功能卡片（只列功能名，不放解释性长句）──────────
    feats = (
        [("语音识别", "OpenAI 兼容 API\n火山引擎豆包流式\n小米 MiMo"),
         ("录音方式", "VAD 自动分段\n按住说话（PTT）\n音量电平显示"),
         ("部署", "DEB / RPM / Arch\n运行期 dlopen\n多发行版 CI")]
        if zh else
        [("Recognition", "OpenAI-compatible API\nVolcengine Doubao\nXiaomi MiMo"),
         ("Recording", "VAD auto-segmentation\nPush-to-talk (PTT)\nLive level meter"),
         ("Packaging", "DEB / RPM / Arch\nRuntime dlopen\nMulti-distro CI")]
    )

    cy0, card_h, gap = 250, 208, 26
    cw = (W - 92 * 2 - gap * 2) // 3
    accents = [(99, 179, 255), (168, 130, 255), (110, 214, 178)]
    for i, (title, body) in enumerate(feats):
        x = 92 + i * (cw + gap)
        d.rounded_rectangle([x * SS, cy0 * SS, (x + cw) * SS, (cy0 + card_h) * SS],
                            radius=16 * SS, fill=(*CARD, 232))
        d.rectangle([x * SS, cy0 * SS, (x + 54) * SS, (cy0 + 4) * SS], fill=(*accents[i], 255))
        d.text(((x + 24) * SS, (cy0 + 28) * SS), title,
               font=font("Bold", 22), fill=TEXT)
        for j, line in enumerate(body.split("\n")):
            y = cy0 + 82 + j * 38
            d.ellipse([(x + 26) * SS, (y + 9) * SS, (x + 33) * SS, (y + 16) * SS],
                      fill=(*accents[i], 255))
            d.text(((x + 44) * SS, y * SS), line, font=font("Regular", 18), fill=MUTED)

    # ── 页脚 ─────────────────────────────────────────────
    d.text((92 * SS, (H - 62) * SS), "github.com/WenYin-Community/fcitx5-voice-input",
           font=font("Medium", 16), fill=(99, 179, 255))
    lic = "LGPL-3.0 · Linux · Fcitx5 ≥ 5.1.19"
    tw = d.textlength(lic, font=font("Regular", 15))
    d.text(((W - 92) * SS - tw, (H - 61) * SS), lic,
           font=font("Regular", 15), fill=MUTED)

    return img.convert("RGB").resize((W, H), Image.LANCZOS)


def main():
    outdir = "docs/assets"
    os.makedirs(outdir, exist_ok=True)
    for lang, name in (("zh", "poster-zh.png"), ("en", "poster-en.png")):
        img = make_poster(lang)
        path = os.path.join(outdir, name)
        img.save(path, "PNG", optimize=True)
        print(f"  {path}  ({os.path.getsize(path) / 1024:.0f} KB, {img.size[0]}x{img.size[1]})")


if __name__ == "__main__":
    main()
