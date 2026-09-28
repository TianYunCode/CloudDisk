#include <iostream>
#include <signal.h>

#include "CloudiskServer.h"
#include "Config.h"
#include "Log.h"

WFFacilities::WaitGroup g_waitGroup { 1 };

void sig_handler(int)
{
    g_waitGroup.done();
}

int main()
{
    signal(SIGINT, sig_handler);

    Config& cfg = Config::instance();
    Log::instance().init(cfg.log_file());

    int port = cfg.http_port();

    CloudiskServer svr;
    svr.register_modules();

    if (svr.track().start(port) == 0) {
        svr.list_routes();
        LOG_INFO("CloudDisk HTTP 网关已启动, 监听端口 " << port);
        g_waitGroup.wait();
        svr.stop();
        LOG_INFO("CloudDisk HTTP 网关已停止");
    } else {
        LOG_ERROR("服务器启动失败, 端口 " << port);
        std::cerr << "Error: server start failed!" << std::endl;
        return 1;
    }
    return 0;
}
