/****************************************************************************
 ** hw_awlibusb.c ***********************************************************
 ****************************************************************************
 *
 *  Userspace Lirc plugin for Awox RF/IR usb remote
 * 
 *  Copyright (C) 2008 Arif <azeemarif@gmail.com>
 *  Copyright (C) 2008 Awox Pte Ltd <marif@awox.com>
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Library General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 *  July 2008:    Created along with corresponding kernel space driver
 *  August 2008:  Modified it to work in userspace using libusb. 
 *                No kernel driver is needed anymore.
 *               (reference taken from atilibusb)
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <errno.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <usb.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>

#include "hardware.h"
#include "ir_remote.h"
#include "lircd.h"
#include "receive.h"

#define AW_MODE_LIRCCODE 1

#define AWUSB_RECEIVE_BYTES 5
#define USB_TIMEOUT (1000*60)
#define AW_VENDOR_THOMSON 0x069b
#define AW_DEVICE_THOMSON 0x1111

#define AW_KEY_GAP 0		/* Original value=200000. Made it 0 to handle it in userspace */

#if !defined(AW_MODE_LIRCCODE)
static ir_code code;
static ir_code code_last;
static struct timeval time_current = { 0 };
static struct timeval time_last = { 0 };
#endif

static int awlibusb_init();
static int awlibusb_deinit();
static char *awlibusb_rec(struct ir_remote *remotes);
static void usb_read_loop(int fd);
static struct usb_device *find_usb_device(void);
static int find_device_endpoints(struct usb_device *dev);

#ifdef AW_MODE_LIRCCODE
struct hardware hw_awlibusb = {
	NULL,			/* default device */
	-1,			/* fd */
	LIRC_CAN_REC_LIRCCODE,	/* features */
	0,			/* send_mode */
	LIRC_MODE_LIRCCODE,	/* rec_mode */
	(AWUSB_RECEIVE_BYTES - 1) * CHAR_BIT,	/* code_length */
	awlibusb_init,		/* init_func */
	awlibusb_deinit,	/* deinit_func */
	NULL,			/* send_func */
	awlibusb_rec,		/* rec_func */
	receive_decode,		/* decode_func */
	NULL,			/* ioctl_func */
	NULL,			/* readdata */
	"awlibusb"
};
#else
struct hardware hw_awlibusb = {
	NULL,			/* default device */
	-1,			/* fd */
	LIRC_CAN_REC_CODE,	/* features */
	0,			/* send_mode */
	LIRC_MODE_CODE,		/* rec_mode */
	CHAR_BIT,		/* code_length */
	awlibusb_init,		/* init_func */
	awlibusb_deinit,	/* deinit_func */
	NULL,			/* send_func */
	awlibusb_rec,		/* rec_func */
	receive_decode,		/* decode_func */
	NULL,			/* ioctl_func */
	NULL,			/* readdata */
	"awlibusb"
};
#endif
typedef struct {
	u_int16_t vendor;
	u_int16_t product;
} usb_device_id;

/* table of compatible remotes -- from lirc_awusb */
static usb_device_id usb_remote_id_table[] = {
	/* Awox RF/Infrared Transciever */
	{AW_VENDOR_THOMSON, AW_DEVICE_THOMSON},
	/* Terminating entry */
	{}
};

static struct usb_dev_handle *dev_handle = NULL;
static struct usb_endpoint_descriptor *dev_ep_in = NULL;
static pid_t child = -1;

/****/

/* initialize driver -- returns 1 on success, 0 on error */
static int awlibusb_init()
{
	struct usb_device *usb_dev;
	int pipe_fd[2] = { -1, -1 };

	LOGPRINTF(1, "initializing USB receiver");

	init_rec_buffer();

	/* A separate process will be forked to read data from the USB
	 * receiver and write it to a pipe. hw.fd is set to the readable
	 * end of this pipe. */
	if (pipe(pipe_fd) != 0) {
		logperror(LOG_ERR, "couldn't open pipe");
		return 0;
	}
	hw.fd = pipe_fd[0];

	usb_dev = find_usb_device();
	if (usb_dev == NULL) {
		logprintf(LOG_ERR, "couldn't find a compatible USB device");
		goto fail;
	}

	if (!find_device_endpoints(usb_dev)) {
		logprintf(LOG_ERR, "couldn't find device endpoints");
		goto fail;
	}

	dev_handle = usb_open(usb_dev);
	if (dev_handle == NULL) {
		logperror(LOG_ERR, "couldn't open USB receiver");
		goto fail;
	}

	if (usb_claim_interface(dev_handle, 0) != 0) {
		logperror(LOG_ERR, "couldn't claim USB interface");
		goto fail;
	}

	child = fork();
	if (child == -1) {
		logperror(LOG_ERR, "couldn't fork child process");
		goto fail;
	} else if (child == 0) {
		usb_read_loop(pipe_fd[1]);
	}

	LOGPRINTF(1, "USB receiver initialized");
	return 1;

fail:
	if (dev_handle) {
		usb_close(dev_handle);
		dev_handle = NULL;
	}
	if (pipe_fd[0] >= 0)
		close(pipe_fd[0]);
	if (pipe_fd[1] >= 0)
		close(pipe_fd[1]);
	return 0;
}

/* deinitialize driver -- returns 1 on success, 0 on error */
static int awlibusb_deinit()
{
	int err = 0;

	if (dev_handle) {
		if (usb_close(dev_handle) < 0)
			err = 1;
		dev_handle = NULL;
	}

	if (hw.fd >= 0) {
		if (close(hw.fd) < 0)
			err = 1;
		hw.fd = -1;
	}

	if (child > 1) {
		if ((kill(child, SIGTERM) == -1)
		    || (waitpid(child, NULL, 0) == 0))
			err = 1;
	}

	return !err;
}

static char *awlibusb_rec(struct ir_remote *remotes)
{
	if (!clear_rec_buffer())
		return NULL;
	return decode_all(remotes);
}

/* returns 1 if the given device should be used, 0 otherwise */
static int is_device_ok(struct usb_device *dev)
{
	/* TODO: allow exact device to be specified */

	/* check if the device ID is in usb_remote_id_table */
	usb_device_id *dev_id;
	for (dev_id = usb_remote_id_table; dev_id->vendor; dev_id++) {
		if ((dev->descriptor.idVendor == dev_id->vendor) && (dev->descriptor.idProduct == dev_id->product)) {
			return 1;
		}
	}

	return 0;
}

/* find a compatible USB receiver and return a usb_device,
 * or NULL on failure. */
static struct usb_device *find_usb_device(void)
{
	struct usb_bus *usb_bus;
	struct usb_device *dev;

	usb_init();
	usb_find_busses();
	usb_find_devices();

	for (usb_bus = usb_busses; usb_bus; usb_bus = usb_bus->next) {
		for (dev = usb_bus->devices; dev; dev = dev->next) {
			if (is_device_ok(dev))
				return dev;
		}
	}
	return NULL;		/* no suitable device found */
}

/* set dev_ep_in and dev_ep_out to the in/out endpoints of the given
 * device. returns 1 on success, 0 on failure. */
static int find_device_endpoints(struct usb_device *dev)
{
	struct usb_interface_descriptor *idesc;

	if (dev->descriptor.bNumConfigurations != 1)
		return 0;

	if (dev->config[0].bNumInterfaces != 1)
		return 0;

	if (dev->config[0].interface[0].num_altsetting != 1)
		return 0;

	idesc = &dev->config[0].interface[0].altsetting[0];
//      if (idesc->bNumEndpoints != 2) return 0;

	dev_ep_in = &idesc->endpoint[0];
	if ((dev_ep_in->bEndpointAddress & USB_ENDPOINT_DIR_MASK)
	    != USB_ENDPOINT_IN)
		return 0;

	if ((dev_ep_in->bmAttributes & USB_ENDPOINT_TYPE_MASK)
	    != USB_ENDPOINT_TYPE_INTERRUPT)
		return 0;

	return 1;
}

/* this function is run in a forked process to read data from the USB
 * receiver and write it to the given fd. it calls exit() with result
 * code 0 on success, or 1 on failure. */
static void usb_read_loop(int fd)
{
	int inited = 0;
	int err = 0;
#if !defined(AW_MODE_LIRCCODE)
	long elapsed_seconds = 0;	/* diff between seconds counter */
	long elapsed_useconds = 0;	/* diff between microseconds counter */
	long time_diff = 0;
#endif

	alarm(0);
	signal(SIGTERM, SIG_DFL);
	signal(SIGPIPE, SIG_DFL);
	signal(SIGINT, SIG_DFL);
	signal(SIGHUP, SIG_IGN);
	signal(SIGALRM, SIG_IGN);

	for (;;) {
		char buf[AWUSB_RECEIVE_BYTES];
		int bytes_r, bytes_w;

		/* read from the USB device */
		bytes_r =
		    usb_interrupt_read(dev_handle, dev_ep_in->bEndpointAddress, &buf[0], sizeof(buf), USB_TIMEOUT);
		if (bytes_r < 0) {
			if (errno == EAGAIN || errno == ETIMEDOUT)
				continue;
			logperror(LOG_ERR, "can't read from USB device");
			err = 1;
			goto done;
		}

		/* sometimes the remote sends one byte on startup; ignore it */
		if (!inited) {
			inited = 1;
			if (bytes_r == 1)
				continue;
		}
#ifdef AW_MODE_LIRCCODE
		bytes_w = write(fd, &(buf[1]), (AWUSB_RECEIVE_BYTES - 1));
		/* ignore first byte */
		if (bytes_w < 0) {
			logperror(LOG_ERR, "can't write to pipe");
			err = 1;
			goto done;
		}
#else
		code = buf[AWUSB_RECEIVE_BYTES - 2];

		/* calculate time diff */
		gettimeofday(&time_current, NULL);
		elapsed_seconds = time_current.tv_sec - time_last.tv_sec;
		elapsed_useconds = time_current.tv_usec - time_last.tv_usec;
		time_diff = (elapsed_seconds) * 1000000 + elapsed_useconds;
		//printf("time_diff = %d usec\n", time_diff);

		if (!((code == code_last) && (time_diff < AW_KEY_GAP))) {
			bytes_w = write(fd, &code, 1);
			if (bytes_w < 0) {
				logperror(LOG_ERR, "can't write to pipe");
				err = 1;
				goto done;
			}
			code_last = code;
			memcpy(&time_last, &time_current, sizeof(struct timeval));
		}
#endif

	}

done:
	if (!usb_close(dev_handle))
		err = 1;
	_exit(err);
}
