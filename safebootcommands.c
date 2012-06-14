#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reboot.h>
//#include <reboot/reboot.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <sys/stat.h>
#include <sys/vfs.h>

#include <signal.h>
#include <sys/wait.h>

#include "common.h"
#include "cutils/properties.h"
#include "recovery_ui.h"

#include "extendedcommands.h"
#include "nandroid.h"
#include "mounts.h"
#include "roots.h"
#include "flashutils/flashutils.h"
#include "edify/expr.h"
#include <libgen.h>
#include "mtdutils/mtdutils.h"

#include "safebootcommands.h"

#define MENU_HEADER_COLS 55

#define BATTERY_CAPACITY_FILE "/sys/class/power_supply/battery/charge_counter"

int safemode = 0;

int check_systemorig_mount() {
    int result = 0;
    result = scan_mounted_volumes();
    if (result < 0) {
	LOGE("failed to scan mounted volumes\n");
	return 0;
    }
    const MountedVolume* mv = find_mounted_volume_by_mount_point("/systemorig");
    if (mv == NULL) {
	__system("mount /dev/block/systemorig /systemorig");
	result = scan_mounted_volumes();
	if (result < 0) {
	    LOGE("failed to scan mounted volumes\n");
	    return 0;
	}
	const MountedVolume* mv = find_mounted_volume_by_mount_point("/systemorig");
	if (mv == NULL) {
	    LOGE ("can't mount primary system.\n");
	    return 0;
	}
    }
    return 1;
}

int get_safe_mode() {
    int result = 0;
    char cmd[256];
    if (check_systemorig_mount()) {
       struct statfs info;
       struct statfs info2;
       if (0 == statfs(SAFE_SYSTEM_FILE, &info)){
	   result = 1;
           if (statfs(DUPE_SAFE_SYSTEM_FILE, &info2)){
             sprintf(cmd, "touch %s", DUPE_SAFE_SYSTEM_FILE);
	   }
       }
       else {
           if (0 == statfs(DUPE_SAFE_SYSTEM_FILE, &info2)){
           sprintf(cmd, "rm %s", DUPE_SAFE_SYSTEM_FILE);
	   }
       }
    }
    __system(cmd);
    return result;
}

void show_safe_boot_menu() {
    char tmp[PATH_MAX];
    char** headers = NULL;
    char* headers_before[] = {  "||  safe-boot menu  |/____________________________,/|",
			        "||-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+|/|",
				 NULL
    };
    headers = prepend_title((const char**)headers_before);
    static char* list[] = { "|| <1> toggle safe system                         |/|",
			    "|| <2> quick toggle safe system (DANGEROUS)       |/|",
			    NULL
    };

    int chosen_item = get_menu_selection(headers, list, 0, 0, 0, 0);
    
    if (chosen_item == GO_BACK)
        return;
    
    switch (chosen_item)
    {
        case 0:
        {
	    const char* confirm_toggle[] = {   
	    				        "|+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+|",
					        "|         | c o n f i r m   t o g g l e   ? |       |",
					        "|+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+|",
					        NULL
				           };

	    char confirm_tog[MENU_HEADER_COLS];
	    sprintf(confirm_tog,  "|| <8> yes - %s safe system                  |/|", !safemode ? " enable" : "disable");
 	    if (confirm_selection(confirm_toggle, confirm_tog))
	    {
            	toggle_safe_mode();
		ui_set_showing_warning(0);
		break;
	    }
	    else
	    {
            	ui_set_showing_warning(0);
		break;
	    }
        }
        case 1:
        {
	    const char* confirm_qtoggle[] = {   
	              				"|+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+|",
					        "|   | c o n f i r m   q u i c k  t o g g l e  ? |   |",
					        "|+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+|",
					        NULL
					    };

	    char confirm_qtog[MENU_HEADER_COLS];
	    sprintf(confirm_qtog, "|| <8> yes - %s safe system (DANGEROUS)      |/|", !safemode ? " enable" : "disable");
	    if (confirm_selection(confirm_qtoggle, confirm_qtog))
	    {
            	quick_toggle_safe_mode();
		ui_set_showing_warning(0);
		break;
	    }
	    else
	    {
            	ui_set_showing_warning(0);
	    }
        }
    }

}

void quick_toggle_safe_mode() {
    char cmd[256];
    safemode = get_safe_mode();
    if (!safemode) {
        /* 4. touch SAFE_SYSTEM_FILE */
        sprintf(cmd, "touch %s", SAFE_SYSTEM_FILE);
        ui_print("\n%s\n", cmd);
        __system(cmd);
    } else {
        /* 4. rm SAFE_SYSTEM_FILE */
        sprintf(cmd, "rm %s", SAFE_SYSTEM_FILE);
        ui_print("\n%s\n", cmd);
        __system(cmd);
    }
    safemode = get_safe_mode();
    ui_print("safe system is now: %s!\n", safemode ? "ENABLED" : "DISABLED");
}

void toggle_safe_mode() {
    struct statfs info;
    char cmd[256];

    safemode = get_safe_mode();
    char orig_backup_path[PATH_MAX];
    sprintf(orig_backup_path, "/emmc/%s/orig", EXPAND(RECOVERY_FOLDER));
    char safe_backup_path[PATH_MAX];
    sprintf(safe_backup_path, "/emmc/%s/safe", EXPAND(RECOVERY_FOLDER));

    if (ensure_path_mounted("/emmc") != 0) {
        ui_print("can't mount /emmc\n");
        return;
    }
   
    int ret;
    if (0 != (ret = statfs("/emmc", &info))) {
        ui_print("unable to stat /emmc\n");
        return;
    }
    uint64_t bavail = info.f_bavail;
    uint64_t bsize = info.f_bsize;
    uint64_t sdcard_free = bavail * bsize;
    uint64_t sdcard_free_mb = sdcard_free / (uint64_t)(1024 * 1024);
    ui_print("emmc space free: %lluMB\n", sdcard_free_mb);
    if (sdcard_free_mb < 1024)
        ui_print("there may not be enough free space to complete backup... continuing...\n");

    //ui_set_background(BACKGROUND_ICON_INSTALLING);
    //ui_show_indeterminate_progress();
    if (!safemode) {
   
        sprintf(cmd, "mkdir -p %s", orig_backup_path);
        __system(cmd);
	
	ui_show_progress(0.22,46);	

        /* 1. make a backup of the existing /data + /cache in /emmc/safestrap/orig/ */
        ui_print("\n-- backing up original data...\n");

        sprintf(cmd, "rm %s/*", orig_backup_path);
        __system(cmd);
        //if (0 != (ret = nandroid_backup_partition(orig_backup_path, "/system"))) return;
        //ui_set_progress(0.15);
        if (0 != (ret = nandroid_backup_partition(orig_backup_path, "/data"))) return;
        //if (0 != (ret = nandroid_backup_partition(orig_backup_path, "/cache"))) return;
        //ui_set_progress(0.45);

        ui_print("\n-- restoring safe system data...\n");
	ui_show_progress(0.75, 154);

        //if (0 != (ret = nandroid_restore_partition(safe_backup_path, "/system"))) return;
        //ui_set_progress(0.60);
        if (0 != (ret = nandroid_restore_partition(safe_backup_path, "/data"))) return;
        //ui_set_progress(0.80);
        //if (0 != (ret = nandroid_restore_partition(safe_backup_path, "/cache"))) return;
        //ui_set_progress(0.90);
	
	ui_show_progress(0.03, 5);

        /* 3. wipe Dalvik Cache */
        __system("rm -r /data/dalvik-cache");
        __system("rm -r /cache/dalvik-cache");
        //__system("rm -r /sd-ext/dalvik-cache");
        //ui_set_progress(0.95);

        /* 4. touch SAFE_SYSTEM_FILE */
        sprintf(cmd, "touch %s", SAFE_SYSTEM_FILE);
        ui_print("\n%s\n", cmd);
        __system(cmd);

	ui_print("swap to safe system complete.\n");

    } else {

        sprintf(cmd, "mkdir -p %s", safe_backup_path);
        __system(cmd);

	ui_show_progress(0.22,46);	

	/* 1. make a backup of the existing /data + /cache in /emmc/safestrap/safe/ */
        ui_print("\n-- backing up safe system...\n");

        sprintf(cmd, "rm %s/*", safe_backup_path);
        __system(cmd);
        //if (0 != (ret = nandroid_backup_partition(safe_backup_path, "/system"))) return;
        //ui_set_progress(0.15);
        if (0 != (ret = nandroid_backup_partition(safe_backup_path, "/data"))) return;
        //if (0 != (ret = nandroid_backup_partition(safe_backup_path, "/cache"))) return;
        //ui_set_progress(0.45);

        ui_print("\n-- restoring original system...\n");
	ui_show_progress(0.75, 154);

        //if (0 != (ret = nandroid_restore_partition(orig_backup_path, "/system"))) return;
        //ui_set_progress(0.60);
        if (0 != (ret = nandroid_restore_partition(orig_backup_path, "/data"))) return;
	
	ui_show_progress(0.03, 5);
        
	//if (0 != (ret = nandroid_restore_partition(orig_backup_path, "/cache"))) return;
        //ui_set_progress(0.90);

        /* 3. wipe Dalvik Cache */
        __system("rm -r /data/dalvik-cache");
        __system("rm -r /cache/dalvik-cache");
        //__system("rm -r /sd-ext/dalvik-cache");

        /* 4. rm SAFE_SYSTEM_FILE */
        sprintf(cmd, "rm %s", SAFE_SYSTEM_FILE);
        ui_print("\n%s\n", cmd);
        __system(cmd);

	ui_print("swap to original system complete.\n");
    }
    sync();
    ui_set_background(BACKGROUND_ICON_CLOCKWORK);

    safemode = get_safe_mode();
    ui_print("safe system is now: %s \n", safemode ? "ENABLED" : "DISABLED");
    ui_reset_progress();
}

