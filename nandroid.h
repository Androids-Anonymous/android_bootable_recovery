#ifndef NANDROID_H
#define NANDROID_H

#define CACHE_RESTORE_TIME 13
#define DATA_RESTORE_TIME 154
#define MD5_RESTORE_TIME 67
#define SYSTEM_RESTORE_TIME 115
#define SYSTEMORIG_RESTORE_TIME 94

int nandroid_main(int argc, char** argv);
int nandroid_backup_partition(const char* backup_path, const char* root);
int nandroid_backup(const char* backup_path, const char* sdcard_path, int skip_webtop, int skip_origsys);
int nandroid_restore_partition(const char* backup_path, const char* root);
int nandroid_restore(const char* backup_path, int restore_system, int restore_data, int restore_cache, int restore_systemorig);

#endif
