# Disaster-Alert-Notification-System
# Disaster Alert Notification System (DAS)

> A real-time, multi-client disaster alert broadcast system built in **C++17** using the **Poco C++ Libraries**.  
> Supports SDG 11 – Sustainable Cities and Communities.

---

## System Overview

```
┌──────────────────────────────────────────────────────────┐
│                   DAS Architecture                       │
│                                                          │
│  ┌──────────┐         ┌────────────────┐                 │
│  │ das_admin│─────────▶  das_server    │                 │
│  │ (Admin   │  TCP    │  (Multi-thread │                 │
│  │  CLI)    │◀─────────  Poco::TCP)   │                 │
│  └──────────┘         └───────┬────────┘                 │
│                               │  Broadcast               │
│              ┌────────────────┼────────────────┐         │
│              ▼                ▼                ▼         │
│        ┌──────────┐    ┌──────────┐    ┌──────────┐      │
│        │das_client│    │das_client│    │das_client│      │
│        │ (User 1) │    │ (User 2) │    │ (User N) │      │
│        └──────────┘    └──────────┘    └──────────┘      │
└──────────────────────────────────────────────────────────┘
```

---

## Features

### Core Communication
| Feature | Description |
|---|---|
| **Multi-client TCP Server** | Poco::Net::TCPServer handles unlimited simultaneous connections with a thread pool |
| **Real-time Broadcast** | Alerts pushed to all matching clients within milliseconds |
| **Client Registration** | Clients register with username and region on connect |
| **Heartbeat / Keep-alive** | Client pings server every 30s; watchdog drops ghost connections after 2 min silence |

### Alert Management
| Feature | Description |
|---|---|
| **10 Disaster Types** | Earthquake, Flood, Cyclone, Tsunami, Wildfire, Landslide, Industrial Accident, Epidemic, Drought, Other |
| **5 Severity Levels** | INFO → LOW → MEDIUM → HIGH → CRITICAL with colour-coded terminal output |
| **Alert Retraction** | Admin can cancel a live alert; retraction broadcast to all clients |
| **Alert Expiry** | Alerts auto-deactivate at set expiry time (checked every 30s) |
| **GPS Coordinates** | Latitude/longitude embedded in every alert |
| **Affected Region Tags** | Clients subscribe to specific regions (e.g. `SOUTH_INDIA`, `COASTAL_AP`) |

### Filtering & Subscriptions
| Feature | Description |
|---|---|
| **Region Subscriptions** | Clients receive only alerts for their subscribed region |
| **Disaster Type Filter** | Clients can subscribe to specific disaster types (e.g. only FLOOD, EARTHQUAKE) |
| **Dynamic Re-subscribe** | Subscription can be updated mid-session without reconnecting |
| **History Replay** | New clients receive all active, non-expired alerts immediately on join |

### Security & Auth
| Feature | Description |
|---|---|
| **MD5 Admin Auth** | Admin passwords stored and verified as MD5 hashes via Poco::MD5Engine |
| **Role Separation** | Only authenticated admins can broadcast or retract alerts |
| **Access Denial** | Unauthenticated broadcast attempts return DENY response |

### Persistence & Logging
| Feature | Description |
|---|---|
| **JSON Alert Log** | All alerts persisted to `alerts_log.json` on disk |
| **Server Event Log** | Timestamped plain-text log in `server.log` |
| **Client Local Save** | Clients can dump received alerts to `local_alerts.txt` |

### Admin Interface
| Feature | Description |
|---|---|
| **Interactive Wizard** | Step-by-step alert composition menu |
| **Alert Templates** | 5 built-in templates for common regional disasters |
| **Bulk Broadcast** | Load and send multiple alerts from a structured text file |
| **Delivery Statistics** | Live dashboard: how many clients received and acknowledged each alert |

### Client Interface
| Feature | Description |
|---|---|
| **Live Alert Display** | Coloured, formatted alerts appear in real time |
| **Alert Bell** | Terminal bell (×3 for CRITICAL) on receipt |
| **Alert List & Filter** | `list`, `active`, `filter <SEVERITY>` commands |
| **Detail View** | `show <id>` shows full alert with GPS, instructions |
| **Local Save** | `save` writes all cached alerts to file |

---

##  Prerequisites

| Dependency | Version | Install |
|---|---|---|
| **CMake** | ≥ 3.14 | `sudo apt install cmake` |
| **GCC / Clang** | C++17 capable | `sudo apt install build-essential` |
| **Poco C++ Libraries** | Any recent | `sudo apt install libpoco-dev` |

**macOS:**
```bash
brew install cmake poco
```

**Windows:**
```bash
vcpkg install poco
```

---

##  Build Instructions

```bash
# Clone the repository
git clone https://github.com/yourusername/DisasterAlertSystem.git
cd DisasterAlertSystem

# Configure and build
mkdir build && cd build
cmake ..
make -j$(nproc)

# Binaries will be in build/bin/
ls build/bin/
# das_server   das_client   das_admin
```

---

## Running the System

### 1. Start the Server
```bash
./build/bin/das_server 9000
```
Default port is `9000`. The server will create `server.log` and `alerts_log.json` in the working directory.

### 2. Connect Clients (multiple terminals)
```bash
./build/bin/das_client 127.0.0.1 9000
```
Enter your username and region (e.g. `SOUTH_INDIA`) when prompted.

**Client commands:**
```
list              – List all received alerts
show <id>         – Show full details of an alert
filter HIGH       – Show only HIGH severity alerts
active            – Show currently active alerts
subscribe COASTAL_AP FLOOD,CYCLONE   – Update subscription
history           – Request alert history from server
save              – Save alerts to local_alerts.txt
help              – Command reference
quit              – Exit
```

### 3. Launch Admin Panel
```bash
./build/bin/das_admin 127.0.0.1 9000
```
Default credentials:
| Username | Password |
|---|---|
| `admin` | `admin123` |
| `root` | `disaster2024` |

**Admin menu:**
```
1 – Broadcast New Alert (interactive wizard)
2 – Use Alert Template (5 pre-built templates)
3 – Retract Alert
4 – View Delivery Statistics
5 – Broadcast Alerts from File
6 – Exit
```

### 4. Bulk Broadcast from File
```bash
# Edit sample_alerts.txt with your alerts, then:
# In admin menu, choose 5 and enter path: ../sample_alerts.txt
```

---

## Wire Protocol

All messages are newline-terminated (`\n`) pipe-delimited strings.

```
MESSAGE := TYPE_INT|PAYLOAD\n

ALERT PAYLOAD := alertId|type|severity|location|lat|lon|message|
                  instructions|timestamp|expiryTime|issuedBy|active|region
```

### Message Types
| Code | Name | Direction |
|---|---|---|
| 0 | ALERT | Server → Clients |
| 1 | ACK | Client → Server |
| 2 | HEARTBEAT | Client → Server |
| 3 | HEARTBEAT_ACK | Server → Client |
| 4 | REGISTER | Client → Server |
| 5 | REGISTER_ACK | Server → Client |
| 6 | RETRACT | Server → Clients |
| 7 | STATUS_REQ | Admin → Server |
| 8 | STATUS_RESP | Server → Admin |
| 9 | SUBSCRIBE | Client → Server |
| 10 | HISTORY_REQ | Client → Server |
| 11 | HISTORY_RESP | Server → Client |
| 12 | ADMIN_LOGIN | Admin → Server |
| 13 | ADMIN_LOGIN_ACK | Server → Admin |

---

## Project Structure

```
DisasterAlertSystem/
├── CMakeLists.txt
├── README.md
├── sample_alerts.txt
├── common/
│   ├── AlertProtocol.h    # Wire protocol, types, serialisation
│   ├── AlertLogger.h      # JSON + plaintext logging
│   └── Utils.h            # ID gen, formatting, printing
├── server/
│   ├── server.cpp         # Main server (TCPServer + broadcast)
│   └── admin.cpp          # Admin control CLI
└── client/
    └── client.cpp         # Client with receive thread + commands
```

---

## SDG 11 Alignment

This project supports **UN Sustainable Development Goal 11: Sustainable Cities and Communities** by:

- Enabling **rapid, simultaneous dissemination** of disaster alerts to large populations
- Supporting **region-based targeting** to reach only affected communities
- Providing **actionable instructions** alongside every alert
- Logging all events for **post-disaster analysis and accountability**
- Using **open-source, cross-platform** technology deployable in resource-constrained environments

---

## Future Enhancements

- [ ] TLS/SSL encrypted connections (Poco::Net::SecureStreamSocket)
- [ ] SMS gateway integration (Twilio REST API via Poco::Net::HTTPSClientSession)
- [ ] Web dashboard (Poco::Net::HTTPServer serving JSON API)
- [ ] GIS map integration (GeoJSON alert overlays)
- [ ] Multi-language alert messages
- [ ] Android/iOS push notification relay
- [ ] Database backend (SQLite via Poco::Data)

---

## License

MIT License – see [LICENSE](LICENSE) for details.

---

## Author

Built as a networking project demonstrating Poco C++ Libraries in disaster management.  
Uses: `Poco::Net::TCPServer`, `Poco::Net::StreamSocket`, `Poco::FastMutex`,  
`Poco::MD5Engine`, `Poco::UUIDGenerator`, `Poco::DateTimeFormatter`.
