/* Nagios check for analog pressure sensors connected via an ADS1115 ADC via I2C
 *
 * Arguments:
 * -v = Verbose.  Print additional info on what the program is doing.
 * -i = Select input pin.  ADS1115 has 4 input pins.
 * -m = Minimum pressure for nagios checking
 * -M = Maximum pressure for nagios checking
 * -d = i2c device to open. Defaults to /dev/i2c-1
 * -a = Address on i2c bus of the ASD1115 in HEX. Defaults to 0x48
 * -A = Address on i2c bus of the ASD1115 in DEC. Defaults to 72
 * -l is pressure in bar at 4ma (lowest reading)
 * -h is pressure in bar at 20ma (highest reading)
 *
 * Written by Richard Allen <ra@ra.is> and released under the GPLv2 license.
 * Thanks to the University of Cambridge for documentation and example code
 * on working with the ADS1115.
 *
 * Also thanks to my friend Helgi V. Jónsson for all the math help.
 */

#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <inttypes.h>
#include <linux/i2c-dev.h>
#include <sys/ioctl.h>
#include <string.h>

#define minval 2090.0				// Reading at 4 milliampers (no pressure)
#define maxval 10630.0				// Reading at 20 milliampers (10 bar for this sensor)
#define BUFSIZE 256				// Typical charbuffer

/*
The resolution of the ADC in single ended 
mode we have 15 bit rather than 16 bit resolution, 
the 16th bit being the sign of the differential reading.
*/

int main(int argc, char **argv) {
	int c, input, fd, ads_address, verbose, nagiosexit;
	float minpressure, maxpressure, voltage, pressure;
	float slope, con, VPS, low, high;
	int16_t val;
	uint8_t writeBuf[3], readBuf[2];
	char i2cdevice[BUFSIZE];

	verbose = nagiosexit = 0;
	minpressure = maxpressure = -1.0;

	low = 0.0;	// pressure in bar at 4ma (lowest reading)
	high = 10.0;	// pressure in bar ar 20ma (highest reading)

	ads_address = 0x48;		// 0x48 is the default address on i2c bus for ads1115
	input = 1;			// Default input here is 1
	strncpy (i2cdevice, "/dev/i2c-1", BUFSIZE-1);	// Default i2c on Raspberry PI

	// Defaults above can be overridden on the command line

	while ((c = getopt (argc, argv, "vd:a:A:i:m:M:l:h:")) != -1) {
		switch (c) {
			case 'v':
				verbose = 1;
				break;
			case 'i':
				input = atoi(optarg);
				break;
			case 'm':
				minpressure = atof(optarg);
				break;
			case 'M':
				maxpressure = atof(optarg);
				break;
			case 'd':
				strncpy (i2cdevice, optarg, BUFSIZE-1);
				break;
			case 'a':
				ads_address = strtol(optarg, NULL, 16);		// base 16 (hex) input
				break;
			case 'A':
				ads_address = atoi(optarg);			// base 10 (dec) input
				break;
			case 'l':
				low = atof(optarg);
				break;
			case 'h':
				high = atof(optarg);
				break;
			default:
				printf("Usage: %s [ -v ] [ -d i2cdevice ] [ -a i2caddress ] [ -A i2caddress ] [ -i input ] [ -l lowval ] [ -h highval ] -m min -M max\n", argv[0]);
				printf("Note:\t-a is in Hexadecimal and -A is Decimal.\n");
				printf("\t -l is pressure in bar at 4ma (lowest reading)\n");
				printf("\t -h is pressure in bar at 20ma (highest reading)\n");
				exit(EXIT_FAILURE);
		}
	}

	if (input < 1 || input > 4) {
		printf("Error. Input must be 1, 2, 3 or 4\n");
		exit(EXIT_FAILURE);
	}

	if (minpressure == -1.0 || maxpressure == -1.0) {
		printf("Error. Both -m and -M options must be present!\n");
		exit(EXIT_FAILURE);
	}

	VPS = 6.144 / maxval;			// volts per step
	slope = (high - low)/(maxval - minval);	// calculate the slope
	con = slope * minval;			// constant

	if (verbose) printf("DEBUG: Device %s, Address 0x%02x (%i), Input %i\n", i2cdevice, 
		ads_address, ads_address, input);
	if (verbose) printf("DEBUG: maxval %f, minval %f, slope %f, constant %f\n", maxval, 
		minval, slope, con);

	// open i2c device
	if ((fd = open(i2cdevice, O_RDWR)) < 0) {
		printf("Error: Couldn't open device %s: %s\n", i2cdevice, strerror(errno));
		exit(EXIT_FAILURE);
	}

	// connect to ads1115 as i2c slave
	if (ioctl(fd, I2C_SLAVE, ads_address) < 0) {
		printf("Error: Couldn't find device on address: %s\n", strerror(errno));
		exit(EXIT_FAILURE);
	}

	// set config register and start conversion
	// ANC1 and GND, 4.096v, 128s/s
	writeBuf[0] = 1;	// config register is 1
	// Bits 15 - 8
	if (input == 1) {
		writeBuf[1] = 0b11000001;	// ANC0 (100)
	} else if (input == 2) {
		writeBuf[1] = 0b11010001;	// ANC1 (101)
	} else if (input == 3) {
		writeBuf[1] = 0b11100001;	// ANC2 (110)
	} else {
		writeBuf[1] = 0b11110001;	// ANC3 (111)
	}

	// bit 15 flag bit for single shot
	// Bits 14-12 input selection:
	// 100 ANC0; 101 ANC1; 110 ANC2; 111 ANC3
	// Bits 11-9 Amp gain. Default to 000
	// Bit 8 Operational mode of the ADS1115.
	// 0 : Continuous conversion mode
	// 1 : Power-down single-shot mode (default)

	writeBuf[2] = 0b10000101;	// bits 7-0  0x85
	// Bits 7-5 data rate default to 100 for 128SPS
	// Bits 4-0  comparator functions see spec sheet.

	// begin conversion
	if (write(fd, writeBuf, 3) != 3) {
		perror("Write to register 1");
		exit(EXIT_FAILURE);
	}
	// wait for conversion complete
	// checking bit 15
	do {
		if (read(fd, writeBuf, 2) != 2) {
			perror("Read conversion");
			exit(EXIT_FAILURE);
		}
	} while ((writeBuf[0] & 0x80) == 0);

	// read conversion register
	// write register pointer first
	readBuf[0] = 0;		// conversion register is 0
	if (write(fd, readBuf, 1) != 1) {
		perror("Write register select");
		exit(EXIT_FAILURE);
	}
	// read 2 bytes
	if (read(fd, readBuf, 2) != 2) {
		perror("Read conversion");
		exit(EXIT_FAILURE);
	}
	// convert display results
	val = readBuf[0] << 8 | readBuf[1];

	if (val < 0 || val > 32768)
		val = 0;

	voltage = val * VPS;	// convert to voltage
	pressure = (slope * val) - con;
	if (pressure < 0.0)
		pressure = 0.0;

	if (verbose) printf("ANC%d: HEX 0x%02x, DEC %d, voltage %4.4f, pressure %4.3f bar\n", (input - 1), val, val, voltage, pressure);

	// Check pressure against min/max values

	if (pressure < minpressure) {
		fprintf(stdout, "CRITICAL: Pressure on probe '%s:0x%02x' is %4.3f which is below %4.3f | 'pressure'=%4.4f\n",
			i2cdevice, ads_address, pressure, minpressure, pressure);
		nagiosexit = 2;
	} else if (pressure > maxpressure) {
		fprintf(stdout, "CRITICAL: Pressure on probe '%s:0x%02x' is %4.3f which is over %4.3f | 'pressure'=%4.4f\n",
			i2cdevice, ads_address, pressure, maxpressure, pressure);
		nagiosexit = 2;
	} else {
		fprintf(stdout, "OK: Pressure on probe '%s:0x%02x' is %4.3f | 'pressure'=%4.3f\n",
			i2cdevice, ads_address, pressure, pressure);
		nagiosexit = 0;
	}

	close(fd);
	exit(nagiosexit);
}
