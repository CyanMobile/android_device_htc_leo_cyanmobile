/*
 * Copyright (C) 2008 The Android Open Source Project
 * Copyright (C) 2012 milaq
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


// #define LOG_NDEBUG 0
#define LOG_TAG "lights"

#include <cutils/log.h>

#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>

#include <sys/ioctl.h>
#include <sys/types.h>

#include <hardware/lights.h>
#include "lights.h"

/******************************************************************************/
/* Synchronization primities */
static pthread_once_t g_init = PTHREAD_ONCE_INIT;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

/* Mini-led state machine */
static struct light_state_t g_notification = {0,0,0,0,0};
static struct light_state_t g_battery = {0,0,0,0,0};

static int g_backlight = 255;

/* The leds we have */
enum {
	LED_AMBER,
	LED_GREEN,
	LED_BLUE,
	LED_BLANK,
};

enum {
	MANUAL = 0,
	AUTOMATIC,
	MANUAL_SENSOR
};

/**
 * device methods
 */

static int write_int(char const* path, int value) {
	int fd;
	static int already_warned = 0;

	fd = open(path, O_RDWR);
	if (fd < 0) {
		if (already_warned == 0) {
			LOGE("write_int failed to open %s\n", path);
			already_warned = 1;
		}
		return -errno;
	}

	char buffer[20];
	int bytes = snprintf(buffer, sizeof(buffer), "%d\n", value);
	int written = write (fd, buffer, bytes);
	close(fd);

	return written == -1 ? -errno : 0;
}

static int write_string (const char *path, const char *value) {
	int fd;
	static int already_warned = 0;

	fd = open(path, O_RDWR);
	if (fd < 0) {
		if (already_warned == 0) {
			LOGE("write_string failed to open %s\n", path);
			already_warned = 1;
		}
		return -errno;
	}

	char buffer[20];
	int bytes = snprintf(buffer, sizeof(buffer), "%s\n", value);
	int written = write (fd, buffer, bytes);
	close(fd);

	return written == -1 ? -errno : 0;
}

static int is_lit(struct light_state_t const* state) {
    return state->color & 0x00ffffff;
}

static int rgb_to_brightness(struct light_state_t const* state) {
    int color = state->color & 0x00ffffff;
    return ((77*((color>>16)&0x00ff))
            + (150*((color>>8)&0x00ff)) + (29*(color&0x00ff))) >> 8;
}

static int set_light_backlight(struct light_device_t* dev, struct light_state_t const* state) {
    int err = 0;
    int brightness = rgb_to_brightness(state);
    LOGV("%s brightness=%d color=0x%08x", __func__,brightness,state->color);
    pthread_mutex_lock(&g_lock);
    g_backlight = brightness;
    err = write_int(LCD_FILE, brightness);
    pthread_mutex_unlock(&g_lock);
    return err;
}

static int set_light_buttons(struct light_device_t* dev, struct light_state_t const* state) {
	size_t i = 0;
	int err = 0;
	int on = is_lit(state);
	pthread_mutex_lock(&g_lock);

	for (i = 0; i < sizeof(BUTTON_BACKLIGHT_FILE)/sizeof(BUTTON_BACKLIGHT_FILE[0]); i++) {
		err = write_int (BUTTON_BACKLIGHT_FILE[i],on?1:0);
	}

	pthread_mutex_unlock(&g_lock);

	return 0;
}

static void set_speaker_light_locked(struct light_device_t* dev, struct light_state_t const* state) {
    	unsigned int colorRGB = state->color & 0xFFFFFF;
	unsigned int color = LED_BLANK;

	if (colorRGB & 0xFF)
		color = LED_BLUE;
	if ((colorRGB >> 8)&0xFF)
		color = LED_GREEN;
	if ((colorRGB >> 16)&0xFF)
		color = LED_AMBER;

	int amber = (colorRGB >> 16)&0xFF;
	int green = (colorRGB >> 8)&0xFF;
	int blue = (colorRGB)&0xFF;

	switch (state->flashMode) {
		case LIGHT_FLASH_TIMED:
			switch (color) {
				case LED_AMBER:
					write_int (AMBER_BLINK_FILE, 2);
					write_int (GREEN_LED_FILE, 0);
					break;
				case LED_GREEN:
					write_int (GREEN_BLINK_FILE, 3);
					write_int (AMBER_LED_FILE, 0);
					break;
				case LED_BLUE:
					write_int (GREEN_BLINK_FILE, 3);
					write_int (AMBER_LED_FILE, 0);
					break;
				case LED_BLANK:
					write_int (AMBER_BLINK_FILE, 0);
					write_int (GREEN_BLINK_FILE, 0);
					break;
				default:
					LOGE("set_led_state colorRGB=%08X, unknown color\n",
							colorRGB);
					break;
			}
			break;
		case LIGHT_FLASH_NONE:
			switch (color) {
				case LED_AMBER:
					write_int (AMBER_LED_FILE, 1);
					write_int (GREEN_LED_FILE, 0);
					break;
				case LED_GREEN:
					write_int (AMBER_LED_FILE, 0);
					write_int (GREEN_LED_FILE, 1);
					break;
				case LED_BLUE:
					write_int (AMBER_LED_FILE, 0);
					write_int (GREEN_LED_FILE, 1);
					break;
				case LED_BLANK:
					write_int (AMBER_LED_FILE, 0);
					write_int (GREEN_LED_FILE, 0);
					break;

			}
			break;
		default:
			LOGE("set_led_state colorRGB=%08X, unknown mode %d\n",
					colorRGB, state->flashMode);
	}

}

static void handle_speaker_battery_locked(struct light_device_t* dev) {

    if (is_lit(&g_battery)) {
        set_speaker_light_locked(dev, &g_battery);
    } else {
        set_speaker_light_locked(dev, &g_notification);
    }
}

static int set_light_battery(struct light_device_t* dev, struct light_state_t const* state) {
    pthread_mutex_lock(&g_lock);
    g_battery = *state;
    handle_speaker_battery_locked(dev);
    pthread_mutex_unlock(&g_lock);
    return 0;
}

static int set_light_notifications(struct light_device_t* dev, struct light_state_t const* state) {
    pthread_mutex_lock(&g_lock);
    g_notification = *state;
    handle_speaker_battery_locked(dev);
    pthread_mutex_unlock(&g_lock);
    return 0;
}

/* Initializations */
void init_globals () {
	pthread_mutex_init (&g_lock, NULL);
}

/** Close the lights device */
static int close_lights(struct light_device_t *dev) {
    if (dev) {
        free(dev);
    }
    return 0;
}

/******************************************************************************/

/**
 * module methods
 */

/** Open a new instance of a lights device using name */
static int open_lights(const struct hw_module_t* module, char const* name,
                                               struct hw_device_t** device) {
    int (*set_light)(struct light_device_t* dev,
            struct light_state_t const* state);

    if (0 == strcmp(LIGHT_ID_BACKLIGHT, name)) {
        set_light = set_light_backlight;
    }
    else if (0 == strcmp(LIGHT_ID_BUTTONS, name)) {
        set_light = set_light_buttons;
    }
    else if (0 == strcmp(LIGHT_ID_BATTERY, name)) {
        set_light = set_light_battery;
    }
    else if (0 == strcmp(LIGHT_ID_NOTIFICATIONS, name)) {
        set_light = set_light_notifications;
    }
    else {
        return -EINVAL;
    }

    pthread_once (&g_init, init_globals);
    struct light_device_t *dev = malloc(sizeof (struct light_device_t));
    memset(dev, 0, sizeof(*dev));

    dev->common.tag = HARDWARE_DEVICE_TAG;
    dev->common.version = 0;
    dev->common.module = (struct hw_module_t*)module;
    dev->common.close = (int (*)(struct hw_device_t*))close_lights;
    dev->set_light = set_light;

    *device = (struct hw_device_t*)dev;
    return 0;
}

static struct hw_module_methods_t lights_module_methods = {
    .open =  open_lights,
};

/*
 * The lights Module
 */
const struct hw_module_t HAL_MODULE_INFO_SYM = {
    .tag = HARDWARE_MODULE_TAG,
    .version_major = 1,
    .version_minor = 0,
    .id = LIGHTS_HARDWARE_MODULE_ID,
    .name = "HTC leo lights module",
    .author = "Micha LaQua && Squadzone",
    .methods = &lights_module_methods,
};
