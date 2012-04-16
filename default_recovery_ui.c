/*
 * Copyright (C) 2009 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <linux/input.h>

#include "recovery_ui.h"
#include "common.h"
#include "extendedcommands.h"

char* MENU_HEADERS[] = { "||    main menu     |/____________________________,/|",
		         "|+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+|/|",
			  NULL
		       };


char* MENU_ITEMS_SAFE[] = {     "|| <1> safe-boot menu                             |/|",
				"|| <2> reboot / power off                         |/|",
				"|| <3> wipe menu                                  |/|",
				"|| <4> nandroid menu (backup and restore)         |/|",
				"|| <5> mounts and USB storage                     |/|",
				"|| <6> advanced and debugging menu                |/|",
				"|| <7> console menu                               |/|",
				"|| <8> update/patch menu                          |/|",
                                NULL };

 char* MENU_ITEMS[] =      {    "|| <1> safe-boot menu                             |/|",
				"|| <2> reboot / power off                         |/|",
				"|| <3> wipe menu                                  |/|",
				"|| <4> nandroid menu (backup and restore)         |/|",
				"|| <5> mounts and USB storage                     |/|",
				"|| <6> advanced and debugging menu                |/|",
				"|| <7> console menu                               |/|",
                                NULL };

 char* ADV_MENU_ITEMS_SAFE[] = {"|| <1> show log                                   |/|",
				"|| <2> dump log for error reporting               |/|",
				"|| <3> key event test                             |/|",
				"|| <4> wipe battery statistics                    |/|",
				"|| <5> fix permissions                            |/|",
#ifndef BOARD_HAS_SMALL_RECOVERY
                           	"|| <6> partition external SD card                 |/|",
#ifdef BOARD_HAS_SDCARD_INTERNAL
#ifndef BOARD_HAS_INTERNAL_PARTITIONS
                           	"|| <7> partition internal SD card                 |/|",
#endif
#endif
#endif
			   	NULL };
 
 char* ADV_MENU_ITEMS[] = {     "|| <1> show log                                   |/|",
				"|| <2> dump log for error reporting               |/|",
				"|| <3> key event test                             |/|",
				"|| <4> wipe battery statistics                    |/|",
				"|| <5> fix permissions                            |/|",
#ifndef BOARD_HAS_SMALL_RECOVERY
                           	"|| <6> partition external SD card                 |/|",
#ifdef BOARD_HAS_SDCARD_INTERNAL
#ifndef BOARD_HAS_INTERNAL_PARTITIONS
                           	"|| <7> partition internal SD card                 |/|",
#endif
#endif
#endif
				"|| <8> update/patch non-safe system (DANGEROUS)   |/|",
                           	NULL };

int device_recovery_start() {
    return 0;
}

int device_reboot_now(volatile char* key_pressed, int key_code) {
    return 0;
}

int device_handle_key(int key_code) {
    
        switch (key_code) {
            case KEY_CAPSLOCK:
            case KEY_DOWN:
            case KEY_VOLUMEDOWN:
            case KEY_MENU:
                return HIGHLIGHT_DOWN;

            case KEY_LEFTSHIFT:
            case KEY_UP:
            case KEY_VOLUMEUP:
            case KEY_HOME:
                return HIGHLIGHT_UP;

            case KEY_POWER:
                if (ui_get_showing_back_button()) {
                    return SELECT_ITEM;
                }
                
            case KEY_LEFTBRACE:
            case KEY_ENTER:
            case BTN_MOUSE:
            case KEY_CENTER:
            case KEY_CAMERA:
            case KEY_F21:
            case KEY_SEND:
                return SELECT_ITEM;
            
            case KEY_END:
            case KEY_BACKSPACE:
            case KEY_SEARCH:
                if (ui_get_showing_back_button()) {
                    return SELECT_ITEM;
                }
                
            case KEY_BACK:
                return GO_BACK;

	    case KEY_0:
		return (SELECT_9-SELECT_OFFSET);

	    case KEY_1:
		return (SELECT_0-SELECT_OFFSET);

	    case KEY_2:
		return (SELECT_1-SELECT_OFFSET);

	    case KEY_3:
		return (SELECT_2-SELECT_OFFSET);

	    case KEY_4:
		return (SELECT_3-SELECT_OFFSET);

	    case KEY_5:
		return (SELECT_4-SELECT_OFFSET);

	    case KEY_6:
		return (SELECT_5-SELECT_OFFSET);

	    case KEY_7:
		return (SELECT_6-SELECT_OFFSET);

	    case KEY_8:
		return (SELECT_7-SELECT_OFFSET);

	    case KEY_9:
		return (SELECT_8-SELECT_OFFSET);

	    case KEY_A:
		return (SELECT_10-SELECT_OFFSET);

	    case KEY_B:
		return (SELECT_11-SELECT_OFFSET);

        }
    
    return NO_ACTION;
}

int device_perform_action(int which) {
    return which;
}

int device_wipe_data() {
    return 0;
}
