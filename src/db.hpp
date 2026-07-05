#pragma once
#include <string>
#include <vector>
#include <map>
#include <functional>
#include <sqlite3.h>

struct AppStat {
  std::string name;
  int duration = 0;
  int sessions = 0;
};

struct TitleStat {
  std::string app;
  std::string title;
  int duration = 0;
};

struct DayStat {
  std::string date;
  std::string full;
  int total = 0;
};

struct Goal {
  std::string app;
  int limit_seconds = 0;
  bool notified = false;
};

struct PomodoroState {
  bool active = false;
  bool visible = false;
  bool on_break = false;
  int work_seconds = 25 * 60;
  int break_seconds = 5 * 60;
  int elapsed = 0;
  int sessions_done = 0;
};

struct DBData {
  int total_today = 0;
  int total_yesterday = 0;
  int peak_hour = -1;
  int peak_hour_val = 0;
  std::map<int, int> hourly;
  std::vector<AppStat> apps;
  std::vector<TitleStat> titles;
  std::vector<DayStat> history;
  std::string active_app = "None";
  std::string active_title = "None";
  bool is_idle = false;
  std::map<std::string, int> streaks;
  int history_days = 30;
};

class DB {
public:
  explicit DB(const std::string &path);
  ~DB();
  DB(const DB&) = delete;
  DB& operator=(const DB&) = delete;
  
  void query(const std::string &sql, std::function<void(sqlite3_stmt *)> cb,
             std::function<void(sqlite3_stmt *)> bind = {});

  static std::string str(sqlite3_stmt *s, int col);
  static int integer(sqlite3_stmt *s, int col);

private:
  sqlite3 *db_ = nullptr;
};

std::string db_path();
std::string goals_path();

DBData fetch_db_data(int history_days = 30);
std::vector<Goal> load_goals();
void save_goals(const std::vector<Goal> &goals);
