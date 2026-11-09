// RetroFront - Main Menu System
#include "menu.h"
#include <cmath>
#include <algorithm>

namespace rf {

MenuItem act(const std::string &l, const std::string &d, std::function<void()> f, bool sub) {
    MenuItem m; m.type = MenuItem::Action; m.label = l; m.desc = d; m.activate = std::move(f); m.submenu = sub; return m;
}
MenuItem toggle(const std::string &l, const std::string &d, std::function<bool()> g, std::function<void(bool)> s) {
    MenuItem m; m.type = MenuItem::Toggle; m.label = l; m.desc = d; m.get_b = std::move(g); m.set_b = std::move(s); return m;
}
MenuItem choice(const std::string &l, const std::string &d, std::vector<std::pair<std::string, std::string>> ch,
                std::function<std::string()> g, std::function<void(const std::string &)> s) {
    MenuItem m; m.type = MenuItem::Choice; m.label = l; m.desc = d; m.choices = std::move(ch); m.get_c = std::move(g); m.set_c = std::move(s); return m;
}
MenuItem slider(const std::string &l, const std::string &d, double mn, double mx, double st,
                std::function<double()> g, std::function<void(double)> s, std::function<std::string(double)> f) {
    MenuItem m; m.type = MenuItem::Slider; m.label = l; m.desc = d; m.mn = mn; m.mx = mx; m.step = st;
    m.get_n = std::move(g); m.set_n = std::move(s); m.fmt_n = std::move(f); return m;
}
MenuItem header(const std::string &l) { MenuItem m; m.type = MenuItem::Header; m.label = l; return m; }
MenuItem info(const std::string &l, std::function<std::string()> v, const std::string &d) {
    MenuItem m; m.type = MenuItem::Info; m.label = l; m.value = std::move(v); m.desc = d; return m;
}

bool MenuStack::usable(const MenuItem &m) const {
    if (m.type == MenuItem::Header) return false;
    if (m.visible && !m.visible()) return false;
    return true;
}
void MenuStack::push(std::unique_ptr<Page> p) {
    // land on the first selectable item
    for (size_t i = 0; i < p->items.size(); i++) if (usable(p->items[i])) { if (p->sel < (int)i || !usable(p->items[p->sel])) p->sel = (int)i; break; }
    if (p->sel >= (int)p->items.size() || (p->items.size() && !usable(p->items[p->sel])))
        for (size_t i = 0; i < p->items.size(); i++) if (usable(p->items[i])) { p->sel = (int)i; break; }
    stack.push_back(std::move(p)); anim = 0; anim_dir = 1;
}
void MenuStack::pop() {
    if (stack.empty()) return;
    if (stack.back()->on_close) stack.back()->on_close();
    stack.pop_back(); anim = 0; anim_dir = -1;
}
void MenuStack::replace_top(std::unique_ptr<Page> p, bool keep_sel) {
    int sel = stack.empty() || !keep_sel ? 0 : stack.back()->sel;
    double sc = stack.empty() || !keep_sel ? 0 : stack.back()->scroll;
    if (!stack.empty()) stack.pop_back();
    if (p->items.empty()) { stack.push_back(std::move(p)); return; }
    p->sel = std::max(0, std::min(sel, (int)p->items.size() - 1)); p->scroll = sc;
    stack.push_back(std::move(p));
    if (!usable(stack.back()->items[stack.back()->sel])) move(*stack.back(), 1);
    if (!keep_sel) { anim = 0; anim_dir = 1; }
}
void MenuStack::move(Page &p, int d) {
    int n = (int)p.items.size();
    if (!n) return;
    int s = p.sel;
    for (int k = 0; k < n; k++) {
        s = (s + d + n) % n;
        if (usable(p.items[s])) { p.sel = s; return; }
    }
}
void MenuStack::adjust(MenuItem &m, int d) {
    if (m.enabled && !m.enabled()) return;
    if (m.type == MenuItem::Toggle) m.set_b(!m.get_b());
    else if (m.type == MenuItem::Choice && !m.choices.empty()) {
        std::string cur = m.get_c();
        int i = 0, n = (int)m.choices.size();
        for (int k = 0; k < n; k++) if (m.choices[k].first == cur) i = k;
        m.set_c(m.choices[(i + d + n) % n].first);
    } else if (m.type == MenuItem::Slider) {
        double v = clampd(m.get_n() + d * m.step, m.mn, m.mx);
        v = std::round(v / m.step) * m.step;
        m.set_n(clampd(v, m.mn, m.mx));
    }
}
bool MenuStack::handle(const UiEvent &e, bool &close) {
    close = false;
    Page *p = top();
    if (!p) return false;
    if (p->on_key) { auto k = p->on_key; if (k(e)) return true; }
    if (p->items.empty()) { if (e.key == UiKey::Back || e.key == UiKey::Menu) { if (stack.size() > 1) pop(); else close = true; } return true; }
    MenuItem &m = p->items[p->sel];
    switch (e.key) {
    case UiKey::Up: move(*p, -1); return true;
    case UiKey::Down: move(*p, 1); return true;
    case UiKey::PageL: for (int i = 0; i < 6; i++) move(*p, -1); return true;
    case UiKey::PageR: for (int i = 0; i < 6; i++) move(*p, 1); return true;
    case UiKey::Left: if (m.type != MenuItem::Action) adjust(m, -1); return true;
    case UiKey::Right: if (m.type != MenuItem::Action) adjust(m, 1); return true;
    case UiKey::Accept:
        if (m.enabled && !m.enabled()) return true;
        if (m.type == MenuItem::Action && m.activate) { auto f = m.activate; f(); }   // copy: the action may replace this page
        else if (m.type == MenuItem::Toggle || m.type == MenuItem::Choice) adjust(m, 1);
        return true;
    case UiKey::Back: case UiKey::Menu:
        if (stack.size() > 1) pop(); else close = true;
        return true;
    default: return false;
    }
}
void MenuStack::mouse(int mx, int my, bool click, int wheel) {
    Page *p = top();
    if (!p) return;
    if (wheel) { move(*p, wheel > 0 ? -1 : 1); return; }
    for (auto &h : hits) if (mx >= h.x && mx < h.x + h.w && my >= h.y && my < h.y + h.h) {
        if (!usable(p->items[h.idx])) return;
        p->sel = h.idx;
        if (click) {
            MenuItem &m = p->items[h.idx];
            if (m.enabled && !m.enabled()) return;
            if (m.type == MenuItem::Action && m.activate) { auto f = m.activate; f(); return; }
            else if (m.type == MenuItem::Slider) {
                double t = clampd((mx - (h.x + h.w * 0.58)) / (h.w * 0.36), 0, 1);
                m.set_n(clampd(std::round((m.mn + t * (m.mx - m.mn)) / m.step) * m.step, m.mn, m.mx));
            } else adjust(m, mx > h.x + h.w * 0.6 ? 1 : 1);
        }
        return;
    }
}
void MenuStack::tick(double dt) { anim = std::min(1.0, anim + dt * 5.5); }

void MenuStack::draw(Gfx &g, const Theme &t, double x, double y, double w, double h, double time, bool pad) {
    Page *p = top();
    hits.clear();
    if (!p) return;
    double a = ease_out(anim);
    double slide = (1 - a) * 40 * g.s * anim_dir;
    double alpha = 0.25 + 0.75 * a;
    x += slide;
    // title
    double ts = 22 * g.s;
    g.text(FontId::Display, ts, x, y, p->title, with_alpha(t.accent, alpha), 0, 2 * g.s);
    double ty = y + ts + 10 * g.s;
    if (!p->subtitle.empty()) { g.text(FontId::Body, 22 * g.s, x, ty, p->subtitle, with_alpha(t.text_dim, alpha)); ty += 26 * g.s; }
    g.gradient_h(x, ty + 4 * g.s, w, 2 * g.s, with_alpha(t.accent, 0.8 * alpha), with_alpha(t.accent, 0));
    double ly = ty + 18 * g.s;
    double desc_h = 64 * g.s;
    double list_h = y + h - ly - desc_h;
    double ih = 40 * g.s, hh = 30 * g.s;
    // compute positions
    std::vector<double> pos(p->items.size(), 0);
    double cy = 0;
    for (size_t i = 0; i < p->items.size(); i++) {
        if (!vis(p->items[i])) { pos[i] = -1; continue; }
        pos[i] = cy; cy += p->items[i].type == MenuItem::Header ? hh + (i ? 8 * g.s : 0) : ih;
    }
    // scroll to keep selection visible
    double sp = pos[p->sel] >= 0 ? pos[p->sel] : 0;
    double target = p->scroll;
    if (sp - target < ih) target = sp - ih;
    if (sp + ih - target > list_h - ih * 0.5) target = sp + ih * 1.5 - list_h;
    target = clampd(target, 0, std::max(0.0, cy - list_h));
    p->scroll += (target - p->scroll) * 0.35;
    SDL_Rect clip{(int)(x - 20 * g.s), (int)ly, (int)(w + 40 * g.s), (int)list_h};
    SDL_RenderSetClipRect(g.r, &clip);
    // animated highlight bar
    double hy = ly + sp - p->scroll;
    if (p->hl_y < 0) p->hl_y = hy;
    p->hl_y += (hy - p->hl_y) * 0.4;
    double pulse = 0.75 + 0.25 * sin(time * 4.0);
    g.glow_bar(x - 10 * g.s, p->hl_y + 2 * g.s, w + 20 * g.s, ih - 4 * g.s, t.accent, pulse);
    g.rounded(x - 10 * g.s, p->hl_y + 2 * g.s, w + 20 * g.s, ih - 4 * g.s, 6 * g.s, with_alpha(t.accent, 0.92 * alpha));
    g.rect(x - 10 * g.s, p->hl_y + 2 * g.s, 5 * g.s, ih - 4 * g.s, with_alpha(t.accent2, alpha));
    for (size_t i = 0; i < p->items.size(); i++) {
        if (pos[i] < 0) continue;
        MenuItem &m = p->items[i];
        double iy = ly + pos[i] - p->scroll;
        if (iy + ih < ly || iy > ly + list_h) continue;
        bool sel = (int)i == p->sel;
        bool en = !m.enabled || m.enabled();
        Color tc = sel ? t.highlight_text : (en ? t.text : t.text_dim);
        if (m.danger && !sel) tc = t.danger;
        tc = with_alpha(tc, alpha * (en ? 1 : 0.55));
        double fs = 26 * g.s;
        if (m.type == MenuItem::Header) {
            g.text(FontId::Display, 11 * g.s, x, iy + (i ? 16 : 8) * g.s, m.label, with_alpha(t.accent3, alpha), 0, 1.5 * g.s);
            continue;
        }
        hits.push_back({x - 10 * g.s, iy, w + 20 * g.s, ih, (int)i});
        double lw = w * (m.type == MenuItem::Slider ? 0.55 : m.type == MenuItem::Choice ? 0.42 : 0.62);
        g.text_fit(FontId::Body, fs, x + 8 * g.s, iy + (ih - fs) / 2 - 2 * g.s, lw, m.label, tc);
        double rx = x + w - 6 * g.s;
        Color vc = sel ? t.highlight_text : t.accent;
        vc = with_alpha(vc, alpha * (en ? 1 : 0.5));
        switch (m.type) {
        case MenuItem::Toggle: {
            bool on = m.get_b();
            double sw = 46 * g.s, shh = 22 * g.s, sx = rx - sw, sy = iy + (ih - shh) / 2;
            g.rounded(sx, sy, sw, shh, shh / 2, on ? (sel ? t.highlight_text : t.accent) : with_alpha(t.text_dim, 0.5));
            g.circle(on ? sx + sw - shh / 2 : sx + shh / 2, sy + shh / 2, shh / 2 - 3 * g.s, on ? (sel ? t.accent : t.highlight_text) : t.text);
            g.text(FontId::Body, 20 * g.s, sx - 10 * g.s, iy + (ih - 20 * g.s) / 2 - 1, on ? "ON" : "OFF", vc, 2);
            break;
        }
        case MenuItem::Choice: {
            std::string cur = m.get_c(), lab = cur;
            for (auto &c : m.choices) if (c.first == cur) lab = c.second;
            double vw = w - lw - 30 * g.s;
            double tw = std::min(vw, g.text_width(FontId::Body, 24 * g.s, lab));
            g.text_fit(FontId::Body, 24 * g.s, rx - 18 * g.s, iy + (ih - 24 * g.s) / 2 - 1, vw - 36 * g.s, lab, vc, 2);
            if (sel) {
                g.text(FontId::Body, 24 * g.s, rx, iy + (ih - 24 * g.s) / 2 - 1, ">", vc, 2);
                g.text(FontId::Body, 24 * g.s, rx - 30 * g.s - std::min(tw, vw - 36 * g.s), iy + (ih - 24 * g.s) / 2 - 1, "<", vc, 2);
            }
            break;
        }
        case MenuItem::Slider: {
            double v = m.get_n(), f = (v - m.mn) / (m.mx - m.mn);
            double bw = w * 0.30, bx = rx - bw - 70 * g.s, by = iy + ih / 2 - 3 * g.s;
            g.rounded(bx, by, bw, 6 * g.s, 3 * g.s, with_alpha(sel ? t.highlight_text : t.text_dim, 0.35));
            g.rounded(bx, by, bw * clampd(f, 0, 1), 6 * g.s, 3 * g.s, sel ? t.highlight_text : t.accent);
            g.circle(bx + bw * clampd(f, 0, 1), by + 3 * g.s, 8 * g.s, sel ? t.highlight_text : t.text);
            g.text(FontId::Body, 22 * g.s, rx, iy + (ih - 22 * g.s) / 2 - 1, m.fmt_n ? m.fmt_n(v) : fmt("%.2f", v), vc, 2);
            break;
        }
        case MenuItem::Action: case MenuItem::Info: {
            std::string v = m.value ? m.value() : "";
            if (m.submenu) { g.text(FontId::Body, 26 * g.s, rx, iy + (ih - 26 * g.s) / 2 - 2, ">", vc, 2); rx -= 24 * g.s; }
            if (!v.empty()) g.text_fit(FontId::Body, 22 * g.s, rx, iy + (ih - 22 * g.s) / 2 - 1, w - lw - 30 * g.s, v, m.type == MenuItem::Info && !sel ? with_alpha(t.text_dim, alpha) : vc, 2);
            break;
        }
        default: break;
        }
    }
    SDL_RenderSetClipRect(g.r, nullptr);
    // scroll hints
    if (p->scroll > 2) g.text(FontId::Body, 20 * g.s, x + w / 2, ly - 22 * g.s, "^", with_alpha(t.text_dim, alpha), 1);
    if (cy - p->scroll > list_h + 2) g.text(FontId::Body, 20 * g.s, x + w / 2, ly + list_h - 4 * g.s, "v", with_alpha(t.text_dim, alpha), 1);
    // description
    const MenuItem &m = p->items.empty() ? MenuItem() : p->items[p->sel];
    if (!m.desc.empty()) {
        double dy = y + h - desc_h + 8 * g.s;
        g.rect(x, dy - 6 * g.s, w, 1, with_alpha(t.panel_edge, alpha));
        auto lines = g.wrap(FontId::Body, 21 * g.s, m.desc, w);
        for (size_t i = 0; i < lines.size() && i < 2; i++) g.text(FontId::Body, 21 * g.s, x, dy + i * 24 * g.s, lines[i], with_alpha(t.text_dim, alpha));
    }
    (void)pad;
}
} // namespace rf
