#define _POSIX_C_SOURCE 200112L

#include <assert.h>
#include <stdlib.h>
#include <wayland-server.h>
#include <wlr/backend.h>
#include <wlr/backend/headless.h>
#include <wlr/backend/multi.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_output.h>
#include <wlr/config.h>
#include <wlr/types/wlr_data_control_v1.h>
#include <wlr/types/wlr_export_dmabuf_v1.h>
#include <wlr/types/wlr_linux_dmabuf_v1.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/util/log.h>
#include <wlr/xwayland.h>

#include "wm/wm_server.h"
#include "wm/wm_util.h"
#include "wm/wm.h"
#include "wm/wm_seat.h"
#include "wm/wm_view_xdg.h"
#include "wm/wm_view_xwayland.h"
#include "wm/wm_layout.h"
#include "wm/wm_widget.h"
#include "wm/wm_config.h"
#include "wm/wm_output.h"
#include "wm/wm_renderer.h"
#include "wm/wm_idle_inhibit.h"
#include "wm/wm_widget.h"
#include "wm/wm_view.h"
#include "wm/wm_drag.h"


/*
 * Callbacks
 */
static void handle_new_input(struct wl_listener* listener, void* data){
    wlr_log(WLR_DEBUG, "Server: New input");

    struct wm_server* server = wl_container_of(listener, server, new_input);
    struct wlr_input_device* input_device = data;

    wm_seat_add_input_device(server->wm_seat, input_device);
}

static void handle_new_output(struct wl_listener* listener, void* data){
    wlr_log(WLR_DEBUG, "Server: New output");

    struct wm_server* server = wl_container_of(listener, server, new_output);
    struct wlr_output* output = data;

    wm_layout_add_output(server->wm_layout, output);

    /* Start the timer loop once an output is there */
    if(!server->callback_timer_started){
        server->callback_timer_started = true;
        wl_event_source_timer_update(
                server->callback_timer,
                1000 / server->wm_config->callback_frequency);
    }
}

static void handle_new_xdg_surface(struct wl_listener* listener, void* data){
    wlr_log(WLR_DEBUG, "Server: New xdg surface");

    struct wm_server* server = wl_container_of(listener, server, new_xdg_surface);
    struct wlr_xdg_surface* surface = data;

    /* Let clients know which (only one is supported) output they're on */
    if(server->wm_layout->default_output) {
        wlr_surface_send_enter(surface->surface,
                server->wm_layout->default_output->wlr_output);
    }

    if(surface->role == WLR_XDG_SURFACE_ROLE_POPUP){
        /* Popups should be handled by the parent */
        return;
    }

    wlr_xdg_surface_ping(surface);

    struct wm_view_xdg* view = calloc(1, sizeof(struct wm_view_xdg));
    wm_view_xdg_init(view, server, surface);
}

static void handle_new_xwayland_surface(struct wl_listener* listener, void* data){
    wlr_log(WLR_DEBUG, "Server: New xwayland surface");
    
    struct wm_server* server = wl_container_of(listener, server, new_xwayland_surface);
    struct wlr_xwayland_surface* surface = data;

    wlr_xwayland_surface_ping(surface);

    struct wm_view_xwayland* view = calloc(1, sizeof(struct wm_view_xwayland));
    wm_view_xwayland_init(view, server, surface);
}

static void handle_new_server_decoration(struct wl_listener* listener, void* data){
    /* struct wm_server* server = wl_container_of(listener, server, new_xdg_decoration); */
    /* struct wlr_server_decoration* wlr_deco = data; */

    wlr_log(WLR_DEBUG, "Server: New server decoration");
}

static void handle_new_xdg_decoration(struct wl_listener* listener, void* data){
    struct wm_server* server = wl_container_of(listener, server, new_xdg_decoration);
    struct wlr_xdg_toplevel_decoration_v1* wlr_deco = data;

    bool found=false;
    struct wm_content* content;
    wl_list_for_each(content, &server->wm_contents, link){
        if(!wm_content_is_view(content)) continue;
        struct wm_view* view = wm_cast(wm_view, content);
        if(!wm_view_is_xdg(view)) continue;
        struct wm_view_xdg* xdg_view = wm_cast(wm_view_xdg, view);
        if(xdg_view->wlr_xdg_surface == wlr_deco->surface){
            wm_view_xdg_register_decoration(xdg_view, wlr_deco);
            found=true;
            break;
        }
    }

    if(!found){
        wlr_log(WLR_INFO, "Could not find view for XDG toplevel decoration");
    }

    wlr_xdg_toplevel_decoration_v1_set_mode(
            wlr_deco, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);

}

static void handle_ready(struct wl_listener* listener, void* data){
    wlr_log(WLR_DEBUG, "Server: Ready");

    wm_callback_ready();
}

static int callback_timer_handler(void* data){
    struct wm_server* server = data;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    long long msec_diff = now.tv_nsec / 1000000 - server->last_callback_externally_sourced.tv_nsec / 1000000;
    msec_diff += (now.tv_sec - server->last_callback_externally_sourced.tv_sec)*1000;
    if(msec_diff > 1000 / server->wm_config->callback_frequency){
        wm_callback_update();
    }

    /* Reschedule */
    wl_event_source_timer_update(
            server->callback_timer,
            1000 / server->wm_config->callback_frequency);
    return 0;
}

/*
 * Class implementation
 */
void wm_server_init(struct wm_server* server, struct wm_config* config){
    wl_list_init(&server->wm_contents);
    server->wm_config = config;

    /* Display */
    server->wl_display = wl_display_create();
    assert(server->wl_display);

    /* Backend */
    server->wlr_backend = wlr_backend_autocreate(server->wl_display);
    assert(server->wlr_backend);


    /* Renderer */
    server->wm_renderer = calloc(1, sizeof(struct wm_renderer));
    wm_renderer_init(server->wm_renderer, server);

    /* Renderer */
    server->wl_event_loop = 
        wl_display_get_event_loop(server->wl_display);
    assert(server->wl_event_loop);


    /* Compositor and protocols */
    server->wlr_compositor = 
        wlr_compositor_create(server->wl_display, server->wm_renderer->wlr_renderer);
    assert(server->wlr_compositor);

    server->wlr_data_device_manager = wlr_data_device_manager_create(server->wl_display);
    assert(server->wlr_data_device_manager);

    server->wlr_xdg_shell = wlr_xdg_shell_create(server->wl_display);
    assert(server->wlr_xdg_shell);

    server->wlr_server_decoration_manager = wlr_server_decoration_manager_create(server->wl_display);
	wlr_server_decoration_manager_set_default_mode(
            server->wlr_server_decoration_manager,
            config->encourage_csd ? WLR_SERVER_DECORATION_MANAGER_MODE_CLIENT : WLR_SERVER_DECORATION_MANAGER_MODE_SERVER);

    server->wlr_xdg_decoration_manager = wlr_xdg_decoration_manager_v1_create(server->wl_display);
    assert(server->wlr_xdg_decoration_manager);

    wlr_export_dmabuf_manager_v1_create(server->wl_display);
    wlr_screencopy_manager_v1_create(server->wl_display);
    wlr_data_control_manager_v1_create(server->wl_display);
    wlr_primary_selection_v1_device_manager_create(server->wl_display);
    /* wlr_viewporter_create(server->wl_display); */

    server->wlr_xwayland = NULL;
    if(config->enable_xwayland){
        server->wlr_xwayland = wlr_xwayland_create(server->wl_display, server->wlr_compositor, false);
        assert(server->wlr_xwayland);
    }

    server->wlr_xcursor_manager = wlr_xcursor_manager_create(server->wm_config->xcursor_theme, server->wm_config->xcursor_size);
    assert(server->wlr_xcursor_manager);

    if(!wlr_xcursor_manager_load(server->wlr_xcursor_manager, server->wm_config->output_scale)){
        wlr_log(WLR_ERROR, "Cannot load XCursor");
    }

    struct wlr_xcursor* xcursor = wlr_xcursor_manager_get_xcursor(server->wlr_xcursor_manager, "left_ptr", 1);
    if(config->enable_xwayland && xcursor){
        struct wlr_xcursor_image* image = xcursor->images[0];
        wlr_xwayland_set_cursor(server->wlr_xwayland,
                image->buffer, image->width * 4, image->width, image->height, image->hotspot_x, image->hotspot_y);
    }


    /* Children */
    server->wm_layout = calloc(1, sizeof(struct wm_layout));
    wm_layout_init(server->wm_layout, server);

    if(config->enable_output_manager){
        wlr_xdg_output_manager_v1_create(server->wl_display, server->wm_layout->wlr_output_layout);
    }

    server->wm_seat = calloc(1, sizeof(struct wm_seat));
    wm_seat_init(server->wm_seat, server, server->wm_layout);

    if(server->wlr_xwayland){
        wlr_xwayland_set_seat(server->wlr_xwayland, server->wm_seat->wlr_seat);
    }

    server->wm_idle_inhibit = calloc(1, sizeof(struct wm_idle_inhibit));
    wm_idle_inhibit_init(server->wm_idle_inhibit, server);

    /* Handlers */
    server->new_input.notify = handle_new_input;
    wl_signal_add(&server->wlr_backend->events.new_input, &server->new_input);

    server->new_output.notify = handle_new_output;
    wl_signal_add(&server->wlr_backend->events.new_output, &server->new_output);

    server->new_xdg_surface.notify = handle_new_xdg_surface;
    wl_signal_add(&server->wlr_xdg_shell->events.new_surface, &server->new_xdg_surface);

	server->new_server_decoration.notify = handle_new_server_decoration;
	wl_signal_add(&server->wlr_server_decoration_manager->events.new_decoration, &server->new_server_decoration);

    server->new_xdg_decoration.notify = handle_new_xdg_decoration;
    wl_signal_add(&server->wlr_xdg_decoration_manager->events.new_toplevel_decoration, &server->new_xdg_decoration);

    if(server->wlr_xwayland){
        server->new_xwayland_surface.notify = handle_new_xwayland_surface;
        wl_signal_add(&server->wlr_xwayland->events.new_surface, &server->new_xwayland_surface);

       /*
        * Due to the unfortunate handling of XWayland forks via SIGUSR1, we need to be sure not
        * to create any threads before the XWayland server is ready
        */
        server->xwayland_ready.notify = handle_ready;
        wl_signal_add(&server->wlr_xwayland->events.ready, &server->xwayland_ready);
    }


	server->callback_timer = wl_event_loop_add_timer(server->wl_event_loop,
		callback_timer_handler, server);
    server->callback_timer_started = false;

    clock_gettime(CLOCK_MONOTONIC, &server->last_callback_externally_sourced);

    server->lock_perc = 0.0;
}

void wm_server_destroy(struct wm_server* server){
    wm_renderer_destroy(server->wm_renderer);
    wm_layout_destroy(server->wm_layout);
    wm_seat_destroy(server->wm_seat);
    wm_idle_inhibit_destroy(server->wm_idle_inhibit);

    free(server->wm_renderer);
    free(server->wm_layout);
    free(server->wm_seat);
    free(server->wm_idle_inhibit);

    wlr_xwayland_destroy(server->wlr_xwayland);
    wl_display_destroy_clients(server->wl_display);
    wl_display_destroy(server->wl_display);
}

void wm_server_surface_at(struct wm_server* server, double at_x, double at_y, 
        struct wlr_surface** result, double* result_sx, double* result_sy, double* result_scale_x, double* result_scale_y){
    struct wm_content* content;
    wl_list_for_each(content, &server->wm_contents, link){
        if(!wm_content_is_view(content)) continue;
        struct wm_view* view = wm_cast(wm_view, content);

        if(!view->mapped) continue;
        if(!view->accepts_input) continue;

        int width;
        int height;
        wm_view_get_size(view, &width, &height);

        if(width <= 0 || height <=0) continue;

        double display_x, display_y, display_width, display_height;
        wm_content_get_box(content, &display_x, &display_y, &display_width, &display_height);

        double scale_x = display_width/width;
        double scale_y = display_height/height;

        int view_at_x = round((at_x - display_x) / scale_x);
        int view_at_y = round((at_y - display_y) / scale_y);

        double sx;
        double sy;
        struct wlr_surface* surface = wm_view_surface_at(view, view_at_x, view_at_y, &sx, &sy);

        if(surface){
            *result = surface;
            if(result_sx) *result_sx = sx;
            if(result_sy) *result_sy = sy;
            if(result_scale_x) *result_scale_x = scale_x;
            if(result_scale_y) *result_scale_y = scale_y;
            return;
        }
    }

    *result = NULL;
}

struct _view_for_surface_data {
    struct wlr_surface* surface;
    bool result;
};

static void _view_for_surface(struct wlr_surface* surface, int sx, int sy, void* _data){
    struct _view_for_surface_data* data = _data;
    if(surface == data->surface){
        data->result = true;
        return;
    }
}

struct wm_view* wm_server_view_for_surface(struct wm_server* server, struct wlr_surface* surface){
    struct wm_content* content;
    wl_list_for_each(content, &server->wm_contents, link){
        if(!wm_content_is_view(content)) continue;
        struct wm_view* view = wm_cast(wm_view, content);

        struct _view_for_surface_data data = { 0 };
        data.surface = surface;
        wm_view_for_each_surface(view, _view_for_surface, &data);
        if(data.result){
            return view;
        }
    }

    return NULL;
}

struct wm_widget* wm_server_create_widget(struct wm_server* server){
    struct wm_widget* widget = calloc(1, sizeof(struct wm_widget));
    wm_widget_init(widget, server);
    return widget;
}


void wm_server_update_contents(struct wm_server* server){
    /* Empty or only one element */
    if(server->wm_contents.next == server->wm_contents.prev) return;

    int swapped = 1;
    do {
        swapped = 0;

        struct wl_list* cur1 = server->wm_contents.next;
        struct wl_list* cur2 = cur1->next;
        for(;cur1 != server->wm_contents.prev; 
                cur1 = cur1->next, cur2 = cur2->next){

            struct wm_content* content1 = wl_container_of(cur1, content1, link);
            struct wm_content* content2 = wl_container_of(cur2, content2, link);

            if(wm_content_get_z_index(content1)<wm_content_get_z_index(content2)){
                cur1->prev->next = cur2;
                cur2->next->prev = cur1;

                cur1->next = cur2->next;
                cur2->prev = cur1->prev;

                cur2->next = cur1;
                cur1->prev = cur2;

                cur1 = cur1->prev;
                cur2 = cur1;
                swapped = 1;
            }
        }

    } while(swapped);
}


void wm_server_callback_update(struct wm_server* server){
    clock_gettime(CLOCK_MONOTONIC, &server->last_callback_externally_sourced);
    wm_callback_update();
}

void wm_server_set_locked(struct wm_server* server, double lock_perc){
    if(fabs(lock_perc - server->lock_perc) < 0.001) return;

    server->lock_perc = lock_perc;
    wm_layout_damage_whole(server->wm_layout);

    if(wm_server_is_locked(server)){
        wm_seat_clear_focus(server->wm_seat);
    }else{
        wm_update_cursor(1);
    }
}

bool wm_server_is_locked(struct wm_server* server){
    return server->lock_perc > 0.001;
}

void wm_server_printf(FILE* file, struct wm_server* server){
    fprintf(file, "---- server begin ----\n");

    struct wm_content* content;
    wl_list_for_each(content, &server->wm_contents, link){
        wm_content_printf(file, content);
    }

    fprintf(file, "---- server end ------\n");

}
