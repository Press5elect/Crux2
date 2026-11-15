// Crux2 - USB pads through the console's ugen nodes: the console's system calls (ps5_ugen.h has the
// transport and says why it is there).
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.

#include "ps5_ugen.h"

#if defined(__PROSPERO__) || defined(__FreeBSD__)
#include <cerrno>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#if __has_include(<dev/usb/usb_ioctl.h>)
#include <dev/usb/usb.h>
#include <dev/usb/usb_ioctl.h>
// The numbers spelt out in ps5_ugen.h, against the SDK's FreeBSD header.
static_assert(PS5Ugen::FS_START == USB_FS_START && PS5Ugen::FS_STOP == USB_FS_STOP && PS5Ugen::FS_COMPLETE == USB_FS_COMPLETE &&
				  PS5Ugen::FS_INIT == USB_FS_INIT && PS5Ugen::FS_UNINIT == USB_FS_UNINIT && PS5Ugen::FS_OPEN == USB_FS_OPEN &&
				  PS5Ugen::FS_CLOSE == USB_FS_CLOSE && PS5Ugen::DO_REQUEST == USB_DO_REQUEST &&
				  PS5Ugen::GET_FULL_DESC == USB_GET_FULL_DESC && PS5Ugen::DEVICE_INFO == USB_GET_DEVICEINFO &&
				  PS5Ugen::IFACE_DRIVER_ACTIVE == USB_IFACE_DRIVER_ACTIVE && PS5Ugen::IFACE_DRIVER_DETACH == USB_IFACE_DRIVER_DETACH,
	"FreeBSD's USB ioctls as spelt out in ps5_ugen.h");
static_assert(sizeof(struct usb_fs_endpoint) == sizeof(PS5Ugen::FsEndpoint) &&
				  sizeof(struct usb_ctl_request) == sizeof(PS5Ugen::CtlRequest) &&
				  sizeof(struct usb_fs_open) == sizeof(PS5Ugen::FsOpen) &&
				  sizeof(struct usb_gen_descriptor) == sizeof(PS5Ugen::GenDescriptor),
	"FreeBSD's USB structures as spelt out in ps5_ugen.h");
#endif

namespace
{
	int Open(const char* path, bool nonblocking)
	{
		const int fd = ::open(path, O_RDWR | (nonblocking ? O_NONBLOCK : 0));
		return fd >= 0 ? fd : -errno;
	}
	int Close(int fd) { return ::close(fd) == 0 ? 0 : -errno; }
	int Ioctl(int fd, unsigned long request, void* arg) { return ::ioctl(fd, request, arg) == 0 ? 0 : -errno; }
	void SleepUs(unsigned us) { ::usleep(us); }
	uint64_t NowMs()
	{
		timespec t{};
		clock_gettime(CLOCK_MONOTONIC, &t);
		return static_cast<uint64_t>(t.tv_sec) * 1000u + static_cast<uint64_t>(t.tv_nsec) / 1000000u;
	}
} // namespace

PS5Ugen::SysOps PS5Ugen::ConsoleSysOps()
{
	SysOps ops;
	ops.open = Open;
	ops.close = Close;
	ops.ioctl = Ioctl;
	ops.sleep_us = SleepUs;
	ops.now_ms = NowMs;
	return ops;
}
#else
PS5Ugen::SysOps PS5Ugen::ConsoleSysOps()
{
	return {}; // no ugen nodes here: Start says no
}
#endif
