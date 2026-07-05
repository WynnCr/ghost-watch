#pragma once
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <string>

using namespace ftxui;

Element card_panel(Element content);
Element section_title(const std::string &title);

Component FallbackEvent(Component child, std::function<bool(Event)> on_event);


