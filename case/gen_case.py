#!/usr/bin/env python3
"""QuotaTV 外壳 STL 生成器 — Claude bot 橙色机器人造型
部件: body(机器人头主壳) / back(后盖) / antenna(天线) / stand(仰角底座带小脚)
用橙色 PLA 打印 (Claude 品牌色约 #D97757)
"""
import numpy as np
import trimesh
from manifold3d import Manifold, CrossSection, JoinType, set_circular_segments

set_circular_segments(64)

# ---------------- 参数 (单位 mm) ----------------
# 屏幕 1.8" ST7735
SCR_VIEW_W, SCR_VIEW_H = 36.5, 29.5     # 可视区开窗
SCR_PCB_W, SCR_PCB_H = 58.0, 35.0       # 模块 PCB, 按实物量!
SCR_Y_OFF = 4.0                          # 屏窗上移

WALL = 2.4
BODY_W, BODY_H, BODY_D = 74.0, 64.0, 30.0
CORNER = 8.0                             # 大圆角, 更像机器人头

LIP_D, LIP_CLEAR = 5.0, 0.28
USB_W, USB_H, USB_Z = 13.0, 8.0, 10.0    # micro-USB 槽

ANT_HOLE_D, ANT_STEM_D = 6.0, 5.6        # 天线孔/杆
ANT_STEM_H, ANT_BALL_D = 16.0, 11.0

TILT = 12.0                              # 底座仰角(度)
STAND_D, STAND_LIP = 46.0, 7.0


def rrect(w, h, r):
    """圆角矩形截面, 以中心为原点"""
    return CrossSection.square((w - 2 * r, h - 2 * r), center=True).offset(
        r, JoinType.Round)


def rbox(w, h, d, r):
    """圆角盒: XY 圆角, 沿 Z 拉伸, XY 中心原点, Z 从 0 起"""
    return rrect(w, h, r).extrude(d)


def make_body():
    outer = rbox(BODY_W, BODY_H, BODY_D, CORNER)
    inner = rbox(BODY_W - 2 * WALL, BODY_H - 2 * WALL, BODY_D, CORNER - 1.5)
    inner = inner.translate((0, 0, WALL))
    body = outer - inner

    # 屏幕开窗 (前脸 z=0..WALL), 外侧带 45° 倒角
    win = Manifold.cube((SCR_VIEW_W, SCR_VIEW_H, WALL + 1), True).translate(
        (0, SCR_Y_OFF, WALL / 2))
    bevel = CrossSection.square((SCR_VIEW_W + 3.2, SCR_VIEW_H + 3.2), center=True)
    bevel = bevel.extrude(WALL - 0.8,
                          scale_top=((SCR_VIEW_W) / (SCR_VIEW_W + 3.2),
                                     (SCR_VIEW_H) / (SCR_VIEW_H + 3.2)))
    bevel = bevel.translate((0, SCR_Y_OFF, -0.01))
    body = body - win - bevel

    # 屏幕定位框 (前脸内侧)
    fo = Manifold.cube((SCR_PCB_W + 3.6, SCR_PCB_H + 3.6, 3.0), True)
    fi = Manifold.cube((SCR_PCB_W + 0.6, SCR_PCB_H + 0.6, 4.0), True)
    frame = (fo - fi).translate((0, SCR_Y_OFF, WALL + 1.5))
    body = body + frame

    # 天线安装: 顶壁内侧加持握凸台 (d10 x 12), 再打通孔 (d6)
    boss = Manifold.cylinder(12, 5.0).rotate((90, 0, 0)).translate(
        (0, BODY_H / 2 - WALL + 0.01, BODY_D / 2))
    hole = Manifold.cylinder(WALL + 12.5, ANT_HOLE_D / 2).rotate((90, 0, 0)).translate(
        (0, BODY_H / 2 + 0.01, BODY_D / 2))
    body = body + boss - hole
    return body


def make_back():
    panel = rbox(BODY_W, BODY_H, WALL, CORNER)
    lip_o = rbox(BODY_W - 2 * WALL - 2 * LIP_CLEAR,
                 BODY_H - 2 * WALL - 2 * LIP_CLEAR, LIP_D, CORNER - 1.5)
    lip_i = rbox(BODY_W - 2 * WALL - 2 * LIP_CLEAR - 3.2,
                 BODY_H - 2 * WALL - 2 * LIP_CLEAR - 3.2, LIP_D + 1, CORNER - 3)
    lip = (lip_o - lip_i.translate((0, 0, -0.5))).translate((0, 0, WALL))
    back = panel + lip

    # micro-USB 槽 (底部)
    usb = Manifold.cube((USB_W, USB_H, WALL + LIP_D + 2), True).translate(
        (0, -BODY_H / 2 + USB_Z, (WALL + LIP_D) / 2))
    back = back - usb

    # 蜂鸣器出音孔 3x3 (右上)
    for dx in (-5, 0, 5):
        for dy in (-5, 0, 5):
            h = Manifold.cylinder(WALL + 2, 1.2).translate(
                (BODY_W / 2 - 16 + dx, BODY_H / 2 - 16 + dy, -1))
            back = back - h

    # 散热缝 (左中)
    for i in range(4):
        slot = rbox(22, 2.4, WALL + 2, 1.2).translate((-BODY_W / 2 + 25, -8 + i * 7, -1))
        back = back - slot
    return back


def make_antenna():
    stem = Manifold.cylinder(8 + ANT_STEM_H, ANT_STEM_D / 2)
    ball = Manifold.sphere(ANT_BALL_D / 2).translate((0, 0, 8 + ANT_STEM_H))
    return stem + ball


def make_stand():
    """楔形底座 + 两只小脚趾, 主体以 TILT 仰角插入"""
    w = BODY_W + 8
    # 侧面轮廓 (Y=深度, Z=高): 前低后高的楔形
    pts = [(0, 0), (STAND_D, 0), (STAND_D, 16), (STAND_D - 14, 16), (6, 3), (0, 3)]
    profile = CrossSection([pts])
    wedge = profile.extrude(w).rotate((90, 0, 90)).translate((-w / 2, 0, 0))

    # 主体卡槽: 按仰角切一个 body 截面的槽
    slot_sec = rrect(BODY_W + 2 * LIP_CLEAR, BODY_D + 2 * LIP_CLEAR, CORNER)
    slot = slot_sec.extrude(BODY_H)  # 沿 Z 是 body 高度方向
    # 让 body 背面靠在楔形斜面上: 绕 X 轴前倾 TILT
    slot = slot.rotate((-TILT, 0, 0)).translate((0, STAND_D - 13, 16 - STAND_LIP))
    stand = wedge - slot

    # 两只小脚趾 (前缘半圆柱, 与底面相切)
    for sx in (-w / 2 + 14, w / 2 - 14):
        toe = Manifold.cylinder(10, 5).rotate((0, 90, 0)).translate((sx - 5, 4, 5.0))
        stand = stand + toe
    return stand


def export(m: Manifold, path: str):
    mesh = m.to_mesh()
    tm = trimesh.Trimesh(vertices=np.asarray(mesh.vert_properties)[:, :3],
                         faces=np.asarray(mesh.tri_verts))
    tm.fix_normals()
    assert tm.is_watertight, f"{path} not watertight!"
    tm.export(path)
    print(f"{path}: watertight={tm.is_watertight} vol={tm.volume/1000:.1f}cm3 "
          f"bbox={np.round(tm.extents, 1)}")


if __name__ == "__main__":
    export(make_body(), "body.stl")
    export(make_back(), "back.stl")
    export(make_antenna(), "antenna.stl")
    export(make_stand(), "stand.stl")
