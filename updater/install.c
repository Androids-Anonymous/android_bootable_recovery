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

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <linux/input.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <termios.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/vfs.h>

#include "cutils/misc.h"
#include "cutils/properties.h"
#include "edify/expr.h"
#include "mincrypt/sha.h"
#include "minzip/DirUtil.h"
#include "mtdutils/mtdutils.h"
#include "updater.h"
#include "applypatch/applypatch.h"
#include "flashutils/flashutils.h"
#include "../roots.h"
#include "../mounts.h"
#include "../safebootcommands.h"
#include "../extendedcommands.h"
#include "../common.h"

#define FLASH_NS_FILE "/.flash_non_safe"

#ifdef USE_EXT4
#include "make_ext4fs.h"
#endif

int check_systemorig_mount() {
    int result = 0;
    result = scan_mounted_volumes();
    if (result < 0) {
        return 0;
    }
    const MountedVolume* mv = find_mounted_volume_by_mount_point("/systemorig");
    if (mv == NULL) {
        system("mount /dev/block/systemorig /systemorig");
        result = scan_mounted_volumes();
        if (result < 0) {
            return 0;
        }
        const MountedVolume* mv = find_mounted_volume_by_mount_point("/systemorig");
        if (mv == NULL) {
            return 0;
        }
    }
    return 1;
}

int get_safe_mode() {
    int result = 0;
    if (check_systemorig_mount()) {
       struct statfs info;
       if (0 == statfs(SAFE_SYSTEM_FILE, &info))
           result = 1;
    }
    system("umount /systemorig");
    return result;
}

int
allow_flash_non_safe()
{
    FILE* fnsfd;
    int chars;
    int closed;
    struct statfs fns_info;
    int flash_non_safe = 0;
    
    if (!(statfs(FLASH_NS_FILE, &fns_info)))
    {
    	fnsfd = fopen(FLASH_NS_FILE, "r");
	char *charsin = calloc(2,sizeof(char));
	fgets(charsin,2,fnsfd);
	flash_non_safe = atoi(charsin);	
	closed = fclose(fnsfd);
    }    
    return flash_non_safe;
}

int safe_mode;

// mount(fs_type, partition_type, location, mount_point)
//
//    fs_type="yaffs2" partition_type="MTD"     location=partition
//    fs_type="ext4"   partition_type="EMMC"    location=device
Value* MountFn(const char* name, State* state, int argc, Expr* argv[]) {
    char* result = NULL;
    if (argc != 4) {
        return ErrorAbort(state, "%s() expects 4 args, got %d", name, argc);
    }
    char* fs_type;
    char* partition_type;
    char* location;
    char* mount_point;
    safe_mode = get_safe_mode();
 
    if (ReadArgs(state, argv, 4, &fs_type, &partition_type,
                 &location, &mount_point) < 0) {
        return NULL;
    }

    if (strlen(fs_type) == 0) {
        ErrorAbort(state, "fs_type argument to %s() can't be empty", name);
        goto done;
    }
    if (strlen(partition_type) == 0) {
        ErrorAbort(state, "partition_type argument to %s() can't be empty",
                   name);
        goto done;
    }
    if (strlen(location) == 0) {
        ErrorAbort(state, "location argument to %s() can't be empty", name);
        goto done;
    }
    if (!(strcmp(location,BOARD_NONSAFE_SYSTEM_DEVICE)) && !(allow_flash_non_safe())) {
        /* dynamically change system block device mount to /dev/block/system alias */
        location = strdup("/dev/block/system");
    }
    if (!strlen(mount_point)) {
        ErrorAbort(state, "mount_point argument to %s() can't be empty", name);
        goto done;
    }
    // if we're in non-safe mode but have enabled flashing, and the script
    // wants to alter /system, then:
    // 1: attempt to backup safestrap files to the internal sdcard
    // 2: swap it to /systemorig
    else if (!(safe_mode) && !(strcmp(mount_point,"/system")) && (allow_flash_non_safe()))
    {
	mount_point = strdup("/systemorig");
    }
    mkdir(mount_point, 0755);

    if (strcmp(partition_type, "MTD") == 0) {
        mtd_scan_partitions();
        const MtdPartition* mtd;
        mtd = mtd_find_partition_by_name(location);
        if (mtd == NULL) {
            fprintf(stderr, "%s: no mtd partition named \"%s\"",
                    name, location);
            result = strdup("");
            goto done;
        }
        if (mtd_mount_partition(mtd, mount_point, fs_type, 0 /* rw */) != 0) {
            fprintf(stderr, "mtd mount of %s failed: %s\n",
                    location, strerror(errno));
            result = strdup("");
            goto done;
        }
        result = mount_point;
    } else {
        if (mount(location, mount_point, fs_type,
                  MS_NOATIME | MS_NODEV | MS_NODIRATIME, "") < 0) {
            fprintf(stderr, "%s: failed to mount %s at %s: %s\n",
                    name, location, mount_point, strerror(errno));
            result = strdup("");
        } else {
            result = mount_point;
        }
    }

done:
    free(fs_type);
    free(partition_type);
    free(location);
    if (result != mount_point) free(mount_point);
    return StringValue(result);
}


// is_mounted(mount_point)
Value* IsMountedFn(const char* name, State* state, int argc, Expr* argv[]) {
    char* result = NULL;
    safe_mode = get_safe_mode();
    if (argc != 1) {
        return ErrorAbort(state, "%s() expects 1 arg, got %d", name, argc);
    }
    char* mount_point;
    if (ReadArgs(state, argv, 1, &mount_point) < 0) {
        return NULL;
    }
    if (strlen(mount_point) == 0) {
        ErrorAbort(state, "mount_point argument to unmount() can't be empty");
        goto done;
    }
    // if in non-safe mode, non-safe flashing is allowed and we want to check
    // if /system is mounted, then make sure we're checking /systemorig
    else if ( (!(safe_mode) ) && (!(strcmp(mount_point,"/system"))) && (allow_flash_non_safe()) )
    {
	mount_point = strdup("/systemorig");
    }
    scan_mounted_volumes();
    const MountedVolume* vol = find_mounted_volume_by_mount_point(mount_point);
    if (vol == NULL) {
        result = strdup("");
    } else {
        result = mount_point;
    }

done:
    if (result != mount_point) free(mount_point);
    return StringValue(result);
}


Value* UnmountFn(const char* name, State* state, int argc, Expr* argv[]) {
    char* result = NULL;
    safe_mode = get_safe_mode();
    if (argc != 1) {
        return ErrorAbort(state, "%s() expects 1 arg, got %d", name, argc);
    }
    char* mount_point;
    if (ReadArgs(state, argv, 1, &mount_point) < 0) {
        return NULL;
    }
    if (strlen(mount_point) == 0) {
        ErrorAbort(state, "mount_point argument to unmount() can't be empty");
        goto done;
    }
    // make sure we unmount /systemorig if in non-safe mode
    else if ( (!(safe_mode) ) && (!(strcmp(mount_point,"/system"))) && (allow_flash_non_safe()) )
    {
	mount_point = strdup("/systemorig");
    }
    scan_mounted_volumes();
    const MountedVolume* vol = find_mounted_volume_by_mount_point(mount_point);
    if (vol == NULL) {
        fprintf(stderr, "unmount of %s failed; no such volume\n", mount_point);
        result = strdup("");
    } else {
        unmount_mounted_volume(vol);
        result = mount_point;
    }

done:
    if (result != mount_point) free(mount_point);
    return StringValue(result);
}


// format(fs_type, partition_type, location)
//
//    fs_type="yaffs2" partition_type="MTD"     location=partition
//    fs_type="ext4"   partition_type="EMMC"    location=device
Value* FormatFn(const char* name, State* state, int argc, Expr* argv[]) {
    char* result = NULL;
    safe_mode = get_safe_mode();
    if (argc != 3) {
        return ErrorAbort(state, "%s() expects 3 args, got %d", name, argc);
    }
    char* fs_type;
    char* partition_type;
    char* location;
    if (ReadArgs(state, argv, 3, &fs_type, &partition_type, &location) < 0) {
        return NULL;
    }

    if (strlen(fs_type) == 0) {
        ErrorAbort(state, "fs_type argument to %s() can't be empty", name);
        goto done;
    }
    if (strlen(partition_type) == 0) {
        ErrorAbort(state, "partition_type argument to %s() can't be empty",
                   name);
        goto done;
    }
    if (strlen(location) == 0) {
        ErrorAbort(state, "location argument to %s() can't be empty", name);
        goto done;
    }
    if ( (strcmp(location,BOARD_NONSAFE_SYSTEM_DEVICE) == 0) && !(allow_flash_non_safe())) {
        /* dynamically change system block device mount to /dev/block/system alias */
        location = strdup("/dev/block/system");
    }

    if (strcmp(partition_type, "MTD") == 0) {
        mtd_scan_partitions();
        const MtdPartition* mtd = mtd_find_partition_by_name(location);
        if (mtd == NULL) {
            fprintf(stderr, "%s: no mtd partition named \"%s\"",
                    name, location);
            result = strdup("");
            goto done;
        }
        MtdWriteContext* ctx = mtd_write_partition(mtd);
        if (ctx == NULL) {
            fprintf(stderr, "%s: can't write \"%s\"", name, location);
            result = strdup("");
            goto done;
        }
        if (mtd_erase_blocks(ctx, -1) == -1) {
            mtd_write_close(ctx);
            fprintf(stderr, "%s: failed to erase \"%s\"", name, location);
            result = strdup("");
            goto done;
        }
        if (mtd_write_close(ctx) != 0) {
            fprintf(stderr, "%s: failed to close \"%s\"", name, location);
            result = strdup("");
            goto done;
        }
        result = location;
#ifdef USE_EXT4
    } else if (strcmp(fs_type, "ext4") == 0) {
        reset_ext4fs_info();
        int status = make_ext4fs(location, NULL, NULL, 0, 0, 0);
        if (status != 0) {
            fprintf(stderr, "%s: make_ext4fs failed (%d) on %s",
                    name, status, location);
            result = strdup("");
            goto done;
        }
        result = location;
#endif
    } else if (strcmp(fs_type, "ext2") == 0) {
        int status = format_ext2_device(location);
        if (status != 0) {
            fprintf(stderr, "%s: format_ext2_device failed (%d) on %s",
                    name, status, location);
            result = strdup("");
            goto done;
        }
        result = location;
    } else if (strcmp(fs_type, "ext3") == 0) {
        int status = format_ext3_device(location);
        if (status != 0) {
            fprintf(stderr, "%s: format_ext3_device failed (%d) on %s",
                    name, status, location);
            result = strdup("");
            goto done;
        }
        result = location;
    } else {
        fprintf(stderr, "%s: unsupported fs_type \"%s\" partition_type \"%s\"",
                name, fs_type, partition_type);
    }

done:
    free(fs_type);
    free(partition_type);
    if (result != location) free(location);
    return StringValue(result);
}


Value* DeleteFn(const char* name, State* state, int argc, Expr* argv[]) {
    char** paths = malloc(argc * sizeof(char*));
    safe_mode = get_safe_mode();
    int i;
    int paths_count;
    int success = 0;
    char paths_copy[argc][PATH_MAX];
    for (i = 0; i < argc; ++i) {
	    paths[i] = Evaluate(state, argv[i]);
	    if (paths[i] == NULL) {
		int j;
		for (j = 0; j < i; ++i) {
		    free(paths[j]);
		}
		free(paths);
		return NULL;
	    }
    }

    switch(safe_mode)
    {
	case 0:
	{
	    bool recursive = (strcmp(name, "delete_recursive") == 0);
		
	    for (paths_count=0; paths_count < argc; ++paths_count)
	    {
		if ( !(strncmp(paths[paths_count],"/system",7)) )
		{
		    strcpy(paths_copy[paths_count],paths[paths_count]);
		    if(strlen(paths_copy[paths_count]))
		    {
			if(!strcmp(paths_copy[paths_count],"/system"))
			{
			    int length=strlen(paths[paths_count])+1;
			    memmove(&paths_copy[paths_count][11], &paths_copy[paths_count][7],((length*sizeof(char))-7));
			    paths_copy[paths_count][7] = 'o';
			    paths_copy[paths_count][8] = 'r';
			    paths_copy[paths_count][9] = 'i';
			    paths_copy[paths_count][10] = 'g';
			    paths_copy[paths_count][(strlen(paths_copy[paths_count]))] = '\0';
			}
		    }
		}
	    }

	    for (i = 0; i < argc; ++i) 
	    {
	        if ((recursive ? dirUnlinkHierarchy(paths_copy[i]) : unlink(paths_copy[i])) == 0)
	        {
		    ++success;
	        }
	        free(paths[i]);
	    }
	break;
	}
	case 1:
    	{
	    for (i = 0; i < argc; ++i) 
	    {
		paths[i] = Evaluate(state, argv[i]);
		if (paths[i] == NULL) {
		    int j;
		    for (j = 0; j < i; ++i) {
			free(paths[j]);
		    }
		    free(paths);
		    return NULL;
		}
	    }

	    bool recursive = (strcmp(name, "delete_recursive") == 0);

	    for (i = 0; i < argc; ++i) 
	    {
	        if ((recursive ? dirUnlinkHierarchy(paths[i]) : unlink(paths[i])) == 0)
	        {
		    ++success;
	        }
	        free(paths[i]);
	    }
	break;
        }
    }
    
    free(paths);

    char buffer[10];
    sprintf(buffer, "%d", success);
    return StringValue(strdup(buffer));
}

Value* ShowProgressFn(const char* name, State* state, int argc, Expr* argv[]) {
    if (argc != 2) {
        return ErrorAbort(state, "%s() expects 2 args, got %d", name, argc);
    }
    char* frac_str;
    char* sec_str;
    if (ReadArgs(state, argv, 2, &frac_str, &sec_str) < 0) {
        return NULL;
    }

    double frac = strtod(frac_str, NULL);
    int sec = strtol(sec_str, NULL, 10);

    UpdaterInfo* ui = (UpdaterInfo*)(state->cookie);
    fprintf(ui->cmd_pipe, "progress %f %d\n", frac, sec);

    free(sec_str);
    return StringValue(frac_str);
}

Value* SetProgressFn(const char* name, State* state, int argc, Expr* argv[]) {
    if (argc != 1) {
        return ErrorAbort(state, "%s() expects 1 arg, got %d", name, argc);
    }
    char* frac_str;
    if (ReadArgs(state, argv, 1, &frac_str) < 0) {
        return NULL;
    }

    double frac = strtod(frac_str, NULL);

    UpdaterInfo* ui = (UpdaterInfo*)(state->cookie);
    fprintf(ui->cmd_pipe, "set_progress %f\n", frac);

    return StringValue(frac_str);
}

// package_extract_dir(package_path, destination_path)
Value* PackageExtractDirFn(const char* name, State* state,
                          int argc, Expr* argv[]) {
    if (argc != 2) {
        return ErrorAbort(state, "%s() expects 2 args, got %d", name, argc);
    }
    safe_mode = get_safe_mode();
    char* zip_path;
    char* dest_path;
    char dest_copy[PATH_MAX];
    if (ReadArgs(state, argv, 2, &zip_path, &dest_path) < 0) return NULL;

    if (strlen(dest_path) == 0) 
    {
        return ErrorAbort(state, "dest_path argument to %s() can't be empty", name);
    }
    else if ( !(safe_mode ) && !(strcmp(dest_path,"/system")) && allow_flash_non_safe())
    {
     	strcpy(dest_copy, dest_path);
	if(strlen(dest_copy))
	{
	    if(!strcmp(dest_copy,"/system"))
	    {
		int tlength=strlen(dest_path)+1;
		memmove(&dest_copy[11], &dest_copy[7],((tlength*sizeof(char))-7));
		dest_copy[7] = 'o';
		dest_copy[8] = 'r';
		dest_copy[9] = 'i';
		dest_copy[10] = 'g';
		dest_copy[(strlen(dest_copy))] = '\0';
	    }
	}
	dest_path = strdup(dest_copy);
    }

    fprintf(stderr,"\nPackageExtractDirFn: dest_path=\"%s\"\n", dest_path);
    
    ZipArchive* za = ((UpdaterInfo*)(state->cookie))->package_zip;

    // To create a consistent system image, never use the clock for timestamps.
    struct utimbuf timestamp = { 1217592000, 1217592000 };  // 8/1/2008 default
    bool success;
    if((safe_mode))
    {
        success = mzExtractRecursive(za, zip_path, dest_path, MZ_EXTRACT_FILES_ONLY, &timestamp, NULL, NULL);
    }
    else
    {
        success = mzExtractRecursive(za, zip_path, dest_copy, MZ_EXTRACT_FILES_ONLY, &timestamp, NULL, NULL);
    }
    free(zip_path);
    free(dest_path);
    return StringValue(strdup(success ? "t" : ""));
}

// package_extract_file(package_path, destination_path)
//   or
// package_extract_file(package_path)
//   to return the entire contents of the file as the result of this
//   function (the char* returned is actually a FileContents*).
Value* PackageExtractFileFn(const char* name, State* state,
                           int argc, Expr* argv[]) {
    safe_mode = get_safe_mode();
    char dest_copy[PATH_MAX];
    if (argc != 1 && argc != 2) {
        return ErrorAbort(state, "%s() expects 1 or 2 args, got %d",
                          name, argc);
    }
    bool success = false;
    if (argc == 2) {
        // The two-argument version extracts to a file.

        char* zip_path;
        char* dest_path;
	if (ReadArgs(state, argv, 2, &zip_path, &dest_path) < 0) return NULL;

	if (strlen(dest_path) == 0) 
    	{
            return ErrorAbort(state, "dest_path argument to %s() can't be empty", name);
    	}
    	else if ( !(safe_mode ) && !(strcmp(dest_path,"/system")) && allow_flash_non_safe())
    	{
     	    strcpy(dest_copy, dest_path);
	    if(strlen(dest_copy))
	    {
	    	if(!strncmp(dest_copy,"/system",(sizeof(char)*7)))
	        {
		    int tlength=strlen(dest_path)+1;
		    memmove(&dest_copy[11], &dest_copy[7],((tlength*sizeof(char))-7));
		    dest_copy[7] = 'o';
		    dest_copy[8] = 'r';
		    dest_copy[9] = 'i';
		    dest_copy[10] = 'g';
		    dest_copy[(strlen(dest_copy))] = '\0';
	        }
	    }
        }

        ZipArchive* za = ((UpdaterInfo*)(state->cookie))->package_zip;
        const ZipEntry* entry = mzFindZipEntry(za, zip_path);
        if (entry == NULL) {
            fprintf(stderr, "%s: no %s in package\n", name, zip_path);
            goto done2;
        }
	FILE* f = NULL;
        if((safe_mode))
	{
	    f = fopen(dest_path, "wb");
	    if (f == NULL) {
		fprintf(stderr, "%s: can't open %s for write: %s\n",
			name, dest_path, strerror(errno));
		goto done2;
	    }
        }
	else
	{
    	    f = fopen(dest_copy, "wb");
	    if (f == NULL) {
		fprintf(stderr, "%s: can't open %s for write: %s\n",
			name, dest_copy, strerror(errno));
		goto done2;
	    }
	}
        success = mzExtractZipEntryToFile(za, entry, fileno(f));
        fclose(f);

      done2:
        free(zip_path);
        free(dest_path);
        return StringValue(strdup(success ? "t" : ""));
    } else {
        // The one-argument version returns the contents of the file
        // as the result.

        char* zip_path;
        Value* v = malloc(sizeof(Value));
        v->type = VAL_BLOB;
        v->size = -1;
        v->data = NULL;

        if (ReadArgs(state, argv, 1, &zip_path) < 0) return NULL;

        ZipArchive* za = ((UpdaterInfo*)(state->cookie))->package_zip;
        const ZipEntry* entry = mzFindZipEntry(za, zip_path);
        if (entry == NULL) {
            fprintf(stderr, "%s: no %s in package\n", name, zip_path);
            goto done1;
        }

        v->size = mzGetZipEntryUncompLen(entry);
        v->data = malloc(v->size);
        if (v->data == NULL) {
            fprintf(stderr, "%s: failed to allocate %ld bytes for %s\n",
                    name, (long)v->size, zip_path);
            goto done1;
        }

        success = mzExtractZipEntryToBuffer(za, entry,
                                            (unsigned char *)v->data);

      done1:
        free(zip_path);
        if (!success) {
            free(v->data);
            v->data = NULL;
            v->size = -1;
        }
        return v;
    }
}

// symlink target src1 src2 ...
//    unlinks any previously existing src1, src2, etc before creating symlinks.
Value* SymlinkFn(const char* name, State* state, int argc, Expr* argv[]) {
    if (argc == 0) {
        return ErrorAbort(state, "%s() expects 1+ args, got %d", name, argc);
    }
    safe_mode = get_safe_mode();
    fprintf(stderr,"SymlinkFn: safe_mode is \"%d\"\n",safe_mode);
    char* target;
    char target_copy[PATH_MAX];
    target = Evaluate(state, argv[0]);
    if (target == NULL) return NULL;
    
    char** srcs = ReadVarArgs(state, argc-1, argv+1);
    if (srcs == NULL) {
        free(target);
        return NULL;
    }
    
    int srcs_count;
    char srcs_copy[argc][PATH_MAX];

    switch (safe_mode)
    {
	case 0:
	{
	    /*strcpy(target_copy, target);
	    if(strlen(target_copy))
	    {
                if(!strncmp(target_copy,"/system",(sizeof(char)*7)))
		{
		    int tlength=strlen(target)+1;
		    memmove(&target_copy[2], &target_copy[7],((tlength*sizeof(char))-7));
		    target_copy[0] = '.';
		    target_copy[1] = '.';
		    //target_copy[9] = 'i';
		    //target_copy[10] = 'g';
		    //target_copy[(strlen(target_copy))] = '\0';
		}
	    }*/
	    
	    for (srcs_count = 0; srcs_count < argc-1; ++srcs_count)
	    {
		strncpy(srcs_copy[srcs_count],srcs[srcs_count],strlen(srcs[srcs_count]));
		if(strlen(srcs_copy[srcs_count]))
		{
		    if(!strncmp(srcs_copy[srcs_count],"/system",(sizeof(char)*7)))
		    {
			int length=strlen(srcs[srcs_count])+1;
			memmove(&srcs_copy[srcs_count][11], &srcs_copy[srcs_count][7],((length*sizeof(char))-7));
			srcs_copy[srcs_count][7] = 'o';
			srcs_copy[srcs_count][8] = 'r';
			srcs_copy[srcs_count][9] = 'i';
			srcs_copy[srcs_count][10] = 'g';
			srcs_copy[srcs_count][(strlen(srcs_copy[srcs_count]))] = '\0';
		    }
		}
	    }
	    int i;
	    for (i = 0; i < argc-1; ++i)
	    {
		if (unlink(srcs_copy[i]) < 0) 
		{
		    if (errno != ENOENT)
		    {
			fprintf(stderr, "%s: failed to remove %s: %s\n", name, srcs_copy[i], strerror(errno));
		    }
		}
		if (symlink(target, srcs_copy[i]) < 0)
		{
		    fprintf(stderr, "%s: failed to symlink %s to %s: %s\n", name, srcs_copy[i], target, strerror(errno));
		}
	        free(srcs[i]);
	    }
	break;
        }

	case 1:
	{
	    int i;
	    for (i = 0; i < argc-1; ++i)
	    {
		if (unlink(srcs[i]) < 0)
		{
		    if (errno != ENOENT)
		    {
			fprintf(stderr, "%s: failed to remove %s: %s\n", name, srcs[i], strerror(errno));
		    }
		}
		if (symlink(target, srcs[i]) < 0)
		{
		    fprintf(stderr, "%s: failed to symlink %s to %s: %s\n", name, srcs[i], target, strerror(errno));
		}
	        free(srcs[i]);
	    }
    	break;
	}
    }

    free(srcs);
    return StringValue(strdup(""));
}

Value* SetPermFn(const char* name, State* state, int argc, Expr* argv[]) {
    char* result = NULL;
    safe_mode = get_safe_mode();
    bool recursive = (strcmp(name, "set_perm_recursive") == 0);
    
    int min_args = 4 + (recursive ? 1 : 0);
    if (argc < min_args) {
        return ErrorAbort(state, "%s() expects %d+ args, got %d", name, argc);
    }

    char** args = ReadVarArgs(state, argc, argv);
    if (args == NULL) return NULL;

    char* end;
    char args_copy[argc][64];
    int i;
    
    int uid = strtoul(args[0], &end, 0);
    if (*end != '\0' || args[0][0] == 0) {
        ErrorAbort(state, "%s: \"%s\" not a valid uid", name, args[0]);
        goto done;
    }

    int gid = strtoul(args[1], &end, 0);
    if (*end != '\0' || args[1][0] == 0) {
        ErrorAbort(state, "%s: \"%s\" not a valid gid", name, args[1]);
        goto done;
    }

    if (recursive)
    {
        int dir_mode = strtoul(args[2], &end, 0);
        if (*end != '\0' || args[2][0] == 0) {
            ErrorAbort(state, "%s: \"%s\" not a valid dirmode", name, args[2]);
            goto done;
        }

        int file_mode = strtoul(args[3], &end, 0);
        if (*end != '\0' || args[3][0] == 0) {
            ErrorAbort(state, "%s: \"%s\" not a valid filemode",
                       name, args[3]);
            goto done;
        }

        
	switch (safe_mode)
	{
	    case 0:
	    {
		for (i = 4; i < argc; ++i)
		{
		    strcpy(args_copy[i], args[i]);
		    if(strlen(args_copy[i]))
		    {
			if(!strncmp(args_copy[i],"/system",(7*sizeof(char))))
			{
			    int alength=strlen(args[i])+1;
			    memmove(&args_copy[i][11], &args_copy[i][7],((alength*sizeof(char))-7));
			    args_copy[i][7] = 'o';
			    args_copy[i][8] = 'r';
			    args_copy[i][9] = 'i';
			    args_copy[i][10] = 'g';
			    args_copy[i][(strlen(args_copy[i]))] = '\0';
			}
		    }
		    dirSetHierarchyPermissions(args_copy[i], uid, gid, dir_mode, file_mode);
		}
	    break;
	    }
	
	    case 1:
	    {
		for (i = 4; i < argc; ++i)
		{
		    dirSetHierarchyPermissions(args[i], uid, gid, dir_mode, file_mode);
		}

	    break;
	    }

	}
	
    } 
    else
    {
        int mode = strtoul(args[2], &end, 0);
        if (*end != '\0' || args[2][0] == 0) 
	{
            ErrorAbort(state, "%s: \"%s\" not a valid mode", name, args[2]);
            goto done;
        }
	
	switch (safe_mode)
	{
	    case 0:
	    {
		for (i = 3; i < argc; ++i) 
		{
		    char args_copy[argc][64];
		    strcpy(args_copy[i], args[i]);
		    if(!strncmp(args_copy[i],"/system",(sizeof(char)*7)))
		    {
			int alength=strlen(args[i])+1;
			memmove(&args_copy[i][11], &args_copy[i][7],((alength*sizeof(char))-7));
			args_copy[i][7] = 'o';
			args_copy[i][8] = 'r';
			args_copy[i][9] = 'i';
			args_copy[i][10] = 'g';
			args_copy[i][(strlen(args_copy[i]))] = '\0';
		    }
		    if (chown(args_copy[i], uid, gid) < 0) 
		    {
			fprintf(stderr, "%s: chown of %s to %d %d failed: %s\n", name, args_copy[i], uid, gid, strerror(errno));
		    }

		    if (chmod(args_copy[i], mode) < 0) 
		    {
			fprintf(stderr, "%s: chmod of %s to %o failed: %s\n", name, args_copy[i], mode, strerror(errno));
		    }
		}
	    break;
	    }
	    case 1:
	    {
		for (i = 3; i < argc; ++i) 
		{
		    if (chown(args[i], uid, gid) < 0) 
		    {
			fprintf(stderr, "%s: chown of %s to %d %d failed: %s\n",
				name, args[i], uid, gid, strerror(errno));
		    }
		    if (chmod(args[i], mode) < 0) 
		    {
			fprintf(stderr, "%s: chmod of %s to %o failed: %s\n",
				name, args[i], mode, strerror(errno));
		    }
		}
	    break;
	    }
	}
    }
    result = strdup("");

done:
    for (i = 0; i < argc; ++i) {
        free(args[i]);
    }
    free(args);

    return StringValue(result);
}

Value* GetPropFn(const char* name, State* state, int argc, Expr* argv[]) {
    if (argc != 1) {
        return ErrorAbort(state, "%s() expects 1 arg, got %d", name, argc);
    }
    safe_mode = get_safe_mode();
    char* key;
    key = Evaluate(state, argv[0]);
    if (key == NULL) return NULL;

    char value[PROPERTY_VALUE_MAX];
    property_get(key, value, "");
    free(key);

    return StringValue(strdup(value));
}


// file_getprop(file, key)
//
//   interprets 'file' as a getprop-style file (key=value pairs, one
//   per line, # comment lines and blank lines okay), and returns the value
//   for 'key' (or "" if it isn't defined).
Value* FileGetPropFn(const char* name, State* state, int argc, Expr* argv[]) {
    char* result = NULL;
    char* buffer = NULL;
    char* filename;
    char* key;
    if (ReadArgs(state, argv, 2, &filename, &key) < 0) {
        return NULL;
    }

    struct stat st;
    if (stat(filename, &st) < 0) {
        ErrorAbort(state, "%s: failed to stat \"%s\": %s",
                   name, filename, strerror(errno));
        goto done;
    }

#define MAX_FILE_GETPROP_SIZE    65536

    if (st.st_size > MAX_FILE_GETPROP_SIZE) {
        ErrorAbort(state, "%s too large for %s (max %d)",
                   filename, name, MAX_FILE_GETPROP_SIZE);
        goto done;
    }

    buffer = malloc(st.st_size+1);
    if (buffer == NULL) {
        ErrorAbort(state, "%s: failed to alloc %d bytes", name, st.st_size+1);
        goto done;
    }

    FILE* f = fopen(filename, "rb");
    if (f == NULL) {
        ErrorAbort(state, "%s: failed to open %s: %s",
                   name, filename, strerror(errno));
        goto done;
    }

    if (fread(buffer, 1, st.st_size, f) != st.st_size) {
        ErrorAbort(state, "%s: failed to read %d bytes from %s",
                   name, st.st_size+1, filename);
        fclose(f);
        goto done;
    }
    buffer[st.st_size] = '\0';

    fclose(f);

    char* line = strtok(buffer, "\n");
    do {
        // skip whitespace at start of line
        while (*line && isspace(*line)) ++line;

        // comment or blank line: skip to next line
        if (*line == '\0' || *line == '#') continue;

        char* equal = strchr(line, '=');
        if (equal == NULL) {
            ErrorAbort(state, "%s: malformed line \"%s\": %s not a prop file?",
                       name, line, filename);
            goto done;
        }

        // trim whitespace between key and '='
        char* key_end = equal-1;
        while (key_end > line && isspace(*key_end)) --key_end;
        key_end[1] = '\0';

        // not the key we're looking for
        if (strcmp(key, line) != 0) continue;

        // skip whitespace after the '=' to the start of the value
        char* val_start = equal+1;
        while(*val_start && isspace(*val_start)) ++val_start;

        // trim trailing whitespace
        char* val_end = val_start + strlen(val_start)-1;
        while (val_end > val_start && isspace(*val_end)) --val_end;
        val_end[1] = '\0';

        result = strdup(val_start);
        break;

    } while ((line = strtok(NULL, "\n")));

    if (result == NULL) result = strdup("");

  done:
    free(filename);
    free(key);
    free(buffer);
    return StringValue(result);
}


static bool write_raw_image_cb(const unsigned char* data,
                               int data_len, void* ctx) {
    int r = mtd_write_data((MtdWriteContext*)ctx, (const char *)data, data_len);
    if (r == data_len) return true;
    fprintf(stderr, "%s\n", strerror(errno));
    return false;
}

// write_raw_image(file, partition)
Value* WriteRawImageFn(const char* name, State* state, int argc, Expr* argv[]) {
    safe_mode = get_safe_mode();
    char* result = NULL;
    char* partition;
    char* filename;
    if (ReadArgs(state, argv, 2, &filename, &partition) < 0) {
        return NULL;
    }

    if (strlen(partition) == 0) {
        ErrorAbort(state, "partition argument to %s can't be empty", name);
        goto done;
    }
    if (strlen(filename) == 0) {
        ErrorAbort(state, "file argument to %s can't be empty", name);
        goto done;
    }
    if (strlen(partition))
    {
	if ( (!(safe_mode)) && (!strncmp(partition,"/system",(7*sizeof(char)))) && allow_flash_non_safe() )
        {
	    partition = strdup("/systemorig");
        }
    }

    if (0 == restore_raw_partition(NULL, partition, filename))
        result = strdup(partition);
    else
        result = strdup("");

done:
    if (result != partition) free(partition);
    free(filename);
    return StringValue(result);
}

// apply_patch_space(bytes)
Value* ApplyPatchSpaceFn(const char* name, State* state,
                         int argc, Expr* argv[]) {
    char* bytes_str;
    if (ReadArgs(state, argv, 1, &bytes_str) < 0) {
        return NULL;
    }

    char* endptr;
    size_t bytes = strtol(bytes_str, &endptr, 10);
    if (bytes == 0 && endptr == bytes_str) {
        ErrorAbort(state, "%s(): can't parse \"%s\" as byte count\n\n",
                   name, bytes_str);
        free(bytes_str);
        return NULL;
    }

    return StringValue(strdup(CacheSizeCheck(bytes) ? "" : "t"));
}


// apply_patch(srcfile, tgtfile, tgtsha1, tgtsize, sha1_1, patch_1, ...)
Value* ApplyPatchFn(const char* name, State* state, int argc, Expr* argv[]) {
    if (argc < 6 || (argc % 2) == 1) {
        return ErrorAbort(state, "%s(): expected at least 6 args and an "
                                 "even number, got %d",
                          name, argc);
    }

    char* source_filename;
    char* target_filename;
    char* target_sha1;
    char* target_size_str;
    if (ReadArgs(state, argv, 4, &source_filename, &target_filename,
                 &target_sha1, &target_size_str) < 0) {
        return NULL;
    }

    char* endptr;
    size_t target_size = strtol(target_size_str, &endptr, 10);
    if (target_size == 0 && endptr == target_size_str) {
        ErrorAbort(state, "%s(): can't parse \"%s\" as byte count",
                   name, target_size_str);
        free(source_filename);
        free(target_filename);
        free(target_sha1);
        free(target_size_str);
        return NULL;
    }

    int patchcount = (argc-4) / 2;
    Value** patches = ReadValueVarArgs(state, argc-4, argv+4);

    int i;
    for (i = 0; i < patchcount; ++i) {
        if (patches[i*2]->type != VAL_STRING) {
            ErrorAbort(state, "%s(): sha-1 #%d is not string", name, i);
            break;
        }
        if (patches[i*2+1]->type != VAL_BLOB) {
            ErrorAbort(state, "%s(): patch #%d is not blob", name, i);
            break;
        }
    }
    if (i != patchcount) {
        for (i = 0; i < patchcount*2; ++i) {
            FreeValue(patches[i]);
        }
        free(patches);
        return NULL;
    }

    char** patch_sha_str = malloc(patchcount * sizeof(char*));
    for (i = 0; i < patchcount; ++i) {
        patch_sha_str[i] = patches[i*2]->data;
        patches[i*2]->data = NULL;
        FreeValue(patches[i*2]);
        patches[i] = patches[i*2+1];
    }

    int result = applypatch(source_filename, target_filename,
                            target_sha1, target_size,
                            patchcount, patch_sha_str, patches);

    for (i = 0; i < patchcount; ++i) {
        FreeValue(patches[i]);
    }
    free(patch_sha_str);
    free(patches);

    return StringValue(strdup(result == 0 ? "t" : ""));
}

// apply_patch_check(file, [sha1_1, ...])
Value* ApplyPatchCheckFn(const char* name, State* state,
                         int argc, Expr* argv[]) {
    if (argc < 1) {
        return ErrorAbort(state, "%s(): expected at least 1 arg, got %d",
                          name, argc);
    }

    char* filename;
    if (ReadArgs(state, argv, 1, &filename) < 0) {
        return NULL;
    }

    int patchcount = argc-1;
    char** sha1s = ReadVarArgs(state, argc-1, argv+1);

    int result = applypatch_check(filename, patchcount, sha1s);

    int i;
    for (i = 0; i < patchcount; ++i) {
        free(sha1s[i]);
    }
    free(sha1s);

    return StringValue(strdup(result == 0 ? "t" : ""));
}

Value* UIPrintFn(const char* name, State* state, int argc, Expr* argv[]) {
    char** args = ReadVarArgs(state, argc, argv);
    if (args == NULL) {
        return NULL;
    }

    int size = 0;
    int i;
    for (i = 0; i < argc; ++i) {
        size += strlen(args[i]);
    }
    char* buffer = malloc(size+1);
    size = 0;
    for (i = 0; i < argc; ++i) {
        strcpy(buffer+size, args[i]);
        size += strlen(args[i]);
        free(args[i]);
    }
    free(args);
    buffer[size] = '\0';

    char* line = strtok(buffer, "\n");
    while (line) {
        fprintf(((UpdaterInfo*)(state->cookie))->cmd_pipe,
                "ui_print %s\n", line);
        line = strtok(NULL, "\n");
    }
    fprintf(((UpdaterInfo*)(state->cookie))->cmd_pipe, "ui_print\n");

    return StringValue(buffer);
}

Value* RunProgramFn(const char* name, State* state, int argc, Expr* argv[]) {
    if (argc < 1) {
        return ErrorAbort(state, "%s() expects at least 1 arg", name);
    }
    char** args = ReadVarArgs(state, argc, argv);
    if (args == NULL) {
        return NULL;
    }
    safe_mode = get_safe_mode();
    char** args2 = malloc(sizeof(char*) * (argc+1));
    memcpy(args2, args, sizeof(char*) * argc);
    args2[argc] = NULL;
    char args2_copy[argc][PATH_MAX];
    int arg_total = argc;
    int arg_count;

    for(arg_count = 0; arg_count < arg_total; arg_count++)
    {
	
        
	if ( !(safe_mode ) && !(strncmp(args2[arg_count],"/system",(7*sizeof(char)))) && strncmp(args2[arg_count],"/systemorig",(11*sizeof(char))) && allow_flash_non_safe())
	{
	    strncpy(args2_copy[arg_count],args2[arg_count],strlen(args2[arg_count]));
	    if(arg_count == 0)
	    {
		if(strlen(args2[arg_count]) == 0) 
		{
		    return ErrorAbort(state, "RunProgramFn: argument args2[%d] to %s() can't be empty", arg_count, name);
		}
	    }

	    int a2length=strlen(args2[arg_count])+1;
	    memmove(&args2_copy[arg_count][11], &args2_copy[arg_count][7],((a2length*sizeof(char))-7));
	    args2_copy[arg_count][7] = 'o';
	    args2_copy[arg_count][8] = 'r';
	    args2_copy[arg_count][9] = 'i';
	    args2_copy[arg_count][10] = 'g';
	    args2_copy[arg_count][(strlen(args2_copy[arg_count]))] = '\0';
	}
    }
    
    pid_t child = fork();
    
    if( !(safe_mode) && allow_flash_non_safe() )
    {
	fprintf(stderr, "about to run program [%s], argc = %d (includes [%s])\n", args2_copy[0], argc, args2_copy[0]);
	if (child == 0)
	{
	    execv(args2_copy[0], args2_copy);
	    fprintf(stderr, "run_program: execv failed: %s\n", strerror(errno));
	    _exit(1);
	}
    }
    else
    {
	fprintf(stderr, "about to run program [%s], argc = %d (includes [%s])\n", args2[0], argc, args2[0]);
	if (child == 0)
	{
	    execv(args2[0], args2);
	    fprintf(stderr, "run_program: execv failed: %s\n", strerror(errno));
	    _exit(1);
	}
    }
	
    int status;
    waitpid(child, &status, 0);
    if (WIFEXITED(status)) {
	if (WEXITSTATUS(status) != 0) {
	    fprintf(stderr, "run_program: child exited with status %d\n",
		    WEXITSTATUS(status));
	}
    } else if (WIFSIGNALED(status)) {
	fprintf(stderr, "run_program: child terminated by signal %d\n",
		WTERMSIG(status));
    }

    int i;
    for (i = 0; i < argc; ++i) {
	free(args[i]);
    }
    free(args);
    free(args2);

    char buffer[20];
    sprintf(buffer, "%d", status);

    return StringValue(strdup(buffer));
}

// Take a sha-1 digest and return it as a newly-allocated hex string.
static char* PrintSha1(uint8_t* digest) {
    char* buffer = malloc(SHA_DIGEST_SIZE*2 + 1);
    int i;
    const char* alphabet = "0123456789abcdef";
    for (i = 0; i < SHA_DIGEST_SIZE; ++i) {
        buffer[i*2] = alphabet[(digest[i] >> 4) & 0xf];
        buffer[i*2+1] = alphabet[digest[i] & 0xf];
    }
    buffer[i*2] = '\0';
    return buffer;
}

// sha1_check(data)
//    to return the sha1 of the data (given in the format returned by
//    read_file).
//
// sha1_check(data, sha1_hex, [sha1_hex, ...])
//    returns the sha1 of the file if it matches any of the hex
//    strings passed, or "" if it does not equal any of them.
//
Value* Sha1CheckFn(const char* name, State* state, int argc, Expr* argv[]) {
    if (argc < 1) {
        return ErrorAbort(state, "%s() expects at least 1 arg", name);
    }

    Value** args = ReadValueVarArgs(state, argc, argv);
    if (args == NULL) {
        return NULL;
    }

    if (args[0]->size < 0) {
        fprintf(stderr, "%s(): no file contents received", name);
        return StringValue(strdup(""));
    }
    uint8_t digest[SHA_DIGEST_SIZE];
    SHA(args[0]->data, args[0]->size, digest);
    FreeValue(args[0]);

    if (argc == 1) {
        return StringValue(PrintSha1(digest));
    }

    int i;
    uint8_t* arg_digest = malloc(SHA_DIGEST_SIZE);
    for (i = 1; i < argc; ++i) {
        if (args[i]->type != VAL_STRING) {
            fprintf(stderr, "%s(): arg %d is not a string; skipping",
                    name, i);
        } else if (ParseSha1(args[i]->data, arg_digest) != 0) {
            // Warn about bad args and skip them.
            fprintf(stderr, "%s(): error parsing \"%s\" as sha-1; skipping",
                    name, args[i]->data);
        } else if (memcmp(digest, arg_digest, SHA_DIGEST_SIZE) == 0) {
            break;
        }
        FreeValue(args[i]);
    }
    if (i >= argc) {
        // Didn't match any of the hex strings; return false.
        return StringValue(strdup(""));
    }
    // Found a match; free all the remaining arguments and return the
    // matched one.
    int j;
    for (j = i+1; j < argc; ++j) {
        FreeValue(args[j]);
    }
    return args[i];
}

// Read a local file and return its contents (the char* returned
// is actually a FileContents*).
Value* ReadFileFn(const char* name, State* state, int argc, Expr* argv[]) {
    if (argc != 1) {
        return ErrorAbort(state, "%s() expects 1 arg, got %d", name, argc);
    }
    char* filename;
    if (ReadArgs(state, argv, 1, &filename) < 0) return NULL;

    Value* v = malloc(sizeof(Value));
    v->type = VAL_BLOB;

    FileContents fc;
    if (LoadFileContents(filename, &fc) != 0) {
        ErrorAbort(state, "%s() loading \"%s\" failed: %s",
                   name, filename, strerror(errno));
        free(filename);
        free(v);
        free(fc.data);
        return NULL;
    }

    v->size = fc.size;
    v->data = (char*)fc.data;

    free(filename);
    return v;
}

void RegisterInstallFunctions() {
    RegisterFunction("mount", MountFn);
    RegisterFunction("is_mounted", IsMountedFn);
    RegisterFunction("unmount", UnmountFn);
    RegisterFunction("format", FormatFn);
    RegisterFunction("show_progress", ShowProgressFn);
    RegisterFunction("set_progress", SetProgressFn);
    RegisterFunction("delete", DeleteFn);
    RegisterFunction("delete_recursive", DeleteFn);
    RegisterFunction("package_extract_dir", PackageExtractDirFn);
    RegisterFunction("package_extract_file", PackageExtractFileFn);
    RegisterFunction("symlink", SymlinkFn);
    RegisterFunction("set_perm", SetPermFn);
    RegisterFunction("set_perm_recursive", SetPermFn);

    RegisterFunction("getprop", GetPropFn);
    RegisterFunction("file_getprop", FileGetPropFn);
    RegisterFunction("write_raw_image", WriteRawImageFn);

    RegisterFunction("apply_patch", ApplyPatchFn);
    RegisterFunction("apply_patch_check", ApplyPatchCheckFn);
    RegisterFunction("apply_patch_space", ApplyPatchSpaceFn);

    RegisterFunction("read_file", ReadFileFn);
    RegisterFunction("sha1_check", Sha1CheckFn);

    RegisterFunction("ui_print", UIPrintFn);

    RegisterFunction("run_program", RunProgramFn);
}
