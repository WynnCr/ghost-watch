#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>
#include <map>
#include <mutex>
#include <set>
#include <sqlite3.h>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>
#include <filesystem>

using namespace ftxui;
struct Theme {
  Color bg = Color::RGB(0, 0, 0);            // Black
  Color fg = Color::RGB(255, 255, 255);      // White Text
  Color primary = Color::RGB(10, 132, 255);  // Blue
  Color secondary = Color::RGB(94, 92, 230); // Indigo
  Color success = Color::RGB(48, 209, 88);   // Green
  Color warning = Color::RGB(255, 159, 10);  // Orange
  Color danger = Color::RGB(255, 69, 58);    // Red
  Color surface0 = Color::RGB(28, 28, 30);   // Background
  Color surface1 = Color::RGB(44, 44, 46);   // Hover Background
  Color surface2 = Color::RGB(58, 58, 60);   // Borders
  Color overlay = Color::RGB(142, 142, 147); // Labels
};
Theme &get_theme() {
  static Theme t;
  return t;
}

Element card_panel(Element content) {
  auto &T = get_theme();
  return vbox({separatorEmpty(), hbox({text("  "), content | flex, text("  ")}),
               separatorEmpty()}) |
         borderRounded | color(T.surface2) | bgcolor(T.surface0);
}

Element section_title(const std::string &title) {
  return hbox({text(title) | bold | color(get_theme().fg), filler()});
}

std::string format_time(int s) {
  if (s <= 0)
    return "0s";
  if (s < 60)
    return std::to_string(s) + "s";
  char buf[64];
  int h = s / 3600, m = (s % 3600) / 60, sc = s % 60;
  if (h > 0)
    snprintf(buf, sizeof(buf), "%dh %dm", h, m);
  else
    snprintf(buf, sizeof(buf), "%dm %ds", m, sc);
  return buf;
}

std::string to_lower(const std::string &s) {
  std::string r = s;
  std::transform(r.begin(), r.end(), r.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return r;
}

std::string get_category(const std::string &app) {
  std::string l = to_lower(app);
  if (l.find("nvim") != std::string::npos ||
      l.find("zed") != std::string::npos ||
      l.find("code") != std::string::npos ||
      l.find("ghostty") != std::string::npos)
    return "Developer Tools";
  if (l.find("firefox") != std::string::npos ||
      l.find("zen") != std::string::npos ||
      l.find("chrome") != std::string::npos ||
      l.find("brave") != std::string::npos)
    return "Web Browser";
  if (l.find("discord") != std::string::npos ||
      l.find("slack") != std::string::npos ||
      l.find("telegram") != std::string::npos)
    return "Social & Comms";
  if (l.find("spotify") != std::string::npos ||
      l.find("mpv") != std::string::npos || l.find("vlc") != std::string::npos)
    return "Media";
  return "System Component";
}

bool is_system_idle() {
  struct stat st;
  return stat("/tmp/ghost-watch-idle", &st) == 0;
}
void send_notification(const std::string &title, const std::string &body) {
  if (std::system(("notify-send -a 'Ghost Watch' '" + title + "' '" + body + "' &")
                  .c_str())) {}
}
std::string db_path() {
  const char *home = std::getenv("HOME");
  if (!home)
    return "screentime.db"; // Fallback safety

  std::string dir = std::string(home) + "/.local/share/ghost-watch";

  // Ensure the directory exists before SQLite tries to open a file inside it
  std::filesystem::create_directories(dir);

  return dir + "/screentime.db";
}

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
  explicit DB(const std::string &path) { 
    sqlite3_open(path.c_str(), &db_); 
    if (db_) sqlite3_busy_timeout(db_, 5000);
  }
  ~DB() {
    if (db_)
      sqlite3_close(db_);
  }
  void query(const std::string &sql, std::function<void(sqlite3_stmt *)> cb,
             std::function<void(sqlite3_stmt *)> bind = {}) {
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
      return;
    if (bind)
      bind(stmt);
    while (sqlite3_step(stmt) == SQLITE_ROW)
      cb(stmt);
    sqlite3_finalize(stmt);
  }
  static std::string str(sqlite3_stmt *s, int col) {
    auto p = reinterpret_cast<const char *>(sqlite3_column_text(s, col));
    return p ? p : "";
  }
  static int integer(sqlite3_stmt *s, int col) {
    return sqlite3_column_int(s, col);
  }

private:
  sqlite3 *db_ = nullptr;
};

DBData fetch(int history_days = 30) {
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
      tm *ltm = localtime(&t);
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
  if (!f)
    return goals;
  std::string line;
  while (std::getline(f, line)) {
    if (line.empty() || line[0] == '#')
      continue;
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

// UI starts
int main() {
  auto &T = get_theme();
  auto screen = ScreenInteractive::Fullscreen();

  DBData data;
  std::vector<Goal> goals = load_goals();
  PomodoroState pomo;
  std::recursive_mutex mtx;

  int active_tab = 0;
  int history_range = 30, range_idx = 2; // Default range is 30d (idx 2)
  int app_sel = 0, title_sel = 0, hist_sel = 0, goal_sel = 0;
  bool show_pomo = false, goal_editing = false;
  std::string app_search, title_search, goal_edit_app, goal_edit_limit;

  std::vector<AppStat> filtered_apps;
  std::vector<std::string> app_menu_entries;
  std::vector<TitleStat> filtered_titles;
  std::vector<std::string> title_menu_entries;
  std::vector<std::string> goal_menu_entries;

  auto reload = [&] { data = fetch(history_range); };
  reload();

  auto reconcile = [&] {
    filtered_apps.clear();
    app_menu_entries.clear();
    std::string lq = to_lower(app_search);
    for (auto &a : data.apps) {
      if (lq.empty() || to_lower(a.name).find(lq) != std::string::npos) {
        filtered_apps.push_back(a);
        app_menu_entries.push_back(a.name);
      }
    }
    app_sel =
        std::clamp(app_sel, 0, std::max(0, (int)filtered_apps.size() - 1));

    filtered_titles.clear();
    title_menu_entries.clear();
    std::string tq = to_lower(title_search);
    for (auto &t : data.titles) {
      if (tq.empty() || (to_lower(t.app) + " " + to_lower(t.title)).find(tq) !=
                            std::string::npos) {
        filtered_titles.push_back(t);
        title_menu_entries.push_back(t.title);
      }
    }
    title_sel =
        std::clamp(title_sel, 0, std::max(0, (int)filtered_titles.size() - 1));

    goal_menu_entries.clear();
    for (auto &g : goals)
      goal_menu_entries.push_back(g.app.empty() ? "Total Screen Time" : g.app);

    hist_sel =
        std::clamp(hist_sel, 0, std::max(0, (int)data.history.size() - 1));
    goal_sel = std::clamp(goal_sel, 0, std::max(0, (int)goals.size() - 1));
  };
  reconcile();

  auto check_goals = [&] {
    std::map<std::string, int> usage;
    for (auto &a : data.apps)
      usage[a.name] = a.duration;
    for (auto &g : goals) {
      int used = g.app.empty() ? data.total_today
                               : (usage.count(g.app) ? usage[g.app] : 0);
      if (!g.notified && used >= g.limit_seconds) {
        g.notified = true;
        send_notification("Screen Time Alert",
                          (g.app.empty() ? "Total Screen Time" : g.app) +
                              " hit limit");
      }
    }
  };

  InputOption input_opt;
  input_opt.on_change = [&] {
    std::lock_guard<std::recursive_mutex> lk(mtx);
    reconcile();
  };
  auto app_search_input = Input(&app_search, "Search Apps...", input_opt);
  auto title_search_input = Input(&title_search, "Search Titles...", input_opt);
  auto goal_app_input = Input(&goal_edit_app, "App (empty=total)");
  auto goal_limit_input = Input(&goal_edit_limit, "Limit (mins)");

  MenuOption app_menu_opt;
  app_menu_opt.entries_option.transform = [&](const EntryState &s) {
    if (s.index >= (int)filtered_apps.size())
      return text("");
    bool sel = s.state;
    bool foc = s.focused;
    auto &a = filtered_apps[s.index];
    auto row = hbox({text(sel ? " ▶ " : "   "),
                     text(a.name) | (sel || foc ? bold : nothing) | flex,
                     text(format_time(a.duration)) | (sel || foc ? bold : nothing),
                     text(" ")});
    Color bg = (sel && foc) ? T.primary : (foc ? T.surface2 : T.surface1);
    return row | color(sel || foc ? T.fg : T.overlay) | 
           (sel || foc ? bgcolor(bg) : nothing);
  };
  auto app_menu = Menu(&app_menu_entries, &app_sel, app_menu_opt);

  MenuOption title_menu_opt;
  title_menu_opt.entries_option.transform = [&](const EntryState &s) {
    if (s.index >= (int)filtered_titles.size())
      return text("");
    bool sel = s.state;
    bool foc = s.focused;
    auto &t = filtered_titles[s.index];
    std::string display = t.title.empty() ? "(Unknown Window)" : t.title;
    if ((int)display.size() > 50)
      display = display.substr(0, 47) + "...";
    auto row = hbox({text(sel ? " ▶ " : "   "),
                     text(t.app) | size(WIDTH, EQUAL, 16),
                     text(display) | (sel || foc ? bold : nothing) | flex,
                     text(format_time(t.duration)) | (sel || foc ? bold : nothing),
                     text(" ")});
    Color bg = (sel && foc) ? T.primary : (foc ? T.surface2 : T.surface1);
    return row | color(sel || foc ? T.fg : T.overlay) | 
           (sel || foc ? bgcolor(bg) : nothing);
  };
  auto title_menu = Menu(&title_menu_entries, &title_sel, title_menu_opt);

  MenuOption goal_menu_opt;
  goal_menu_opt.entries_option.transform = [&](const EntryState &s) {
    if (s.index >= (int)goals.size())
      return text("");
    bool sel = s.state;
    bool foc = s.focused;
    auto &g = goals[s.index];
    std::map<std::string, int> usage;
    for (auto &a : data.apps)
      usage[a.name] = a.duration;
    int used = g.app.empty() ? data.total_today
                             : (usage.count(g.app) ? usage[g.app] : 0);
    float pct =
        g.limit_seconds > 0 ? std::min(1.f, (float)used / g.limit_seconds) : 0;
    bool over = used >= g.limit_seconds;
    Color bar_col = over ? T.danger : pct > 0.75f ? T.warning : T.primary;
    bool active = sel || foc;

    auto row = vbox(
        {hbox({text(sel ? " ▶ " : "   ") | color(active ? T.fg : T.primary),
               text(g.app.empty() ? "Total Screen Time" : g.app) | bold |
                   color(active ? T.fg : T.overlay) | flex,
               text(format_time(used) + " / " + format_time(g.limit_seconds)) |
                   color(active ? T.fg : (over ? T.danger : T.overlay)),
               text(over ? " OVER " : " ") | bold | color(active ? T.warning : T.danger),
               text(" ")}),
         separatorEmpty(),
         hbox({text("   "),
               gauge(pct) | color(active ? T.fg : bar_col) | size(HEIGHT, EQUAL, 1) | flex,
               text(" " + std::to_string((int)(pct * 100)) + "%") | dim |
                   color(active ? T.fg : T.overlay),
               text(" ")}),
         separatorEmpty()});
    Color bg = (sel && foc) ? T.primary : (foc ? T.surface2 : T.surface1);
    return row | (sel || foc ? bgcolor(bg) : nothing);
  };
  auto goal_menu = Menu(&goal_menu_entries, &goal_sel, goal_menu_opt);

  std::vector<std::string> range_labels = {" 7d ", " 14d ", " 30d ", " 90d "};
  auto range_toggle = Toggle(&range_labels, &range_idx);

  auto render_dashboard = [&]() -> Element {
    int delta = data.total_today - data.total_yesterday;
    std::string delta_str =
        (delta >= 0 ? "+" : "") + format_time(std::abs(delta));
    Color delta_col = delta >= 0 ? T.overlay : T.success;

    auto stat_block = [&](const std::string &label, const std::string &val,
                          Color c) {
      return vbox({text(label) | dim | color(T.overlay), separatorEmpty(),
                   text(val) | bold | color(c)}) |
             flex;
    };

    auto top_metrics = card_panel(hbox({
        stat_block("SCREEN TIME", format_time(data.total_today), T.fg),
        stat_block("VS YESTERDAY", delta_str, delta_col),
        stat_block("PEAK HOUR",
                   data.peak_hour >= 0 ? std::to_string(data.peak_hour) + ":00"
                                       : "--",
                   T.primary),
        stat_block("CURRENT WINDOW", data.active_app, T.fg),
    }));

    Elements hourly_bars;
    int mx = 1;
    for (auto &[hr, v] : data.hourly)
      mx = std::max(mx, v);
    for (int i = 0; i <= 23; ++i) {
      float r = mx > 0 ? (float)data.hourly[i] / mx : 0;
      hourly_bars.push_back(vbox({gaugeUp(r) | color(T.primary) | flex}) |
                            size(WIDTH, EQUAL, 3));
      if (i < 23)
        hourly_bars.push_back(text(" ")); // Strict separator
    }

    Elements hour_labels;
    for (int h = 0; h <= 23; h += 4)
      hour_labels.push_back(text(std::to_string(h) + ":00") | dim |
                            color(T.overlay) | size(WIDTH, EQUAL, 16));

    auto timeline =
        card_panel(
            vbox({section_title("24H Activity"), separatorEmpty(),
                  hbox(std::move(hourly_bars)) | hcenter | flex,
                  separatorEmpty(), hbox(std::move(hour_labels)) | hcenter})) |
        size(HEIGHT, EQUAL, 14);

    Elements app_rows;
    int show = std::min((int)data.apps.size(), 5);
    for (int i = 0; i < show; ++i) {
      float pct = data.total_today > 0
                      ? (float)data.apps[i].duration / data.total_today
                      : 0;
      app_rows.push_back(hbox({
          text(data.apps[i].name) | bold | color(T.fg) | flex,
          text(format_time(data.apps[i].duration)) | color(T.overlay),
      }));
      app_rows.push_back(gauge(pct) | color(T.primary) |
                         size(HEIGHT, EQUAL, 1));
      app_rows.push_back(separatorEmpty());
    }

    auto bottom_split = hbox(
        {card_panel(vbox({section_title("Most Used"), separatorEmpty(),
                          vbox(std::move(app_rows)) | flex})) |
             flex,
         text("  "),
         card_panel(vbox(
             {section_title("Insights"), separatorEmpty(),
              text("You've completed ") | color(T.overlay),
              text(std::to_string(pomo.sessions_done) + " focus sessions") |
                  bold | color(T.success),
              separatorEmpty(), text("Longest streak:") | color(T.overlay),
              text((data.apps.empty() ? "None" : data.apps[0].name) + " (" +
                   std::to_string(data.apps.empty()
                                      ? 0
                                      : data.streaks[data.apps[0].name]) +
                   " days)") |
                  bold | color(T.warning)})) |
             flex});

    return vbox({top_metrics, timeline, bottom_split | flex}) | flex;
  };

  auto render_apps = [&]() -> Element {
    auto list = card_panel(vbox({
        app_search_input->Render() | color(T.fg) | borderEmpty |
            bgcolor(app_search_input->Focused() ? T.primary : T.surface1),
        separatorLight() | color(T.surface2),
        app_menu->Render() | vscroll_indicator | yframe | flex,
    }));
    Element inspector;
    if (filtered_apps.empty()) {
      inspector = text(" Select an item to view details. ") | dim |
                  color(T.overlay) | hcenter | vcenter | flex;
    } else {
      auto &a = filtered_apps[app_sel];
      int pct =
          data.total_today > 0 ? (a.duration * 100) / data.total_today : 0;
      auto stat_block = [&](const std::string &lbl, const std::string &val,
                            Color c) {
        return vbox({text(lbl) | dim | color(T.overlay), separatorEmpty(),
                     text(val) | bold | color(c)}) |
               flex;
      };

      Elements window_rows;
      for (const auto &t : data.titles) {
        if (t.app == a.name) {
          window_rows.push_back(
              hbox({text(" • " + (t.title.empty() ? "(Background)" : t.title)) |
                        dim | color(T.overlay) | flex,
                    text(format_time(t.duration)) | color(T.primary)}));
          window_rows.push_back(separatorEmpty());
        }
      }
      if (window_rows.empty())
        window_rows.push_back(text(" No specific windows recorded.") | dim |
                              color(T.overlay));

      inspector =
          vbox({hbox({vbox({text(a.name) | bold | color(T.fg),
                            text(get_category(a.name)) | dim |
                                color(T.overlay)}) |
                          flex,
                      text(format_time(a.duration)) | bold | color(T.primary)}),
                separatorLight() | color(T.surface2), separatorEmpty(),
                hbox({stat_block("SHARE OF DAY", std::to_string(pct) + "%",
                                 T.fg),
                      stat_block("SESSIONS", std::to_string(a.sessions), T.fg),
                      stat_block("CURRENT STREAK",
                                 std::to_string(data.streaks.count(a.name)
                                                    ? data.streaks.at(a.name)
                                                    : 0) +
                                     " days",
                                 T.warning)}),
                separatorEmpty(), separatorLight() | color(T.surface2),
                separatorEmpty(), section_title("Associated Windows"),
                separatorEmpty(),
                vbox(std::move(window_rows)) | vscroll_indicator | yframe |
                    flex}) |
          flex;
    }
    return hbox({list | size(WIDTH, EQUAL, 40), text(" "),
                 card_panel(inspector) | flex}) |
           flex;
  };

  auto render_titles = [&]() -> Element {
    auto list = card_panel(vbox({
        title_search_input->Render() | color(T.fg) | borderEmpty |
            bgcolor(title_search_input->Focused() ? T.primary : T.surface1),
        separatorLight() | color(T.surface2),
        title_menu->Render() | vscroll_indicator | yframe | flex,
    }));
    Element inspector;
    if (filtered_titles.empty()) {
      inspector = text(" Select an item to view details. ") | dim |
                  color(T.overlay) | hcenter | vcenter | flex;
    } else {
      auto &t = filtered_titles[title_sel];
      inspector = vbox({text(t.title.empty() ? "(Unknown Window)" : t.title) |
                            bold | color(T.fg),
                        text("App: " + t.app) | dim | color(T.overlay),
                        separatorLight() | color(T.surface2), separatorEmpty(),
                        text("Time Spent: " + format_time(t.duration)) | bold |
                            color(T.primary)}) |
                  flex;
    }
    return hbox({list | size(WIDTH, EQUAL, 55), text(" "),
                 card_panel(inspector) | flex}) |
           flex;
  };

  auto render_goals = [&]() -> Element {
    auto list = card_panel(vbox({
                    section_title("Configured Limits"),
                    separatorEmpty(),
                    goal_menu->Render() | vscroll_indicator | yframe | flex,
                    separatorEmpty(),
                    separatorLight() | color(T.surface2),
                    separatorEmpty(),
                    hbox({
                        text(" [a] Add   [d] Delete ") | dim | color(T.overlay),
                    }) | hcenter,
                })) |
                flex;
    Element edit_panel = text("") | size(WIDTH, EQUAL, 0);
    if (goal_editing) {
      edit_panel =
          card_panel(vbox({
              section_title("Add New Limit"),
              separatorEmpty(),
              text("Target App (empty = total):") | dim | color(T.overlay),
              goal_app_input->Render() | borderEmpty |
                  bgcolor(goal_app_input->Focused() ? T.primary : T.surface1),
              separatorEmpty(),
              text("Time Limit (minutes):") | dim | color(T.overlay),
              goal_limit_input->Render() | borderEmpty |
                  bgcolor(goal_limit_input->Focused() ? T.primary : T.surface1),
              separatorEmpty(),
              filler(),
              text("[Enter] Save   [Esc] Cancel") | dim | color(T.overlay) |
                  hcenter,
          })) |
          size(WIDTH, EQUAL, 36);
    }
    return hbox({list, text("  "), edit_panel}) | flex;
  };

  auto render_history = [&]() -> Element {
    int target_range = (range_idx == 0)   ? 7
                       : (range_idx == 1) ? 14
                       : (range_idx == 2) ? 30
                                          : 90;
    if (target_range != history_range) {
      history_range = target_range;
      reload();
      reconcile();
    }

    Elements bars;
    int mx = 1;
    for (auto &d : data.history)
      mx = std::max(mx, d.total);
    for (int i = 0; i < (int)data.history.size(); ++i) {
      bool sel = i == hist_sel;
      auto &d = data.history[i];
      float r = mx > 0 ? (float)d.total / mx : 0;
      int N = data.history.size() > 30 ? 7 : data.history.size() > 14 ? 3 : 1;
      std::string lbl = (i % N == 0 || sel) ? d.date : "";

      bars.push_back(
          vbox({
              vbox({gaugeUp(r) | flex | color(sel ? T.fg : T.primary)}) |
                  size(WIDTH, EQUAL, 3) | hcenter | flex,
              separatorEmpty(),
              text(lbl) | hcenter | dim | color(T.overlay) |
                  size(WIDTH, EQUAL, 5),
          }) |
          flex | (sel ? bgcolor(T.surface1) : nothing));

      if (i < (int)data.history.size() - 1)
        bars.push_back(text(" "));
    }

    auto graph_panel = card_panel(
        vbox({hbox({section_title("Historical Trends"), filler(),
                    range_toggle->Render() | color(T.fg)}),
              separatorEmpty(), hbox(std::move(bars)) | hcenter | flex}));

    Element detail = text("") | flex;
    if (!data.history.empty()) {
      auto &sel_day = data.history[hist_sel];
      std::vector<std::pair<std::string, int>> day_apps;
      DB db(db_path());
      db.query(
          "SELECT app_id, SUM(duration_seconds) FROM window_usage WHERE "
          "date(timestamp,'localtime')=? GROUP BY app_id ORDER BY 2 DESC LIMIT "
          "10;",
          [&](sqlite3_stmt *s) {
            day_apps.push_back({DB::str(s, 0), DB::integer(s, 1)});
          },
          [&](sqlite3_stmt *s) {
            sqlite3_bind_text(s, 1, sel_day.full.c_str(), -1, SQLITE_TRANSIENT);
          });

      Elements drows;
      for (auto &[app, dur] : day_apps) {
        float p = sel_day.total > 0 ? (float)dur / sel_day.total : 0;
        drows.push_back(hbox({
            text(app) | bold | color(T.fg) | flex,
            text(" " + format_time(dur)) | color(T.overlay) |
                size(WIDTH, EQUAL, 10),
        }));
        drows.push_back(gauge(p) | color(T.primary) | size(HEIGHT, EQUAL, 1));
        drows.push_back(separatorEmpty());
      }
      if (drows.empty())
        drows.push_back(text(" No data for this day.") | dim |
                        color(T.overlay));

      detail =
          card_panel(vbox({
              text(" " + sel_day.full + " ") | bold | hcenter | color(T.fg),
              text("Total Time: " + format_time(sel_day.total)) | dim |
                  color(T.overlay) | hcenter,
              separatorLight() | color(T.surface2),
              separatorEmpty(),
              vbox(std::move(drows)) | flex,
          })) |
          size(WIDTH, EQUAL, 42);
    }
    return hbox({graph_panel | flex, text("  "), detail}) | flex;
  };

  // Pomodoro
  auto render_pomo = [&]() -> Element {
    if (!show_pomo)
      return text("") | size(WIDTH, EQUAL, 0) | size(HEIGHT, EQUAL, 0);

    int total = pomo.on_break ? pomo.break_seconds : pomo.work_seconds;
    int remain = std::max(0, total - pomo.elapsed);
    float prog = total > 0 ? (float)pomo.elapsed / total : 0;
    int mm = remain / 60, ss = remain % 60;
    char tbuf[16];
    snprintf(tbuf, sizeof(tbuf), "%02d:%02d", mm, ss);

    Color bar_col = pomo.on_break ? T.success : T.primary;
    std::string phase = pomo.on_break ? " BREAK " : " FOCUS ";

    return vbox({
               text(" POMODORO ") | bold | hcenter | color(T.overlay),
               separatorEmpty(),
               separatorEmpty(),
               text(phase) | bold | hcenter | color(bar_col),
               separatorEmpty(),
               text(tbuf) | bold | hcenter | color(T.fg) |
                   size(HEIGHT, EQUAL, 1),
               separatorEmpty(),
               separatorEmpty(),
               gauge(prog) | color(bar_col) | size(HEIGHT, EQUAL, 1),
               separatorEmpty(),
               text("Sessions: " + std::to_string(pomo.sessions_done)) | dim |
                   color(T.overlay) | hcenter,
               separatorEmpty(),
               separatorLight() | color(T.surface2),
               separatorEmpty(),
               text(pomo.active ? "[Space] Pause" : "[Space] Resume") | dim |
                   color(T.overlay) | hcenter,
               text("[r] Reset   [F2] Hide") | dim | color(T.overlay) | hcenter,
           }) |
           borderRounded | color(T.surface2) | bgcolor(T.surface0) |
           size(WIDTH, EQUAL, 35) | center;
  };

  // Routing
  std::vector<std::string> tab_labels = {" Dashboard ", " Apps ", " Windows ",
                                         " Limits ", " History "};
  
  MenuOption tab_opt = MenuOption::Horizontal();
  tab_opt.entries_option.transform = [&](const EntryState &s) {
    bool sel = s.state;
    bool foc = s.focused;
    Color bg = (sel && foc) ? T.primary : (foc ? T.surface2 : T.surface1);
    return text(s.label) | (sel || foc ? bold : nothing) |
           color(sel || foc ? T.fg : T.overlay) |
           (sel || foc ? bgcolor(bg) : nothing) |
           (sel ? underlined : nothing);
  };
  auto tab_toggle = Menu(&tab_labels, &active_tab, tab_opt);

  auto apps_container = Container::Vertical({app_search_input, app_menu});
  auto titles_container = Container::Vertical({title_search_input, title_menu});
  auto goals_container = Container::Vertical({
      Maybe(Container::Vertical({goal_app_input, goal_limit_input}), &goal_editing),
      goal_menu
  });
  auto history_container = Container::Vertical({range_toggle});

  auto tabs_container =
      Container::Tab({Container::Vertical({}), apps_container, titles_container,
                      goals_container, history_container},
                     &active_tab);
  auto main_layout = Container::Vertical({tab_toggle, tabs_container});

  auto root_renderer = Renderer(main_layout, [&]() -> Element {
    std::lock_guard<std::recursive_mutex> lk(mtx);
    reconcile();

    if (active_tab != 4) {
      range_idx = history_range == 7    ? 0
                  : history_range == 14 ? 1
                  : history_range == 30 ? 2
                                        : 3;
    }

    auto header = hbox({
        text("  Ghost Watch") | bold | color(T.fg),
        filler(),
        tab_toggle->Render() | color(T.fg),
        filler(),
        text(data.is_idle ? " IDLE  " : " LIVE  ") | bold |
            color(data.is_idle ? T.warning : T.success),
    });

    Element body;
    switch (active_tab) {
    case 0:
      body = render_dashboard();
      break;
    case 1:
      body = render_apps();
      break;
    case 2:
      body = render_titles();
      break;
    case 3:
      body = render_goals();
      break;
    case 4:
      body = render_history();
      break;
    default:
      body = text("") | flex;
    }

    std::string keys_by_tab[] = {
        " [1-5] Tabs   [↑/↓] Navigate   [F2] Pomodoro   [q] Quit ",
        " [1-5] Tabs   [↑/↓] Navigate   [F2] Pomodoro   [q] Quit ",
        " [1-5] Tabs   [↑/↓] Navigate   [F2] Pomodoro   [q] Quit ",
        " [a] Add Limit   [d] Delete   [F2] Pomodoro   [q] Quit ",
        " [←/→] Navigate Days   [F2] Pomodoro   [q] Quit ",
    };

    auto main_ui = vbox({separatorEmpty(), header, separatorEmpty(),
                         body | flex, separatorEmpty(),
                         hbox({text(keys_by_tab[std::clamp(active_tab, 0, 4)]) |
                               dim | color(T.overlay) | hcenter}),
                         separatorEmpty()}) |
                   flex | bgcolor(T.bg);

    if (show_pomo)
      return dbox({main_ui, render_pomo()});
    return main_ui;
  });

  class FallbackEvent : public ComponentBase {
  public:
    FallbackEvent(Component child, std::function<bool(Event)> handler)
        : child_(child), handler_(handler) {
      Add(child_);
    }
    bool OnEvent(Event e) override {
      if (child_->OnEvent(e)) return true;
      return handler_(e);
    }
  private:
    Component child_;
    std::function<bool(Event)> handler_;
  };

  // Event Handler
  auto event_handler = Make<FallbackEvent>(root_renderer, [&](Event e) -> bool {
    std::lock_guard<std::recursive_mutex> lk(mtx);

    if (e == Event::Escape || (e == Event::Character('q') && !goal_editing)) {
      if (goal_editing) {
        goal_editing = false;
        return true;
      }
      screen.Exit();
      return true;
    }

    if (e == Event::Character('1')) { active_tab = 0; return true; }
    if (e == Event::Character('2')) { active_tab = 1; return true; }
    if (e == Event::Character('3')) { active_tab = 2; return true; }
    if (e == Event::Character('4')) { active_tab = 3; return true; }
    if (e == Event::Character('5')) { active_tab = 4; return true; }

    if (e == Event::F2) {
      show_pomo = !show_pomo;
      return true;
    }
    if (show_pomo && e == Event::Character(' ')) {
      pomo.active = !pomo.active;
      return true;
    }
    if (show_pomo && e == Event::Character('r')) {
      pomo.active = false;
      pomo.elapsed = 0;
      pomo.on_break = false;
      return true;
    }

    if (active_tab == 4) {
      if (e == Event::ArrowRight && hist_sel < (int)data.history.size() - 1) {
        hist_sel++;
        return true;
      }
      if (e == Event::ArrowLeft && hist_sel > 0) {
        hist_sel--;
        return true;
      }
    }

    if (active_tab == 3 && !goal_editing) {
      if (e == Event::Character('a')) {
        goal_edit_app = "";
        goal_edit_limit = "";
        goal_editing = true;
        return true;
      }
      if (e == Event::Character('d') && !goals.empty()) {
        goals.erase(goals.begin() + goal_sel);
        save_goals(goals);
        goal_sel = std::clamp(goal_sel, 0, std::max(0, (int)goals.size() - 1));
        return true;
      }
    }
    if (goal_editing && e == Event::Return) {
      try {
        goals.push_back(
            {goal_edit_app, std::stoi(goal_edit_limit) * 60, false});
        save_goals(goals);
      } catch (...) {
      }
      goal_editing = false;
      return true;
    }
    return false;
  });

  // Threads
  std::atomic<bool> running{true};

  std::thread refresh_thread([&] {
    while (running) {
      for (int i = 0; i < 50 && running; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      if (!running)
        break;
      auto fresh = fetch(history_range);
      {
        std::lock_guard<std::recursive_mutex> lk(mtx);
        data = fresh;
        check_goals();
      }
      screen.PostEvent(Event::Custom);
    }
  });

  std::thread pomo_thread([&] {
    while (running) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      if (!running)
        break;
      {
        std::lock_guard<std::recursive_mutex> lk(mtx);

        // Live UI without hammering SQLite
        bool currently_idle = is_system_idle();
        if (!currently_idle && data.active_app != "None") {
          data.total_today++;
          for (auto &a : data.apps) {
            if (a.name == data.active_app) {
              a.duration++;
              break;
            }
          }
          for (auto &t_stat : data.titles) {
            if (t_stat.app == data.active_app && t_stat.title == data.active_title) {
              t_stat.duration++;
              break;
            }
          }
          time_t t = time(0);
          tm *ltm = localtime(&t);
          data.hourly[ltm->tm_hour]++;
        }

        if (pomo.active) {
          pomo.elapsed++;
          int total = pomo.on_break ? pomo.break_seconds : pomo.work_seconds;
          if (pomo.elapsed >= total) {
            pomo.elapsed = 0;
            if (!pomo.on_break) {
              pomo.sessions_done++;
              pomo.on_break = true;
              send_notification("🍅 Focus done!",
                                "Session complete. Take a break.");
            } else {
              pomo.on_break = false;
              send_notification("🌿 Break over!", "Back to work.");
            }
          }
        }
      }
      screen.PostEvent(Event::Custom);
    }
  });

  screen.Loop(event_handler);
  running = false;
  refresh_thread.join();
  pomo_thread.join();
  return 0;
}
