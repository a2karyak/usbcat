#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <assert.h>
#include <getopt.h>

#include <libusb-1.0/libusb.h>

enum
{
	USB_XFER_SZ = 512,
	USB_BUFS = 2, // must have at least two buffers in the queue
	USB_TIMEOUT = -1,
};

struct buffer_t
{
	struct libusb_transfer* usb_xfer;
	unsigned char *data;
};

struct buffer_queue_t
{
	/** Queue of buffers waiting for stdin/stdout. */
	struct buffer_t buf[USB_BUFS];
	unsigned buf_tail, buf_head;
	struct pollfd *pollfd;
	/** For USB -> stdout tramsmission, number of bytes in the last transfer. */
	size_t xfer_length;
	/** For USB -> stdout tramsmission, number of bytes written to stdout. */
	size_t xfer_written;
	/** True if stdin has been closed. */
	int shutdown;
	/** True if unrecoverable error occurred. */
	int error;
};

static int diag(const char *context, int ok)
{
	if (!ok)
	{
		char buf[256];
#if (_POSIX_C_SOURCE >= 200112L || _XOPEN_SOURCE >= 600) && !_GNU_SOURCE
		strerror_r(errno, buf, sizeof(buf) / sizeof(buf[0]));
		fprintf(stderr, "%s: %d, %s\n", context, errno, buf);
#else
		fprintf(stderr, "%s: %d, %s\n", context, errno, strerror_r(errno, buf, sizeof buf / sizeof buf[0]));
#endif
		exit(1);
	}
	return ok;
}

static int non_blocking(int fd)
{
	int flags = fcntl(fd, F_GETFL, 0);
	if (diag("F_GETFL", flags != -1))
	{
		flags |= O_NONBLOCK;
		if (diag("F_SETFL", fcntl(fd, F_SETFL, flags) != -1))
			return 0;
	}
	return -1;
}

static void LIBUSB_CALL usb_callback(struct libusb_transfer *xfer)
{
	int ret;

	struct buffer_queue_t *q = (struct buffer_queue_t *) xfer->user_data;
	struct buffer_t *b = &q->buf[q->buf_tail];

	// Check if the queue has space for one more buffer.
	unsigned buf_tail = (q->buf_tail + 1) % USB_BUFS;
	assert(q->buf_head != buf_tail);

	switch (xfer->status)
	{
	case LIBUSB_TRANSFER_COMPLETED:
		// add the buffer to the queue tail
		b->usb_xfer = xfer;
		b->data = xfer->buffer;
		if (q->buf_head == q->buf_tail)
		{
			unsigned mask = (xfer->endpoint & LIBUSB_ENDPOINT_DIR_MASK) ? POLLOUT : POLLIN;
			assert(!(q->pollfd->events & mask));
			if (!q->shutdown)
			{
				// If it is the first buffer, unblock the other side.
				q->pollfd->events |= mask;
				// Also, set up the pointers, necessary for writing only.
				q->xfer_written = 0;
				q->xfer_length = xfer->actual_length;
			}
		}
		q->buf_tail = buf_tail;
		break;
	case LIBUSB_TRANSFER_TIMED_OUT:
		ret = libusb_submit_transfer(xfer);
		if (ret != LIBUSB_SUCCESS)
		{
			fprintf(stderr, "Error re-submitting timed-out transfer: %s\n", libusb_error_name(ret));
			exit(1);
		}
		break;
	case LIBUSB_TRANSFER_ERROR:
	case LIBUSB_TRANSFER_CANCELLED:
	case LIBUSB_TRANSFER_STALL:
	case LIBUSB_TRANSFER_NO_DEVICE:
	case LIBUSB_TRANSFER_OVERFLOW:
	default:
		fprintf(stderr, "USB transfer failed on endpoint 0x%2.2x: %s\n", xfer->endpoint, libusb_error_name(xfer->status));
		q->error = 1;
		break;
	}
}

struct libusb_device_handle *open_device(libusb_context *ctx,
		unsigned vid, unsigned pid)
{
	struct libusb_device **devices;
	ssize_t cnt, i;
	int ret;

	cnt = libusb_get_device_list(ctx, &devices);

	if (cnt == 0)
	{
		fprintf(stderr, "Error getting USB device list\n");
		return NULL;
	}

	struct libusb_device_handle *h = NULL;
	for (i = 0; i != cnt - 1; i++)
	{
		struct libusb_device_descriptor dev_desc;

		if (libusb_get_device_descriptor(devices[i], &dev_desc) != 0)
			continue;

		if (dev_desc.idVendor == vid &&
				dev_desc.idProduct == pid)
		{
			ret = libusb_open(devices[i], &h);

			if (ret == 0)
				break;

			fprintf(stderr, "Error opening device: %s", libusb_error_name(ret));
		}
	}

	libusb_free_device_list(devices, 1);

	return h;
}

static void usage(int help)
{
	fprintf(stderr, "Usage: usbcat [-d] -v vid -p pid [-i interface] [-r read-endpoint] [-w write-endoint]\n");
	if (help)
	{
		printf("Read or write raw data to USB endpoints. \n"
		"  -v Vendor ID\n"
		"  -p Product ID\n"
		"  -i interface-number \n"
		"      Use specified interface number, default 0.\n"
		"  -d, --detach\n"
		"      Detach kernel driver from the interface.\n"
		"  -r read-endpoint\n"
		"  -w write-endoint\n"
		"      Read and/or write endpoint number(s). The read endpoint should have its bit 7 set (IN endpoint).\n"
		"      If both endpoint numbers are specified, usbcat works bidirectionally.\n"
		"  -h, --help\n"
		"      Show usage\n");
	}
}

int main(int argc, char *argv[])
{
	int ret = EXIT_SUCCESS;

	int help = 0, detach = 0;
	unsigned vid = 0;
	unsigned pid = 0;
	unsigned int_no = 0;
	int read_ep = -1;
	int write_ep = -1;
	int fd_in = STDIN_FILENO;
	int fd_out = STDOUT_FILENO;

	static const struct option long_options[] = {
		{ "detach", 0, 0, 'd' },
		{ "help", 0, 0, 'h' },
		{ 0, 0, 0, 0 }
	};

	char c;
	while ((c = getopt_long(argc, argv, "v:p:i:dr:w:h",
			long_options, NULL)) != -1)
		switch (c)
		{
		case 'v':
			vid = strtoul(optarg, NULL, 0);
			break;
		case 'p':
			pid = strtoul(optarg, NULL, 0);
			break;
		case 'i':
			int_no = strtoul(optarg, NULL, 0);
			break;
		case 'd':
			detach = 1;
			break;
		case 'r':
			read_ep = strtoul(optarg, NULL, 0);
			break;
		case 'w':
			write_ep = strtoul(optarg, NULL, 0);
			break;
		case 'h':
			help = 1;
			break;
		case '?':
			ret = EXIT_FAILURE;
		}

	if (help)
	{
		usage(1);
		return ret;
	}

	if (vid == 0 || pid == 0)
	{
		fprintf(stderr, "Vendor ID and product ID must be specified.\n");
		ret = EXIT_FAILURE;
	}

	if (read_ep < 0 && write_ep < 0)
	{
		fprintf(stderr, "At least one endpoint number must be specified.\n");
		ret = EXIT_FAILURE;
	}

	if (ret != EXIT_SUCCESS)
	{
		usage(0);
		return ret;
	}

	ret = libusb_init(NULL);
	if (ret != LIBUSB_SUCCESS)
	{
		fprintf(stderr, "Error initializing libusb: %s\n", libusb_error_name(ret));
		return EXIT_FAILURE;
	}

	struct libusb_device_handle *usb_dev = open_device(NULL, vid, pid);
	if (!usb_dev)
	{
		fprintf(stderr, "Error finding USB device\n");
		return EXIT_FAILURE;
	}

	if (detach)
	{
		ret = libusb_detach_kernel_driver(usb_dev, int_no);
		if (ret != LIBUSB_SUCCESS)
		{
			fprintf(stderr, "Error detaching the kernel driver from the interface: %s\n",
					libusb_error_name(ret));
		}
	}

	ret = libusb_claim_interface(usb_dev, int_no);
	if (ret != LIBUSB_SUCCESS)
	{
		fprintf(stderr, "Error claiming interface: %s\n", libusb_error_name(ret));
		return EXIT_FAILURE;
	}

	const struct libusb_pollfd **usb_fds = libusb_get_pollfds(NULL);

	const struct libusb_pollfd **it = usb_fds;
	for (; *it != NULL; ++it);

	unsigned num_usb_fds = it - usb_fds;
	unsigned fd_num = 0;

	struct pollfd *fds = (struct pollfd *)malloc(sizeof(struct pollfd) * (2 + num_usb_fds));

	struct buffer_queue_t usb_write_buf, usb_read_buf;

	{
		struct buffer_queue_t *q = &usb_write_buf;

		q->buf_head = 0;
		q->shutdown = 0;
		q->error = 0;

		non_blocking(fd_in);

		if (write_ep >= 0)
		{
			q->buf_tail = USB_BUFS - 1;
			q->pollfd = &fds[fd_num];

			for (unsigned i = 0; i != USB_BUFS - 1; ++i)
			{
				struct buffer_t *b = &q->buf[i];
				b->usb_xfer = libusb_alloc_transfer(0);
				b->data = (unsigned char *)malloc(USB_XFER_SZ);
			}

			fds[fd_num].fd = fd_in;
			fds[fd_num].events = POLLIN | POLLHUP | POLLERR;
			fd_num++;
		}
		else
		{
			q->buf_tail = 0;
			q->pollfd = NULL;
		}
	}

	{
		struct buffer_queue_t *q = &usb_read_buf;

		q->buf_head = 0;
		q->buf_tail = 0;
		q->shutdown = 0;
		q->error = 0;

		non_blocking(fd_out);

		if (read_ep >= 0)
		{
			q->pollfd = &fds[fd_num];

			for (unsigned i = 0; i != USB_BUFS - 1; ++i)
			{
				struct buffer_t *b = &q->buf[i];
				struct libusb_transfer* xfer = libusb_alloc_transfer(0);
				unsigned char *data = (unsigned char *)malloc(USB_XFER_SZ);

				libusb_fill_bulk_transfer(xfer, usb_dev, read_ep, data, USB_XFER_SZ, usb_callback, q, USB_TIMEOUT);

				ret = libusb_submit_transfer(xfer);
				if (ret != LIBUSB_SUCCESS)
				{
					fprintf(stderr, "Error submitting transfer: %s\n", libusb_error_name(ret));
					return EXIT_FAILURE;
				}
			}

			fds[fd_num].fd = fd_out;
			fds[fd_num].events = POLLHUP | POLLERR; // no polling of stdout until there is data to send
			fd_num++;
		}
		else
		{
			q->pollfd = NULL;
		}
	}

	// Copy USB fds to the poll fd array.
	for (unsigned i = 0; i != num_usb_fds; ++i)
	{
		fds[fd_num].fd = usb_fds[i]->fd;
		fds[fd_num].events = usb_fds[i]->events;
		fd_num++;
	}

	libusb_free_pollfds(usb_fds);

	// It would be nice to cancel outstanding IN requests and wait for their completion,
	// but this would require remembering them in the first place.
	while (!usb_write_buf.error
			&& !usb_read_buf.error
			&& (!usb_write_buf.shutdown
				|| (usb_write_buf.buf_tail + 1) % USB_BUFS != usb_write_buf.buf_head))
	{
		ret = poll(fds, fd_num, -1);
		if (ret > 0)
		{
			for (unsigned i = 0; i != fd_num && ret != 0; ++i)
			{
				if (fds[i].events & fds[i].revents)
				{
					if (fds[i].fd == fd_in)
					{
						struct buffer_queue_t *q = &usb_write_buf;

						if (fds[i].revents & POLLIN)
						{
							// check if there are spare buffers in the USB queue
							assert(q->buf_head != q->buf_tail);

							struct buffer_t *b = &q->buf[q->buf_head];

							ssize_t ret = read(fds[i].fd, b->data, USB_XFER_SZ / 2);

							if (ret > 0)
							{
								libusb_fill_bulk_transfer(b->usb_xfer, usb_dev, write_ep, b->data, ret, usb_callback, q,
									USB_TIMEOUT);

								ret = libusb_submit_transfer(b->usb_xfer);
								if (ret != LIBUSB_SUCCESS)
								{
									fprintf(stderr, "Error submitting USB OUT transfer: %s\n", libusb_error_name(ret));
									return EXIT_FAILURE;
								}

								q->buf_head = (q->buf_head + 1) % USB_BUFS;

								// No spare buffers, block stdin
								if (q->buf_head == q->buf_tail)
									fds[i].events &= ~POLLIN;
							}
							else
							if (ret == 0)
							{
								// End of input.
								q->shutdown = 1;
							}
							else
							{
								if (errno != EINTR)
								{
									fprintf(stderr, "Error reading input: %d\n", errno);
									return EXIT_FAILURE;
								}
							}
						}
						else
						{
							// POLHUP/POLLERR
							q->shutdown = 1;
						}

						ret--;
					}
					else
					if (fds[i].fd == fd_out)
					{
						struct buffer_queue_t *q = &usb_read_buf;

						// check if there are spare buffers in the USB queue
						assert(q->buf_head != q->buf_tail);

						struct buffer_t *b = &q->buf[q->buf_head];
						ssize_t ret = write(q->pollfd->fd, b->data + q->xfer_written,
								q->xfer_length - q->xfer_written);

						if (ret >= 0)
						{
							q->xfer_written += ret;
							// When a buffer is fully sent to stdout, return it to USB and move to the next one.
							if (q->xfer_written == q->xfer_length)
							{
								libusb_fill_bulk_transfer(b->usb_xfer, usb_dev, read_ep, b->data, USB_XFER_SZ,
										usb_callback, q, USB_TIMEOUT);

								ret = libusb_submit_transfer(b->usb_xfer);
								if (ret != LIBUSB_SUCCESS)
								{
									fprintf(stderr, "Error submitting USB IN transfer: %s\n", libusb_error_name(ret));
									return EXIT_FAILURE;
								}

								q->buf_head = (q->buf_head + 1) % USB_BUFS;
								if (q->buf_head != q->buf_tail)
								{
									q->xfer_written = 0;
									q->xfer_length = q->buf[q->buf_head].usb_xfer->actual_length;
								}
								else
									fds[i].events &= ~POLLOUT;
							}
						}
						else
						{
							if (errno != EINTR)
							{
								fprintf(stderr, "Error writing output: %d\n", errno);
								return EXIT_FAILURE;
							}
						}

						ret--;
					}
					else
					{
						// Check out any USB events.
						struct timeval timeout = { 0, 0 };
						ret = libusb_handle_events_timeout(NULL, &timeout);
						if (ret != LIBUSB_SUCCESS)
						{
							fprintf(stderr, "Error handling libusb events: %s\n", libusb_error_name(ret));
							return EXIT_FAILURE;
						}
						break; // USB fds are the last, as soon as they are handled, we are done
					}
				}
			}
		}
		else
		{
			if (errno != EINTR)
			{
				fprintf(stderr, "Poll returned error: %d\n", errno);
				return EXIT_FAILURE;
			}
		}
	}

	libusb_release_interface(usb_dev, int_no);
	libusb_close(usb_dev);
	libusb_exit(NULL);

	return EXIT_SUCCESS;
}
