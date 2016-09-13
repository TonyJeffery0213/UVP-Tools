/******************************************************************************
 * Talks to Xen Store to figure out what devices we have (backend half).
 *
 * Copyright (C) 2005 Rusty Russell, IBM Corporation
 * Copyright (C) 2005 Mike Wray, Hewlett-Packard
 * Copyright (C) 2005, 2006 XenSource Ltd
 * Copyright (C) 2007 Solarflare Communications, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation; or, when distributed
 * separately from the Linux kernel or incorporated into other
 * software packages, subject to the following license:
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this source file (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy, modify,
 * merge, publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#define DPRINTK(fmt, args...)				\
	pr_debug("xenbus_probe (%s:%d) " fmt ".\n",	\
		 __func__, __LINE__, ##args)

#include <linux/kernel.h>
#include <linux/version.h>
#include <linux/err.h>
#include <linux/string.h>
#include <linux/ctype.h>
#include <linux/fcntl.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/notifier.h>

#include <asm/page.h>
#include <asm/pgtable.h>
#ifndef CONFIG_XEN
#include <asm/xen/hypervisor.h>
#endif
#include <asm/hypervisor.h>
#include <xen/xenbus.h>
#ifdef CONFIG_XEN
#include <xen/xen_proc.h>
#include <xen/evtchn.h>
#endif
#include <xen/features.h>

#include "xenbus_comms.h"
#include "xenbus_probe.h"

#ifdef HAVE_XEN_PLATFORM_COMPAT_H
#include <xen/platform-compat.h>
#endif

/* backend/<type>/<fe-uuid>/<id> => <type>-<fe-domid>-<id> */
static int backend_bus_id(char bus_id[XEN_BUS_ID_SIZE], const char *nodename)
{
	int domid, err;
	const char *devid, *type, *frontend;
	unsigned int typelen;

	type = strchr(nodename, '/');
	if (!type)
		return -EINVAL;
	type++;
	typelen = strcspn(type, "/");
	if (!typelen || type[typelen] != '/')
		return -EINVAL;

	devid = strrchr(nodename, '/') + 1;

	err = xenbus_gather(XBT_NIL, nodename, "frontend-id", "%i", &domid,
			    "frontend", NULL, &frontend,
			    NULL);
	if (err)
		return err;
	if (strlen(frontend) == 0)
		err = -ERANGE;
	if (!err && !xenbus_exists(XBT_NIL, frontend, ""))
		err = -ENOENT;
	kfree(frontend);

	if (err)
		return err;

	if (snprintf(bus_id, XEN_BUS_ID_SIZE, "%.*s-%i-%s",
		     typelen, type, domid, devid) >= XEN_BUS_ID_SIZE)
		return -ENOSPC;
	return 0;
}

static int xenbus_uevent_backend(struct device *dev,
				 struct kobj_uevent_env *env)
{
	struct xenbus_device *xdev;
	struct xenbus_driver *drv;
	struct xen_bus_type *bus;

	DPRINTK("");

	if (dev == NULL)
		return -ENODEV;

	xdev = to_xenbus_device(dev);
	bus = container_of(xdev->dev.bus, struct xen_bus_type, bus);
	if (xdev == NULL)
		return -ENODEV;

	/* stuff we want to pass to /sbin/hotplug */
	if (add_uevent_var(env, "XENBUS_TYPE=%s", xdev->devicetype))
		return -ENOMEM;

	if (add_uevent_var(env, "XENBUS_PATH=%s", xdev->nodename))
		return -ENOMEM;

	if (add_uevent_var(env, "XENBUS_BASE_PATH=%s", bus->root))
		return -ENOMEM;

	if (dev->driver) {
		drv = to_xenbus_driver(dev->driver);
		if (drv && drv->uevent)
			return drv->uevent(xdev, env);
	}

	return 0;
}

/* backend/<typename>/<frontend-uuid>/<name> */
static int xenbus_probe_backend_unit(struct xen_bus_type *bus,
				     const char *dir,
				     const char *type,
				     const char *name)
{
	char *nodename;
	int err;

	nodename = kasprintf(GFP_KERNEL, "%s/%s", dir, name);
	if (!nodename)
		return -ENOMEM;

	DPRINTK("%s\n", nodename);

	err = xenbus_probe_node(bus, type, nodename);
	kfree(nodename);
	return err;
}

/* backend/<typename>/<frontend-domid> */
static int xenbus_probe_backend(struct xen_bus_type *bus, const char *type,
				const char *domid)
{
	char *nodename;
	int err = 0;
	char **dir;
	unsigned int i, dir_n = 0;

	DPRINTK("");

	nodename = kasprintf(GFP_KERNEL, "%s/%s/%s", bus->root, type, domid);
	if (!nodename)
		return -ENOMEM;

	dir = xenbus_directory(XBT_NIL, nodename, "", &dir_n);
	if (IS_ERR(dir)) {
		kfree(nodename);
		return PTR_ERR(dir);
	}

	for (i = 0; i < dir_n; i++) {
		err = xenbus_probe_backend_unit(bus, nodename, type, dir[i]);
		if (err)
			break;
	}
	kfree(dir);
	kfree(nodename);
	return err;
}

#ifndef CONFIG_XEN
static void frontend_changed(struct xenbus_watch *watch,
			    const char **vec, unsigned int len)
{
	xenbus_otherend_changed(watch, vec, len, 0);
}
#endif

static struct device_attribute xenbus_backend_dev_attrs[] = {
	__ATTR_NULL
};

static struct xen_bus_type xenbus_backend = {
	.root = "backend",
	.levels = 3,		/* backend/type/<frontend>/<id> */
	.get_bus_id = backend_bus_id,
	.probe = xenbus_probe_backend,
#ifndef CONFIG_XEN
	.otherend_changed = frontend_changed,
#else
	.dev = {
		.init_name = "xen-backend",
	},
#endif
	.error = -ENODEV,
	.bus = {
		.name		= "xen-backend",
		.match		= xenbus_match,
		.uevent		= xenbus_uevent_backend,
		.probe		= xenbus_dev_probe,
		.remove		= xenbus_dev_remove,
#ifdef CONFIG_XEN
		.shutdown	= xenbus_dev_shutdown,
#endif
		.dev_attrs	= xenbus_backend_dev_attrs,
	},
};

static void backend_changed(struct xenbus_watch *watch,
			    const char **vec, unsigned int len)
{
	DPRINTK("");

	xenbus_dev_changed(vec[XS_WATCH_PATH], &xenbus_backend);
}

static struct xenbus_watch be_watch = {
	.node = "backend",
	.callback = backend_changed,
};

static int read_frontend_details(struct xenbus_device *xendev)
{
	return xenbus_read_otherend_details(xendev, "frontend-id", "frontend");
}

#ifndef CONFIG_XEN
int xenbus_dev_is_online(struct xenbus_device *dev)
{
	int rc, val;

	rc = xenbus_scanf(XBT_NIL, dev->nodename, "online", "%d", &val);
	if (rc != 1)
		val = 0; /* no online node present */

	return val;
}
EXPORT_SYMBOL_GPL(xenbus_dev_is_online);
#endif

int xenbus_register_backend(struct xenbus_driver *drv)
{
	drv->read_otherend_details = read_frontend_details;

	return xenbus_register_driver_common(drv, &xenbus_backend);
}
EXPORT_SYMBOL_GPL(xenbus_register_backend);

#if defined(CONFIG_XEN) && defined(CONFIG_PM_SLEEP)
void xenbus_backend_suspend(int (*fn)(struct device *, void *))
{
	DPRINTK("");
	if (!xenbus_backend.error)
		bus_for_each_dev(&xenbus_backend.bus, NULL, NULL, fn);
}

void xenbus_backend_resume(int (*fn)(struct device *, void *))
{
	DPRINTK("");
	if (!xenbus_backend.error)
		bus_for_each_dev(&xenbus_backend.bus, NULL, NULL, fn);
}
#endif

#ifndef CONFIG_XEN
static int backend_probe_and_watch(struct notifier_block *notifier,
				   unsigned long event,
				   void *data)
#else
void xenbus_backend_probe_and_watch(void)
#endif
{
	/* Enumerate devices in xenstore and watch for changes. */
	xenbus_probe_devices(&xenbus_backend);
	register_xenbus_watch(&be_watch);

#ifndef CONFIG_XEN
	return NOTIFY_DONE;
#endif
}

#ifndef CONFIG_XEN

static int __init xenbus_probe_backend_init(void)
{
	static struct notifier_block xenstore_notifier = {
		.notifier_call = backend_probe_and_watch
	};
	int err;

	DPRINTK("");

	/* Register ourselves with the kernel bus subsystem */
	err = bus_register(&xenbus_backend.bus);
	if (err)
		return err;

	register_xenstore_notifier(&xenstore_notifier);

	return 0;
}
subsys_initcall(xenbus_probe_backend_init);

#else

void __init xenbus_backend_bus_register(void)
{
	xenbus_backend.error = bus_register(&xenbus_backend.bus);
	if (xenbus_backend.error)
		pr_warning("XENBUS: Error registering backend bus: %i\n",
			   xenbus_backend.error);
}

void __init xenbus_backend_device_register(void)
{
	if (xenbus_backend.error)
		return;

	xenbus_backend.error = device_register(&xenbus_backend.dev);
	if (xenbus_backend.error) {
		bus_unregister(&xenbus_backend.bus);
		pr_warning("XENBUS: Error registering backend device: %i\n",
			   xenbus_backend.error);
	}
}

int xenbus_for_each_backend(void *arg, int (*fn)(struct device *, void *))
{
	return bus_for_each_dev(&xenbus_backend.bus, NULL, arg, fn);
}
EXPORT_SYMBOL_GPL(xenbus_for_each_backend);

#endif
