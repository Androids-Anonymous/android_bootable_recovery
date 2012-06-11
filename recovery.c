/* Copyright (C) 2007 The Android Open Source Project
 *
 * Copyright (c) 2010, Code Aurora Forum. All rights reserved.
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

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <linux/input.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <signal.h>

#include "bootloader.h"
#include "firmware.h"
#include "common.h"
#include "cutils/properties.h"
#include "cutils/android_reboot.h"
#include "install.h"
#include <minui/minui.h>
#include "minzip/DirUtil.h"
#include "roots.h"
#include "recovery_ui.h"

#include "extendedcommands.h"
#include "flashutils/flashutils.h"

#include "safebootcommands.h"

#include "console.h"

#define MENU_HEADER_COLS 55

static const struct option OPTIONS[] = {
  { "send_intent", required_argument, NULL, 's' },
  { "update_package", required_argument, NULL, 'u' },
  { "wipe_data", no_argument, NULL, 'w' },
  { "wipe_cache", no_argument, NULL, 'c' },
  { "set_encrypted_filesystems", required_argument, NULL, 'e' },
  { "show_text", no_argument, NULL, 't' },
  { NULL, 0, NULL, 0 },
};

static const char *COMMAND_FILE = "/cache/recovery/command";
static const char *INTENT_FILE = "/cache/recovery/intent";
static const char *LOG_FILE = "/cache/recovery/log";
static const char *LAST_LOG_FILE = "/cache/recovery/last_log";
static const char *CACHE_ROOT = "/cache";
static const char *SDCARD_ROOT = "/sdcard";
static const char *EMMC_ROOT = "/emmc";
static int allow_display_toggle = 1;
static int poweroff = 0;
static const char *SDCARD_PACKAGE_FILE = "/sdcard/update.zip";
static const char *TEMPORARY_LOG_FILE = "/tmp/recovery.log";
static const char *SIDELOAD_TEMP_DIR = "/tmp/sideload";
static int poweroff;

/*
 * The recovery tool communicates with the main system through /cache files.
 *   /cache/recovery/command - INPUT - command line for tool, one arg per line
 *   /cache/recovery/log - OUTPUT - combined log file from recovery run(s)
 *   /cache/recovery/intent - OUTPUT - intent that was passed in
 *
 * The arguments which may be supplied in the recovery.command file:
 *   --send_intent=anystring - write the text out to recovery.intent
 *   --update_package=path - verify install an OTA package file
 *   --wipe_data - erase user data (and cache), then reboot
 *   --wipe_cache - wipe cache (but not user data), then reboot
 *   --set_encrypted_filesystem=on|off - enables / diasables encrypted fs
 *
 * After completing, we remove /cache/recovery/command and reboot.
 * Arguments may also be supplied in the bootloader control block (BCB).
 * These important scenarios must be safely restartable at any point:
 *
 * FACTORY RESET
 * 1. user selects "factory reset"
 * 2. main system writes "--wipe_data" to /cache/recovery/command
 * 3. main system reboots into recovery
 * 4. get_args() writes BCB with "boot-recovery" and "--wipe_data"
 *    -- after this, rebooting will restart the erase --
 * 5. erase_volume() reformats /data
 * 6. erase_volume() reformats /cache
 * 7. finish_recovery() erases BCB
 *    -- after this, rebooting will restart the main system --
 * 8. main() calls reboot() to boot main system
 *
 * OTA INSTALL
 * 1. main system downloads OTA package to /cache/some-filename.zip
 * 2. main system writes "--update_package=/cache/some-filename.zip"
 * 3. main system reboots into recovery
 * 4. get_args() writes BCB with "boot-recovery" and "--update_package=..."
 *    -- after this, rebooting will attempt to reinstall the update --
 * 5. install_package() attempts to install the update
 *    NOTE: the package install must itself be restartable from any point
 * 6. finish_recovery() erases BCB
 *    -- after this, rebooting will (try to) restart the main system --
 * 7. ** if install failed **
 *    7a. prompt_and_wait() shows an error icon and waits for the user
 *    7b; the user reboots (pulling the battery, etc) into the main system
 * 8. main() calls maybe_install_firmware_update()
 *    ** if the update contained radio/hboot firmware **:
 *    8a. m_i_f_u() writes BCB with "boot-recovery" and "--wipe_cache"
 *        -- after this, rebooting will reformat cache & restart main system --
 *    8b. m_i_f_u() writes firmware image into raw cache partition
 *    8c. m_i_f_u() writes BCB with "update-radio/hboot" and "--wipe_cache"
 *        -- after this, rebooting will attempt to reinstall firmware --
 *    8d. bootloader tries to flash firmware
 *    8e. bootloader writes BCB with "boot-recovery" (keeping "--wipe_cache")
 *        -- after this, rebooting will reformat cache & restart main system --
 *    8f. erase_volume() reformats /cache
 *    8g. finish_recovery() erases BCB
 *        -- after this, rebooting will (try to) restart the main system --
 * 9. main() calls reboot() to boot main system
 *
 * SECURE FILE SYSTEMS ENABLE/DISABLE
 * 1. user selects "enable encrypted file systems"
 * 2. main system writes "--set_encrypted_filesystems=on|off" to
 *    /cache/recovery/command
 * 3. main system reboots into recovery
 * 4. get_args() writes BCB with "boot-recovery" and
 *    "--set_encrypted_filesystems=on|off"
 *    -- after this, rebooting will restart the transition --
 * 5. read_encrypted_fs_info() retrieves encrypted file systems settings from /data
 *    Settings include: property to specify the Encrypted FS istatus and
 *    FS encryption key if enabled (not yet implemented)
 * 6. erase_volume() reformats /data
 * 7. erase_volume() reformats /cache
 * 8. restore_encrypted_fs_info() writes required encrypted file systems settings to /data
 *    Settings include: property to specify the Encrypted FS status and
 *    FS encryption key if enabled (not yet implemented)
 * 9. finish_recovery() erases BCB
 *    -- after this, rebooting will restart the main system --
 * 10. main() calls reboot() to boot main system
 */

static const int MAX_ARG_LENGTH = 4096;
static const int MAX_ARGS = 100;

// open a given path, mounting partitions as necessary
FILE*
fopen_path(const char *path, const char *mode) {
    if (ensure_path_mounted(path) != 0) {
        LOGE("can't mount %s\n", path);
        return NULL;
    }

    // When writing, try to create the containing directory, if necessary.
    // Use generous permissions, the system (init.rc) will reset them.
    if (strchr("wa", mode[0])) dirCreateHierarchy(path, 0777, NULL, 1);

    FILE *fp = fopen(path, mode);
    if (fp == NULL && path != COMMAND_FILE) LOGE("can't open %s\n", path);
    return fp;
}

// close a file, log an error if the error indicator is set
static void
check_and_fclose(FILE *fp, const char *name) {
    fflush(fp);
    if (ferror(fp)) LOGE("error in %s\n(%s)\n", name, strerror(errno));
    fclose(fp);
}

// command line args come from, in decreasing precedence:
//   - the actual command line
//   - the bootloader control block (one per line, after "recovery")
//   - the contents of COMMAND_FILE (one per line)
static void
get_args(int *argc, char ***argv) {
    struct bootloader_message boot;
    memset(&boot, 0, sizeof(boot));
    if (device_flash_type() == MTD || device_flash_type() == MMC) {
        get_bootloader_message(&boot);  // this may fail, leaving a zeroed structure
    }

    if (boot.command[0] != 0 && boot.command[0] != 255) {
        LOGI("boot command: %.*s\n", sizeof(boot.command), boot.command);
    }

    if (boot.status[0] != 0 && boot.status[0] != 255) {
        LOGI("boot status: %.*s\n", sizeof(boot.status), boot.status);
    }

    struct stat file_info;

    // --- if arguments weren't supplied, look in the bootloader control block
    if (*argc <= 1 && 0 != stat("/tmp/.ignorebootmessage", &file_info)) {
        boot.recovery[sizeof(boot.recovery) - 1] = '\0';  // Ensure termination
        const char *arg = strtok(boot.recovery, "\n");
        if (arg != NULL && !strcmp(arg, "recovery")) {
            *argv = (char **) malloc(sizeof(char *) * MAX_ARGS);
            (*argv)[0] = strdup(arg);
            for (*argc = 1; *argc < MAX_ARGS; ++*argc) {
                if ((arg = strtok(NULL, "\n")) == NULL) break;
                (*argv)[*argc] = strdup(arg);
            }
            LOGI("got arguments from boot message\n");
        } else if (boot.recovery[0] != 0 && boot.recovery[0] != 255) {
            LOGE("bad boot message\n\"%.20s\"\n", boot.recovery);
        }
    }

    // --- if that doesn't work, try the command file
    if (*argc <= 1) {
        FILE *fp = fopen_path(COMMAND_FILE, "r");
        if (fp != NULL) {
            char *argv0 = (*argv)[0];
            *argv = (char **) malloc(sizeof(char *) * MAX_ARGS);
            (*argv)[0] = argv0;  // use the same program name

            char buf[MAX_ARG_LENGTH];
            for (*argc = 1; *argc < MAX_ARGS; ++*argc) {
                if (!fgets(buf, sizeof(buf), fp)) break;
                (*argv)[*argc] = strdup(strtok(buf, "\r\n"));  // Strip newline.
            }

            check_and_fclose(fp, COMMAND_FILE);
            LOGI("got arguments from %s\n", COMMAND_FILE);
        }
    }

    // --> write the arguments we have back into the bootloader control block
    // always boot into recovery after this (until finish_recovery() is called)
    strlcpy(boot.command, "boot-recovery", sizeof(boot.command));
    strlcpy(boot.recovery, "recovery\n", sizeof(boot.recovery));
    int i;
    for (i = 1; i < *argc; ++i) {
        strlcat(boot.recovery, (*argv)[i], sizeof(boot.recovery));
        strlcat(boot.recovery, "\n", sizeof(boot.recovery));
    }
    if (device_flash_type() == MTD) {
        set_bootloader_message(&boot);
    }
}

void
set_sdcard_update_bootloader_message() {
    struct bootloader_message boot;
    memset(&boot, 0, sizeof(boot));
    strlcpy(boot.command, "boot-recovery", sizeof(boot.command));
    strlcpy(boot.recovery, "recovery\n", sizeof(boot.recovery));
    set_bootloader_message(&boot);
}

// How much of the temp log we have copied to the copy in cache.
static long tmplog_offset = 0;

void copy_log_file(const char* destination, int append) {
    FILE *log = fopen_path(destination, append ? "a" : "w");
    if (log == NULL) {
        LOGE("can't open %s\n", destination);
    } else {
        FILE *tmplog = fopen(TEMPORARY_LOG_FILE, "r");
        if (tmplog == NULL) {
            LOGE("can't open %s\n", TEMPORARY_LOG_FILE);
        } else {
            if (append) {
                fseek(tmplog, tmplog_offset, SEEK_SET);  // Since last write
            }
            char buf[4096];
            while (fgets(buf, sizeof(buf), tmplog)) fputs(buf, log);
            if (append) {
                tmplog_offset = ftell(tmplog);
            }
            check_and_fclose(tmplog, TEMPORARY_LOG_FILE);
        }
        check_and_fclose(log, destination);
    }
}


// clear the recovery command and prepare to boot a (hopefully working) system,
// copy our log file to cache as well (for the system to read), and
// record any intent we were asked to communicate back to the system.
// this function is idempotent: call it as many times as you like.
void finish_recovery(const char *send_intent) {
    // By this point, we're ready to return to the main system...
    if (send_intent != NULL) {
        FILE *fp = fopen_path(INTENT_FILE, "w");
        if (fp == NULL) {
            LOGE("can't open %s\n", INTENT_FILE);
        } else {
            fputs(send_intent, fp);
            check_and_fclose(fp, INTENT_FILE);
        }
    }

    // Copy logs to cache so the system can find out what happened.
    copy_log_file(LOG_FILE, true);
    copy_log_file(LAST_LOG_FILE, false);
    chmod(LAST_LOG_FILE, 0640);

    if (device_flash_type() == MTD) {
        // Reset to normal system boot so recovery won't cycle indefinitely.
        struct bootloader_message boot;
        memset(&boot, 0, sizeof(boot));
        set_bootloader_message(&boot);
    }

    // Remove the command file, so recovery won't repeat indefinitely.
    if (ensure_path_mounted(COMMAND_FILE) != 0 ||
        (unlink(COMMAND_FILE) && errno != ENOENT)) {
        LOGW("can't unlink %s\n", COMMAND_FILE);
    }

    sync();  // For good measure.
}

int erase_volume(const char *volume) {
    //ui_set_background(BACKGROUND_ICON_INSTALLING);
    ui_show_indeterminate_progress();
    ui_print("formatting %s...\n", volume);

    if (strcmp(volume, "/cache") == 0) {
        // Any part of the log we'd copied to cache is now gone.
        // Reset the pointer so we copy from the beginning of the temp
        // log.
        tmplog_offset = 0;
    }

    return format_volume(volume);
}

char* copy_sideloaded_package(const char* original_path) {
  if (ensure_path_mounted(original_path) != 0) {
    LOGE("can't mount %s\n", original_path);
    return NULL;
  }

  if (ensure_path_mounted(SIDELOAD_TEMP_DIR) != 0) {
    LOGE("can't mount %s\n", SIDELOAD_TEMP_DIR);
    return NULL;
  }

  if (mkdir(SIDELOAD_TEMP_DIR, 0700) != 0) {
    if (errno != EEXIST) {
      LOGE("can't mkdir %s (%s)\n", SIDELOAD_TEMP_DIR, strerror(errno));
      return NULL;
    }
  }

  // verify that SIDELOAD_TEMP_DIR is exactly what we expect: a
  // directory, owned by root, readable and writable only by root.
  struct stat st;
  if (stat(SIDELOAD_TEMP_DIR, &st) != 0) {
    LOGE("failed to stat %s (%s)\n", SIDELOAD_TEMP_DIR, strerror(errno));
    return NULL;
  }
  if (!S_ISDIR(st.st_mode)) {
    LOGE("%s isn't a directory\n", SIDELOAD_TEMP_DIR);
    return NULL;
  }
  if ((st.st_mode & 0777) != 0700) {
    LOGE("%s has perms %o\n", SIDELOAD_TEMP_DIR, st.st_mode);
    return NULL;
  }
  if (st.st_uid != 0) {
    LOGE("%s owned by %lu; not root\n", SIDELOAD_TEMP_DIR, st.st_uid);
    return NULL;
  }

  char copy_path[PATH_MAX];
  strcpy(copy_path, SIDELOAD_TEMP_DIR);
  strcat(copy_path, "/package.zip");

  char* buffer = malloc(BUFSIZ);
  if (buffer == NULL) {
    LOGE("failed to allocate buffer\n");
    return NULL;
  }

  size_t read;
  FILE* fin = fopen(original_path, "rb");
  if (fin == NULL) {
    LOGE("failed to open %s (%s)\n", original_path, strerror(errno));
    return NULL;
  }
  FILE* fout = fopen(copy_path, "wb");
  if (fout == NULL) {
    LOGE("failed to open %s (%s)\n", copy_path, strerror(errno));
    return NULL;
  }

  while ((read = fread(buffer, 1, BUFSIZ, fin)) > 0) {
    if (fwrite(buffer, 1, read, fout) != read) {
      LOGE("short write of %s (%s)\n", copy_path, strerror(errno));
      return NULL;
    }
  }

  free(buffer);

  if (fclose(fout) != 0) {
    LOGE("failed to close %s (%s)\n", copy_path, strerror(errno));
    return NULL;
  }

  if (fclose(fin) != 0) {
    LOGE("failed to close %s (%s)\n", original_path, strerror(errno));
    return NULL;
  }

  // "adb push" is happy to overwrite read-only files when it's
  // running as root, but we'll try anyway.
  if (chmod(copy_path, 0400) != 0) {
    LOGE("failed to chmod %s (%s)\n", copy_path, strerror(errno));
    return NULL;
  }

  return strdup(copy_path);
}

char** prepend_title(const char** headers) {
    char tmp1[PATH_MAX];
    char tmp2[PATH_MAX];
    
    sprintf(tmp1,     "|| battery:         |/|  %s  |", EXPAND(RECOVERY_VERSION));
    sprintf(tmp2,     "||                  |/|  safe system is:             ");
  //sprintf(tmp2,     "||__________________|/___safe system is:__%s", safemode ? "_ENABLED_|/" : "DISABLED_|/");
    char* title[] = { "//__________________//|                             |", 
		      "",
                      "",
		      "||__________________|/|_____________________________|", 
		       NULL
                    };
    
    title[1] = strdup(tmp1);
    title[2] = strdup(tmp2);

    // count the number of lines in our title, plus the
    // caller-provided headers.
    int count = 0;
    char** p;
    for (p = title; *p; ++p, ++count);
    for (p = headers; *p; ++p, ++count);
    
    char** new_headers = malloc((count+1) * sizeof(char*));
    char** h = new_headers;
    for (p = title; *p; ++p, ++h) *h = *p;
    for (p = headers; *p; ++p, ++h) *h = *p;
    *h = NULL;

    return new_headers;
}

int get_menu_selection(char** headers, char** items, int menu_only,
                   int initial_selection, int main_menu, int adv_menu) {
    // throw away keys pressed previously, so user doesn't
    // accidentally trigger menu items.
    ui_clear_key_queue();
    
    if(main_menu){
	items=MENU_ITEMS_SAFE;
    }

    if(adv_menu){
	items=ADV_MENU_ITEMS_SAFE;
    }

    int item_count = ui_start_menu(headers, items, initial_selection);
    int selected = initial_selection;
    int chosen_item = -1;

    // Some users with dead enter keys need a way to turn on power to select.
    // Jiggering across the wrapping menu is one "secret" way to enable it.
    // We can't rely on /cache or /sdcard since they may not be available.
    int wrap_count = 0;

    while (chosen_item < 0 && chosen_item != GO_BACK) {
        int key = ui_wait_key();
        int visible = ui_text_visible();

        int action = device_handle_key(key);

        int old_selected = selected;

        if (action < 0) {
            switch (action) {
                case HIGHLIGHT_UP:
                    --selected;
                    selected = ui_menu_select(selected);
                    break;
                case HIGHLIGHT_DOWN:
                    ++selected;
                    selected = ui_menu_select(selected);
                    break;
                case SELECT_ITEM:
                    chosen_item = selected;
                    if (ui_get_showing_back_button()) {
                        if (chosen_item == item_count) {
                            chosen_item = GO_BACK;
                        }
                    }
                    break;
                case NO_ACTION:
                    break;
                case GO_BACK:
                    chosen_item = GO_BACK;
                    break;
		case SELECT_0:
		    chosen_item = 9;
		    break;
		case SELECT_1:
		    chosen_item = 0;
		    break;
		case SELECT_2:
		    chosen_item = 1;
		    break;
		case SELECT_3:
		    chosen_item = 2;
		    break;
		case SELECT_4:
		    chosen_item = 3;
		    break;
		case SELECT_5:
		    chosen_item = 4;
		    break;
		case SELECT_6:
		    chosen_item = 5;
		    break;
		case SELECT_7:
		    chosen_item = 6;
		    break;
		case SELECT_8:
		    chosen_item = 7;
		    break;
		case SELECT_9:
		    chosen_item = 8;
		    break;
            }
        } else if (!menu_only) {
            chosen_item = action;
        }

    }

    ui_end_menu();
    ui_clear_key_queue();
    return chosen_item;
}

int compare_string(const void* a, const void* b) {
    return strcmp(*(const char**)a, *(const char**)b);
}

void prompt_and_wait() {
    safemode = get_safe_mode();
    char** headers = NULL;
    
    for (;;) {
        headers = prepend_title((const char**)MENU_HEADERS);
        ui_reset_progress();
	finish_recovery(NULL);
    
	allow_display_toggle = 1;
	int chosen_item = get_menu_selection(headers, MENU_ITEMS, 0, 0, safemode, 0);
        allow_display_toggle = 0;

        // device-specific code may take some action here.  It may
        // return one of the core actions handled in the switch
        // statement below.
        chosen_item = device_perform_action(chosen_item);

        switch (chosen_item) {
            
	    case ITEM_SAFEBOOT:
                show_safe_boot_menu();
                safemode = get_safe_mode();
                break;
	    
	    case ITEM_REBOOT_MENU:
	    {
                poweroff = show_reboot_menu();
		if (poweroff == REBOOT_NOW || poweroff == SHUTDOWN_NOW)
		    return;
		else
		    break;
	    } 

            case ITEM_WIPE_MENU:
                show_wipe_menu();
                break;

            case ITEM_NANDROID:
                show_nandroid_menu();
                safemode = get_safe_mode();
                break;
	    
	    case ITEM_PARTITION:
                show_partition_menu();
                safemode = get_safe_mode();
                break;
	    
	    case ITEM_ADVANCED:
                safemode = get_safe_mode();
		show_advanced_menu(safemode);
                safemode = get_safe_mode();
                break;

            case ITEM_CONSOLE:
		show_console_menu();
                safemode = get_safe_mode();
            	break;

	    case ITEM_INSTALL_ZIP:
                show_install_update_menu();
                safemode = get_safe_mode();
                break;
        }
    }
}

void print_property(const char *key, const char *name, void *cookie) {
    printf("%s=%s\n", key, name);
}

int
main(int argc, char **argv) {
	if (strstr(argv[0], "recovery") == NULL)
	{
	    if (strstr(argv[0], "flash_image") != NULL)
	        return flash_image_main(argc, argv);
	    if (strstr(argv[0], "volume") != NULL)
	        return volume_main(argc, argv);
	    if (strstr(argv[0], "edify") != NULL)
	        return edify_main(argc, argv);
	    if (strstr(argv[0], "dump_image") != NULL)
	        return dump_image_main(argc, argv);
	    if (strstr(argv[0], "erase_image") != NULL)
	        return erase_image_main(argc, argv);
	    if (strstr(argv[0], "mkyaffs2image") != NULL)
	        return mkyaffs2image_main(argc, argv);
	    if (strstr(argv[0], "unyaffs") != NULL)
	        return unyaffs_main(argc, argv);
        if (strstr(argv[0], "nandroid"))
            return nandroid_main(argc, argv);
        if (strstr(argv[0], "reboot"))
            return reboot_main(argc, argv);
#ifdef BOARD_RECOVERY_HANDLES_MOUNT
        if (strstr(argv[0], "mount") && argc == 2 && !strstr(argv[0], "umount"))
        {
            load_volume_table();
            return ensure_path_mounted(argv[1]);
        }
#endif
        if (strstr(argv[0], "poweroff")){
            return reboot_main(argc, argv);
        }
        if (strstr(argv[0], "setprop"))
            return setprop_main(argc, argv);
		return busybox_driver(argc, argv);
	}
    __system("/sbin/postrecoveryboot.sh");

    int is_user_initiated_recovery = 0;
    time_t start = time(NULL);

    // If these fail, there's not really anywhere to complain...
    freopen(TEMPORARY_LOG_FILE, "a", stdout); setbuf(stdout, NULL);
    freopen(TEMPORARY_LOG_FILE, "a", stderr); setbuf(stderr, NULL);
    printf("starting recovery on %s", ctime(&start));

    ui_init();
    //ui_print(EXPAND(RECOVERY_VERSION)"\n");
    load_volume_table();
    process_volumes();
    LOGI("processing arguments.\n");
    get_args(&argc, &argv);

    int previous_runs = 0;
    const char *send_intent = NULL;
    const char *update_package = NULL;
    const char *encrypted_fs_mode = NULL;
    int wipe_data = 0, wipe_cache = 0;
    int toggle_secure_fs = 0;

    LOGI("checking arguments.\n");
    int arg;
    while ((arg = getopt_long(argc, argv, "", OPTIONS, NULL)) != -1) {
        switch (arg) {
        case 'p': previous_runs = atoi(optarg); break;
        case 's': send_intent = optarg; break;
        case 'u': update_package = optarg; break;
        case 'w': 
#ifndef BOARD_RECOVERY_ALWAYS_WIPES
		wipe_data = wipe_cache = 1;
#endif
		break;
        case 'c': wipe_cache = 1; break;
        case 'e': encrypted_fs_mode = optarg; toggle_secure_fs = 1; break;
        case 't': ui_show_text(1); break;
        case '?':
            LOGE("invalid command argument\n");
            continue;
        }
    }

    LOGI("device_recovery_start()\n");
    device_recovery_start();

    printf("command:");
    for (arg = 0; arg < argc; arg++) {
        printf(" \"%s\"", argv[arg]);
    }
    printf("\n");

    if (update_package) {
        // For backwards compatibility on the cache partition only, if
        // we're given an old 'root' path "CACHE:foo", change it to
        // "/cache/foo".
        if (strncmp(update_package, "CACHE:", 6) == 0) {
            int len = strlen(update_package) + 10;
            char* modified_path = malloc(len);
            strlcpy(modified_path, "/cache/", len);
            strlcat(modified_path, update_package+6, len);
            printf("(replacing path \"%s\" with \"%s\")\n",
                   update_package, modified_path);
            update_package = modified_path;
        }
    }
    printf("\n");

    property_list(print_property, NULL);
    printf("\n");

    int status = INSTALL_SUCCESS;
    
    if (update_package != NULL) {
        status = install_package(update_package);
        if (status != INSTALL_SUCCESS) ui_print("installation aborted.\n");
    } else if (wipe_data) {
        if (device_wipe_data()) status = INSTALL_ERROR;
        if (erase_volume("/data")) status = INSTALL_ERROR;
        if (wipe_cache && erase_volume("/cache")) status = INSTALL_ERROR;
        if (status != INSTALL_SUCCESS) ui_print("data wipe failed.\n");
    } else if (wipe_cache) {
        if (wipe_cache && erase_volume("/cache")) status = INSTALL_ERROR;
        if (status != INSTALL_SUCCESS) ui_print("cache wipe failed.\n");
    } else {
        LOGI("checking for extendedcommand...\n");
        status = INSTALL_ERROR;  // No command specified
        // we are starting up in user initiated recovery here
        // let's set up some default options
        signature_check_enabled = 0;
        script_assert_enabled = 0;
        is_user_initiated_recovery = 1;
        ui_set_show_text(1);
        ui_set_background(BACKGROUND_ICON_CLOCKWORK);
        
        if (extendedcommand_file_exists()) {
            LOGI("running extendedcommand...\n");
            int ret;
            if (0 == (ret = run_and_remove_extendedcommand())) {
                status = INSTALL_SUCCESS;
                ui_set_show_text(0);
            }
            else {
                handle_failure(ret);
            }
        } else {
            LOGI("skipping execution of extendedcommand, file not found...\n");
        }
    }

    if (status != INSTALL_SUCCESS && !is_user_initiated_recovery) ui_set_background(BACKGROUND_ICON_ERROR);
    if (status != INSTALL_SUCCESS || ui_text_visible()) {
        prompt_and_wait();
    }

    // If there is a radio image pending, reboot now to install it.
    maybe_install_firmware_update(send_intent);

    // Otherwise, get ready to boot the main system...
    finish_recovery(send_intent);
    if(poweroff == REBOOT_NOW)
        ui_print("rebooting...\n");
    else if (poweroff == SHUTDOWN_NOW)
    {
    	ui_print("shutting down...\n");
    }
    sync();
    reboot((poweroff==REBOOT_NOW) ? RB_AUTOBOOT : RB_POWER_OFF);
    return EXIT_SUCCESS;
}

int get_allow_toggle_display() {
    return allow_display_toggle;
}
