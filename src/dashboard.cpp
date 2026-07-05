#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <map>
#include <string>
#include <thread>
#include <vector>
#include <fstream>

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

using namespace ftxui;
#include "theme.hpp"
#include "utils.hpp"
#include "db.hpp"
#include "components.hpp"

// UI starts
int main() {
  auto &T = get_theme();
  auto screen = ScreenInteractive::Fullscreen();

  DBData data;
  std::vector<Goal> goals = load_goals();
  PomodoroState pomo;

  int active_tab = 0;
  std::atomic<int> history_range{30};
  int range_idx = 2; // Default range is 30d (idx 2)
  int app_sel = 0, title_sel = 0, hist_sel = 0, goal_sel = 0;
  bool show_pomo = false, goal_editing = false;
  std::string app_search, title_search, goal_edit_app, goal_edit_limit;

  std::vector<AppStat> filtered_apps;
  std::vector<std::string> app_menu_entries;
  std::vector<TitleStat> filtered_titles;
  std::vector<std::string> title_menu_entries;
  std::vector<std::string> goal_menu_entries;

  auto reload = [&] { data = fetch_db_data(history_range); };
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
    std::string arrow = delta > 0 ? "▲ " : delta < 0 ? "▼ " : "  ";
    std::string delta_str = arrow + format_time(std::abs(delta));
    // Less than yesterday = green (good), more = warning (bad)
    Color delta_col = delta > 0 ? T.warning : delta < 0 ? T.success : T.overlay;

    auto stat_block = [&](const std::string &label, const std::string &val,
                          Color c) {
      return vbox({text(label) | dim | color(T.overlay), separatorEmpty(),
                   text(val) | bold | color(c)}) |
             flex;
    };

    auto top_metrics = card_panel(hbox({
        stat_block("SCREEN TIME", format_time(data.total_today), T.fg),
        stat_block("YESTERDAY", format_time(data.total_yesterday), T.overlay),
        stat_block("VS YESTERDAY", delta_str, delta_col),
        stat_block("PEAK HOUR",
                   data.peak_hour >= 0 ? std::to_string(data.peak_hour) + ":00"
                                       : "--",
                   T.primary),
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
      // Defer reload to avoid blocking the render thread
      screen.Post([&]() {
        reload();
        reconcile();
      });
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
        text(" " + data.active_app + " ") | dim | color(T.overlay),
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

  // Event Handler
  auto event_handler = FallbackEvent(root_renderer, [&](Event e) -> bool {


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
      auto fresh = fetch_db_data(history_range);
      
      screen.Post([&, fresh]() {
        // Reset goal notifications at midnight
        static std::string last_day = "";
        time_t t = time(nullptr);
        struct tm ltm_data;
        tm *ltm = localtime_r(&t, &ltm_data);
        char buf[32];
        snprintf(buf, sizeof(buf), "%04d-%02d-%02d", 1900 + ltm->tm_year, 1 + ltm->tm_mon, ltm->tm_mday);
        std::string current_day(buf);
        
        if (last_day != "" && last_day != current_day) {
            for (auto &g : goals) {
                g.notified = false;
            }
        }
        last_day = current_day;

        std::string latest_app = data.active_app;
        std::string latest_title = data.active_title;
        
        data = fresh;
        
        // Preserve the immediate active window state from pomo_thread
        // to prevent flickering to a stale value read by fetch_db_data
        data.active_app = latest_app;
        data.active_title = latest_title;
        
        check_goals();
        reconcile(); // Dynamically update lists!
      });
    }
  });

  std::thread pomo_thread([&] {
    while (running) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      if (!running)
        break;

      std::string current_app = "None", current_title = "None";
      std::ifstream app_file("/tmp/ghost-watch-current-app");
      if (app_file) {
        std::getline(app_file, current_app);
        std::getline(app_file, current_title);
      }
      if (current_app.empty()) current_app = "None";
      if (current_title.empty()) current_title = "None";

      screen.Post([&, current_app, current_title] {
        data.active_app = current_app;
        data.active_title = current_title;
        
        // Live UI without hammering SQLite
        bool currently_idle = is_system_idle();
        if (!currently_idle && data.active_app != "None") {
          data.total_today++;
          
          bool found_a = false;
          for (auto &a : data.apps) {
            if (a.name == data.active_app) {
              a.duration++;
              found_a = true;
              break;
            }
          }
          if (!found_a) data.apps.push_back({data.active_app, 1, 1});
          
          bool found_t = false;
          for (auto &t_stat : data.titles) {
            if (t_stat.app == data.active_app && t_stat.title == data.active_title) {
              t_stat.duration++;
              found_t = true;
              break;
            }
          }
          if (!found_t) data.titles.push_back({data.active_app, data.active_title, 1});
          time_t t = time(0);
          struct tm ltm_data;
          tm *ltm = localtime_r(&t, &ltm_data);
          int cur_hour = ltm->tm_hour;
          data.hourly[cur_hour]++;
          // Keep peak_hour in sync
          if (data.hourly[cur_hour] > data.peak_hour_val) {
            data.peak_hour_val = data.hourly[cur_hour];
            data.peak_hour = cur_hour;
          }
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
      });
    }
  });

  screen.Loop(event_handler);
  running = false;
  refresh_thread.join();
  pomo_thread.join();
  return 0;
}
