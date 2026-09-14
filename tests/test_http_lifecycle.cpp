#include <catch2/catch_all.hpp>

#include "config/config.h"
#include "net/http_server.h"

int main(int argc, char* argv[]) {
    return Catch::Session().run(argc, argv);
}

#include <string>

// HTTP 生命周期测试不加载真实注册表 token；服务鉴权逻辑本身由请求测试覆盖，
// 本目标只验证 Start/Stop 在线程尚未进入 listen 时也能安全回收。
namespace moekoe {
bool Config::IsUsingFallbackToken() { return false; }
std::string Config::GetAuthToken() { return "test-token"; }
}

TEST_CASE("HttpServer can stop immediately after start", "[http][lifecycle]") {
    moekoe::HttpServer server;
    REQUIRE(server.Start(65230));
    server.Stop();
    REQUIRE_FALSE(server.IsRunning());
}

TEST_CASE("HttpServer start-stop can be repeated", "[http][lifecycle]") {
    moekoe::HttpServer server;
    for (int i = 0; i < 3; ++i) {
        REQUIRE(server.Start(65230));
        server.Stop();
        REQUIRE_FALSE(server.IsRunning());
    }
}

TEST_CASE("HttpServer repeated Start is idempotent while active", "[http][lifecycle]") {
    moekoe::HttpServer server;
    REQUIRE(server.Start(65230));
    REQUIRE(server.Start(65230));
    server.Stop();
    REQUIRE_FALSE(server.IsRunning());
}
