#define _POSIX_C_SOURCE 200112L

#include <assert.h>
#include <stdlib.h>
#include <wayland-server.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <wlr/util/region.h>
#include <wlr/xwayland.h>

#include "wm/wm_view.h"
#include "wm/wm_seat.h"
#include "wm/wm_output.h"
#include "wm/wm_renderer.h"
#include "wm/wm_server.h"
#include "wm/wm.h"

#include "wm/wm_util.h"

struct wm_content_vtable wm_view_vtable;

void wm_view_base_init(struct wm_view* view, struct wm_server* server){
    wm_content_init(&view->super, server);

    view->super.vtable = &wm_view_vtable;

    /* Abstract class */
    view->vtable = NULL;

    view->mapped = false;
    view->inhibiting_idle = false;
    view->accepts_input = true;
}

static void wm_view_base_destroy(struct wm_content* super){
    struct wm_view* view = wm_cast(wm_view, super);

    (view->vtable->destroy)(view);
    wm_content_base_destroy(super);
}

bool wm_content_is_view(struct wm_content* content){
    return content->vtable == &wm_view_vtable;
}

void wm_view_set_inhibiting_idle(struct wm_view* view, bool inhibiting_idle){
    view->inhibiting_idle = inhibiting_idle;
}
bool wm_view_is_inhibiting_idle(struct wm_view* view){
    return view->inhibiting_idle;
}

struct render_data {
    struct wm_output *output;
    pixman_region32_t* damage;
    struct timespec when;
    double x;
    double y;
    double x_scale;
    double y_scale;
    double opacity;
    double corner_radius;
    double lock_perc;
    double mask_x;
    double mask_y;
    double mask_w;
    double mask_h;
};


static void render_surface(struct wlr_surface *surface, int sx, int sy,
        void *data) {
    struct render_data *rdata = data;
    struct wm_output *output = rdata->output;

    struct wlr_texture *texture = wlr_surface_get_texture(surface);
    if (!texture) {
        return;
    }

    struct wlr_box box = {
        .x = round((rdata->x + sx * rdata->x_scale) * output->wlr_output->scale),
        .y = round((rdata->y + sy * rdata->y_scale) * output->wlr_output->scale),
        .width = round(surface->current.width * rdata->x_scale *
                output->wlr_output->scale),
        .height = round(surface->current.height * rdata->y_scale *
                output->wlr_output->scale)};

    double mask_l = fmax(0., (rdata->mask_x * output->wlr_output->scale) - box.x);
    double mask_t = fmax(0., (rdata->mask_y * output->wlr_output->scale) - box.y);
    double mask_r = fmax(0., box.x + box.width - (rdata->mask_x + rdata->mask_w) * output->wlr_output->scale);
    double mask_b = fmax(0., box.y + box.height - (rdata->mask_y + rdata->mask_h) * output->wlr_output->scale);


    double corner_radius = rdata->corner_radius * output->wlr_output->scale;
    if (sx || sy) {
        /* Only for surfaces which extend fully */
        mask_l = 0;
        mask_t = 0;
        mask_r = 0;
        mask_b = 0;
        corner_radius = 0;
    }
    wm_renderer_render_texture_at(output->wm_server->wm_renderer, rdata->damage, texture, &box,
                                  rdata->opacity,
                                  mask_l, mask_t, mask_r, mask_b,
                                  corner_radius, rdata->lock_perc);

    /* Notify client */
    wlr_surface_send_frame_done(surface, &rdata->when);
}


static void wm_view_render(struct wm_content* super, struct wm_output* output, pixman_region32_t* output_damage, struct timespec now){
    struct wm_view* view = wm_cast(wm_view, super);

    if (!view->mapped) {
        return;
    }

    int width, height;
    wm_view_get_size(view, &width, &height);

    double display_x, display_y, display_width, display_height;
    wm_content_get_box(&view->super, &display_x, &display_y, &display_width,
            &display_height);
    double mask_x, mask_y, mask_w, mask_h;
    wm_content_get_mask(&view->super, &mask_x, &mask_y, &mask_w, &mask_h);
    double corner_radius = wm_content_get_corner_radius(&view->super);

    // Firefox starts off as a 1x1 view which causes subsurfaces to be scaled up,
    // that's why we require at least size 2x2 for the root surface
    struct render_data rdata = {
        .output = output,
        .when = now,
        .damage = output_damage,
        .x = display_x,
        .y = display_y,
        .opacity = wm_content_get_opacity(&view->super),
        .x_scale = width > 1 ? display_width / width : 0,
        .y_scale = width > 1 ? display_height / height : 0,
        .corner_radius = corner_radius,
        .lock_perc = view->super.lock_enabled ? 0.0 : view->super.wm_server->lock_perc,
        .mask_x = display_x + mask_x,
        .mask_y = display_y + mask_y,
        .mask_w = mask_w,
        .mask_h = mask_h
    };


    wm_view_for_each_surface(view, render_surface, &rdata);
}


struct damage_data {
    struct wm_output *output;
    double x;
    double y;
    double x_scale;
    double y_scale;
    struct wlr_surface* origin;
};

static void damage_surface(struct wlr_surface *surface, int sx, int sy,
        void *data) {
    struct damage_data *ddata = data;
    struct wm_output *output = ddata->output;

    if(ddata->origin && ddata->origin != surface) return;

    double x = (ddata->x + sx * ddata->x_scale) * output->wlr_output->scale;
    double y = (ddata->y + sy * ddata->y_scale) * output->wlr_output->scale;
    double width = surface->current.width * ddata->x_scale * output->wlr_output->scale;
    double height = surface->current.height * ddata->y_scale * output->wlr_output->scale;

    struct wlr_box box = {
        .x = floor(x),
        .y = floor(y),
        .width = ceil(x + width) - floor(x),
        .height = ceil(y + height) - floor(y)};

    /* origin == NULL means damage everything */
    if(!ddata->origin){
        pixman_region32_t region;
        pixman_region32_init(&region);

        pixman_region32_union_rect(&region, &region,
                box.x, box.y, box.width, box.height);

        wlr_output_damage_add(output->wlr_output_damage, &region);
        pixman_region32_fini(&region);

    }

    /* effective damage might go beyond box, so do this even if origin == NULL */
	if (pixman_region32_not_empty(&surface->buffer_damage)) {
		pixman_region32_t region;
		pixman_region32_init(&region);

		wlr_surface_get_effective_damage(surface, &region);

        wlr_region_scale_xy(&region, &region, 
                ddata->x_scale * output->wlr_output->scale,
                ddata->y_scale * output->wlr_output->scale);

		pixman_region32_translate(&region, box.x, box.y);


		wlr_output_damage_add(output->wlr_output_damage, &region);
		pixman_region32_fini(&region);
	}

	if (!wl_list_empty(&surface->current.frame_callback_list)) {
		wlr_output_schedule_frame(output->wlr_output);
	}

}

static void wm_view_damage_output(struct wm_content* super, struct wm_output* output, struct wlr_surface* origin){
    struct wm_view* view = wm_cast(wm_view, super);

    int width, height;
    wm_view_get_size(view, &width, &height);

    if (width <= 0 || height <= 0) {
        return;
    }

    double display_x, display_y, display_width, display_height;
    wm_content_get_box(&view->super, &display_x, &display_y, &display_width,
            &display_height);

    struct damage_data ddata = {
        .output = output,
        .x = display_x,
        .y = display_y,
        .x_scale = display_width / width,
        .y_scale = display_height / height,
        .origin = origin
    };

    wm_view_for_each_surface(view, damage_surface, &ddata);
}

static void print_surface(struct wlr_surface *surface, int sx, int sy,
        void *data) {
    FILE* file = data;
    fprintf(file, "  surface (%d, %d) of size %d, %d: %p\n", sx, sy, surface->current.width, surface->current.height, surface);
}

static void wm_view_printf(FILE* file, struct wm_content* super){
    struct wm_view* view = wm_cast(wm_view, super);
    const char* title;
    const char* app_id;
    const char* role;
    pid_t pid;
    uid_t uid;
    gid_t gid;
    int width;
    int height;
    wm_view_get_info(view, &title, &app_id, &role);
    wm_view_get_credentials(view, &pid, &uid, &gid);
    wm_view_get_size(view, &width, &height);

    fprintf(file, "wm_view: %s, %s, %s, %d (%f, %f - %f, %f) of size %d, %d\n",
            title, app_id, role, pid, view->super.display_x, view->super.display_y, view->super.display_width, view->super.display_height,
            width, height);

    wm_view_for_each_surface(view, print_surface, file);

    wm_view_structure_printf(file, view);
}


struct wm_content_vtable wm_view_vtable = {
    .destroy = &wm_view_base_destroy,
    .render = &wm_view_render,
    .damage_output = &wm_view_damage_output,
    .printf = &wm_view_printf
};
