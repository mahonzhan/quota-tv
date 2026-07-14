// ============================================================
// QuotaTV 外壳 — Claude bot 橙色机器人造型 (参数化, 与 gen_case.py 等价)
// 部件: body / back / antenna / stand    橙色 PLA (#D97757)
// 注意: 发货 STL 由 gen_case.py 生成; 本文件供 OpenSCAD 用户改参
// ============================================================

part = "all"; // [all, body, back, antenna, stand]

/* ---- 屏幕 1.8" ST7735 ---- */
scr_view_w = 36.5;  scr_view_h = 29.5;   // 可视区开窗
scr_pcb_w  = 58;    scr_pcb_h  = 35;     // 模块 PCB, 按实物量!
scr_y_off  = 4;

/* ---- 主体 ---- */
wall = 2.4;
body_w = 74; body_h = 64; body_d = 30;
corner = 8;                              // 大圆角机器人头

/* ---- 后盖 ---- */
lip_d = 5; lip_clear = 0.28;
usb_w = 13; usb_h = 8; usb_z = 10;

/* ---- 天线 ---- */
ant_hole_d = 6; ant_stem_d = 5.6;
ant_stem_h = 16; ant_ball_d = 11;

/* ---- 底座 ---- */
tilt = 12; stand_d = 46; stand_lip = 7;

$fn = 64;

module rbox(w, h, d, r) {
  translate([w/2, h/2, 0]) linear_extrude(d) offset(r = r) square([w - 2*r, h - 2*r], center = true);
}

module body() {
  difference() {
    union() {
      difference() {
        rbox(body_w, body_h, body_d, corner);
        translate([wall, wall, wall]) rbox(body_w - 2*wall, body_h - 2*wall, body_d, corner - 1.5);
      }
      // 天线凸台
      translate([body_w/2, body_h - wall + 0.01, body_d/2])
        rotate([90, 0, 0]) cylinder(h = 12, d = 10);
      // 屏幕定位框
      translate([body_w/2, body_h/2 + scr_y_off, wall]) difference() {
        translate([-scr_pcb_w/2 - 1.8, -scr_pcb_h/2 - 1.8, 0]) cube([scr_pcb_w + 3.6, scr_pcb_h + 3.6, 3]);
        translate([-scr_pcb_w/2 - 0.3, -scr_pcb_h/2 - 0.3, -0.5]) cube([scr_pcb_w + 0.6, scr_pcb_h + 0.6, 4]);
      }
    }
    // 屏幕开窗 + 外侧倒角
    translate([body_w/2, body_h/2 + scr_y_off, 0]) {
      translate([-scr_view_w/2, -scr_view_h/2, -0.5]) cube([scr_view_w, scr_view_h, wall + 1]);
      hull() {
        translate([-scr_view_w/2 - 1.6, -scr_view_h/2 - 1.6, -0.5]) cube([scr_view_w + 3.2, scr_view_h + 3.2, 0.5]);
        translate([-scr_view_w/2, -scr_view_h/2, wall - 0.8]) cube([scr_view_w, scr_view_h, 0.1]);
      }
    }
    // 天线通孔
    translate([body_w/2, body_h + 0.01, body_d/2]) rotate([90, 0, 0]) cylinder(h = wall + 12.5, d = ant_hole_d);
  }
}

module back() {
  difference() {
    union() {
      rbox(body_w, body_h, wall, corner);
      translate([wall + lip_clear, wall + lip_clear, wall]) difference() {
        rbox(body_w - 2*wall - 2*lip_clear, body_h - 2*wall - 2*lip_clear, lip_d, corner - 1.5);
        translate([1.6, 1.6, -0.5])
          rbox(body_w - 2*wall - 2*lip_clear - 3.2, body_h - 2*wall - 2*lip_clear - 3.2, lip_d + 1, corner - 3);
      }
    }
    translate([body_w/2 - usb_w/2, usb_z - usb_h/2, -0.5]) cube([usb_w, usb_h, wall + lip_d + 1]);
    for (dx = [-5, 0, 5], dy = [-5, 0, 5])
      translate([body_w - 16 + dx, body_h - 16 + dy, -0.5]) cylinder(h = wall + 2, d = 2.4);
    for (i = [0:3]) translate([14, body_h/2 - 8 + i*7, -0.5]) rbox(22, 2.4, wall + 2, 1.2);
  }
}

module antenna() {
  cylinder(h = 8 + ant_stem_h, d = ant_stem_d);
  translate([0, 0, 8 + ant_stem_h]) sphere(d = ant_ball_d);
}

module stand() {
  w = body_w + 8;
  difference() {
    translate([w, 0, 0]) rotate([0, -90, 0]) linear_extrude(w)
      polygon([[0,0],[3,0],[3,6],[16,stand_d-14],[16,stand_d],[0,stand_d]]); // (z,y) 轮廓
    // 主体卡槽
    translate([w/2 - 4, stand_d - 13, 16 - stand_lip]) rotate([-tilt, 0, 0])
      translate([-(body_w/2 + lip_clear) + 4, -(body_d/2 + lip_clear), 0])
        rbox(body_w + 2*lip_clear, body_d + 2*lip_clear, body_h, corner);
  }
  for (sx = [14, w - 14]) translate([sx - 5, 4, 5]) rotate([0, 90, 0]) cylinder(h = 10, r = 5);
}

if (part == "body"    || part == "all") body();
if (part == "back"    || part == "all") translate([body_w + 12, 0, 0]) back();
if (part == "antenna" || part == "all") translate([-16, 20, 0]) antenna();
if (part == "stand"   || part == "all") translate([0, body_h + 14, 0]) stand();
