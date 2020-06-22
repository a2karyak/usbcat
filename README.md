# usbcat

netcat for USB endpoints.

## Prerequisites

lib

## Compile

gcc -std=gnu11 -o usbcat usbcat.c -lusb-1.0

## Usage

	Usage: usbcat [-d] -v vid -p pid [-i interface] [-r read-endpoint] [-w write-endoint]
	Read or write raw data to USB endpoints. 
	  -v Vendor ID
	  -p Product ID
	  -i interface-number 
	      Use specified interface number, default 0.
	  -d, --detach
	      Detach kernel driver from the interface.
	  -r read-endpoint
	  -w write-endoint
	      Read and/or write endpoint number(s). The read endpoint should have its bit 7 set (IN endpoint).
	      If both endpoint numbers are specified, usbcat works bidirectionally.
	  -h, --help
	      Show usage







