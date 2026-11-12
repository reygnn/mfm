/*
 * Pastes from the clipboard through ext-data-control-v1, for the tests.
 *
 *   clipget types          prints the offered types, one per line
 *   clipget TYPE           writes the clipboard as TYPE to stdout
 *
 * Exits 1 when the clipboard is empty or does not have that type.
 */
#define _GNU_SOURCE
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-client.h>

#include "ext-data-control-v1-client-protocol.h"

static struct wl_seat *seat;
static struct ext_data_control_manager_v1 *manager;
static struct ext_data_control_offer_v1 *selection;
static bool got_selection;
static char types[32][128];
static int n_types;

static void offer_offer(void *data, struct ext_data_control_offer_v1 *offer,
	const char *mime)
{
	if (n_types < 32)
		snprintf(types[n_types++], sizeof(types[0]), "%s", mime);
}

static const struct ext_data_control_offer_v1_listener offer_listener = {
	.offer = offer_offer,
};

static void device_data_offer(void *data, struct ext_data_control_device_v1 *device,
	struct ext_data_control_offer_v1 *offer)
{
	n_types = 0;
	ext_data_control_offer_v1_add_listener(offer, &offer_listener, NULL);
}

static void device_selection(void *data, struct ext_data_control_device_v1 *device,
	struct ext_data_control_offer_v1 *offer)
{
	selection = offer;
	got_selection = true;
}

static void device_finished(void *data, struct ext_data_control_device_v1 *device)
{
}

static void device_primary_selection(void *data,
	struct ext_data_control_device_v1 *device, struct ext_data_control_offer_v1 *offer)
{
}

static const struct ext_data_control_device_v1_listener device_listener = {
	.data_offer = device_data_offer,
	.selection = device_selection,
	.finished = device_finished,
	.primary_selection = device_primary_selection,
};

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
	const char *interface, uint32_t version)
{
	if (strcmp(interface, wl_seat_interface.name) == 0 && !seat)
		seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
	else if (strcmp(interface, ext_data_control_manager_v1_interface.name) == 0)
		manager = wl_registry_bind(registry, name,
			&ext_data_control_manager_v1_interface, 1);
}

static void registry_global_remove(void *data, struct wl_registry *registry,
	uint32_t name)
{
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

int main(int argc, char *argv[])
{
	if (argc != 2) {
		fprintf(stderr, "usage: clipget types | TYPE\n");
		return 2;
	}
	struct wl_display *display = wl_display_connect(NULL);
	if (!display) {
		fprintf(stderr, "clipget: no compositor\n");
		return 1;
	}
	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);
	if (!seat || !manager) {
		fprintf(stderr, "clipget: no ext-data-control-v1\n");
		return 1;
	}
	struct ext_data_control_device_v1 *device =
		ext_data_control_manager_v1_get_data_device(manager, seat);
	ext_data_control_device_v1_add_listener(device, &device_listener, NULL);
	while (!got_selection && wl_display_roundtrip(display) >= 0)
		;
	if (!selection) {
		fprintf(stderr, "clipget: the clipboard is empty\n");
		return 1;
	}
	if (strcmp(argv[1], "types") == 0) {
		for (int i = 0; i < n_types; i++)
			printf("%s\n", types[i]);
		return 0;
	}
	bool offered = false;
	for (int i = 0; i < n_types; i++)
		offered |= strcmp(types[i], argv[1]) == 0;
	if (!offered) {
		fprintf(stderr, "clipget: no %s in the clipboard\n", argv[1]);
		return 1;
	}
	int fds[2];
	if (pipe(fds) < 0)
		return 1;
	ext_data_control_offer_v1_receive(selection, argv[1], fds[1]);
	wl_display_flush(display);
	close(fds[1]);
	char buf[65536];
	ssize_t n;
	while ((n = read(fds[0], buf, sizeof(buf))) > 0)
		fwrite(buf, 1, n, stdout);
	return n < 0;
}
