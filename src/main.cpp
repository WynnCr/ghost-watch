/*
 * Ghost Watch Daemon
 *
 * A simple, lightweight screen time tracker that parses JSON events from the
 * Niri IPC socket and stores usage stuff in an SQLite database.
 *
 * Author: Amritanshu Kumar <amritrespawned@gmail.com>
 */
#include "../external/json.hpp"
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <map>
#include <sqlite3.h>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <filesystem>
#include <sys/stat.h>
#include <fstream>

using json = nlohmann::json;

// --- CRITICAL FIX: Match the XDG Base Directory path used in dashboard.cpp ---
std::string get_db_path() {
  const char *home = std::getenv("HOME");
  if (!home)
    return "screentime.db"; // Fallback safety

  std::string dir = std::string(home) + "/.local/share/ghost-watch";

  // Ensure the directory exists before SQLite tries to open a file inside it
  std::filesystem::create_directories(dir);

  return dir + "/screentime.db";
}

// database function
sqlite3 *init_database() {
  sqlite3 *db;

  std::string db_file = get_db_path();

  if (sqlite3_open(db_file.c_str(), &db)) {
    std::cerr << "[ERROR] Cannot open database: " << sqlite3_errmsg(db)
              << std::endl;
    return nullptr;
  }

  const char *sql = "CREATE TABLE IF NOT EXISTS window_usage ("
                    "id INTEGER PRIMARY KEY AUTOINCREMENT, "
                    "app_id TEXT, "
                    "window_title TEXT, "
                    "duration_seconds INTEGER, "
                    "timestamp DATETIME DEFAULT CURRENT_TIMESTAMP);";

  char *err_msg = 0;
  if (sqlite3_exec(db, sql, 0, 0, &err_msg) != SQLITE_OK) {
    std::cerr << "[ERROR] SQL error: " << err_msg << std::endl;
    sqlite3_free(err_msg);
  } else {
    // Enable WAL mode and busy timeout to allow TUI continuous reads concurrently
    sqlite3_exec(db, "PRAGMA journal_mode=WAL;", 0, 0, 0);
    sqlite3_busy_timeout(db, 5000);

    std::cout << "[SUCCESS] SQLite Database initialized (" << db_file << ")!"
              << std::endl;
  }
  return db;
}

int main() {
  // Init database
  sqlite3 *db = init_database();
  if (!db)
    return 1;

  // Connect to niri socket
  const char *socket_path = std::getenv("NIRI_SOCKET");
  if (!socket_path) {
    std::cerr << "[ERROR] NIRI_SOCKET not found." << std::endl;
    return 1;
  }

  // Socket init
  int sock = socket(AF_UNIX, SOCK_STREAM, 0);
  if (sock == -1) {
    perror("socket");
    return 1;
  }

  // Address setup
  struct sockaddr_un addr = {};
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

  // Socket connect
  if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
    perror("connect");
    return 1;
  }
  std::cout << "[SUCCESS] Connected to Niri IPC!" << std::endl;

  // Niri Handshake
  std::string subscribe_msg = "\"EventStream\"\n";
  if (write(sock, subscribe_msg.c_str(), subscribe_msg.length()) == -1) {
    perror("write");
    return 1;
  }
  std::cout << "[SUCCESS] EventStream requested!" << std::endl;

  // Set timeout to poll for idle detection
  struct timeval tv;
  tv.tv_sec = 2;
  tv.tv_usec = 0;
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));

  // State trackers
  int current_focused_id = -1;
  std::string current_app = "None";
  std::string current_title = "None";
  auto focus_start_time = std::chrono::steady_clock::now();
  std::map<int, std::pair<std::string, std::string>> window_directory;
  bool was_idle = false;

  auto log_current_focus = [&](int duration) {
    if (current_focused_id != -1 && duration > 0 && current_app != "None") {
      std::cout << "[LOGGED] " << current_app << " for " << duration << "s" << std::endl;

      std::string sql =
          "INSERT INTO window_usage (app_id, window_title, duration_seconds) VALUES (?, ?, ?);";
      sqlite3_stmt *stmt;
      sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);

      sqlite3_bind_text(stmt, 1, current_app.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 2, current_title.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_int(stmt, 3, duration);

      sqlite3_step(stmt);
      sqlite3_finalize(stmt);
    }
  };

  auto sync_focus_state = [&]() {
    std::ofstream app_file("/tmp/ghost-watch-current-app");
    if (app_file) {
      if (was_idle || current_focused_id == -1) {
        app_file << "None\nNone\n";
      } else {
        app_file << current_app << "\n" << current_title << "\n";
      }
      app_file << (long long)time(nullptr) << "\n";
    }
  };

  std::cout << "[LISTENING] Tracking screen time directly to database..." << std::endl;
  char buffer[8192];
  std::string leftover;

  while (true) {
    ssize_t bytes_read = read(sock, buffer, sizeof(buffer) - 1);
    
    if (bytes_read == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      struct stat st;
      bool is_idle = (stat("/tmp/ghost-watch-idle", &st) == 0);
      auto now = std::chrono::steady_clock::now();

      if (is_idle && !was_idle) {
        // System just went idle. Log duration and suspend accumulation.
        int duration = std::chrono::duration_cast<std::chrono::seconds>(now - focus_start_time).count();
        log_current_focus(duration);
        was_idle = is_idle;
        sync_focus_state();
      } else if (!is_idle && was_idle) {
        // System woke up. Reset the start time so the idle period isn't counted.
        focus_start_time = now;
        was_idle = is_idle;
        sync_focus_state();
      } else if (!is_idle && !was_idle) {
        // Actively being used. Flush to DB every 60 seconds to prevent overnight time bleed.
        int duration = std::chrono::duration_cast<std::chrono::seconds>(now - focus_start_time).count();
        if (duration >= 60) {
          log_current_focus(duration);
          focus_start_time = now;
          sync_focus_state();
        }
      }
      
      was_idle = is_idle;
      continue;
    }

    if (bytes_read > 0) {
      buffer[bytes_read] = '\0';
      std::string raw_data = leftover + buffer;
      leftover.clear();

      std::istringstream stream(raw_data);
      std::string line;

      while (std::getline(stream, line)) {
        if (line.empty())
          continue;
        
        // If the string doesn't end with a newline, the last line is incomplete.
        if (stream.eof() && raw_data.back() != '\n') {
          leftover = line;
          break;
        }

        try {
          json event = json::parse(line);

          if (event.contains("WindowsChanged")) {
            bool current_window_changed = false;
            for (auto &window : event["WindowsChanged"]["windows"]) {
              if (window["id"].is_number()) {
                int win_id = window["id"];
                std::string app = window["app_id"].is_string() ? window["app_id"] : "Unknown App";
                std::string title = window["title"].is_string() ? window["title"] : "No Title";
                window_directory[win_id] = {app, title};
                
                if (win_id == current_focused_id) {
                    if (current_app != app || current_title != title) {
                        current_app = app;
                        current_title = title;
                        current_window_changed = true;
                    }
                }
              }
            }
            if (current_window_changed) {
                sync_focus_state();
            }
          }

          if (event.contains("WindowOpenedOrChanged")) {
            auto window = event["WindowOpenedOrChanged"]["window"];
            if (window["id"].is_number()) {
              int win_id = window["id"];
              std::string app = window["app_id"].is_string() ? window["app_id"] : "Unknown App";
              std::string title = window["title"].is_string() ? window["title"] : "No Title";
              
              bool changed = false;
              if (window_directory.count(win_id)) {
                  if (window_directory[win_id].first != app || window_directory[win_id].second != title) {
                      changed = true;
                  }
              } else {
                  changed = true;
              }

              if (changed && win_id == current_focused_id && !was_idle) {
                  auto now = std::chrono::steady_clock::now();
                  int duration = std::chrono::duration_cast<std::chrono::seconds>(now - focus_start_time).count();
                  log_current_focus(duration);
                  focus_start_time = now;
              }

              window_directory[win_id] = {app, title};
              
              if (changed && win_id == current_focused_id) {
                  current_app = app;
                  current_title = title;
                  sync_focus_state();
              }
            }
          }

          if (event.contains("WindowClosed")) {
            auto closed_data = event["WindowClosed"];
            if (closed_data["id"].is_number()) {
              int win_id = closed_data["id"];
              window_directory.erase(win_id);
            }
          }

          if (event.contains("WindowFocusChanged")) {
            auto focus_data = event["WindowFocusChanged"];
            auto now = std::chrono::steady_clock::now();
            int new_id = focus_data["id"].is_number() ? (int)focus_data["id"] : -1;

            if (current_focused_id != -1 && current_focused_id != new_id) {
              if (!was_idle) {
                int duration = std::chrono::duration_cast<std::chrono::seconds>(now - focus_start_time).count();
                log_current_focus(duration);
              }
            }

            current_focused_id = new_id;
            if (current_focused_id != -1 && window_directory.count(current_focused_id)) {
              current_app = window_directory[current_focused_id].first;
              current_title = window_directory[current_focused_id].second;
            } else {
              current_app = "None";
              current_title = "None";
            }
            
            focus_start_time = now;
            sync_focus_state();
          }

        } catch (json::parse_error &e) {
          // ignore broken json
        }
      }
    } else if (bytes_read == 0) {
      std::cout << "[DISCONNECTED] Compositor closed the connection." << std::endl;
      if (!was_idle) {
        auto now = std::chrono::steady_clock::now();
        int duration = std::chrono::duration_cast<std::chrono::seconds>(now - focus_start_time).count();
        log_current_focus(duration);
      }
      break;
    } else {
      perror("read");
      break;
    }
  }

  close(sock);
  sqlite3_close(db);
  return 1; // Return 1 to trigger systemd Restart=on-failure
}
