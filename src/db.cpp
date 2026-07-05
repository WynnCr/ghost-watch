#include "db.hpp"
#include "utils.hpp"
#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>

std::string db_path() {
  const char *home = std::getenv("HOME");
  if (!home)
    return "screentime.db"; // Fallback safety

  std::string dir = std::string(home) + "/.local/share/ghost-watch";
  std::filesystem::create_directories(dir);
  return dir + "/screentime.db";
}

DB::DB(const std::string &path) {
  sqlite3_open(path.c_str(), &db_);
  if (db_) sqlite3_busy_timeout(db_, 5000);
}

DB::~DB() {
  if (db_) sqlite3_close(db_);
}

void DB::query(const std::string &sql, std::function<void(sqlite3_stmt *)> cb,
           std::function<void(sqlite3_stmt *)> bind) {
  sqlite3_stmt *stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
    return;
  if (bind)
    bind(stmt);
  while (sqlite3_step(stmt) == SQLITE_ROW)
    cb(stmt);
  sqlite3_finalize(stmt);
}

std::string DB::str(sqlite3_stmt *s, int col) {
  auto p = reinterpret_cast<const char *>(sqlite3_column_text(s, col));
  return p ? p : "";
}

int DB::integer(sqlite3_stmt *s, int col) {
  return sqlite3_column_int(s, col);
}

DBData fetch_db_data(int history_days) {
  DBData d;
  d.history_days = history_days;
  d.is_idle = is_system_idle();
  for (int i = 0; i < 24; ++i)
    d.hourly[i] = 0;
  DB db(db_path());

  db.query("SELECT cast(strftime('%H',timestamp,'localtime') as integer), "
           "SUM(duration_seconds) FROM window_usage WHERE "
           "date(timestamp,'localtime')=date('now','localtime') GROUP BY 1;",
           [&](sqlite3_stmt *s) {
             int h = DB::integer(s, 0);
             int v = DB::integer(s, 1);
             if (h >= 0 && h <= 23) {
               d.hourly[h] = v;
               d.total_today += v;
             }
           });
           
  db.query("SELECT COALESCE(SUM(duration_seconds),0) FROM window_usage WHERE "
           "date(timestamp,'localtime')=date('now','-1 day','localtime');",
           [&](sqlite3_stmt *s) { d.total_yesterday = DB::integer(s, 0); });
           
  for (auto &[h, v] : d.hourly)
    if (v > d.peak_hour_val) {
      d.peak_hour_val = v;
      d.peak_hour = h;
    }

  db.query("SELECT app_id, SUM(duration_seconds), COUNT(*) FROM window_usage "
           "WHERE date(timestamp,'localtime')=date('now','localtime') GROUP BY "
           "app_id ORDER BY 2 DESC;",
           [&](sqlite3_stmt *s) {
             d.apps.push_back(
                 {DB::str(s, 0), DB::integer(s, 1), DB::integer(s, 2)});
           });

  db.query(
      "SELECT app_id, window_title, SUM(duration_seconds) FROM window_usage "
      "WHERE date(timestamp,'localtime')=date('now','localtime') GROUP BY "
      "app_id, window_title ORDER BY 3 DESC LIMIT 300;",
      [&](sqlite3_stmt *s) {
        d.titles.push_back({DB::str(s, 0), DB::str(s, 1), DB::integer(s, 2)});
      });

  std::ifstream app_file("/tmp/ghost-watch-current-app");
  long long start_time = 0;
  if (app_file) {
    std::getline(app_file, d.active_app);
    std::getline(app_file, d.active_title);
    app_file >> start_time;
  }
  if (d.active_app.empty()) d.active_app = "None";
  if (d.active_title.empty()) d.active_title = "None";

  int live_secs = 0;
  if (d.active_app != "None" && !d.is_idle && start_time > 0) {
      live_secs = std::max(0LL, (long long)time(nullptr) - start_time);
      // The daemon flushes every 60s, so anything beyond 120s means the
      // timestamp file is stale (daemon crashed or was stopped).
      if (live_secs > 120) live_secs = 0;
  }

  if (live_secs > 0) {
      d.total_today += live_secs;
      
      bool found_app = false;
      for (auto &a : d.apps) {
          if (a.name == d.active_app) {
              a.duration += live_secs;
              found_app = true;
              break;
          }
      }
      if (!found_app) {
          d.apps.push_back({d.active_app, live_secs, 1});
      }

      bool found_title = false;
      for (auto &t : d.titles) {
          if (t.app == d.active_app && t.title == d.active_title) {
              t.duration += live_secs;
              found_title = true;
              break;
          }
      }
      if (!found_title) {
          d.titles.push_back({d.active_app, d.active_title, live_secs});
      }

      std::sort(d.apps.begin(), d.apps.end(), [](const AppStat& a, const AppStat& b) {
          return a.duration > b.duration;
      });
      std::sort(d.titles.begin(), d.titles.end(), [](const TitleStat& a, const TitleStat& b) {
          return a.duration > b.duration;
      });

      time_t t = time(0);
      struct tm ltm_data;
      tm *ltm = localtime_r(&t, &ltm_data);
      d.hourly[ltm->tm_hour] += live_secs;
  }

  for (int i = history_days - 1; i >= 0; --i) {
    char buf[32];
    snprintf(buf, sizeof(buf), "-%d days", i);
    std::string offset(buf);
    db.query("SELECT date('now','" + offset +
                 "','localtime'), COALESCE((SELECT SUM(duration_seconds) FROM "
                 "window_usage WHERE date(timestamp,'localtime')=date('now','" +
                 offset + "','localtime')),0);",
             [&](sqlite3_stmt *s) {
               std::string full = DB::str(s, 0);
               std::string label = full.size() >= 10 ? full.substr(5) : full;
               d.history.push_back({label, full, DB::integer(s, 1)});
             });
  }
  
  for (auto &app_stat : d.apps) {
    int streak = 0;
    for (int i = 0; i < 90; ++i) {
      char buf[32];
      snprintf(buf, sizeof(buf), "-%d days", i);
      std::string offset(buf);
      bool used = false;
      db.query(
          "SELECT 1 FROM window_usage WHERE app_id=? AND "
          "date(timestamp,'localtime')=date('now','" +
              offset + "','localtime') LIMIT 1;",
          [&](sqlite3_stmt *) { used = true; },
          [&](sqlite3_stmt *s) {
            sqlite3_bind_text(s, 1, app_stat.name.c_str(), -1,
                              SQLITE_TRANSIENT);
          });
      if (used)
        ++streak;
      else
        break;
    }
    d.streaks[app_stat.name] = streak;
  }
  return d;
}

std::string goals_path() {
  const char *home = std::getenv("HOME");
  return home ? std::string(home) + "/.config/ghost-watch/goals.conf"
              : "goals.conf";
}

std::vector<Goal> load_goals() {
  std::vector<Goal> goals;
  std::ifstream f(goals_path());
  if (!f) return goals;
  std::string line;
  while (std::getline(f, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream ss(line);
    Goal g;
    std::string limit_str;
    std::getline(ss, g.app, '|');
    std::getline(ss, limit_str);
    try {
        g.limit_seconds = std::stoi(limit_str);
    } catch (...) {
        continue;
    }
    goals.push_back(g);
  }
  return goals;
}

void save_goals(const std::vector<Goal> &goals) {
  std::string p = goals_path();
  size_t slash = p.rfind('/');
  if (slash != std::string::npos) {
    std::string dir = p.substr(0, slash);
    std::filesystem::create_directories(dir);
  }
  std::ofstream f(p);
  for (auto &g : goals)
    f << g.app << "|" << g.limit_seconds << "\n";
}
