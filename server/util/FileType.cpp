// =============================================================================
// FileType.cpp —— 文件类型判定的实现 (单一真源)
// =============================================================================
#include "FileType.h"

#include <map>
#include <set>
#include <cctype>

std::string ext_of(const std::string& filename)
{
    std::string ext;
    auto dot = filename.rfind('.');
    if (dot != std::string::npos) {
        ext = filename.substr(dot + 1);
        for (auto& ch : ext) ch = (char)std::tolower((unsigned char)ch);
    }
    return ext;
}

std::string mime_of(const std::string& filename)
{
    static const std::map<std::string, std::string> M = {
        {"png","image/png"}, {"jpg","image/jpeg"}, {"jpeg","image/jpeg"}, {"gif","image/gif"},
        {"webp","image/webp"}, {"bmp","image/bmp"}, {"svg","image/svg+xml"}, {"ico","image/x-icon"},
        {"mp4","video/mp4"}, {"webm","video/webm"}, {"mov","video/quicktime"}, {"mkv","video/x-matroska"}, {"ogv","video/ogg"},
        {"mp3","audio/mpeg"}, {"wav","audio/wav"}, {"flac","audio/flac"}, {"aac","audio/aac"},
        {"ogg","audio/ogg"}, {"m4a","audio/mp4"},
        {"pdf","application/pdf"},
        {"txt","text/plain; charset=utf-8"}, {"md","text/markdown; charset=utf-8"},
        {"json","application/json; charset=utf-8"}, {"xml","application/xml; charset=utf-8"},
        {"csv","text/csv; charset=utf-8"}, {"log","text/plain; charset=utf-8"},
        {"js","text/javascript; charset=utf-8"}, {"ts","text/plain; charset=utf-8"},
        {"css","text/css; charset=utf-8"}, {"html","text/html; charset=utf-8"},
        {"c","text/plain; charset=utf-8"}, {"cpp","text/plain; charset=utf-8"}, {"h","text/plain; charset=utf-8"},
        {"py","text/plain; charset=utf-8"}, {"java","text/plain; charset=utf-8"}, {"go","text/plain; charset=utf-8"},
        {"sh","text/plain; charset=utf-8"}, {"rs","text/plain; charset=utf-8"}, {"yml","text/plain; charset=utf-8"},
        {"yaml","text/plain; charset=utf-8"}, {"sql","text/plain; charset=utf-8"},
    };
    auto it = M.find(ext_of(filename));
    return it == M.end() ? "application/octet-stream" : it->second;
}

bool is_thumbnailable(const std::string& filename)
{
    static const std::set<std::string> S = {"png","jpg","jpeg","gif","bmp","tga","psd","ppm","pgm"};
    return S.count(ext_of(filename)) > 0;
}
