/*
    w83781d.c - Part of lm_sensors, Linux kernel modules for hardware
                monitoring
    Copyright (c) 1998, 1999  Frodo Looijaard <frodol@dds.nl>,
    Philip Edelbrock <phil@netroedge.com>,
    and Mark Studebaker <mds@eng.paradyne.com>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

/*
    Supports following chips:

    Chip	#vin	#fanin	#pwm	#temp	wchipid	i2c	ISA
    w83781d	7	3	0	3	0x10	yes	yes
    w83782d	9	3	2-4	3	0x30	yes	yes
    w83783s	5-6	3	2	1-2	0x40	yes	no

*/

#include <linux/module.h>
#include <linux/malloc.h>
#include <linux/proc_fs.h>
#include <linux/ioport.h>
#include <linux/sysctl.h>
#include <asm/errno.h>
#include <asm/io.h>
#include <linux/types.h>
#include "smbus.h"
#include "version.h"
#include "i2c-isa.h"
#include "sensors.h"
#include "i2c.h"
#include "compat.h"

/* RT Table support experimental - define this to enable */
#undef W83781D_RT

/* Many W83781D constants specified below */

/* Length of ISA address segment */
#define W83781D_EXTENT 8

/* Where are the ISA address/data registers relative to the base address */
#define W83781D_ADDR_REG_OFFSET 5
#define W83781D_DATA_REG_OFFSET 6

/* The W83781D registers */
/* The W83782D registers for nr=7,8 are in bank 5 */
#define W83781D_REG_IN_MAX(nr) ((nr < 7) ? (0x2b + (nr) * 2) : (0x554 + (((nr) - 7) * 2)))
#define W83781D_REG_IN_MIN(nr) ((nr < 7) ? (0x2c + (nr) * 2) : (0x555 + (((nr) - 7) * 2)))
#define W83781D_REG_IN(nr)     ((nr < 7) ? (0x20 + (nr)) : (0x550 + (nr) - 7))

#define W83781D_REG_FAN_MIN(nr) (0x3a + (nr))
#define W83781D_REG_FAN(nr) (0x27 + (nr))

#define W83781D_REG_TEMP2 0x0150
#define W83781D_REG_TEMP3 0x0250
#define W83781D_REG_TEMP2_HYST 0x153
#define W83781D_REG_TEMP3_HYST 0x253
#define W83781D_REG_TEMP2_CONFIG 0x152
#define W83781D_REG_TEMP3_CONFIG 0x252
#define W83781D_REG_TEMP2_OVER 0x155
#define W83781D_REG_TEMP3_OVER 0x255

#define W83781D_REG_TEMP 0x27
#define W83781D_REG_TEMP_OVER 0x39
#define W83781D_REG_TEMP_HYST 0x3A
#define W83781D_REG_TEMP_CONFIG 0x52
#define W83781D_REG_BANK 0x4E

#define W83781D_REG_CONFIG 0x40
#define W83781D_REG_ALARM1 0x41
#define W83781D_REG_ALARM2 0x42
#define W83781D_REG_ALARM3 0x450	/* W83782D only */

#define W83781D_REG_BEEP_CONFIG 0x4D
#define W83781D_REG_BEEP_INTS1 0x56
#define W83781D_REG_BEEP_INTS2 0x57
#define W83781D_REG_BEEP_INTS3 0x453	/* W83782D only */

#define W83781D_REG_VID_FANDIV 0x47

#define W83781D_REG_CHIPID 0x49
#define W83781D_REG_WCHIPID 0x58
#define W83781D_REG_CHIPMAN 0x4F
#define W83781D_REG_PIN 0x4B

/* PWM 782D (1-4) and 783S (1-2) only */
#define W83781D_REG_PWM1 0x5B		/* 782d and 783s datasheets disagree on which is which. */
#define W83781D_REG_PWM2 0x5A		/* We follow 782d datasheet convention here */
#define W83781D_REG_PWM3 0x5E
#define W83781D_REG_PWM4 0x5F
#define W83781D_REG_PWMCLK12 0x5C
#define W83781D_REG_PWMCLK34 0x45C
const u8 regpwm[] = {W83781D_REG_PWM1, W83781D_REG_PWM2, W83781D_REG_PWM3, W83781D_REG_PWM4};
#define W83781D_REG_PWM(nr) (regpwm[(nr) - 1])

/* The following are undocumented in the data sheets however we
   received the information in an email from Winbond tech support */
/* Sensor selection 782D/783S only */
#define W83781D_REG_SCFG1 0x5D
const u8 BIT_SCFG1[] = {0x02, 0x04, 0x08};
#define W83781D_REG_SCFG2 0x59
const u8 BIT_SCFG2[] = {0x10, 0x04, 0x08};
#define W83781D_DEFAULT_BETA 3435

/* RT Table registers */
#define W83781D_REG_RT_IDX 0x50
#define W83781D_REG_RT_VAL 0x51

#define W83781D_WCHIPID 0x10
#define W83782D_WCHIPID 0x30
#define W83783S_WCHIPID 0x40


/* Conversions. Rounding is only done on the TO_REG variants. */
#define IN_TO_REG(val,nr) (((val) * 10 + 8)/16)
#define IN_FROM_REG(val,nr) (((val) * 16) / 10)

static inline unsigned char
FAN_TO_REG (unsigned rpm, unsigned divisor)
{
  unsigned val;
  
  if (rpm == 0)
      return 255;

  val = (1350000 + rpm * divisor / 2) / (rpm * divisor);
  if (val > 255)
      val = 255;
  return val;
}
#define FAN_FROM_REG(val,div) ((val)==0?-1:(val)==255?0:1350000/((val)*(div)))

#define TEMP_TO_REG(val) (((val)<0?(((val)-5)/10)&0xff:((val)+5)/10) & 0xff)
#define TEMP_FROM_REG(val) (((val)>0x80?(val)-0x100:(val))*10)

#define TEMP_ADD_TO_REG(val)   (((((val) + 2) / 5) << 7) & 0xff80)
#define TEMP_ADD_FROM_REG(val) (((val) >> 7) * 5)

#define VID_FROM_REG(val) ((val)==0x1f?0:(val)>=0x10?510-(val)*10:\
                           (val)>=0x06?0:205-(val)*5)
#define ALARMS_FROM_REG(val) (val)
#define PWM_FROM_REG(val) (val)
#define PWM_TO_REG(val) ((val) & 0xff)
#define BEEPS_FROM_REG(val) (val)
#define BEEPS_TO_REG(val) ((val) & 0xffff)

#define BEEP_ENABLE_TO_REG(val) (val)
#define BEEP_ENABLE_FROM_REG(val) ((val)?1:0)

#define DIV_FROM_REG(val) (1 << (val))
#define DIV_TO_REG(val) ((val)==8?3:(val)==4?2:(val)==1?0:1)

/* Initial limits */
#define W83781D_INIT_IN_0 (vid==350?280:vid)
#define W83781D_INIT_IN_1 (vid==350?280:vid)
#define W83781D_INIT_IN_2 330
#define W83781D_INIT_IN_3 (((500)   * 100)/168)
#define W83781D_INIT_IN_4 (((1200)  * 10)/38)
#define W83781D_INIT_IN_5 (((-1200) * -604)/2100)
#define W83781D_INIT_IN_6 (((-500)  * -604)/909)
#define W83781D_INIT_IN_7 (((500)   * 100)/168)
#define W83781D_INIT_IN_8 330
/* Initial limits for 782d/783s negative voltages */
/* Note level shift. Change min/max below if you change these. */
#define W83782D_INIT_IN_5 ((((-1200) + 1491) * 100)/514)
#define W83782D_INIT_IN_6 ((( (-500)  + 771) * 100)/314)

#define W83781D_INIT_IN_PERCENTAGE 10

#define W83781D_INIT_IN_MIN_0 \
        (W83781D_INIT_IN_0 - W83781D_INIT_IN_0 * W83781D_INIT_IN_PERCENTAGE \
         / 100) 
#define W83781D_INIT_IN_MAX_0 \
        (W83781D_INIT_IN_0 + W83781D_INIT_IN_0 * W83781D_INIT_IN_PERCENTAGE \
         / 100) 
#define W83781D_INIT_IN_MIN_1 \
        (W83781D_INIT_IN_1 - W83781D_INIT_IN_1 * W83781D_INIT_IN_PERCENTAGE \
         / 100) 
#define W83781D_INIT_IN_MAX_1 \
        (W83781D_INIT_IN_1 + W83781D_INIT_IN_1 * W83781D_INIT_IN_PERCENTAGE \
         / 100) 
#define W83781D_INIT_IN_MIN_2 \
        (W83781D_INIT_IN_2 - W83781D_INIT_IN_2 * W83781D_INIT_IN_PERCENTAGE \
         / 100) 
#define W83781D_INIT_IN_MAX_2 \
        (W83781D_INIT_IN_2 + W83781D_INIT_IN_2 * W83781D_INIT_IN_PERCENTAGE \
         / 100) 
#define W83781D_INIT_IN_MIN_3 \
        (W83781D_INIT_IN_3 - W83781D_INIT_IN_3 * W83781D_INIT_IN_PERCENTAGE \
         / 100) 
#define W83781D_INIT_IN_MAX_3 \
        (W83781D_INIT_IN_3 + W83781D_INIT_IN_3 * W83781D_INIT_IN_PERCENTAGE \
         / 100) 
#define W83781D_INIT_IN_MIN_4 \
        (W83781D_INIT_IN_4 - W83781D_INIT_IN_4 * W83781D_INIT_IN_PERCENTAGE \
         / 100) 
#define W83781D_INIT_IN_MAX_4 \
        (W83781D_INIT_IN_4 + W83781D_INIT_IN_4 * W83781D_INIT_IN_PERCENTAGE \
         / 100) 
#define W83781D_INIT_IN_MIN_5 \
        (W83781D_INIT_IN_5 - W83781D_INIT_IN_5 * W83781D_INIT_IN_PERCENTAGE \
         / 100) 
#define W83781D_INIT_IN_MAX_5 \
        (W83781D_INIT_IN_5 + W83781D_INIT_IN_5 * W83781D_INIT_IN_PERCENTAGE \
         / 100) 
#define W83781D_INIT_IN_MIN_6 \
        (W83781D_INIT_IN_6 - W83781D_INIT_IN_6 * W83781D_INIT_IN_PERCENTAGE \
         / 100) 
#define W83781D_INIT_IN_MAX_6 \
        (W83781D_INIT_IN_6 + W83781D_INIT_IN_6 * W83781D_INIT_IN_PERCENTAGE \
         / 100) 
#define W83781D_INIT_IN_MIN_7 \
        (W83781D_INIT_IN_7 - W83781D_INIT_IN_7 * W83781D_INIT_IN_PERCENTAGE \
         / 100) 
#define W83781D_INIT_IN_MAX_7 \
        (W83781D_INIT_IN_7 + W83781D_INIT_IN_7 * W83781D_INIT_IN_PERCENTAGE \
         / 100) 
#define W83781D_INIT_IN_MIN_8 \
        (W83781D_INIT_IN_8 - W83781D_INIT_IN_8 * W83781D_INIT_IN_PERCENTAGE \
         / 100) 
#define W83781D_INIT_IN_MAX_8 \
        (W83781D_INIT_IN_8 + W83781D_INIT_IN_8 * W83781D_INIT_IN_PERCENTAGE \
         / 100) 
/* Initial limits for 782d/783s negative voltages */
/* These aren't direct multiples because of level shift */
/* Beware going negative - check */
#define W83782D_INIT_IN_MIN_5_TMP \
        (((-1200 * (100 + W83781D_INIT_IN_PERCENTAGE)) + (1491 * 100))/514)
#define W83782D_INIT_IN_MIN_5 \
        ((W83782D_INIT_IN_MIN_5_TMP > 0) ? W83782D_INIT_IN_MIN_5_TMP : 0)
#define W83782D_INIT_IN_MAX_5 \
        (((-1200 * (100 - W83781D_INIT_IN_PERCENTAGE)) + (1491 * 100))/514)
#define W83782D_INIT_IN_MIN_6_TMP \
        ((( -500 * (100 + W83781D_INIT_IN_PERCENTAGE)) +  (771 * 100))/314)
#define W83782D_INIT_IN_MIN_6 \
        ((W83782D_INIT_IN_MIN_6_TMP > 0) ? W83782D_INIT_IN_MIN_6_TMP : 0)
#define W83782D_INIT_IN_MAX_6 \
        ((( -500 * (100 - W83781D_INIT_IN_PERCENTAGE)) +  (771 * 100))/314)

#define W83781D_INIT_FAN_MIN_1 3000
#define W83781D_INIT_FAN_MIN_2 3000
#define W83781D_INIT_FAN_MIN_3 3000

#define W83781D_INIT_TEMP_OVER 600
#define W83781D_INIT_TEMP_HYST 500
#define W83781D_INIT_TEMP2_OVER 600
#define W83781D_INIT_TEMP2_HYST 500
#define W83781D_INIT_TEMP3_OVER 600
#define W83781D_INIT_TEMP3_HYST 500

#ifdef MODULE
extern int init_module(void);
extern int cleanup_module(void);
#endif /* MODULE */

/* There are some complications in a module like this. First off, W83781D chips
   may be both present on the SMBus and the ISA bus, and we have to handle
   those cases separately at some places. Second, there might be several
   W83781D chips available (well, actually, that is probably never done; but
   it is a clean illustration of how to handle a case like that). Finally,
   a specific chip may be attached to *both* ISA and SMBus, and we would
   not like to detect it double. Fortunately, in the case of the W83781D at
   least, a register tells us what SMBus address we are on, so that helps
   a bit - except if there could be more than one SMBus. Groan. No solution
   for this yet. */

/* This module may seem overly long and complicated. In fact, it is not so
   bad. Quite a lot of bookkeeping is done. A real driver can often cut
   some corners. */

/* For each registered W83781D, we need to keep some data in memory. That
   data is pointed to by w83781d_list[NR]->data. The structure itself is
   dynamically allocated, at the same time when a new w83781d client is
   allocated. */
struct w83781d_data {
         struct semaphore lock;
         int sysctl_id;

         struct semaphore update_lock;
         char valid;                 /* !=0 if following fields are valid */
         unsigned long last_updated; /* In jiffies */

         u8 in[9];                   /* Register value - 8 and 9 for 782D only */
         u8 in_max[9];               /* Register value - 8 and 9 for 782D only */
         u8 in_min[9];               /* Register value - 8 and 9 for 782D only */
         u8 fan[3];                  /* Register value */
         u8 fan_min[3];              /* Register value */
         u8 temp;
         u8 temp_over;               /* Register value */
         u8 temp_hyst;               /* Register value */
         u16 temp_add[2];             /* Register value */
         u16 temp_add_over[2];       /* Register value */
         u16 temp_add_hyst[2];       /* Register value */
         u8 fan_div[3];              /* Register encoding, shifted right */
         u8 vid;                     /* Register encoding, combined */
         u32 alarms;                 /* Register encoding, combined */
         u16 beeps;                  /* Register encoding, combined */
         u8 beep_enable;             /* Boolean */
         u8 wchipid;                 /* Register value */
         u8 pwm[4];                  /* Register value */				
         u16 sens[3];                /* 782D/783S only.
					1 = pentium diode; 2 = 3904 diode;
                                        3000-5000 = thermistor beta.
                                        Default = 3435. Other Betas unimplemented */
#ifdef W83781D_RT
         u8 rt[3][32];               /* Register value */
#endif
};


static int w83781d_init(void);
static int w83781d_cleanup(void);

static int w83781d_attach_adapter(struct i2c_adapter *adapter);
static int w83781d_detect_isa(struct isa_adapter *adapter);
static int w83781d_detect_smbus(struct i2c_adapter *adapter);
static int w83781d_detach_client(struct i2c_client *client);
static int w83781d_detach_isa(struct isa_client *client);
static int w83781d_detach_smbus(struct i2c_client *client);
static int w83781d_new_client(struct i2c_adapter *adapter,
                           struct i2c_client *new_client);
static void w83781d_remove_client(struct i2c_client *client);
static int w83781d_command(struct i2c_client *client, unsigned int cmd, 
                        void *arg);
static void w83781d_inc_use (struct i2c_client *client);
static void w83781d_dec_use (struct i2c_client *client);

static int w83781d_read_value(struct i2c_client *client, u16 register);
static int w83781d_write_value(struct i2c_client *client, u16 register, 
                               u16 value);
static void w83781d_update_client(struct i2c_client *client);
static void w83781d_init_client(struct i2c_client *client);


static void w83781d_in(struct i2c_client *client, int operation, int ctl_name,
                    int *nrels_mag, long *results);
static void w83781d_fan(struct i2c_client *client, int operation, int ctl_name,
                     int *nrels_mag, long *results);
static void w83781d_temp(struct i2c_client *client, int operation, 
                          int ctl_name, int *nrels_mag, long *results);
static void w83781d_temp_add(struct i2c_client *client, int operation, 
                          int ctl_name, int *nrels_mag, long *results);
static void w83781d_vid(struct i2c_client *client, int operation, int ctl_name,
                         int *nrels_mag, long *results);
static void w83781d_alarms(struct i2c_client *client, int operation,
                           int ctl_name, int *nrels_mag, long *results);
static void w83781d_beep(struct i2c_client *client, int operation, int ctl_name,
                      int *nrels_mag, long *results);
static void w83781d_fan_div(struct i2c_client *client, int operation,
                            int ctl_name, int *nrels_mag, long *results);
static void w83781d_pwm(struct i2c_client *client, int operation,
                        int ctl_name, int *nrels_mag, long *results);
static void w83781d_sens(struct i2c_client *client, int operation,
                        int ctl_name, int *nrels_mag, long *results);
#ifdef W83781D_RT
static void w83781d_rt(struct i2c_client *client, int operation,
                        int ctl_name, int *nrels_mag, long *results);
#endif

/* I choose here for semi-static W83781D allocation. Complete dynamic
   allocation could also be used; the code needed for this would probably
   take more memory than the datastructure takes now. */
#define MAX_W83781D_NR 4
static struct i2c_client *w83781d_list[MAX_W83781D_NR];

/* The driver. I choose to use type i2c_driver, as at is identical to both
   smbus_driver and isa_driver, and clients could be of either kind */
static struct i2c_driver w83781d_driver = {
  /* name */		"W83781D sensor driver",
  /* id */		I2C_DRIVERID_W83781D,
  /* flags */		DF_NOTIFY,
  /* attach_adapter */  &w83781d_attach_adapter,
  /* detach_client */	&w83781d_detach_client,
  /* command */		&w83781d_command,
  /* inc_use */		&w83781d_inc_use,
  /* dec_use */		&w83781d_dec_use
};

/* Used by w83781d_init/cleanup */
static int w83781d_initialized = 0;

/* The /proc/sys entries */
/* These files are created for each detected W83781D. This is just a template;
   though at first sight, you might think we could use a statically
   allocated list, we need some way to get back to the parent - which
   is done through one of the 'extra' fields which are initialized 
   when a new copy is allocated. */
static ctl_table w83781d_dir_table_template[] = {
  { W83781D_SYSCTL_IN0, "in0", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_in },
  { W83781D_SYSCTL_IN1, "in1", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_in },
  { W83781D_SYSCTL_IN2, "in2", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_in },
  { W83781D_SYSCTL_IN3, "in3", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_in },
  { W83781D_SYSCTL_IN4, "in4", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_in },
  { W83781D_SYSCTL_IN5, "in5", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_in },
  { W83781D_SYSCTL_IN6, "in6", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_in },
  { W83781D_SYSCTL_FAN1, "fan1", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_fan },
  { W83781D_SYSCTL_FAN2, "fan2", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_fan },
  { W83781D_SYSCTL_FAN3, "fan3", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_fan },
  { W83781D_SYSCTL_TEMP1, "temp1", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_temp },
  { W83781D_SYSCTL_TEMP2, "temp2", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_temp_add },
  { W83781D_SYSCTL_TEMP3, "temp3", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_temp_add },
  { W83781D_SYSCTL_VID, "vid", NULL, 0, 0444, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_vid },
  { W83781D_SYSCTL_FAN_DIV, "fan_div", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_fan_div },
  { W83781D_SYSCTL_ALARMS, "alarms", NULL, 0, 0444, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_alarms },
  { W83781D_SYSCTL_BEEP, "beep", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_beep },
#ifdef W83781D_RT
  { W83781D_SYSCTL_RT1, "rt1", NULL, 0, 0444, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_rt },
  { W83781D_SYSCTL_RT2, "rt2", NULL, 0, 0444, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_rt },
  { W83781D_SYSCTL_RT3, "rt3", NULL, 0, 0444, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_rt },
#endif
  { 0 }
};

/* without pwm3-4 */
static ctl_table w83782d_isa_dir_table_template[] = {
  { W83781D_SYSCTL_IN0, "in0", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_in },
  { W83781D_SYSCTL_IN1, "in1", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_in },
  { W83781D_SYSCTL_IN2, "in2", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_in },
  { W83781D_SYSCTL_IN3, "in3", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_in },
  { W83781D_SYSCTL_IN4, "in4", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_in },
  { W83781D_SYSCTL_IN5, "in5", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_in },
  { W83781D_SYSCTL_IN6, "in6", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_in },
  { W83781D_SYSCTL_IN7, "in7", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_in },
  { W83781D_SYSCTL_IN8, "in8", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_in },
  { W83781D_SYSCTL_FAN1, "fan1", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_fan },
  { W83781D_SYSCTL_FAN2, "fan2", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_fan },
  { W83781D_SYSCTL_FAN3, "fan3", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_fan },
  { W83781D_SYSCTL_TEMP1, "temp1", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_temp },
  { W83781D_SYSCTL_TEMP2, "temp2", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_temp_add },
  { W83781D_SYSCTL_TEMP3, "temp3", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_temp_add },
  { W83781D_SYSCTL_VID, "vid", NULL, 0, 0444, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_vid },
  { W83781D_SYSCTL_FAN_DIV, "fan_div", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_fan_div },
  { W83781D_SYSCTL_ALARMS, "alarms", NULL, 0, 0444, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_alarms },
  { W83781D_SYSCTL_BEEP, "beep", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_beep },
  { W83781D_SYSCTL_PWM1, "pwm1", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_pwm },
  { W83781D_SYSCTL_PWM2, "pwm2", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_pwm },
  { W83781D_SYSCTL_SENS1, "sensor1", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_sens },
  { W83781D_SYSCTL_SENS2, "sensor2", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_sens },
  { W83781D_SYSCTL_SENS3, "sensor3", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_sens },
  { 0 }
};

/* with pwm3-4 */
static ctl_table w83782d_i2c_dir_table_template[] = {
  { W83781D_SYSCTL_IN0, "in0", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_in },
  { W83781D_SYSCTL_IN1, "in1", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_in },
  { W83781D_SYSCTL_IN2, "in2", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_in },
  { W83781D_SYSCTL_IN3, "in3", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_in },
  { W83781D_SYSCTL_IN4, "in4", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_in },
  { W83781D_SYSCTL_IN5, "in5", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_in },
  { W83781D_SYSCTL_IN6, "in6", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_in },
  { W83781D_SYSCTL_IN7, "in7", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_in },
  { W83781D_SYSCTL_IN8, "in8", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_in },
  { W83781D_SYSCTL_FAN1, "fan1", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_fan },
  { W83781D_SYSCTL_FAN2, "fan2", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_fan },
  { W83781D_SYSCTL_FAN3, "fan3", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_fan },
  { W83781D_SYSCTL_TEMP1, "temp1", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_temp },
  { W83781D_SYSCTL_TEMP2, "temp2", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_temp_add },
  { W83781D_SYSCTL_TEMP3, "temp3", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_temp_add },
  { W83781D_SYSCTL_VID, "vid", NULL, 0, 0444, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_vid },
  { W83781D_SYSCTL_FAN_DIV, "fan_div", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_fan_div },
  { W83781D_SYSCTL_ALARMS, "alarms", NULL, 0, 0444, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_alarms },
  { W83781D_SYSCTL_BEEP, "beep", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_beep },
  { W83781D_SYSCTL_PWM1, "pwm1", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_pwm },
  { W83781D_SYSCTL_PWM2, "pwm2", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_pwm },
  { W83781D_SYSCTL_PWM3, "pwm3", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_pwm },
  { W83781D_SYSCTL_PWM4, "pwm4", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_pwm },
  { W83781D_SYSCTL_SENS1, "sensor1", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_sens },
  { W83781D_SYSCTL_SENS2, "sensor2", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_sens },
  { W83781D_SYSCTL_SENS3, "sensor3", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_sens },
  { 0 }
};

static ctl_table w83783s_dir_table_template[] = {
  { W83781D_SYSCTL_IN0, "in0", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_in },
  /* no in1 to maintain compatibility with 781d and 782d. */
  { W83781D_SYSCTL_IN2, "in2", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_in },
  { W83781D_SYSCTL_IN3, "in3", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_in },
  { W83781D_SYSCTL_IN4, "in4", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_in },
  { W83781D_SYSCTL_IN5, "in5", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_in },
  { W83781D_SYSCTL_IN6, "in6", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_in },
  { W83781D_SYSCTL_FAN1, "fan1", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_fan },
  { W83781D_SYSCTL_FAN2, "fan2", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_fan },
  { W83781D_SYSCTL_FAN3, "fan3", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_fan },
  { W83781D_SYSCTL_TEMP1, "temp1", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_temp },
  { W83781D_SYSCTL_TEMP2, "temp2", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_temp_add },
  { W83781D_SYSCTL_VID, "vid", NULL, 0, 0444, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_vid },
  { W83781D_SYSCTL_FAN_DIV, "fan_div", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_fan_div },
  { W83781D_SYSCTL_ALARMS, "alarms", NULL, 0, 0444, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_alarms },
  { W83781D_SYSCTL_BEEP, "beep", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_beep },
  { W83781D_SYSCTL_PWM1, "pwm1", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_pwm },
  { W83781D_SYSCTL_PWM2, "pwm2", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_pwm },
  { W83781D_SYSCTL_SENS1, "sensor1", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_sens },
  { W83781D_SYSCTL_SENS2, "sensor2", NULL, 0, 0644, NULL, &sensors_proc_real,
    &sensors_sysctl_real, NULL, &w83781d_sens },
  { 0 }
};


/* This function is called when:
     * w83781d_driver is inserted (when this module is loaded), for each
       available adapter
     * when a new adapter is inserted (and w83781d_driver is still present) */
int w83781d_attach_adapter(struct i2c_adapter *adapter)
{
  if (i2c_is_isa_adapter(adapter))
    return w83781d_detect_isa((struct isa_adapter *) adapter);
  else
    return w83781d_detect_smbus(adapter);
}

/* This function is called whenever a client should be removed:
    * w83781d_driver is removed (when this module is unloaded)
    * when an adapter is removed which has a w83781d client (and w83781d_driver
      is still present). */
int w83781d_detach_client(struct i2c_client *client)
{
  if (i2c_is_isa_client(client))
    return w83781d_detach_isa((struct isa_client *) client);
  else
    return w83781d_detach_smbus(client);
}

/* Detect whether there is a W83781D on the ISA bus, register and initialize 
   it. */
int w83781d_detect_isa(struct isa_adapter *adapter)
{
  int address,err,temp,wchipid;
  struct isa_client *new_client;
  const char *type_name;
  const char *client_name;

  /* OK, this is no detection. I know. It will do for now, though.  */

  err = 0;
  for (address = 0x290; (! err) && (address <= 0x290); address += 0x08) {
    if (check_region(address, W83781D_EXTENT))
      continue;

    if (inb_p(address + W83781D_ADDR_REG_OFFSET) == 0xff) {
      outb_p(0x00,address + W83781D_ADDR_REG_OFFSET);
      if (inb_p(address + W83781D_ADDR_REG_OFFSET) == 0xff)
        continue;
    }
    
    /* Real detection code goes here */

    /* The Winbond may be stuck in bank 1 or 2. This should reset it. 
       We really need some nifty detection code, because this can lead
       to a lot of problems if there is no Winbond present! */
    outb_p(W83781D_REG_BANK,address + W83781D_ADDR_REG_OFFSET);
    outb_p(0x00,address + W83781D_DATA_REG_OFFSET);
    
    /* Detection -- To bad we can't do this before setting to bank 0 */
    outb_p(W83781D_REG_CHIPMAN,address + W83781D_ADDR_REG_OFFSET);
    temp=inb_p(address + W83781D_DATA_REG_OFFSET);
 #ifdef DEBUG
    printk("w83781d.o: Detect byte: 0x%X\n",temp);
 #endif
    if (temp != 0x0A3) {
 #ifdef DEBUG
        printk("w83781d.o: Winbond W8378xx detection failed (ISA at 0x%X)\n",address);
 #endif
    	continue;
    }
    outb_p(W83781D_REG_WCHIPID,address + W83781D_ADDR_REG_OFFSET);
    /* mask off lower bit, not reliable */
    wchipid = 0xFE & inb_p(address + W83781D_DATA_REG_OFFSET);
    if(wchipid == W83782D_WCHIPID) {
      printk("w83781d.o: Winbond W83782D detected (ISA addr=0x%X)\n",address);
      type_name = "w83782d";
      client_name = "Winbond W83782D chip";
    } else {
      printk("w83781d.o: Winbond W83781D detected (ISA addr=0x%X)\n",address);
      type_name = "w83781d";
      client_name = "Winbond W83781D chip";
    }

    request_region(address, W83781D_EXTENT, type_name);

    /* Allocate space for a new client structure */
    if (! (new_client = kmalloc(sizeof(struct isa_client) + 
                                sizeof(struct w83781d_data),
                               GFP_KERNEL)))
    {
      err=-ENOMEM;
      goto ERROR1;
    } 

    /* Fill the new client structure with data */
    new_client->data = (struct w83781d_data *) (new_client + 1);
    new_client->addr = 0;
    strcpy(new_client->name,client_name);
    new_client->isa_addr = address;
    if ((err = w83781d_new_client((struct i2c_adapter *) adapter,
                               (struct i2c_client *) new_client)))
      goto ERROR2;

    /* Tell i2c-core a new client has arrived */
    if ((err = isa_attach_client(new_client)))
      goto ERROR3;
    
    /* Register a new directory entry with module sensors */
    if ((err = sensors_register_entry((struct i2c_client *) new_client,
                                      type_name,
                                      (wchipid == W83782D_WCHIPID) ? w83782d_isa_dir_table_template :
                                      w83781d_dir_table_template)) < 0)
      goto ERROR4;
    ((struct w83781d_data *) (new_client->data)) -> sysctl_id = err;
    ((struct w83781d_data *) (new_client->data))->wchipid = wchipid;
    err = 0;

    /* Initialize the W83781D chip */
    w83781d_init_client((struct i2c_client *) new_client);
    continue;

/* OK, this is not exactly good programming practice, usually. But it is
   very code-efficient in this case. */

ERROR4:
    isa_detach_client(new_client);
ERROR3:
    w83781d_remove_client((struct i2c_client *) new_client);
ERROR2:
    kfree(new_client);
ERROR1:
    release_region(address, W83781D_EXTENT);
  }
  return err;

}

/* Deregister and remove a W83781D client */
int w83781d_detach_isa(struct isa_client *client)
{
  int err,i;
  for (i = 0; i < MAX_W83781D_NR; i++)
    if ((client == (struct isa_client *) (w83781d_list[i])))
      break;
  if (i == MAX_W83781D_NR) {
    printk("w83781d.o: Client to detach not found.\n");
    return -ENOENT;
  }

  sensors_deregister_entry(((struct w83781d_data *)(client->data))->sysctl_id);

  if ((err = isa_detach_client(client))) {
    printk("w83781d.o: Client deregistration failed, client not detached.\n");
    return err;
  }
  w83781d_remove_client((struct i2c_client *) client);
  release_region(client->isa_addr,W83781D_EXTENT);
  kfree(client);
  return 0;
}

int w83781d_detect_smbus(struct i2c_adapter *adapter)
{
  int address,err,wchipid;
  struct i2c_client *new_client;
  const char *type_name,*client_name;

  /* OK, this is no detection. I know. It will do for now, though.  */
  err = 0;
  for (address = 0x20; (! err) && (address <= 0x2f); address ++) {

    /* Later on, we will keep a list of registered addresses for each
       adapter, and check whether they are used here */

    if (smbus_read_byte_data(adapter,address,W83781D_REG_CONFIG) < 0) 
      continue;

    smbus_write_byte_data(adapter,address,W83781D_REG_BANK,0x00);

    err = smbus_read_byte_data(adapter,address,W83781D_REG_CHIPMAN);
 #ifdef DEBUG
    printk("w83781d.o: Detect byte: 0x%X\n",err);
 #endif
    if (err == 0x0A3) {
      /* mask off lower bit, not reliable */
      wchipid = 0xFE & smbus_read_byte_data(adapter,address,W83781D_REG_WCHIPID);
      if(wchipid == W83783S_WCHIPID) {
        printk("w83781d.o: Winbond W83783S detected (SMBus addr 0x%X)\n",address);
        type_name = "w83783s";
        client_name = "Winbond W83783S chip";
      } else if (wchipid == W83782D_WCHIPID) {
        printk("w83781d.o: Winbond W83782D detected (SMBus addr 0x%X)\n",address);
        type_name = "w83782d";
        client_name = "Winbond W83782D chip";
      } else {
        printk("w83781d.o: Winbond W83781D detected (SMBus addr 0x%X)\n",address);
        type_name = "w83781d";
        client_name = "Winbond W83781D chip";
      }
      err=0;
    } else {
 #ifdef DEBUG
     printk("w83781d.o: Winbond W8378xx detection failed (SMBus/I2C at 0x%X)\n",address);
 #endif
     continue;
    }


    /* Allocate space for a new client structure. To counter memory
       ragmentation somewhat, we only do one kmalloc. */
    if (! (new_client = kmalloc(sizeof(struct i2c_client) + 
                                sizeof(struct w83781d_data),
                               GFP_KERNEL))) {
      err = -ENOMEM;
      continue;
    }

    /* Fill the new client structure with data */
    new_client->data = (struct w83781d_data *) (new_client + 1);
    new_client->addr = address;
    strcpy(new_client->name,client_name);
    if ((err = w83781d_new_client(adapter,new_client)))
      goto ERROR2;

    /* Tell i2c-core a new client has arrived */
    if ((err = i2c_attach_client(new_client))) 
      goto ERROR3;

    /* Register a new directory entry with module sensors */
    if ((err = sensors_register_entry(new_client,type_name,
                                      (wchipid == W83783S_WCHIPID) ? w83783s_dir_table_template : 
                                      ((wchipid == W83782D_WCHIPID) ? w83782d_i2c_dir_table_template :
                                      w83781d_dir_table_template))) < 0)
      goto ERROR4;
    ((struct w83781d_data *) (new_client->data))->sysctl_id = err;
    ((struct w83781d_data *) (new_client->data))->wchipid = wchipid;
    err = 0;

    /* Initialize the W83781D chip */
    w83781d_init_client(new_client);
    continue;

/* OK, this is not exactly good programming practice, usually. But it is
   very code-efficient in this case. */
ERROR4:
    i2c_detach_client(new_client);
ERROR3:
    w83781d_remove_client((struct i2c_client *) new_client);
ERROR2:
    kfree(new_client);
  }
  return err;
}

int w83781d_detach_smbus(struct i2c_client *client)
{
  int err,i;
  for (i = 0; i < MAX_W83781D_NR; i++)
    if (client == w83781d_list[i])
      break;
  if ((i == MAX_W83781D_NR)) {
    printk("w83781d.o: Client to detach not found.\n");
    return -ENOENT;
  }

  sensors_deregister_entry(((struct w83781d_data *)(client->data))->sysctl_id);

  if ((err = i2c_detach_client(client))) {
    printk("w83781d.o: Client deregistration failed, client not detached.\n");
    return err;
  }
  w83781d_remove_client(client);
  kfree(client);
  return 0;
}


/* Find a free slot, and initialize most of the fields */
int w83781d_new_client(struct i2c_adapter *adapter,
                    struct i2c_client *new_client)
{
  int i;
  struct w83781d_data *data;

  /* First, seek out an empty slot */
  for(i = 0; i < MAX_W83781D_NR; i++)
    if (! w83781d_list[i])
      break;
  if (i == MAX_W83781D_NR) {
    printk("w83781d.o: No empty slots left, recompile and heighten "
           "MAX_W83781D_NR!\n");
    return -ENOMEM;
  }
  
  w83781d_list[i] = new_client;
  new_client->id = i;
  new_client->adapter = adapter;
  new_client->driver = &w83781d_driver;
  data = new_client->data;
  data->valid = 0;
  data->lock = MUTEX;
  data->update_lock = MUTEX;
  return 0;
}

/* Inverse of w83781d_new_client */
void w83781d_remove_client(struct i2c_client *client)
{
  int i;
  for (i = 0; i < MAX_W83781D_NR; i++)
    if (client == w83781d_list[i]) 
      w83781d_list[i] = NULL;
}

/* No commands defined yet */
int w83781d_command(struct i2c_client *client, unsigned int cmd, void *arg)
{
  return 0;
}

void w83781d_inc_use (struct i2c_client *client)
{
#ifdef MODULE
  MOD_INC_USE_COUNT;
#endif
}

void w83781d_dec_use (struct i2c_client *client)
{
#ifdef MODULE
  MOD_DEC_USE_COUNT;
#endif
}
 

/* The SMBus locks itself, usually, but nothing may access the Winbond between
   bank switches. ISA access must always be locked explicitely! 
   We ignore the W83781D BUSY flag at this moment - it could lead to deadlocks,
   would slow down the W83781D access and should not be necessary. 
   There are some ugly typecasts here, but the good new is - they should
   nowhere else be necessary! */
int w83781d_read_value(struct i2c_client *client, u16 reg)
{
  int res,word_sized;

  word_sized = (((reg & 0xff00) == 0x100) || ((reg & 0xff00) == 0x200)) &&
                                 (((reg & 0x00ff) == 0x50) || 
                                  ((reg & 0x00ff) == 0x53) || 
                                  ((reg & 0x00ff) == 0x55));
  down((struct semaphore *) (client->data));
  if (i2c_is_isa_client(client)) {
    if (reg & 0xff00) {
      outb_p(W83781D_REG_BANK,(((struct isa_client *) client)->isa_addr) +
                              W83781D_ADDR_REG_OFFSET);
      outb_p(reg >> 8,(((struct isa_client *) client)->isa_addr) +
                      W83781D_DATA_REG_OFFSET);
    }
    outb_p(reg & 0xff,(((struct isa_client *) client)->isa_addr) +
                      W83781D_ADDR_REG_OFFSET);
    res = inb_p((((struct isa_client *) client)->isa_addr) +
                W83781D_DATA_REG_OFFSET);
    if (word_sized) {
      outb_p((reg & 0xff)+1,(((struct isa_client *) client)->isa_addr) +
                        W83781D_ADDR_REG_OFFSET);
      res = (res << 8) + inb_p((((struct isa_client *) client)->isa_addr) +
                         W83781D_DATA_REG_OFFSET);
    }
    if (reg & 0xff00) {
      outb_p(W83781D_REG_BANK,(((struct isa_client *) client)->isa_addr) +
                              W83781D_ADDR_REG_OFFSET);
      outb_p(0,(((struct isa_client *) client)->isa_addr) +
               W83781D_DATA_REG_OFFSET);
    }
  } else {
    if (reg & 0xff00)
      smbus_write_byte_data(client->adapter,client->addr,W83781D_REG_BANK,
                            reg >> 8);
    res = smbus_read_byte_data(client->adapter,client->addr, reg);
    if (word_sized)
      res = (res << 8) + smbus_read_byte_data(client->adapter,client->addr, 
                                              reg);
    if (reg & 0xff00)
      smbus_write_byte_data(client->adapter,client->addr,W83781D_REG_BANK,0);
  }
  up((struct semaphore *) (client->data));
  return res;
}

/* The SMBus locks itself, usually, but nothing may access the Winbond between
   bank switches. ISA access must always be locked explicitely! 
   We ignore the W83781D BUSY flag at this moment - it could lead to deadlocks,
   would slow down the W83781D access and should not be necessary. 
   There are some ugly typecasts here, but the good new is - they should
   nowhere else be necessary! */
int w83781d_write_value(struct i2c_client *client, u16 reg, u16 value)
{
  int word_sized;

  word_sized = (((reg & 0xff00) == 0x100) || ((reg & 0xff00) == 0x200)) &&
                                 (((reg & 0x00ff) == 0x50) || 
                                  ((reg & 0x00ff) == 0x53) || 
                                  ((reg & 0x00ff) == 0x55));
  down((struct semaphore *) (client->data));
  if (i2c_is_isa_client(client)) {
    if (reg & 0xff00) {
      outb_p(W83781D_REG_BANK,(((struct isa_client *) client)->isa_addr) +
                              W83781D_ADDR_REG_OFFSET);
      outb_p(reg >> 8,(((struct isa_client *) client)->isa_addr) +
                      W83781D_DATA_REG_OFFSET);
    }
    outb_p(reg & 0xff,(((struct isa_client *) client)->isa_addr) +
                      W83781D_ADDR_REG_OFFSET);
    if (word_sized) {
      outb_p(value >> 8,(((struct isa_client *) client)->isa_addr) +
                        W83781D_DATA_REG_OFFSET);
      outb_p((reg & 0xff)+1,(((struct isa_client *) client)->isa_addr) +
                        W83781D_ADDR_REG_OFFSET);
    }
    outb_p(value &0xff,(((struct isa_client *) client)->isa_addr) +
                       W83781D_DATA_REG_OFFSET);
    if (reg & 0xff00) {
      outb_p(W83781D_REG_BANK,(((struct isa_client *) client)->isa_addr) +
                              W83781D_ADDR_REG_OFFSET);
      outb_p(0,(((struct isa_client *) client)->isa_addr) +
               W83781D_DATA_REG_OFFSET);
    }
  } else {
    if (reg & 0xff00)
      smbus_write_byte_data(client->adapter,client->addr,W83781D_REG_BANK,
                            reg >> 8);
    if (word_sized) {
       smbus_write_byte_data(client->adapter,client->addr, reg, value >> 8);
       smbus_write_byte_data(client->adapter,client->addr, reg+1, value &0xff);
    } else
      smbus_write_byte_data(client->adapter,client->addr, reg, value &0xff);
    if (reg & 0xff00)
      smbus_write_byte_data(client->adapter,client->addr,W83781D_REG_BANK,0);
  }
  up((struct semaphore *) (client->data));
  return 0;
}

/* Called when we have found a new W83781D. It should set limits, etc. */
void w83781d_init_client(struct i2c_client *client)
{
  struct w83781d_data *data = client->data;
  int vid,wchipid;
  int i;
  u8 tmp;

  wchipid = data->wchipid;
  /* Reset all except Watchdog values and last conversion values
     This sets fan-divs to 2, among others */
  w83781d_write_value(client,W83781D_REG_CONFIG,0x80);

  vid = w83781d_read_value(client,W83781D_REG_VID_FANDIV) & 0x0f;
  vid |= (w83781d_read_value(client,W83781D_REG_CHIPID) & 0x01) << 4;
  vid = VID_FROM_REG(vid);

  if(wchipid != W83781D_WCHIPID) {
    tmp = w83781d_read_value(client,W83781D_REG_SCFG1);
    for (i = 1; i <= 3; i++) {
      if(!(tmp & BIT_SCFG1[i-1])) {
        data->sens[i-1] = W83781D_DEFAULT_BETA;
      } else {
        if(w83781d_read_value(client,W83781D_REG_TEMP) & BIT_SCFG2[i-1])
          data->sens[i-1] = 1;
        else
          data->sens[i-1] = 2;
      }
      if(data->wchipid == W83783S_WCHIPID && i == 2)
        break;
    }
  }

#ifdef W83781D_RT
/*
   Fill up the RT Tables.
   We assume that they are 32 bytes long, in order for temp 1-3.
   Data sheet documentation is sparse.
   We also assume that it is only for the 781D although I suspect
   that the 782D/783D support it as well....
*/

  if(wchipid == W83781D_WCHIPID) {
    u16 k = 0;
/*
    Auto-indexing doesn't seem to work...
    w83781d_write_value(client,W83781D_REG_RT_IDX,0);
*/
    for (i = 0; i < 3; i++) {
      int j;
      for (j = 0; j < 32; j++) {
        w83781d_write_value(client,W83781D_REG_RT_IDX,k++);
        data->rt[i][j] = w83781d_read_value(client,W83781D_REG_RT_VAL);
      }
    }
  }
#endif

  w83781d_write_value(client,W83781D_REG_IN_MIN(0),
                      IN_TO_REG(W83781D_INIT_IN_MIN_0,0));
  w83781d_write_value(client,W83781D_REG_IN_MAX(0),
                      IN_TO_REG(W83781D_INIT_IN_MAX_0,0));
  if(wchipid != W83783S_WCHIPID) {
    w83781d_write_value(client,W83781D_REG_IN_MIN(1),
                        IN_TO_REG(W83781D_INIT_IN_MIN_1,1));
    w83781d_write_value(client,W83781D_REG_IN_MAX(1),
                        IN_TO_REG(W83781D_INIT_IN_MAX_1,1));
  }
  w83781d_write_value(client,W83781D_REG_IN_MIN(2),
                      IN_TO_REG(W83781D_INIT_IN_MIN_2,2));
  w83781d_write_value(client,W83781D_REG_IN_MAX(2),
                      IN_TO_REG(W83781D_INIT_IN_MAX_2,2));
  w83781d_write_value(client,W83781D_REG_IN_MIN(3),
                      IN_TO_REG(W83781D_INIT_IN_MIN_3,3));
  w83781d_write_value(client,W83781D_REG_IN_MAX(3),
                      IN_TO_REG(W83781D_INIT_IN_MAX_3,3));
  w83781d_write_value(client,W83781D_REG_IN_MIN(4),
                      IN_TO_REG(W83781D_INIT_IN_MIN_4,4));
  w83781d_write_value(client,W83781D_REG_IN_MAX(4),
                      IN_TO_REG(W83781D_INIT_IN_MAX_4,4));
  if(wchipid == W83781D_WCHIPID) {
    w83781d_write_value(client,W83781D_REG_IN_MIN(5),
                        IN_TO_REG(W83781D_INIT_IN_MIN_5,5));
    w83781d_write_value(client,W83781D_REG_IN_MAX(5),
                        IN_TO_REG(W83781D_INIT_IN_MAX_5,5));
  } else {
    w83781d_write_value(client,W83781D_REG_IN_MIN(5),
                        IN_TO_REG(W83782D_INIT_IN_MIN_5,5));
    w83781d_write_value(client,W83781D_REG_IN_MAX(5),
                        IN_TO_REG(W83782D_INIT_IN_MAX_5,5));
  }
  if(wchipid == W83781D_WCHIPID) {
    w83781d_write_value(client,W83781D_REG_IN_MIN(6),
                        IN_TO_REG(W83781D_INIT_IN_MIN_6,6));
    w83781d_write_value(client,W83781D_REG_IN_MAX(6),
                        IN_TO_REG(W83781D_INIT_IN_MAX_6,6));
  } else {
    w83781d_write_value(client,W83781D_REG_IN_MIN(6),
                        IN_TO_REG(W83782D_INIT_IN_MIN_6,6));
    w83781d_write_value(client,W83781D_REG_IN_MAX(6),
                        IN_TO_REG(W83782D_INIT_IN_MAX_6,6));
  }
  if(wchipid == W83782D_WCHIPID) {
    w83781d_write_value(client,W83781D_REG_IN_MIN(7),
                        IN_TO_REG(W83781D_INIT_IN_MIN_7,7));
    w83781d_write_value(client,W83781D_REG_IN_MAX(7),
                        IN_TO_REG(W83781D_INIT_IN_MAX_7,7));
    w83781d_write_value(client,W83781D_REG_IN_MIN(8),
                        IN_TO_REG(W83781D_INIT_IN_MIN_8,8));
    w83781d_write_value(client,W83781D_REG_IN_MAX(8),
                        IN_TO_REG(W83781D_INIT_IN_MAX_8,8));
  }
  w83781d_write_value(client,W83781D_REG_FAN_MIN(1),
                      FAN_TO_REG(W83781D_INIT_FAN_MIN_1,2));
  w83781d_write_value(client,W83781D_REG_FAN_MIN(2),
                      FAN_TO_REG(W83781D_INIT_FAN_MIN_2,2));
  w83781d_write_value(client,W83781D_REG_FAN_MIN(3),
                      FAN_TO_REG(W83781D_INIT_FAN_MIN_3,2));

  w83781d_write_value(client,W83781D_REG_TEMP_OVER,
                      TEMP_TO_REG(W83781D_INIT_TEMP_OVER));
  w83781d_write_value(client,W83781D_REG_TEMP_HYST,
                      TEMP_TO_REG(W83781D_INIT_TEMP_HYST));
  w83781d_write_value(client,W83781D_REG_TEMP_CONFIG,0x00);

  w83781d_write_value(client,W83781D_REG_TEMP2_OVER,
                      TEMP_ADD_TO_REG(W83781D_INIT_TEMP2_OVER));
  w83781d_write_value(client,W83781D_REG_TEMP2_HYST,
                      TEMP_ADD_TO_REG(W83781D_INIT_TEMP2_HYST));
  w83781d_write_value(client,W83781D_REG_TEMP2_CONFIG,0x00);

  if(wchipid != W83783S_WCHIPID) {
    w83781d_write_value(client,W83781D_REG_TEMP3_OVER,
                        TEMP_ADD_TO_REG(W83781D_INIT_TEMP3_OVER));
    w83781d_write_value(client,W83781D_REG_TEMP3_HYST,
                        TEMP_ADD_TO_REG(W83781D_INIT_TEMP3_HYST));
    w83781d_write_value(client,W83781D_REG_TEMP3_CONFIG,0x00);
  }

  /* Start monitoring */
  w83781d_write_value(client,W83781D_REG_CONFIG,
                   (w83781d_read_value(client,
                                       W83781D_REG_CONFIG) & 0xf7) | 0x01);
}

void w83781d_update_client(struct i2c_client *client)
{
  struct w83781d_data *data = client->data;
  int i;

  down(&data->update_lock);

  if ((jiffies - data->last_updated > HZ+HZ/2 ) ||
      (jiffies < data->last_updated) || ! data->valid) {

#ifdef DEBUG
    printk("Starting w83781d update\n");
#endif
    for (i = 0; i <= 8; i++) {
      if(data->wchipid == W83783S_WCHIPID  &&  i == 1)
        continue; /* 783S has no in1 */
      data->in[i]     = w83781d_read_value(client,W83781D_REG_IN(i));
      data->in_min[i] = w83781d_read_value(client,W83781D_REG_IN_MIN(i));
      data->in_max[i] = w83781d_read_value(client,W83781D_REG_IN_MAX(i));
      if(data->wchipid != W83782D_WCHIPID  &&  i == 6)
        break;
    }
    for (i = 1; i <= 3; i++) {
      data->fan[i-1] = w83781d_read_value(client,W83781D_REG_FAN(i));
      data->fan_min[i-1] = w83781d_read_value(client,W83781D_REG_FAN_MIN(i));
    }
    if(data->wchipid != W83781D_WCHIPID) {
      for (i = 1; i <= 4; i++) {
        data->pwm[i-1] = w83781d_read_value(client,W83781D_REG_PWM(i));
        if((data->wchipid == W83783S_WCHIPID ||
           (data->wchipid == W83782D_WCHIPID && i2c_is_isa_client(client)))
          &&  i == 2)
          break;
      }
    }
    data->temp = w83781d_read_value(client,W83781D_REG_TEMP);
    data->temp_over = w83781d_read_value(client,W83781D_REG_TEMP_OVER);
    data->temp_hyst = w83781d_read_value(client,W83781D_REG_TEMP_HYST);
    data->temp_add[0] = w83781d_read_value(client,W83781D_REG_TEMP2);
    data->temp_add_over[0] = w83781d_read_value(client,W83781D_REG_TEMP2_OVER);
    data->temp_add_hyst[0] = w83781d_read_value(client,W83781D_REG_TEMP2_HYST);
    data->temp_add[1] = w83781d_read_value(client,W83781D_REG_TEMP3);
    data->temp_add_over[1] = w83781d_read_value(client,W83781D_REG_TEMP3_OVER);
    data->temp_add_hyst[1] = w83781d_read_value(client,W83781D_REG_TEMP3_HYST);
    i = w83781d_read_value(client,W83781D_REG_VID_FANDIV);
    data->vid = i & 0x0f;
    data->vid |= (w83781d_read_value(client,W83781D_REG_CHIPID) & 0x01) << 4;
    data->fan_div[0] = (i >> 4) & 0x03;
    data->fan_div[1] = i >> 6;
    if(data->wchipid != W83782D_WCHIPID) {
      data->fan_div[2] = (w83781d_read_value(client,W83781D_REG_PIN) >> 6) & 0x03;
    }
    data->alarms = w83781d_read_value(client,W83781D_REG_ALARM1) +
                   (w83781d_read_value(client,W83781D_REG_ALARM2) << 8);
    if(data->wchipid == W83782D_WCHIPID) {
      data->alarms |= w83781d_read_value(client,W83781D_REG_ALARM3) << 16;
    }
    i = w83781d_read_value(client,W83781D_REG_BEEP_INTS2);
    data->beep_enable = i >> 7;
    data->beeps = ((i & 0x7f) << 8) + 
                  w83781d_read_value(client,W83781D_REG_BEEP_INTS1);
    data->last_updated = jiffies;
    data->valid = 1;
  }

  up(&data->update_lock);
}


/* The next few functions are the call-back functions of the /proc/sys and
   sysctl files. Which function is used is defined in the ctl_table in
   the extra1 field.
   Each function must return the magnitude (power of 10 to divide the date
   with) if it is called with operation==SENSORS_PROC_REAL_INFO. It must
   put a maximum of *nrels elements in results reflecting the data of this
   file, and set *nrels to the number it actually put in it, if operation==
   SENSORS_PROC_REAL_READ. Finally, it must get upto *nrels elements from
   results and write them to the chip, if operations==SENSORS_PROC_REAL_WRITE.
   Note that on SENSORS_PROC_REAL_READ, I do not check whether results is
   large enough (by checking the incoming value of *nrels). This is not very
   good practice, but as long as you put less than about 5 values in results,
   you can assume it is large enough. */
void w83781d_in(struct i2c_client *client, int operation, int ctl_name, 
             int *nrels_mag, long *results)
{
  struct w83781d_data *data = client->data;
  int nr = ctl_name - W83781D_SYSCTL_IN0;

  if (operation == SENSORS_PROC_REAL_INFO)
    *nrels_mag = 2;
  else if (operation == SENSORS_PROC_REAL_READ) {
    w83781d_update_client(client);
    results[0] = IN_FROM_REG(data->in_min[nr],nr);
    results[1] = IN_FROM_REG(data->in_max[nr],nr);
    results[2] = IN_FROM_REG(data->in[nr],nr);
    *nrels_mag = 3;
  } else if (operation == SENSORS_PROC_REAL_WRITE) {
      if (*nrels_mag >= 1) {
        data->in_min[nr] = IN_TO_REG(results[0],nr);
        w83781d_write_value(client,W83781D_REG_IN_MIN(nr),data->in_min[nr]);
      }
      if (*nrels_mag >= 2) {
        data->in_max[nr] = IN_TO_REG(results[1],nr);
        w83781d_write_value(client,W83781D_REG_IN_MAX(nr),data->in_max[nr]);
      }
  }
}

void w83781d_fan(struct i2c_client *client, int operation, int ctl_name,
              int *nrels_mag, long *results)
{
  struct w83781d_data *data = client->data;
  int nr = ctl_name - W83781D_SYSCTL_FAN1 + 1;

  if (operation == SENSORS_PROC_REAL_INFO)
    *nrels_mag = 0;
  else if (operation == SENSORS_PROC_REAL_READ) {
    w83781d_update_client(client);
    results[0] = FAN_FROM_REG(data->fan_min[nr-1],
                              DIV_FROM_REG(data->fan_div[nr-1]));
    results[1] = FAN_FROM_REG(data->fan[nr-1],
                              DIV_FROM_REG(data->fan_div[nr-1]));
    *nrels_mag = 2;
  } else if (operation == SENSORS_PROC_REAL_WRITE) {
    if (*nrels_mag >= 1) {
      data->fan_min[nr-1] = FAN_TO_REG(results[0],
                                       DIV_FROM_REG(data->fan_div[nr-1]));
      w83781d_write_value(client,W83781D_REG_FAN_MIN(nr),data->fan_min[nr-1]);
    }
  }
}


void w83781d_temp(struct i2c_client *client, int operation, int ctl_name,
               int *nrels_mag, long *results)
{
  struct w83781d_data *data = client->data;
  if (operation == SENSORS_PROC_REAL_INFO)
    *nrels_mag = 1;
  else if (operation == SENSORS_PROC_REAL_READ) {
    w83781d_update_client(client);
    results[0] = TEMP_FROM_REG(data->temp_over);
    results[1] = TEMP_FROM_REG(data->temp_hyst);
    results[2] = TEMP_FROM_REG(data->temp);
    *nrels_mag = 3;
  } else if (operation == SENSORS_PROC_REAL_WRITE) {
    if (*nrels_mag >= 1) {
      data->temp_over = TEMP_TO_REG(results[0]);
      w83781d_write_value(client,W83781D_REG_TEMP_OVER,data->temp_over);
    }
    if (*nrels_mag >= 2) {
      data->temp_hyst = TEMP_TO_REG(results[1]);
      w83781d_write_value(client,W83781D_REG_TEMP_HYST,data->temp_hyst);
    }
  }
}


void w83781d_temp_add(struct i2c_client *client, int operation, int ctl_name,
                      int *nrels_mag, long *results)
{
  struct w83781d_data *data = client->data;
  int nr = ctl_name - W83781D_SYSCTL_TEMP2;

  if (operation == SENSORS_PROC_REAL_INFO)
    *nrels_mag = 1;
  else if (operation == SENSORS_PROC_REAL_READ) {
    w83781d_update_client(client);
    results[0] = TEMP_ADD_FROM_REG(data->temp_add_over[nr]);
    results[1] = TEMP_ADD_FROM_REG(data->temp_add_hyst[nr]);
    results[2] = TEMP_ADD_FROM_REG(data->temp_add[nr]);
    *nrels_mag = 3;
  } else if (operation == SENSORS_PROC_REAL_WRITE) {
    if (*nrels_mag >= 1) {
      data->temp_add_over[nr] = TEMP_ADD_TO_REG(results[0]);
      w83781d_write_value(client,
                          nr?W83781D_REG_TEMP3_OVER:W83781D_REG_TEMP2_OVER,
                          data->temp_add_over[nr]);
    }
    if (*nrels_mag >= 2) {
      data->temp_add_hyst[nr] = TEMP_ADD_TO_REG(results[1]);
      w83781d_write_value(client,
                          nr?W83781D_REG_TEMP3_HYST:W83781D_REG_TEMP2_HYST,
                          data->temp_add_hyst[nr]);
    }
  }
}


void w83781d_vid(struct i2c_client *client, int operation, int ctl_name,
              int *nrels_mag, long *results)
{
  struct w83781d_data *data = client->data;
  if (operation == SENSORS_PROC_REAL_INFO)
    *nrels_mag = 2;
  else if (operation == SENSORS_PROC_REAL_READ) {
    w83781d_update_client(client);
    results[0] = VID_FROM_REG(data->vid);
    *nrels_mag = 1;
  }
}

void w83781d_alarms(struct i2c_client *client, int operation, int ctl_name,
                 int *nrels_mag, long *results)
{
  struct w83781d_data *data = client->data;
  if (operation == SENSORS_PROC_REAL_INFO)
    *nrels_mag = 0;
  else if (operation == SENSORS_PROC_REAL_READ) {
    w83781d_update_client(client);
    results[0] = ALARMS_FROM_REG(data->alarms);
    *nrels_mag = 1;
  }
}

void w83781d_beep(struct i2c_client *client, int operation, int ctl_name,
                 int *nrels_mag, long *results)
{
  struct w83781d_data *data = client->data;
  int val;

  if (operation == SENSORS_PROC_REAL_INFO)
    *nrels_mag = 0;
  else if (operation == SENSORS_PROC_REAL_READ) {
    w83781d_update_client(client);
    results[0] = BEEP_ENABLE_FROM_REG(data->beep_enable);
    results[1] = BEEPS_FROM_REG(data->beeps);
    *nrels_mag = 2;
  } else if (operation == SENSORS_PROC_REAL_WRITE) {
    if (*nrels_mag >= 2) {
      data->beeps = BEEPS_TO_REG(results[1]);
      w83781d_write_value(client,W83781D_REG_BEEP_INTS1,data->beeps & 0xff);
      val = data->beeps >> 8;
    } else if (*nrels_mag >= 1)
      val = w83781d_read_value(client,W83781D_REG_BEEP_INTS1) & 0x7f;
    if (*nrels_mag >= 1) {
      data->beep_enable = BEEP_ENABLE_TO_REG(results[0]);
      w83781d_write_value(client,W83781D_REG_BEEP_INTS2,
                          val | data->beep_enable << 7);
    }
  }
}

void w83781d_fan_div(struct i2c_client *client, int operation, int ctl_name,
                  int *nrels_mag, long *results)
{
  struct w83781d_data *data = client->data;
  int old;

  if (operation == SENSORS_PROC_REAL_INFO)
    *nrels_mag = 0;
  else if (operation == SENSORS_PROC_REAL_READ) {
    w83781d_update_client(client);
    results[0] = DIV_FROM_REG(data->fan_div[0]);
    results[1] = DIV_FROM_REG(data->fan_div[1]);
    results[2] = DIV_FROM_REG(data->fan_div[2]);
    *nrels_mag = 3;
  } else if (operation == SENSORS_PROC_REAL_WRITE) {
    old = w83781d_read_value(client,W83781D_REG_VID_FANDIV);
    if (*nrels_mag >= 2) {
      data->fan_div[1] = DIV_TO_REG(results[1]);
      old = (old & 0x3f) | (data->fan_div[1] << 6);
    }
    if (*nrels_mag >= 1) {
      data->fan_div[0] = DIV_TO_REG(results[0]);
      old = (old & 0xcf) | (data->fan_div[0] << 4);
      w83781d_write_value(client,W83781D_REG_VID_FANDIV,old);
    }
    if (*nrels_mag >= 3) {
      data->fan_div[2] = DIV_TO_REG(results[2]);
      w83781d_write_value(client,W83781D_REG_PIN,
                          w83781d_read_value(client,W83781D_REG_PIN));
    }
  }
}

void w83781d_pwm(struct i2c_client *client, int operation, int ctl_name,
                 int *nrels_mag, long *results)
{
  struct w83781d_data *data = client->data;
  int nr = 1 + ctl_name - W83781D_SYSCTL_PWM1;

  if (operation == SENSORS_PROC_REAL_INFO)
    *nrels_mag = 0;
  else if (operation == SENSORS_PROC_REAL_READ) {
    w83781d_update_client(client);
    results[0] = PWM_FROM_REG(data->pwm[nr-1]);
    *nrels_mag = 1;
  } else if (operation == SENSORS_PROC_REAL_WRITE) {
    if (*nrels_mag >= 1) {
      data->pwm[nr-1] = PWM_TO_REG(results[0]);
      w83781d_write_value(client,W83781D_REG_PWM(nr),data->pwm[nr-1]);
    }
  }
}

void w83781d_sens(struct i2c_client *client, int operation, int ctl_name,
                 int *nrels_mag, long *results)
{
  struct w83781d_data *data = client->data;
  int nr = 1 + ctl_name - W83781D_SYSCTL_SENS1;
  u8 tmp;

  if (operation == SENSORS_PROC_REAL_INFO)
    *nrels_mag = 0;
  else if (operation == SENSORS_PROC_REAL_READ) {
    results[0] = data->sens[nr-1];
    *nrels_mag = 1;
  } else if (operation == SENSORS_PROC_REAL_WRITE) {
    if (*nrels_mag >= 1) {
      switch(results[0]) {
        case 1:				/* PII/Celeron diode */
          tmp = w83781d_read_value(client,W83781D_REG_SCFG1);
          w83781d_write_value(client,W83781D_REG_SCFG1, tmp | BIT_SCFG2[nr-1]);
          tmp = w83781d_read_value(client,W83781D_REG_SCFG2);
          w83781d_write_value(client,W83781D_REG_SCFG2, tmp | BIT_SCFG2[nr-1]);
          data->sens[nr-1] = results[0];
          break;
        case 2:				/* 3904 */
          tmp = w83781d_read_value(client,W83781D_REG_SCFG1);
          w83781d_write_value(client,W83781D_REG_SCFG1, tmp | BIT_SCFG2[nr-1]);
          tmp = w83781d_read_value(client,W83781D_REG_SCFG2);
          w83781d_write_value(client,W83781D_REG_SCFG2, tmp & ~ BIT_SCFG2[nr-1]);
          data->sens[nr-1] = results[0];
          break;
        case W83781D_DEFAULT_BETA: 	/* thermistor */
          tmp = w83781d_read_value(client,W83781D_REG_SCFG1);
          w83781d_write_value(client,W83781D_REG_SCFG1, tmp & ~ BIT_SCFG2[nr-1]);
          data->sens[nr-1] = results[0];
          break;
        default:
          printk("w83781d.o: Invalid sensor type %ld; must be 1, 2, or %d\n", results[0], W83781D_DEFAULT_BETA);
          break;
      }
    }
  }
}

#ifdef W83781D_RT
void w83781d_rt(struct i2c_client *client, int operation, int ctl_name,
                 int *nrels_mag, long *results)
{
  struct w83781d_data *data = client->data;
  int nr = 1 + ctl_name - W83781D_SYSCTL_RT1;
  int i;

  if (operation == SENSORS_PROC_REAL_INFO)
    *nrels_mag = 0;
  else if (operation == SENSORS_PROC_REAL_READ) {
    for(i = 0; i < 32; i++) {
      results[i] = data->rt[nr-1][i];
    }
    *nrels_mag = 32;
  }
}
#endif

int w83781d_init(void)
{
  int res;

  printk("w83781d.o version %s (%s)\n",LM_VERSION,LM_DATE);
  w83781d_initialized = 0;

  if ((res =i2c_add_driver(&w83781d_driver))) {
    printk("w83781d.o: Driver registration failed, module not inserted.\n");
    w83781d_cleanup();
    return res;
  }
  w83781d_initialized ++;
  return 0;
}

int w83781d_cleanup(void)
{
  int res;

  if (w83781d_initialized >= 1) {
    if ((res = i2c_del_driver(&w83781d_driver))) {
      printk("w83781d.o: Driver deregistration failed, module not removed.\n");
      return res;
    }
    w83781d_initialized --;
  }
  return 0;
}


#ifdef MODULE

MODULE_AUTHOR("Frodo Looijaard <frodol@dds.nl>, Philip Edelbrock <phil@netroedge.com>, and Mark Studebaker <mds@eng.paradyne.com>");
MODULE_DESCRIPTION("W83781D driver");

int init_module(void)
{
  return w83781d_init();
}

int cleanup_module(void)
{
  return w83781d_cleanup();
}

#endif /* MODULE */

