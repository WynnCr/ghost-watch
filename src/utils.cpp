#include "utils.hpp"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>

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
