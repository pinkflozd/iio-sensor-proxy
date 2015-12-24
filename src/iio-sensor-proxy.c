/*
 * Copyright (c) 2014-2015 Bastien Nocera <hadess@hadess.net>
 *
 * orientation_calc() from the sensorfw package
 * Copyright (C) 2009-2010 Nokia Corporation
 * Authors:
 *   Üstün Ergenoglu <ext-ustun.ergenoglu@nokia.com>
 *   Timo Rongas <ext-timo.2.rongas@nokia.com>
 *   Lihan Guo <lihan.guo@digia.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3 as published by
 * the Free Software Foundation.
 *
 */

#define _GNU_SOURCE

#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include <gio/gio.h>
#include <gudev/gudev.h>
#include "drivers.h"
#include "orientation.h"

#include "iio-sensor-proxy-resources.h"

#define SENSOR_PROXY_DBUS_NAME          "net.hadess.SensorProxy"
#define SENSOR_PROXY_DBUS_PATH          "/net/hadess/SensorProxy"
#define SENSOR_PROXY_COMPASS_DBUS_PATH  "/net/hadess/SensorProxy/Compass"
#define SENSOR_PROXY_IFACE_NAME         SENSOR_PROXY_DBUS_NAME
#define SENSOR_PROXY_COMPASS_IFACE_NAME SENSOR_PROXY_DBUS_NAME ".Compass"

#define NUM_SENSOR_TYPES DRIVER_TYPE_COMPASS + 1

#define ORIENTATIONS 5

typedef struct {
	GMainLoop *loop;
	GDBusNodeInfo *introspection_data;
	GDBusConnection *connection;
	guint name_id;
	gboolean init_done;

	SensorDriver *drivers[NUM_SENSOR_TYPES];
	GUdevDevice  *devices[NUM_SENSOR_TYPES];
	GHashTable   *clients[NUM_SENSOR_TYPES]; /* key = D-Bus name, value = watch ID */

	/* Accelerometer */
	int accel_x, accel_y, accel_z;
	OrientationUp previous_orientation;
	OrientationUp orientation_quirk[ORIENTATIONS];
	OrientationUp orientation_quirk_reverse[ORIENTATIONS];

	/* Light */
	gdouble previous_level;
	gboolean uses_lux;

	/* Compass */
	gdouble previous_heading;
} SensorData;

static const SensorDriver * const drivers[] = {
	&iio_buffer_accel,
	&iio_poll_accel,
	&input_accel,
	&iio_poll_light,
	&iio_buffer_light,
	&hwmon_light,
	&fake_compass,
	&fake_light,
	&iio_buffer_compass
};

static ReadingsUpdateFunc driver_type_to_callback_func (DriverType type);

static const char *
driver_type_to_str (DriverType type)
{
	switch (type) {
	case DRIVER_TYPE_ACCEL:
		return "accelerometer";
	case DRIVER_TYPE_LIGHT:
		return "ambient light sensor";
	case DRIVER_TYPE_COMPASS:
		return "compass";
	default:
		g_assert_not_reached ();
	}
}

#define DRIVER_FOR_TYPE(driver_type) data->drivers[driver_type]
#define DEVICE_FOR_TYPE(driver_type) data->devices[driver_type]

static gboolean
driver_type_exists (SensorData *data,
		    DriverType  driver_type)
{
	return (DRIVER_FOR_TYPE(driver_type) != NULL);
}

static gboolean
find_sensors (GUdevClient *client,
	      SensorData  *data)
{
	GList *devices, *input, *platform, *l;
	gboolean found = FALSE;

	devices = g_udev_client_query_by_subsystem (client, "iio");
	input = g_udev_client_query_by_subsystem (client, "input");
	platform = g_udev_client_query_by_subsystem (client, "platform");
	devices = g_list_concat (devices, input);
	devices = g_list_concat (devices, platform);

	/* Find the devices */
	for (l = devices; l != NULL; l = l->next) {
		GUdevDevice *dev = l->data;
		guint i;

		for (i = 0; i < G_N_ELEMENTS(drivers); i++) {
			SensorDriver *driver = (SensorDriver *) drivers[i];
			if (!driver_type_exists(data, driver->type) &&
			    driver_discover (driver, dev)) {
				g_debug ("Found device %s of type %s at %s",
					 g_udev_device_get_sysfs_path (dev),
					 driver_type_to_str (driver->type),
					 driver->name);
				DEVICE_FOR_TYPE(driver->type) = g_object_ref (dev);
				DRIVER_FOR_TYPE(driver->type) = (SensorDriver *) driver;

				found = TRUE;
			}
		}

		if (driver_type_exists (data, DRIVER_TYPE_ACCEL) &&
		    driver_type_exists (data, DRIVER_TYPE_LIGHT) &&
		    driver_type_exists (data, DRIVER_TYPE_COMPASS))
			break;
	}

	g_list_free_full (devices, g_object_unref);
	return found;
}

static void
free_client_watch (gpointer data)
{
	guint watch_id = GPOINTER_TO_UINT (data);

	if (watch_id == 0)
		return;
	g_bus_unwatch_name (watch_id);
}

static GHashTable *
create_clients_hash_table (void)
{
	return g_hash_table_new_full (g_str_hash, g_str_equal,
				      g_free, free_client_watch);
}

typedef enum {
	PROP_HAS_ACCELEROMETER		= 1 << 0,
	PROP_ACCELEROMETER_ORIENTATION  = 1 << 1,
	PROP_HAS_AMBIENT_LIGHT		= 1 << 2,
	PROP_LIGHT_LEVEL		= 1 << 3,
	PROP_HAS_COMPASS                = 1 << 4,
	PROP_COMPASS_HEADING            = 1 << 5
} PropertiesMask;

#define PROP_ALL (PROP_HAS_ACCELEROMETER | \
                  PROP_ACCELEROMETER_ORIENTATION | \
                  PROP_HAS_AMBIENT_LIGHT | \
                  PROP_LIGHT_LEVEL)
#define PROP_ALL_COMPASS (PROP_HAS_COMPASS | \
			  PROP_COMPASS_HEADING)

static void
send_dbus_event (SensorData     *data,
		 PropertiesMask  mask)
{
	GVariantBuilder props_builder;
	GVariant *props_changed = NULL;

	g_assert (data->connection);

	if (mask == 0)
		return;

	g_assert ((mask & PROP_ALL) == 0 || (mask & PROP_ALL_COMPASS) == 0);

	g_variant_builder_init (&props_builder, G_VARIANT_TYPE ("a{sv}"));

	if (mask & PROP_HAS_ACCELEROMETER) {
		gboolean has_accel;

		has_accel = driver_type_exists (data, DRIVER_TYPE_ACCEL);
		g_variant_builder_add (&props_builder, "{sv}", "HasAccelerometer",
				       g_variant_new_boolean (has_accel));

		/* Send the orientation when the device appears */
		if (has_accel)
			mask |= PROP_ACCELEROMETER_ORIENTATION;
		else
			data->previous_orientation = data->orientation_quirk[ (int)ORIENTATION_UNDEFINED];
	}

	if (mask & PROP_ACCELEROMETER_ORIENTATION) {
		g_variant_builder_add (&props_builder, "{sv}", "AccelerometerOrientation",
				       g_variant_new_string (orientation_to_string (data->previous_orientation)));
	}

	if (mask & PROP_HAS_AMBIENT_LIGHT) {
		gboolean has_als;

		has_als = driver_type_exists (data, DRIVER_TYPE_LIGHT);
		g_variant_builder_add (&props_builder, "{sv}", "HasAmbientLight",
				       g_variant_new_boolean (has_als));

		/* Send the light level when the device appears */
		if (has_als)
			mask |= PROP_LIGHT_LEVEL;
	}

	if (mask & PROP_LIGHT_LEVEL) {
		g_variant_builder_add (&props_builder, "{sv}", "LightLevelUnit",
				       g_variant_new_string (data->uses_lux ? "lux" : "vendor"));
		g_variant_builder_add (&props_builder, "{sv}", "LightLevel",
				       g_variant_new_double (data->previous_level));
	}

	if (mask & PROP_HAS_COMPASS) {
		gboolean has_compass;

		has_compass = driver_type_exists (data, DRIVER_TYPE_COMPASS);
		g_variant_builder_add (&props_builder, "{sv}", "HasCompass",
				       g_variant_new_boolean (has_compass));

		/* Send the heading when the device appears */
		if (has_compass)
			mask |= PROP_COMPASS_HEADING;
	}

	if (mask & PROP_COMPASS_HEADING) {
		g_variant_builder_add (&props_builder, "{sv}", "CompassHeading",
				       g_variant_new_double (data->previous_heading));
	}

	props_changed = g_variant_new ("(s@a{sv}@as)", (mask & PROP_ALL) ? SENSOR_PROXY_IFACE_NAME : SENSOR_PROXY_COMPASS_IFACE_NAME,
				       g_variant_builder_end (&props_builder),
				       g_variant_new_strv (NULL, 0));

	g_dbus_connection_emit_signal (data->connection,
				       NULL,
				       (mask & PROP_ALL) ? SENSOR_PROXY_DBUS_PATH : SENSOR_PROXY_COMPASS_DBUS_PATH,
				       "org.freedesktop.DBus.Properties",
				       "PropertiesChanged",
				       props_changed, NULL);
}

static void
send_driver_changed_dbus_event (SensorData   *data,
				DriverType    driver_type)
{
	if (driver_type == DRIVER_TYPE_ACCEL)
		send_dbus_event (data, PROP_HAS_ACCELEROMETER);
	else if (driver_type == DRIVER_TYPE_LIGHT)
		send_dbus_event (data, PROP_HAS_AMBIENT_LIGHT);
	else if (driver_type == DRIVER_TYPE_COMPASS)
		send_dbus_event (data, PROP_HAS_COMPASS);
	else
		g_assert_not_reached ();
}

static gboolean
any_sensors_left (SensorData *data)
{
	guint i;
	gboolean exists = FALSE;

	for (i = 0; i < NUM_SENSOR_TYPES; i++) {
		if (driver_type_exists (data, i)) {
			exists = TRUE;
			break;
		}
	}

	return exists;
}

static void
client_release (SensorData            *data,
		const char            *sender,
		DriverType             driver_type)
{
	GHashTable *ht;
	guint watch_id;

	ht = data->clients[driver_type];

	watch_id = GPOINTER_TO_UINT (g_hash_table_lookup (ht, sender));
	if (watch_id == 0)
		return;

	g_hash_table_remove (ht, sender);

	if (driver_type_exists (data, driver_type) &&
	    g_hash_table_size (ht) == 0)
		driver_set_polling (DRIVER_FOR_TYPE(driver_type), FALSE);
}

static void
client_vanished_cb (GDBusConnection *connection,
		    const gchar     *name,
		    gpointer         user_data)
{
	SensorData *data = user_data;
	guint i;
	char *sender;

	if (name == NULL)
		return;

	sender = g_strdup (name);

	for (i = 0; i < NUM_SENSOR_TYPES; i++) {
		GHashTable *ht;
		guint watch_id;

		ht = data->clients[i];
		g_assert (ht);

		watch_id = GPOINTER_TO_UINT (g_hash_table_lookup (ht, sender));
		if (watch_id > 0)
			client_release (data, sender, i);
	}

	g_free (sender);
}

static void
handle_generic_method_call (SensorData            *data,
			    const gchar           *sender,
			    const gchar           *object_path,
			    const gchar           *interface_name,
			    const gchar           *method_name,
			    GVariant              *parameters,
			    GDBusMethodInvocation *invocation,
			    DriverType             driver_type)
{
	GHashTable *ht;
	guint watch_id;

	g_debug ("Handling driver refcounting method '%s' for %s device",
		 method_name, driver_type_to_str (driver_type));

	ht = data->clients[driver_type];

	if (g_str_has_prefix (method_name, "Claim")) {
		watch_id = GPOINTER_TO_UINT (g_hash_table_lookup (ht, sender));
		if (watch_id > 0) {
			g_dbus_method_invocation_return_value (invocation, NULL);
			return;
		}

		/* No other clients for this sensor? Start it */
		if (driver_type_exists (data, driver_type) &&
		    g_hash_table_size (ht) == 0)
			driver_set_polling (DRIVER_FOR_TYPE(driver_type), TRUE);

		watch_id = g_bus_watch_name_on_connection (data->connection,
							   sender,
							   G_BUS_NAME_WATCHER_FLAGS_NONE,
							   NULL,
							   client_vanished_cb,
							   data,
							   NULL);
		g_hash_table_insert (ht, g_strdup (sender), GUINT_TO_POINTER (watch_id));

		g_dbus_method_invocation_return_value (invocation, NULL);
	} else if (g_str_has_prefix (method_name, "Release")) {
		client_release (data, sender, driver_type);
		g_dbus_method_invocation_return_value (invocation, NULL);
	}
}

static void
handle_method_call (GDBusConnection       *connection,
		    const gchar           *sender,
		    const gchar           *object_path,
		    const gchar           *interface_name,
		    const gchar           *method_name,
		    GVariant              *parameters,
		    GDBusMethodInvocation *invocation,
		    gpointer               user_data)
{
	SensorData *data = user_data;
	DriverType driver_type;

	if (g_strcmp0 (method_name, "ClaimAccelerometer") == 0 ||
	    g_strcmp0 (method_name, "ReleaseAccelerometer") == 0)
		driver_type = DRIVER_TYPE_ACCEL;
	else if (g_strcmp0 (method_name, "ClaimLight") == 0 ||
		 g_strcmp0 (method_name, "ReleaseLight") == 0)
		driver_type = DRIVER_TYPE_LIGHT;
	else {
		g_dbus_method_invocation_return_error (invocation,
						       G_DBUS_ERROR,
						       G_DBUS_ERROR_UNKNOWN_METHOD,
						       "Method '%s' does not exist on object %s",
						       method_name, object_path);
		return;
	}

	handle_generic_method_call (data, sender, object_path,
				    interface_name, method_name,
				    parameters, invocation, driver_type);
}

static GVariant *
handle_get_property (GDBusConnection *connection,
		     const gchar     *sender,
		     const gchar     *object_path,
		     const gchar     *interface_name,
		     const gchar     *property_name,
		     GError         **error,
		     gpointer         user_data)
{
	SensorData *data = user_data;

	g_assert (data->connection);

	if (g_strcmp0 (property_name, "HasAccelerometer") == 0)
		return g_variant_new_boolean (driver_type_exists (data, DRIVER_TYPE_ACCEL));
	if (g_strcmp0 (property_name, "AccelerometerOrientation") == 0)
		return g_variant_new_string (orientation_to_string (data->previous_orientation));
	if (g_strcmp0 (property_name, "HasAmbientLight") == 0)
		return g_variant_new_boolean (driver_type_exists (data, DRIVER_TYPE_LIGHT));
	if (g_strcmp0 (property_name, "LightLevelUnit") == 0)
		return g_variant_new_string (data->uses_lux ? "lux" : "vendor");
	if (g_strcmp0 (property_name, "LightLevel") == 0)
		return g_variant_new_double (data->previous_level);

	return NULL;
}

static const GDBusInterfaceVTable interface_vtable =
{
	handle_method_call,
	handle_get_property,
	NULL
};

static void
handle_compass_method_call (GDBusConnection       *connection,
			    const gchar           *sender,
			    const gchar           *object_path,
			    const gchar           *interface_name,
			    const gchar           *method_name,
			    GVariant              *parameters,
			    GDBusMethodInvocation *invocation,
			    gpointer               user_data)
{
	SensorData *data = user_data;

	if (g_strcmp0 (method_name, "ClaimCompass") != 0 &&
	    g_strcmp0 (method_name, "ReleaseCompass") != 0) {
		g_dbus_method_invocation_return_error (invocation,
						       G_DBUS_ERROR,
						       G_DBUS_ERROR_UNKNOWN_METHOD,
						       "Method '%s' does not exist on object %s",
						       method_name, object_path);
		return;
	}

	handle_generic_method_call (data, sender, object_path,
				    interface_name, method_name,
				    parameters, invocation, DRIVER_TYPE_COMPASS);
}

static GVariant *
handle_compass_get_property (GDBusConnection *connection,
			     const gchar     *sender,
			     const gchar     *object_path,
			     const gchar     *interface_name,
			     const gchar     *property_name,
			     GError         **error,
			     gpointer         user_data)
{
	SensorData *data = user_data;

	g_assert (data->connection);

	if (g_strcmp0 (property_name, "HasCompass") == 0)
		return g_variant_new_boolean (data->drivers[DRIVER_TYPE_COMPASS] != NULL);
	if (g_strcmp0 (property_name, "CompassHeading") == 0)
		return g_variant_new_double (data->previous_heading);

	return NULL;
}

static const GDBusInterfaceVTable compass_interface_vtable =
{
	handle_compass_method_call,
	handle_compass_get_property,
	NULL
};

static void
name_lost_handler (GDBusConnection *connection,
		   const gchar     *name,
		   gpointer         user_data)
{
	g_debug ("iio-sensor-proxy is already running, or it cannot own its D-Bus name. Verify installation.");
	exit (0);
}

static void
bus_acquired_handler (GDBusConnection *connection,
		      const gchar     *name,
		      gpointer         user_data)
{
	SensorData *data = user_data;

	g_dbus_connection_register_object (connection,
					   SENSOR_PROXY_DBUS_PATH,
					   data->introspection_data->interfaces[0],
					   &interface_vtable,
					   data,
					   NULL,
					   NULL);

	g_dbus_connection_register_object (connection,
					   SENSOR_PROXY_COMPASS_DBUS_PATH,
					   data->introspection_data->interfaces[1],
					   &compass_interface_vtable,
					   data,
					   NULL,
					   NULL);

	data->connection = g_object_ref (connection);
}

static void
name_acquired_handler (GDBusConnection *connection,
		       const gchar     *name,
		       gpointer         user_data)
{
	SensorData *data = user_data;

	if (data->init_done) {
		send_dbus_event (data, PROP_ALL);
		send_dbus_event (data, PROP_ALL_COMPASS);
	}
}

static gboolean
setup_dbus (SensorData *data)
{
	GBytes *bytes;

	bytes = g_resources_lookup_data ("/net/hadess/SensorProxy/net.hadess.SensorProxy.xml",
					 G_RESOURCE_LOOKUP_FLAGS_NONE,
					 NULL);
	data->introspection_data = g_dbus_node_info_new_for_xml (g_bytes_get_data (bytes, NULL), NULL);
	g_bytes_unref (bytes);
	g_assert (data->introspection_data != NULL);

	data->name_id = g_bus_own_name (G_BUS_TYPE_SYSTEM,
					SENSOR_PROXY_DBUS_NAME,
					G_BUS_NAME_OWNER_FLAGS_NONE,
					bus_acquired_handler,
					name_acquired_handler,
					name_lost_handler,
					data,
					NULL);

	return TRUE;
}

static void
accel_changed_func (SensorDriver *driver,
		    gpointer      readings_data,
		    gpointer      user_data)
{
	SensorData *data = user_data;
	AccelReadings *readings = (AccelReadings *) readings_data;
	OrientationUp orientation = data->previous_orientation;

	//FIXME handle errors
	g_debug ("Accel sent by driver (quirk applied): %d, %d, %d", readings->accel_x, readings->accel_y, readings->accel_z);

	orientation = data->orientation_quirk[ (int)orientation_calc ( (OrientationUp)data->orientation_quirk_reverse[data->previous_orientation], readings->accel_x, readings->accel_y, readings->accel_z)];

	data->accel_x = readings->accel_x;
	data->accel_y = readings->accel_y;
	data->accel_z = readings->accel_z;

	if (data->previous_orientation != orientation) {
		OrientationUp tmp;

		tmp = data->previous_orientation;
		data->previous_orientation = orientation;
		send_dbus_event (data, PROP_ACCELEROMETER_ORIENTATION);
		g_debug ("Emitted orientation changed: from %s to %s",
			 orientation_to_string (tmp),
			 orientation_to_string (data->previous_orientation));
	}
}

static void
light_changed_func (SensorDriver *driver,
		    gpointer      readings_data,
		    gpointer      user_data)
{
	SensorData *data = user_data;
	LightReadings *readings = (LightReadings *) readings_data;

	//FIXME handle errors
	g_debug ("Light level sent by driver (quirk applied): %lf (unit: %s)",
		 readings->level, data->uses_lux ? "lux" : "vendor");

	if (data->previous_level != readings->level ||
	    data->uses_lux != readings->uses_lux) {
		gdouble tmp;

		tmp = data->previous_level;
		data->previous_level = readings->level;

		data->uses_lux = readings->uses_lux;

		send_dbus_event (data, PROP_LIGHT_LEVEL);
		g_debug ("Emitted light changed: from %lf to %lf",
			 tmp, data->previous_level);
	}
}

static void
compass_changed_func (SensorDriver *driver,
                      gpointer      readings_data,
                      gpointer      user_data)
{
	SensorData *data = user_data;
	CompassReadings *readings = (CompassReadings *) readings_data;

	//FIXME handle errors
	g_debug ("Heading sent by driver (quirk applied): %lf degrees",
	         readings->heading);

	if (data->previous_heading != readings->heading) {
		gdouble tmp;

		tmp = data->previous_heading;
		data->previous_heading = readings->heading;

		send_dbus_event (data, PROP_COMPASS_HEADING);
		g_debug ("Emitted heading changed: from %lf to %lf",
			 tmp, data->previous_heading);
	}
}

static ReadingsUpdateFunc
driver_type_to_callback_func (DriverType type)
{
	switch (type) {
	case DRIVER_TYPE_ACCEL:
		return accel_changed_func;
	case DRIVER_TYPE_LIGHT:
		return light_changed_func;
	case DRIVER_TYPE_COMPASS:
		return compass_changed_func;
	default:
		g_assert_not_reached ();
	}
}

static void
free_orientation_data (SensorData *data)
{
	guint i;

	if (data == NULL)
		return;

	if (data->name_id != 0) {
		g_bus_unown_name (data->name_id);
		data->name_id = 0;
	}

	for (i = 0; i < NUM_SENSOR_TYPES; i++) {
		if (driver_type_exists (data, i))
			driver_close (DRIVER_FOR_TYPE(i));
		g_clear_object (&DEVICE_FOR_TYPE(i));
		g_clear_pointer (&data->clients[i], g_hash_table_unref);
	}

	g_clear_pointer (&data->introspection_data, g_dbus_node_info_unref);
	g_clear_object (&data->connection);
	g_clear_pointer (&data->loop, g_main_loop_unref);
	g_free (data);
}

static void
sensor_changes (GUdevClient *client,
		gchar       *action,
		GUdevDevice *device,
		SensorData  *data)
{
	guint i;

	if (g_strcmp0 (action, "remove") == 0) {
		for (i = 0; i < NUM_SENSOR_TYPES; i++) {
			GUdevDevice *dev = DEVICE_FOR_TYPE(i);

			if (!dev)
				continue;

			if (g_strcmp0 (g_udev_device_get_sysfs_path (device), g_udev_device_get_sysfs_path (dev)) == 0) {
				g_debug ("Sensor type %s got removed (%s)",
					 driver_type_to_str (i),
					 g_udev_device_get_sysfs_path (dev));

				g_clear_object (&DEVICE_FOR_TYPE(i));
				DRIVER_FOR_TYPE(i) = NULL;

				g_clear_pointer (&data->clients[i], g_hash_table_unref);
				data->clients[i] = create_clients_hash_table ();

				send_driver_changed_dbus_event (data, i);
			}
		}

		if (!any_sensors_left (data))
			g_main_loop_quit (data->loop);
	} else if (g_strcmp0 (action, "add") == 0) {
		guint i;

		for (i = 0; i < G_N_ELEMENTS(drivers); i++) {
			SensorDriver *driver = (SensorDriver *) drivers[i];
			if (!driver_type_exists (data, driver->type) &&
			    driver_discover (driver, device)) {
				g_debug ("Found hotplugged device %s of type %s at %s",
					 g_udev_device_get_sysfs_path (device),
					 driver_type_to_str (driver->type),
					 driver->name);

				if (driver_open (driver, device,
						 driver_type_to_callback_func (driver->type), data)) {
					GHashTable *ht;

					DEVICE_FOR_TYPE(driver->type) = g_object_ref (device);
					DRIVER_FOR_TYPE(driver->type) = (SensorDriver *) driver;
					send_driver_changed_dbus_event (data, driver->type);

					ht = data->clients[driver->type];

					if (g_hash_table_size (ht) > 0)
						driver_set_polling (DRIVER_FOR_TYPE(driver->type), TRUE);
				}
				break;
			}
		}
	}
}

int main (int argc, char **argv)
{
	SensorData *data;
	GUdevClient *client;
	int ret = 0;
	const gchar * const subsystems[] = { "iio", "input", "platform", NULL };
	guint i;
	FILE *orientation_quirk_fp;

	/* g_setenv ("G_MESSAGES_DEBUG", "all", TRUE); */

	data = g_new0 (SensorData, 1);
	data->previous_orientation = ORIENTATION_UNDEFINED;
	data->uses_lux = TRUE;
	for (i = 0; i < ORIENTATIONS; i++) {
		data->orientation_quirk[i] = (OrientationUp)i;
	}

	/* Set up D-Bus */
	setup_dbus (data);

	if( (orientation_quirk_fp = fopen ("/etc/orientation_quirk.conf", "r"))) {
		g_debug("Found orientation quirk: /etc/orientation_quirk.conf.");
		char quirk[ORIENTATIONS];
		if (fread (quirk, 1, ORIENTATIONS, orientation_quirk_fp) == ORIENTATIONS) {
			for (i = 0; i < ORIENTATIONS; i++) {
				data->orientation_quirk[i] = (OrientationUp) (quirk[i] - '0');
				data->orientation_quirk_reverse[quirk[i] - '0'] = (OrientationUp) i;
				g_debug("Orientation quirk: %c", quirk[i]);
			}
		}
		fclose (orientation_quirk_fp);
	}

	client = g_udev_client_new (subsystems);
	if (!find_sensors (client, data)) {
		g_debug ("Could not find any supported sensors");
		return 0;
	}
	g_signal_connect (G_OBJECT (client), "uevent",
			  G_CALLBACK (sensor_changes), data);

	for (i = 0; i < NUM_SENSOR_TYPES; i++) {
		data->clients[i] = create_clients_hash_table ();

		if (!driver_type_exists (data, i))
			continue;

		if (!driver_open (DRIVER_FOR_TYPE(i), DEVICE_FOR_TYPE(i),
				  driver_type_to_callback_func (data->drivers[i]->type), data)) {
			DRIVER_FOR_TYPE(i) = NULL;
			g_clear_object (&DEVICE_FOR_TYPE(i));
		}
	}

	if (!any_sensors_left (data))
		goto out;

	data->init_done = TRUE;
	if (data->connection) {
		send_dbus_event (data, PROP_ALL);
		send_dbus_event (data, PROP_ALL_COMPASS);
	}

	data->loop = g_main_loop_new (NULL, TRUE);
	g_main_loop_run (data->loop);

out:
	free_orientation_data (data);

	return ret;
}
