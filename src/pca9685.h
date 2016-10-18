#define   PCA9685_MODE1          0x00
#define   PCA9685_MODE2          0x01
#define   PCA9685_SUBADR1        0x02
#define   PCA9685_SUBADR2        0x03
#define   PCA9685_SUBADR3        0x04
#define   PCA9685_PRESCALE       0xFE
#define   PCA9685_LED0_ON_L      0x06
#define   PCA9685_LED0_ON_H      0x07
#define   PCA9685_LED0_OFF_L     0x08
#define   PCA9685_LED0_OFF_H     0x09
#define   PCA9685_ALL_LED_ON_L   0xFA
#define   PCA9685_ALL_LED_ON_H   0xFB
#define   PCA9685_ALL_LED_OFF_L  0xFC
#define   PCA9685_ALL_LED_OFF_H  0xFD

#define   PCA9685_RESTART        (1<<7)
#define   PCA9685_AI             (1<<5)
#define   PCA9685_SLEEP          (1<<4)
#define   PCA9685_ALLCALL        (1<<0)

#define   PCA9685_OCH            (1<<3)
#define   PCA9685_OUTDRV         (1<<2)

