// 缩略图生成 (基于 stb 单头库, 无外部依赖)。独立 TU 隔离 stb 实现。
#include <cstdlib>
#include <algorithm>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_FAILURE_USERMSG
#include "third_party/stb/stb_image.h"

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "third_party/stb/stb_image_resize2.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "third_party/stb/stb_image_write.h"

#include "Thumbnailer.h"

namespace thumb {

bool make_thumbnail(const std::string& src, const std::string& outPath, int maxDim)
{
    if (maxDim < 8) maxDim = 8;
    int w = 0, h = 0, comp = 0;
    // 强制解码为 RGB(3 通道), 与扩展名无关(按文件魔数嗅探)
    unsigned char* data = stbi_load(src.c_str(), &w, &h, &comp, 3);
    if (!data || w <= 0 || h <= 0) { if (data) stbi_image_free(data); return false; }

    int nw, nh;
    if (w <= maxDim && h <= maxDim) { nw = w; nh = h; }
    else if (w >= h) { nw = maxDim; nh = std::max(1, (int)((long long)h * maxDim / w)); }
    else { nh = maxDim; nw = std::max(1, (int)((long long)w * maxDim / h)); }

    unsigned char* out = (unsigned char*)malloc((size_t)nw * nh * 3);
    if (!out) { stbi_image_free(data); return false; }

    unsigned char* r = stbir_resize_uint8_srgb(
        data, w, h, 0, out, nw, nh, 0, STBIR_RGB);
    stbi_image_free(data);
    if (!r) { free(out); return false; }

    int ok = stbi_write_jpg(outPath.c_str(), nw, nh, 3, out, 82);
    free(out);
    return ok != 0;
}

} // namespace thumb
