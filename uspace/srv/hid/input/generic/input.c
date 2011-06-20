/*
 * Copyright (c) 2006 Josef Cejka
 * Copyright (c) 2011 Jiri Svoboda
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * - Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the distribution.
 * - The name of the author may not be used to endorse or promote products
 *   derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * @addtogroup inputgen generic
 * @brief HelenOS input server.
 * @ingroup input
 * @{
 */
/** @file
 */

#include <adt/list.h>
#include <ipc/services.h>
#include <ipc/input.h>
#include <sysinfo.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <ns.h>
#include <ns_obsolete.h>
#include <async.h>
#include <async_obsolete.h>
#include <errno.h>
#include <adt/fifo.h>
#include <io/console.h>
#include <io/keycode.h>
#include <devmap.h>
#include <input.h>
#include <kbd.h>
#include <kbd_port.h>
#include <kbd_ctl.h>
#include <layout.h>
#include <mouse.h>

// FIXME: remove this header
#include <kernel/ipc/ipc_methods.h>

/* In microseconds */
#define DISCOVERY_POLL_INTERVAL		(10*1000*1000)

static void kbd_devs_yield(void);
static void kbd_devs_reclaim(void);

static void input_event_key(int, unsigned int, unsigned, wchar_t);

int client_phone = -1;

/** List of keyboard devices */
static list_t kbd_devs;

/** List of mouse devices */
list_t mouse_devs;

bool irc_service = false;
int irc_phone = -1;

#define NUM_LAYOUTS 3

static layout_ops_t *layout[NUM_LAYOUTS] = {
	&us_qwerty_ops,
	&us_dvorak_ops,
	&cz_ops
};

void kbd_push_scancode(kbd_dev_t *kdev, int scancode)
{
/*	printf("scancode: 0x%x\n", scancode);*/
	(*kdev->ctl_ops->parse_scancode)(scancode);
}

void kbd_push_ev(kbd_dev_t *kdev, int type, unsigned int key)
{
	kbd_event_t ev;
	unsigned mod_mask;

	switch (key) {
	case KC_LCTRL: mod_mask = KM_LCTRL; break;
	case KC_RCTRL: mod_mask = KM_RCTRL; break;
	case KC_LSHIFT: mod_mask = KM_LSHIFT; break;
	case KC_RSHIFT: mod_mask = KM_RSHIFT; break;
	case KC_LALT: mod_mask = KM_LALT; break;
	case KC_RALT: mod_mask = KM_RALT; break;
	default: mod_mask = 0; break;
	}

	if (mod_mask != 0) {
		if (type == KEY_PRESS)
			kdev->mods = kdev->mods | mod_mask;
		else
			kdev->mods = kdev->mods & ~mod_mask;
	}

	switch (key) {
	case KC_CAPS_LOCK: mod_mask = KM_CAPS_LOCK; break;
	case KC_NUM_LOCK: mod_mask = KM_NUM_LOCK; break;
	case KC_SCROLL_LOCK: mod_mask = KM_SCROLL_LOCK; break;
	default: mod_mask = 0; break;
	}

	if (mod_mask != 0) {
		if (type == KEY_PRESS) {
			/*
			 * Only change lock state on transition from released
			 * to pressed. This prevents autorepeat from messing
			 * up the lock state.
			 */
			kdev->mods = kdev->mods ^ (mod_mask & ~kdev->lock_keys);
			kdev->lock_keys = kdev->lock_keys | mod_mask;

			/* Update keyboard lock indicator lights. */
			(*kdev->ctl_ops->set_ind)(kdev, kdev->mods);
		} else {
			kdev->lock_keys = kdev->lock_keys & ~mod_mask;
		}
	}
/*
	printf("type: %d\n", type);
	printf("mods: 0x%x\n", mods);
	printf("keycode: %u\n", key);
*/
	if (type == KEY_PRESS && (kdev->mods & KM_LCTRL) &&
		key == KC_F1) {
		layout_destroy(kdev->active_layout);
		kdev->active_layout = layout_create(layout[0]);
		return;
	}

	if (type == KEY_PRESS && (kdev->mods & KM_LCTRL) &&
		key == KC_F2) {
		layout_destroy(kdev->active_layout);
		kdev->active_layout = layout_create(layout[1]);
		return;
	}

	if (type == KEY_PRESS && (kdev->mods & KM_LCTRL) &&
		key == KC_F3) {
		layout_destroy(kdev->active_layout);
		kdev->active_layout = layout_create(layout[2]);
		return;
	}

	ev.type = type;
	ev.key = key;
	ev.mods = kdev->mods;

	ev.c = layout_parse_ev(kdev->active_layout, &ev);
	input_event_key(ev.type, ev.key, ev.mods, ev.c);
}

/** Key has been pressed or released. */
static void input_event_key(int type, unsigned int key, unsigned mods,
    wchar_t c)
{
	async_obsolete_msg_4(client_phone, INPUT_EVENT_KEY, type, key,
	    mods, c);
}

/** Mouse pointer has moved. */
void input_event_move(int dx, int dy)
{
	async_obsolete_msg_2(client_phone, INPUT_EVENT_MOVE, dx, dy);
}

/** Mouse button has been pressed. */
void input_event_button(int bnum, int press)
{
	async_obsolete_msg_2(client_phone, INPUT_EVENT_BUTTON, bnum, press);
}

static void client_connection(ipc_callid_t iid, ipc_call_t *icall, void *arg)
{
	ipc_callid_t callid;
	ipc_call_t call;
	int retval;

	async_answer_0(iid, EOK);

	while (true) {
		callid = async_get_call(&call);
		
		if (!IPC_GET_IMETHOD(call)) {
			if (client_phone != -1) {
				async_obsolete_hangup(client_phone);
				client_phone = -1;
			}
			
			async_answer_0(callid, EOK);
			return;
		}
		
		switch (IPC_GET_IMETHOD(call)) {
		case IPC_M_CONNECT_TO_ME:
			if (client_phone != -1) {
				retval = ELIMIT;
				break;
			}
			client_phone = IPC_GET_ARG5(call);
			retval = 0;
			break;
		case INPUT_YIELD:
			kbd_devs_yield();
			retval = 0;
			break;
		case INPUT_RECLAIM:
			kbd_devs_reclaim();
			retval = 0;
			break;
		default:
			retval = EINVAL;
		}
		async_answer_0(callid, retval);
	}	
}

static kbd_dev_t *kbd_dev_new(void)
{
	kbd_dev_t *kdev;

	kdev = calloc(1, sizeof(kbd_dev_t));
	if (kdev == NULL) {
		printf(NAME ": Error allocating keyboard device. "
		    "Out of memory.\n");
		return NULL;
	}

	link_initialize(&kdev->kbd_devs);

	kdev->mods = KM_NUM_LOCK;
	kdev->lock_keys = 0;
	kdev->active_layout = layout_create(layout[0]);

	return kdev;
}

/** Add new legacy keyboard device. */
static void kbd_add_dev(kbd_port_ops_t *port, kbd_ctl_ops_t *ctl)
{
	kbd_dev_t *kdev;

	kdev = kbd_dev_new();
	if (kdev == NULL)
		return;

	kdev->port_ops = port;
	kdev->ctl_ops = ctl;
	kdev->dev_path = NULL;

	/* Initialize port driver. */
	if ((*kdev->port_ops->init)(kdev) != 0)
		goto fail;

	/* Initialize controller driver. */
	if ((*kdev->ctl_ops->init)(kdev) != 0) {
		/* XXX Uninit port */
		goto fail;
	}

	list_append(&kdev->kbd_devs, &kbd_devs);
	return;
fail:
	free(kdev);
}

/** Add new kbdev device.
 *
 * @param dev_path	Filesystem path to the device (/dev/class/...)
 */
static int kbd_add_kbdev(const char *dev_path)
{
	kbd_dev_t *kdev;

	kdev = kbd_dev_new();
	if (kdev == NULL)
		return -1;

	kdev->dev_path = dev_path;
	kdev->port_ops = NULL;
	kdev->ctl_ops = &kbdev_ctl;

	/* Initialize controller driver. */
	if ((*kdev->ctl_ops->init)(kdev) != 0) {
		goto fail;
	}

	list_append(&kdev->kbd_devs, &kbd_devs);
	return EOK;
fail:
	free(kdev);
	return -1;
}

/** Add legacy drivers/devices. */
static void kbd_add_legacy_devs(void)
{
	/*
	 * Need to add these drivers based on config unless we can probe
	 * them automatically.
	 */
#if defined(UARCH_amd64)
	kbd_add_dev(&chardev_port, &pc_ctl);
#endif
#if defined(UARCH_arm32) && defined(MACHINE_gta02)
	kbd_add_dev(&chardev_port, &stty_ctl);
#endif
#if defined(UARCH_arm32) && defined(MACHINE_testarm) && defined(CONFIG_FB)
	kbd_add_dev(&gxemul_port, &gxe_fb_ctl);
#endif
#if defined(UARCH_arm32) && defined(MACHINE_testarm) && !defined(CONFIG_FB)
	kbd_add_dev(&gxemul_port, &stty_ctl);
#endif
#if defined(UARCH_arm32) && defined(MACHINE_integratorcp)
	kbd_add_dev(&pl050_port, &pc_ctl);
#endif
#if defined(UARCH_ia32)
	kbd_add_dev(&chardev_port, &pc_ctl);
#endif
#if defined(MACHINE_i460GX)
	kbd_add_dev(&chardev_port, &pc_ctl);
#endif
#if defined(MACHINE_ski)
	kbd_add_dev(&ski_port, &stty_ctl);
#endif
#if defined(MACHINE_msim)
	kbd_add_dev(&msim_port, &pc_ctl);
#endif
#if (defined(MACHINE_lgxemul) || defined(MACHINE_bgxemul)) && defined(CONFIG_FB)
	kbd_add_dev(&gxemul_port, &gxe_fb_ctl);
#endif
#if defined(MACHINE_lgxemul) || defined(MACHINE_bgxemul) && !defined(CONFIG_FB)
	kbd_add_dev(&gxemul_port, &stty_ctl);
#endif
#if defined(UARCH_ppc32)
	kbd_add_dev(&adb_port, &apple_ctl);
#endif
#if defined(UARCH_sparc64) && defined(PROCESSOR_sun4v)
	kbd_add_dev(&niagara_port, &stty_ctl);
#endif
#if defined(UARCH_sparc64) && defined(MACHINE_serengeti)
	kbd_add_dev(&sgcn_port, &stty_ctl);
#endif
#if defined(UARCH_sparc64) && defined(MACHINE_generic)
	kbd_add_dev(&z8530_port, &sun_ctl);
	kbd_add_dev(&ns16550_port, &sun_ctl);
#endif
	/* Silence warning on abs32le about kbd_add_dev() being unused */
	(void) kbd_add_dev;
}

static void kbd_devs_yield(void)
{
	/* For each keyboard device */
	list_foreach(kbd_devs, kdev_link) {
		kbd_dev_t *kdev = list_get_instance(kdev_link, kbd_dev_t,
		    kbd_devs);

		/* Yield port */
		if (kdev->port_ops != NULL)
			(*kdev->port_ops->yield)();
	}
}

static void kbd_devs_reclaim(void)
{
	/* For each keyboard device */
	list_foreach(kbd_devs, kdev_link) {
		kbd_dev_t *kdev = list_get_instance(kdev_link, kbd_dev_t,
		    kbd_devs);

		/* Reclaim port */
		if (kdev->port_ops != NULL)
			(*kdev->port_ops->reclaim)();
	}
}

/** Periodically check for new input devices.
 *
 * Looks under /dev/class/keyboard and /dev/class/mouse.
 *
 * @param arg	Ignored
 */
static int dev_discovery_fibril(void *arg)
{
	char *dev_path;
	size_t kbd_id = 1;
	size_t mouse_id = 1;
	int rc;

	while (true) {
		async_usleep(DISCOVERY_POLL_INTERVAL);

		/*
		 * Check for new keyboard device
		 */
		rc = asprintf(&dev_path, "/dev/class/keyboard\\%zu", kbd_id);
		if (rc < 0)
			continue;

		if (kbd_add_kbdev(dev_path) == EOK) {
			printf(NAME ": Connected keyboard device '%s'\n",
			    dev_path);

			/* XXX Handle device removal */
			++kbd_id;
		}

		free(dev_path);

		/*
		 * Check for new mouse device
		 */
		rc = asprintf(&dev_path, "/dev/class/mouse\\%zu", mouse_id);
		if (rc < 0)
			continue;

		if (mouse_add_dev(dev_path) == EOK) {
			printf(NAME ": Connected mouse device '%s'\n",
			    dev_path);

			/* XXX Handle device removal */
			++mouse_id;
		}

		free(dev_path);
	}

	return EOK;
}

/** Start a fibril for discovering new devices. */
static void input_start_dev_discovery(void)
{
	fid_t fid;

	fid = fibril_create(dev_discovery_fibril, NULL);
	if (!fid) {
		printf(NAME ": Failed to create device discovery fibril.\n");
		return;
	}

	fibril_add_ready(fid);
}

int main(int argc, char **argv)
{
	printf("%s: HelenOS input service\n", NAME);
	
	sysarg_t fhc;
	sysarg_t obio;
	
	list_initialize(&kbd_devs);
	list_initialize(&mouse_devs);
	
	if (((sysinfo_get_value("kbd.cir.fhc", &fhc) == EOK) && (fhc))
	    || ((sysinfo_get_value("kbd.cir.obio", &obio) == EOK) && (obio)))
		irc_service = true;
	
	if (irc_service) {
		while (irc_phone < 0)
			irc_phone = service_obsolete_connect_blocking(SERVICE_IRC, 0, 0);
	}
	
	/* Add legacy keyboard devices. */
	kbd_add_legacy_devs();

	/* Add legacy (devmap-style) mouse device. */
	(void) mouse_add_dev("/dev/hid_in/mouse");
	
	/* Register driver */
	int rc = devmap_driver_register(NAME, client_connection);
	if (rc < 0) {
		printf("%s: Unable to register driver (%d)\n", NAME, rc);
		return -1;
	}
	
	char kbd[DEVMAP_NAME_MAXLEN + 1];
	snprintf(kbd, DEVMAP_NAME_MAXLEN, "%s/%s", NAMESPACE, NAME);
	
	devmap_handle_t devmap_handle;
	if (devmap_device_register(kbd, &devmap_handle) != EOK) {
		printf("%s: Unable to register device %s\n", NAME, kbd);
		return -1;
	}

	/* Start looking for new input devices */
	input_start_dev_discovery();

	printf(NAME ": Accepting connections\n");
	async_manager();

	/* Not reached. */
	return 0;
}

/**
 * @}
 */