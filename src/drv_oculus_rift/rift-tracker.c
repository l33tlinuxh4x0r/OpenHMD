/*
 * Rift position tracking
 * Copyright 2014-2015 Philipp Zabel
 * Copyright 2019 Jan Schmidt
 * SPDX-License-Identifier: BSL-1.0
 */

 #define _GNU_SOURCE

#include <libusb.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>

#include "rift-tracker.h"
#include "rift-sensor.h"

#include "rift-sensor-maths.h"
#include "rift-sensor-opencv.h"
#include "rift-sensor-pose-helper.h"

#include "rift-debug-draw.h"

#include "ohmd-pipewire.h"

#define ASSERT_MSG(_v, label, ...) if(!(_v)){ fprintf(stderr, __VA_ARGS__); goto label; }

#define MAX_SENSORS 4

static void rift_tracked_device_send_imu_debug(rift_tracked_device *dev);

struct rift_tracker_ctx_s
{
	ohmd_context* ohmd_ctx;
	libusb_context *usb_ctx;
	ohmd_mutex *tracker_lock;

	ohmd_thread* usb_thread;
	int usb_completed;

	bool have_exposure_info;
	rift_tracker_exposure_info exposure_info;

	rift_sensor_ctx *sensors[MAX_SENSORS];
	uint8_t n_sensors;

	rift_tracked_device devices[RIFT_MAX_TRACKED_DEVICES];
	uint8_t n_devices;
};

rift_tracked_device *
rift_tracker_add_device (rift_tracker_ctx *ctx, int device_id, posef *imu_pose, rift_leds *leds)
{
	int i;
	rift_tracked_device *next_dev;
	char device_name[64];

	snprintf(device_name,64,"openhmd-rift-device-%d", device_id);
	device_name[63] = 0;

	assert (ctx->n_devices < RIFT_MAX_TRACKED_DEVICES);

	ohmd_lock_mutex (ctx->tracker_lock);
	next_dev = ctx->devices + ctx->n_devices;

	next_dev->id = device_id;
	ofusion_init(&next_dev->fusion);
	next_dev->fusion_to_model = *imu_pose;

	next_dev->debug_metadata = ohmd_pw_debug_stream_new (device_name);
	next_dev->leds = leds;
	next_dev->led_search = led_search_model_new (leds);
	ctx->n_devices++;
	ohmd_unlock_mutex (ctx->tracker_lock);

	/* Tell the sensors about the new device */
	for (i = 0; i < ctx->n_sensors; i++) {
		rift_sensor_ctx *sensor_ctx = ctx->sensors[i];
		if (!rift_sensor_add_device (sensor_ctx, next_dev)) {
			LOGE("Failed to configure object tracking for device %d\n", device_id);
		}
	}

	printf("device %d online. Now tracking.\n", device_id);
	return next_dev;
}

static unsigned int uvc_handle_events(void *arg)
{
	rift_tracker_ctx *tracker_ctx = arg;

	while (!tracker_ctx->usb_completed)
		libusb_handle_events_completed(tracker_ctx->usb_ctx, &tracker_ctx->usb_completed);

	return 0;
}

rift_tracker_ctx *
rift_tracker_new (ohmd_context* ohmd_ctx,
		const uint8_t radio_id[5])
{
	rift_tracker_ctx *tracker_ctx = NULL;
	int ret, i;
	libusb_device **devs;

	tracker_ctx = ohmd_alloc(ohmd_ctx, sizeof (rift_tracker_ctx));
	tracker_ctx->ohmd_ctx = ohmd_ctx;
	tracker_ctx->tracker_lock = ohmd_create_mutex(ohmd_ctx);

	for (i = 0; i < RIFT_MAX_TRACKED_DEVICES; i++) {
		rift_tracked_device *dev = tracker_ctx->devices + i;
		dev->device_lock = ohmd_create_mutex(ohmd_ctx);
	}

	ret = libusb_init(&tracker_ctx->usb_ctx);
	ASSERT_MSG(ret >= 0, fail, "could not initialize libusb\n");

	ret = libusb_get_device_list(tracker_ctx->usb_ctx, &devs);
	ASSERT_MSG(ret >= 0, fail, "Could not get USB device list\n");

	/* Start USB event thread */
	tracker_ctx->usb_completed = false;
	tracker_ctx->usb_thread = ohmd_create_thread (ohmd_ctx, uvc_handle_events, tracker_ctx);

	for (i = 0; devs[i]; ++i) {
		struct libusb_device_descriptor desc;
		libusb_device_handle *usb_devh;
		rift_sensor_ctx *sensor_ctx = NULL;
		unsigned char serial[33];

		ret = libusb_get_device_descriptor(devs[i], &desc);
		if (ret < 0)
			continue; /* Can't access this device */
		if (desc.idVendor != 0x2833 || (desc.idProduct != CV1_PID && desc.idProduct != DK2_PID))
			continue;

		ret = libusb_open(devs[i], &usb_devh);
		if (ret) {
			fprintf (stderr, "Failed to open Rift Sensor device. Check permissions\n");
			continue;
		}

		sprintf ((char *) serial, "UNKNOWN");
		serial[32] = '\0';

		if (desc.iSerialNumber) {
			ret = libusb_get_string_descriptor_ascii(usb_devh, desc.iSerialNumber, serial, 32);
			if (ret < 0)
				fprintf (stderr, "Failed to read the Rift Sensor Serial number.\n");
		}

		sensor_ctx = rift_sensor_new (ohmd_ctx, tracker_ctx->n_sensors, (char *) serial, tracker_ctx->usb_ctx, usb_devh, tracker_ctx, radio_id);
		if (sensor_ctx != NULL) {
			tracker_ctx->sensors[tracker_ctx->n_sensors] = sensor_ctx;
			tracker_ctx->n_sensors++;
			if (tracker_ctx->n_sensors == MAX_SENSORS)
				break;
		}
	}
	libusb_free_device_list(devs, 1);

	printf ("Opened %u Rift Sensor cameras\n", tracker_ctx->n_sensors);

	return tracker_ctx;

fail:
	if (tracker_ctx)
		rift_tracker_free (tracker_ctx);
	return NULL;
}

bool rift_tracker_get_exposure_info (rift_tracker_ctx *ctx, rift_tracker_exposure_info *info)
{
	bool ret;

	ohmd_lock_mutex (ctx->tracker_lock);
	ret = ctx->have_exposure_info;
	*info = ctx->exposure_info;
	ohmd_unlock_mutex (ctx->tracker_lock);

	return ret;
}

void rift_tracker_update_exposure (rift_tracker_ctx *ctx, uint16_t exposure_count, uint32_t exposure_hmd_ts, uint8_t led_pattern_phase)
{
	ohmd_lock_mutex (ctx->tracker_lock);
	if (ctx->exposure_info.led_pattern_phase != led_pattern_phase) {
		LOGD ("%f LED pattern phase changed to %d",
			(double) (now) / 1000000.0, led_pattern_phase);
		ctx->exposure_info.led_pattern_phase = led_pattern_phase;
	}

	if (ctx->exposure_info.count != exposure_count) {
		uint64_t now = ohmd_monotonic_get(ctx->ohmd_ctx);
		int i;

		ctx->exposure_info.local_ts = now;
		ctx->exposure_info.count = exposure_count;
		ctx->exposure_info.hmd_ts = exposure_hmd_ts;
		ctx->exposure_info.led_pattern_phase = led_pattern_phase;
		ctx->have_exposure_info = true;

		LOGD ("%f Have new exposure TS %u count %d LED pattern phase %d",
			(double) (now) / 1000000.0, exposure_count, exposure_hmd_ts, led_pattern_phase);

		for (i = 0; i < RIFT_MAX_TRACKED_DEVICES; i++) {
			rift_tracked_device *dev = ctx->devices + i;

			ohmd_lock_mutex (dev->device_lock);
			rift_tracked_device_send_imu_debug(dev);
			ohmd_unlock_mutex (dev->device_lock);
		}
	}
	ohmd_unlock_mutex (ctx->tracker_lock);
}

void
rift_tracker_free (rift_tracker_ctx *tracker_ctx)
{
	int i;

	if (!tracker_ctx)
		return;

	for (i = 0; i < tracker_ctx->n_sensors; i++) {
		rift_sensor_ctx *sensor_ctx = tracker_ctx->sensors[i];
		rift_sensor_free (sensor_ctx);
	}

	for (i = 0; i < RIFT_MAX_TRACKED_DEVICES; i++) {
		rift_tracked_device *dev = tracker_ctx->devices + i;
		if (dev->led_search)
			led_search_model_free (dev->led_search);
		if (dev->debug_metadata != NULL)
			ohmd_pw_debug_stream_free (dev->debug_metadata);
		ohmd_destroy_mutex (dev->device_lock);
	}

	/* Stop USB event thread */
	tracker_ctx->usb_completed = true;
	ohmd_destroy_thread (tracker_ctx->usb_thread);

	if (tracker_ctx->usb_ctx)
		libusb_exit (tracker_ctx->usb_ctx);

	ohmd_destroy_mutex (tracker_ctx->tracker_lock);
	free (tracker_ctx);
}

void rift_tracked_device_imu_update(rift_tracked_device *dev, uint64_t local_ts, uint32_t device_ts, float dt, const vec3f* ang_vel, const vec3f* accel, const vec3f* mag_field)
{
	rift_tracked_device_imu_observation *obs;

	ohmd_lock_mutex (dev->device_lock);
	ofusion_update(&dev->fusion, dt, ang_vel, accel, mag_field);
	dev->last_device_ts = device_ts;

	obs = dev->pending_imu_observations + dev->num_pending_imu_observations;
	obs->local_ts = local_ts;
	obs->device_ts = device_ts;
	obs->dt = dt;
	obs->ang_vel = *ang_vel;
	obs->accel = *accel;
	obs->mag = *mag_field;
	obs->simple_orient = dev->fusion.orient;

	dev->num_pending_imu_observations++;

	if (dev->num_pending_imu_observations == RIFT_MAX_PENDING_IMU_OBSERVATIONS) {
		/* No camera observations for a while - send our observations from here instead */
		rift_tracked_device_send_imu_debug(dev);
	}

	ohmd_unlock_mutex (dev->device_lock);
}

void rift_tracked_device_get_view_pose(rift_tracked_device *dev, posef *pose)
{
	ohmd_lock_mutex (dev->device_lock);
	oposef_init(pose, &dev->fusion.world_position, &dev->fusion.orient);
	ohmd_unlock_mutex (dev->device_lock);
}

void rift_tracked_device_model_pose_update(rift_tracked_device *dev, uint64_t local_ts, rift_tracker_exposure_info *exposure_info, posef *pose)
{
	double time = (double)(exposure_info->local_ts) / 1000000000.0;

	ohmd_lock_mutex (dev->device_lock);

	/* Undo any IMU to device conversion */
	oposef_apply_inverse(pose, &dev->fusion_to_model, pose);

	if (dev->id == 0) {
		/* Mirror the pose in XZ to go from device axes to view-plane */
		oposef_mirror_XZ(pose);
	}

	rift_tracked_device_send_imu_debug(dev);
	ofusion_tracker_update (&dev->fusion, time, &pose->pos, &pose->orient);
	ohmd_unlock_mutex (dev->device_lock);
}

void rift_tracked_device_get_model_pose(rift_tracked_device *dev, double ts, posef *pose, float *gravity_error_rad)
{
	posef tmp;
	ohmd_lock_mutex (dev->device_lock);

	oposef_init(&tmp, &dev->fusion.world_position, &dev->fusion.orient);
	if (dev->id == 0) {
		/* Mirror the pose in XZ to go from view-plane to device axes for the HMD */
		oposef_mirror_XZ(&tmp);
	}

	/* Apply any needed global pose change */
	oposef_apply(&tmp, &dev->fusion_to_model, pose);

	/* FIXME: Return a real value based on orientation covariance, when the filtering can supply that.
	 * For now, check that there was a recent gravity update and it was small */
	if (gravity_error_rad) {
		double time_since_gravity = (ts - dev->fusion.last_gravity_vector_time);
		if (time_since_gravity > -0.5 && time_since_gravity < 0.5) {
			*gravity_error_rad = dev->fusion.grav_error_angle;
		}
		else {
			*gravity_error_rad = M_PI;
		}
	}

	ohmd_unlock_mutex (dev->device_lock);
}

/* Called with the device lock held */
static void
rift_tracked_device_send_imu_debug(rift_tracked_device *dev)
{
	int i;

	if (dev->num_pending_imu_observations == 0)
		return;

	if (dev->debug_metadata && ohmd_pw_debug_stream_connected(dev->debug_metadata)) {
		char debug_str[1024];

		for (i = 0; i < dev->num_pending_imu_observations; i++) {
			rift_tracked_device_imu_observation *obs = dev->pending_imu_observations + i;

			snprintf (debug_str, 1024, ",\n{ \"type\": \"imu\", \"local-ts\": %llu, "
				 "\"device-ts\": %u, \"dt\": %f, "
				 "\"ang_vel\": [ %f, %f, %f ], \"accel\": [ %f, %f, %f ], "
				 "\"mag\": [ %f, %f, %f ], "
				 "\"simple-orient\" : [ %f, %f, %f, %f ] }",
				(unsigned long long) obs->local_ts,
				obs->device_ts, obs->dt,
				obs->ang_vel.x, obs->ang_vel.y, obs->ang_vel.z,
				obs->accel.x, obs->accel.y, obs->accel.z,
				obs->mag.x, obs->mag.y, obs->mag.z,
				obs->simple_orient.x, obs->simple_orient.y, obs->simple_orient.z, obs->simple_orient.w);

			debug_str[1023] = '\0';

			ohmd_pw_debug_stream_push (dev->debug_metadata, obs->local_ts, debug_str);
		}
	}

	dev->num_pending_imu_observations = 0;
}
