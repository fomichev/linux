// SPDX-License-Identifier: GPL-2.0-or-later
#include <linux/netdevice.h>

#include "dev.h"

/**
 *	dev_change_name - change name of a device
 *	@dev: device
 *	@newname: name (or format string) must be at least IFNAMSIZ
 *
 *	Change name of a device, can pass format strings "eth%d".
 *	for wildcarding.
 */
int dev_change_name(struct net_device *dev, const char *newname)
{
	int ret;

	netdev_lock_ops(dev);
	ret = netif_change_name(dev, newname);
	netdev_unlock_ops(dev);

	return ret;
}

/**
 *	dev_set_alias - change ifalias of a device
 *	@dev: device
 *	@alias: name up to IFALIASZ
 *	@len: limit of bytes to copy from info
 *
 *	Set ifalias for a device,
 */
int dev_set_alias(struct net_device *dev, const char *alias, size_t len)
{
	int ret;

	netdev_lock_ops(dev);
	ret = netif_set_alias(dev, alias, len);
	netdev_unlock_ops(dev);

	return ret;
}
EXPORT_SYMBOL(dev_set_alias);

/**
 *	dev_change_flags - change device settings
 *	@dev: device
 *	@flags: device state flags
 *	@extack: netlink extended ack
 *
 *	Change settings on device based state flags. The flags are
 *	in the userspace exported format.
 */
int dev_change_flags(struct net_device *dev, unsigned int flags,
		     struct netlink_ext_ack *extack)
{
	int ret;

	netdev_lock_ops(dev);
	ret = netif_change_flags(dev, flags, extack);
	netdev_unlock_ops(dev);

	return ret;
}
EXPORT_SYMBOL(dev_change_flags);

/**
 *	dev_set_group - Change group this device belongs to
 *	@dev: device
 *	@new_group: group this device should belong to
 */
void dev_set_group(struct net_device *dev, int new_group)
{
	netdev_lock_ops(dev);
	netif_set_group(dev, new_group);
	netdev_unlock_ops(dev);
}

int dev_set_mac_address_user(struct net_device *dev, struct sockaddr *sa,
			     struct netlink_ext_ack *extack)
{
	int ret;

	netdev_lock_ops(dev);
	ret = netif_set_mac_address_user(dev, sa, extack);
	netdev_unlock_ops(dev);

	return ret;
}
EXPORT_SYMBOL(dev_set_mac_address_user);

/**
 *	dev_change_net_namespace - move device to different nethost namespace
 *	@dev: device
 *	@net: network namespace
 *	@pat: If not NULL name pattern to try if the current device name
 *	      is already taken in the destination network namespace.
 *	@new_ifindex: If not zero, specifies device index in the target
 *	              namespace.
 *
 *	This function shuts down a device interface and moves it
 *	to a new network namespace. On success 0 is returned, on
 *	a failure a netagive errno code is returned.
 *
 *	Callers must hold the rtnl semaphore.
 */
int dev_change_net_namespace(struct net_device *dev, struct net *net,
			     const char *pat)
{
	int ret;

	netdev_lock_ops(dev);
	ret = netif_change_net_namespace(dev, net, pat, 0);
	netdev_unlock_ops(dev);

	return ret;
}
EXPORT_SYMBOL_GPL(dev_change_net_namespace);

/**
 *	dev_change_carrier - Change device carrier
 *	@dev: device
 *	@new_carrier: new value
 *
 *	Change device carrier
 */
int dev_change_carrier(struct net_device *dev, bool new_carrier)
{
	int ret;

	netdev_lock_ops(dev);
	ret = netif_change_carrier(dev, new_carrier);
	netdev_unlock_ops(dev);

	return ret;
}

/**
 *	dev_change_tx_queue_len - Change TX queue length of a netdevice
 *	@dev: device
 *	@new_len: new tx queue length
 */
int dev_change_tx_queue_len(struct net_device *dev, unsigned long new_len)
{
	int ret;

	netdev_lock_ops(dev);
	ret = netif_change_tx_queue_len(dev, new_len);
	netdev_unlock_ops(dev);

	return ret;
}

/**
 *	dev_change_proto_down - set carrier according to proto_down.
 *
 *	@dev: device
 *	@proto_down: new value
 */
int dev_change_proto_down(struct net_device *dev, bool proto_down)
{
	int ret;

	netdev_lock_ops(dev);
	ret = netif_change_proto_down(dev, proto_down);
	netdev_unlock_ops(dev);

	return ret;
}

/**
 *	dev_open	- prepare an interface for use.
 *	@dev: device to open
 *	@extack: netlink extended ack
 *
 *	Takes a device from down to up state. The device's private open
 *	function is invoked and then the multicast lists are loaded. Finally
 *	the device is moved into the up state and a %NETDEV_UP message is
 *	sent to the netdev notifier chain.
 *
 *	Calling this function on an active interface is a nop. On a failure
 *	a negative errno code is returned.
 */
int dev_open(struct net_device *dev, struct netlink_ext_ack *extack)
{
	int ret;

	netdev_lock_ops(dev);
	ret = netif_open(dev, extack);
	netdev_unlock_ops(dev);

	return ret;
}
EXPORT_SYMBOL(dev_open);

/**
 *	dev_close - shutdown an interface.
 *	@dev: device to shutdown
 *
 *	This function moves an active device into down state. A
 *	%NETDEV_GOING_DOWN is sent to the netdev notifier chain. The device
 *	is then deactivated and finally a %NETDEV_DOWN is sent to the notifier
 *	chain.
 */
void dev_close(struct net_device *dev)
{
	netdev_lock_ops(dev);
	netif_close(dev);
	netdev_unlock_ops(dev);
}
EXPORT_SYMBOL(dev_close);
