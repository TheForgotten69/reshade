#include "linux/wayland_overlay_surface.hpp"
static reshade::wayland_overlay_surface probe;
extern "C" void probe_bind(wl_registry *r, uint32_t n, const char *i) { probe.bind(r,n,i); }
extern "C" bool probe_attach(wl_surface *s) { return probe.attach(s); }
extern "C" double probe_scale() { return probe.preferred(); }
extern "C" void probe_reset() { probe.reset(); }
