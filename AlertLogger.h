#pragma once
// ============================================================
//  AlertLogger.h  –  Persistent JSON alert log + history
//  Disaster Alert Notification System
// ============================================================

#include "AlertProtocol.h"

#include <Poco/File.h>
#include <Poco/FileStream.h>
#include <Poco/Mutex.h>
#include <Poco/DateTimeFormatter.h>
#include <Poco/DateTime.h>
#include <Poco/Timestamp.h>

#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <iostream>

namespace DAS {

class AlertLogger {
public:
    explicit AlertLogger(const std::string& logPath = "alerts_log.json")
        : _logPath(logPath) {
        // Create log file if it doesn't exist
        std::ifstream check(logPath);
        if (!check.good()) {
            std::ofstream init(logPath);
            init << "[]" << std::endl;
        }
    }

    // Append an alert to the JSON log file
    void logAlert(const Alert& alert) {
        Poco::FastMutex::ScopedLock lock(_mutex);
        try {
            // Read existing content
            std::ifstream in(_logPath);
            std::string content((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());
            in.close();

            // Remove trailing ']' to append
            auto pos = content.rfind(']');
            if (pos != std::string::npos) content = content.substr(0, pos);

            // Add comma if not empty array
            bool needsComma = (content.find('{') != std::string::npos);

            std::ofstream out(_logPath, std::ios::trunc);
            out << content;
            if (needsComma) out << ",\n";
            out << alertToJson(alert) << "\n]";
            out.close();
        } catch (std::exception& e) {
            std::cerr << "[Logger] Error: " << e.what() << std::endl;
        }
    }

    // Load all alerts from log
    std::vector<Alert> loadHistory() {
        Poco::FastMutex::ScopedLock lock(_mutex);
        std::vector<Alert> alerts;
        // Simple line-by-line JSON parse (robust enough for our format)
        std::ifstream in(_logPath);
        std::string line, block;
        bool inBlock = false;
        while (std::getline(in, line)) {
            if (line.find('{') != std::string::npos) { inBlock = true; block.clear(); }
            if (inBlock) block += line + "\n";
            if (inBlock && line.find('}') != std::string::npos) {
                alerts.push_back(parseJsonAlert(block));
                inBlock = false;
            }
        }
        return alerts;
    }

    // Append a plain-text event to server.log
    void logEvent(const std::string& event) {
        Poco::FastMutex::ScopedLock lock(_mutex);
        std::ofstream out("server.log", std::ios::app);
        Poco::Timestamp now;
        out << "[" << Poco::DateTimeFormatter::format(now, "%Y-%m-%d %H:%M:%S") << "] "
            << event << "\n";
    }

private:
    std::string  _logPath;
    Poco::FastMutex _mutex;

    static std::string escapeJson(const std::string& s) {
        std::string out;
        for (char c : s) {
            if (c == '"')  out += "\\\"";
            else if (c == '\\') out += "\\\\";
            else if (c == '\n') out += "\\n";
            else if (c == '\r') out += "\\r";
            else out += c;
        }
        return out;
    }

    static std::string alertToJson(const Alert& a) {
        std::ostringstream j;
        j << "  {\n"
          << "    \"alertId\": \""       << escapeJson(a.alertId)    << "\",\n"
          << "    \"type\": \""          << disasterTypeToString(a.type) << "\",\n"
          << "    \"severity\": \""      << severityToString(a.severity) << "\",\n"
          << "    \"location\": \""      << escapeJson(a.location)   << "\",\n"
          << "    \"latitude\": "        << a.latitude               << ",\n"
          << "    \"longitude\": "       << a.longitude              << ",\n"
          << "    \"message\": \""       << escapeJson(a.message)    << "\",\n"
          << "    \"instructions\": \""  << escapeJson(a.instructions) << "\",\n"
          << "    \"timestamp\": "       << a.timestamp              << ",\n"
          << "    \"expiryTime\": "      << a.expiryTime             << ",\n"
          << "    \"issuedBy\": \""      << escapeJson(a.issuedBy)   << "\",\n"
          << "    \"active\": "          << (a.active ? "true" : "false") << ",\n"
          << "    \"affectedRegion\": \""<< escapeJson(a.affectedRegion) << "\"\n"
          << "  }";
        return j.str();
    }

    static std::string extractJsonValue(const std::string& block,
                                        const std::string& key) {
        // Finds "key": "value" or "key": number
        std::string search = "\"" + key + "\": ";
        auto pos = block.find(search);
        if (pos == std::string::npos) return "";
        pos += search.size();
        if (block[pos] == '"') {
            ++pos;
            std::string val;
            while (pos < block.size() && block[pos] != '"') {
                if (block[pos] == '\\') ++pos;
                val += block[pos++];
            }
            return val;
        } else {
            std::string val;
            while (pos < block.size() && block[pos] != ',' && block[pos] != '\n')
                val += block[pos++];
            return val;
        }
    }

    static Alert parseJsonAlert(const std::string& block) {
        Alert a;
        a.alertId       = extractJsonValue(block, "alertId");
        a.type          = disasterTypeFromString(extractJsonValue(block, "type"));
        a.severity      = severityFromString(extractJsonValue(block, "severity"));
        a.location      = extractJsonValue(block, "location");
        std::string lat = extractJsonValue(block, "latitude");
        std::string lon = extractJsonValue(block, "longitude");
        if (!lat.empty()) a.latitude  = std::stod(lat);
        if (!lon.empty()) a.longitude = std::stod(lon);
        a.message       = extractJsonValue(block, "message");
        a.instructions  = extractJsonValue(block, "instructions");
        std::string ts  = extractJsonValue(block, "timestamp");
        std::string exp = extractJsonValue(block, "expiryTime");
        if (!ts.empty())  a.timestamp  = (std::time_t)std::stoll(ts);
        if (!exp.empty()) a.expiryTime = (std::time_t)std::stoll(exp);
        a.issuedBy      = extractJsonValue(block, "issuedBy");
        std::string act = extractJsonValue(block, "active");
        a.active        = (act == "true");
        a.affectedRegion= extractJsonValue(block, "affectedRegion");
        return a;
    }
};

} // namespace DAS
