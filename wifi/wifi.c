/*
 * Copyright 2008, The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <dirent.h>

#include "hardware_legacy/wifi.h"
#include "libwpa_client/wpa_ctrl.h"

#define LOG_TAG "WifiHW"
#include "cutils/log.h"
#include "cutils/memory.h"
#include "cutils/misc.h"
#include "cutils/properties.h"
#include "private/android_filesystem_config.h"
#ifdef HAVE_LIBC_SYSTEM_PROPERTIES
#define _REALLY_INCLUDE_SYS__SYSTEM_PROPERTIES_H_
#include <sys/_system_properties.h>
#endif

static struct wpa_ctrl *ctrl_conn;
static struct wpa_ctrl *monitor_conn;

extern int do_dhcp();
extern int ifc_init();
extern void ifc_close();
extern char *dhcp_lasterror();
extern void get_dhcp_info();
extern int init_module(void *, unsigned long, const char *);
extern int delete_module(const char *, unsigned int);

static char iface[PROPERTY_VALUE_MAX];
// TODO: use new ANDROID_SOCKET mechanism, once support for multiple
// sockets is in

#ifndef WIFI_DRIVER_MODULE_PATH
#define WIFI_DRIVER_MODULE_PATH         "/system/lib/modules/wlan.ko"
#endif
#ifndef WIFI_DRIVER_MODULE_NAME
#define WIFI_DRIVER_MODULE_NAME         "wlan"
#endif
#ifndef WIFI_DRIVER_MODULE_ARG
#define WIFI_DRIVER_MODULE_ARG          ""
#endif
#ifndef WIFI_FIRMWARE_LOADER
#define WIFI_FIRMWARE_LOADER		""
#endif
#define WIFI_TEST_INTERFACE		"sta"

#define WIFI_DRIVER_LOADER_DELAY	1000000
#define SYSFS_PATH_MAX                  256

static const char IFACE_DIR[]           = "/data/system/wpa_supplicant";
static const char DRIVER_MODULE_ARG[]   = WIFI_DRIVER_MODULE_ARG;
static const char FIRMWARE_LOADER[]     = WIFI_FIRMWARE_LOADER;
static const char DRIVER_PROP_NAME[]    = "wlan.driver.status";
static const char DRIVER_NAME_PROP[]    = "wlan.modname";
static const char DRIVER_PATH_PROP[]    = "wlan.modpath";
static const char SUPPLICANT_NAME[]     = "wpa_supplicant";
static const char SUPP_PROP_NAME[]      = "init.svc.wpa_supplicant";
static const char SUPP_CONFIG_TEMPLATE[]= "/system/etc/wifi/wpa_supplicant.conf";
static const char SUPP_CONFIG_FILE[]    = "/data/misc/wifi/wpa_supplicant.conf";
static const char MODULE_FILE[]         = "/proc/modules";

static const char SYSFS_CLASS_NET[]     = "/sys/class/net";
static const char MODULE_DEFAULT_DIR[]  = "/system/lib/modules";
static const char SYS_MOD_NAME_DIR[]    = "device/driver/module";

static int insmod(const char *filename, const char *args)
{
    void *module;
    unsigned int size;
    int ret;

    module = load_file(filename, &size);
    if (!module)
        return -1;

    ret = init_module(module, size, args);

    free(module);

    return ret;
}

static int rmmod(const char *modname)
{
    int ret = -1;
    int maxtry = 10;

    while (maxtry-- > 0) {
        ret = delete_module(modname, O_NONBLOCK | O_EXCL);
        if (ret < 0 && errno == EAGAIN)
            usleep(500000);
        else
            break;
    }

    if (ret != 0)
        LOGD("Unable to unload driver module \"%s\": %s\n",
             modname, strerror(errno));
    return ret;
}

int do_dhcp_request(int *ipaddr, int *gateway, int *mask,
                    int *dns1, int *dns2, int *server, int *lease) {
    /* For test driver, always report success */
    if (strcmp(iface, WIFI_TEST_INTERFACE) == 0)
        return 0;

    if (ifc_init() < 0)
        return -1;

    if (do_dhcp(iface) < 0) {
        ifc_close();
        return -1;
    }
    ifc_close();
    get_dhcp_info(ipaddr, gateway, mask, dns1, dns2, server, lease);
    return 0;
}

const char *get_dhcp_error_string() {
    return dhcp_lasterror();
}

static int get_driver_path(const char *mod, const char *path, char *buf) {
    DIR *dir;
    struct dirent *de;
    char modpath[SYSFS_PATH_MAX];
    int ret = 0;

    if ((dir = opendir(path))) {
        while ((de = readdir(dir))) {
            struct stat sb;
            if (de->d_name[0] == '.')
                continue;
            snprintf(modpath, SYSFS_PATH_MAX, "%s/%s", path, de->d_name);
            if (!strcmp(de->d_name, mod)) {
                strncpy(buf, modpath, SYSFS_PATH_MAX - 1);
                buf[SYSFS_PATH_MAX - 1] = '\0';
                ret = 1;
                break;
            }
            if (!stat(modpath, &sb) && (sb.st_mode & S_IFMT) == S_IFDIR)
                if ((ret = get_driver_path(mod, modpath, buf)))
                    break;
        }
        closedir(dir);
    }
    return ret;
}

static int get_driver_info(char *buf) {
    DIR *netdir;
    struct dirent *de;
    char path[SYSFS_PATH_MAX];
    char link[SYSFS_PATH_MAX];
    int ret = 0;

    if ((netdir = opendir(SYSFS_CLASS_NET))) {
        while ((de = readdir(netdir))) {
            int cnt;
            char *pos;
            if (de->d_name[0] == '.')
                continue;
            snprintf(path, SYSFS_PATH_MAX, "%s/%s/wireless", SYSFS_CLASS_NET, de->d_name);
            if (access(path, F_OK)) {
                snprintf(path, SYSFS_PATH_MAX, "%s/%s/phy80211", SYSFS_CLASS_NET, de->d_name);
                if (access(path, F_OK))
                    continue;
            }
            /* found the wifi interface */
            property_set("wlan.interface", de->d_name);
            snprintf(path, SYSFS_PATH_MAX, "%s/%s/%s", SYSFS_CLASS_NET, de->d_name, SYS_MOD_NAME_DIR);
            if ((cnt = readlink(path, link, SYSFS_PATH_MAX - 1)) < 0) {
                LOGW("can not find link of %s", path);
                continue;
            }
            link[cnt] = '\0';
            if ((pos = strrchr(link, '/'))) {
                property_set(DRIVER_NAME_PROP, ++pos);
                strncpy(buf, pos, PROPERTY_VALUE_MAX - 1);
                buf[PROPERTY_VALUE_MAX - 1] = '\0';
                ret = 1;
                break;
            }
        }
        closedir(netdir);
    }

    return ret;
}

int is_wifi_driver_loaded() {
    char modname[PROPERTY_VALUE_MAX];
    char line[PROPERTY_VALUE_MAX];
    FILE *proc;
    int cnt = property_get(DRIVER_NAME_PROP, modname, NULL);

    if (!cnt) {
        if (get_driver_info(modname))
            cnt = strlen(modname);
        else
            goto unloaded;
    }
    modname[cnt++] = ' ';

    /*
     * If the property says the driver is loaded, check to
     * make sure that the property setting isn't just left
     * over from a previous manual shutdown or a runtime
     * crash.
     */
    if ((proc = fopen(MODULE_FILE, "r")) == NULL) {
        LOGW("Could not open %s: %s", MODULE_FILE, strerror(errno));
        goto unloaded;
    }

    while ((fgets(line, sizeof(line), proc)) != NULL) {
        if (strncmp(line, modname, cnt) == 0) {
            fclose(proc);
            return 1;
        }
    }
    fclose(proc);

unloaded:
    property_set(DRIVER_PROP_NAME, "unloaded");
    return 0;
}

int wifi_load_driver()
{
    char driver_status[PROPERTY_VALUE_MAX];
    char modname[PROPERTY_VALUE_MAX];
    char modpath[SYSFS_PATH_MAX];
    int count = 100; /* wait at most 20 seconds for completion */

    if (is_wifi_driver_loaded()) {
        return 0;
    }

    if (!property_get(DRIVER_PATH_PROP, modpath, NULL)) {
        property_get(DRIVER_NAME_PROP, modname, WIFI_DRIVER_MODULE_NAME);
        strcat(modname, ".ko");
        if (!get_driver_path(modname, MODULE_DEFAULT_DIR, modpath))
            strcpy(modpath, WIFI_DRIVER_MODULE_PATH);
    }

    LOGI("got module patch %s", modpath);
    if (insmod(modpath, DRIVER_MODULE_ARG) < 0)
        return -1;

    if (strcmp(FIRMWARE_LOADER,"") == 0) {
        /* usleep(WIFI_DRIVER_LOADER_DELAY); */
        property_set(DRIVER_PROP_NAME, "ok");
    }
    else {
        property_set("ctl.start", FIRMWARE_LOADER);
    }
    sched_yield();
    while (count-- > 0) {
        if (property_get(DRIVER_PROP_NAME, driver_status, NULL)) {
            if (strcmp(driver_status, "ok") == 0) {
                get_driver_info(modname);
                return 0;
            } else if (strcmp(DRIVER_PROP_NAME, "failed") == 0) {
                wifi_unload_driver();
                return -1;
            }
        }
        usleep(200000);
    }
    property_set(DRIVER_PROP_NAME, "timeout");
    wifi_unload_driver();
    return -1;
}

int wifi_unload_driver()
{
    int count = 20; /* wait at most 10 seconds for completion */
    char modname[PROPERTY_VALUE_MAX];

    if (!property_get(DRIVER_NAME_PROP, modname, WIFI_DRIVER_MODULE_NAME))
        return -1;

    if (rmmod(modname) == 0) {
        while (count-- > 0) {
            if (!is_wifi_driver_loaded())
                break;
            usleep(500000);
        }
        if (count) {
            return 0;
        }
    }
    return -1;
}

int ensure_config_file_exists()
{
    char buf[2048];
    char ifc[PROPERTY_VALUE_MAX];
    char *sptr;
    char *pbuf;
    int srcfd, destfd;
    struct stat sb;
    int nread;

    if (access(SUPP_CONFIG_FILE, R_OK|W_OK) == 0) {
        /* return if filesize is at least 10 bytes */
        if (stat(SUPP_CONFIG_FILE, &sb) == 0 && sb.st_size > 10) {
            pbuf = malloc(sb.st_size + PROPERTY_VALUE_MAX);
            if (!pbuf)
                return 0;
            srcfd = open(SUPP_CONFIG_FILE, O_RDONLY);
            if (srcfd < 0) {
                LOGE("Cannot open \"%s\": %s", SUPP_CONFIG_FILE, strerror(errno));
                free(pbuf);
                return 0;
            }
            nread = read(srcfd, pbuf, sb.st_size);
            close(srcfd);
            if (nread < 0) {
                LOGE("Cannot read \"%s\": %s", SUPP_CONFIG_FILE, strerror(errno));
                free(pbuf);
                return 0;
            }
            property_get("wlan.interface", ifc, WIFI_TEST_INTERFACE);
            if ((sptr = strstr(pbuf, "ctrl_interface="))) {
                char *iptr = sptr + strlen("ctrl_interface=");
                int ilen = 0;
                int mlen = strlen(ifc);
                if (strncmp("DIR=", iptr, 4) && strncmp(ifc, iptr, mlen)) {
                    LOGE("ctrl_interface != %s", ifc);
                    while (((ilen + (iptr - pbuf)) < nread) && (iptr[ilen] != '\n'))
                        ilen++;
                    mlen = ((ilen >= mlen) ? ilen : mlen) + 1;
                    memmove(iptr + mlen, iptr + ilen + 1, nread - (iptr + ilen + 1 - pbuf));
                    memset(iptr, '\n', mlen);
                    memcpy(iptr, ifc, strlen(ifc));
                    destfd = open(SUPP_CONFIG_FILE, O_RDWR, 0660);
                    if (destfd < 0) {
                        LOGE("Cannot update \"%s\": %s", SUPP_CONFIG_FILE, strerror(errno));
                        free(pbuf);
                        return -1;
                    }
                    write(destfd, pbuf, nread);
                    close(destfd);
                }
            }
            free(pbuf);
            return 0;
        }
    } else if (errno != ENOENT) {
        LOGE("Cannot access \"%s\": %s", SUPP_CONFIG_FILE, strerror(errno));
        return -1;
    }

    srcfd = open(SUPP_CONFIG_TEMPLATE, O_RDONLY);
    if (srcfd < 0) {
        LOGE("Cannot open \"%s\": %s", SUPP_CONFIG_TEMPLATE, strerror(errno));
        return -1;
    }

    destfd = open(SUPP_CONFIG_FILE, O_CREAT|O_RDWR, 0660);
    if (destfd < 0) {
        close(srcfd);
        LOGE("Cannot create \"%s\": %s", SUPP_CONFIG_FILE, strerror(errno));
        return -1;
    }

    while ((nread = read(srcfd, buf, sizeof(buf))) != 0) {
        if (nread < 0) {
            LOGE("Error reading \"%s\": %s", SUPP_CONFIG_TEMPLATE, strerror(errno));
            close(srcfd);
            close(destfd);
            unlink(SUPP_CONFIG_FILE);
            return -1;
        }
        write(destfd, buf, nread);
    }

    close(destfd);
    close(srcfd);

    /* chmod is needed because open() didn't set permisions properly */
    if (chmod(SUPP_CONFIG_FILE, 0660) < 0) {
        LOGE("Error changing permissions of %s to 0660: %s",
             SUPP_CONFIG_FILE, strerror(errno));
        unlink(SUPP_CONFIG_FILE);
        return -1;
    }

    if (chown(SUPP_CONFIG_FILE, AID_SYSTEM, AID_WIFI) < 0) {
        LOGE("Error changing group ownership of %s to %d: %s",
             SUPP_CONFIG_FILE, AID_WIFI, strerror(errno));
        unlink(SUPP_CONFIG_FILE);
        return -1;
    }
    return 0;
}

int wifi_start_supplicant()
{
    char daemon_cmd[PROPERTY_VALUE_MAX];
    char supp_status[PROPERTY_VALUE_MAX] = {'\0'};
    int count = 200; /* wait at most 20 seconds for completion */
#ifdef HAVE_LIBC_SYSTEM_PROPERTIES
    const prop_info *pi;
    unsigned serial = 0;
#endif

    /* Check whether already running */
    if (property_get(SUPP_PROP_NAME, supp_status, NULL)
            && strcmp(supp_status, "running") == 0) {
        return 0;
    }

    /* Before starting the daemon, make sure its config file exists */
    if (ensure_config_file_exists() < 0) {
        LOGE("Wi-Fi will not be enabled");
        return -1;
    }

    /* Clear out any stale socket files that might be left over. */
    wpa_ctrl_cleanup();

#ifdef HAVE_LIBC_SYSTEM_PROPERTIES
    /*
     * Get a reference to the status property, so we can distinguish
     * the case where it goes stopped => running => stopped (i.e.,
     * it start up, but fails right away) from the case in which
     * it starts in the stopped state and never manages to start
     * running at all.
     */
    pi = __system_property_find(SUPP_PROP_NAME);
    if (pi != NULL) {
        serial = pi->serial;
    }
#endif
    property_get("wlan.interface", iface, WIFI_TEST_INTERFACE);
    snprintf(daemon_cmd, PROPERTY_VALUE_MAX, "%s:-i%s", SUPPLICANT_NAME, iface);
    property_set("ctl.start", daemon_cmd);
    sched_yield();

    while (count-- > 0) {
#ifdef HAVE_LIBC_SYSTEM_PROPERTIES
        if (pi == NULL) {
            pi = __system_property_find(SUPP_PROP_NAME);
        }
        if (pi != NULL) {
            __system_property_read(pi, NULL, supp_status);
            if (strcmp(supp_status, "running") == 0) {
                return 0;
            } else if (pi->serial != serial &&
                    strcmp(supp_status, "stopped") == 0) {
                return -1;
            }
        }
#else
        if (property_get(SUPP_PROP_NAME, supp_status, NULL)) {
            if (strcmp(supp_status, "running") == 0)
                return 0;
        }
#endif
        usleep(100000);
    }
    return -1;
}

int wifi_stop_supplicant()
{
    char supp_status[PROPERTY_VALUE_MAX] = {'\0'};
    int count = 50; /* wait at most 5 seconds for completion */

    /* Check whether supplicant already stopped */
    if (property_get(SUPP_PROP_NAME, supp_status, NULL)
        && strcmp(supp_status, "stopped") == 0) {
        return 0;
    }

    property_set("ctl.stop", SUPPLICANT_NAME);
    sched_yield();

    while (count-- > 0) {
        if (property_get(SUPP_PROP_NAME, supp_status, NULL)) {
            if (strcmp(supp_status, "stopped") == 0)
                return 0;
        }
        usleep(100000);
    }
    return -1;
}

int wifi_connect_to_supplicant()
{
    char ifname[256];
    char supp_status[PROPERTY_VALUE_MAX] = {'\0'};

    /* Make sure supplicant is running */
    if (!property_get(SUPP_PROP_NAME, supp_status, NULL)
            || strcmp(supp_status, "running") != 0) {
        LOGE("Supplicant not running, cannot connect");
        return -1;
    }

    if (access(IFACE_DIR, F_OK) == 0) {
        snprintf(ifname, sizeof(ifname), "%s/%s", IFACE_DIR, iface);
    } else {
        strlcpy(ifname, iface, sizeof(ifname));
    }

    ctrl_conn = wpa_ctrl_open(ifname);
    if (ctrl_conn == NULL) {
        LOGE("Unable to open connection to supplicant on \"%s\": %s",
             ifname, strerror(errno));
        return -1;
    }
    monitor_conn = wpa_ctrl_open(ifname);
    if (monitor_conn == NULL) {
        wpa_ctrl_close(ctrl_conn);
        ctrl_conn = NULL;
        return -1;
    }
    if (wpa_ctrl_attach(monitor_conn) != 0) {
        wpa_ctrl_close(monitor_conn);
        wpa_ctrl_close(ctrl_conn);
        ctrl_conn = monitor_conn = NULL;
        return -1;
    }
    return 0;
}

int wifi_send_command(struct wpa_ctrl *ctrl, const char *cmd, char *reply, size_t *reply_len)
{
    int ret;

    if (ctrl_conn == NULL) {
        LOGV("Not connected to wpa_supplicant - \"%s\" command dropped.\n", cmd);
        return -1;
    }
    ret = wpa_ctrl_request(ctrl, cmd, strlen(cmd), reply, reply_len, NULL);
    if (ret == -2) {
        LOGD("'%s' command timed out.\n", cmd);
        return -2;
    } else if (ret < 0 || strncmp(reply, "FAIL", 4) == 0) {
        return -1;
    }
    if (strncmp(cmd, "PING", 4) == 0) {
        reply[*reply_len] = '\0';
    }
    return 0;
}

int wifi_wait_for_event(char *buf, size_t buflen)
{
    size_t nread = buflen - 1;
    int fd;
    fd_set rfds;
    int result;
    struct timeval tval;
    struct timeval *tptr;

    if (monitor_conn == NULL) {
        LOGD("Connection closed\n");
        strncpy(buf, WPA_EVENT_TERMINATING " - connection closed", buflen-1);
        buf[buflen-1] = '\0';
        return strlen(buf);
    }

    result = wpa_ctrl_recv(monitor_conn, buf, &nread);
    if (result < 0) {
        LOGD("wpa_ctrl_recv failed: %s\n", strerror(errno));
        strncpy(buf, WPA_EVENT_TERMINATING " - recv error", buflen-1);
        buf[buflen-1] = '\0';
        return strlen(buf);
    }
    buf[nread] = '\0';
    /* LOGD("wait_for_event: result=%d nread=%d string=\"%s\"\n", result, nread, buf); */
    /* Check for EOF on the socket */
    if (result == 0 && nread == 0) {
        /* Fabricate an event to pass up */
        LOGD("Received EOF on supplicant socket\n");
        strncpy(buf, WPA_EVENT_TERMINATING " - signal 0 received", buflen-1);
        buf[buflen-1] = '\0';
        return strlen(buf);
    }
    /*
     * Events strings are in the format
     *
     *     <N>CTRL-EVENT-XXX 
     *
     * where N is the message level in numerical form (0=VERBOSE, 1=DEBUG,
     * etc.) and XXX is the event name. The level information is not useful
     * to us, so strip it off.
     */
    if (buf[0] == '<') {
        char *match = strchr(buf, '>');
        if (match != NULL) {
            nread -= (match+1-buf);
            memmove(buf, match+1, nread+1);
        }
    }
    return nread;
}

void wifi_close_supplicant_connection()
{
    if (ctrl_conn != NULL) {
        wpa_ctrl_close(ctrl_conn);
        ctrl_conn = NULL;
    }
    if (monitor_conn != NULL) {
        wpa_ctrl_close(monitor_conn);
        monitor_conn = NULL;
    }
}

int wifi_command(const char *command, char *reply, size_t *reply_len)
{
    return wifi_send_command(ctrl_conn, command, reply, reply_len);
}
