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

using json = nlohmann::json;

class OnlineManager {
private:
    std::mutex mtx_;
    std::unordered_set<std::string> online_users_;  // 在线用户ID集合
    std::unordered_set<std::string> online_sessions_; // 在线会话ID
    std::atomic<int> total_online_{0};  // 总在线人数
    
    // 清理过期连接
    struct SessionInfo {
        std::string session_id;
        std::string user_id;
        std::chrono::steady_clock::time_point last_active;
    };
    std::unordered_map<std::string, SessionInfo> sessions_;
    
public:
    OnlineManager() {
        // 启动清理线程
        std::thread([this]() {
            while (true) {
                cleanupExpiredSessions();
                std::this_thread::sleep_for(std::chrono::seconds(30));
            }
        }).detach();
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
        
        total_online_ = online_users_.size();
        
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
            total_online_ = online_users_.size();
        }
    }
    
    // 获取在线人数
    int getOnlineCount() const {
        return total_online_;
    }
    
    // 获取在线用户列表
    std::vector<std::string> getOnlineUsers() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return std::vector<std::string>(online_users_.begin(), online_users_.end());
    }
    
private:
    std::string generateSessionId() {
        auto now = std::chrono::system_clock::now();
        auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count();
        
        // 简单生成随机会话ID
        return "sess_" + std::to_string(timestamp) + "_" + 
               std::to_string(rand() % 10000);
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
        
        total_online_ = online_users_.size();
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
                {"timestamp", std::chrono::system_clock::now().time_since_epoch().count()}
            }}
        };
        
        res.set_content(response.dump(), "application/json");
    });
    
    // 2. 用户登录（上线）
    server.Post("/api/online/login", [&](const httplib::Request& req, httplib::Response& res) {
        try {
            auto body = json::parse(req.body);
            std::string user_id = body["user_id"];
            
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
                {"message", e.what()}
            };
            res.set_content(response.dump(), "application/json");
        }
    });
    
    // 3. 心跳接口
    server.Post("/api/online/heartbeat", [&](const httplib::Request& req, httplib::Response& res) {
        try {
            auto body = json::parse(req.body);
            std::string session_id = body["session_id"];
            
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
            std::string session_id = body["session_id"];
            
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
    
    // 6. 健康检查
    server.Get("/api/health", [](const httplib::Request& req, httplib::Response& res) {
        json response = {
            {"status", "healthy"},
            {"timestamp", std::chrono::system_clock::now().time_since_epoch().count()}
        };
        res.set_content(response.dump(), "application/json");
    });
    
    std::cout << "Starting server on port 8080...\n";
    std::cout << "API endpoints:\n";
    std::cout << "  GET  /api/online/count     - 获取在线人数\n";
    std::cout << "  GET  /api/online/users     - 获取在线用户列表\n";
    std::cout << "  POST /api/online/login     - 用户登录\n";
    std::cout << "  POST /api/online/heartbeat - 心跳\n";
    std::cout << "  POST /api/online/logout    - 用户退出\n";
    
    server.listen("0.0.0.0", 8080);
    
    return 0;
}
