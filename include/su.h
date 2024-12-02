/*
** Copyright 2021, Corellium, LLC
** Copyright 2010, Adam Shanks (@ChainsDD)
** Copyright 2008, Zinx Verituse (@zinxv)
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/

#ifndef SU_h 
#define SU_h 1

#include <android/log.h>

#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGV(...) __android_log_print(ANDROID_LOG_VERBOSE, LOG_TAG, __VA_ARGS__)
#define LOGW LOGD

#define PORT 3523

#ifdef LOG_TAG
#undef LOG_TAG
#endif
#define LOG_TAG "sud"

#ifndef AID_SHELL
#define AID_SHELL (get_shell_uid())
#endif

#ifndef AID_ROOT
#define AID_ROOT  2000
#endif

#ifndef AID_SYSTEM
#define AID_SYSTEM (get_system_uid())
#endif

#ifndef AID_RADIO
#define AID_RADIO (get_radio_uid())
#endif

// CyanogenMod-specific behavior
#define CM_ROOT_ACCESS_DISABLED      0
#define CM_ROOT_ACCESS_APPS_ONLY     1
#define CM_ROOT_ACCESS_ADB_ONLY      2
#define CM_ROOT_ACCESS_APPS_AND_ADB  3

#define DEFAULT_SHELL "/system/bin/sh"

#define xstr(a) str(a)
#define str(a) #a

#ifndef VERSION_CODE
#define VERSION_CODE 16
#endif

#define PROTO_VERSION 1

struct su_initiator {
    pid_t pid;
    unsigned uid;
    unsigned user;
    char name[64];
    char bin[PATH_MAX];
    char args[4096];
};

struct su_request {
    unsigned uid;
    char name[64];
    int login;
    int keepenv;
    char *shell;
    char *command;
    char **argv;
    int argc;
    int optind;
};

struct su_user_info {
    // the user in android userspace (multiuser)
    // that invoked this action.
    unsigned android_user_id;
};

struct su_context {
    struct su_initiator from;
    struct su_request to;
    struct su_user_info user;
    mode_t umask;
    char sock_path[PATH_MAX];
};

static inline char *get_command(const struct su_request *to)
{
  if (to->command)
    return to->command;
  if (to->shell)
    return to->shell;
  char* ret = to->argv[to->optind];
  if (ret)
    return ret;
  return DEFAULT_SHELL;
}

int run_daemon();
int connect_daemon(int argc, char *argv[], int ppid);
int su_main(int argc, char *argv[], int need_client);

#include <errno.h>
#include <string.h>
#define PLOGE(fmt,args...) LOGE(fmt " failed with %d: %s", ##args, errno, strerror(errno))
#define PLOGEV(fmt,err,args...) LOGE(fmt " failed with %d: %s", ##args, err, strerror(err))

#endif
