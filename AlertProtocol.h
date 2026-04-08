#pragma once
// ============================================================
//  AlertProtocol.h  –  Shared wire-protocol definitions
//  Disaster Alert Notification System
// ============================================================

#include <string>
#include <ctime>
#include <map>

namespace DAS {

// ── Disaster severity levels ──────────────────────────────
enum class Severity {
    INFO     = 0,   // General advisories
    LOW      = 1,   // Watch / preparedness
    MEDIUM   = 2,   // Warning – take precautions
    HIGH     = 3,   // Severe – evacuate if advised
    CRITICAL = 4    // Life-threatening emergency
};

inline std::string severityToString(Severity s) {
    switch (s) {
        case Severity::INFO:     return "INFO";
        case Severity::LOW:      return "LOW";
        case Severity::MEDIUM:   return "MEDIUM";
        case Severity::HIGH:     return "HIGH";
        case Severity::CRITICAL: return "CRITICAL";
    }
    return "UNKNOWN";
}

inline Severity severityFromString(const std::string& s) {
    if (s == "LOW")      return Severity::LOW;
    if (s == "MEDIUM")   return Severity::MEDIUM;
    if (s == "HIGH")     return Severity::HIGH;
    if (s == "CRITICAL") return Severity::CRITICAL;
    return Severity::INFO;
}

inline std::string severityColor(Severity s) {
    // ANSI colour codes for terminal output
    switch (s) {
        case Severity::INFO:     return "\033[36m";   // Cyan
        case Severity::LOW:      return "\033[32m";   // Green
        case Severity::MEDIUM:   return "\033[33m";   // Yellow
        case Severity::HIGH:     return "\033[31m";   // Red
        case Severity::CRITICAL: return "\033[1;31m"; // Bold Red
    }
    return "\033[0m";
}

// ── Disaster categories ───────────────────────────────────
enum class DisasterType {
    EARTHQUAKE,
    FLOOD,
    CYCLONE,
    TSUNAMI,
    WILDFIRE,
    LANDSLIDE,
    INDUSTRIAL_ACCIDENT,
    EPIDEMIC,
    DROUGHT,
    OTHER
};

inline std::string disasterTypeToString(DisasterType t) {
    switch (t) {
        case DisasterType::EARTHQUAKE:           return "EARTHQUAKE";
        case DisasterType::FLOOD:                return "FLOOD";
        case DisasterType::CYCLONE:              return "CYCLONE";
        case DisasterType::TSUNAMI:              return "TSUNAMI";
        case DisasterType::WILDFIRE:             return "WILDFIRE";
        case DisasterType::LANDSLIDE:            return "LANDSLIDE";
        case DisasterType::INDUSTRIAL_ACCIDENT:  return "INDUSTRIAL_ACCIDENT";
        case DisasterType::EPIDEMIC:             return "EPIDEMIC";
        case DisasterType::DROUGHT:              return "DROUGHT";
        default:                                 return "OTHER";
    }
}

inline DisasterType disasterTypeFromString(const std::string& s) {
    if (s == "EARTHQUAKE")          return DisasterType::EARTHQUAKE;
    if (s == "FLOOD")               return DisasterType::FLOOD;
    if (s == "CYCLONE")             return DisasterType::CYCLONE;
    if (s == "TSUNAMI")             return DisasterType::TSUNAMI;
    if (s == "WILDFIRE")            return DisasterType::WILDFIRE;
    if (s == "LANDSLIDE")           return DisasterType::LANDSLIDE;
    if (s == "INDUSTRIAL_ACCIDENT") return DisasterType::INDUSTRIAL_ACCIDENT;
    if (s == "EPIDEMIC")            return DisasterType::EPIDEMIC;
    if (s == "DROUGHT")             return DisasterType::DROUGHT;
    return DisasterType::OTHER;
}

inline std::string disasterEmoji(DisasterType t) {
    // ASCII art symbols for terminal
    switch (t) {
        case DisasterType::EARTHQUAKE:          return "[EQ]";
        case DisasterType::FLOOD:               return "[FL]";
        case DisasterType::CYCLONE:             return "[CY]";
        case DisasterType::TSUNAMI:             return "[TS]";
        case DisasterType::WILDFIRE:            return "[WF]";
        case DisasterType::LANDSLIDE:           return "[LS]";
        case DisasterType::INDUSTRIAL_ACCIDENT: return "[IA]";
        case DisasterType::EPIDEMIC:            return "[EP]";
        case DisasterType::DROUGHT:             return "[DR]";
        default:                                return "[??]";
    }
}

// ── Message types on the wire ─────────────────────────────
enum class MessageType {
    ALERT          = 0,   // Broadcast alert from admin
    ACK            = 1,   // Client acknowledges receipt
    HEARTBEAT      = 2,   // Keep-alive ping
    HEARTBEAT_ACK  = 3,   // Server responds to heartbeat
    REGISTER       = 4,   // Client registers with server
    REGISTER_ACK   = 5,   // Server confirms registration
    RETRACT        = 6,   // Admin retracts / cancels alert
    STATUS_REQ     = 7,   // Admin requests delivery stats
    STATUS_RESP    = 8,   // Server returns delivery stats
    SUBSCRIBE      = 9,   // Client subscribes to region/type
    HISTORY_REQ    = 10,  // Client requests alert history
    HISTORY_RESP   = 11,  // Server sends alert history
    ADMIN_LOGIN    = 12,  // Admin authentication
    ADMIN_LOGIN_ACK= 13   // Server confirms admin
};

// ── Core alert structure ──────────────────────────────────
struct Alert {
    std::string  alertId;        // UUID-like unique ID
    DisasterType type;
    Severity     severity;
    std::string  location;       // Human-readable location name
    double       latitude;       // GPS coordinates
    double       longitude;
    std::string  message;        // Detailed description
    std::string  instructions;   // What to do
    std::time_t  timestamp;      // UNIX epoch of creation
    std::time_t  expiryTime;     // When alert expires (0 = never)
    std::string  issuedBy;       // Admin username who issued
    bool         active;         // false = retracted
    std::string  affectedRegion; // Region tag for filtering

    Alert() : latitude(0.0), longitude(0.0),
              timestamp(0), expiryTime(0), active(true),
              type(DisasterType::OTHER),
              severity(Severity::INFO) {}
};

// ── Protocol field delimiters ─────────────────────────────
// Messages are newline-terminated; fields separated by pipe
const char   FIELD_SEP  = '|';
const char   MSG_END    = '\n';
const size_t MAX_MSG_LEN = 8192;

// ── Serialisation helpers ─────────────────────────────────
inline std::string serializeAlert(const Alert& a) {
    // FORMAT: alertId|type|severity|location|lat|lon|message|
    //         instructions|timestamp|expiry|issuedBy|active|region
    return  a.alertId                                   + FIELD_SEP +
            disasterTypeToString(a.type)                + FIELD_SEP +
            severityToString(a.severity)                + FIELD_SEP +
            a.location                                  + FIELD_SEP +
            std::to_string(a.latitude)                  + FIELD_SEP +
            std::to_string(a.longitude)                 + FIELD_SEP +
            a.message                                   + FIELD_SEP +
            a.instructions                              + FIELD_SEP +
            std::to_string(a.timestamp)                 + FIELD_SEP +
            std::to_string(a.expiryTime)                + FIELD_SEP +
            a.issuedBy                                  + FIELD_SEP +
            (a.active ? "1" : "0")                      + FIELD_SEP +
            a.affectedRegion;
}

inline Alert deserializeAlert(const std::string& data) {
    Alert a;
    std::vector<std::string> parts;
    std::string token;
    for (char c : data) {
        if (c == FIELD_SEP) { parts.push_back(token); token.clear(); }
        else token += c;
    }
    parts.push_back(token);

    if (parts.size() >= 13) {
        a.alertId       = parts[0];
        a.type          = disasterTypeFromString(parts[1]);
        a.severity      = severityFromString(parts[2]);
        a.location      = parts[3];
        a.latitude      = std::stod(parts[4]);
        a.longitude     = std::stod(parts[5]);
        a.message       = parts[6];
        a.instructions  = parts[7];
        a.timestamp     = (std::time_t)std::stoll(parts[8]);
        a.expiryTime    = (std::time_t)std::stoll(parts[9]);
        a.issuedBy      = parts[10];
        a.active        = (parts[11] == "1");
        a.affectedRegion= parts[12];
    }
    return a;
}

// ── Build a full wire message ─────────────────────────────
// FORMAT: TYPE|payload\n
inline std::string buildMessage(MessageType type, const std::string& payload) {
    return std::to_string(static_cast<int>(type)) + FIELD_SEP + payload + MSG_END;
}

inline bool parseMessage(const std::string& raw,
                         MessageType& outType,
                         std::string& outPayload) {
    auto pos = raw.find(FIELD_SEP);
    if (pos == std::string::npos) return false;
    outType    = static_cast<MessageType>(std::stoi(raw.substr(0, pos)));
    outPayload = raw.substr(pos + 1);
    // Strip trailing newline
    if (!outPayload.empty() && outPayload.back() == '\n')
        outPayload.pop_back();
    return true;
}

} // namespace DAS
