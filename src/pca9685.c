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
 * Copyright 2014 Bill Wilson <billw@krellm.net>
 */



#include "pikrellcam.h"
#include "pca9685.h"

  /* write a data byte to a PCA9685 register.
  |  Write the register address followed by the data byte.
  */
static void
pca9685_register_write(PCA9685 *ic, uint8_t reg, uint8_t data)
	{
	uint8_t buf[2];

	buf[0] = reg;
	buf[1] = data;
	if (write(ic->fd, buf, 2) != 2)
		printf("pca9685_register_write(0x%x, 0x%x) failed: %s\n",
				reg, data, strerror(errno));
	}


  /* Set the modulation frequency of the LED outputs.
  |  The pre scale value can range from 3 (hardware enforced) giving a
  |  max modulation frequency of 1525 to 255 giving a min modulation
  |  frequency of 24.
  |  PRE_SCALE register can be set only when the SLEEP bit of MODE1 register
  |  is set to 1 and then the SLEEP bit must be reset and the oscillator
  |  allowed to settle at least 500uS before restarting the LED outputs.
  */
void
pca9685_set_modulation_frequency(PCA9685 *ic, int freq)
	{
	uint8_t	pre_scale;

	if (!ic || ic->fd < 0)
		return;

	pre_scale = (PCA9685_CLOCK_FREQ / (4096 * freq)) - 1;

	pca9685_register_write(ic, PCA9685_REG_MODE1,
						PCA9685_MODE1_DEFAULT | PCA9685_MODE1_SLEEP);
	pca9685_register_write(ic, PCA9685_REG_PRE_SCALE, pre_scale);
	pca9685_register_write(ic, PCA9685_REG_MODE1, PCA9685_MODE1_DEFAULT);

	/* After setting SLEEP bit to zero, allow oscillator to stabilize
	|  at least 500uS before restarting.
	*/
	usleep(1000);

	pca9685_register_write(ic, PCA9685_REG_MODE1,
						PCA9685_MODE1_DEFAULT | PCA9685_MODE1_RESTART);
	}


  /* Set the PWM duty cycle on and off counts within each cycle of the
  |  modulation frequency.  The on/off registers are 12 bits so the on/off
  |  counts range from 0 to 4095.
  |  Examples:
  |		50% duty cycle 0%  phase shift => on 0     off 2047
  |		20% duty cycle 10% phase shift => on 409   off 409 + 819 = 1228
  */
void
pca9685_set_PWM(PCA9685 *ic, uint8_t led, int on_count, int off_count)
	{
	if (!ic || ic->fd < 0)
		return;
	pca9685_register_write(ic, PCA9685_REG_LED_ON_L(led), on_count & 0xFF);
	pca9685_register_write(ic, PCA9685_REG_LED_ON_H(led), on_count >> 8);

	pca9685_register_write(ic, PCA9685_REG_LED_OFF_L(led), off_count & 0xFF);
	pca9685_register_write(ic, PCA9685_REG_LED_OFF_H(led), off_count >> 8);
	}


  /* Set an LED off count to get an output duty cycle as a percent.
  |  Start count is zero, so no phase shift.
  */
void
pca9685_set_PWM_percent(PCA9685 *ic, uint8_t led, float percent)
	{
	int	off_count;

	if (percent < 0.0)
		percent = 0.0;
	else if (percent > 100.0)
		percent = 100.0;
	off_count = (int) roundf(4095.0 * percent / 100.0);

	pca9685_set_PWM(ic, led, 0, off_count);
	}


  /* Set an LED full on.
  */
void
pca9685_set_on(PCA9685 *ic, uint8_t led)
	{
	if (!ic || ic->fd < 0)
		return;
	pca9685_register_write(ic, PCA9685_REG_LED_ON_L(led), 0);
	pca9685_register_write(ic, PCA9685_REG_LED_ON_H(led), 0x10);

	pca9685_register_write(ic, PCA9685_REG_LED_OFF_L(led), 0);
	pca9685_register_write(ic, PCA9685_REG_LED_OFF_H(led), 0);
	}

  /* Set an LED full off.
  */
void
pca9685_set_off(PCA9685 *ic, uint8_t led)
	{
	if (!ic || ic->fd < 0)
		return;
	pca9685_register_write(ic, PCA9685_REG_LED_ON_L(led), 0);
	pca9685_register_write(ic, PCA9685_REG_LED_ON_H(led), 0);

	pca9685_register_write(ic, PCA9685_REG_LED_OFF_L(led), 0);
	pca9685_register_write(ic, PCA9685_REG_LED_OFF_H(led), 0x10);
	}

/*
int
i2c_device_open(char *device, int i2c_address)
	{
	int 	fd;

	if ((fd = open(device, O_RDWR)) < 0)
		{
		printf("I2C device %s open failed: %s\n", device, strerror(errno));
		exit(EXIT_FAILURE);
		}
	if (ioctl(fd, I2C_SLAVE, i2c_address) < 0)
		{
		printf("%s I2C slave address 0x%x failed: %s\n",
			device, i2c_address, strerror(errno));
		exit(EXIT_FAILURE);
		}
	return fd;
	}
*/

PCA9685 * pca9685_setup(char *i2c_device, int i2c_address)
	{
        PCA9685 *pwm = malloc(sizeof(PCA9685));

        pwm->i2c_address = i2c_address;
        pwm->fd = i2c_open(i2c_device, i2c_address);
        pca9685_register_write(pwm, PCA9685_REG_MODE1, PCA9685_MODE1_DEFAULT);
        pca9685_register_write(pwm, PCA9685_REG_MODE2, PCA9685_MODE2_OCH);
        usleep(500);

        return pwm;
        }
