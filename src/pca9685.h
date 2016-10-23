/*
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *
 */

#ifndef _PCA9685_H
#define _PCA9685_H

#define PCA9685_I2C_ADDRESS			0x40

/* PCA9685 Register Definitions - from data sheet Section 7.3
*/

#define PCA9685_REG_MODE1			0x0		/*	*/
#define PCA9685_REG_MODE2			0x1		/*	*/
#define PCA9685_REG_SUBADR1			0x2
#define PCA9685_REG_SUBADR2			0x3
#define PCA9685_REG_SUBADR3			9x4
#define PCA9685_REG_ALLCALLADR		0x5

/* LED output and brightness control registers - 4 registers for each LED
*/
#define PCA9685_LED_START_REGISTER	0x6		/* Start of LED registers	*/
#define	PCA9685_LED_REGISTER_SIZE	4		/* 4 registers for each LED	*/

#define	PCA9685_REG_LED_ON_L(led)	(PCA9685_LED_START_REGISTER + \
										led * PCA9685_LED_REGISTER_SIZE)
#define	PCA9685_REG_LED_ON_H(led)	(PCA9685_REG_LED_ON_L(led) + 1)
#define	PCA9685_REG_LED_OFF_L(led)	(PCA9685_REG_LED_ON_L(led) + 2)
#define	PCA9685_REG_LED_OFF_H(led)	(PCA9685_REG_LED_ON_L(led) + 3)

/* Registers to control all LEDs simultaneously
*/
#define PCA9685_REG_ALL_LED_ON_L	0xFA	/* LEDs 0-7 on		*/
#define PCA9685_REG_ALL_LED_ON_H	0xFB	/* LEDs 8-15 on		*/
#define PCA9685_REG_ALL_LED_OFF_L	0xFC	/* LEDs 0-7 off		*/
#define PCA9685_REG_ALL_LED_OFF_H	0xFD	/* LEDs 8-15 off	*/

#define PCA9685_REG_PRE_SCALE		0xFE	/* for modulation frequency*/


#define PCA9685_CLOCK_FREQ			25000000	/* 25MHz internal clock */


/* MODE1 register:
|		RESTART  EXTCLOCK  AI  SLEEP  SUB1  SUB2  SUB3  ALLCALL
*/
#define	PCA9685_MODE1_DEFAULT		0
#define	PCA9685_MODE1_SLEEP			0x10
#define	PCA9685_MODE1_RESTART		0x80

/* MODE2 register:
|		X  X  X  INVRT  OCH  OUTDRV  OUTNE1  OUTNE0
*/
#define	PCA9685_MODE2_OCH		0x4		/* If set, outputs change after		*/
										/*  updating all four LED registers	*/
#define PCA9685_MODE2_INVRT		0x10	/* Invert output logic state 		*/


typedef struct
	{
	int		fd,
			i2c_address;
	}
	PCA9685;


PCA9685* pca9685_setup(char *i2c_device, int i2c_address);
void	pca9685_set_modulation_frequency(PCA9685 *ic, int freq);
void	pca9685_set_PWM(PCA9685 *ic, uint8_t led, int on_count, int off_count);
void	pca9685_set_PWM_percent(PCA9685 *ic, uint8_t led, float percent);
void	pca9685_set_on(PCA9685 *ic, uint8_t led);
void	pca9685_set_off(PCA9685 *ic, uint8_t led);

#endif
