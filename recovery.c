/*
 * Copyright (C) 2007 The Android Open Source Project
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
#include <stdio.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reboot.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <termios.h> 
#include <stdio.h>

#include "bootloader.h"
#include "commands.h"
#include "common.h"
#include "cutils/properties.h"
#include "firmware.h"
#include "install.h"
#include "minui/minui.h"
#include "minzip/DirUtil.h"
#include "roots.h"

static const struct option OPTIONS[] = {
    { "send_intent", required_argument, NULL, 's' },
    { "update_package", required_argument, NULL, 'u' },
    { "wipe_data", no_argument, NULL, 'w' },
    { "wipe_cache", no_argument, NULL, 'c' },
};

static const char *COMMAND_FILE = "CACHE:recovery/command";
static const char *INTENT_FILE = "CACHE:recovery/intent";
static const char *LOG_FILE = "CACHE:recovery/log";
static const char *SDCARD_PACKAGE_FILE = "SDCARD:update.zip";
static const char *SDCARD_PATH = "SDCARD:";
#define SDCARD_PATH_LENGTH 7
static const char *TEMPORARY_LOG_FILE = "/tmp/recovery.log";

static int usb_ms = 0;

/*
 * The recovery tool communicates with the main system through /cache files.
 *   /cache/recovery/command - INPUT - command line for tool, one arg per line
 *   /cache/recovery/log - OUTPUT - combined log file from recovery run(s)
 *   /cache/recovery/intent - OUTPUT - intent that was passed in
 *
 * The arguments which may be supplied in the recovery.command file:
 *   --send_intent=anystring - write the text out to recovery.intent
 *   --update_package=root:path - verify install an OTA package file
 *   --wipe_data - erase user data (and cache), then reboot
 *   --wipe_cache - wipe cache (but not user data), then reboot
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
 * 5. erase_root() reformats /data
 * 6. erase_root() reformats /cache
 * 7. finish_recovery() erases BCB
 *    -- after this, rebooting will restart the main system --
 * 8. main() calls reboot() to boot main system
 *
 * OTA INSTALL
 * 1. main system downloads OTA package to /cache/some-filename.zip
 * 2. main system writes "--update_package=CACHE:some-filename.zip"
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
 *    8f. erase_root() reformats /cache
 *    8g. finish_recovery() erases BCB
 *        -- after this, rebooting will (try to) restart the main system --
 * 9. main() calls reboot() to boot main system
 */

static const int MAX_ARG_LENGTH = 4096;
static const int MAX_ARGS = 100;

static int do_reboot = 1;

// open a file given in root:path format, mounting partitions as necessary
static FILE*
        fopen_root_path(const char *root_path, const char *mode) {
    if (ensure_root_path_mounted(root_path) != 0) {
        LOGE("Can't mount %s\n", root_path);
        return NULL;
    }

    char path[PATH_MAX] = "";
    if (translate_root_path(root_path, path, sizeof(path)) == NULL) {
        LOGE("Bad path %s\n", root_path);
        return NULL;
    }

    // When writing, try to create the containing directory, if necessary.
    // Use generous permissions, the system (init.rc) will reset them.
    if (strchr("wa", mode[0])) dirCreateHierarchy(path, 0777, NULL, 1);

    FILE *fp = fopen(path, mode);
    return fp;
}

// close a file, log an error if the error indicator is set
static void
        check_and_fclose(FILE *fp, const char *name) {
    fflush(fp);
    if (ferror(fp)) LOGE("Error in %s\n(%s)\n", name, strerror(errno));
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
    get_bootloader_message(&boot);  // this may fail, leaving a zeroed structure

    if (boot.command[0] != 0 && boot.command[0] != 255) {
        LOGI("Boot command: %.*s\n", sizeof(boot.command), boot.command);
    }

    if (boot.status[0] != 0 && boot.status[0] != 255) {
        LOGI("Boot status: %.*s\n", sizeof(boot.status), boot.status);
    }

    // --- if arguments weren't supplied, look in the bootloader control block
    if (*argc <= 1) {
        boot.recovery[sizeof(boot.recovery) - 1] = '\0';  // Ensure termination
        const char *arg = strtok(boot.recovery, "\n");
        if (arg != NULL && !strcmp(arg, "recovery")) {
            *argv = (char **) malloc(sizeof(char *) * MAX_ARGS);
            (*argv)[0] = strdup(arg);
            for (*argc = 1; *argc < MAX_ARGS; ++*argc) {
                if ((arg = strtok(NULL, "\n")) == NULL) break;
                (*argv)[*argc] = strdup(arg);
            }
            LOGI("Got arguments from boot message\n");
        } else if (boot.recovery[0] != 0 && boot.recovery[0] != 255) {
            LOGE("Bad boot message\n\"%.20s\"\n", boot.recovery);
        }
    }

    // --- if that doesn't work, try the command file
    if (*argc <= 1) {
        FILE *fp = fopen_root_path(COMMAND_FILE, "r");
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
            LOGI("Got arguments from %s\n", COMMAND_FILE);
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
    set_bootloader_message(&boot);
}


// clear the recovery command and prepare to boot a (hopefully working) system,
// copy our log file to cache as well (for the system to read), and
// record any intent we were asked to communicate back to the system.
// this function is idempotent: call it as many times as you like.
static void
        finish_recovery(const char *send_intent)
{
    // By this point, we're ready to return to the main system...
    if (send_intent != NULL) {
        FILE *fp = fopen_root_path(INTENT_FILE, "w");
        if (fp == NULL) {
            LOGE("Can't open %s\n", INTENT_FILE);
        } else {
            fputs(send_intent, fp);
            check_and_fclose(fp, INTENT_FILE);
        }
    }

    // Copy logs to cache so the system can find out what happened.
    FILE *log = fopen_root_path(LOG_FILE, "a");
    if (log == NULL) {
        LOGE("Can't open %s\n", LOG_FILE);
    } else {
        FILE *tmplog = fopen(TEMPORARY_LOG_FILE, "r");
        if (tmplog == NULL) {
            LOGE("Can't open %s\n", TEMPORARY_LOG_FILE);
        } else {
            static long tmplog_offset = 0;
            fseek(tmplog, tmplog_offset, SEEK_SET);  // Since last write
            char buf[4096];
            while (fgets(buf, sizeof(buf), tmplog)) fputs(buf, log);
            tmplog_offset = ftell(tmplog);
            check_and_fclose(tmplog, TEMPORARY_LOG_FILE);
        }
        check_and_fclose(log, LOG_FILE);
    }

    // Reset the bootloader message to revert to a normal main system boot.
    struct bootloader_message boot;
    memset(&boot, 0, sizeof(boot));
    set_bootloader_message(&boot);

    // Remove the command file, so recovery won't repeat indefinitely.
    char path[PATH_MAX] = "";
    if (ensure_root_path_mounted(COMMAND_FILE) != 0 ||
        translate_root_path(COMMAND_FILE, path, sizeof(path)) == NULL ||
        (unlink(path) && errno != ENOENT)) {
        LOGW("Can't unlink %s\n", COMMAND_FILE);
    }

    sync();  // For good measure.
}

#define TEST_AMEND 0
#if TEST_AMEND
static void
        test_amend()
{
    extern int test_symtab(void);
    extern int test_cmd_fn(void);
    int ret;
    LOGD("Testing symtab...\n");
    ret = test_symtab();
    LOGD("  returned %d\n", ret);
    LOGD("Testing cmd_fn...\n");
    ret = test_cmd_fn();
    LOGD("  returned %d\n", ret);
}
#endif  // TEST_AMEND

static int
        erase_root(const char *root)
{
    ui_set_background(BACKGROUND_ICON_INSTALLING);
    ui_show_indeterminate_progress();
    ui_print("Formatting %s..", root);
    return format_root_device(root);
}

static void
        choose_update_file()
{
    static char* headers[] = { "Choose update ZIP file",
                               "",
                               "Use Up/Down keys to highlight;",
                               "click OK to select.",
                               "",
                               NULL };

    char path[PATH_MAX] = "";
    DIR *dir;
    struct dirent *de;
    char **files;
    int total = 0;
    int i;

    if (ensure_root_path_mounted(SDCARD_PATH) != 0) {
        LOGE("Can't mount %s\n", SDCARD_PATH);
        return;
    }

    if (translate_root_path(SDCARD_PATH, path, sizeof(path)) == NULL) {
        LOGE("Bad path %s", path);
        return;
    }

    dir = opendir(path);
    if (dir == NULL) {
        LOGE("Couldn't open directory %s", path);
        return;
    }

    /* count how many files we're looking at */
    while ((de = readdir(dir)) != NULL) {
        char *extension = strrchr(de->d_name, '.');
        if (extension == NULL || de->d_name[0] == '.') {
            continue;
        } else if (!strcasecmp(extension, ".zip")) {
            total++;
        }
    }

    /* allocate the array for the file list menu */
    files = (char **) malloc((total + 1) * sizeof(*files));
    files[total] = NULL;

    /* set it up for the second pass */
    rewinddir(dir);

    /* put the names in the array for the menu */
    i = 0;
    while ((de = readdir(dir)) != NULL) {
        char *extension = strrchr(de->d_name, '.');
        if (extension == NULL || de->d_name[0] == '.') {
            continue;
        } else if (!strcasecmp(extension, ".zip")) {
            files[i] = (char *) malloc(SDCARD_PATH_LENGTH + strlen(de->d_name) + 1);
            strcpy(files[i], SDCARD_PATH);
            strcat(files[i], de->d_name);
            i++;
        }
    }

    /* close directory handle */
    if (closedir(dir) < 0) {
        LOGE("Failure closing directory %s", path);
        goto out;
    }

    ui_start_menu(headers, files);
    int selected = 0;
    int chosen_item = -1;

    finish_recovery(NULL);
    ui_reset_progress();
    for (;;) {
        int key = ui_wait_key();
        int visible = ui_text_visible();

        if (key == KEY_DREAM_BACK) {
            break;
        } else if ((key == KEY_DOWN || key == KEY_VOLUMEDOWN || key == KEY_I5700_DOWN) && visible) {
            ++selected;
            selected = ui_menu_select(selected);
        } else if ((key == KEY_UP || key == KEY_VOLUMEUP || key == KEY_I5700_UP) && visible) {
            --selected;
            selected = ui_menu_select(selected);
        } else if ((key == BTN_MOUSE || key == KEY_I5700_CENTER) && visible) {
            chosen_item = selected;
        }

        if (chosen_item >= 0) {
            // turn off the menu, letting ui_print() to scroll output
            // on the screen.
            ui_end_menu();

            ui_print("\n-- Installing new image!");
            ui_print("\n-- Press HOME to confirm, or");
            ui_print("\n-- any other key to abort..");
            int confirm_apply = ui_wait_key();
            if (confirm_apply == KEY_DREAM_HOME) {
                ui_print("\nInstall from sdcard...\n");
                int status = install_package(files[chosen_item]);
                if (status != INSTALL_SUCCESS) {
                    ui_set_background(BACKGROUND_ICON_ERROR);
                    ui_print("Installation aborted.\n");
                } else if (!ui_text_visible()) {
                    break;  // reboot if logs aren't visible
                } else {
                    if (firmware_update_pending()) {
                        ui_print("\nReboot via home+back or menu\n"
                                 "to complete installation.\n");
                    } else {
                        ui_print("\nInstall from sdcard complete.\n");
                    }
                }
            } else {
                ui_print("\nInstallation aborted.\n");
            }
            if (!ui_text_visible()) break;
            break;
        }
    }

    out:

    for (i = 0; i < total; i++) {
        free(files[i]);
    }
    free(files);
}

static int
        choose_tar_file(char* sfpath)
{
    static char* headers[] = { "Choose backup TAR file",
                               "",
                               "Use Up/Down keys to highlight;",
                               "click OK to select.",
                               "",
                               NULL };

    char path[PATH_MAX] = "";
    DIR *dir;
    struct dirent *de;
    char **files;
    int total = 0;
    int retval = 1;
    int i;

    if (ensure_root_path_mounted(SDCARD_PATH) != 0) {
        LOGE("Can't mount %s\n", SDCARD_PATH);
        return 1;
    }

    if (translate_root_path(SDCARD_PATH, path, sizeof(path)) == NULL) {
        LOGE("Bad path %s", path);
        return 2;
    }

    strcat(path, "samdroid/");

    dir = opendir(path);
    if (dir == NULL) {
        LOGE("Couldn't open directory %s", path);
        return 3;
    }

    /* count how many files we're looking at */
    while ((de = readdir(dir)) != NULL) {
        char *extension = strrchr(de->d_name, '.');
        if (extension == NULL || de->d_name[0] == '.') {
            continue;
        } else if (!strcasecmp(extension, ".tar")) {
            total++;
        }
    }

    /* allocate the array for the file list menu */
    files = (char **) malloc((total + 1) * sizeof(*files));
    files[total] = NULL;

    /* set it up for the second pass */
    rewinddir(dir);

    /* put the names in the array for the menu */
    i = 0;
    while ((de = readdir(dir)) != NULL) {
        char *extension = strrchr(de->d_name, '.');
        if (extension == NULL || de->d_name[0] == '.') {
            continue;
        } else if (!strcasecmp(extension, ".tar")) {
            files[i] = (char *) malloc(SDCARD_PATH_LENGTH + strlen(de->d_name) + 1);
            //strcpy(files[i], SDCARD_PATH);
            //strcat(files[i], de->d_name);
            strcpy(files[i], de->d_name);
            i++;
        }
    }

    /* close directory handle */
    if (closedir(dir) < 0) {
        LOGE("Failure closing directory %s", path);
        goto out;
    }

    ui_start_menu(headers, files);
    int selected = 0;
    int chosen_item = -1;

    finish_recovery(NULL);
    ui_reset_progress();
    for (;;) {
        int key = ui_wait_key();
        int visible = ui_text_visible();

        if (key == KEY_DREAM_BACK) {
            break;
        } else if ((key == KEY_DOWN || key == KEY_VOLUMEDOWN || key == KEY_I5700_DOWN) && visible) {
            ++selected;
            selected = ui_menu_select(selected);
        } else if ((key == KEY_UP || key == KEY_VOLUMEUP || key == KEY_I5700_UP) && visible) {
            --selected;
            selected = ui_menu_select(selected);
        } else if ((key == BTN_MOUSE || key == KEY_I5700_CENTER) && visible) {
            chosen_item = selected;
        }

        if (chosen_item >= 0) {
            // turn off the menu, letting ui_print() to scroll output
            // on the screen.
            ui_end_menu();
            strcpy(sfpath, files[chosen_item]);
            retval = 0;
            break;
        }
    }

    out:

    for (i = 0; i < total; i++) {
        free(files[i]);
    }
    free(files);
    return retval;
}

static void
        choose_wipe_type()
{
    static char* headers[] = { 	"Choose what you want to wipe?"
                                "",
                                "Use Up/Down and OK to select",
                                "",
                                NULL };

#define WTYPE_BACK	 		0
#define WTYPE_DATA_CACHE 	1
#define WTYPE_CACHE	 		2
#define WTYPE_DELVIK_CACHE	3

    static char* items[] = { 	"Back to main menu",
                                "Wipe data/cache (factory reset)",
                                "Wipe cache",
                                "Wipe dalvik-cache",
                                NULL };


    ui_start_menu(headers, items);
    int selected = 0;
    int chosen_item = -1;

    finish_recovery(NULL);
    ui_reset_progress();
    for (;;) {
        int key = ui_wait_key();
        int visible = ui_text_visible();


        if (key == KEY_DREAM_BACK) {
            break;
        } else if ((key == KEY_DOWN || key == KEY_VOLUMEDOWN || key == KEY_I5700_DOWN) && visible) {
            ++selected;
            selected = ui_menu_select(selected);
        } else if ((key == KEY_UP || key == KEY_VOLUMEUP || key == KEY_I5700_UP) && visible) {
            --selected;
            selected = ui_menu_select(selected);
        } else if ((key == BTN_MOUSE || key == KEY_I5700_CENTER) && visible) {
            chosen_item = selected;
        }


        if (chosen_item >= 0) {
            if (chosen_item == WTYPE_BACK) break;

            // turn off the menu, letting ui_print() to scroll output
            // on the screen.
            ui_end_menu();

            ui_print("\n-- This will ERASE your data!");
            ui_print("\n-- Press HOME to confirm, or");
            ui_print("\n-- any other key to abort..");
            int confirm_wipe = ui_wait_key();
            if (confirm_wipe == KEY_DREAM_HOME) {
              	ui_print("\nWiping data...\n");
                switch (chosen_item) {
                case WTYPE_DATA_CACHE:
                    erase_root("DATA:");
                case WTYPE_CACHE:
                    erase_root("CACHE:");
                    ui_print("Data wipe complete.\n");
                    break;
                case WTYPE_DELVIK_CACHE: {
                        if (ensure_root_path_mounted("DATA:") != 0) {
                            ui_print("Can't mount DATA\n");
                        } else {
                            ui_print("Formatting DATA:dalvik-cache..");
                            pid_t pid = fork();
                            if (pid == 0) {
                                char *args[] = {"/xbin/rm", "-r", "/data/dalvik-cache", NULL};
                                execv("/xbin/rm", args);
                                fprintf(stderr, "E:Can't wipe dalvik-cache\n(%s)\n", strerror(errno));
    	                        _exit(-1);
                            }

    	                    int status;

                            while (waitpid(pid, &status, WNOHANG) == 0) {
                                ui_print(".");
                                sleep(1);
    	                    }
                            ui_print("\n");

            	            if (!WIFEXITED(status) || (WEXITSTATUS(status) != 0)) {
                                ui_print("Error wiping dalvik-cache.\n\n");
                            } else {
                                ui_print("Data wipe complete.\n");
                            }
                        }
                    }
                    break;
                }
            } else {
                ui_print("\nData wipe aborted.\n");
            }

            if (!ui_text_visible()) break;
            break;
        }

	
    }
}

int
        get_selected_item(char** headers, char** items)
{
    ui_start_menu(headers, items);
    int selected = 0;
    int chosen_item = -1;

    finish_recovery(NULL);
    ui_reset_progress();
    for (;;) {
        int key = ui_wait_key();
        int visible = ui_text_visible();

        if (key == KEY_DREAM_BACK) {
            chosen_item = 0;
            break;
        } else if ((key == KEY_DOWN || key == KEY_VOLUMEDOWN || key == KEY_I5700_DOWN) && visible) {
            ++selected;
            selected = ui_menu_select(selected);
        } else if ((key == KEY_UP || key == KEY_VOLUMEUP || key == KEY_I5700_UP) && visible) {
            --selected;
            selected = ui_menu_select(selected);
        } else if ((key == BTN_MOUSE || key == KEY_I5700_CENTER) && visible) {
            chosen_item = selected;
            break;
        }
    }
    ui_end_menu();
    return chosen_item;
}

static void
        choose_mount_type()
{
    static char* headers[] = { 	"Choose what you want to mount?"
                                "",
                                "Use Up/Down and OK to select",
                                "",
                                NULL };

#define MNTTYPE_BACK		0
#define MNTTYPE_SYSTEM	 	1
#define MNTTYPE_DATA	 	2
#define MNTTYPE_SDCARD	 	3
#define MNTTYPE_USB_MS	 	4

    static char* partition[] = { "SYSTEM:", "DATA:", "SDCARD:" };

    char** items = (char**) malloc(7 * sizeof(char*));

    for (;;) {

        items[0] = "Back to main menu";
        items[1] = !is_root_path_mounted("SYSTEM:") ? "Mount /system" : "Unmount /system";
        items[2] = !is_root_path_mounted("DATA:") ? "Mount /data" : "Unmount /data";
        items[3] = !is_root_path_mounted("SDCARD:") ? "Mount /sdcard" : "Unmount /sdcard";
        items[4] = !usb_ms ? "Enable USB Mass Storage" : "Disable USB Mass Storage";
        items[5] = NULL;

        int chosen_item = get_selected_item(headers, items);

        if (chosen_item >= 0) {
            if (chosen_item == MNTTYPE_BACK) break;

            // turn off the menu, letting ui_print() to scroll output
            // on the screen.
            switch (chosen_item) {
            case MNTTYPE_SYSTEM:
            case MNTTYPE_DATA:
            case MNTTYPE_SDCARD:
                if (is_root_path_mounted(partition[chosen_item-1])) {
                    if (!ensure_root_path_unmounted(partition[chosen_item-1])) {
                        ui_print("\nUnmounted %s", partition[chosen_item-1]);
                    }
                } else {
                    if (!ensure_root_path_mounted(partition[chosen_item-1])) {
                        ui_print("\nMounted %s", partition[chosen_item-1]);
                    }
                }
                break;
				case MNTTYPE_USB_MS:
                if (usb_ms) {
                    system("echo > /sys/devices/platform/s3c6410-usbgadget/gadget/lun0/file");
                } else {
                    system("echo /dev/block/mmcblk0p1 > /sys/devices/platform/s3c6410-usbgadget/gadget/lun0/file");
                }
                usb_ms = !usb_ms;
                break;
            }
        }
    }
}



static void
        choose_backup_type()
{
    static char* headers[] = { 	"Choose what you want to backup?"
                                "",
                                "Use Up/Down and OK to select",
                                "",
                                NULL };

#define BRTYPE_BACK			0
#define BRTYPE_B_SYS		1
#define BRTYPE_B_DATA	 	2
#define BRTYPE_HL1		 	3
#define BRTYPE_RESTORE	 	4
#define BRTYPE_REST_FORMAT 	5

    char st[255];
    static char* backup_parts[] = { "/system", "/data"};
    static char* backup_file[] = { "Sys", "Data"};

    static char* items[] = { 	"Back to main menu",
                                "TAR backup system",
                                "TAR backup data",
                                "    -------",
                                "TAR restore",
                                "TAR restore (+ format)",
                                NULL };


    for (;;) {

        int chosen_item = get_selected_item(headers, items);

        if (chosen_item >= BRTYPE_RESTORE) {
            char sfpath[255];
            if (choose_tar_file(st) == 0) {
                ui_print("\n-- Press HOME to confirm, or");
    	        ui_print("\n-- any other key to abort..");
                if (ui_wait_key() == KEY_DREAM_HOME) {
                    switch (chosen_item) {
                    case BRTYPE_REST_FORMAT:
                        ui_print("\nFormating ");
                        if (strstr(st, "_Sys.")) {
                            if (!ensure_root_path_unmounted("SYSTEM:")) {
                                ui_print("/system");
                                if (!format_root_device("SYSTEM:")) ui_print("ok");
                            }
                        }
                        if (strstr(st, "_Data.")) {
                            if (!ensure_root_path_unmounted("DATA:")) {
                                ui_print("/data");
                                if (!format_root_device("DATA:")) ui_print("ok");
                            }
                        }
						case BRTYPE_RESTORE:
                        strcpy(sfpath, "/sdcard/samdroid/");
                        strcat(sfpath, st);

                        ui_print("\nMount ");
                        if (strstr(st, "_Sys.")) {
                            ui_print("/system");
                            if (ensure_root_path_mounted("SYSTEM:")) { ui_print("\nError mount /system\n"); return; }
                        }
                        if (strstr(st, "_Data.")) {
                            ui_print("/data");
                            if (ensure_root_path_mounted("DATA:")) { ui_print("\nError mount /data\n"); return; }
                        }

                        ui_print("\nRestoring..");

                        pid_t pid = fork();
                        if (pid == 0) {
                            chdir("/");
                            char *args[] = {"/xbin/tar", "-x","-f", sfpath, NULL};
                            execv("/xbin/tar", args);
                            fprintf(stderr, "E:Can't backup\n(%s)\n", strerror(errno));
                            _exit(-1);
                        }

                        int status;

                        while (waitpid(pid, &status, WNOHANG) == 0) {
                            ui_print(".");
                            sleep(1);
                        }
                        ui_print("\n");

                        if (!WIFEXITED(status) || (WEXITSTATUS(status) != 0)) {
                            LOGE("Can't extract tar file %s\n", st);
                        } else {
                            ui_print("\nRestore complete.\n");
                        }
                        break;
                    }
                    continue;
                }
            }
            ui_print("\nData restore aborted.\n");
            continue;
        }

        if (chosen_item >= 0 && chosen_item < BRTYPE_HL1) {
            if (chosen_item == BRTYPE_BACK) break;

            ui_print("\n-- Press HOME to confirm, or");
            ui_print("\n-- any other key to abort..");
            int confirm_wipe = ui_wait_key();
            if (confirm_wipe == KEY_DREAM_HOME) {
                switch (chosen_item) {
                case BRTYPE_B_SYS:
                    if (ensure_root_path_mounted("SYSTEM:")) { ui_print("\nError mount /system\n"); return; }
                    break;
                case BRTYPE_B_DATA:
                    if (ensure_root_path_mounted("DATA:")) { ui_print("\nError mount /data\n"); return; }
                    break;
                }
                switch (chosen_item) {
                case BRTYPE_B_SYS:
                case BRTYPE_B_DATA:
                    if (ensure_root_path_mounted("SDCARD:")) { ui_print("\nError mount sdcard\n"); return; }
                    ui_print("\nBackuping: ");
                    ui_print(backup_parts[chosen_item-1]);
                    ui_print("\n");

                    // create backup folder
                    mkdir("/sdcard/samdroid", 0777);

                    // create file name
                    time_t rawtime;
                    struct tm * ti;
                    time ( &rawtime );
                    ti = localtime ( &rawtime );
                    strftime(st,255,"/sdcard/samdroid/Backup_%Y%m%d-%H%M%S_",ti);
                    strcat(st, backup_file[chosen_item-1]);
                    strcat(st, ".tar");

                    pid_t pid = fork();
                    if (pid == 0) {
                        char *args[] = {"/xbin/busybox", "tar", "-c", "--exclude=*RFS_LOG.LO*", "-f", st, backup_parts[chosen_item-1], NULL};
                        execv("/xbin/busybox", args);
                        fprintf(stderr, "E:Can't backup\n(%s)\n", strerror(errno));
                        _exit(-1);
                    }

                    int status;

                    while (waitpid(pid, &status, WNOHANG) == 0) {
                        ui_print(".");
                        sleep(1);
                    }
                    ui_print("\n");

                    if (!WIFEXITED(status) || (WEXITSTATUS(status) != 0)) {
                        LOGE("Can't create tar file %s\n", st);
                    } else {
                        ui_print("Backup complete.\n");
                    }
                    break;
                }

            } else {
                ui_print("\nData backup aborted.\n");
            }
            if (!ui_text_visible()) break;
        }
    }
}

static void
        choose_sdparted_type()
{
    static char* headers[] = { 	"Choose size of ext2 partition"
                                "",
                                "Use Up/Down and OK to select",
                                "",
                                NULL };

#define SDPARTED_BACK		0
#define SDPARTED_256M	 	1
#define SDPARTED_384M	 	2
#define SDPARTED_512M	 	3
#define SDPARTED_768M	 	4
#define SDPARTED_1024M	 	5
#define SDPARTED_0		6
#define SDPARTED_FORMAT		7

    static char* part_size[] = { "256M", "384M", "512M" , "768M" , "1024M" , "0" };

    static char* items[] = { 	"Back to main menu",
                                "Make 256M ext2 on SD",
                                "Make 384M ext2 on SD",
                                "Make 512M ext2 on SD",
                                "Make 786M ext2 on SD",
                                "Make 1024M ext2 on SD",
                                "Delete second partition",
                                "Format second partition (ext2)",
                                NULL };

    for (;;) {
        int chosen_item = get_selected_item(headers, items);

        if (chosen_item >= 0) {
            if (chosen_item == SDPARTED_BACK) break;

            ui_print("\n-- This will ERASE");
            if (SDPARTED_FORMAT == chosen_item) {
                ui_print("\n-- your data on 2nd part !!!");
            }
            else {
                ui_print("\n--    your data on SDCARD !!!");
            }
            ui_print("\n-- Press HOME to confirm, or");
            ui_print("\n-- any other key to abort..");

            if (ui_wait_key() == KEY_DREAM_HOME) {
                if (chosen_item == SDPARTED_FORMAT) {
                    ui_print("\nFormating 2nd partition (ext2)");
                    pid_t pid = fork();
                    if (pid == 0) {
                        char *args[] = {"/xbin/mke2fs", "/dev/block/mmcblk0p2", NULL};
                        execv("/xbin/mke2fs", args);
                        fprintf(stderr, "E:Can't format sdcard\n(%s)\n", strerror(errno));
                        _exit(-1);
                    }
                    int status;
                    while (waitpid(pid, &status, WNOHANG) == 0) {
                        ui_print(".");
                        sleep(1);
                    }
                    ui_print("\n");

                    if (!WIFEXITED(status) || (WEXITSTATUS(status) != 0)) {
                        ui_print("Error formating.\n\n");
                    } else {
                        ui_print("Format complete.\n");
                    }
                }
                else if (chosen_item >= SDPARTED_256M || chosen_item <= SDPARTED_0) {
                    ui_print("\nFormating SDCARD");
                    pid_t pid = fork();
                    if (pid == 0) {
                        char *args[] = {"/xbin/sdparted", "-es", part_size[chosen_item-1], "-ss", "0", "-s", NULL};
                        execv("/xbin/sdparted", args);
                        fprintf(stderr, "E:Can't format sdcard\n(%s)\n", strerror(errno));
                        _exit(-1);
                    }
                    int status;
                    while (waitpid(pid, &status, WNOHANG) == 0) {
                        ui_print(".");
                        sleep(1);
                    }
                    ui_print("\n");
                    if (!WIFEXITED(status) || (WEXITSTATUS(status) != 0)) {
                        ui_print("Error formating sdcard.\n\n");
                    } else {
                        ui_print("Format SDCARD complete.\n");
                    }
                }
            } else {
                ui_print("\nFormat aborted.\n");
            }
            if (!ui_text_visible()) break;
            break;
        }
    }
}

static void choose_os()
{
    static char* headers[] = { "Choose OS to boot"
                               "",
                               "Use Up/Down and OK to select",
                               "",
                               NULL
                             };

    /* Reading boot list from sd card */

    if (ensure_root_path_mounted("SDCARD:")) { ui_print("\nError mount sdcard\n"); return; }
    FILE* f = fopen("/sdcard/.bootlst","r");
    char list[20][20];
    list[0] = "Back to main menu";
    list[1] = "Boot from internal memory";
    int i=1;
    while(!feof(f))
    {
        fgets(list[i++],20,f);
    }
    fclose(f);

    for (;;) {
        int chosen_item = get_selected_item(headers, list);
        if (chosen_item >= 0)
        {
            if(chosen_item == 0) { break; } /* "Back" choosed */
            char file_name[40];
            if(chosen_item == 1) {          /* "Internal" choosed */

                file_name="internal_init.rc","r";
            }
            else {
                strcat(file_name,"/sdcard/");
                strcat(file_name,list[choosen_item]);
                strcat(file_name,"/init.rc");
            }

            /* Copying init.rc from coosed folder to /sdcard/next_step.rc */

            FILE* i=fopen(file_name,"r");
            FILE* o=fopen("/sdcard/next_step.rc","w");
            while(!feof(i))
            {
                char c=fgetc(i);
                fputc(c,o);
            }
            flose(i);
            fclose(o);
            break;
        }
    }
}

static void
        prompt_and_wait()
{
    static char* headers[] = { 	"Android system recovery <"
            	              	EXPAND(RECOVERY_API_VERSION) ">",
                                "   -- Samsung Spica i5700 --",
                                "",
                                "Use Up/Down and OK to select",
                                "",
                                NULL };

    // these constants correspond to elements of the items[] list.
#define ITEM_REBOOT        0
#define ITEM_APPLY_UPDATE  1
#define ITEM_APPLY_ANYZIP  2
#define ITEM_SAMDROID      3
#define ITEM_TAR_BACKUP    4
#define ITEM_WIPE_DATA     5
#define ITEM_PARTED        6
#define ITEM_MOUNT         7
#define ITEM_RESTORE       8
#define ITEM_FSCK          9
#define ITEM_CHOOSE_OS     10

    static char* items[] = { "Reboot system now [Home+Back]",
                             "Apply sdcard/update.zip",
                             "Apply any zip from SD",
                             "Samdroid v0.2.1 backup (4 Odin)",
                             "TAR Backup/Restore ->",
                             "Wipe, choose what ->",
                             "Partition sdcard ->",
                             "Mount ->",
                             "Choose OS ->",
                             NULL };

    static char* items0[] = { "Reboot system now [Home+Back]",
                              "Apply sdcard/update.zip",
                              "Apply any zip from SD",
                              NULL };


    FILE* f = fopen("/xbin/samdroid", "r");
    if (f == NULL) {
        ui_start_menu(headers, items0);
    }
    else {
        ui_start_menu(headers, items);
        fclose(f);
    }

    int selected = 0;
    int chosen_item = -1;

    finish_recovery(NULL);
    ui_reset_progress();
    for (;;) {
        int key = ui_wait_key();

        //---- get key code for spica
        //	char stt[32];
        //	sprintf(stt, "Key: %d [%2.2x]\n", key, key);
        //	ui_print(stt);
        //----

        int alt = ui_key_pressed(KEY_LEFTALT) || ui_key_pressed(KEY_RIGHTALT);
        int visible = ui_text_visible();

        if (key == KEY_DREAM_BACK && ui_key_pressed(KEY_DREAM_HOME)) {
            // Wait for the keys to be released, to avoid triggering
            // special boot modes (like coming back into recovery!).
            while (ui_key_pressed(KEY_DREAM_BACK) ||
                   ui_key_pressed(KEY_DREAM_HOME)) {
                usleep(1000);
            }
            chosen_item = ITEM_REBOOT;
        } else if (alt && key == KEY_W) {
            chosen_item = ITEM_WIPE_DATA;
        } else if (alt && key == KEY_A) {
            chosen_item = ITEM_APPLY_UPDATE;
        } else if (alt && key == KEY_B) {
            chosen_item = ITEM_SAMDROID;
        } else if (alt && key == KEY_F) {
            chosen_item = ITEM_FSCK;
        } else if (alt && key == KEY_R) {
            chosen_item = ITEM_RESTORE;
        } else if ((key == KEY_DOWN || key == KEY_VOLUMEDOWN || key == KEY_I5700_DOWN) && visible) {
            ++selected;
            selected = ui_menu_select(selected);
        } else if ((key == KEY_UP || key == KEY_VOLUMEUP || key == KEY_I5700_UP) && visible) {
            --selected;
            selected = ui_menu_select(selected);
        } else if ((key == BTN_MOUSE || key == KEY_I5700_CENTER) && visible) {
            chosen_item = selected;
        }

        if (chosen_item >= 0) {
            // turn off the menu, letting ui_print() to scroll output
            // on the screen.
            ui_end_menu();

            switch (chosen_item) {
            case ITEM_CHOOSE_OS:
                // TODO: write here
                choose_os();
                break;

            case ITEM_REBOOT:
                return;

            case ITEM_PARTED:
                choose_sdparted_type();
                if (!ui_text_visible()) return;
                break;

            case ITEM_TAR_BACKUP:
                choose_backup_type();
                if (!ui_text_visible()) return;
                break;

            case ITEM_MOUNT:
                choose_mount_type();
                if (!ui_text_visible()) return;
                break;

            case ITEM_WIPE_DATA:
                choose_wipe_type();
                if (!ui_text_visible()) return;
                break;

            case ITEM_APPLY_UPDATE:
                ui_print("\n-- Installing new image!");
                ui_print("\n-- Press HOME to confirm, or");
                ui_print("\n-- any other key to abort..");
                int confirm_apply = ui_wait_key();
                if (confirm_apply == KEY_DREAM_HOME) {
                    sui_print("\nInstall from sdcard...\n");
                    int status = install_package(SDCARD_PACKAGE_FILE);
                    if (status != INSTALL_SUCCESS) {
                        ui_set_background(BACKGROUND_ICON_ERROR);
                        ui_print("Installation aborted.\n");
                    } else if (!ui_text_visible()) {
                        return;  // reboot if logs aren't visible
                    } else {
                        if (firmware_update_pending()) {
                            ui_print("\nReboot via home+back or menu\n"
                                     "to complete installation.\n");
                        } else {
                            ui_print("\nInstall from sdcard complete.\n");
                        }
                    }
                } else {
                    ui_print("\nInstallation aborted.\n");
                }
                if (!ui_text_visible()) return;
                break;
		case ITEM_APPLY_ANYZIP:
                choose_update_file();
                break;

                case ITEM_SAMDROID:
                if (ensure_root_path_mounted("SDCARD:") != 0) {
                    ui_print("Can't mount sdcard\n");
                } else {
                    ui_print("\nPerforming backup");
                    pid_t pid = fork();
                    if (pid == 0) {
                        char *args[] = {"/xbin/bash", "-c", "/xbin/samdroid backup", "1>&2", NULL};
                        execv("/xbin/bash", args);
                        fprintf(stderr, "E:Can't run samdroid\n(%s)\n", strerror(errno));
                        _exit(-1);
                    }

                    int status;

                    while (waitpid(pid, &status, WNOHANG) == 0) {
                        ui_print(".");
                        sleep(1);
                    }
                    ui_print("\n");

                    if (!WIFEXITED(status) || (WEXITSTATUS(status) != 0)) {
                        ui_print("Error running samdroid backup. Backup not performed.\n\n");
                    } else {
                        ui_print("Backup complete!\nUse Odin for restore\n\n");
                    }
                }
                break;

                case ITEM_RESTORE:
                ui_print("\n-- Restore latest backup");
                ui_print("\n-- Press HOME to confirm, or");
                ui_print("\n-- any other key to abort.");
                int confirm_restore = ui_wait_key();
                if (confirm_restore == KEY_DREAM_HOME) {
                    ui_print("\n");
                    if (ensure_root_path_mounted("SDCARD:") != 0) {
                        ui_print("Can't mount sdcard, aborting.\n");
                    } else {
                        ui_print("Restoring latest backup");
                        pid_t pid = fork();
                        if (pid == 0) {
                            char *args[] = {"/sbin/sh", "-c", "/sbin/nandroid-mobile.sh restore", "1>&2", NULL};
                            execv("/sbin/sh", args);
                            fprintf(stderr, "Can't run nandroid-mobile.sh\n(%s)\n", strerror(errno));
                            _exit(-1);
                        }

                        int status3;

                        while (waitpid(pid, &status3, WNOHANG) == 0) {
                            ui_print(".");
                            sleep(1);
                        }
                        ui_print("\n");

                        if (!WIFEXITED(status3) || (WEXITSTATUS(status3) != 0)) {
                            ui_print("Error performing restore!  Try running 'nandroid-mobile.sh restore' from console.\n\n");
                        } else {
                            ui_print("Restore complete!\n\n");
                        }
                    }
                } else {
                    ui_print("Restore complete!\n\n");
                }
                if (!ui_text_visible()) return;
                break;

                case ITEM_FSCK:ITEM_CHOOSE_OS
                            ui_print("Checking filesystems");
                    pid_t pidf = fork();
                    if (pidf == 0) {
                        char *args[] = { "/sbin/sh", "-c", "/sbin/repair_fs", "1>&2", NULL };
                        execv("/sbin/sh", args);
                        fprintf(stderr, "Unable to execute e2fsck!\n(%s)\n", strerror(errno));
                        _exit(-1);
                    }

                    int fsck_status;

                    while (waitpid(pidf, &fsck_status, WNOHANG) == 0) {
                        ui_print(".");
                        sleep(1);
                    }
                    ui_print("\n");

                    if (!WIFEXITED(fsck_status) || (WEXITSTATUS(fsck_status) != 0)) {
                        ui_print("Error checking filesystem!  Run e2fsck manually from console.\n\n");
                    } else {
                        ui_print("Filesystem checked and repaired.\n\n");
                    }
                    break;

                }

            // if we didn't return from this function to reboot, show
            // the menu again.
            ui_start_menu(headers, items);
            selected = 0;
            chosen_item = -1;

            finish_recovery(NULL);
            ui_reset_progress();

            // throw away keys pressed while the command was running,
            // so user doesn't accidentally trigger menu items.
            ui_clear_key_queue();
        }
    }
}

static void
        print_property(const char *key, const char *name, void *cookie)
{
    fprintf(stderr, "%s=%s\n", key, name);
}

int
        main(int argc, char **argv)
{
    time_t start = time(NULL);

    // If these fail, there's not really anywhere to complain...
    freopen(TEMPORARY_LOG_FILE, "a", stdout); setbuf(stdout, NULL);
    freopen(TEMPORARY_LOG_FILE, "a", stderr); setbuf(stderr, NULL);
    fprintf(stderr, "Starting recovery on %s", ctime(&start));

    tcflow(STDIN_FILENO, TCOOFF);
    
    char prop_value[PROPERTY_VALUE_MAX];
    property_get("ro.modversion", &prop_value, "not set");

    ui_init();
    ui_print("Build: ");
    ui_print(prop_value);
    ui_print("\n  by LeshaK (forum.samdroid.net)\n\n");
    get_args(&argc, &argv);

    int previous_runs = 0;
    const char *send_intent = NULL;
    const char *update_package = NULL;
    int wipe_data = 0, wipe_cache = 0;

    int arg;
    while ((arg = getopt_long(argc, argv, "", OPTIONS, NULL)) != -1) {
        switch (arg) {
        case 'p': previous_runs = atoi(optarg); break;
        case 's': send_intent = optarg; break;
        case 'u': update_package = optarg; break;
        case 'w': wipe_data = wipe_cache = 1; break;
        case 'c': wipe_cache = 1; break;
        case '?':
            LOGE("Invalid command argument\n");
            continue;
        }
    }

    fprintf(stderr, "Command:");
    for (arg = 0; arg < argc; arg++) {
        fprintf(stderr, " \"%s\"", argv[arg]);
    }
    fprintf(stderr, "\n\n");

    property_list(print_property, NULL);
    fprintf(stderr, "\n");

#if TEST_AMEND
    test_amend();
#endif

    RecoveryCommandContext ctx = { NULL };
    if (register_update_commands(&ctx)) {
        LOGE("Can't install update commands\n");
    }

    int status = INSTALL_SUCCESS;

    if (update_package != NULL) {
        status = install_package(update_package);
        if (status != INSTALL_SUCCESS) ui_print("Installation aborted.\n");
    } else if (wipe_data || wipe_cache) {
        if (wipe_data && erase_root("DATA:")) status = INSTALL_ERROR;
        if (wipe_cache && erase_root("CACHE:")) status = INSTALL_ERROR;
        if (status != INSTALL_SUCCESS) ui_print("Data wipe failed.\n");
    } else {
        status = INSTALL_ERROR;  // No command specified
    }

    if (status != INSTALL_SUCCESS) ui_set_background(BACKGROUND_ICON_ERROR);
    if (status != INSTALL_SUCCESS /* || ui_text_visible() */ ) prompt_and_wait();

    // If there is a radio image pending, reboot now to install it.
    // maybe_install_firmware_update(send_intent);

    // Otherwise, get ready to boot the main system...
    finish_recovery(send_intent);
    sync();
    if (do_reboot)
    {
    	ui_print("Rebooting...\n");
	sync();
    	reboot(RB_AUTOBOOT);
    }

    tcflush(STDIN_FILENO, TCIOFLUSH);
    tcflow(STDIN_FILENO, TCOON);

    return EXIT_SUCCESS;
}
