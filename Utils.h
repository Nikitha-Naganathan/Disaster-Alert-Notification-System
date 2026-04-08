#pragma once
// ============================================================
//  Utils.h  –  Shared utility helpers
//  Disaster Alert Notification System
// ============================================================

#include "AlertProtocol.h"

#include <Poco/UUID.h>
#include <Poco/UUIDGenerator.h>
#include <Poco/DateTimeFormatter.h>
#include <Poco/Timestamp.h>
#include <Poco/DateTime.h>

#include <string>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <iostream>

namespace DAS {

// ── Unique alert ID generator ─────────────────────────────
inline std::string generateAlertId() {
    Poco::UUIDGenerator& gen = Poco::UUIDGenerator::defaultGenerator();
    return gen.createRandom().toString();
}

// ── Human-readable timestamp ──────────────────────────────
inline std::string formatTimestamp(std::time_t t) {
    std::tm tm_info;
#ifdef _WIN32
    localtime_s(&tm_info, &t);
#else
    localtime_r(&t, &tm_info);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_info);
    return std::string(buf);
}

inline std::time_t currentTime() {
    return std::time(nullptr);
}

// ── Check if alert is expired ─────────────────────────────
inline bool isExpired(const Alert& a) {
    if (a.expiryTime == 0) return false; // Never expires
    return std::time(nullptr) > a.expiryTime;
}

// ── Pretty-print banner for terminal ─────────────────────
inline void printBanner(const std::string& title) {
    const std::string border(60, '=');
    std::cout << "\033[1;34m" << border << "\033[0m\n";
    int pad = (60 - (int)title.size()) / 2;
    std::cout << "\033[1;34m"
              << std::string(pad, ' ') << title
              << "\033[0m\n";
    std::cout << "\033[1;34m" << border << "\033[0m\n";
}

// ── Pretty-print a single alert ───────────────────────────
inline void printAlert(const Alert& alert) {
    const std::string sep(60, '-');
    std::string color = severityColor(alert.severity);
    const std::string reset = "\033[0m";

    std::cout << "\n" << color << sep << reset << "\n";
    std::cout << color << " *** DISASTER ALERT ***" << reset << "\n";
    std::cout << color << sep << reset << "\n";
    std::cout << " ID       : " << alert.alertId << "\n";
    std::cout << " Type     : " << disasterEmoji(alert.type)
              << " " << disasterTypeToString(alert.type) << "\n";
    std::cout << " Severity : " << color
              << severityToString(alert.severity) << reset << "\n";
    std::cout << " Location : " << alert.location << "\n";
    std::cout << " Coords   : " << std::fixed << std::setprecision(4)
              << alert.latitude << " N, " << alert.longitude << " E\n";
    std::cout << " Region   : " << alert.affectedRegion << "\n";
    std::cout << " Issued by: " << alert.issuedBy << "\n";
    std::cout << " Time     : " << formatTimestamp(alert.timestamp) << "\n";
    if (alert.expiryTime > 0)
        std::cout << " Expires  : " << formatTimestamp(alert.expiryTime) << "\n";
    std::cout << "\n Message  :\n  " << alert.message << "\n";
    std::cout << "\n Instructions:\n  " << alert.instructions << "\n";
    std::cout << color << sep << reset << "\n\n";
}

// ── Print a compact summary row ───────────────────────────
inline void printAlertSummary(const Alert& alert, int index = -1) {
    std::string color  = severityColor(alert.severity);
    const std::string reset = "\033[0m";
    if (index >= 0)
        std::cout << "[" << std::setw(3) << index << "] ";
    std::cout << color
              << std::left << std::setw(12)
              << severityToString(alert.severity) << reset
              << " | " << std::setw(20) << alert.location
              << " | " << disasterTypeToString(alert.type)
              << " | " << formatTimestamp(alert.timestamp) << "\n";
}

// ── Compute delivery stats string ─────────────────────────
inline std::string statsToString(const std::string& alertId,
                                  int delivered, int ackd) {
    std::ostringstream s;
    s << alertId << FIELD_SEP << delivered << FIELD_SEP << ackd;
    return s.str();
}

// ── Parse stats string ─────────────────────────────────────
inline void parseStats(const std::string& data,
                        std::string& alertId, int& delivered, int& ackd) {
    auto p1 = data.find(FIELD_SEP);
    auto p2 = data.find(FIELD_SEP, p1 + 1);
    if (p1 == std::string::npos) return;
    alertId   = data.substr(0, p1);
    delivered = std::stoi(data.substr(p1 + 1, p2 - p1 - 1));
    ackd      = std::stoi(data.substr(p2 + 1));
}

// ── Terminal spinner for async waits ─────────────────────
class Spinner {
public:
    void spin(int steps = 4, int delayMs = 100) {
        const char* frames[] = { "|", "/", "-", "\\" };
        for (int i = 0; i < steps; ++i) {
            std::cout << "\r" << frames[i % 4] << " " << std::flush;
            Poco::Thread::sleep(delayMs);
        }
        std::cout << "\r  \r" << std::flush;
    }
};

} // namespace DAS
