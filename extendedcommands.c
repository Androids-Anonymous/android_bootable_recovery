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
#include <reboot/reboot.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <sys/wait.h>
#include <sys/limits.h>
#include <dirent.h>
#include <sys/stat.h>

#include <signal.h>
#include <sys/wait.h>

#include "bootloader.h"
#include "common.h"
#include "cutils/properties.h"
#include "firmware.h"
#include "install.h"
#include "minui/minui.h"
#include "minzip/DirUtil.h"
#include "roots.h"
#include "recovery_ui.h"

#include "../../external/yaffs2/yaffs2/utils/mkyaffs2image.h"
#include "../../external/yaffs2/yaffs2/utils/unyaffs.h"

#include "extendedcommands.h"
#include "nandroid.h"
#include "mounts.h"
#include "flashutils/flashutils.h"
#include "edify/expr.h"
#include <libgen.h>
#include "mtdutils/mtdutils.h"
#include "safebootcommands.h"

#define MENU_HEADER_ROWS 32
#define MENU_HEADER_COLS 55

int signature_check_enabled = 1;
int script_assert_enabled = 1;
static const char *SDCARD_UPDATE_FILE = "/sdcard/update.zip";

void
toggle_signature_check()
{
    signature_check_enabled = !signature_check_enabled;
    ui_print("signature check: %s\n", signature_check_enabled ? "ENABLED" : "DISABLED");
}

void toggle_script_asserts()
{
    script_assert_enabled = !script_assert_enabled;
    ui_print("script asserts: %s\n", script_assert_enabled ? "ENABLED" : "DISABLED");
}

int install_zip(const char* packagefilepath)
{
    ui_print("\n-- installing: %s\n", packagefilepath);
    if (device_flash_type() == MTD) {
        set_sdcard_update_bootloader_message();
    }
    int status = install_package(packagefilepath);
    ui_reset_progress();
    if (status != INSTALL_SUCCESS) {
        ui_set_background(BACKGROUND_ICON_ERROR);
        ui_print("installation aborted.\n");
        return 1;
    }
    ui_set_background(BACKGROUND_ICON_NONE);
    ui_print("\ninstall from SD card complete.\n");
    return 0;
}

static char**
prepend_title(const char** headers) {
    char tmp1[PATH_MAX];
    char tmp2[PATH_MAX];
    sprintf(tmp1, "|                         %s |", EXPAND(RECOVERY_VERSION));
    sprintf(tmp2, "|                         safe system is: %s", safemode ? "  ENABLED |" : " DISABLED |");
    char* title[] = { ".+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+.", 
		      "",
                      "",
		      "|,==================/\\______________________________|", 
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

int show_sdcard_selection_menu()
{
    char** headers = NULL;
    char* headers_iore[] = { "||  choose SD card  |/____________________________,/|",
			     "|+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+|/|",
			     NULL
			   };
    headers = prepend_title((const char**)headers_iore);
    char* list[] = { "| <1> internal SD card                            |/|",
                     "| <2> external SD card                            |/|",
	             NULL
                   };
    int chosen_item = get_menu_selection(headers, list, 0, 0, 0, 0);
    return (chosen_item < 2) ? chosen_item : -1;
}

char** INSTALL_MENU_ITEMS[] = {  "| <1> choose .zip from SD card                    |/|",
                                 "| <2> apply update.zip from root of SD card       |/|",
                                 "| <3> toggle signature verification               |/|",
                                 "| <4> toggle script asserts                       |/|",
                                 "| <5> choose .zip from internal sdcard            |/|",
			         NULL
			      };

#define ITEM_CHOOSE_ZIP       0
#define ITEM_APPLY_SDCARD     1
#define ITEM_SIG_CHECK        2
#define ITEM_ASSERTS          3
#define ITEM_CHOOSE_ZIP_INT   4

void show_install_update_menu()
{
#ifdef BOARD_HAS_SDCARD_INTERNAL
    char sdcard_package_file[PATH_MAX];
    char confirmsd[PATH_MAX];
    int chosen_sdcard = -1;
#endif
    char** headers = NULL;
    char* headers_up[] = {  "||   update  menu   |/____________________________,/|",
			    "|+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+|/|",
			     NULL
                               };
    headers = prepend_title((const char**)headers_up);    

    INSTALL_MENU_ITEMS[ITEM_CHOOSE_ZIP_INT] = NULL;

    for (;;)
    {
        int chosen_item = get_menu_selection(headers, INSTALL_MENU_ITEMS, 0, 0, 0, 0);
	switch (chosen_item)
        {
            case ITEM_ASSERTS:
                toggle_script_asserts();
                break;
            case ITEM_SIG_CHECK:
                toggle_signature_check();
                break;
            case ITEM_APPLY_SDCARD:
            {
#ifdef BOARD_HAS_SDCARD_INTERNAL
                chosen_sdcard = show_sdcard_selection_menu();
                if (chosen_sdcard > -1)
                {
                    switch (chosen_sdcard) {
                        case 0:
                            sprintf(sdcard_package_file, "/emmc/update.zip                  |");
                            break;
                        case 1:
                            sprintf(sdcard_package_file, "/sdcard/update.zip                |");
                            break;
                    }
                    sprintf(confirmsd, "| <8> yes - install %s", sdcard_package_file);
                }
                else break;
                const char* confirm_install[] = {   
						    "|+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+|",
						    "|       | c o n f i r m   i n s t a l l   ? |       |",
						    "|+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+|",
		      		NULL
						       };

		if (confirm_selection(confirm_install, confirmsd))
                    install_zip(sdcard_package_file);
#else
		char confirm_update[MENU_HEADER_COLS] = "| <8> yes - install update                        |/|";

		if (confirm_selection(confirm_install, confirm_update))
                    install_zip(SDCARD_UPDATE_FILE);
#endif
                break;
            }
            case ITEM_CHOOSE_ZIP:
            {
#ifdef BOARD_HAS_SDCARD_INTERNAL
                chosen_sdcard = show_sdcard_selection_menu();
                if (chosen_sdcard > -1)
                {
                    switch (chosen_sdcard) {
                        case 0:
                            sprintf(sdcard_package_file, "/emmc/");
                            break;
                        case 1:
                            sprintf(sdcard_package_file, "/sdcard/");
                            break;
                    }
                }
                else break;
                show_choose_zip_menu(sdcard_package_file);
#else
                show_choose_zip_menu("/sdcard/");
#endif
                break;
            }
            case ITEM_CHOOSE_ZIP_INT:
                show_choose_zip_menu("/emmc/");
                break;
            default:
                return;
        }

    }
}

void free_string_array(char** array)
{
    if (array == NULL)
        return;
    char* cursor = array[0];
    int i = 0;
    while (cursor != NULL)
    {
        free(cursor);
        cursor = array[++i];
    }
    free(array);
}

char** gather_files(const char* directory, const char* fileExtensionOrDirectory, int* numFiles)
{
    char path[PATH_MAX] = "";
    DIR *dir;
    struct dirent *de;
    int total = 0;
    int i;
    char** files = NULL;
    int pass;
    *numFiles = 0;
    int dirLen = strlen(directory);

    dir = opendir(directory);
    if (dir == NULL) {
        ui_print("couldn't open directory.\n");
        return NULL;
    }

    int extension_length = 0;
    if (fileExtensionOrDirectory != NULL)
        extension_length = strlen(fileExtensionOrDirectory);

    int isCounting = 1;
    i = 0;
    for (pass = 0; pass < 2; pass++) {
        while ((de=readdir(dir)) != NULL) {
            // skip hidden files
            if (de->d_name[0] == '.')
                continue;

            // NULL means that we are gathering directories, so skip this
            if (fileExtensionOrDirectory != NULL)
            {
                // make sure that we can have the desired extension (prevent seg fault)
                if (strlen(de->d_name) < extension_length)
                    continue;
                // compare the extension
                if (strcmp(de->d_name + strlen(de->d_name) - extension_length, fileExtensionOrDirectory) != 0)
                    continue;
            }
            else
            {
                struct stat info;
                char fullFileName[PATH_MAX];
                strcpy(fullFileName, directory);
                strcat(fullFileName, de->d_name);
                stat(fullFileName, &info);
                // make sure it is a directory
                if (!(S_ISDIR(info.st_mode)))
                    continue;
            }

            if (pass == 0)
            {
                total++;
                continue;
            }

            files[i] = (char*) malloc(dirLen + strlen(de->d_name) + 2);
            strcpy(files[i], directory);
            strcat(files[i], de->d_name);
            if (fileExtensionOrDirectory == NULL)
                strcat(files[i], "/");
            i++;
        }
        if (pass == 1)
            break;
        if (total == 0)
            break;
        rewinddir(dir);
        *numFiles = total;
        files = (char**) malloc((total+1)*sizeof(char*));
        files[total]=NULL;
    }

    if(closedir(dir) < 0) {
        LOGE("failed to close directory.");
    }

    if (total==0) {
        return NULL;
    }

    // sort the result
    if (files != NULL) {
        for (i = 0; i < total; i++) {
            int curMax = -1;
            int j;
            for (j = 0; j < total - i; j++) {
                if (curMax == -1 || strcmp(files[curMax], files[j]) < 0)
                    curMax = j;
            }
            char* temp = files[curMax];
            files[curMax] = files[total - i - 1];
            files[total - i - 1] = temp;
        }
    }

    return files;
}

// pass in NULL for fileExtensionOrDirectory and you will get a directory chooser
char* choose_file_menu(const char* directory, const char* fileExtensionOrDirectory, char** headers[])
{
    char path[PATH_MAX] = "";
    DIR *dir;
    struct dirent *de;
    int numFiles = 0;
    int numDirs = 0;
    int i;
    char* return_value = NULL;
    int dir_len = strlen(directory);

    char** files = gather_files(directory, fileExtensionOrDirectory, &numFiles);
    char** dirs = NULL;
    if (fileExtensionOrDirectory != NULL)
        dirs = gather_files(directory, NULL, &numDirs);
    int total = numDirs + numFiles;

    if (total == 0)
    {
        ui_print("no files found.\n");
    }
    else
    {
        char** list = (char**) malloc((total + 1) * sizeof(char*));
	char** list_copy = (char**) malloc((total + 1) * sizeof(char*));
	list[total] = NULL;
	list_copy[total] = NULL;
	const char *template = "|                                                 |/|";

	for (i = 0; i < numDirs; i++)
	{
	    list[i] = strdup(template);
	    char *t_c2 = list[i] + 2;
	    char *dirp = strdup(dirs[i] + dir_len);
	    strncpy(t_c2, dirp, strlen(dirp));
	}
	for (i = 0 ; i < numFiles; i++)
	{
	    list[numDirs + i] = strdup(template);
	    char *t_c2 = list[numDirs + i] + 2;
	    char *dirp = strdup(files[i] + dir_len);
	    strncpy(t_c2, dirp, strlen(dirp));
	}
/*
        for (i = 0 ; i < numDirs; i++)
        {
            list[i] = strdup(dirs[i] + dir_len);
        }

        for (i = 0 ; i < numFiles; i++)
        {
            list[numDirs + i] = strdup(files[i] + dir_len);
        }
*/
        for (;;)
        {
            int chosen_item = get_menu_selection(headers, list, 0, 0, 0, 0);
            if (chosen_item == GO_BACK)
                break;
            static char ret[PATH_MAX];
            if (chosen_item < numDirs)
            {
                char* subret = choose_file_menu(dirs[chosen_item], fileExtensionOrDirectory, headers);
                if (subret != NULL)
                {
                    strcpy(ret, subret);
                    return_value = ret;
                    break;
                }
                continue;
            }
            strcpy(ret, files[chosen_item - numDirs]);
            return_value = ret;
            break;
        }
	free_string_array(list);
    }

    free_string_array(files);
    free_string_array(dirs);
    return return_value;
}

void show_choose_zip_menu(const char *mount_point)
{
    if (ensure_path_mounted(mount_point) != 0) {
        LOGE ("can't mount %s\n", mount_point);
        return;
    }
    char** headers = NULL;
    char** headers_zip[] = {  "|| choose .zip file |/____________________________,/|",
			  "|+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+|/|",
			   NULL
    };
    headers = prepend_title((const char**)headers_zip);
    char* file = choose_file_menu(mount_point, ".zip", headers);
    if (file == NULL)
        return;
    const char* confirm_install[]={        
					   "|+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+|",
					   "|       | c o n f i r m   i n s t a l l   ? |       |",
					   "|+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+|",
		      			   NULL
				  };
    char* confirmpath[PATH_MAX];
    sprintf(confirmpath, "|8] yes - install %s", basename(file));
    if (confirm_selection(confirm_install, confirmpath))
        install_zip(file);
}

void show_nandroid_restore_menu(const char* path)
{
    if (ensure_path_mounted(path) != 0) {
        LOGE("can't mount %s\n", path);
        return;
    }

    int restore_webtop = 1;
    char** headers = NULL;
    char** headers_nanr[] = {  "|| choose the image |/____________________________,/|",
			  "|+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+|/|",
			  NULL
    		       };
    headers = prepend_title((const char**)headers_nanr);
    char tmp[PATH_MAX];
    sprintf(tmp, "%s/%s/backup/", path, EXPAND(RECOVERY_FOLDER));
    char* file = choose_file_menu(tmp, NULL, headers);
    if (file == NULL)
        return;

#ifdef BOARD_HAS_WEBTOP
    static char** header[] = { "|| include  webtop? |/____________________________,/|",
			       "|+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+|/|",
			        NULL
    };

    static char** list[] = { "| <1> yes - include webtop                        |/|",
                             "| <2> no                                          |/|",
			     NULL

    };

    switch (get_menu_selection(header, list, 0, 0, 0, 0))
    {
        case 0:
	    restore_webtop = 1;
            break;
        case 1:
	    restore_webtop = 0;
            break;
        default:
            return;
    }
#endif
    const char* confirm_restore[] = {  
				       "|+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+|",
				       "|       | c o n f i r m   r e s t o r e   ? |       |",
				       "|+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+|",
		      		       NULL

				    };
    
    char confirm_re2[MENU_HEADER_COLS] = "| <8> yes - restore                               |/|";

	if (confirm_selection(confirm_restore, confirm_re2))
        nandroid_restore(file, 1, 1, 1, (safemode) ? 0 : 1);
}

void show_nandroid_verify_menu(const char* volume)
{
    if (ensure_path_mounted(volume) != 0) {
        LOGE ("can't mount %s\n", volume);
        return;
    }
    char** headers = NULL;
    char** headers_ver[] = { "||   choose image   |/____________________________,/|",
			 "|+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+|/|",
			  NULL

    };
    headers = prepend_title((const char**)headers_ver);
    char backup_path[PATH_MAX];
    sprintf(backup_path, "%s/%s/backup/", volume, EXPAND(RECOVERY_FOLDER));
    char* file = choose_file_menu(backup_path, NULL, headers);
    if (file == NULL)
        return;

    const char* confirm_verify[] = {  
				       "|+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+|",
				       "|       | c o n f i r m     v e r i f y   ? |       |",
				       "|+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+|",
		      		       NULL
					  };
     
    char confirm_ver[MENU_HEADER_COLS] = "| <8> yes - verify image                          |/|";
    
    if (confirm_selection(confirm_verify, confirm_ver))
     {
        char tmp[PATH_MAX];
        ui_set_background(BACKGROUND_ICON_INSTALLING);
        ui_show_indeterminate_progress();

        ui_print("checking MD5 sums...\n");
        sprintf(tmp, "cd %s && md5sum -c nandroid.md5", file);
        if (0 != __system(tmp)) {
            ui_print("MD5 sum mismatch.\n");
            return;
        }

        ui_set_background(BACKGROUND_ICON_NONE);
        ui_reset_progress();
        ui_print("\nverify complete.\n");
    }
}

#ifndef BOARD_UMS_LUNFILE
#ifdef BOARD_HAS_SDCARD_INTERNAL
#define BOARD_UMS_LUNFILE "/sys/devices/platform/usb_mass_storage/lun1/file"
#else
#define BOARD_UMS_LUNFILE "/sys/devices/platform/usb_mass_storage/lun0/file"
#endif
#endif

void show_mount_usb_storage_menu()
{
    int fd, fd2;
    Volume *vol = volume_for_path("/sdcard");
    if ((fd = open(BOARD_UMS_LUNFILE, O_WRONLY)) < 0) {
        LOGE("unable to open ums lunfile (%s)", strerror(errno));
        return -1;
    }

    if ((write(fd, vol->device, strlen(vol->device)) < 0) &&
        (!vol->device2 || (write(fd, vol->device, strlen(vol->device2)) < 0))) {
        LOGE("unable to write to ums lunfile (%s)", strerror(errno));
        close(fd);
        return -1;
    }
#ifdef BOARD_HAS_SDCARD_INTERNAL
    vol = volume_for_path("/emmc");
    if ((fd2 = open("/sys/devices/platform/usb_mass_storage/lun0/file", O_WRONLY)) < 0) {
        LOGE("unable to open ums lunfile for internal SD card (%s)", strerror(errno));
    }
    else if ((write(fd2, vol->device, strlen(vol->device)) < 0) &&
        (!vol->device2 || (write(fd2, vol->device, strlen(vol->device2)) < 0))) {
        LOGE("unable to write to ums lunfile for internal SD card (%s)", strerror(errno));
        close(fd2);
    }
#endif
    char** headers = NULL;
    char* headers_usb[] = {   "|| USB mass storage |/____________________________,/|",
			      "|+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+|/|",
			      "| leaving this menu will unmount all SD cards     |/|",
                              "| from your PC.                                   |/|",
			      NULL
    };
    headers = prepend_title((const char**)headers_usb);
    char* list[] = {
		     NULL		
                   };

    for (;;)
    {
        int chosen_item = get_menu_selection(headers, list, 0, 0, 0, 0);
        if (chosen_item == GO_BACK || chosen_item == 0)
            break;
    }

    if ((fd = open(BOARD_UMS_LUNFILE, O_WRONLY)) < 0) {
        LOGE("unable to open ums lunfile (%s)", strerror(errno));
        return -1;
    }

    char ch = 0;
    if (write(fd, &ch, 1) < 0) {
        LOGE("unable to write to ums lunfile (%s)", strerror(errno));
        close(fd);
        return -1;
    }
#ifdef BOARD_HAS_SDCARD_INTERNAL
    if ((fd2 = open("/sys/devices/platform/usb_mass_storage/lun0/file", O_WRONLY)) < 0) {
        LOGE("unable to open ums lunfile for internal SD card (%s)", strerror(errno));
    }
    else if (write(fd2, &ch, 1) < 0) {
	    LOGE("unable to write to ums lunfile for internal SD card (%s)", strerror(errno));
        close(fd2);
    }
#endif
}

int confirm_selection(const char** title, char* confirm)
{
	struct stat info;
    if (0 == stat("/sdcard/safestrap/.no_confirm", &info))
	    return 1;
#ifdef BOARD_HAS_SDCARD_INTERNAL
    if (0 == stat("/emmc/safestrap/.no_confirm", &info))
        return 1;
#endif
    char title_l0[MENU_HEADER_COLS];
    strcpy(title_l0,title[0]);

    char title_l1[MENU_HEADER_COLS];
    strcpy(title_l1,title[1]);

    char title_l2[MENU_HEADER_COLS];
    strcpy(title_l2,title[2]);

//    char title_l5[MENU_HEADER_COLS];
//    strcpy(title_l5,title[5]);

    char* confirm_headers[]  = {   		   ".+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+.", //0
						   "|                                __                 |", //1
						   "|  __  _  _______ _______  ____ |__| ____    ____   |", 
						   "|  \\ \\/ \\/ /\\__  \\\\_  __ \\/    \\|  |/    \\  / ___\\  |",
						   "|   \\     /  / __ \\|  | \\/   |  \\  |   |  \\/ /_/  > |",
						   "|    \\/\\_/  (____  /__|  |___|  /__|___|  /\\___  /  |", //5
						   "|                \\/           \\/        \\//_____/   |",
						   "|       ___ (^')(^')             (^`)(^`) ___       |",
						   "|      /   \\_\\ \\/ /               \\ \\/ /_/   \\      |",
						   "|     |   x_ ]\\ \\/                 \\/ /[ _x   |     |",
						   "|     |   x _]/\\ \\                 / /\\[_ x   |     |", //10
						   "|      \\___/ / /\\ \\               / /\\ \\ \\___/      |",
						   "|           (_.)(._)             (_.)(._)           |", //12
						   "|                                                   |", //13
						   "",
						   "",
						   "",   //16
						   NULL};
    
    confirm_headers[14] = strdup(title_l0);
    confirm_headers[15] = strdup(title_l1);
    confirm_headers[16] = strdup(title_l2);
 
    char* items[] = { "| <1> no                                          |/|",
                      "| <2> no                                          |/|",
                      "| <3> no                                          |/|",
                      "| <4> no                                          |/|",
                      "| <5> no                                          |/|",
                      "| <6> no                                          |/|",
                      "| <7> no                                          |/|",
                      confirm, 			 //" Yes -- xxxxxxxxxx",   //
                      "| <9> no                                          |/|",
                      "| <0> no                                          |/|",
                      "| <A> no                                          |/|",
		      NULL
                    };

    int chosen_item = get_menu_selection(confirm_headers, items, 0, 0, 0, 0);
    return chosen_item == 7;
}

#define MKE2FS_BIN      "/sbin/mke2fs"
#define TUNE2FS_BIN     "/sbin/tune2fs"
#define E2FSCK_BIN      "/sbin/e2fsck"

int format_device(const char *device, const char *path, const char *fs_type) {
    Volume* v = volume_for_path(path);
    if (v == NULL) {
        // no /sdcard? let's assume /data/media
        if (strstr(path, "/sdcard") == path && is_data_media()) {
            return format_unknown_device(NULL, path, NULL);
        }
        // silent failure for sd-ext
        if (strcmp(path, "/sd-ext") == 0)
            return -1;
        LOGE("unknown volume \"%s\"\n", path);
        return -1;
    }
    if (strcmp(fs_type, "ramdisk") == 0) {
        // you can't format the ramdisk.
        LOGE("can't format_volume \"%s\"", path);
        return -1;
    }

    if (strcmp(v->mount_point, path) != 0) {
        return format_unknown_device(v->device, path, NULL);
    }

    if (ensure_path_unmounted(path) != 0) {
        LOGE("format_volume failed to unmount \"%s\"\n", v->mount_point);
        return -1;
    }

    if (strcmp(fs_type, "yaffs2") == 0 || strcmp(fs_type, "mtd") == 0) {
        mtd_scan_partitions();
        const MtdPartition* partition = mtd_find_partition_by_name(device);
        if (partition == NULL) {
            LOGE("format_volume: no MTD partition \"%s\"\n", device);
            return -1;
        }

        MtdWriteContext *write = mtd_write_partition(partition);
        if (write == NULL) {
            LOGW("format_volume: can't open MTD \"%s\"\n", device);
            return -1;
        } else if (mtd_erase_blocks(write, -1) == (off_t) -1) {
            LOGW("format_volume: can't erase MTD \"%s\"\n", device);
            mtd_write_close(write);
            return -1;
        } else if (mtd_write_close(write)) {
            LOGW("format_volume: can't close MTD \"%s\"\n",device);
            return -1;
        }
        return 0;
    }

    if (strcmp(fs_type, "ext4") == 0) {
        reset_ext4fs_info();
        int result = make_ext4fs(device, NULL, NULL, 0, 0, 0);
        if (result != 0) {
            LOGE("format_volume: make_extf4fs failed on %s\n", device);
            return -1;
        }
        return 0;
    }

    return format_unknown_device(device, path, fs_type);
}

int format_unknown_device(const char *device, const char* path, const char *fs_type)
{
    LOGI("formatting unknown device.\n");

    if (fs_type != NULL && get_flash_type(fs_type) != UNSUPPORTED)
        return erase_raw_partition(fs_type, device);

    // if this is SDEXT:, don't worry about it if it does not exist.
    if (0 == strcmp(path, "/sd-ext"))
    {
        struct stat st;
        Volume *vol = volume_for_path("/sd-ext");
        if (vol == NULL || 0 != stat(vol->device, &st))
        {
            ui_print("no app2sd partition found. skipping format of /sd-ext.\n");
            return 0;
        }
    }

    if (NULL != fs_type) {
        if (strcmp("ext3", fs_type) == 0) {
            LOGI("formatting ext3 device.\n");
            if (0 != ensure_path_unmounted(path)) {
                LOGE("error while unmounting %s.\n", path);
                return -12;
            }
            return format_ext3_device(device);
        }

        if (strcmp("ext2", fs_type) == 0) {
            LOGI("formatting ext2 device.\n");
            if (0 != ensure_path_unmounted(path)) {
                LOGE("error while unmounting %s.\n", path);
                return -12;
            }
            return format_ext2_device(device);
        }
    }

    if (0 != ensure_path_mounted(path))
    {
        ui_print("error mounting %s.\n", path);
        ui_print("skipping format...\n");
        return 0;
    }

    static char tmp[PATH_MAX];
    if (strcmp(path, "/data") == 0) {
        sprintf(tmp, "cd /data ; for f in $(ls -a | grep -v ^media$); do rm -rf $f; done");
        __system(tmp);
    }
    else {
        sprintf(tmp, "rm -rf %s/*", path);
        __system(tmp);
        sprintf(tmp, "rm -rf %s/.*", path);
        __system(tmp);
    }

    ensure_path_unmounted(path);
    return 0;
}

//#define MOUNTABLE_COUNT 5
//#define DEVICE_COUNT 4
//#define MMC_COUNT 2

typedef struct {
    char mount[255];
    char unmount[255];
    Volume* v;
} MountMenuEntry;

typedef struct {
    char txt[255];
    Volume* v;
} FormatMenuEntry;

int is_safe_to_format(char* name)
{
    char str[255];
    char* partition;
    property_get("ro.cwm.forbid_format", str, "/misc,/radio,/bootloader,/recovery,/efs");

    partition = strtok(str, ", ");
    while (partition != NULL) {
        if (strcmp(name, partition) == 0) {
            return 0;
        }
        partition = strtok(NULL, ", ");
    }

    return 1;
}

void show_partition_menu()
{
    char** headers = NULL;
    char* headers_msm[] = {  "|| mounts & storage |/____________________________,/|",
			     "|+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+|/|",
		              NULL
    		           };
    headers = prepend_title((const char**)headers_msm);

    static MountMenuEntry* mount_menue = NULL;
    static FormatMenuEntry* format_menue = NULL;

    typedef char* string;

    int i, mountable_volumes, formatable_volumes;
    int num_volumes;
    Volume* device_volumes;

    num_volumes = get_num_volumes();
    device_volumes = get_device_volumes();

    string options[255];

    if(!device_volumes)
		return;

		mountable_volumes = 0;
		formatable_volumes = 0;

		mount_menue = malloc(num_volumes * sizeof(MountMenuEntry));
		format_menue = malloc(num_volumes * sizeof(FormatMenuEntry));

		for (i = 0; i < num_volumes; ++i) {
			Volume* v = &device_volumes[i];
			if(strcmp("ramdisk", v->fs_type) != 0 && strcmp("mtd", v->fs_type) != 0 && strcmp("emmc", v->fs_type) != 0 && strcmp("bml", v->fs_type) != 0)
			{
				if(strcmp("\/sdcard", v->mount_point) == 0) {
					sprintf(&mount_menue[mountable_volumes].mount, "| <1> mount %s                               |/|", v->mount_point);
                                	sprintf(&mount_menue[mountable_volumes].unmount, "| <1> umount %s                              |/|", v->mount_point);
}

				if(strcmp("\/emmc", v->mount_point) == 0) {
					sprintf(&mount_menue[mountable_volumes].mount, "| <2> mount %s                                 |/|", v->mount_point);
                                	sprintf(&mount_menue[mountable_volumes].unmount, "| <2> unmount %s                               |/|", v->mount_point);
}

				if(strcmp("\/systemorig", v->mount_point) == 0) {
					sprintf(&mount_menue[mountable_volumes].mount, "| <3> mount %s                           |/|", v->mount_point);
                                	sprintf(&mount_menue[mountable_volumes].unmount, "| <3> unmount %s                         |/|", v->mount_point);
}

				if(strcmp("\/cache", v->mount_point) == 0) {
					sprintf(&mount_menue[mountable_volumes].mount, "| <4> mount %s                                |/|", v->mount_point);
                                	sprintf(&mount_menue[mountable_volumes].unmount, "| <4> unmount %s                              |/|", v->mount_point);
}

				if(strcmp("\/system", v->mount_point) == 0) {
					sprintf(&mount_menue[mountable_volumes].mount, "| <5> mount %s                               |/|", v->mount_point);
                                	sprintf(&mount_menue[mountable_volumes].unmount, "| <5> unmount %s                             |/|", v->mount_point);
}

				if(strcmp("\/data", v->mount_point) == 0) {
					sprintf(&mount_menue[mountable_volumes].mount, "| <6> mount %s                                 |/|", v->mount_point);
                                	sprintf(&mount_menue[mountable_volumes].unmount, "| <6> unmount %s                               |/|", v->mount_point);
}
				mount_menue[mountable_volumes].v = &device_volumes[i];
				++mountable_volumes;
				if (is_safe_to_format(v->mount_point)) {
					if(strcmp("\/sdcard", v->mount_point) == 0) {
						sprintf(&format_menue[formatable_volumes].txt, "| <7> format %s                              |/|",v->mount_point);
					}
					if(strcmp("\/emmc", v->mount_point) == 0) {
						sprintf(&format_menue[formatable_volumes].txt, "| <8> format %s                                |/|",v->mount_point);
					}
					if(strcmp("\/cache", v->mount_point) == 0) {
						sprintf(&format_menue[formatable_volumes].txt, "| <9> format %s                               |/|",v->mount_point);
					}
					if(strcmp("\/system", v->mount_point) == 0) {
						sprintf(&format_menue[formatable_volumes].txt, "| <0> format %s                              |/|",v->mount_point);
					}
					if(strcmp("\/data", v->mount_point) == 0) {
						sprintf(&format_menue[formatable_volumes].txt, "| <A> format %s                                |/|",v->mount_point);
					}

					format_menue[formatable_volumes].v = &device_volumes[i];
					++formatable_volumes;
				}
		    }
		    else if (strcmp("ramdisk", v->fs_type) != 0 && strcmp("mtd", v->fs_type) == 0 && is_safe_to_format(v->mount_point))
		    {
				sprintf(&format_menue[formatable_volumes].txt, "| <C> format %s                             |/|", v->mount_point);
				format_menue[formatable_volumes].v = &device_volumes[i];
				++formatable_volumes;
			}
		}


    const char* confirm_format[]= {   
				       "|+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+|",
				       "|       | c o n f i r m     f o r m a t   ? |       |",
				       "|+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+|",
		      		       NULL
				     };

    char confirm_f[MENU_HEADER_COLS] = "| <8> yes - confirm format                        |/|";

    for (;;)
    {

		for (i = 0; i < mountable_volumes; i++)
		{
			MountMenuEntry* e = &mount_menue[i];
			Volume* v = e->v;
			if(is_path_mounted(v->mount_point))
				options[i] = e->unmount;
			else
				options[i] = e->mount;
		}

		for (i = 0; i < formatable_volumes; i++)
		{
			FormatMenuEntry* e = &format_menue[i];

			options[mountable_volumes+i] = e->txt;
		}

        options[mountable_volumes+formatable_volumes] = "| <B> mount USB storage                           |/|";
        options[mountable_volumes+formatable_volumes + 1] = NULL;

        int chosen_item = get_menu_selection(headers, &options, 0, 0, 0, 0);
	if (chosen_item == GO_BACK)
            break;
        if (chosen_item == (mountable_volumes+formatable_volumes))
        {
            show_mount_usb_storage_menu();
        }
        else if (chosen_item < mountable_volumes)
        {
	    MountMenuEntry* e = &mount_menue[chosen_item];
            Volume* v = e->v;

            if (is_path_mounted(v->mount_point))
            {
                if (0 != ensure_path_unmounted(v->mount_point))
                    ui_print("error unmounting %s.\n", v->mount_point);
            }
            else
            {
                if (0 != ensure_path_mounted(v->mount_point))
                    ui_print("error mounting %s.\n",  v->mount_point);
            }
        }
        else if (chosen_item < (mountable_volumes + formatable_volumes))
        {
            chosen_item = chosen_item - mountable_volumes;
            FormatMenuEntry* e = &format_menue[chosen_item];
            Volume* v = e->v;

            //sprintf(confirm_string, "%s - %s", v->mount_point, confirm_format);

            if (!confirm_selection(confirm_format, confirm_f))
                continue;
            ui_print("formatting %s...\n", v->mount_point);
            if (0 != format_volume(v->mount_point))
                ui_print("error formatting %s.\n", v->mount_point);
            else
                ui_print("done.\n");
        }
    }

    free(mount_menue);
    free(format_menue);

}

void show_nandroid_advanced_restore_menu(const char* path)
{
    if (ensure_path_mounted(path) != 0) {
        LOGE ("can't mount /sdcard\n");
        return;
    }
    char** advancedheaders = NULL;
    static char* advancedheaders_before[] = {  "|| image  selection |/____________________________,/|",
                                	       "|+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+|/|",
                                	       "|-------------------------------------------------|/|",
                                	       "| choose an image to restore first.  then you     |/|",
                                	       "| can select the partition you wish to restore.   |/|",
                                	       "|-------------------------------------------------|/|",
		                               NULL

                                             };

    advancedheaders = prepend_title((const char**)advancedheaders_before);
    char tmp[PATH_MAX];
    sprintf(tmp, "%s/%s/backup/", path, EXPAND(RECOVERY_FOLDER));

    char* file = choose_file_menu(tmp, NULL, advancedheaders);
    if (file == NULL)
        return;
    char** headers = NULL;
    char* headers_adv[] = {  "|| advanced restore |/____________________________,/|",
			     "|+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+|/|",
		              NULL

    };
    headers = prepend_title((const char**)headers_adv);

    char** list[] = {   "| <1> restore /system                             |/|",
                        "| <2> restore /data                               |/|",
                        "| <3> restore /cache                              |/|",
                        "| <4> restore /systemorig (stock /system)         |/|",
		        NULL

    };

    int num_menu_items = 4;

    const char* confirm_restore[]={  
				       "|+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+|",
				       "|       | c o n f i r m   r e s t o r e   ? |       |",
				       "|+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+|",
		      		       NULL
				    };

    char confirm_rs[MENU_HEADER_COLS] = "| <8> yes - restore /system                       |/|";

    int chosen_item = get_menu_selection(headers, list, 0, 0, 0, 0);
     if (chosen_item == 0){
        if (confirm_selection(confirm_restore, confirm_rs))
            nandroid_restore(file, 1, 0, 0, 0);
       }
       if (chosen_item == 1){
        if (confirm_selection(confirm_restore, "| <8> yes - restore /data                         |/|"))
            nandroid_restore(file, 0, 1, 0, 0);
       }
       if (chosen_item == 2){
        if (confirm_selection(confirm_restore, "| <8> yes - restore /cache                        |/|"))
            nandroid_restore(file, 0, 0, 1, 0);
       }
       if (chosen_item == 3){
        if (confirm_selection(confirm_restore, "| <8> yes - restore /systemorig                   |/|"))
            nandroid_restore(file, 0, 0, 0, 1);
       }
}

void show_nandroid_menu()
{
    char** headers = NULL;
    char* headers_nan[] = {    "||  nandroid  menu  |/____________________________,/|",
			       "|+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+|/|",
		               NULL
    		           };

    headers = prepend_title((const char**)headers_nan);

    char** list[] =    {    "| <1> backup                                      |/|",
                            "| <2> verify                                      |/|",
                            "| <3> restore                                     |/|",
                            "| <4> advanced restore                            |/|",
		            NULL
    		       };
    safemode = get_safe_mode();
#ifdef BOARD_HAS_SDCARD_INTERNAL
    int chosen_sdcard = -1;
#endif
    int chosen_item = get_menu_selection(headers, list, 0, 0, 0, 0);
    switch (chosen_item)
    {
        case 0:
            {
                char backup_path[PATH_MAX];
                char final_path[PATH_MAX];
                time_t t = time(NULL);
                struct tm *tmp = localtime(&t);
#ifdef BOARD_HAS_SDCARD_INTERNAL
                chosen_sdcard = show_sdcard_selection_menu();
                if (chosen_sdcard > -1)
                {
                    switch (chosen_sdcard) {
                        case 0:
                            sprintf(backup_path, "/emmc");
                            break;
                        case 1:
                            sprintf(backup_path, "/sdcard");
                            break;
                    }
                }
                else break;
#else
                sprintf(backup_path, "/sdcard");
#endif
                int skip_webtop = 1;
#ifdef BOARD_HAS_WEBTOP
                static char** header[] = { "|| include  webtop? |/____________________________,/|",
					   "|+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+|/|",
		            		   NULL

                };

                static char** item[] = { "| <1> yes - include webtop                        |/|",
                                         "| <2> no                                          |/|", 
		            		 NULL

                };

                skip_webtop = get_menu_selection(header, item, 0, 0, 0, 0);
                if (skip_webtop == GO_BACK) {
                    return;
                }
#endif
                int skip_origsys = 0;
                if (safemode) {
                    char** header = NULL;
		    static char** header_origs[] = { "||include  /sysorig?|/____________________________,/|",
					       "|+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+|/|",
		            			NULL

                    			    };
		    header = prepend_title((const char**)header_origs);
                    static char** item[] = { "| <1> yes - include /systemorig                   |/|",
                                             "| <2> no                                          |/|", 
		            NULL

                    			  };
                    skip_origsys = get_menu_selection(header, item, 0, 0, 0, 0);
                    if (skip_origsys == GO_BACK) {
                        return;
                    }
                }
                if (tmp == NULL)
                {
                    struct timeval tp;
                    gettimeofday(&tp, NULL);
                    sprintf(final_path, "%s/%s/backup/%s-%d", backup_path, EXPAND(RECOVERY_FOLDER), safemode==0 ? "nonsafe" : "safe", tp.tv_sec);
                }
                else
                {
                    char tmp_path[PATH_MAX];
                    strftime(tmp_path, sizeof(tmp_path), "%F.%H.%M.%S", tmp);
                    sprintf(final_path, "%s/%s/backup/%s-%s", backup_path, EXPAND(RECOVERY_FOLDER), safemode==0 ? "nonsafe" : "safe", tmp_path);
                }
                nandroid_backup(final_path, backup_path, skip_webtop, skip_origsys);
            }
            break;
        case 1:
#ifdef BOARD_HAS_SDCARD_INTERNAL
            chosen_sdcard = show_sdcard_selection_menu();
            if (chosen_sdcard > -1)
            {
                switch (chosen_sdcard) {
                    case 0:
                        show_nandroid_verify_menu("/emmc");
                        break;
                    case 1:
                        show_nandroid_verify_menu("/sdcard");
                        break;
                }
            }
            else break;
#else
            show_nandroid_verify_menu("/sdcard");
#endif
            break;
        case 2:
#ifdef BOARD_HAS_SDCARD_INTERNAL
            chosen_sdcard = show_sdcard_selection_menu();
            if (chosen_sdcard > -1)
            {
                switch (chosen_sdcard) {
                    case 0:
                        show_nandroid_restore_menu("/emmc");
                        break;
                    case 1:
                        show_nandroid_restore_menu("/sdcard");
                        break;
                }
            }
            else break;
#else
            show_nandroid_restore_menu("/sdcard");
#endif
        case 3:
#ifdef BOARD_HAS_SDCARD_INTERNAL
            chosen_sdcard = show_sdcard_selection_menu();
            if (chosen_sdcard > -1)
            {
                switch (chosen_sdcard) {
                    case 0:
                        show_nandroid_advanced_restore_menu("/emmc");
                        break;
                    case 1:
                        show_nandroid_advanced_restore_menu("/sdcard");
                        break;
                }
            }
            else break;
#else
            show_nandroid_advanced_restore_menu("/sdcard");
#endif
            break;
    }
}

void wipe_battery_stats()
{
    ensure_path_mounted("/data");
    remove("/data/system/batterystats.bin");
    ensure_path_unmounted("/data");
    ui_print("battery statistics wiped.\n");
}

void show_advanced_menu(int safemode_enabled)
{
    char** headers = NULL;
    char* headers_advdbg[] = {  "||  advanced  menu  |/____________________________,/|",
				"|+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+|/|",
		                NULL
    };

    headers = prepend_title((const char**)headers_advdbg);

    for (;;)
    {
	int chosen_item = get_menu_selection(headers, ADV_MENU_ITEMS, 0, 0, 0, safemode_enabled);
	if (chosen_item == GO_BACK)
            break;
        switch (chosen_item)
        {
            case 1:
            {
                reboot_wrapper("recovery");
                break;
            }
            case 2:
            {
                if (0 != ensure_path_mounted("/data"))
                    break;
                ensure_path_mounted("/sd-ext");
                ensure_path_mounted("/cache");
                const char* confirm_wipe[] = {
				       		"|+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+|",
				       		"|       | c o n f i r m       w i p e     ? |       |",
				      		"|+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+|",
		      		       		NULL
				             };

		char confirm_dal[MENU_HEADER_COLS] = "| <8> yes - wipe dalvik cache                     |/|";

		if (confirm_selection(confirm_wipe,confirm_dal)) {
                    __system("rm -r /data/dalvik-cache");
                    __system("rm -r /cache/dalvik-cache");
                    __system("rm -r /sd-ext/dalvik-cache");
                    ui_print("dalvik cache wiped.\n");
                }
                ensure_path_unmounted("/data");
                break;
            }
            case 3:
            {
		const const char* confirm_wipe[] = {    
				       			   "|+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+|",
				       			   "|       | c o n f i r m       w i p e     ? |       |",
				      			   "|+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+|",
		      		       			   NULL
					         };
	
		char confirm_bat[MENU_HEADER_COLS] = "| <8> yes - wipe battery statistics               |/|";
 
		if (confirm_selection( confirm_wipe, confirm_bat))
                    wipe_battery_stats();
                break;
            }
            case 4:
                handle_failure(1);
                break;
            case 5:
            {
                ui_print("outputting key codes.\n");
                ui_print("go back to end debugging.\n");
                int key;
                int action;
                do
                {
                    key = ui_wait_key();
                    action = device_handle_key(key);
                    ui_print("key: %d\n", key);
                }
                while (action != GO_BACK);
                break;
            }
            case 0:
            {
                ui_printlogtail(12);
                break;
            }
            case 7:
            {
                static char* ext_sizes[] = { "| <1> 128MB                                       |/|",
                                             "| <2> 256MB                                       |/|",
                                             "| <3> 512MB                                       |/|",
                                             "| <4> 1024MB                                      |/|",
                                             "| <5> 2048MB                                      |/|",
                                             "| <6> 4096MB                                      |/|",
		            		     NULL
                                           };

                static char* swap_sizes[] = { "| <1> 0MB                                         |/|",
                                              "| <2> 32MB                                        |/|",
                                              "| <3> 64MB                                        |/|",
                                              "| <4> 128MB                                       |/|",
                                              "| <5> 256MB                                       |/|",
		                              NULL
                                            };

                char** ext_headers = NULL;
		char** swap_headers = NULL;
		char* ext_headers_b[] = { "||  external  size  |/____________________________,/|",
				          "|+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+|/|", 
					  NULL
					};
                char* swap_headers_b[] = { "||     swap size    |/____________________________,/|",
				           "|+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+|/|", 
					    NULL 
					 };
		ext_headers = prepend_title((const char**)ext_headers_b);
		swap_headers = prepend_title((const char**)swap_headers_b);

                int ext_size = get_menu_selection(ext_headers, ext_sizes, 0, 0, 0, 0);
                if (ext_size == GO_BACK)
                    continue;

                int swap_size = get_menu_selection(swap_headers, swap_sizes, 0, 0, 0, 0);
                if (swap_size == GO_BACK)
                    continue;

                char sddevice[256];
                Volume *vol = volume_for_path("/sdcard");
                strcpy(sddevice, vol->device);
                // we only want the mmcblk, not the partition
                sddevice[strlen("/dev/block/mmcblkX")] = NULL;
                char cmd[PATH_MAX];
                setenv("SDPATH", sddevice, 1);
                sprintf(cmd, "sdparted -es %s -ss %s -efs ext3 -s", ext_sizes[ext_size], swap_sizes[swap_size]);
                ui_print("partitioning SD card... please wait...\n");
                if (0 == __system(cmd))
                    ui_print("done.\n");
                else
                    ui_print("an error occured while partitioning your SD card.  please see /tmp/recovery.log for more details.\n");
                break;
            }
            case 6:
            {
                ensure_path_mounted("/system");
                ensure_path_mounted("/data");
                ui_print("fixing permissions...\n");
                __system("fix_permissions");
                ui_print("done.\n");
                break;
            }
            case 8:
            {
                static char* dat_sizes[] = { "| <1> 128MB                                       |/|",
                                             "| <2> 256MB                                       |/|",
                                             "| <3> 512MB                                       |/|",
                                             "| <4> 1024MB                                      |/|",
                                             "| <5> 2048MB                                      |/|",
                                             "| <6> 4096MB                                      |/|",
		            		     NULL

                                           };

                static char* swap_sizes[] = { "| <1> 0MB                                         |/|",
                                              "| <2> 32MB                                        |/|",
                                              "| <3> 64MB                                        |/|",
                                              "| <4> 128MB                                       |/|",
                                              "| <5> 256MB                                       |/|",
		                              NULL
                                            };

                char** dat_headers = NULL;
		char** swap_headers = NULL;
                char* dat_headers_b[] = { "||     data size    |/____________________________,/|",
				          "|+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+|/|", 
					  NULL 
				        };
                char* swap_headers_b[] = { "||     swap size    |/____________________________,/|",
				           "|+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+|/|", 
					    NULL  
					 };
		dat_headers = prepend_title((const char**)dat_headers_b);
		swap_headers = prepend_title((const char**)swap_headers_b);

                int dat_size = get_menu_selection(dat_headers, dat_sizes, 0, 0, 0, 0);
                if (dat_size == GO_BACK)
                    continue;

                int swap_size = 0;
                if (swap_size == GO_BACK)
                    continue;

                char sddevice[256];
                Volume *vol = volume_for_path("/emmc");
                strcpy(sddevice, vol->device);
                // we only want the mmcblk, not the partition
                sddevice[strlen("/dev/block/mmcblkX")] = NULL;
                char cmd[PATH_MAX];
                setenv("SDPATH", sddevice, 1);
                sprintf(cmd, "sdparted -es %s -ss %s -efs ext3 -s", dat_sizes[dat_size], swap_sizes[swap_size]);
                ui_print("partitioning internal SD card... please wait...\n");
                if (0 == __system(cmd))
                    ui_print("done.\n");
                else
                    ui_print("an error occured while partitioning your internal SD card.  please see /tmp/recovery.log for more details.\n");
                break;
            }
            case 9:
            {   
		const char* confirm_install[] = { 
				       	   	  "|+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+|",
				       	  	  "|       | c o n f i r m   i n s t a l l   ? |       |",
				      	   	  "|+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+|",
		      		       		   NULL
				      		};
	
		char confirm_sd[MENU_HEADER_COLS] = "| <8> yes - install .zip to non-safe partition    |/|";

	        if (confirm_selection( confirm_install, confirm_sd ))
		{ 
		const char* confirm_sure[] = {   
				       	   	  "|+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+|",
				       	  	  "|       | a r e   y o u   s u r e   ? ? ? ? |       |",
				      	   	  "|+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+|",
		      		       		   NULL

					       };
 		char confirm2[MENU_HEADER_COLS] = "| <8> yes - i know what i'm doing                 |/|"; 

		if(confirm_selection(confirm_sure, confirm2 ))
		  {
		    show_install_update_menu();
               	    safemode = get_safe_mode();
		  }
		}
                break;
	    }
        }
    }
}

void write_fstab_root(char *path, FILE *file)
{
    Volume *vol = volume_for_path(path);
    if (vol == NULL) {
        LOGW("unable to get recovery.fstab info for %s during fstab generation.\n", path);
        return;
    }

    char device[200];
    if (vol->device[0] != '/')
        get_partition_device(vol->device, device);
    else
        strcpy(device, vol->device);

    fprintf(file, "%s ", device);
    fprintf(file, "%s ", path);
    // special case rfs cause auto will mount it as vfat on samsung.
    fprintf(file, "%s rw\n", vol->fs_type2 != NULL && strcmp(vol->fs_type, "rfs") != 0 ? "auto" : vol->fs_type);
}

void create_fstab()
{
    struct stat info;
    __system("touch /etc/mtab");
    FILE *file = fopen("/etc/fstab", "w");
    if (file == NULL) {
        LOGW("unable to create /etc/fstab.\n");
        return;
    }
    Volume *vol = volume_for_path("/boot");
#ifndef BOARD_HAS_LOCKED_BOOTLOADER
    if (NULL != vol && strcmp(vol->fs_type, "mtd") != 0 && strcmp(vol->fs_type, "emmc") != 0 && strcmp(vol->fs_type, "bml") != 0)
         write_fstab_root("/boot", file);
#endif
    write_fstab_root("/cache", file);
    write_fstab_root("/data", file);
    if (has_datadata()) {
        write_fstab_root("/datadata", file);
    }
    write_fstab_root("/systemorig", file);
    write_fstab_root("/system", file);
#ifdef BOARD_HAS_SDCARD_INTERNAL
    write_fstab_root("/emmc", file);
#endif
    write_fstab_root("/sdcard", file);
#ifdef BOARD_HAS_SDEXT
    write_fstab_root("/sd-ext", file);
#endif
#ifdef BOARD_HAS_WEBTOP
    write_fstab_root("/osh", file);
#endif
    fclose(file);
    LOGI("completed outputting fstab.\n");
}

int bml_check_volume(const char *path) {
    ui_print("checking %s...\n", path);
    ensure_path_unmounted(path);
    if (0 == ensure_path_mounted(path)) {
        ensure_path_unmounted(path);
        return 0;
    }
    
    Volume *vol = volume_for_path(path);
    if (vol == NULL) {
        LOGE("unable process volume.  skipping...\n");
        return 0;
    }
    
    ui_print("%s may be rfs.  checking...\n", path);
    char tmp[PATH_MAX];
    sprintf(tmp, "mount -t rfs %s %s", vol->device, path);
    int ret = __system(tmp);
    printf("%d\n", ret);
    return ret == 0 ? 1 : 0;
}

void process_volumes() {
    create_fstab();

    if (is_data_media()) {
        setup_data_media();
    }

    return;

    // dead code.
    if (device_flash_type() != BML)
        return;

    ui_print("checking for ext4 partitions...\n");
    int ret = 0;
    ret = bml_check_volume("/system");
    ret |= bml_check_volume("/data");
    if (has_datadata())
        ret |= bml_check_volume("/datadata");
    ret |= bml_check_volume("/cache");
    
    if (ret == 0) {
        ui_print("done.\n");
        return;
    }
    
    char backup_path[PATH_MAX];
    time_t t = time(NULL);
    char backup_name[PATH_MAX];
    struct timeval tp;
    gettimeofday(&tp, NULL);
    sprintf(backup_name, "before-ext4-convert-%d", tp.tv_sec);
    sprintf(backup_path, "/sdcard/%s/backup/%s", EXPAND(RECOVERY_FOLDER), backup_name);

    ui_set_show_text(1);
    ui_print("filesystems need to be converted to ext4.\n");
    ui_print("a backup and restore will now take place.\n");
    ui_print("if anything goes wrong, your backup will be\n");
    ui_print("named %s. try restoring it\n", backup_name);
    ui_print("in case of error.\n");

    nandroid_backup(backup_path, "/sdcard", 0, 0);
    nandroid_restore(backup_path, 1, 1, 1, 0);
    ui_set_show_text(0);
}

void handle_failure(int ret)
{
    char tmp[PATH_MAX];
    if (ret == 0)
        return;
    if (0 != ensure_path_mounted("/sdcard"))
        return;
    sprintf(tmp, "/sdcard/%s", EXPAND(RECOVERY_FOLDER));
    mkdir(tmp, S_IRWXU);
    sprintf(tmp, "cp /tmp/recovery.log /sdcard/%s/recovery.log", EXPAND(RECOVERY_FOLDER));
    __system(tmp);
    ui_print("/tmp/recovery.log was copied to /sdcard/%s/recovery.log.  please open Safestrap to report the issue.\n", EXPAND(RECOVERY_FOLDER));
}

int is_path_mounted(const char* path) {
    Volume* v = volume_for_path(path);
    if (v == NULL) {
        return 0;
    }
    if (strcmp(v->fs_type, "ramdisk") == 0) {
        // the ramdisk is always mounted.
        return 1;
    }

    int result;
    result = scan_mounted_volumes();
    if (result < 0) {
        LOGE("failed to scan mounted volumes.\n");
        return 0;
    }

    const MountedVolume* mv =
        find_mounted_volume_by_mount_point(v->mount_point);
    if (mv) {
        // volume is already mounted
        return 1;
    }
    return 0;
}

int has_datadata() {
    Volume *vol = volume_for_path("/datadata");
    return vol != NULL;
}

int volume_main(int ar, char **argv) {
    load_volume_table();
    return 0;
}
