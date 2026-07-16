package main

// 运行时把额度数字渲染进托盘图标 (Windows/Linux 用;
// Windows 托盘不支持文字标题, Linux 各桌面对 SNI 标题支持不一)
// 上行橙色 = Claude 5h 已用%, 下行白色 = Codex 5h 已用%

import (
	"bytes"
	"encoding/binary"
	"image"
	"image/color"
	"image/png"
	"strconv"
)

const iconSize = 32

// 3x5 像素数字字体, 每行 3 bit
var pixFont = map[byte][5]uint8{
	'0': {0b111, 0b101, 0b101, 0b101, 0b111},
	'1': {0b010, 0b110, 0b010, 0b010, 0b111},
	'2': {0b111, 0b001, 0b111, 0b100, 0b111},
	'3': {0b111, 0b001, 0b111, 0b001, 0b111},
	'4': {0b101, 0b101, 0b111, 0b001, 0b001},
	'5': {0b111, 0b100, 0b111, 0b001, 0b111},
	'6': {0b111, 0b100, 0b111, 0b101, 0b111},
	'7': {0b111, 0b001, 0b010, 0b010, 0b010},
	'8': {0b111, 0b101, 0b111, 0b101, 0b111},
	'9': {0b111, 0b101, 0b111, 0b001, 0b111},
	'-': {0b000, 0b000, 0b111, 0b000, 0b000},
}

func drawText(img *image.RGBA, s string, y int, c color.RGBA) {
	const scale = 2
	w := len(s)*(3*scale+scale) - scale
	x := (iconSize - w) / 2
	if x < 0 {
		x = 0
	}
	for i := 0; i < len(s); i++ {
		glyph, ok := pixFont[s[i]]
		if !ok {
			glyph = pixFont['-']
		}
		for row := 0; row < 5; row++ {
			for col := 0; col < 3; col++ {
				if glyph[row]&(1<<(2-col)) != 0 {
					for dy := 0; dy < scale; dy++ {
						for dx := 0; dx < scale; dx++ {
							img.SetRGBA(x+col*scale+dx, y+row*scale+dy, c)
						}
					}
				}
			}
		}
		x += 3*scale + scale
	}
}

// 渲染双行数字图标, 返回 PNG 字节
func renderNumberPNG(top, bottom string) []byte {
	img := image.NewRGBA(image.Rect(0, 0, iconSize, iconSize))
	drawText(img, top, 4, color.RGBA{217, 119, 87, 255})    // Claude 橙
	drawText(img, bottom, 18, color.RGBA{255, 255, 255, 255}) // Codex 白
	var buf bytes.Buffer
	png.Encode(&buf, img)
	return buf.Bytes()
}

// PNG 包一层 ICO 容器 (Vista+ 原生支持 PNG-in-ICO), Windows SetIcon 用
func pngToICO(p []byte) []byte {
	var buf bytes.Buffer
	binary.Write(&buf, binary.LittleEndian, uint16(0)) // reserved
	binary.Write(&buf, binary.LittleEndian, uint16(1)) // type: icon
	binary.Write(&buf, binary.LittleEndian, uint16(1)) // count
	buf.WriteByte(iconSize)                            // width
	buf.WriteByte(iconSize)                            // height
	buf.WriteByte(0)                                   // palette
	buf.WriteByte(0)                                   // reserved
	binary.Write(&buf, binary.LittleEndian, uint16(1))      // planes
	binary.Write(&buf, binary.LittleEndian, uint16(32))     // bpp
	binary.Write(&buf, binary.LittleEndian, uint32(len(p))) // data size
	binary.Write(&buf, binary.LittleEndian, uint32(22))     // data offset
	buf.Write(p)
	return buf.Bytes()
}

// 百分比 → 短字符串 ("62" / "-")
func pctStr(f *float64) string {
	if f == nil {
		return "-"
	}
	return strconv.Itoa(int(*f + 0.5))
}
