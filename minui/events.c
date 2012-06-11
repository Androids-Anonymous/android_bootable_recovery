/*
* Copyright (C) 2007 The Android Open Source Project
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
* http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/poll.h>
#include <limits.h>

#include <linux/input.h>

#include "minui.h"

#define MAX_DEVICES 16
#define MAX_MISC_FDS 16
#define PRESS_THRESHOLD 10

#define VIBRATOR_TIMEOUT_FILE "/sys/class/timed_output/vibrator/enable"

#define ABS_MT_POSITION_X 0x35
#define ABS_MT_POSITION_Y 0x36
#define ABS_MT_TOUCH_MAJOR 0x30
#define SYN_MT_REPORT 2

#define NORMAL_DELAY 225
#define FAST_DELAY 75

int keyhold_delay = NORMAL_DELAY; 
int vibrator_time_ms = 30;

enum {
    DOWN_NOT,
    DOWN_SENT,
    DOWN_RELEASED,
};

#define BITS_PER_LONG (sizeof(unsigned long) * 8)
#define BITS_TO_LONGS(x) (((x) + BITS_PER_LONG - 1) / BITS_PER_LONG)

#define test_bit(bit, array) \
    ((array)[(bit)/BITS_PER_LONG] & (1 << ((bit) % BITS_PER_LONG)))

struct fd_info {
    ev_callback cb;
    void *data;
};

struct position {
    int x, y;
    int pressed;
    struct input_absinfo xi, yi;
};

struct ev {
	struct pollfd *fd;
	struct position p, mt_p;
	int sent, sent2, mt_idx;
};

static struct pollfd ev_fds[MAX_DEVICES + MAX_MISC_FDS];
static struct fd_info ev_fdinfo[MAX_DEVICES + MAX_MISC_FDS];
static struct ev evs[MAX_DEVICES + MAX_MISC_FDS];

static unsigned ev_count = 0;
static unsigned ev_dev_count = 0;
static unsigned ev_misc_count = 0;

int vibrate(int timeout_ms)
{
    char str[20];
    int fd;
    int ret;

    fd = open(VIBRATOR_TIMEOUT_FILE, O_WRONLY);
    if (fd < 0)
        return -1;

    ret = snprintf(str, sizeof(str), "%d", timeout_ms);
    ret = write(fd, str, ret);
    close(fd);

    if (ret < 0)
       return -1;

    return 0;
}
/* Returns empty tokens */
int ev_init(void)
//int ev_init(ev_callback input_cb, void *data)
{
    DIR *dir;
    struct dirent *de;
    int fd;

    dir = opendir("/dev/input");
    if(dir != 0) {
        while((de = readdir(dir))) {
            unsigned long ev_bits[BITS_TO_LONGS(EV_MAX)];

//            fprintf(stderr,"/dev/input/%s\n", de->d_name);
            if(strncmp(de->d_name,"event",5)) continue;
            fd = openat(dirfd(dir), de->d_name, O_RDONLY);
            if(fd < 0) continue;

            /* read the evbits of the input device */
            if (ioctl(fd, EVIOCGBIT(0, sizeof(ev_bits)), ev_bits) < 0) {
                close(fd);
                continue;
            }

            /* TODO: add ability to specify event masks. For now, just assume
             * that only EV_KEY and EV_REL event types are ever needed. */
#ifdef BOARD_HAS_VIRTUAL_KEYS
            if (!test_bit(EV_KEY, ev_bits) && !test_bit(EV_REL, ev_bits) && !test_bit(EV_ABS, ev_bits)) {
#else
            if (!test_bit(EV_KEY, ev_bits) && !test_bit(EV_REL, ev_bits)) {
#endif
                close(fd);
                continue;
            }

            ev_fds[ev_count].fd = fd;
            ev_fds[ev_count].events = POLLIN;
            //ev_fdinfo[ev_count].cb = input_cb;
            //ev_fdinfo[ev_count].data = data;
	    evs[ev_count].fd = &ev_fds[ev_count];
            /* Load virtualkeys if there are any */
            ev_count++;
            ev_dev_count++;
            if(ev_dev_count == MAX_DEVICES) break;
        }
    }

    return 0;
}

int ev_add_fd(int fd, ev_callback cb, void *data)
{
    if (ev_misc_count == MAX_MISC_FDS || cb == NULL)
        return -1;

    ev_fds[ev_count].fd = fd;
    ev_fds[ev_count].events = POLLIN;
    ev_fdinfo[ev_count].cb = cb;
    ev_fdinfo[ev_count].data = data;
    ev_count++;
    ev_misc_count++;
    return 0;
}

void ev_exit(void)
{
    while (ev_count > 0) {
        close(ev_fds[--ev_count].fd);
    }
    ev_misc_count = 0;
    ev_dev_count = 0;
}

int ev_get_input(struct input_event *ev, unsigned dont_wait, unsigned keyheld, unsigned fast){    
	int r;    
	unsigned n;    
	
	do {        
	// When keyheld is true, the previous event        
	// was an up/down keypress so wait keyhold_delay.
		if(fast) {	    
			keyhold_delay = FAST_DELAY;
		} else {           
			keyhold_delay = NORMAL_DELAY;	
		}	
		r = poll(ev_fds, ev_count, dont_wait ? 0 : keyheld ? keyhold_delay : -1);

		if(r > 0) {	    
			for(n = 0; n < ev_count; n++) {
				if(ev_fds[n].revents & POLLIN) {
					r = read(ev_fds[n].fd, ev, sizeof(*ev));
					if(r == sizeof(*ev)) {
							return 0;
					}
				}
			}
		}  if (r == 0 ) {
		// If a timeout occurred when keyheld was set, let the
		// caller know so it can generate a repeated event.
		   return 1;        
		}    
        } while(dont_wait == 0);
      	
	return -1;
}

int ev_sync_key_state(ev_set_key_callback set_key_cb, void *data)
{
    unsigned long key_bits[BITS_TO_LONGS(KEY_MAX)];
    unsigned long ev_bits[BITS_TO_LONGS(EV_MAX)];
    unsigned i;
    int ret;

    for (i = 0; i < ev_dev_count; i++) {
        int code;

        memset(key_bits, 0, sizeof(key_bits));
        memset(ev_bits, 0, sizeof(ev_bits));

        ret = ioctl(ev_fds[i].fd, EVIOCGBIT(0, sizeof(ev_bits)), ev_bits);
        if (ret < 0 || !test_bit(EV_KEY, ev_bits))
            continue;

        ret = ioctl(ev_fds[i].fd, EVIOCGKEY(sizeof(key_bits)), key_bits);
        if (ret < 0)
            continue;

        for (code = 0; code <= KEY_MAX; code++) {
            if (test_bit(code, key_bits))
                set_key_cb(code, 1, data);
        }
    }

    return 0;
}
