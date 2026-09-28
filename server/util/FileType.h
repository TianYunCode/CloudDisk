// =============================================================================
// FileType.h —— 按扩展名的文件类型判定 (纯查表逻辑, 无外部运行时依赖)
//
// 从网关支撑层抽出的纯函数模块: MIME 推断与缩略图可生成性判定。
// 高内聚、可复用、可独立单元测试 (见 tests/unit)。
// =============================================================================
#ifndef CLOUDDISK_FILE_TYPE_H
#define CLOUDDISK_FILE_TYPE_H

#include <string>

// 取小写扩展名 (不含 '.'; 无扩展名返回空串)
std::string ext_of(const std::string& filename);

// 按扩展名推断 MIME 类型 (未知返回 application/octet-stream)
std::string mime_of(const std::string& filename);

// 是否为可生成缩略图的位图格式 (stb 支持)
bool is_thumbnailable(const std::string& filename);

#endif // CLOUDDISK_FILE_TYPE_H
