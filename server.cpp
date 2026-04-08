// ============================================================
//  server.cpp  –  Disaster Alert Notification Server
//
//  Features:
//   • Multi-threaded TCP server (Poco::Net::TCPServer)
//   • Admin authentication (username + hashed password)
//   • Broadcast to all connected clients simultaneously
//   • Per-client subscription filtering (region + type)
//   • Delivery acknowledgement tracking
//   • Heartbeat / keep-alive monitoring
//   • Alert history replay for newly joined clients
//   • Alert retraction broadcast
//   • JSON persistent alert log
//   • Expiry-based automatic alert invalidation
//   • Real-time delivery statistics dashboard
// ============================================================

#include "../common/AlertProtocol.h"
#include "../common/AlertLogger.h"
#include "../common/Utils.h"

#include <Poco/Net/TCPServer.h>
#include <Poco/Net/TCPServerConnection.h>
#include <Poco/Net/TCPServerConnectionFactory.h>
#include <Poco/Net/TCPServerParams.h>
#include <Poco/Net/StreamSocket.h>
#include <Poco/Net/ServerSocket.h>
#include <Poco/Net/SocketAddress.h>
#include <Poco/Util/ServerApplication.h>
#include <Poco/Thread.h>
#include <Poco/ThreadPool.h>
#include <Poco/Mutex.h>
#include <Poco/Condition.h>
#include <Poco/Timestamp.h>
#include <Poco/DateTimeFormatter.h>
#include <Poco/MD5Engine.h>
#include <Poco/DigestStream.h>
#include <Poco/StreamCopier.h>

#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <memory>
#include <algorithm>
#include <atomic>
#include <thread>
#include <chrono>

using namespace DAS;

// ─────────────────────────────────────────────────────────
//  Global server state (protected by mutex)
// ─────────────────────────────────────────────────────────
struct ClientInfo {
    std::string clientId;
    std::string username;
    std::string subscribedRegion; // "" = all regions
    std::set<DisasterType> subscribedTypes; // empty = all types
    Poco::Net::StreamSocket* socket;
    Poco::Timestamp lastHeartbeat;
    bool isAdmin;
    std::set<std::string> ackedAlerts; // alertIds acknowledged

    ClientInfo()
        : socket(nullptr), isAdmin(false) {}
};

struct DeliveryStats {
    std::atomic<int> delivered{0};
    std::atomic<int> acknowledged{0};
};

class ServerState {
public:
    Poco::FastMutex clientsMutex;
    Poco::FastMutex alertsMutex;

    std::map<std::string, ClientInfo*>     clients;   // clientId -> info
    std::map<std::string, Alert>           alerts;    // alertId  -> alert
    std::map<std::string, DeliveryStats*>  stats;     // alertId  -> stats

    // Admin credentials  (md5 hashed)
    // Default: admin / admin123
    std::map<std::string, std::string> admins;

    AlertLogger logger;

    ServerState() {
        // Load default admin credentials
        // Password hashing using Poco MD5
        admins["admin"] = hashPassword("admin123");
        admins["root"]  = hashPassword("disaster2024");
        logger.logEvent("Server state initialised");
    }

    static std::string hashPassword(const std::string& pw) {
        Poco::MD5Engine md5;
        md5.update(pw);
        return Poco::DigestEngine::digestToHex(md5.digest());
    }

    bool validateAdmin(const std::string& user, const std::string& pw) {
        auto it = admins.find(user);
        if (it == admins.end()) return false;
        return it->second == hashPassword(pw);
    }

    void addClient(ClientInfo* ci) {
        Poco::FastMutex::ScopedLock lock(clientsMutex);
        clients[ci->clientId] = ci;
    }

    void removeClient(const std::string& id) {
        Poco::FastMutex::ScopedLock lock(clientsMutex);
        clients.erase(id);
    }

    void storeAlert(const Alert& a) {
        Poco::FastMutex::ScopedLock lock(alertsMutex);
        alerts[a.alertId] = a;
        stats[a.alertId]  = new DeliveryStats();
        logger.logAlert(a);
    }

    void retractAlert(const std::string& id) {
        Poco::FastMutex::ScopedLock lock(alertsMutex);
        auto it = alerts.find(id);
        if (it != alerts.end()) {
            it->second.active = false;
            logger.logEvent("Alert retracted: " + id);
        }
    }

    // Returns serialised alert list for history replay
    std::string getHistoryPayload() {
        Poco::FastMutex::ScopedLock lock(alertsMutex);
        std::string payload;
        int count = 0;
        for (auto& [id, a] : alerts) {
            if (a.active && !isExpired(a)) {
                if (!payload.empty()) payload += "~~";
                payload += serializeAlert(a);
                ++count;
            }
        }
        return std::to_string(count) + "~~" + payload;
    }
};

// Global state singleton
static ServerState* gState = nullptr;

// ─────────────────────────────────────────────────────────
//  Safe send helper
// ─────────────────────────────────────────────────────────
static bool safeSend(Poco::Net::StreamSocket& sock, const std::string& msg) {
    try {
        int total = 0;
        int len   = (int)msg.size();
        while (total < len) {
            int sent = sock.sendBytes(msg.c_str() + total, len - total);
            if (sent <= 0) return false;
            total += sent;
        }
        return true;
    } catch (...) {
        return false;
    }
}

// ─────────────────────────────────────────────────────────
//  Broadcast to matching clients
// ─────────────────────────────────────────────────────────
static void broadcastAlert(const Alert& alert) {
    std::string wire = buildMessage(MessageType::ALERT, serializeAlert(alert));
    Poco::FastMutex::ScopedLock lock(gState->clientsMutex);

    for (auto& [id, ci] : gState->clients) {
        // Filter by subscription
        bool regionMatch = ci->subscribedRegion.empty() ||
                           ci->subscribedRegion == alert.affectedRegion;
        bool typeMatch   = ci->subscribedTypes.empty() ||
                           ci->subscribedTypes.count(alert.type);

        if (regionMatch && typeMatch && ci->socket) {
            if (safeSend(*ci->socket, wire)) {
                auto it = gState->stats.find(alert.alertId);
                if (it != gState->stats.end())
                    ++it->second->delivered;
                gState->logger.logEvent(
                    "Alert " + alert.alertId + " sent to " + ci->username);
            }
        }
    }
}

// ─────────────────────────────────────────────────────────
//  Per-connection handler
// ─────────────────────────────────────────────────────────
class AlertConnection : public Poco::Net::TCPServerConnection {
public:
    explicit AlertConnection(const Poco::Net::StreamSocket& sock)
        : Poco::Net::TCPServerConnection(sock) {}

    void run() override {
        Poco::Net::StreamSocket& sock = socket();
        sock.setReceiveTimeout(Poco::Timespan(60, 0)); // 60s timeout

        // Create client record
        ClientInfo ci;
        ci.clientId = generateAlertId();
        ci.socket   = &sock;
        ci.isAdmin  = false;
        ci.lastHeartbeat.update();
        gState->addClient(&ci);

        gState->logger.logEvent("New connection: " + ci.clientId
                                + " from " + sock.peerAddress().toString());

        std::string recvBuf;
        char buf[4096];

        try {
            while (true) {
                int n = sock.receiveBytes(buf, sizeof(buf) - 1);
                if (n <= 0) break;
                buf[n] = '\0';
                recvBuf += buf;

                // Process all complete messages
                size_t pos;
                while ((pos = recvBuf.find(MSG_END)) != std::string::npos) {
                    std::string raw = recvBuf.substr(0, pos + 1);
                    recvBuf.erase(0, pos + 1);
                    handleMessage(raw, ci, sock);
                }
            }
        } catch (Poco::TimeoutException&) {
            gState->logger.logEvent("Client timed out: " + ci.clientId);
        } catch (std::exception& e) {
            gState->logger.logEvent("Connection error ["
                                    + ci.clientId + "]: " + e.what());
        }

        gState->removeClient(ci.clientId);
        gState->logger.logEvent("Disconnected: " + ci.clientId
                                + " (" + ci.username + ")");
    }

private:
    void handleMessage(const std::string& raw, ClientInfo& ci,
                       Poco::Net::StreamSocket& sock) {
        MessageType type;
        std::string payload;
        if (!parseMessage(raw, type, payload)) return;

        switch (type) {
        // ── Client registers ───────────────────────────
        case MessageType::REGISTER: {
            // payload: username|region
            auto sep = payload.find(FIELD_SEP);
            ci.username = payload.substr(0, sep);
            if (sep != std::string::npos)
                ci.subscribedRegion = payload.substr(sep + 1);

            gState->logger.logEvent("Registered: " + ci.username
                                    + " region=" + ci.subscribedRegion);

            std::string ack = buildMessage(MessageType::REGISTER_ACK,
                                            ci.clientId + "|Welcome to DAS");
            safeSend(sock, ack);

            // Send active alert history
            std::string hist = gState->getHistoryPayload();
            if (hist != "0~~") {
                safeSend(sock, buildMessage(MessageType::HISTORY_RESP, hist));
            }
            break;
        }
        // ── Heartbeat ────────────────────────────────
        case MessageType::HEARTBEAT: {
            ci.lastHeartbeat.update();
            safeSend(sock, buildMessage(MessageType::HEARTBEAT_ACK, "pong"));
            break;
        }
        // ── Client acks an alert ──────────────────────
        case MessageType::ACK: {
            ci.ackedAlerts.insert(payload);
            auto it = gState->stats.find(payload);
            if (it != gState->stats.end())
                ++it->second->acknowledged;
            gState->logger.logEvent(ci.username + " acked " + payload);
            break;
        }
        // ── Admin login ───────────────────────────────
        case MessageType::ADMIN_LOGIN: {
            auto sep  = payload.find(FIELD_SEP);
            std::string user = payload.substr(0, sep);
            std::string pw   = (sep != std::string::npos)
                               ? payload.substr(sep + 1) : "";
            if (gState->validateAdmin(user, pw)) {
                ci.isAdmin  = true;
                ci.username = user + "[ADMIN]";
                safeSend(sock, buildMessage(MessageType::ADMIN_LOGIN_ACK,
                                            "OK|Authenticated as " + user));
                gState->logger.logEvent("Admin login: " + user);
            } else {
                safeSend(sock, buildMessage(MessageType::ADMIN_LOGIN_ACK,
                                            "FAIL|Invalid credentials"));
                gState->logger.logEvent("Failed admin login attempt: " + user);
            }
            break;
        }
        // ── Subscription update ───────────────────────
        case MessageType::SUBSCRIBE: {
            // payload: region|TYPE1,TYPE2,...
            auto sep = payload.find(FIELD_SEP);
            ci.subscribedRegion = payload.substr(0, sep);
            ci.subscribedTypes.clear();
            if (sep != std::string::npos) {
                std::string types = payload.substr(sep + 1);
                std::istringstream ss(types);
                std::string tok;
                while (std::getline(ss, tok, ','))
                    if (!tok.empty())
                        ci.subscribedTypes.insert(disasterTypeFromString(tok));
            }
            safeSend(sock, buildMessage(MessageType::REGISTER_ACK,
                                        "Subscription updated"));
            break;
        }
        // ── History request ───────────────────────────
        case MessageType::HISTORY_REQ: {
            safeSend(sock, buildMessage(MessageType::HISTORY_RESP,
                                        gState->getHistoryPayload()));
            break;
        }
        // ── Admin: broadcast alert ────────────────────
        case MessageType::ALERT: {
            if (!ci.isAdmin) {
                safeSend(sock, buildMessage(MessageType::ACK,
                                            "DENY|Not authorised"));
                return;
            }
            Alert a = deserializeAlert(payload);
            a.timestamp = currentTime();
            if (a.alertId.empty()) a.alertId = generateAlertId();
            a.issuedBy  = ci.username;
            gState->storeAlert(a);
            broadcastAlert(a);
            safeSend(sock, buildMessage(MessageType::ACK,
                                        "OK|" + a.alertId));
            std::cout << "\n[SERVER] Alert broadcast: "
                      << disasterTypeToString(a.type)
                      << " @ " << a.location << "\n";
            break;
        }
        // ── Admin: retract alert ──────────────────────
        case MessageType::RETRACT: {
            if (!ci.isAdmin) return;
            gState->retractAlert(payload);
            // Broadcast retraction
            std::string retractMsg = buildMessage(MessageType::RETRACT, payload);
            Poco::FastMutex::ScopedLock lock(gState->clientsMutex);
            for (auto& [id, c] : gState->clients)
                if (c->socket) safeSend(*c->socket, retractMsg);
            break;
        }
        // ── Admin: delivery stats ─────────────────────
        case MessageType::STATUS_REQ: {
            if (!ci.isAdmin) return;
            std::string resp;
            Poco::FastMutex::ScopedLock lock(gState->alertsMutex);
            for (auto& [id, st] : gState->stats) {
                if (!resp.empty()) resp += "~~";
                resp += statsToString(id, st->delivered.load(),
                                       st->acknowledged.load());
            }
            safeSend(sock, buildMessage(MessageType::STATUS_RESP, resp));
            break;
        }
        default:
            break;
        }
    }
};

// ─────────────────────────────────────────────────────────
//  Connection factory
// ─────────────────────────────────────────────────────────
class AlertConnectionFactory
    : public Poco::Net::TCPServerConnectionFactory {
public:
    Poco::Net::TCPServerConnection*
    createConnection(const Poco::Net::StreamSocket& sock) override {
        return new AlertConnection(sock);
    }
};

// ─────────────────────────────────────────────────────────
//  Expiry checker thread – runs every 30 seconds
// ─────────────────────────────────────────────────────────
static void expiryCheckerThread() {
    while (true) {
        Poco::Thread::sleep(30000);
        Poco::FastMutex::ScopedLock lock(gState->alertsMutex);
        for (auto& [id, a] : gState->alerts) {
            if (a.active && isExpired(a)) {
                a.active = false;
                gState->logger.logEvent("Alert expired: " + id);
                std::cout << "[SERVER] Alert expired: " << id << "\n";
            }
        }
    }
}

// ─────────────────────────────────────────────────────────
//  Heartbeat watchdog – disconnects silent clients
// ─────────────────────────────────────────────────────────
static void heartbeatWatchdog() {
    while (true) {
        Poco::Thread::sleep(60000); // check every 60s
        Poco::FastMutex::ScopedLock lock(gState->clientsMutex);
        Poco::Timestamp now;
        for (auto& [id, ci] : gState->clients) {
            Poco::Timespan silence = now - ci->lastHeartbeat;
            if (silence.totalSeconds() > 120) { // 2 min silence
                gState->logger.logEvent("Stale client detected: " + id);
                if (ci->socket) {
                    try { ci->socket->close(); } catch (...) {}
                }
            }
        }
    }
}

// ─────────────────────────────────────────────────────────
//  Main entry point
// ─────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    int port = 9000;
    if (argc > 1) port = std::stoi(argv[1]);

    gState = new ServerState();
    printBanner("Disaster Alert Notification Server");
    std::cout << " Listening on port : " << port << "\n";
    std::cout << " Log file          : server.log\n";
    std::cout << " Alert archive     : alerts_log.json\n";
    std::cout << " Press Ctrl+C to stop\n\n";

    try {
        Poco::Net::ServerSocket svs(port);

        Poco::Net::TCPServerParams* params = new Poco::Net::TCPServerParams();
        params->setMaxThreads(64);
        params->setMaxQueued(32);
        params->setThreadIdleTime(Poco::Timespan(60, 0));

        Poco::Net::TCPServer server(new AlertConnectionFactory(), svs, params);
        server.start();

        gState->logger.logEvent("Server started on port " + std::to_string(port));
        std::cout << "[SERVER] Started. Waiting for connections...\n";

        // Start background threads
        std::thread expiryThread(expiryCheckerThread);
        expiryThread.detach();
        std::thread watchdogThread(heartbeatWatchdog);
        watchdogThread.detach();

        // Wait for Ctrl+C
        std::string cmd;
        while (true) {
            std::cout << "\n[SERVER CMD] Enter command (stats/quit): ";
            std::getline(std::cin, cmd);
            if (cmd == "quit" || cmd == "exit") break;
            else if (cmd == "stats") {
                Poco::FastMutex::ScopedLock cl(gState->clientsMutex);
                std::cout << "Connected clients: " << gState->clients.size() << "\n";
                Poco::FastMutex::ScopedLock al(gState->alertsMutex);
                std::cout << "Stored alerts    : " << gState->alerts.size() << "\n";
                for (auto& [id, st] : gState->stats)
                    std::cout << "  " << id.substr(0, 8)
                              << "... delivered=" << st->delivered.load()
                              << " acked=" << st->acknowledged.load() << "\n";
            }
        }

        server.stop();
        gState->logger.logEvent("Server shutdown");
    } catch (std::exception& e) {
        std::cerr << "[SERVER ERROR] " << e.what() << "\n";
        return 1;
    }
    return 0;
}
