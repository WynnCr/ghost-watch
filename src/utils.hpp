#pragma once
#include <string>

std::string format_time(int s);
std::string to_lower(const std::string &s);
std::string get_category(const std::string &app);
bool is_system_idle();
void send_notification(const std::string &title, const std::string &body);
