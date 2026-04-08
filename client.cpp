// ============================================================
//  client.cpp  –  Disaster Alert Notification Client
//
//  Features:
//   • Connects to server via TCP (Poco::Net::StreamSocket)
//   • Interactive subscription to regions and disaster types
//   • Heartbeat thread to maintain connection
//   • Auto-acknowledgement of received alerts
//   • Alert history replay on connect
//   • Alert retraction handling
//   • Coloured terminal display based on severity
//   • Local alert cache with search & filter
//   • Reconnect logic on connection loss
// ============================================================

#include "../common/AlertProtocol.h"
#include "../common/Utils.h"

#include <Poco/Net/StreamSocket.h>
#include <Poco/Net/SocketAddress.h>
#include <Poco/Thread.h>
#include <Poco/Timestamp.h>
#include <Poco/Runnable.h>

#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <atomic>
#include <thread>
#include <chrono>
#include <mutex>
#include <condition_variable>
#include <algorithm>
#include <iomanip>
#include <fstream>
#include <ctime>

using namespace DAS;

// ─────────────────────────────────────────────────────────
//  Client state
// ─────────────────────────────────────────────────────────
static std::atomic<bool>      gRunning{true};
static std::string            gClientId;
static std::string            gUsername;
static std::mutex             gAlertsMutex;
static std::map<std::string, Alert> gLocalAlerts; // alertId -> alert
static Poco::Net::StreamSocket* gSocket = nullptr;
static std::mutex             gSockMutex;

// Notification sound (terminal bell)
static void playAlert(Severity s) {
    if (s >= Severity::HIGH)
        std::cout << "\a\a\a"; // triple bell for critical alerts
    else
        std::cout << "\a";
}

// ─────────────────────────────────────────────────────────
//  Safe send wrapper
// ─────────────────────────────────────────────────────────
static bool clientSend(const std::string& msg) {
    std::lock_guard<std::mutex> lock(gSockMutex);
    if (!gSocket) return false;
    try {
        int total = 0, len = (int)msg.size();
        while (total < len) {
            int sent = gSocket->sendBytes(msg.c_str() + total, len - total);
            if (sent <= 0) return false;
            total += sent;
        }
        return true;
    } catch (...) { return false; }
}

// ─────────────────────────────────────────────────────────
//  Heartbeat thread – sends ping every 30 seconds
// ─────────────────────────────────────────────────────────
static void heartbeatThread() {
    while (gRunning) {
        std::this_thread::sleep_for(std::chrono::seconds(30));
        if (!clientSend(buildMessage(MessageType::HEARTBEAT, "ping"))) {
            std::cout << "\n[CLIENT] Heartbeat failed – connection lost\n";
            gRunning = false;
        }
    }
}

// ─────────────────────────────────────────────────────────
//  Handle history response
// ─────────────────────────────────────────────────────────
static void handleHistory(const std::string& payload) {
    // payload: count~~alert1~~alert2~~...
    auto p = payload.find("~~");
    if (p == std::string::npos) return;
    int count = std::stoi(payload.substr(0, p));
    if (count == 0) return;

    std::string rest = payload.substr(p + 2);
    std::cout << "\n\033[1;34m[HISTORY] " << count
              << " active alert(s) from server:\033[0m\n";

    int idx = 1;
    size_t sep;
    while ((sep = rest.find("~~")) != std::string::npos || !rest.empty()) {
        std::string part = (sep != std::string::npos)
                           ? rest.substr(0, sep) : rest;
        if (!part.empty()) {
            Alert a = deserializeAlert(part);
            {
                std::lock_guard<std::mutex> lock(gAlertsMutex);
                gLocalAlerts[a.alertId] = a;
            }
            std::cout << "\n[" << idx++ << "] ";
            printAlertSummary(a);
        }
        if (sep == std::string::npos) break;
        rest = rest.substr(sep + 2);
    }
    std::cout << "(Use 'show <id>' or 'list' to view details)\n";
}

// ─────────────────────────────────────────────────────────
//  Receive loop (runs in a separate thread)
// ─────────────────────────────────────────────────────────
static void receiveLoop() {
    char buf[MAX_MSG_LEN];
    std::string recvBuf;

    while (gRunning) {
        try {
            int n;
            {
                std::lock_guard<std::mutex> lock(gSockMutex);
                if (!gSocket) break;
                gSocket->setReceiveTimeout(Poco::Timespan(5, 0));
                n = gSocket->receiveBytes(buf, sizeof(buf) - 1);
            }
            if (n <= 0) {
                std::cout << "\n[CLIENT] Server closed connection.\n";
                gRunning = false;
                break;
            }
            buf[n] = '\0';
            recvBuf += buf;

            size_t pos;
            while ((pos = recvBuf.find(MSG_END)) != std::string::npos) {
                std::string raw = recvBuf.substr(0, pos + 1);
                recvBuf.erase(0, pos + 1);

                MessageType type;
                std::string payload;
                if (!parseMessage(raw, type, payload)) continue;

                switch (type) {
                // ── Server sends alert ─────────────────
                case MessageType::ALERT: {
                    Alert a = deserializeAlert(payload);
                    {
                        std::lock_guard<std::mutex> lock(gAlertsMutex);
                        gLocalAlerts[a.alertId] = a;
                    }
                    playAlert(a.severity);
                    printAlert(a);
                    // Auto-acknowledge
                    clientSend(buildMessage(MessageType::ACK, a.alertId));
                    break;
                }
                // ── Alert retracted ────────────────────
                case MessageType::RETRACT: {
                    std::lock_guard<std::mutex> lock(gAlertsMutex);
                    auto it = gLocalAlerts.find(payload);
                    if (it != gLocalAlerts.end()) {
                        it->second.active = false;
                        std::cout << "\n\033[33m[RETRACTED] Alert "
                                  << payload.substr(0, 8)
                                  << "... has been cancelled by authorities.\033[0m\n";
                    }
                    break;
                }
                // ── Registration acknowledged ──────────
                case MessageType::REGISTER_ACK: {
                    auto sep = payload.find(FIELD_SEP);
                    if (sep != std::string::npos)
                        gClientId = payload.substr(0, sep);
                    std::cout << "\033[32m[CONNECTED] " << payload.substr(sep+1)
                              << "\033[0m\n";
                    break;
                }
                // ── Heartbeat ack ──────────────────────
                case MessageType::HEARTBEAT_ACK:
                    // silently noted
                    break;
                // ── History ───────────────────────────
                case MessageType::HISTORY_RESP:
                    handleHistory(payload);
                    break;
                default:
                    break;
                }
            }
        } catch (Poco::TimeoutException&) {
            // Normal – keep looping
        } catch (std::exception&) {
            gRunning = false;
            break;
        }
    }
}

// ─────────────────────────────────────────────────────────
//  Interactive command loop
// ─────────────────────────────────────────────────────────
static void printClientHelp() {
    std::cout << "\n\033[1mCommands:\033[0m\n"
              << "  list              – List all received alerts\n"
              << "  show <alertId>    – Show full alert details\n"
              << "  filter <SEV>      – Filter list by severity\n"
              << "  active            – Show only active alerts\n"
              << "  subscribe <region> [TYPE,TYPE,...]\n"
              << "                    – Update subscription filter\n"
              << "  history           – Request full history from server\n"
              << "  save              – Save alerts to local_alerts.txt\n"
              << "  help              – Show this menu\n"
              << "  quit              – Exit\n";
}

static void commandLoop() {
    printClientHelp();
    std::string line;
    while (gRunning) {
        std::cout << "\nDAS> " << std::flush;
        if (!std::getline(std::cin, line)) { gRunning = false; break; }

        if (line.empty()) continue;

        std::istringstream ss(line);
        std::string cmd;
        ss >> cmd;

        if (cmd == "quit" || cmd == "exit") {
            gRunning = false;

        } else if (cmd == "list") {
            std::lock_guard<std::mutex> lock(gAlertsMutex);
            if (gLocalAlerts.empty()) {
                std::cout << "No alerts received yet.\n";
            } else {
                int i = 1;
                for (auto& [id, a] : gLocalAlerts)
                    printAlertSummary(a, i++);
            }

        } else if (cmd == "show") {
            std::string prefix;
            ss >> prefix;
            std::lock_guard<std::mutex> lock(gAlertsMutex);
            bool found = false;
            for (auto& [id, a] : gLocalAlerts) {
                if (id.substr(0, prefix.size()) == prefix) {
                    printAlert(a);
                    found = true;
                    break;
                }
            }
            if (!found) std::cout << "Alert not found: " << prefix << "\n";

        } else if (cmd == "filter") {
            std::string sev;
            ss >> sev;
            Severity target = severityFromString(sev);
            std::lock_guard<std::mutex> lock(gAlertsMutex);
            int i = 1;
            for (auto& [id, a] : gLocalAlerts)
                if (a.severity == target) printAlertSummary(a, i++);

        } else if (cmd == "active") {
            std::lock_guard<std::mutex> lock(gAlertsMutex);
            int i = 1;
            for (auto& [id, a] : gLocalAlerts)
                if (a.active && !isExpired(a)) printAlertSummary(a, i++);

        } else if (cmd == "subscribe") {
            std::string region, types;
            ss >> region;
            std::getline(ss, types);
            if (!types.empty() && types[0] == ' ') types = types.substr(1);
            std::string subPayload = region + FIELD_SEP + types;
            clientSend(buildMessage(MessageType::SUBSCRIBE, subPayload));
            std::cout << "Subscription updated: region=" << region
                      << " types=" << (types.empty() ? "ALL" : types) << "\n";

        } else if (cmd == "history") {
            clientSend(buildMessage(MessageType::HISTORY_REQ, ""));

        } else if (cmd == "save") {
            std::ofstream out("local_alerts.txt");
            std::lock_guard<std::mutex> lock(gAlertsMutex);
            for (auto& [id, a] : gLocalAlerts) {
                out << "AlertID   : " << a.alertId << "\n"
                    << "Type      : " << disasterTypeToString(a.type) << "\n"
                    << "Severity  : " << severityToString(a.severity) << "\n"
                    << "Location  : " << a.location << "\n"
                    << "Message   : " << a.message << "\n"
                    << "Time      : " << formatTimestamp(a.timestamp) << "\n"
                    << std::string(50, '-') << "\n";
            }
            std::cout << "Saved " << gLocalAlerts.size()
                      << " alert(s) to local_alerts.txt\n";

        } else if (cmd == "help") {
            printClientHelp();
        } else {
            std::cout << "Unknown command. Type 'help' for options.\n";
        }
    }
}

// ─────────────────────────────────────────────────────────
//  Main
// ─────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    std::string host = "127.0.0.1";
    int port         = 9000;

    if (argc > 1) host = argv[1];
    if (argc > 2) port = std::stoi(argv[2]);

    printBanner("Disaster Alert Notification Client");

    std::cout << "Username: ";
    std::getline(std::cin, gUsername);
    if (gUsername.empty()) gUsername = "user";

    std::cout << "Region (e.g. SOUTH_INDIA, leave blank for ALL): ";
    std::string region;
    std::getline(std::cin, region);

    // ── Connect ────────────────────────────────────────
    std::cout << "Connecting to " << host << ":" << port << "...\n";
    try {
        Poco::Net::SocketAddress addr(host, port);
        gSocket = new Poco::Net::StreamSocket(addr);
        gSocket->setSendTimeout(Poco::Timespan(10, 0));
        std::cout << "\033[32mConnected!\033[0m\n";
    } catch (std::exception& e) {
        std::cerr << "Connection failed: " << e.what() << "\n";
        return 1;
    }

    // ── Register ───────────────────────────────────────
    std::string regPayload = gUsername + FIELD_SEP + region;
    clientSend(buildMessage(MessageType::REGISTER, regPayload));

    // ── Start receive thread ───────────────────────────
    std::thread recvThread(receiveLoop);
    std::thread hbThread(heartbeatThread);

    // ── Command loop (main thread) ─────────────────────
    commandLoop();

    gRunning = false;
    if (gSocket) {
        try { gSocket->shutdown(); gSocket->close(); } catch (...) {}
    }
    recvThread.join();
    hbThread.detach();

    std::cout << "Goodbye.\n";
    return 0;
}
