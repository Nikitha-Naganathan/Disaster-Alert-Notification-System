// ============================================================
//  admin.cpp  –  Admin Control Interface
//
//  Features:
//   • Authenticated admin session (connects as admin client)
//   • Interactive menu to compose and broadcast alerts
//   • Predefined alert templates for common disasters
//   • Alert retraction by ID
//   • Real-time delivery statistics dashboard
//   • Bulk alert scheduling from a text file
//   • Admin-to-admin messaging (future: via server relay)
// ============================================================

#include "../common/AlertProtocol.h"
#include "../common/Utils.h"

#include <Poco/Net/StreamSocket.h>
#include <Poco/Net/SocketAddress.h>

#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <fstream>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>
#include <iomanip>

using namespace DAS;

static Poco::Net::StreamSocket* gAdminSocket = nullptr;
static std::atomic<bool>        gRunning{true};
static std::mutex               gSockMutex;
static std::string              gAdminUser;
static std::map<std::string, std::pair<int,int>> gStatsCache; // id->(del,ack)

// ── Safe send ─────────────────────────────────────────────
static bool adminSend(const std::string& msg) {
    std::lock_guard<std::mutex> lock(gSockMutex);
    try {
        int total = 0, len = (int)msg.size();
        while (total < len) {
            int sent = gAdminSocket->sendBytes(msg.c_str() + total, len - total);
            if (sent <= 0) return false;
            total += sent;
        }
        return true;
    } catch (...) { return false; }
}

// ── Receive a single response ─────────────────────────────
static bool adminRecv(std::string& payload, MessageType& type,
                      int timeoutSec = 10) {
    char buf[MAX_MSG_LEN];
    std::string recvBuf;
    gAdminSocket->setReceiveTimeout(Poco::Timespan(timeoutSec, 0));
    try {
        int n = gAdminSocket->receiveBytes(buf, sizeof(buf) - 1);
        if (n <= 0) return false;
        buf[n] = '\0';
        recvBuf += buf;
        // Find end
        auto pos = recvBuf.find(MSG_END);
        if (pos == std::string::npos) return false;
        return parseMessage(recvBuf.substr(0, pos + 1), type, payload);
    } catch (...) { return false; }
}

// ── Receive loop for background messages ─────────────────
static void adminReceiveLoop() {
    char buf[MAX_MSG_LEN];
    std::string recvBuf;
    while (gRunning) {
        try {
            gAdminSocket->setReceiveTimeout(Poco::Timespan(3, 0));
            int n = gAdminSocket->receiveBytes(buf, sizeof(buf) - 1);
            if (n <= 0) { gRunning = false; break; }
            buf[n] = '\0';
            recvBuf += buf;
            size_t pos;
            while ((pos = recvBuf.find(MSG_END)) != std::string::npos) {
                std::string raw = recvBuf.substr(0, pos + 1);
                recvBuf.erase(0, pos + 1);
                MessageType type;
                std::string payload;
                if (!parseMessage(raw, type, payload)) continue;
                if (type == MessageType::ACK)
                    std::cout << "\n[ACK] " << payload << "\n";
                else if (type == MessageType::STATUS_RESP) {
                    // Parse stats
                    gStatsCache.clear();
                    std::string rest = payload;
                    size_t sep;
                    while (!(rest.empty())) {
                        sep = rest.find("~~");
                        std::string entry = (sep != std::string::npos)
                                            ? rest.substr(0, sep) : rest;
                        std::string id; int d, a;
                        parseStats(entry, id, d, a);
                        gStatsCache[id] = {d, a};
                        if (sep == std::string::npos) break;
                        rest = rest.substr(sep + 2);
                    }
                }
            }
        } catch (Poco::TimeoutException&) {
            // normal
        } catch (...) { gRunning = false; break; }
    }
}

// ─────────────────────────────────────────────────────────
//  Build alert interactively
// ─────────────────────────────────────────────────────────
static Alert buildAlertInteractive() {
    Alert a;
    a.alertId  = generateAlertId();
    a.issuedBy = gAdminUser;
    a.timestamp = currentTime();
    a.active    = true;

    std::cout << "\n\033[1mDisaster Types:\033[0m\n"
              << " 1. EARTHQUAKE        2. FLOOD\n"
              << " 3. CYCLONE           4. TSUNAMI\n"
              << " 5. WILDFIRE          6. LANDSLIDE\n"
              << " 7. INDUSTRIAL_ACCIDENT 8. EPIDEMIC\n"
              << " 9. DROUGHT          10. OTHER\n"
              << "Choice: ";
    int tc; std::cin >> tc; std::cin.ignore();
    DisasterType types[] = {
        DisasterType::EARTHQUAKE, DisasterType::FLOOD,
        DisasterType::CYCLONE,    DisasterType::TSUNAMI,
        DisasterType::WILDFIRE,   DisasterType::LANDSLIDE,
        DisasterType::INDUSTRIAL_ACCIDENT, DisasterType::EPIDEMIC,
        DisasterType::DROUGHT,    DisasterType::OTHER
    };
    a.type = (tc >= 1 && tc <= 10) ? types[tc - 1] : DisasterType::OTHER;

    std::cout << "\n\033[1mSeverity Levels:\033[0m\n"
              << " 0. INFO  1. LOW  2. MEDIUM  3. HIGH  4. CRITICAL\n"
              << "Choice: ";
    int sc; std::cin >> sc; std::cin.ignore();
    a.severity = static_cast<Severity>(std::max(0, std::min(4, sc)));

    std::cout << "Location (e.g. Chennai, Tamil Nadu): ";
    std::getline(std::cin, a.location);

    std::cout << "Latitude  (e.g. 13.0827): ";
    std::cin >> a.latitude; std::cin.ignore();

    std::cout << "Longitude (e.g. 80.2707): ";
    std::cin >> a.longitude; std::cin.ignore();

    std::cout << "Affected Region tag (e.g. SOUTH_INDIA): ";
    std::getline(std::cin, a.affectedRegion);

    std::cout << "Alert message (description): ";
    std::getline(std::cin, a.message);

    std::cout << "Public instructions (what to do): ";
    std::getline(std::cin, a.instructions);

    std::cout << "Expiry in hours (0 = never): ";
    int exh; std::cin >> exh; std::cin.ignore();
    a.expiryTime = (exh > 0) ? (currentTime() + exh * 3600) : 0;

    return a;
}

// ─────────────────────────────────────────────────────────
//  Predefined alert templates
// ─────────────────────────────────────────────────────────
static std::vector<Alert> gTemplates;

static void loadTemplates() {
    auto makeTemplate = [](DisasterType t, Severity s,
                           const std::string& loc,
                           const std::string& msg,
                           const std::string& instr,
                           const std::string& region,
                           double lat, double lon) {
        Alert a;
        a.alertId        = "";   // assigned on broadcast
        a.type           = t;
        a.severity       = s;
        a.location       = loc;
        a.message        = msg;
        a.instructions   = instr;
        a.affectedRegion = region;
        a.latitude       = lat;
        a.longitude      = lon;
        a.active         = true;
        return a;
    };

    gTemplates.push_back(makeTemplate(
        DisasterType::CYCLONE, Severity::HIGH,
        "Bay of Bengal Coast",
        "Severe cyclonic storm approaching at 150 km/h. "
        "Expected landfall within 24 hours.",
        "Move to higher ground. Secure loose items. "
        "Evacuate coastal zones immediately.",
        "COASTAL_INDIA", 13.08, 80.27));

    gTemplates.push_back(makeTemplate(
        DisasterType::FLOOD, Severity::MEDIUM,
        "Chennai, Tamil Nadu",
        "Heavy rainfall causing flash floods in low-lying areas. "
        "River water level rising rapidly.",
        "Avoid underpasses. Do not drive through flooded roads. "
        "Relocate to relief camps if needed.",
        "SOUTH_INDIA", 13.08, 80.27));

    gTemplates.push_back(makeTemplate(
        DisasterType::EARTHQUAKE, Severity::CRITICAL,
        "Andaman Islands",
        "Magnitude 7.2 earthquake detected. Tsunami warning issued.",
        "Move 3 km inland immediately. Do not return until all-clear. "
        "Tune to emergency radio.",
        "ANDAMAN", 11.74, 92.65));

    gTemplates.push_back(makeTemplate(
        DisasterType::WILDFIRE, Severity::HIGH,
        "Nilgiri Hills, Tamil Nadu",
        "Wildfire spreading rapidly across forest zones. "
        "Wind speed accelerating fire spread.",
        "Evacuate forest-adjacent villages. Wet towels over nose/mouth. "
        "Follow fire department instructions.",
        "SOUTH_INDIA", 11.41, 76.69));

    gTemplates.push_back(makeTemplate(
        DisasterType::EPIDEMIC, Severity::MEDIUM,
        "Multiple Districts",
        "Outbreak of waterborne illness detected. "
        "Contaminated water supply suspected.",
        "Boil all drinking water. Visit nearest health camp. "
        "Report symptoms immediately.",
        "STATEWIDE", 12.97, 80.17));
}

// ─────────────────────────────────────────────────────────
//  Stats display
// ─────────────────────────────────────────────────────────
static void displayStats() {
    adminSend(buildMessage(MessageType::STATUS_REQ, ""));
    std::this_thread::sleep_for(std::chrono::seconds(1));

    const std::string sep(55, '-');
    std::cout << "\n" << sep << "\n";
    std::cout << std::left << std::setw(38) << "Alert ID"
              << std::setw(10) << "Delivered"
              << std::setw(10) << "Acked\n";
    std::cout << sep << "\n";
    for (auto& [id, p] : gStatsCache) {
        std::cout << std::setw(38) << id.substr(0, 36)
                  << std::setw(10) << p.first
                  << std::setw(10) << p.second << "\n";
    }
    std::cout << sep << "\n";
}

// ─────────────────────────────────────────────────────────
//  Load and broadcast alerts from file
//  File format (one alert per block, blank-line separated):
//    TYPE=FLOOD
//    SEVERITY=HIGH
//    LOCATION=Chennai
//    LAT=13.08
//    LON=80.27
//    REGION=SOUTH_INDIA
//    MSG=Heavy rainfall...
//    INSTR=Evacuate...
//    EXPIRY=12
// ─────────────────────────────────────────────────────────
static void broadcastFromFile(const std::string& path) {
    std::ifstream in(path);
    if (!in) { std::cout << "Cannot open file: " << path << "\n"; return; }

    int count = 0;
    std::string line;
    Alert a;
    a.active = true;

    auto dispatch = [&]() {
        if (a.location.empty()) return;
        a.alertId   = generateAlertId();
        a.issuedBy  = gAdminUser;
        a.timestamp = currentTime();
        adminSend(buildMessage(MessageType::ALERT, serializeAlert(a)));
        std::cout << "Broadcasted: " << disasterTypeToString(a.type)
                  << " @ " << a.location << "\n";
        ++count;
        a = Alert(); a.active = true;
    };

    while (std::getline(in, line)) {
        if (line.empty()) { dispatch(); continue; }
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        if (key == "TYPE")     a.type     = disasterTypeFromString(val);
        else if (key == "SEVERITY")  a.severity = severityFromString(val);
        else if (key == "LOCATION")  a.location = val;
        else if (key == "LAT")       a.latitude  = std::stod(val);
        else if (key == "LON")       a.longitude = std::stod(val);
        else if (key == "REGION")    a.affectedRegion = val;
        else if (key == "MSG")       a.message  = val;
        else if (key == "INSTR")     a.instructions = val;
        else if (key == "EXPIRY") {
            int h = std::stoi(val);
            a.expiryTime = (h > 0) ? currentTime() + h * 3600 : 0;
        }
    }
    dispatch(); // flush last block
    std::cout << count << " alert(s) broadcast from file.\n";
}

// ─────────────────────────────────────────────────────────
//  Admin menu
// ─────────────────────────────────────────────────────────
static void adminMenu() {
    while (gRunning) {
        std::cout << "\n\033[1;34m╔══════════════════════════════╗\033[0m\n"
                  << "\033[1;34m║   DAS ADMIN CONTROL PANEL    ║\033[0m\n"
                  << "\033[1;34m╚══════════════════════════════╝\033[0m\n"
                  << " 1. Broadcast New Alert (Interactive)\n"
                  << " 2. Use Alert Template\n"
                  << " 3. Retract Alert\n"
                  << " 4. View Delivery Statistics\n"
                  << " 5. Broadcast Alerts from File\n"
                  << " 6. Exit\n"
                  << "Choice: ";

        int ch; std::cin >> ch; std::cin.ignore();

        switch (ch) {
        case 1: {
            Alert a = buildAlertInteractive();
            a.issuedBy = gAdminUser;
            adminSend(buildMessage(MessageType::ALERT, serializeAlert(a)));
            std::cout << "\033[32mAlert broadcast sent.\033[0m\n";
            break;
        }
        case 2: {
            std::cout << "\nTemplates:\n";
            for (int i = 0; i < (int)gTemplates.size(); ++i) {
                auto& t = gTemplates[i];
                std::cout << " [" << i+1 << "] "
                          << disasterTypeToString(t.type)
                          << " – " << t.location
                          << " (" << severityToString(t.severity) << ")\n";
            }
            std::cout << "Choose template (0 to cancel): ";
            int ti; std::cin >> ti; std::cin.ignore();
            if (ti < 1 || ti > (int)gTemplates.size()) break;
            Alert a     = gTemplates[ti - 1];
            a.alertId   = generateAlertId();
            a.issuedBy  = gAdminUser;
            a.timestamp = currentTime();
            adminSend(buildMessage(MessageType::ALERT, serializeAlert(a)));
            std::cout << "\033[32mTemplate alert broadcast.\033[0m\n";
            break;
        }
        case 3: {
            std::cout << "Enter Alert ID prefix to retract: ";
            std::string id; std::getline(std::cin, id);
            adminSend(buildMessage(MessageType::RETRACT, id));
            std::cout << "\033[33mRetraction sent.\033[0m\n";
            break;
        }
        case 4:
            displayStats();
            break;
        case 5: {
            std::cout << "File path: ";
            std::string path; std::getline(std::cin, path);
            broadcastFromFile(path);
            break;
        }
        case 6:
            gRunning = false;
            break;
        default:
            std::cout << "Invalid choice.\n";
        }
    }
}

// ─────────────────────────────────────────────────────────
//  Main
// ─────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    std::string host = "127.0.0.1";
    int port = 9000;
    if (argc > 1) host = argv[1];
    if (argc > 2) port = std::stoi(argv[2]);

    printBanner("DAS Admin Interface");

    std::cout << "Admin username: ";
    std::getline(std::cin, gAdminUser);
    std::cout << "Admin password: ";
    std::string pw;
    // Disable echo (Unix)
#ifndef _WIN32
    system("stty -echo");
#endif
    std::getline(std::cin, pw);
#ifndef _WIN32
    system("stty echo");
#endif
    std::cout << "\n";

    // ── Connect ────────────────────────────────────────
    try {
        Poco::Net::SocketAddress addr(host, port);
        gAdminSocket = new Poco::Net::StreamSocket(addr);
    } catch (std::exception& e) {
        std::cerr << "Connection failed: " << e.what() << "\n";
        return 1;
    }

    // Register as regular client first
    adminSend(buildMessage(MessageType::REGISTER,
                            gAdminUser + FIELD_SEP + "ADMIN"));

    // Then authenticate as admin
    adminSend(buildMessage(MessageType::ADMIN_LOGIN,
                            gAdminUser + FIELD_SEP + pw));

    // Start background receive
    std::thread recvThread(adminReceiveLoop);

    // Wait for auth acknowledgement
    std::this_thread::sleep_for(std::chrono::seconds(1));

    loadTemplates();
    adminMenu();

    gRunning = false;
    try { gAdminSocket->close(); } catch (...) {}
    recvThread.join();
    return 0;
}
