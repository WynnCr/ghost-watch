#include "components.hpp"
#include "theme.hpp"

Element card_panel(Element content) {
  auto &T = get_theme();
  return vbox({separatorEmpty(), hbox({text("  "), content | flex, text("  ")}),
               separatorEmpty()}) |
         borderRounded | color(T.surface2) | bgcolor(T.surface0);
}

Element section_title(const std::string &title) {
  return hbox({text(title) | bold | color(get_theme().fg), filler()});
}

class FallbackEventBase : public ComponentBase {
public:
  FallbackEventBase(Component child, std::function<bool(Event)> on_event)
      : on_event_(std::move(on_event)) {
    Add(child);
  }

  bool OnEvent(Event event) override {
    // If the child handled it, we're done
    if (ComponentBase::OnEvent(event)) {
      return true;
    }
    // Otherwise, try our fallback handler
    if (on_event_(event)) {
      return true;
    }
    return false;
  }

private:
  std::function<bool(Event)> on_event_;
};

Component FallbackEvent(Component child, std::function<bool(Event)> on_event) {
  return Make<FallbackEventBase>(std::move(child), std::move(on_event));
}
