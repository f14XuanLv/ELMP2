#include "overlay/model.h"

namespace elmp::overlay {

namespace {
Seqlock<Hud> g_hud;
Seqlock<Menu> g_menu;
Seqlock<Manager> g_manager;
SpscRing<Request, 64> g_requests;
} // namespace

Seqlock<Hud>& hud() { return g_hud; }
Seqlock<Menu>& menu() { return g_menu; }
Seqlock<Manager>& manager() { return g_manager; }
bool request(const Request& r) { return g_requests.push(r); }
bool next_request(Request& r) { return g_requests.pop(r); }

} // namespace elmp::overlay
