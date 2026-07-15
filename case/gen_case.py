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
# 屏幕 1.8" ST7735 蓝板 (实测: PCB 56x34, 白框 44x34, 显示区 38x32)
SCR_VIEW_W, SCR_VIEW_H = 38.5, 32.5     # 开窗 = 显示区 + 0.5 余量
SCR_PCB_W, SCR_PCB_H = 56.0, 34.0       # 模块 PCB (实测)
# 显示区中心相对 PCB 中心的偏移 (实测):
# 白框 44 靠玻璃侧, 玻璃侧留边 5.2 / 排针侧留边 0.8
# => 显示区中心在白框中心 +2.2 (偏排针侧), 白框中心在 PCB 中心 -6
# => 相对 PCB 中心 = -6 + 2.2 = -3.8
SCR_ACT_OFF_X = -3.8
SCR_Y_OFF = 4.0                          # 屏窗上移
SCR_PIN_SIDE_OPEN = True                 # 定位框排针侧开口 (已焊排针+飞线)

WALL = 2.4
BODY_W, BODY_H, BODY_D = 74.0, 64.0, 30.0
CORNER = 8.0                             # 大圆角, 更像机器人头

LIP_D, LIP_CLEAR = 5.0, 0.28

# 合宙 Core-ESP32C3 V1639 LuatOS (飞线直焊, 元件面朝后盖贴内侧, USB-C 朝侧面缺口)
ESP_L, ESP_W = 51.0, 21.0     # 板长x宽 (实测)
BTN_FROM_USB = 17.5            # USB 端边缘 -> 按钮中心 (实测: 边缘 15/20 取中)
BTN_GAP = 11.8                 # BOOT/RST 两按钮中心距 (实测)
BTN_HOLE_D = 5.2               # 后盖按钮孔径
USB_NOTCH_W, USB_NOTCH_H = 11.0, 8.0   # 侧面 USB-C 出线缺口 (宽x高)

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
    # 开窗以显示区为准居中于前脸, PCB 中心相应偏移 -SCR_ACT_OFF_X
    pcb_cx = -SCR_ACT_OFF_X
    fo = Manifold.cube((SCR_PCB_W + 3.6, SCR_PCB_H + 3.6, 3.0), True)
    fi = Manifold.cube((SCR_PCB_W + 0.6, SCR_PCB_H + 0.6, 4.0), True)
    frame = (fo - fi).translate((pcb_cx, SCR_Y_OFF, WALL + 1.5))
    if SCR_PIN_SIDE_OPEN:
        # 排针侧 (+x) 开口: 已焊排针和飞线从这里出
        cut = Manifold.cube((9, SCR_PCB_H + 9, 7), True).translate(
            (pcb_cx + SCR_PCB_W / 2 + 1.5, SCR_Y_OFF, WALL + 1.5))
        frame = frame - cut
    body = body + frame

    # 天线安装: 顶壁内侧加持握凸台 (d10 x 12), 再打通孔 (d6)
    boss = Manifold.cylinder(12, 5.0).rotate((90, 0, 0)).translate(
        (0, BODY_H / 2 - WALL + 0.01, BODY_D / 2))
    hole = Manifold.cylinder(WALL + 12.5, ANT_HOLE_D / 2).rotate((90, 0, 0)).translate(
        (0, BODY_H / 2 + 0.01, BODY_D / 2))
    body = body + boss - hole

    # 右侧壁后缘 USB-C 出线缺口 (开到后端面, 后盖装上即封闭成方孔)
    notch = Manifold.cube((WALL + 2, USB_NOTCH_H, USB_NOTCH_W + 1), True).translate(
        (BODY_W / 2 - WALL / 2, 0, BODY_D - USB_NOTCH_W / 2 + 0.5))
    body = body - notch
    return body


def make_back():
    panel = rbox(BODY_W, BODY_H, WALL, CORNER)
    lip_o = rbox(BODY_W - 2 * WALL - 2 * LIP_CLEAR,
                 BODY_H - 2 * WALL - 2 * LIP_CLEAR, LIP_D, CORNER - 1.5)
    lip_i = rbox(BODY_W - 2 * WALL - 2 * LIP_CLEAR - 3.2,
                 BODY_H - 2 * WALL - 2 * LIP_CLEAR - 3.2, LIP_D + 1, CORNER - 3)
    lip = (lip_o - lip_i.translate((0, 0, -0.5))).translate((0, 0, WALL))
    back = panel + lip

    # 合宙 C3 板贴后盖内侧 (元件面朝后盖), USB-C 朝 -x 缺口方向
    # 板 USB 端边缘位置:
    usb_edge_x = -(BODY_W / 2 - WALL - 1)
    # BOOT/RST 按钮孔 (一对, 沿 y 对称)
    for dy in (-BTN_GAP / 2, BTN_GAP / 2):
        h = Manifold.cylinder(WALL + 2, BTN_HOLE_D / 2).translate(
            (usb_edge_x + BTN_FROM_USB, dy, -1))
        back = back - h
    # 后盖唇 -x 侧对应 USB 缺口 (与主壳侧缺口拼合)
    lipn = Manifold.cube((2 * WALL + 3, USB_NOTCH_H, LIP_D + 2), True).translate(
        (-(BODY_W / 2 - WALL), 0, WALL + LIP_D / 2))
    back = back - lipn

    # 蜂鸣器出音孔 3x3 (右上, 避开板区)
    for dx in (-5, 0, 5):
        for dy in (-5, 0, 5):
            h = Manifold.cylinder(WALL + 2, 1.2).translate(
                (BODY_W / 2 - 16 + dx, BODY_H / 2 - 16 + dy, -1))
            back = back - h

    # 散热缝 (下方, 避开板区)
    for i in range(3):
        slot = rbox(22, 2.4, WALL + 2, 1.2).translate((-BODY_W / 2 + 25, -25 + i * 5.5, -1))
        back = back - slot
    return back


def make_plunger():
    """按钮顶杆 x2: 蘑菇头朝内顶住板载按钮, 杆穿出后盖孔"""
    head = Manifold.cylinder(1.4, 3.6)
    stem = Manifold.cylinder(4.6, (BTN_HOLE_D - 0.6) / 2).translate((0, 0, 1.4))
    return head + stem


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
    export(make_plunger(), "button_plunger_x2.stl")
