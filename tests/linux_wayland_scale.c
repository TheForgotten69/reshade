#define _GNU_SOURCE
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static struct wl_compositor *compositor;
static struct wl_subcompositor *subcompositor;
static struct wl_shm *shm;
static struct xdg_wm_base *shell;
static struct wp_fractional_scale_manager_v1 *fractional;
static unsigned parent_scale, child_scale;
extern void probe_bind(struct wl_registry *, uint32_t, const char *);
extern _Bool probe_attach(struct wl_surface *);
extern double probe_scale(void);
extern void probe_reset(void);
static void ping(void *data, struct xdg_wm_base *s, uint32_t serial) { xdg_wm_base_pong(s, serial); }
static const struct xdg_wm_base_listener shell_listener = {ping};
static void global(void *data, struct wl_registry *r, uint32_t name, const char *interface, uint32_t version)
{
    probe_bind(r,name,interface);
    if (!strcmp(interface, "wl_compositor")) compositor = wl_registry_bind(r,name,&wl_compositor_interface,4);
    if (!strcmp(interface, "wl_subcompositor")) subcompositor = wl_registry_bind(r,name,&wl_subcompositor_interface,1);
    if (!strcmp(interface, "wl_shm")) shm = wl_registry_bind(r,name,&wl_shm_interface,1);
    if (!strcmp(interface, "xdg_wm_base")) {
        shell = wl_registry_bind(r,name,&xdg_wm_base_interface,1);
        xdg_wm_base_add_listener(shell,&shell_listener,NULL);
    }
    if (!strcmp(interface, "wp_fractional_scale_manager_v1")) fractional = wl_registry_bind(r,name,&wp_fractional_scale_manager_v1_interface,1);
}
static void removed(void *data, struct wl_registry *r, uint32_t name) {}
static const struct wl_registry_listener registry_listener = {global,removed};
static void preferred(void *data, struct wp_fractional_scale_v1 *f, uint32_t scale) {
    *(unsigned *)data=scale;
    printf("%s preferred scale: %u/120\n",data==&parent_scale?"parent":"child",scale); fflush(stdout);
}
static const struct wp_fractional_scale_v1_listener fractional_listener={preferred};
static void configured(void *data, struct xdg_surface *surface, uint32_t serial) { xdg_surface_ack_configure(surface,serial); }
static const struct xdg_surface_listener xdg_listener={configured};
static void size(void *d,struct xdg_toplevel *t,int32_t w,int32_t h,struct wl_array *states) {}
static void closed(void *d,struct xdg_toplevel *t) {}
static const struct xdg_toplevel_listener toplevel_listener={size,closed};
static struct wl_buffer *buffer(int width,int height)
{
    int fd=memfd_create("reshade-scale-test",MFD_CLOEXEC);
    assert(fd>=0 && ftruncate(fd,width*height*4)==0);
    struct wl_shm_pool *pool=wl_shm_create_pool(shm,fd,width*height*4);
    struct wl_buffer *b=wl_shm_pool_create_buffer(pool,0,width,height,width*4,WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool); close(fd); return b;
}
int main(void)
{
    if (!getenv("RESHADE_TEST_WAYLAND_ISOLATED")) {
        fputs("Set RESHADE_TEST_WAYLAND_ISOLATED=1 inside an isolated compositor.\n", stderr);
        return 2;
    }
    struct wl_display *d=wl_display_connect(NULL); assert(d);
    struct wl_registry *r=wl_display_get_registry(d);
    wl_registry_add_listener(r,&registry_listener,NULL);
    assert(wl_display_roundtrip(d)>=0 && compositor && subcompositor && shm && shell && fractional);
    struct wl_surface *parent=wl_compositor_create_surface(compositor);
    struct xdg_surface *xdg=xdg_wm_base_get_xdg_surface(shell,parent);
    xdg_surface_add_listener(xdg,&xdg_listener,NULL);
    struct xdg_toplevel *top=xdg_surface_get_toplevel(xdg);
    xdg_toplevel_add_listener(top,&toplevel_listener,NULL);
    xdg_toplevel_set_app_id(top,"reshade-scale-probe");
    struct wp_fractional_scale_v1 *pf=wp_fractional_scale_manager_v1_get_fractional_scale(fractional,parent);
    wp_fractional_scale_v1_add_listener(pf,&fractional_listener,&parent_scale);
    wl_surface_commit(parent); assert(wl_display_roundtrip(d)>=0);
    assert(probe_attach(parent));
    struct wl_surface *child=wl_compositor_create_surface(compositor);
    struct wl_subsurface *sub=wl_subcompositor_get_subsurface(subcompositor,child,parent);
    struct wl_region *empty=wl_compositor_create_region(compositor);
    wl_surface_set_input_region(child,empty); wl_region_destroy(empty);
    struct wp_fractional_scale_v1 *cf=wp_fractional_scale_manager_v1_get_fractional_scale(fractional,child);
    wp_fractional_scale_v1_add_listener(cf,&fractional_listener,&child_scale);
    struct wl_buffer *cb=buffer(1,1),*pb=buffer(400,300);
    /* Deliberately leave the child bufferless and uncommitted. */
    wl_surface_attach(parent,pb,0,0); wl_surface_commit(parent);
    for(int i=0;i<20;i++) { assert(wl_display_roundtrip(d)>=0); usleep(20000); }
    printf("RESULT parent=%u child=%u actual_parent_buffer_scale=1\n",parent_scale,child_scale);
    printf("PRODUCTION PROBE scale=%.3f\n",probe_scale());
    assert(probe_scale()==parent_scale/120.0);
    probe_reset();
    wp_fractional_scale_v1_destroy(cf); wl_subsurface_destroy(sub); wl_surface_destroy(child);
    wp_fractional_scale_v1_destroy(pf); xdg_toplevel_destroy(top); xdg_surface_destroy(xdg); wl_surface_destroy(parent);
    wl_buffer_destroy(cb); wl_buffer_destroy(pb); wl_display_roundtrip(d); wl_display_disconnect(d);
    return parent_scale==child_scale && child_scale!=0?0:1;
}
