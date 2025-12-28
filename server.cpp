// server.cpp - 在线人数统计服务器
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <iostream>
#include <mutex>
#include <unordered_set>
#include <chrono>
#include <thread>
#include <atomic>
#include <memory>
#include <random>
#include <vector>

using json = nlohmann::json;

class OnlineManager {
private:
    // 将 mutex 声明为 mutable，这样可以在 const 成员函数中锁定
    mutable std::mutex mtx_;
    std::unordered_set<std::string> online_users_;  // 在线用户ID集合
    
    // 清理过期连接
    struct SessionInfo {
        std::string session_id;
        std::string user_id;
        std::chrono::steady_clock::time_point last_active;
    };
    std::unordered_map<std::string, SessionInfo> sessions_;
    
    std::atomic<int> total_online_{0};  // 总在线人数
    bool running_{true};
    std::thread cleanup_thread_;
    
    // 随机数生成器
    std::random_device rd_;
    std::mt19937 gen_;
    std::uniform_int_distribution<> dis_;
    
public:
    OnlineManager() : gen_(rd_()), dis_(1000, 9999) {
        // 启动清理线程
        cleanup_thread_ = std::thread([this]() {
            while (running_) {
                cleanupExpiredSessions();
                std::this_thread::sleep_for(std::chrono::seconds(30));
            }
        });
    }
    
    ~OnlineManager() {
        running_ = false;
        if (cleanup_thread_.joinable()) {
            cleanup_thread_.join();
        }
    }
    
    // 用户上线
    std::string userLogin(const std::string& user_id) {
        std::lock_guard<std::mutex> lock(mtx_);
        
        // 生成唯一会话ID
        std::string session_id = generateSessionId();
        
        online_users_.insert(user_id);
        sessions_[session_id] = {
            session_id,
            user_id,
            std::chrono::steady_clock::now()
        };
        
        total_online_ = static_cast<int>(online_users_.size());
        
        return session_id;
    }
    
    // 用户心跳（保持在线状态）
    bool userHeartbeat(const std::string& session_id) {
        std::lock_guard<std::mutex> lock(mtx_);
        
        auto it = sessions_.find(session_id);
        if (it != sessions_.end()) {
            it->second.last_active = std::chrono::steady_clock::now();
            return true;
        }
        return false;
    }
    
    // 用户下线
    void userLogout(const std::string& session_id) {
        std::lock_guard<std::mutex> lock(mtx_);
        
        auto it = sessions_.find(session_id);
        if (it != sessions_.end()) {
            online_users_.erase(it->second.user_id);
            sessions_.erase(it);
            total_online_ = static_cast<int>(online_users_.size());
        }
    }
    
    // 获取在线人数
    int getOnlineCount() const {
        return total_online_.load();
    }
    
    // 获取在线用户列表
    std::vector<std::string> getOnlineUsers() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return std::vector<std::string>(online_users_.begin(), online_users_.end());
    }
    
    // 检查会话是否有效
    bool isValidSession(const std::string& session_id) const {
        std::lock_guard<std::mutex> lock(mtx_);
        return sessions_.find(session_id) != sessions_.end();
    }
    
private:
    std::string generateSessionId() {
        auto now = std::chrono::system_clock::now();
        auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count();
        
        // 使用随机数生成器
        int random_num = dis_(gen_);
        return "sess_" + std::to_string(timestamp) + "_" + std::to_string(random_num);
    }
    
    void cleanupExpiredSessions() {
        std::lock_guard<std::mutex> lock(mtx_);
        auto now = std::chrono::steady_clock::now();
        
        for (auto it = sessions_.begin(); it != sessions_.end(); ) {
            auto duration = std::chrono::duration_cast<std::chrono::seconds>(
                now - it->second.last_active);
            
            // 60秒无心跳视为过期
            if (duration.count() > 60) {
                online_users_.erase(it->second.user_id);
                it = sessions_.erase(it);
            } else {
                ++it;
            }
        }
        
        total_online_ = static_cast<int>(online_users_.size());
    }
};

int main() {
    OnlineManager online_manager;
    
    httplib::Server server;
    
    // 设置CORS头（如果前端是Web应用）
    server.set_default_headers({
        {"Access-Control-Allow-Origin", "*"},
        {"Access-Control-Allow-Methods", "GET, POST, OPTIONS"},
        {"Access-Control-Allow-Headers", "Content-Type"}
    });
    
    // 1. 获取在线人数
    server.Get("/api/online/count", [&](const httplib::Request& req, httplib::Response& res) {
        json response = {
            {"code", 0},
            {"message", "success"},
            {"data", {
                {"online_count", online_manager.getOnlineCount()},
                {"timestamp", std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count()}
            }}
        };
        
        res.set_content(response.dump(), "application/json");
    });
    
    // 2. 用户登录（上线）
    server.Post("/api/online/login", [&](const httplib::Request& req, httplib::Response& res) {
        try {
            auto body = json::parse(req.body);
            std::string user_id = body.value("user_id", "");
            
            if (user_id.empty()) {
                json response = {{"code", -1}, {"message", "user_id is required"}};
                res.set_content(response.dump(), "application/json");
                return;
            }
            
            std::string session_id = online_manager.userLogin(user_id);
            
            json response = {
                {"code", 0},
                {"message", "login success"},
                {"data", {
                    {"session_id", session_id},
                    {"online_count", online_manager.getOnlineCount()}
                }}
            };
            
            res.set_content(response.dump(), "application/json");
        } catch (const std::exception& e) {
            json response = {
                {"code", -1},
                {"message", std::string("parse error: ") + e.what()}
            };
            res.set_content(response.dump(), "application/json");
        }
    });
    
    // 3. 心跳接口
    server.Post("/api/online/heartbeat", [&](const httplib::Request& req, httplib::Response& res) {
        try {
            auto body = json::parse(req.body);
            std::string session_id = body.value("session_id", "");
            
            if (session_id.empty()) {
                json response = {{"code", -1}, {"message", "session_id is required"}};
                res.set_content(response.dump(), "application/json");
                return;
            }
            
            bool success = online_manager.userHeartbeat(session_id);
            
            json response = {
                {"code", success ? 0 : -1},
                {"message", success ? "heartbeat success" : "invalid session"},
                {"data", {
                    {"online_count", online_manager.getOnlineCount()}
                }}
            };
            
            res.set_content(response.dump(), "application/json");
        } catch (...) {
            json response = {{"code", -1}, {"message", "invalid request"}};
            res.set_content(response.dump(), "application/json");
        }
    });
    
    // 4. 用户退出
    server.Post("/api/online/logout", [&](const httplib::Request& req, httplib::Response& res) {
        try {
            auto body = json::parse(req.body);
            std::string session_id = body.value("session_id", "");
            
            if (session_id.empty()) {
                json response = {{"code", -1}, {"message", "session_id is required"}};
                res.set_content(response.dump(), "application/json");
                return;
            }
            
            online_manager.userLogout(session_id);
            
            json response = {
                {"code", 0},
                {"message", "logout success"}
            };
            
            res.set_content(response.dump(), "application/json");
        } catch (...) {
            json response = {{"code", -1}, {"message", "invalid request"}};
            res.set_content(response.dump(), "application/json");
        }
    });
    
    // 5. 获取在线用户列表
    server.Get("/api/online/users", [&](const httplib::Request& req, httplib::Response& res) {
        auto users = online_manager.getOnlineUsers();
        
        json response = {
            {"code", 0},
            {"message", "success"},
            {"data", {
                {"users", users},
                {"count", users.size()}
            }}
        };
        
        res.set_content(response.dump(), "application/json");
    });
    
    // 6. 检查会话有效性
    server.Post("/api/online/validate", [&](const httplib::Request& req, httplib::Response& res) {
        try {
            auto body = json::parse(req.body);
            std::string session_id = body.value("session_id", "");
            
            if (session_id.empty()) {
                json response = {{"code", -1}, {"message", "session_id is required"}};
                res.set_content(response.dump(), "application/json");
                return;
            }
            
            bool valid = online_manager.isValidSession(session_id);
            
            json response = {
                {"code", 0},
                {"message", "success"},
                {"data", {
                    {"valid", valid}
                }}
            };
            
            res.set_content(response.dump(), "application/json");
        } catch (...) {
            json response = {{"code", -1}, {"message", "invalid request"}};
            res.set_content(response.dump(), "application/json");
        }
    });
    
    // 7. 健康检查
    server.Get("/api/health", [](const httplib::Request& req, httplib::Response& res) {
        json response = {
            {"status", "healthy"},
            {"timestamp", std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count()}
        };
        res.set_content(response.dump(), "application/json");
    });
    
    // 8. 首页
    server.Get("/", [](const httplib::Request& req, httplib::Response& res) {
        std::string html = R"(
<!DOCTYPE html>
<html>
<head>
    <title>在线人数统计服务器</title>
    <meta charset="utf-8">
    <style>
        body { font-family: Arial, sans-serif; margin: 40px; }
        .endpoint { background: #f5f5f5; padding: 10px; margin: 10px 0; border-radius: 5px; }
        .method { font-weight: bold; color: #0076ff; }
        .path { font-family: monospace; }
    </style>
</head>
<body>
    <h1>在线人数统计服务器</h1>
    <p>服务器已启动！以下是可用的API端点：</p>
    
    <div class="endpoint">
        <span class="method">GET</span> <span class="path">/api/online/count</span> - 获取在线人数
    </div>
    <div class="endpoint">
        <span class="method">POST</span> <span class="path">/api/online/login</span> - 用户登录
    </div>
    <div class="endpoint">
        <span class="method">POST</span> <span class="path">/api/online/heartbeat</span> - 心跳
    </div>
    <div class="endpoint">
        <span class="method">POST</span> <span class="path">/api/online/logout</span> - 用户退出
    </div>
    <div class="endpoint">
        <span class="method">GET</span> <span class="path">/api/online/users</span> - 获取在线用户列表
    </div>
    <div class="endpoint">
        <span class="method">GET</span> <span class="path">/api/health</span> - 健康检查
    </div>
    
    <p>当前时间: <span id="time"></span></p>
    <p>当前在线人数: <span id="count">0</span></p>
    
    <script>
        function updateTime() {
            document.getElementById('time').textContent = new Date().toLocaleString();
        }
        
        function fetchOnlineCount() {
            fetch('/api/online/count')
                .then(response => response.json())
                .then(data => {
                    if (data.code === 0) {
                        document.getElementById('count').textContent = data.data.online_count;
                    }
                })
                .catch(console.error);
        }
        
        updateTime();
        fetchOnlineCount();
        setInterval(updateTime, 1000);
        setInterval(fetchOnlineCount, 5000);
    </script>
</body>
</html>
        )";
        res.set_content(html, "text/html");
    });
    
    std::cout << "Starting server on port 8080...\n";
    std::cout << "API endpoints:\n";
    std::cout << "  GET  /api/online/count     - 获取在线人数\n";
    std::cout << "  GET  /api/online/users     - 获取在线用户列表\n";
    std::cout << "  POST /api/online/login     - 用户登录\n";
    std::cout << "  POST /api/online/heartbeat - 心跳\n";
    std::cout << "  POST /api/online/logout    - 用户退出\n";
    std::cout << "  POST /api/online/validate  - 检查会话有效性\n";
    std::cout << "  GET  /api/health           - 健康检查\n";
    std::cout << "  GET  /                      - 首页\n";
    
    server.listen("0.0.0.0", 8080);
    
    return 0;
}
