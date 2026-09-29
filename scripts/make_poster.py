#!/usr/bin/env python3
"""生成 fcitx5-voice-input 项目海报（竖版 1080x1350，中文主版 + 英文版）。

设计取向：以视觉为主体，只做「原理一图流 + 功能清单」。不放安装命令、不放配置项
清单——那些内容属于 README 与 Pages 的职责，海报应一眼看清"这是什么、怎么工作、
能做什么"。用 Pillow 直接绘制而非 AI 生图：文案需与仓库严格一致，矢量式绘制保证
零错字，且可随后续版本重新生成。2 倍超采样后缩放，确保文字锐利。
"""
from PIL import Image, ImageDraw, ImageFilter, ImageFont
import os
import re

SS = 2  # 超采样倍数
FONTS = "/usr/share/fonts/google-noto-sans-cjk-fonts"

W, H = 1080, 1350          # 竖版 4:5，适配主流社交平台
MARGIN = 84
ACCENTS = [(99, 179, 255), (168, 130, 255), (110, 214, 178)]

BG_TOP = (23, 26, 43)
BG_BOT = (32, 38, 62)
TEXT = (240, 243, 250)
MUTED = (152, 162, 188)
CARD = (40, 46, 72)


def font(weight, size):
    # index 2 = Noto Sans CJK SC（简体）
    return ImageFont.truetype(f"{FONTS}/NotoSansCJK-{weight}.ttc", size * SS, index=2)


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


TOKEN = re.compile(r"[\u2000-\u9fff\uff00-\uffef]|[A-Za-z0-9_.+/≥-]+|\s+|[^\s]")


def wrap_text(d, text, fnt, max_w):
    """按像素宽度折行：CJK 逐字断，西文按词断，避免把单词切开。"""
    lines, cur = [], ""
    for tok in TOKEN.findall(text):
        trial = cur + tok
        if cur and d.textlength(trial, font=fnt) > max_w:
            lines.append(cur.rstrip())
            cur = "" if tok.isspace() else tok
        else:
            cur = trial
    if cur.strip():
        lines.append(cur.rstrip())
    return lines


def section_title(d, text, y):
    d.rounded_rectangle([MARGIN * SS, (y + 3) * SS,
                         (MARGIN + 5) * SS, (y + 25) * SS],
                        radius=2 * SS, fill=(*ACCENTS[0], 255))
    d.text(((MARGIN + 20) * SS, y * SS), text, font=font("Bold", 27), fill=TEXT)


def make_poster(lang):
    img = gradient((W * SS, H * SS))
    d = ImageDraw.Draw(img, "RGBA")
    zh = lang == "zh"

    # ── 背景光斑（竖版纵向分布）────────────────────────────
    glow = Image.new("RGBA", img.size, (0, 0, 0, 0))
    gd = ImageDraw.Draw(glow)
    for cx, cy, rad, col in [
        (940, 120, 340, (99, 179, 255, 46)),
        (110, 660, 300, (168, 130, 255, 30)),
        (930, 1210, 320, (99, 179, 255, 26)),
    ]:
        gd.ellipse([(cx - rad) * SS, (cy - rad) * SS,
                    (cx + rad) * SS, (cy + rad) * SS], fill=col)
    # 硬边椭圆在放大后会露出明显轮廓，模糊成柔和光晕
    glow = glow.filter(ImageFilter.GaussianBlur(radius=110 * SS / 2))
    img.paste(glow, (0, 0), glow)

    # ── 页头：图标 + 标题 + 一行简介 ──────────────────────
    ix, iy, isz = MARGIN, 80, 96
    icon = mic_icon((ix * SS, iy * SS, isz * SS),
                    bg=(99, 179, 255, 245), fg=(255, 255, 255, 255), radius=25 * SS)
    img.paste(icon, (ix * SS, iy * SS), icon)

    tx = ix + isz + 30
    d.text((tx * SS, (iy + 4) * SS), "fcitx5-voice-input",
           font=font("Bold", 48), fill=TEXT)
    sub = ("Fcitx5 语音输入插件 · 说出即上屏" if zh
           else "Voice input addon for Fcitx5")
    d.text((tx * SS, (iy + 66) * SS), sub, font=font("Medium", 23), fill=ACCENTS[0])

    d.line([(MARGIN * SS, 228 * SS), ((W - MARGIN) * SS, 228 * SS)],
           fill=(255, 255, 255, 32), width=int(SS))

    # ── 工作原理：一句话说明 + 四步流程 ───────────────────
    section_title(d, "工作原理" if zh else "How it works", 266)

    body = ("本机只做音频采集和端点检测（Silero VAD），语音经 HTTPS / WSS 送往云端 ASR "
            "转写，结果回传后直接上屏——本地不加载任何语音识别模型。"
            if zh else
            "The addon captures audio and finds speech endpoints locally with Silero VAD, "
            "streams the audio to a cloud ASR over HTTPS / WSS, then commits the text "
            "straight into the focused input box. No speech model runs on your machine.")
    for i, line in enumerate(wrap_text(d, body, font("Regular", 17),
                                       (W - 2 * MARGIN) * SS)):
        d.text((MARGIN * SS, (312 + i * 28) * SS), line,
               font=font("Regular", 17), fill=MUTED)

    steps = ([("音频采集", "16 kHz 单声道"), ("VAD 分段", "Silero 端点检测"),
              ("云端识别", "HTTPS / WSS"), ("自动上屏", "插入光标处")]
             if zh else
             [("Capture", "16 kHz mono"), ("VAD split", "Silero endpoint"),
              ("Cloud ASR", "HTTPS / WSS"), ("Commit", "focused input box")])
    fy, fh, gap = 430, 116, 34
    bw = (W - 2 * MARGIN - gap * 3) // 4
    for i, (label, note) in enumerate(steps):
        x = MARGIN + i * (bw + gap)
        d.rounded_rectangle([x * SS, fy * SS, (x + bw) * SS, (fy + fh) * SS],
                            radius=14 * SS, fill=(*CARD, 235))
        d.rounded_rectangle([x * SS, fy * SS, (x + bw) * SS, (fy + 4) * SS],
                            radius=2 * SS, fill=(*ACCENTS[i % 3], 255))
        cx = x + bw / 2
        d.text((cx * SS, (fy + 46) * SS), label,
               font=font("Bold", 21), fill=TEXT, anchor="mm")
        d.text((cx * SS, (fy + 82) * SS), note,
               font=font("Regular", 14), fill=MUTED, anchor="mm")

        if i < 3:  # 步骤之间的箭头
            ax0, ax1 = x + bw + 8, x + bw + gap - 8
            ay = fy + fh / 2
            d.line([(ax0 * SS, ay * SS), ((ax1 - 7) * SS, ay * SS)],
                   fill=(*MUTED, 200), width=max(2, int(2.2 * SS)))
            d.polygon([((ax1 - 8) * SS, (ay - 6) * SS), (ax1 * SS, ay * SS),
                       ((ax1 - 8) * SS, (ay + 6) * SS)], fill=(*MUTED, 220))

    d.line([(MARGIN * SS, 566 * SS), ((W - MARGIN) * SS, 566 * SS)],
           fill=(255, 255, 255, 32), width=int(SS))

    # ── 功能卡片（三项，竖排通栏；只列功能名）─────────────
    section_title(d, "功能特性" if zh else "Features", 600)

    feats = (
        [("语音识别", [(0, "OpenAI 兼容 API"), (0, "火山引擎豆包流式"),
                       (0, "小米 MiMo")]),
         ("录音方式", [(1, "VAD 自动分段"), (1, "按住说话（PTT）"),
                       (1, "实时音量电平")]),
         ("系统集成", [(2, "DEB / RPM / Arch"), (2, "运行期 dlopen 加载录音库"),
                       (2, "LLM 后处理 · 自动上屏")])]
        if zh else
        [("Recognition", [(0, "OpenAI-compatible API"), (0, "Volcengine Doubao"),
                          (0, "Xiaomi MiMo")]),
         ("Recording", [(1, "VAD auto-segmentation"), (1, "Push-to-talk (PTT)"),
                        (1, "Live level meter")]),
         ("Integration", [(2, "DEB / RPM / Arch"), (2, "Runtime dlopen for audio libs"),
                          (2, "LLM post-processing")])]
    )

    cy0, card_h, cgap = 650, 158, 22
    for i, (title, items) in enumerate(feats):
        y = cy0 + i * (card_h + cgap)
        c = ACCENTS[i]
        d.rounded_rectangle([MARGIN * SS, y * SS, (W - MARGIN) * SS, (y + card_h) * SS],
                            radius=16 * SS, fill=(*CARD, 232))
        d.rounded_rectangle([MARGIN * SS, y * SS, (MARGIN + 5) * SS, (y + card_h) * SS],
                            radius=2 * SS, fill=(*c, 255))
        d.text(((MARGIN + 32) * SS, (y + 22) * SS), title,
               font=font("Bold", 22), fill=TEXT)
        for j, (_, item) in enumerate(items):
            ly = y + 68 + j * 30
            d.ellipse([(MARGIN + 34) * SS, (ly + 7) * SS,
                       (MARGIN + 41) * SS, (ly + 14) * SS], fill=(*c, 255))
            d.text(((MARGIN + 54) * SS, ly * SS), item,
                   font=font("Regular", 17), fill=MUTED)

    # ── 页脚（只写组织/仓库名，不放完整地址）──────────────
    d.line([(MARGIN * SS, 1218 * SS), ((W - MARGIN) * SS, 1218 * SS)],
           fill=(255, 255, 255, 32), width=int(SS))
    d.text((MARGIN * SS, 1262 * SS), "WenYin-Community/fcitx5-voice-input",
           font=font("Medium", 17), fill=ACCENTS[0], anchor="lm")
    d.text(((W - MARGIN) * SS, 1262 * SS), "LGPL-3.0 · Linux · Fcitx5 ≥ 5.1.19",
           font=font("Regular", 15), fill=MUTED, anchor="rm")

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
