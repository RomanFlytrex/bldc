/*
	Copyright 2017 Arvid Brodin	arvidb@kth.se
	Copyright 2018 Vadim	vadim@flytrex

	This file is meant to be compiled as part of Benjamin Vedder's
	VESC firmware.

	This file is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	This file is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <math.h>
#include <string.h>
#include <stdio.h>
#include "mc_interface.h"
#include "timeout.h"

// for debug terminal functionality
#include "terminal.h"
#include "commands.h"
#include "utils.h"

#include "pid.h"
#include "app_brake.h"

#define GEN_UPDATE_RATE_HZ	1000
#define SLEEP_TIME (CH_CFG_ST_FREQUENCY / GEN_UPDATE_RATE_HZ)

#define MAX_CURRENT 20.0

#define RPM_THRESHOLD 150

static volatile bool stop_now = true;
static volatile bool is_running = false;
static volatile bool is_active = false;
static volatile float target_rpm = 1000;
static volatile float Kp = 0.005;
static volatile float Ki = 0.015;
static volatile float Kd = 0;
static volatile char calc_mode = 0;

// Threads
static THD_FUNCTION(gen_thread, arg);
static THD_WORKING_AREA(gen_thread_wa, 1024);


void app_custom_start(void) {
	stop_now = false;
	chThdCreateStatic(gen_thread_wa, sizeof(gen_thread_wa), NORMALPRIO, gen_thread, NULL);
}

void app_custom_stop(void) {
	stop_now = true;
	while (is_running) {
		chThdSleepMilliseconds(1);
	}
}


// Private functions
static void terminal_cmd_brake_status(int argc, const char **argv);

void app_custom_configure(app_configuration *conf) {
	(void) conf;

	terminal_register_command_callback(
			"brake",
			"Print the status of the brake app",
			0,
			terminal_cmd_brake_status);
}



static volatile float brake_current_val = 0;
static volatile float brake_rpm_error = 0;
static volatile float current_integral_val = 0;


float app_brake_rpm_error(void){
	return brake_rpm_error;
}

float app_brake_current_command(void){
	return brake_current_val;
}

float app_brake_current_integral_val(void){
	return current_integral_val;
}

char app_brake_status(void) {

    char s = 0;
    s |= (is_active)  ? 1   : 0;
    s |= (is_running) ? 2   : 0;
    s |= calc_mode<<4;
    return s;
}

static THD_FUNCTION(gen_thread, arg) {
	(void)arg;

	is_running = true;
	PID pid;
	for(;;) {
		if (is_active) {
            calc_mode = 0;
            float current = 0;
			const float rpm_now_dir = mc_interface_get_rpm();
			const float rpm_now = fabsf(rpm_now_dir);
			const float rpm_error = rpm_now - target_rpm;

			if (rpm_now < RPM_THRESHOLD){
				pid = pid_init(1.0/GEN_UPDATE_RATE_HZ, MAX_CURRENT, 0, Kp, Kd, Ki);
				mc_interface_set_duty(0);
				calc_mode = 1;
			}
			else
            {
				current = (float)pid_calc(&pid, rpm_error ); // positive error - too fast, more braking needed
                current *= -SIGN(rpm_now_dir);

				if(fabsf(current) < 0.5  && pid.integral>0.) { // "cogging" mode, use only when slowing down
                    calc_mode = 2;
                    mc_interface_set_brake_current(0.4);
                    //mc_interface_set_current(current);
                }
                else {
                    calc_mode = 3;
                    mc_interface_set_current(current);
                }
            }

			// for reporting
			brake_rpm_error = rpm_error/1000;
			brake_current_val = current;
			current_integral_val = pid.integral;
		} // is_active

		// Sleep for a time according to the specified rate
		systime_t sleep_time = SLEEP_TIME;

		// At least one tick should be slept to not block the other threads
		if (sleep_time == 0) {
			sleep_time = 1;
		}
		chThdSleep(sleep_time);

		if (stop_now) {
			is_running = false;
			return;
		}

		// Reset timeout
		timeout_reset();
	}
}


static void terminal_cmd_brake_status(int argc, const char **argv) {

    if (argc > 1) {     // parse command argument
        if (strcmp(argv[1], "on") == 0) {
            is_active = true;
        } else if (strcmp(argv[1], "off") == 0) {
            is_active = false;
        } else if (argc==3 && strcmp(argv[1], "rpm") == 0) {
            sscanf(argv[2], "%f", &target_rpm);
        } else if (argc==3 && strcmp(argv[1], "lim") == 0) {
        	float limit = 0.0;
            sscanf(argv[2], "%f", &limit);
            mc_interface_set_current_limit2(limit);
		} else if (argc==3 && strcmp(argv[1], "kp") == 0) {
			sscanf(argv[2], "%f", &Kp);
		} else if (argc==3 && strcmp(argv[1], "ki") == 0) {
			sscanf(argv[2], "%f", &Ki);
		} else if (argc==3 && strcmp(argv[1], "kd") == 0) {
			sscanf(argv[2], "%f", &Kd);
		} else if (argc==5 && strcmp(argv[1], "pid") == 0) {
			sscanf(argv[2], "%f", &Kp);
			sscanf(argv[3], "%f", &Ki);
			sscanf(argv[4], "%f", &Kd);
		}

    } else if (argc==1) {
        commands_printf("Brake Status");
        commands_printf("   FW version: %s", GIT_VERSION);
        commands_printf("   App running: %s", is_running ? "On" : "Off");
        commands_printf("   Active: %s", is_active ? "On" : "Off");
        commands_printf("   Target RPM: %.1f", (double)target_rpm);
        commands_printf("   Kp: %.6f", (double)Kp);
        commands_printf("   Ki: %.6f", (double)Ki);
        commands_printf("   Kd: %.6f", (double)Kd);
        commands_printf(" ");
    }

}
