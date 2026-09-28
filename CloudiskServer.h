#pragma once

#include <wfrest/HttpServer.h>
#include <workflow/WFFacilities.h>

// 装饰者模式 (套壳)
// CloudiskServer 的用法和 HttpServer 的用法非常类似
// 接口一致，可以降低用户的学习成本
class CloudiskServer
{
public:
    CloudiskServer() {}

    void register_modules();

    int start(unsigned short port) { return m_server.start(port); }

    void stop() { m_server.stop(); }

    void list_routes() { m_server.list_routes(); }

    CloudiskServer& track()
    {
        m_server.track();
        return *this;
    }
private:
    void register_static_resources_module();
    void register_signup_module();
    void register_signin_module();
    void register_userinfo_module();
    void register_fileupload_module();
    void register_filelist_module();
    void register_filedownload_module();
    void register_folder_module();
    void register_trash_module();
    void register_share_module();
    void register_preview_module();
    void register_chunk_upload_module();
    void register_offline_module();
    void register_account_module();
    void register_admin_module();
    void register_metrics_module();
    void register_token_module();
    void register_webdav_module();
    void register_favorite_module();
private:
    // 名字中最好不要带具体的实现细节
    // 方便以后修改具体的实现
    wfrest::HttpServer m_server {};    
};

