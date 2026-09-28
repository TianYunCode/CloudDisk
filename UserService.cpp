#include <iostream>
#include <signal.h>

#include "UserService.srpc.h"
#include "workflow/WFFacilities.h"
#include "workflow/WFTaskFactory.h"
#include "workflow/MySQLResult.h"
#include "workflow/MySQLUtil.h"
#include "ppconsul/agent.h"

#include "CryptoUtil.h"
#include "User.h"
#include "Config.h"
#include "SqlUtil.h"

using namespace std;
using namespace std::placeholders;
using namespace ppconsul::agent;
using namespace srpc;

using ppconsul::Consul;

static std::string MYSQL_URL;
static const int RETRY_MAX = 3;

static WFFacilities::WaitGroup g_waitGroup{ 1 };

void sighandler(int signo)
{
	g_waitGroup.done();
}

class UserServiceImpl : public UserService::Service
{
public:
	void sign_up(UserRequest *req, UserResponse *resp, srpc::RPCContext *ctx) override
	{
		// 1. 参数校验应该是API网关要做的事情
        const string& username = req->username();
        const string& password = req->password();
        // 2. 注册
        string salt = CryptoUtil::generate_salt();
        string hashcode = CryptoUtil::hash_password(password, salt);
        string sql = "INSERT INTO tbl_user (username, password, salt) VALUES ("
            + SqlUtil::quote(username) + ", "
            + SqlUtil::quote(hashcode) + ", "
            + SqlUtil::quote(salt) + ")";
        cout << "[SQL] " << sql << endl;   /* 日志 */

        WFMySQLTask* mysqlTask = WFTaskFactory::create_mysql_task(MYSQL_URL, RETRY_MAX, [resp](WFMySQLTask* task)
        {
            // 任务失败或者SQL语句执行失败
            if (task->get_state() != WFT_STATE_SUCCESS || 
                task->get_resp()->get_packet_type() == MYSQL_PACKET_ERROR) {
                resp->set_success(false); 
            } else {
                resp->set_success(true);
            }
        });
        mysqlTask->get_req()->set_query(sql); 

        SeriesWork* series = ctx->get_series();
        series->push_back(mysqlTask);
	}

	void sign_in(UserRequest *req, UserResponse *resp, srpc::RPCContext *ctx) override
	{
		// 1. 解析请求参数
        const string& username = req->username();
        const string& password = req->password();
        // 2. 构建SQL语句 (用户名转义, 防注入)
        string sql = "SELECT * FROM tbl_user WHERE username="
            + SqlUtil::quote(username) + " AND tomb=0";
        cout << "[SQL] " << sql << endl;   /* 日志 */

        WFMySQLTask* mysqlTask = WFTaskFactory::create_mysql_task(
            MYSQL_URL, 
            RETRY_MAX, 
            std::bind(signin_callback, resp, password, _1)
        );
        mysqlTask->get_req()->set_query(sql);

        SeriesWork* series = ctx->get_series();
        series->push_back(mysqlTask);
	}
private:
	static void signin_callback(UserResponse* resp, const std::string& password, WFMySQLTask* task)
    {
        using namespace protocol;
        if (task->get_state() != WFT_STATE_SUCCESS ||
            task->get_resp()->get_packet_type() == MYSQL_PACKET_ERROR) {
            resp->set_success(false);
            return ;
        }
        // MySQL 任务执行成功
        MySQLResultCursor cursor { task->get_resp() };
        std::vector<MySQLCell> record;
        
        if (!cursor.fetch_row(record)) {
            resp->set_success(false);
            return ;
        }

        User user;
        user.id = record[0].as_int();
        user.username = record[1].as_string();
        user.hashcode = record[2].as_string();
        user.salt = record[3].as_string();
        user.createdAt = record[4].as_datetime();
#ifdef DEBUG
        cout << "[INFO] id: " << user.id
            << ", username: " << user.username
            << ", hashcode: " << user.hashcode
            << ", salt: " << user.salt
            << ", createdAt: " << user.createdAt << endl;
#endif
        string hashcode1 = CryptoUtil::hash_password(password, user.salt);
#ifdef DEBUG
        std::cout << "generated hashcode: " << hashcode1 << endl;
#endif
        if (hashcode1 == user.hashcode) {
            resp->set_success(true); 
            resp->set_id(user.id);
            resp->set_username(user.username);
            resp->set_createdat(user.createdAt);
            resp->set_token(CryptoUtil::generate_token(user));
            return ;
        }
        // 密码错误
        resp->set_success(false);
    }
};

static void timer_callback(WFTimerTask* task)
{
    if (task->get_state() != WFT_STATE_SUCCESS) {
        return ;
    }
    SeriesWork* series = series_of(task);
    Agent* agent = (Agent*)series->get_context();
    agent->servicePass("UserService1");     // 发送心跳检测包
    
    WFTimerTask* nextTask = WFTaskFactory::create_timer_task(
        "health_check",
        9, 
        0, 
        timer_callback
    );
    series->push_back(nextTask);
}

int main()
{
	GOOGLE_PROTOBUF_VERIFY_VERSION;

	// 加载配置
	::Config& cfg = ::Config::instance();
	MYSQL_URL = cfg.mysql_url();
	int port = cfg.srpc_port();
	string consulUrl = cfg.consul_url();

	// 0. 注册信号处理函数
	signal(SIGINT, sighandler);
	// 1. 创建服务器
	SRPCServer server;
	// 2. 添加服务(类似HttpServer注册路由)
	UserServiceImpl service;
	server.add_service(&service);
	// 3. 启动服务器
	if (server.start(port) == 0) {
        cout << "[INFO] UserService 已启动, SRPC 端口 " << port << endl;
        // 4. 向注册中心 consul 注册服务
        Consul consul { consulUrl, ppconsul::kw::dc = "dc1" };
        Agent agent{ consul };
        // 注册服务 (注册端口必须与实际监听端口一致)
        agent.registerService(
            kw::id = "UserService1",
            kw::name = "UserService",
            kw::address = "127.0.0.1",
            kw::port = (uint16_t)port,
            kw::check = TtlCheck(std::chrono::seconds{ 10 })
        );

        agent.servicePass("UserService1");  // 发送心跳检测包
        // 之后每9秒发送一个心跳检测包
        WFTimerTask* timerTask = WFTaskFactory::create_timer_task(
            "health_check",
            9, 
            0, 
            timer_callback
        );
        SeriesWork* series = Workflow::create_series_work(timerTask, nullptr);
        series->set_context(&agent);        // 设置序列的上下文 
        series->start();                    // 启动序列

		g_waitGroup.wait();                 // 用户按下了Ctrl+C
        WFTaskFactory::cancel_by_name("health_check");  // 取消定时任务，不然会出现Segmentation Fault

		server.stop();
	} else {
		cerr << "Error: start SRPCServer failed!" << endl;
		exit(1);
	}

	google::protobuf::ShutdownProtobufLibrary();
	return 0;
}
