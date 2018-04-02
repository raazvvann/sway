#include <stdlib.h>
#include <wayland-server.h>
#include <wlr/types/wlr_output_layout.h>
#include "log.h"
#include "sway/output.h"
#include "sway/tree/container.h"
#include "sway/tree/layout.h"
#include "sway/tree/view.h"

struct sway_view *view_create(enum sway_view_type type,
		const struct sway_view_impl *impl) {
	struct sway_view *view = calloc(1, sizeof(struct sway_view));
	if (view == NULL) {
		return NULL;
	}
	view->type = type;
	view->impl = impl;
	wl_list_init(&view->unmanaged_view_link);
	return view;
}

void view_destroy(struct sway_view *view) {
	if (view == NULL) {
		return;
	}

	if (view->surface != NULL) {
		view_unmap(view);
	}
	if (view->swayc != NULL) {
		container_view_destroy(view->swayc);
	}

	free(view);
}

const char *view_get_title(struct sway_view *view) {
	if (view->impl->get_prop) {
		return view->impl->get_prop(view, VIEW_PROP_TITLE);
	}
	return NULL;
}

const char *view_get_app_id(struct sway_view *view) {
	if (view->impl->get_prop) {
		return view->impl->get_prop(view, VIEW_PROP_APP_ID);
	}
	return NULL;
}

const char *view_get_class(struct sway_view *view) {
	if (view->impl->get_prop) {
		return view->impl->get_prop(view, VIEW_PROP_CLASS);
	}
	return NULL;
}

const char *view_get_instance(struct sway_view *view) {
	if (view->impl->get_prop) {
		return view->impl->get_prop(view, VIEW_PROP_INSTANCE);
	}
	return NULL;
}

void view_configure(struct sway_view *view, double ox, double oy, int width,
		int height) {
	if (view->impl->configure) {
		view->impl->configure(view, ox, oy, width, height);
	}
}

void view_set_activated(struct sway_view *view, bool activated) {
	if (view->impl->set_activated) {
		view->impl->set_activated(view, activated);
	}
}

void view_close(struct sway_view *view) {
	if (view->impl->close) {
		view->impl->close(view);
	}
}

struct sway_container *container_view_destroy(struct sway_container *view) {
	if (!view) {
		return NULL;
	}
	wlr_log(L_DEBUG, "Destroying view '%s'", view->name);
	struct sway_container *parent = container_destroy(view);
	arrange_windows(parent, -1, -1);
	return parent;
}

void view_damage_whole(struct sway_view *view) {
	for (int i = 0; i < root_container.children->length; ++i) {
		struct sway_container *cont = root_container.children->items[i];
		if (cont->type == C_OUTPUT) {
			output_damage_whole_view(cont->sway_output, view);
		}
	}
}

void view_damage_from(struct sway_view *view) {
	// TODO
	view_damage_whole(view);
}

static void view_get_layout_box(struct sway_view *view, struct wlr_box *box) {
	struct sway_container *cont = container_parent(view->swayc, C_OUTPUT);

	struct wlr_output_layout *output_layout =
		root_container.sway_root->output_layout;
	struct wlr_box *output_box = wlr_output_layout_get_box(output_layout,
		cont->sway_output->wlr_output);

	box->x = output_box->x + view->swayc->x;
	box->y = output_box->y + view->swayc->y;
	box->width = view->width;
	box->height = view->height;
}

static void view_update_outputs(struct sway_view *view,
		const struct wlr_box *before) {
	struct wlr_box box;
	view_get_layout_box(view, &box);

	struct wlr_output_layout *output_layout =
		root_container.sway_root->output_layout;
	struct wlr_output_layout_output *layout_output;
	wl_list_for_each(layout_output, &output_layout->outputs, link) {
		bool intersected = before != NULL && wlr_output_layout_intersects(
			output_layout, layout_output->output, before);
		bool intersects = wlr_output_layout_intersects(output_layout,
			layout_output->output, &box);
		if (intersected && !intersects) {
			wlr_surface_send_leave(view->surface, layout_output->output);
		}
		if (!intersected && intersects) {
			wlr_surface_send_enter(view->surface, layout_output->output);
		}
	}
}

void view_map(struct sway_view *view, struct wlr_surface *wlr_surface) {
	if (!sway_assert(view->surface == NULL, "cannot map mapped view")) {
		return;
	}

	struct sway_seat *seat = input_manager_current_seat(input_manager);
	struct sway_container *focus = sway_seat_get_focus_inactive(seat,
		&root_container);
	struct sway_container *cont = container_view_create(focus, view);

	view->surface = wlr_surface;
	view->swayc = cont;

	arrange_windows(cont->parent, -1, -1);
	sway_input_manager_set_focus(input_manager, cont);

	view_damage_whole(view);
	view_update_outputs(view, NULL);
}

void view_map_unmanaged(struct sway_view *view,
		struct wlr_surface *wlr_surface) {
	if (!sway_assert(view->surface == NULL, "cannot map mapped view")) {
		return;
	}

	view->surface = wlr_surface;
	view->swayc = NULL;

	wl_list_insert(&root_container.sway_root->unmanaged_views,
		&view->unmanaged_view_link);

	view_damage_whole(view);
	// TODO: make this work for unmanaged views
	//view_update_outputs(view, NULL);
}

void view_unmap(struct sway_view *view) {
	if (!sway_assert(view->surface != NULL, "cannot unmap unmapped view")) {
		return;
	}

	view_damage_whole(view);

	wl_list_remove(&view->unmanaged_view_link);
	wl_list_init(&view->unmanaged_view_link);

	container_view_destroy(view->swayc);

	view->swayc = NULL;
	view->surface = NULL;
}

void view_update_position(struct sway_view *view, double ox, double oy) {
	if (view->swayc->x == ox && view->swayc->y == oy) {
		return;
	}

	struct wlr_box box;
	view_get_layout_box(view, &box);
	view_damage_whole(view);
	view->swayc->x = ox;
	view->swayc->y = oy;
	view_update_outputs(view, &box);
	view_damage_whole(view);
}

void view_update_size(struct sway_view *view, int width, int height) {
	if (view->width == width && view->height == height) {
		return;
	}

	struct wlr_box box;
	view_get_layout_box(view, &box);
	view_damage_whole(view);
	view->width = width;
	view->height = height;
	view_update_outputs(view, &box);
	view_damage_whole(view);
}
