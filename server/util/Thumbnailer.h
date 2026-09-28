#pragma once
#include <string>

namespace thumb {
// 从任意图片文件(按内容嗅探格式, 与扩展名无关)生成 JPEG 缩略图。
// maxDim: 长边最大像素。成功写入 outPath 返回 true。
bool make_thumbnail(const std::string& src, const std::string& outPath, int maxDim);
}
