// RetroFront - Main Menu System (declarative pages + widgets)
#pragma once
#include "gfx.h"
#include "io.h"
#include <functional>
#include <memory>

namespace rf {

struct MenuItem {
    enum Type { Action, Toggle, Choice, Slider, Header, Info } type = Action;
    std::string label, desc;
    std::function<bool()> visible;                  // empty = visible
    std::function<bool()> enabled;                  // empty = enabled
    std::function<void()> activate;                 // Action
    std::function<bool()> get_b; std::function<void(bool)> set_b;                       // Toggle
    std::vector<std::pair<std::string, std::string>> choices;                           // Choice: value,label
    std::function<std::string()> get_c; std::function<void(const std::string &)> set_c;
    double mn = 0, mx = 1, step = 0.05;                                                 // Slider
    std::function<double()> get_n; std::function<void(double)> set_n;
    std::function<std::string(double)> fmt_n;
    std::function<std::string()> value;             // right-aligned text (Action / Info)
    bool submenu = false;                           // shows a chevron
    bool danger = false;
};

struct Page {
    std::string title, subtitle;
    std::vector<MenuItem> items;
    int sel = 0;
    double scroll = 0, hl_y = -1;
    std::function<void(Gfx &, const Theme &, double x, double y, double w, double h)> side;  // optional extra panel
    std::function<bool(const UiEvent &)> on_key;   // custom key handling, return true if consumed
    std::function<void()> on_close;
    bool wide = false;
};

// helpers to build items concisely
MenuItem act(const std::string &label, const std::string &desc, std::function<void()> f, bool sub = false);
MenuItem toggle(const std::string &label, const std::string &desc, std::function<bool()> g, std::function<void(bool)> s);
MenuItem choice(const std::string &label, const std::string &desc, std::vector<std::pair<std::string, std::string>> ch,
                std::function<std::string()> g, std::function<void(const std::string &)> s);
MenuItem slider(const std::string &label, const std::string &desc, double mn, double mx, double step,
                std::function<double()> g, std::function<void(double)> s, std::function<std::string(double)> f);
MenuItem header(const std::string &label);
MenuItem info(const std::string &label, std::function<std::string()> v, const std::string &desc = "");

class MenuStack {
public:
    std::vector<std::unique_ptr<Page>> stack;
    double anim = 1.0;             // 0..1 page transition progress
    int anim_dir = 1;
    void push(std::unique_ptr<Page> p);
    void pop();
    void replace_top(std::unique_ptr<Page> p, bool keep_sel = true);
    Page *top() { return stack.empty() ? nullptr : stack.back().get(); }
    bool empty() const { return stack.empty(); }
    void clear() { while (!stack.empty()) pop(); }
    // returns true if the event was consumed; sets close_requested when Back on the root page
    bool handle(const UiEvent &e, bool &close_requested);
    void mouse(int mx, int my, bool click, int wheel);
    void draw(Gfx &g, const Theme &t, double x, double y, double w, double h, double time, bool pad_prompts);
    void tick(double dt);
private:
    struct Hit { double x, y, w, h; int idx; };
    std::vector<Hit> hits;
    bool usable(const MenuItem &m) const;
    bool vis(const MenuItem &m) const { return !m.visible || m.visible(); }
    void move(Page &p, int d);
    void adjust(MenuItem &m, int d);
};

} // namespace rf
