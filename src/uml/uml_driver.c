/*
 * uml_driver.c: core driver methods for managing UML guests
 *
 * Copyright (C) 2006-2012 Red Hat, Inc.
 * Copyright (C) 2006-2008 Daniel P. Berrange
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307  USA
 *
 * Author: Daniel P. Berrange <berrange@redhat.com>
 */

#include <config.h>

#include <sys/types.h>
#include <sys/poll.h>
#include <limits.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/utsname.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <paths.h>
#include <pwd.h>
#include <stdio.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <sys/inotify.h>
#include <sys/un.h>

#include "uml_driver.h"
#include "uml_conf.h"
#include "buf.h"
#include "util.h"
#include "nodeinfo.h"
#include "stats_linux.h"
#include "capabilities.h"
#include "memory.h"
#include "uuid.h"
#include "domain_conf.h"
#include "domain_audit.h"
#include "datatypes.h"
#include "logging.h"
#include "domain_nwfilter.h"
#include "virfile.h"
#include "fdstream.h"
#include "configmake.h"
#include "virnetdevtap.h"
#include "virnodesuspend.h"
#include "viruri.h"

#define VIR_FROM_THIS VIR_FROM_UML

/* For storing short-lived temporary files. */
#define TEMPDIR LOCALSTATEDIR "/cache/libvirt"

typedef struct _umlDomainObjPrivate umlDomainObjPrivate;
typedef umlDomainObjPrivate *umlDomainObjPrivatePtr;
struct _umlDomainObjPrivate {
    int monitor;
    int monitorWatch;
};

static int umlProcessAutoDestroyInit(struct uml_driver *driver);
static void umlProcessAutoDestroyRun(struct uml_driver *driver,
                                     virConnectPtr conn);
static void umlProcessAutoDestroyShutdown(struct uml_driver *driver);
static int umlProcessAutoDestroyAdd(struct uml_driver *driver,
                                    virDomainObjPtr vm,
                                    virConnectPtr conn);
static int umlProcessAutoDestroyRemove(struct uml_driver *driver,
                                       virDomainObjPtr vm);


static int umlShutdown(void);

static void *umlDomainObjPrivateAlloc(void)
{
    umlDomainObjPrivatePtr priv;

    if (VIR_ALLOC(priv) < 0)
        return NULL;

    priv->monitor = -1;
    priv->monitorWatch = -1;

    return priv;
}

static void umlDomainObjPrivateFree(void *data)
{
    umlDomainObjPrivatePtr priv = data;

    VIR_FREE(priv);
}


static void umlDriverLock(struct uml_driver *driver)
{
    virMutexLock(&driver->lock);
}
static void umlDriverUnlock(struct uml_driver *driver)
{
    virMutexUnlock(&driver->lock);
}


static int umlOpenMonitor(struct uml_driver *driver,
                          virDomainObjPtr vm);
static int umlReadPidFile(struct uml_driver *driver,
                          virDomainObjPtr vm);
static void umlDomainEventQueue(struct uml_driver *driver,
                                virDomainEventPtr event);

static int umlStartVMDaemon(virConnectPtr conn,
                            struct uml_driver *driver,
                            virDomainObjPtr vm,
                            bool autoDestroy);

static void umlShutdownVMDaemon(struct uml_driver *driver,
                                virDomainObjPtr vm,
                                virDomainShutoffReason reason);


static int umlMonitorCommand(const struct uml_driver *driver,
                             const virDomainObjPtr vm,
                             const char *cmd,
                             char **reply);

static struct uml_driver *uml_driver = NULL;

struct umlAutostartData {
    struct uml_driver *driver;
    virConnectPtr conn;
};

static void
umlAutostartDomain(void *payload, const void *name ATTRIBUTE_UNUSED, void *opaque)
{
    virDomainObjPtr vm = payload;
    const struct umlAutostartData *data = opaque;

    virDomainObjLock(vm);
    if (vm->autostart &&
        !virDomainObjIsActive(vm)) {
        int ret;
        virResetLastError();
        ret = umlStartVMDaemon(data->conn, data->driver, vm, false);
        virDomainAuditStart(vm, "booted", ret >= 0);
        if (ret < 0) {
            virErrorPtr err = virGetLastError();
            VIR_ERROR(_("Failed to autostart VM '%s': %s"),
                      vm->def->name, err ? err->message : _("unknown error"));
        } else {
            virDomainEventPtr event =
                virDomainEventNewFromObj(vm,
                                         VIR_DOMAIN_EVENT_STARTED,
                                         VIR_DOMAIN_EVENT_STARTED_BOOTED);
            if (event)
                umlDomainEventQueue(data->driver, event);
        }
    }
    virDomainObjUnlock(vm);
}

static void
umlAutostartConfigs(struct uml_driver *driver) {
    /* XXX: Figure out a better way todo this. The domain
     * startup code needs a connection handle in order
     * to lookup the bridge associated with a virtual
     * network
     */
    virConnectPtr conn = virConnectOpen(driver->privileged ?
                                        "uml:///system" :
                                        "uml:///session");
    /* Ignoring NULL conn which is mostly harmless here */

    struct umlAutostartData data = { driver, conn };

    umlDriverLock(driver);
    virHashForEach(driver->domains.objs, umlAutostartDomain, &data);
    umlDriverUnlock(driver);

    if (conn)
        virConnectClose(conn);
}


static int
umlIdentifyOneChrPTY(struct uml_driver *driver,
                     virDomainObjPtr dom,
                     virDomainChrDefPtr def,
                     const char *dev)
{
    char *cmd;
    char *res = NULL;
    int retries = 0;
    if (virAsprintf(&cmd, "config %s%d", dev, def->target.port) < 0) {
        virReportOOMError();
        return -1;
    }
requery:
    if (umlMonitorCommand(driver, dom, cmd, &res) < 0)
        return -1;

    if (res && STRPREFIX(res, "pts:")) {
        VIR_FREE(def->source.data.file.path);
        if ((def->source.data.file.path = strdup(res + 4)) == NULL) {
            virReportOOMError();
            VIR_FREE(res);
            VIR_FREE(cmd);
            return -1;
        }
    } else if (!res || STRPREFIX(res, "pts")) {
        /* It can take a while to startup, so retry for
           upto 5 seconds */
        /* XXX should do this in a better non-blocking
           way somehow ...perhaps register a timer */
        if (retries++ < 50) {
            usleep(1000*10);
            goto requery;
        }
    }

    VIR_FREE(cmd);
    VIR_FREE(res);
    return 0;
}

static int
umlIdentifyChrPTY(struct uml_driver *driver,
                  virDomainObjPtr dom)
{
    int i;

    for (i = 0 ; i < dom->def->nconsoles; i++)
        if (dom->def->consoles[i]->source.type == VIR_DOMAIN_CHR_TYPE_PTY)
        if (umlIdentifyOneChrPTY(driver, dom,
                                 dom->def->consoles[i], "con") < 0)
            return -1;

    for (i = 0 ; i < dom->def->nserials; i++)
        if (dom->def->serials[i]->source.type == VIR_DOMAIN_CHR_TYPE_PTY &&
            umlIdentifyOneChrPTY(driver, dom,
                                 dom->def->serials[i], "ssl") < 0)
            return -1;

    return 0;
}

static void
umlInotifyEvent(int watch,
                int fd,
                int events ATTRIBUTE_UNUSED,
                void *data)
{
    char buf[1024];
    struct inotify_event *e;
    int got;
    char *tmp, *name;
    struct uml_driver *driver = data;
    virDomainObjPtr dom;
    virDomainEventPtr event = NULL;

    umlDriverLock(driver);
    if (watch != driver->inotifyWatch)
        goto cleanup;

reread:
    got = read(fd, buf, sizeof(buf));
    if (got == -1) {
        if (errno == EINTR)
            goto reread;
        goto cleanup;
    }

    tmp = buf;
    while (got) {
        if (got < sizeof(struct inotify_event))
            goto cleanup; /* bad */

        e = (struct inotify_event *)tmp;
        tmp += sizeof(struct inotify_event);
        got -= sizeof(struct inotify_event);

        if (got < e->len)
            goto cleanup;

        tmp += e->len;
        got -= e->len;

        name = (char *)&(e->name);

        dom = virDomainFindByName(&driver->domains, name);

        if (!dom) {
            continue;
        }

        if (e->mask & IN_DELETE) {
            VIR_DEBUG("Got inotify domain shutdown '%s'", name);
            if (!virDomainObjIsActive(dom)) {
                virDomainObjUnlock(dom);
                continue;
            }

            umlShutdownVMDaemon(driver, dom, VIR_DOMAIN_SHUTOFF_SHUTDOWN);
            virDomainAuditStop(dom, "shutdown");
            event = virDomainEventNewFromObj(dom,
                                             VIR_DOMAIN_EVENT_STOPPED,
                                             VIR_DOMAIN_EVENT_STOPPED_SHUTDOWN);
            if (!dom->persistent) {
                virDomainRemoveInactive(&driver->domains,
                                        dom);
                dom = NULL;
            }
        } else if (e->mask & (IN_CREATE | IN_MODIFY)) {
            VIR_DEBUG("Got inotify domain startup '%s'", name);
            if (virDomainObjIsActive(dom)) {
                virDomainObjUnlock(dom);
                continue;
            }

            if (umlReadPidFile(driver, dom) < 0) {
                virDomainObjUnlock(dom);
                continue;
            }

            dom->def->id = driver->nextvmid++;
            virDomainObjSetState(dom, VIR_DOMAIN_RUNNING,
                                 VIR_DOMAIN_RUNNING_BOOTED);

            if (umlOpenMonitor(driver, dom) < 0) {
                VIR_WARN("Could not open monitor for new domain");
                umlShutdownVMDaemon(driver, dom,
                                    VIR_DOMAIN_SHUTOFF_FAILED);
                virDomainAuditStop(dom, "failed");
                event = virDomainEventNewFromObj(dom,
                                                 VIR_DOMAIN_EVENT_STOPPED,
                                                 VIR_DOMAIN_EVENT_STOPPED_FAILED);
                if (!dom->persistent) {
                    virDomainRemoveInactive(&driver->domains,
                                            dom);
                    dom = NULL;
                }
            } else if (umlIdentifyChrPTY(driver, dom) < 0) {
                VIR_WARN("Could not identify character devices for new domain");
                umlShutdownVMDaemon(driver, dom,
                                    VIR_DOMAIN_SHUTOFF_FAILED);
                virDomainAuditStop(dom, "failed");
                event = virDomainEventNewFromObj(dom,
                                                 VIR_DOMAIN_EVENT_STOPPED,
                                                 VIR_DOMAIN_EVENT_STOPPED_FAILED);
                if (!dom->persistent) {
                    virDomainRemoveInactive(&driver->domains,
                                            dom);
                    dom = NULL;
                }
            }
        }
        if (dom)
            virDomainObjUnlock(dom);
    }

cleanup:
    if (event)
        umlDomainEventQueue(driver, event);
    umlDriverUnlock(driver);
}

/**
 * umlStartup:
 *
 * Initialization function for the Uml daemon
 */
static int
umlStartup(int privileged)
{
    uid_t uid = geteuid();
    char *base = NULL;
    char *userdir = NULL;

    if (VIR_ALLOC(uml_driver) < 0)
        return -1;

    uml_driver->privileged = privileged;

    if (virMutexInit(&uml_driver->lock) < 0) {
        VIR_FREE(uml_driver);
        return -1;
    }
    umlDriverLock(uml_driver);

    /* Don't have a dom0 so start from 1 */
    uml_driver->nextvmid = 1;
    uml_driver->inotifyWatch = -1;

    if (virDomainObjListInit(&uml_driver->domains) < 0)
        goto error;

    uml_driver->domainEventState = virDomainEventStateNew();
    if (!uml_driver->domainEventState)
        goto error;

    userdir = virGetUserDirectory(uid);
    if (!userdir)
        goto error;

    if (privileged) {
        if (virAsprintf(&uml_driver->logDir,
                        "%s/log/libvirt/uml", LOCALSTATEDIR) == -1)
            goto out_of_memory;

        if ((base = strdup (SYSCONFDIR "/libvirt")) == NULL)
            goto out_of_memory;

        if (virAsprintf(&uml_driver->monitorDir,
                        "%s/run/libvirt/uml-guest", LOCALSTATEDIR) == -1)
            goto out_of_memory;
    } else {

        if (virAsprintf(&uml_driver->logDir,
                        "%s/.libvirt/uml/log", userdir) == -1)
            goto out_of_memory;

        if (virAsprintf(&base, "%s/.libvirt", userdir) == -1)
            goto out_of_memory;

        if (virAsprintf(&uml_driver->monitorDir,
                        "%s/.uml", userdir) == -1)
            goto out_of_memory;
    }

    /* Configuration paths are either ~/.libvirt/uml/... (session) or
     * /etc/libvirt/uml/... (system).
     */
    if (virAsprintf(&uml_driver->configDir, "%s/uml", base) == -1)
        goto out_of_memory;

    if (virAsprintf(&uml_driver->autostartDir, "%s/uml/autostart", base) == -1)
        goto out_of_memory;

    VIR_FREE(base);

    if ((uml_driver->caps = umlCapsInit()) == NULL)
        goto out_of_memory;

    uml_driver->caps->privateDataAllocFunc = umlDomainObjPrivateAlloc;
    uml_driver->caps->privateDataFreeFunc = umlDomainObjPrivateFree;

    if ((uml_driver->inotifyFD = inotify_init()) < 0) {
        VIR_ERROR(_("cannot initialize inotify"));
        goto error;
    }

    if (virFileMakePath(uml_driver->monitorDir) < 0) {
        char ebuf[1024];
        VIR_ERROR(_("Failed to create monitor directory %s: %s"),
                  uml_driver->monitorDir, virStrerror(errno, ebuf, sizeof(ebuf)));
        goto error;
    }

    VIR_INFO("Adding inotify watch on %s", uml_driver->monitorDir);
    if (inotify_add_watch(uml_driver->inotifyFD,
                          uml_driver->monitorDir,
                          IN_CREATE | IN_MODIFY | IN_DELETE) < 0) {
        goto error;
    }

    if ((uml_driver->inotifyWatch =
         virEventAddHandle(uml_driver->inotifyFD, POLLIN,
                           umlInotifyEvent, uml_driver, NULL)) < 0)
        goto error;

    if (umlProcessAutoDestroyInit(uml_driver) < 0)
        goto error;

    if (virDomainLoadAllConfigs(uml_driver->caps,
                                &uml_driver->domains,
                                uml_driver->configDir,
                                uml_driver->autostartDir,
                                0, 1 << VIR_DOMAIN_VIRT_UML,
                                NULL, NULL) < 0)
        goto error;

    umlDriverUnlock(uml_driver);

    umlAutostartConfigs(uml_driver);

    VIR_FREE(userdir);

    return 0;

out_of_memory:
    VIR_ERROR(_("umlStartup: out of memory"));

error:
    VIR_FREE(userdir);
    VIR_FREE(base);
    umlDriverUnlock(uml_driver);
    umlShutdown();
    return -1;
}

static void umlNotifyLoadDomain(virDomainObjPtr vm, int newVM, void *opaque)
{
    struct uml_driver *driver = opaque;

    if (newVM) {
        virDomainEventPtr event =
            virDomainEventNewFromObj(vm,
                                     VIR_DOMAIN_EVENT_DEFINED,
                                     VIR_DOMAIN_EVENT_DEFINED_ADDED);
        if (event)
            umlDomainEventQueue(driver, event);
    }
}


/**
 * umlReload:
 *
 * Function to restart the Uml daemon, it will recheck the configuration
 * files and update its state and the networking
 */
static int
umlReload(void) {
    if (!uml_driver)
        return 0;

    umlDriverLock(uml_driver);
    virDomainLoadAllConfigs(uml_driver->caps,
                            &uml_driver->domains,
                            uml_driver->configDir,
                            uml_driver->autostartDir,
                            0, 1 << VIR_DOMAIN_VIRT_UML,
                            umlNotifyLoadDomain, uml_driver);
    umlDriverUnlock(uml_driver);

    umlAutostartConfigs(uml_driver);

    return 0;
}

/**
 * umlActive:
 *
 * Checks if the Uml daemon is active, i.e. has an active domain or
 * an active network
 *
 * Returns 1 if active, 0 otherwise
 */
static int
umlActive(void) {
    int active = 0;

    if (!uml_driver)
        return 0;

    umlDriverLock(uml_driver);
    active = virDomainObjListNumOfDomains(&uml_driver->domains, 1);
    umlDriverUnlock(uml_driver);

    return active;
}

static void
umlShutdownOneVM(void *payload, const void *name ATTRIBUTE_UNUSED, void *opaque)
{
    virDomainObjPtr dom = payload;
    struct uml_driver *driver = opaque;

    virDomainObjLock(dom);
    if (virDomainObjIsActive(dom)) {
        umlShutdownVMDaemon(driver, dom, VIR_DOMAIN_SHUTOFF_SHUTDOWN);
        virDomainAuditStop(dom, "shutdown");
    }
    virDomainObjUnlock(dom);
}

/**
 * umlShutdown:
 *
 * Shutdown the Uml daemon, it will stop all active domains and networks
 */
static int
umlShutdown(void) {
    if (!uml_driver)
        return -1;

    umlDriverLock(uml_driver);
    if (uml_driver->inotifyWatch != -1)
        virEventRemoveHandle(uml_driver->inotifyWatch);
    VIR_FORCE_CLOSE(uml_driver->inotifyFD);
    virCapabilitiesFree(uml_driver->caps);

    /* shutdown active VMs
     * XXX allow them to stay around & reconnect */
    virHashForEach(uml_driver->domains.objs, umlShutdownOneVM, uml_driver);

    virDomainObjListDeinit(&uml_driver->domains);

    virDomainEventStateFree(uml_driver->domainEventState);

    VIR_FREE(uml_driver->logDir);
    VIR_FREE(uml_driver->configDir);
    VIR_FREE(uml_driver->autostartDir);
    VIR_FREE(uml_driver->monitorDir);

    umlProcessAutoDestroyShutdown(uml_driver);

    umlDriverUnlock(uml_driver);
    virMutexDestroy(&uml_driver->lock);
    VIR_FREE(uml_driver);

    return 0;
}


static int umlProcessAutoDestroyInit(struct uml_driver *driver)
{
    if (!(driver->autodestroy = virHashCreate(5, NULL)))
        return -1;

    return 0;
}

struct umlProcessAutoDestroyData {
    struct uml_driver *driver;
    virConnectPtr conn;
};

static void umlProcessAutoDestroyDom(void *payload,
                                     const void *name,
                                     void *opaque)
{
    struct umlProcessAutoDestroyData *data = opaque;
    virConnectPtr conn = payload;
    const char *uuidstr = name;
    unsigned char uuid[VIR_UUID_BUFLEN];
    virDomainObjPtr dom;
    virDomainEventPtr event = NULL;

    VIR_DEBUG("conn=%p uuidstr=%s thisconn=%p", conn, uuidstr, data->conn);

    if (data->conn != conn)
        return;

    if (virUUIDParse(uuidstr, uuid) < 0) {
        VIR_WARN("Failed to parse %s", uuidstr);
        return;
    }

    if (!(dom = virDomainFindByUUID(&data->driver->domains,
                                    uuid))) {
        VIR_DEBUG("No domain object to kill");
        return;
    }

    VIR_DEBUG("Killing domain");
    umlShutdownVMDaemon(data->driver, dom, VIR_DOMAIN_SHUTOFF_DESTROYED);
    virDomainAuditStop(dom, "destroyed");
    event = virDomainEventNewFromObj(dom,
                                     VIR_DOMAIN_EVENT_STOPPED,
                                     VIR_DOMAIN_EVENT_STOPPED_DESTROYED);

    if (dom && !dom->persistent)
        virDomainRemoveInactive(&data->driver->domains, dom);

    if (dom)
        virDomainObjUnlock(dom);
    if (event)
        umlDomainEventQueue(data->driver, event);
    virHashRemoveEntry(data->driver->autodestroy, uuidstr);
}

/*
 * Precondition: driver is locked
 */
static void umlProcessAutoDestroyRun(struct uml_driver *driver, virConnectPtr conn)
{
    struct umlProcessAutoDestroyData data = {
        driver, conn
    };
    VIR_DEBUG("conn=%p", conn);
    virHashForEach(driver->autodestroy, umlProcessAutoDestroyDom, &data);
}

static void umlProcessAutoDestroyShutdown(struct uml_driver *driver)
{
    virHashFree(driver->autodestroy);
}

static int umlProcessAutoDestroyAdd(struct uml_driver *driver,
                                    virDomainObjPtr vm,
                                    virConnectPtr conn)
{
    char uuidstr[VIR_UUID_STRING_BUFLEN];
    virUUIDFormat(vm->def->uuid, uuidstr);
    VIR_DEBUG("vm=%s uuid=%s conn=%p", vm->def->name, uuidstr, conn);
    if (virHashAddEntry(driver->autodestroy, uuidstr, conn) < 0)
        return -1;
    return 0;
}

static int umlProcessAutoDestroyRemove(struct uml_driver *driver,
                                       virDomainObjPtr vm)
{
    char uuidstr[VIR_UUID_STRING_BUFLEN];
    virUUIDFormat(vm->def->uuid, uuidstr);
    VIR_DEBUG("vm=%s uuid=%s", vm->def->name, uuidstr);
    if (virHashRemoveEntry(driver->autodestroy, uuidstr) < 0)
        return -1;
    return 0;
}


static int umlReadPidFile(struct uml_driver *driver,
                          virDomainObjPtr vm)
{
    int rc = -1;
    FILE *file;
    char *pidfile = NULL;
    int retries = 0;

    vm->pid = -1;
    if (virAsprintf(&pidfile, "%s/%s/pid",
                    driver->monitorDir, vm->def->name) < 0) {
        virReportOOMError();
        return -1;
    }

reopen:
    if (!(file = fopen(pidfile, "r"))) {
        if (errno == ENOENT &&
            retries++ < 50) {
            usleep(1000 * 100);
            goto reopen;
        }
        goto cleanup;
    }

    if (fscanf(file, "%d", &vm->pid) != 1) {
        errno = EINVAL;
        VIR_FORCE_FCLOSE(file);
        goto cleanup;
    }

    if (VIR_FCLOSE(file) < 0)
        goto cleanup;

    rc = 0;

 cleanup:
    if (rc != 0)
        virReportSystemError(errno,
                             _("failed to read pid: %s"),
                             pidfile);
    VIR_FREE(pidfile);
    return rc;
}

static int umlMonitorAddress(const struct uml_driver *driver,
                             virDomainObjPtr vm,
                             struct sockaddr_un *addr) {
    char *sockname;
    int retval = 0;

    if (virAsprintf(&sockname, "%s/%s/mconsole",
                    driver->monitorDir, vm->def->name) < 0) {
        virReportOOMError();
        return -1;
    }

    memset(addr, 0, sizeof(*addr));
    addr->sun_family = AF_UNIX;
    if (virStrcpyStatic(addr->sun_path, sockname) == NULL) {
        umlReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Unix path %s too long for destination"), sockname);
        retval = -1;
    }
    VIR_FREE(sockname);
    return retval;
}

static int umlOpenMonitor(struct uml_driver *driver,
                          virDomainObjPtr vm) {
    struct sockaddr_un addr;
    struct stat sb;
    int retries = 0;
    umlDomainObjPrivatePtr priv = vm->privateData;

    if (umlMonitorAddress(driver, vm, &addr) < 0)
        return -1;

    VIR_DEBUG("Dest address for monitor is '%s'", addr.sun_path);
restat:
    if (stat(addr.sun_path, &sb) < 0) {
        if (errno == ENOENT &&
            retries++ < 50) {
            usleep(1000 * 100);
            goto restat;
        }
        return -1;
    }

    if ((priv->monitor = socket(PF_UNIX, SOCK_DGRAM, 0)) < 0) {
        virReportSystemError(errno,
                             "%s", _("cannot open socket"));
        return -1;
    }

    memset(addr.sun_path, 0, sizeof(addr.sun_path));
    snprintf(addr.sun_path + 1, sizeof(addr.sun_path) - 1,
             "libvirt-uml-%u", vm->pid);
    VIR_DEBUG("Reply address for monitor is '%s'", addr.sun_path+1);
    if (bind(priv->monitor, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        virReportSystemError(errno,
                             "%s", _("cannot bind socket"));
        VIR_FORCE_CLOSE(priv->monitor);
        return -1;
    }

    return 0;
}


#define MONITOR_MAGIC 0xcafebabe
#define MONITOR_BUFLEN 512
#define MONITOR_VERSION 2

struct monitor_request {
    uint32_t magic;
    uint32_t version;
    uint32_t length;
    char data[MONITOR_BUFLEN];
};

struct monitor_response {
    uint32_t error;
    uint32_t extra;
    uint32_t length;
    char data[MONITOR_BUFLEN];
};


static int umlMonitorCommand(const struct uml_driver *driver,
                             const virDomainObjPtr vm,
                             const char *cmd,
                             char **reply)
{
    struct monitor_request req;
    struct monitor_response res;
    char *retdata = NULL;
    int retlen = 0, ret = 0;
    struct sockaddr_un addr;
    unsigned int addrlen;
    umlDomainObjPrivatePtr priv = vm->privateData;

    VIR_DEBUG("Run command '%s'", cmd);

    *reply = NULL;

    if (umlMonitorAddress(driver, vm, &addr) < 0)
        return -1;

    memset(&req, 0, sizeof(req));
    req.magic = MONITOR_MAGIC;
    req.version = MONITOR_VERSION;
    req.length = strlen(cmd);
    if (req.length > (MONITOR_BUFLEN-1)) {
        virReportSystemError(EINVAL,
                             _("cannot send too long command %s (%d bytes)"),
                             cmd, req.length);
        return -1;
    }
    if (virStrcpyStatic(req.data, cmd) == NULL) {
        umlReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Command %s too long for destination"), cmd);
        return -1;
    }

    if (sendto(priv->monitor, &req, sizeof(req), 0,
               (struct sockaddr *)&addr, sizeof(addr)) != sizeof(req)) {
        virReportSystemError(errno,
                             _("cannot send command %s"),
                             cmd);
        return -1;
    }

    do {
        ssize_t nbytes;
        addrlen = sizeof(addr);
        nbytes = recvfrom(priv->monitor, &res, sizeof(res), 0,
                          (struct sockaddr *)&addr, &addrlen);
        if (nbytes < 0) {
            if (errno == EAGAIN || errno == EINTR)
                continue;
            virReportSystemError(errno, _("cannot read reply %s"), cmd);
            goto error;
        }
        /* Ensure res.length is safe to read before validating its value.  */
        if (nbytes < offsetof(struct monitor_request, data) ||
            nbytes < offsetof(struct monitor_request, data) + res.length) {
            virReportSystemError(0, _("incomplete reply %s"), cmd);
            goto error;
        }

        if (VIR_REALLOC_N(retdata, retlen + res.length) < 0) {
            virReportOOMError();
            goto error;
        }
        memcpy(retdata + retlen, res.data, res.length);
        retlen += res.length - 1;
        retdata[retlen] = '\0';

        if (res.error)
            ret = -1;

    } while (res.extra);

    VIR_DEBUG("Command reply is '%s'", NULLSTR(retdata));

    if (ret < 0)
        VIR_FREE(retdata);
    else
        *reply = retdata;

    return ret;

error:
    VIR_FREE(retdata);
    return -1;
}


static void umlCleanupTapDevices(virDomainObjPtr vm) {
    int i;

    for (i = 0 ; i < vm->def->nnets ; i++) {
        virDomainNetDefPtr def = vm->def->nets[i];

        if (def->type != VIR_DOMAIN_NET_TYPE_BRIDGE &&
            def->type != VIR_DOMAIN_NET_TYPE_NETWORK)
            continue;

        ignore_value(virNetDevTapDelete(def->ifname));
    }
}

static int umlStartVMDaemon(virConnectPtr conn,
                            struct uml_driver *driver,
                            virDomainObjPtr vm,
                            bool autoDestroy) {
    int ret = -1;
    char *logfile;
    int logfd = -1;
    umlDomainObjPrivatePtr priv = vm->privateData;
    virCommandPtr cmd = NULL;
    size_t i;

    if (virDomainObjIsActive(vm)) {
        umlReportError(VIR_ERR_OPERATION_INVALID, "%s",
                       _("VM is already active"));
        return -1;
    }

    if (!vm->def->os.kernel) {
        umlReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("no kernel specified"));
        return -1;
    }
    /* Make sure the binary we are about to try exec'ing exists.
     * Technically we could catch the exec() failure, but that's
     * in a sub-process so its hard to feed back a useful error
     */
    if (!virFileIsExecutable(vm->def->os.kernel)) {
        virReportSystemError(errno,
                             _("Cannot find UML kernel %s"),
                             vm->def->os.kernel);
        return -1;
    }

    if (virFileMakePath(driver->logDir) < 0) {
        virReportSystemError(errno,
                             _("cannot create log directory %s"),
                             driver->logDir);
        return -1;
    }

    if (virAsprintf(&logfile, "%s/%s.log",
                    driver->logDir, vm->def->name) < 0) {
        virReportOOMError();
        return -1;
    }

    if ((logfd = open(logfile, O_CREAT | O_TRUNC | O_WRONLY,
                      S_IRUSR | S_IWUSR)) < 0) {
        virReportSystemError(errno,
                             _("failed to create logfile %s"),
                             logfile);
        VIR_FREE(logfile);
        return -1;
    }
    VIR_FREE(logfile);

    if (virSetCloseExec(logfd) < 0) {
        virReportSystemError(errno, "%s",
                             _("Unable to set VM logfile close-on-exec flag"));
        VIR_FORCE_CLOSE(logfd);
        return -1;
    }

    /* Do this upfront, so any part of the startup process can add
     * runtime state to vm->def that won't be persisted. This let's us
     * report implicit runtime defaults in the XML, like vnc listen/socket
     */
    VIR_DEBUG("Setting current domain def as transient");
    if (virDomainObjSetDefTransient(driver->caps, vm, true) < 0) {
        VIR_FORCE_CLOSE(logfd);
        return -1;
    }

    if (!(cmd = umlBuildCommandLine(conn, driver, vm)))
        goto cleanup;

    for (i = 0 ; i < vm->def->nconsoles ; i++) {
        VIR_FREE(vm->def->consoles[i]->info.alias);
        if (virAsprintf(&vm->def->consoles[i]->info.alias, "console%zu", i) < 0) {
            virReportOOMError();
            goto cleanup;
        }
    }

    virCommandWriteArgLog(cmd, logfd);

    priv->monitor = -1;

    virCommandClearCaps(cmd);
    virCommandSetOutputFD(cmd, &logfd);
    virCommandSetErrorFD(cmd, &logfd);
    virCommandDaemonize(cmd);

    ret = virCommandRun(cmd, NULL);
    if (ret < 0)
        goto cleanup;

    if (autoDestroy &&
        (ret = umlProcessAutoDestroyAdd(driver, vm, conn)) < 0)
        goto cleanup;

    ret = virDomainObjSetDefTransient(driver->caps, vm, false);
cleanup:
    VIR_FORCE_CLOSE(logfd);
    virCommandFree(cmd);

    if (ret < 0) {
        virDomainConfVMNWFilterTeardown(vm);
        umlCleanupTapDevices(vm);
        if (vm->newDef) {
            virDomainDefFree(vm->def);
            vm->def = vm->newDef;
            vm->def->id = -1;
            vm->newDef = NULL;
        }
    }

    /* NB we don't mark it running here - we do that async
       with inotify */
    /* XXX what if someone else tries to start it again
       before we get the inotification ? Sounds like
       trouble.... */
    /* XXX this is bad for events too. must fix this better */

    return ret;
}

static void umlShutdownVMDaemon(struct uml_driver *driver,
                                virDomainObjPtr vm,
                                virDomainShutoffReason reason)
{
    int ret;
    umlDomainObjPrivatePtr priv = vm->privateData;

    if (!virDomainObjIsActive(vm))
        return;

    virProcessKill(vm->pid, SIGTERM);

    VIR_FORCE_CLOSE(priv->monitor);

    if ((ret = waitpid(vm->pid, NULL, 0)) != vm->pid) {
        VIR_WARN("Got unexpected pid %d != %d",
               ret, vm->pid);
    }

    vm->pid = -1;
    vm->def->id = -1;
    virDomainObjSetState(vm, VIR_DOMAIN_SHUTOFF, reason);

    virDomainConfVMNWFilterTeardown(vm);
    umlCleanupTapDevices(vm);

    /* Stop autodestroy in case guest is restarted */
    umlProcessAutoDestroyRemove(driver, vm);

    if (vm->newDef) {
        virDomainDefFree(vm->def);
        vm->def = vm->newDef;
        vm->def->id = -1;
        vm->newDef = NULL;
    }
}


static virDrvOpenStatus umlOpen(virConnectPtr conn,
                                virConnectAuthPtr auth ATTRIBUTE_UNUSED,
                                unsigned int flags)
{
    virCheckFlags(VIR_CONNECT_RO, VIR_DRV_OPEN_ERROR);

    if (conn->uri == NULL) {
        if (uml_driver == NULL)
            return VIR_DRV_OPEN_DECLINED;

        if (!(conn->uri = virURIParse(uml_driver->privileged ?
                                      "uml:///system" :
                                      "uml:///session")))
            return VIR_DRV_OPEN_ERROR;
    } else {
        if (conn->uri->scheme == NULL ||
            STRNEQ (conn->uri->scheme, "uml"))
            return VIR_DRV_OPEN_DECLINED;

        /* Allow remote driver to deal with URIs with hostname server */
        if (conn->uri->server != NULL)
            return VIR_DRV_OPEN_DECLINED;


        /* Check path and tell them correct path if they made a mistake */
        if (uml_driver->privileged) {
            if (STRNEQ (conn->uri->path, "/system") &&
                STRNEQ (conn->uri->path, "/session")) {
                umlReportError(VIR_ERR_INTERNAL_ERROR,
                               _("unexpected UML URI path '%s', try uml:///system"),
                               conn->uri->path);
                return VIR_DRV_OPEN_ERROR;
            }
        } else {
            if (STRNEQ (conn->uri->path, "/session")) {
                umlReportError(VIR_ERR_INTERNAL_ERROR,
                               _("unexpected UML URI path '%s', try uml:///session"),
                               conn->uri->path);
                return VIR_DRV_OPEN_ERROR;
            }
        }

        /* URI was good, but driver isn't active */
        if (uml_driver == NULL) {
            umlReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("uml state driver is not active"));
            return VIR_DRV_OPEN_ERROR;
        }
    }

    conn->privateData = uml_driver;

    return VIR_DRV_OPEN_SUCCESS;
}

static int umlClose(virConnectPtr conn) {
    struct uml_driver *driver = conn->privateData;

    umlDriverLock(driver);
    virDomainEventStateDeregisterConn(conn,
                                      driver->domainEventState);
    umlProcessAutoDestroyRun(driver, conn);
    umlDriverUnlock(driver);

    conn->privateData = NULL;

    return 0;
}

static const char *umlGetType(virConnectPtr conn ATTRIBUTE_UNUSED) {
    return "UML";
}


static int umlIsSecure(virConnectPtr conn ATTRIBUTE_UNUSED)
{
    /* Trivially secure, since always inside the daemon */
    return 1;
}


static int umlIsEncrypted(virConnectPtr conn ATTRIBUTE_UNUSED)
{
    /* Not encrypted, but remote driver takes care of that */
    return 0;
}


static int umlIsAlive(virConnectPtr conn ATTRIBUTE_UNUSED)
{
    return 1;
}


static char *umlGetCapabilities(virConnectPtr conn) {
    struct uml_driver *driver = (struct uml_driver *)conn->privateData;
    char *xml;

    umlDriverLock(driver);
    if ((xml = virCapabilitiesFormatXML(driver->caps)) == NULL)
        virReportOOMError();
    umlDriverUnlock(driver);

    return xml;
}



static int umlGetProcessInfo(unsigned long long *cpuTime, pid_t pid)
{
    char *proc;
    FILE *pidinfo;
    unsigned long long usertime, systime;

    if (virAsprintf(&proc, "/proc/%lld/stat", (long long) pid) < 0) {
        return -1;
    }

    if (!(pidinfo = fopen(proc, "r"))) {
        /* VM probably shut down, so fake 0 */
        *cpuTime = 0;
        VIR_FREE(proc);
        return 0;
    }

    VIR_FREE(proc);

    if (fscanf(pidinfo, "%*d %*s %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &usertime, &systime) != 2) {
        umlDebug("not enough arg");
        VIR_FORCE_FCLOSE(pidinfo);
        return -1;
    }

    /* We got jiffies
     * We want nanoseconds
     * _SC_CLK_TCK is jiffies per second
     * So calulate thus....
     */
    *cpuTime = 1000ull * 1000ull * 1000ull * (usertime + systime) / (unsigned long long)sysconf(_SC_CLK_TCK);

    umlDebug("Got %llu %llu %llu", usertime, systime, *cpuTime);

    VIR_FORCE_FCLOSE(pidinfo);

    return 0;
}


static virDomainPtr umlDomainLookupByID(virConnectPtr conn,
                                          int id) {
    struct uml_driver *driver = (struct uml_driver *)conn->privateData;
    virDomainObjPtr vm;
    virDomainPtr dom = NULL;

    umlDriverLock(driver);
    vm = virDomainFindByID(&driver->domains, id);
    umlDriverUnlock(driver);

    if (!vm) {
        umlReportError(VIR_ERR_NO_DOMAIN, NULL);
        goto cleanup;
    }

    dom = virGetDomain(conn, vm->def->name, vm->def->uuid);
    if (dom) dom->id = vm->def->id;

cleanup:
    if (vm)
        virDomainObjUnlock(vm);
    return dom;
}

static virDomainPtr umlDomainLookupByUUID(virConnectPtr conn,
                                            const unsigned char *uuid) {
    struct uml_driver *driver = (struct uml_driver *)conn->privateData;
    virDomainObjPtr vm;
    virDomainPtr dom = NULL;

    umlDriverLock(driver);
    vm = virDomainFindByUUID(&driver->domains, uuid);
    umlDriverUnlock(driver);

    if (!vm) {
        umlReportError(VIR_ERR_NO_DOMAIN, NULL);
        goto cleanup;
    }

    dom = virGetDomain(conn, vm->def->name, vm->def->uuid);
    if (dom) dom->id = vm->def->id;

cleanup:
    if (vm)
        virDomainObjUnlock(vm);
    return dom;
}

static virDomainPtr umlDomainLookupByName(virConnectPtr conn,
                                            const char *name) {
    struct uml_driver *driver = (struct uml_driver *)conn->privateData;
    virDomainObjPtr vm;
    virDomainPtr dom = NULL;

    umlDriverLock(driver);
    vm = virDomainFindByName(&driver->domains, name);
    umlDriverUnlock(driver);

    if (!vm) {
        umlReportError(VIR_ERR_NO_DOMAIN, NULL);
        goto cleanup;
    }

    dom = virGetDomain(conn, vm->def->name, vm->def->uuid);
    if (dom) dom->id = vm->def->id;

cleanup:
    if (vm)
        virDomainObjUnlock(vm);
    return dom;
}


static int umlDomainIsActive(virDomainPtr dom)
{
    struct uml_driver *driver = dom->conn->privateData;
    virDomainObjPtr obj;
    int ret = -1;

    umlDriverLock(driver);
    obj = virDomainFindByUUID(&driver->domains, dom->uuid);
    umlDriverUnlock(driver);
    if (!obj) {
        umlReportError(VIR_ERR_NO_DOMAIN, NULL);
        goto cleanup;
    }
    ret = virDomainObjIsActive(obj);

cleanup:
    if (obj)
        virDomainObjUnlock(obj);
    return ret;
}


static int umlDomainIsPersistent(virDomainPtr dom)
{
    struct uml_driver *driver = dom->conn->privateData;
    virDomainObjPtr obj;
    int ret = -1;

    umlDriverLock(driver);
    obj = virDomainFindByUUID(&driver->domains, dom->uuid);
    umlDriverUnlock(driver);
    if (!obj) {
        umlReportError(VIR_ERR_NO_DOMAIN, NULL);
        goto cleanup;
    }
    ret = obj->persistent;

cleanup:
    if (obj)
        virDomainObjUnlock(obj);
    return ret;
}

static int umlDomainIsUpdated(virDomainPtr dom)
{
    struct uml_driver *driver = dom->conn->privateData;
    virDomainObjPtr obj;
    int ret = -1;

    umlDriverLock(driver);
    obj = virDomainFindByUUID(&driver->domains, dom->uuid);
    umlDriverUnlock(driver);
    if (!obj) {
        umlReportError(VIR_ERR_NO_DOMAIN, NULL);
        goto cleanup;
    }
    ret = obj->updated;

cleanup:
    if (obj)
        virDomainObjUnlock(obj);
    return ret;
}

static int umlGetVersion(virConnectPtr conn, unsigned long *version) {
    struct uml_driver *driver = conn->privateData;
    struct utsname ut;
    int ret = -1;

    umlDriverLock(driver);

    if (driver->umlVersion == 0) {
        uname(&ut);

        if (virParseVersionString(ut.release, &driver->umlVersion, true) < 0) {
            umlReportError(VIR_ERR_INTERNAL_ERROR,
                           _("cannot parse version %s"), ut.release);
            goto cleanup;
        }
    }

    *version = driver->umlVersion;
    ret = 0;

cleanup:
    umlDriverUnlock(driver);
    return ret;
}

static int umlListDomains(virConnectPtr conn, int *ids, int nids) {
    struct uml_driver *driver = conn->privateData;
    int n;

    umlDriverLock(driver);
    n = virDomainObjListGetActiveIDs(&driver->domains, ids, nids);
    umlDriverUnlock(driver);

    return n;
}
static int umlNumDomains(virConnectPtr conn) {
    struct uml_driver *driver = conn->privateData;
    int n;

    umlDriverLock(driver);
    n = virDomainObjListNumOfDomains(&driver->domains, 1);
    umlDriverUnlock(driver);

    return n;
}
static virDomainPtr umlDomainCreate(virConnectPtr conn, const char *xml,
                                      unsigned int flags) {
    struct uml_driver *driver = conn->privateData;
    virDomainDefPtr def;
    virDomainObjPtr vm = NULL;
    virDomainPtr dom = NULL;
    virDomainEventPtr event = NULL;

    virCheckFlags(VIR_DOMAIN_START_AUTODESTROY, NULL);

    umlDriverLock(driver);
    if (!(def = virDomainDefParseString(driver->caps, xml,
                                        1 << VIR_DOMAIN_VIRT_UML,
                                        VIR_DOMAIN_XML_INACTIVE)))
        goto cleanup;

    if (virDomainObjIsDuplicate(&driver->domains, def, 1) < 0)
        goto cleanup;

    if (!(vm = virDomainAssignDef(driver->caps,
                                  &driver->domains,
                                  def, false)))
        goto cleanup;
    def = NULL;

    if (umlStartVMDaemon(conn, driver, vm,
                         (flags & VIR_DOMAIN_START_AUTODESTROY)) < 0) {
        virDomainAuditStart(vm, "booted", false);
        virDomainRemoveInactive(&driver->domains,
                                vm);
        vm = NULL;
        goto cleanup;
    }
    virDomainAuditStart(vm, "booted", true);
    event = virDomainEventNewFromObj(vm,
                                     VIR_DOMAIN_EVENT_STARTED,
                                     VIR_DOMAIN_EVENT_STARTED_BOOTED);

    dom = virGetDomain(conn, vm->def->name, vm->def->uuid);
    if (dom) dom->id = vm->def->id;

cleanup:
    virDomainDefFree(def);
    if (vm)
        virDomainObjUnlock(vm);
    if (event)
        umlDomainEventQueue(driver, event);
    umlDriverUnlock(driver);
    return dom;
}


static int umlDomainShutdownFlags(virDomainPtr dom,
                                  unsigned int flags) {
    struct uml_driver *driver = dom->conn->privateData;
    virDomainObjPtr vm;
    char *info = NULL;
    int ret = -1;

    virCheckFlags(0, -1);

    umlDriverLock(driver);
    vm = virDomainFindByID(&driver->domains, dom->id);
    umlDriverUnlock(driver);
    if (!vm) {
        umlReportError(VIR_ERR_NO_DOMAIN,
                       _("no domain with matching id %d"), dom->id);
        goto cleanup;
    }

#if 0
    if (umlMonitorCommand(driver, vm, "system_powerdown", &info) < 0) {
        umlReportError(VIR_ERR_OPERATION_FAILED, "%s",
                       _("shutdown operation failed"));
        goto cleanup;
    }
    ret = 0;
#endif

cleanup:
    VIR_FREE(info);
    if (vm)
        virDomainObjUnlock(vm);
    return ret;
}

static int
umlDomainShutdown(virDomainPtr dom)
{
    return umlDomainShutdownFlags(dom, 0);
}

static int
umlDomainDestroyFlags(virDomainPtr dom,
                      unsigned int flags)
{
    struct uml_driver *driver = dom->conn->privateData;
    virDomainObjPtr vm;
    virDomainEventPtr event = NULL;
    int ret = -1;

    virCheckFlags(0, -1);

    umlDriverLock(driver);
    vm = virDomainFindByID(&driver->domains, dom->id);
    if (!vm) {
        umlReportError(VIR_ERR_NO_DOMAIN,
                       _("no domain with matching id %d"), dom->id);
        goto cleanup;
    }

    umlShutdownVMDaemon(driver, vm, VIR_DOMAIN_SHUTOFF_DESTROYED);
    virDomainAuditStop(vm, "destroyed");
    event = virDomainEventNewFromObj(vm,
                                     VIR_DOMAIN_EVENT_STOPPED,
                                     VIR_DOMAIN_EVENT_STOPPED_DESTROYED);
    if (!vm->persistent) {
        virDomainRemoveInactive(&driver->domains,
                                vm);
        vm = NULL;
    }
    ret = 0;

cleanup:
    if (vm)
        virDomainObjUnlock(vm);
    if (event)
        umlDomainEventQueue(driver, event);
    umlDriverUnlock(driver);
    return ret;
}


static int umlDomainDestroy(virDomainPtr dom)
{
    return umlDomainDestroyFlags(dom, 0);
}


static char *umlDomainGetOSType(virDomainPtr dom) {
    struct uml_driver *driver = dom->conn->privateData;
    virDomainObjPtr vm;
    char *type = NULL;

    umlDriverLock(driver);
    vm = virDomainFindByUUID(&driver->domains, dom->uuid);
    umlDriverUnlock(driver);
    if (!vm) {
        umlReportError(VIR_ERR_NO_DOMAIN, "%s",
                       _("no domain with matching uuid"));
        goto cleanup;
    }

    if (!(type = strdup(vm->def->os.type)))
        virReportOOMError();

cleanup:
    if (vm)
        virDomainObjUnlock(vm);
    return type;
}

/* Returns max memory in kb, 0 if error */
static unsigned long long
umlDomainGetMaxMemory(virDomainPtr dom)
{
    struct uml_driver *driver = dom->conn->privateData;
    virDomainObjPtr vm;
    unsigned long long ret = 0;

    umlDriverLock(driver);
    vm = virDomainFindByUUID(&driver->domains, dom->uuid);
    umlDriverUnlock(driver);

    if (!vm) {
        char uuidstr[VIR_UUID_STRING_BUFLEN];

        virUUIDFormat(dom->uuid, uuidstr);
        umlReportError(VIR_ERR_NO_DOMAIN,
                       _("no domain with matching uuid '%s'"), uuidstr);
        goto cleanup;
    }
    ret = vm->def->mem.max_balloon;

cleanup:
    if (vm)
        virDomainObjUnlock(vm);
    return ret;
}

static int umlDomainSetMaxMemory(virDomainPtr dom, unsigned long newmax) {
    struct uml_driver *driver = dom->conn->privateData;
    virDomainObjPtr vm;
    int ret = -1;

    umlDriverLock(driver);
    vm = virDomainFindByUUID(&driver->domains, dom->uuid);
    umlDriverUnlock(driver);

    if (!vm) {
        char uuidstr[VIR_UUID_STRING_BUFLEN];

        virUUIDFormat(dom->uuid, uuidstr);
        umlReportError(VIR_ERR_NO_DOMAIN,
                       _("no domain with matching uuid '%s'"), uuidstr);
        goto cleanup;
    }

    if (newmax < vm->def->mem.cur_balloon) {
        umlReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("cannot set max memory lower than current memory"));
        goto cleanup;
    }

    vm->def->mem.max_balloon = newmax;
    ret = 0;

cleanup:
    if (vm)
        virDomainObjUnlock(vm);
    return ret;
}

static int umlDomainSetMemory(virDomainPtr dom, unsigned long newmem) {
    struct uml_driver *driver = dom->conn->privateData;
    virDomainObjPtr vm;
    int ret = -1;

    umlDriverLock(driver);
    vm = virDomainFindByUUID(&driver->domains, dom->uuid);
    umlDriverUnlock(driver);

    if (!vm) {
        char uuidstr[VIR_UUID_STRING_BUFLEN];

        virUUIDFormat(dom->uuid, uuidstr);
        umlReportError(VIR_ERR_NO_DOMAIN,
                       _("no domain with matching uuid '%s'"), uuidstr);
        goto cleanup;
    }

    if (virDomainObjIsActive(vm)) {
        umlReportError(VIR_ERR_OPERATION_INVALID, "%s",
                       _("cannot set memory of an active domain"));
        goto cleanup;
    }

    if (newmem > vm->def->mem.max_balloon) {
        umlReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("cannot set memory higher than max memory"));
        goto cleanup;
    }

    vm->def->mem.cur_balloon = newmem;
    ret = 0;

cleanup:
    if (vm)
        virDomainObjUnlock(vm);
    return ret;
}

static int umlDomainGetInfo(virDomainPtr dom,
                              virDomainInfoPtr info) {
    struct uml_driver *driver = dom->conn->privateData;
    virDomainObjPtr vm;
    int ret = -1;

    umlDriverLock(driver);
    vm = virDomainFindByUUID(&driver->domains, dom->uuid);
    umlDriverUnlock(driver);

    if (!vm) {
        umlReportError(VIR_ERR_NO_DOMAIN, "%s",
                       _("no domain with matching uuid"));
        goto cleanup;
    }

    info->state = virDomainObjGetState(vm, NULL);

    if (!virDomainObjIsActive(vm)) {
        info->cpuTime = 0;
    } else {
        if (umlGetProcessInfo(&(info->cpuTime), vm->pid) < 0) {
            umlReportError(VIR_ERR_OPERATION_FAILED, "%s",
                           _("cannot read cputime for domain"));
            goto cleanup;
        }
    }

    info->maxMem = vm->def->mem.max_balloon;
    info->memory = vm->def->mem.cur_balloon;
    info->nrVirtCpu = vm->def->vcpus;
    ret = 0;

cleanup:
    if (vm)
        virDomainObjUnlock(vm);
    return ret;
}


static int
umlDomainGetState(virDomainPtr dom,
                  int *state,
                  int *reason,
                  unsigned int flags)
{
    struct uml_driver *driver = dom->conn->privateData;
    virDomainObjPtr vm;
    int ret = -1;

    virCheckFlags(0, -1);

    umlDriverLock(driver);
    vm = virDomainFindByUUID(&driver->domains, dom->uuid);
    umlDriverUnlock(driver);

    if (!vm) {
        umlReportError(VIR_ERR_NO_DOMAIN, "%s",
                       _("no domain with matching uuid"));
        goto cleanup;
    }

    *state = virDomainObjGetState(vm, reason);
    ret = 0;

cleanup:
    if (vm)
        virDomainObjUnlock(vm);
    return ret;
}


static char *umlDomainGetXMLDesc(virDomainPtr dom,
                                 unsigned int flags)
{
    struct uml_driver *driver = dom->conn->privateData;
    virDomainObjPtr vm;
    char *ret = NULL;

    /* Flags checked by virDomainDefFormat */

    umlDriverLock(driver);
    vm = virDomainFindByUUID(&driver->domains, dom->uuid);
    umlDriverUnlock(driver);

    if (!vm) {
        umlReportError(VIR_ERR_NO_DOMAIN, "%s",
                       _("no domain with matching uuid"));
        goto cleanup;
    }

    ret = virDomainDefFormat((flags & VIR_DOMAIN_XML_INACTIVE) && vm->newDef ?
                             vm->newDef : vm->def,
                             flags);

cleanup:
    if (vm)
        virDomainObjUnlock(vm);
    return ret;
}


static int umlListDefinedDomains(virConnectPtr conn,
                            char **const names, int nnames) {
    struct uml_driver *driver = conn->privateData;
    int n;

    umlDriverLock(driver);
    n = virDomainObjListGetInactiveNames(&driver->domains, names, nnames);
    umlDriverUnlock(driver);

    return n;
}

static int umlNumDefinedDomains(virConnectPtr conn) {
    struct uml_driver *driver = conn->privateData;
    int n;

    umlDriverLock(driver);
    n = virDomainObjListNumOfDomains(&driver->domains, 0);
    umlDriverUnlock(driver);

    return n;
}


static int umlDomainStartWithFlags(virDomainPtr dom, unsigned int flags) {
    struct uml_driver *driver = dom->conn->privateData;
    virDomainObjPtr vm;
    virDomainEventPtr event = NULL;
    int ret = -1;

    virCheckFlags(VIR_DOMAIN_START_AUTODESTROY, -1);

    umlDriverLock(driver);
    vm = virDomainFindByUUID(&driver->domains, dom->uuid);

    if (!vm) {
        umlReportError(VIR_ERR_NO_DOMAIN, "%s",
                       _("no domain with matching uuid"));
        goto cleanup;
    }

    ret = umlStartVMDaemon(dom->conn, driver, vm,
                           (flags & VIR_DOMAIN_START_AUTODESTROY));
    virDomainAuditStart(vm, "booted", ret >= 0);
    if (ret == 0)
        event = virDomainEventNewFromObj(vm,
                                         VIR_DOMAIN_EVENT_STARTED,
                                         VIR_DOMAIN_EVENT_STARTED_BOOTED);

cleanup:
    if (vm)
        virDomainObjUnlock(vm);
    if (event)
        umlDomainEventQueue(driver, event);
    umlDriverUnlock(driver);
    return ret;
}

static int umlDomainStart(virDomainPtr dom) {
    return umlDomainStartWithFlags(dom, 0);
}

static virDomainPtr umlDomainDefine(virConnectPtr conn, const char *xml) {
    struct uml_driver *driver = conn->privateData;
    virDomainDefPtr def;
    virDomainObjPtr vm = NULL;
    virDomainPtr dom = NULL;

    umlDriverLock(driver);
    if (!(def = virDomainDefParseString(driver->caps, xml,
                                        1 << VIR_DOMAIN_VIRT_UML,
                                        VIR_DOMAIN_XML_INACTIVE)))
        goto cleanup;

    if (virDomainObjIsDuplicate(&driver->domains, def, 0) < 0)
        goto cleanup;

    if (!(vm = virDomainAssignDef(driver->caps,
                                  &driver->domains,
                                  def, false)))
        goto cleanup;
    def = NULL;
    vm->persistent = 1;

    if (virDomainSaveConfig(driver->configDir,
                            vm->newDef ? vm->newDef : vm->def) < 0) {
        virDomainRemoveInactive(&driver->domains,
                                vm);
        vm = NULL;
        goto cleanup;
    }

    dom = virGetDomain(conn, vm->def->name, vm->def->uuid);
    if (dom) dom->id = vm->def->id;

cleanup:
    virDomainDefFree(def);
    if (vm)
        virDomainObjUnlock(vm);
    umlDriverUnlock(driver);
    return dom;
}

static int umlDomainUndefineFlags(virDomainPtr dom,
                                  unsigned int flags)
{
    struct uml_driver *driver = dom->conn->privateData;
    virDomainObjPtr vm;
    int ret = -1;

    virCheckFlags(0, -1);

    umlDriverLock(driver);
    vm = virDomainFindByUUID(&driver->domains, dom->uuid);
    if (!vm) {
        umlReportError(VIR_ERR_NO_DOMAIN, "%s",
                       _("no domain with matching uuid"));
        goto cleanup;
    }

    if (!vm->persistent) {
        umlReportError(VIR_ERR_OPERATION_INVALID, "%s",
                       _("cannot undefine transient domain"));
        goto cleanup;
    }

    if (virDomainDeleteConfig(driver->configDir, driver->autostartDir, vm) < 0)
        goto cleanup;

    if (virDomainObjIsActive(vm)) {
        vm->persistent = 0;
    } else {
        virDomainRemoveInactive(&driver->domains, vm);
        vm = NULL;
    }

    ret = 0;

cleanup:
    if (vm)
        virDomainObjUnlock(vm);
    umlDriverUnlock(driver);
    return ret;
}


static int umlDomainUndefine(virDomainPtr dom)
{
    return umlDomainUndefineFlags(dom, 0);
}

static int umlDomainAttachUmlDisk(struct uml_driver *driver,
                                  virDomainObjPtr vm,
                                  virDomainDiskDefPtr disk)
{
    int i;
    char *cmd = NULL;
    char *reply = NULL;

    for (i = 0 ; i < vm->def->ndisks ; i++) {
        if (STREQ(vm->def->disks[i]->dst, disk->dst)) {
            umlReportError(VIR_ERR_OPERATION_FAILED,
                           _("target %s already exists"), disk->dst);
            return -1;
        }
    }

    if (!disk->src) {
        umlReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("disk source path is missing"));
        goto error;
    }

    if (virAsprintf(&cmd, "config %s=%s", disk->dst, disk->src) < 0) {
        virReportOOMError();
        return -1;
    }

    if (umlMonitorCommand(driver, vm, cmd, &reply) < 0)
        goto error;

    if (VIR_REALLOC_N(vm->def->disks, vm->def->ndisks+1) < 0) {
        virReportOOMError();
        goto error;
    }

    virDomainDiskInsertPreAlloced(vm->def, disk);

    VIR_FREE(reply);
    VIR_FREE(cmd);

    return 0;

error:

    VIR_FREE(reply);
    VIR_FREE(cmd);

    return -1;
}


static int umlDomainAttachDevice(virDomainPtr dom, const char *xml)
{
    struct uml_driver *driver = dom->conn->privateData;
    virDomainObjPtr vm;
    virDomainDeviceDefPtr dev = NULL;
    int ret = -1;

    umlDriverLock(driver);

    vm = virDomainFindByUUID(&driver->domains, dom->uuid);
    if (!vm) {
        char uuidstr[VIR_UUID_STRING_BUFLEN];
        virUUIDFormat(dom->uuid, uuidstr);
        umlReportError(VIR_ERR_NO_DOMAIN,
                       _("no domain with matching uuid '%s'"), uuidstr);
        goto cleanup;
    }

    if (!virDomainObjIsActive(vm)) {
        umlReportError(VIR_ERR_OPERATION_INVALID,
                       "%s", _("cannot attach device on inactive domain"));
        goto cleanup;
    }

    dev = virDomainDeviceDefParse(driver->caps, vm->def, xml,
                                  VIR_DOMAIN_XML_INACTIVE);

    if (dev == NULL)
        goto cleanup;

    if (dev->type == VIR_DOMAIN_DEVICE_DISK) {
        if (dev->data.disk->bus == VIR_DOMAIN_DISK_BUS_UML) {
            ret = umlDomainAttachUmlDisk(driver, vm, dev->data.disk);
            if (ret == 0)
                dev->data.disk = NULL;
        } else {
            umlReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("disk bus '%s' cannot be hotplugged."),
                           virDomainDiskBusTypeToString(dev->data.disk->bus));
        }
    } else {
        umlReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("device type '%s' cannot be attached"),
                       virDomainDeviceTypeToString(dev->type));
        goto cleanup;
    }

cleanup:

    virDomainDeviceDefFree(dev);
    if (vm)
        virDomainObjUnlock(vm);
    umlDriverUnlock(driver);
    return ret;
}


static int
umlDomainAttachDeviceFlags(virDomainPtr dom,
                           const char *xml,
                           unsigned int flags)
{
    virCheckFlags(VIR_DOMAIN_AFFECT_LIVE | VIR_DOMAIN_AFFECT_CONFIG, -1);

    if (flags & VIR_DOMAIN_AFFECT_CONFIG) {
        umlReportError(VIR_ERR_OPERATION_INVALID,
                       "%s", _("cannot modify the persistent configuration of a domain"));
        return -1;
    }

    return umlDomainAttachDevice(dom, xml);
}


static int umlDomainDetachUmlDisk(struct uml_driver *driver,
                                  virDomainObjPtr vm,
                                  virDomainDeviceDefPtr dev)
{
    int i, ret = -1;
    virDomainDiskDefPtr detach = NULL;
    char *cmd;
    char *reply;

    for (i = 0 ; i < vm->def->ndisks ; i++) {
        if (STREQ(vm->def->disks[i]->dst, dev->data.disk->dst)) {
            break;
        }
    }

    if (i == vm->def->ndisks) {
        umlReportError(VIR_ERR_OPERATION_FAILED,
                       _("disk %s not found"), dev->data.disk->dst);
        return -1;
    }

    detach = vm->def->disks[i];

    if (virAsprintf(&cmd, "remove %s", detach->dst) < 0) {
        virReportOOMError();
        return -1;
    }

    if (umlMonitorCommand(driver, vm, cmd, &reply) < 0)
        goto cleanup;

    virDomainDiskRemove(vm->def, i);

    virDomainDiskDefFree(detach);

    ret = 0;

    VIR_FREE(reply);

cleanup:
    VIR_FREE(cmd);

    return ret;
}


static int umlDomainDetachDevice(virDomainPtr dom, const char *xml) {
    struct uml_driver *driver = dom->conn->privateData;
    virDomainObjPtr vm;
    virDomainDeviceDefPtr dev = NULL;
    int ret = -1;

    umlDriverLock(driver);
    vm = virDomainFindByUUID(&driver->domains, dom->uuid);
    if (!vm) {
        char uuidstr[VIR_UUID_STRING_BUFLEN];
        virUUIDFormat(dom->uuid, uuidstr);
        umlReportError(VIR_ERR_NO_DOMAIN,
                       _("no domain with matching uuid '%s'"), uuidstr);
        goto cleanup;
    }

    if (!virDomainObjIsActive(vm)) {
        umlReportError(VIR_ERR_OPERATION_INVALID,
                       "%s", _("cannot detach device on inactive domain"));
        goto cleanup;
    }

    dev = virDomainDeviceDefParse(driver->caps, vm->def, xml,
                                  VIR_DOMAIN_XML_INACTIVE);
    if (dev == NULL)
        goto cleanup;

    if (dev->type == VIR_DOMAIN_DEVICE_DISK &&
        dev->data.disk->device == VIR_DOMAIN_DISK_DEVICE_DISK) {
        if (dev->data.disk->bus == VIR_DOMAIN_DISK_BUS_UML)
            ret = umlDomainDetachUmlDisk(driver, vm, dev);
        else {
            umlReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("This type of disk cannot be hot unplugged"));
        }
    } else {
        umlReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       "%s", _("This type of device cannot be hot unplugged"));
    }

cleanup:
    virDomainDeviceDefFree(dev);
    if (vm)
        virDomainObjUnlock(vm);
    umlDriverUnlock(driver);
    return ret;
}


static int
umlDomainDetachDeviceFlags(virDomainPtr dom,
                           const char *xml,
                           unsigned int flags)
{
    virCheckFlags(VIR_DOMAIN_AFFECT_LIVE | VIR_DOMAIN_AFFECT_CONFIG, -1);

    if (flags & VIR_DOMAIN_AFFECT_CONFIG) {
        umlReportError(VIR_ERR_OPERATION_INVALID,
                       "%s", _("cannot modify the persistent configuration of a domain"));
        return -1;
    }

    return umlDomainDetachDevice(dom, xml);
}


static int umlDomainGetAutostart(virDomainPtr dom,
                            int *autostart) {
    struct uml_driver *driver = dom->conn->privateData;
    virDomainObjPtr vm;
    int ret = -1;

    umlDriverLock(driver);
    vm = virDomainFindByUUID(&driver->domains, dom->uuid);

    if (!vm) {
        umlReportError(VIR_ERR_NO_DOMAIN, "%s",
                       _("no domain with matching uuid"));
        goto cleanup;
    }

    *autostart = vm->autostart;
    ret = 0;

cleanup:
    if (vm)
        virDomainObjUnlock(vm);
    umlDriverUnlock(driver);
    return ret;
}

static int umlDomainSetAutostart(virDomainPtr dom,
                                   int autostart) {
    struct uml_driver *driver = dom->conn->privateData;
    virDomainObjPtr vm;
    char *configFile = NULL, *autostartLink = NULL;
    int ret = -1;

    umlDriverLock(driver);
    vm = virDomainFindByUUID(&driver->domains, dom->uuid);

    if (!vm) {
        umlReportError(VIR_ERR_NO_DOMAIN, "%s",
                       _("no domain with matching uuid"));
        goto cleanup;
    }

    if (!vm->persistent) {
        umlReportError(VIR_ERR_OPERATION_INVALID, "%s",
                       _("cannot set autostart for transient domain"));
        goto cleanup;
    }

    autostart = (autostart != 0);

    if (vm->autostart != autostart) {
        if ((configFile = virDomainConfigFile(driver->configDir, vm->def->name)) == NULL)
            goto cleanup;
        if ((autostartLink = virDomainConfigFile(driver->autostartDir, vm->def->name)) == NULL)
            goto cleanup;

        if (autostart) {
            if (virFileMakePath(driver->autostartDir) < 0) {
                virReportSystemError(errno,
                                     _("cannot create autostart directory %s"),
                                     driver->autostartDir);
                goto cleanup;
            }

            if (symlink(configFile, autostartLink) < 0) {
                virReportSystemError(errno,
                                     _("Failed to create symlink '%s to '%s'"),
                                     autostartLink, configFile);
                goto cleanup;
            }
        } else {
            if (unlink(autostartLink) < 0 && errno != ENOENT && errno != ENOTDIR) {
                virReportSystemError(errno,
                                     _("Failed to delete symlink '%s'"),
                                     autostartLink);
                goto cleanup;
            }
        }

        vm->autostart = autostart;
    }
    ret = 0;

cleanup:
    VIR_FREE(configFile);
    VIR_FREE(autostartLink);
    if (vm)
        virDomainObjUnlock(vm);
    umlDriverUnlock(driver);
    return ret;
}


static int
umlDomainBlockPeek(virDomainPtr dom,
                   const char *path,
                   unsigned long long offset, size_t size,
                   void *buffer,
                   unsigned int flags)
{
    struct uml_driver *driver = dom->conn->privateData;
    virDomainObjPtr vm;
    int fd = -1, ret = -1;
    const char *actual;

    virCheckFlags(0, -1);

    umlDriverLock(driver);
    vm = virDomainFindByUUID(&driver->domains, dom->uuid);
    umlDriverUnlock(driver);

    if (!vm) {
        umlReportError(VIR_ERR_NO_DOMAIN, "%s",
                       _("no domain with matching uuid"));
        goto cleanup;
    }

    if (!path || path[0] == '\0') {
        umlReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("NULL or empty path"));
        goto cleanup;
    }

    /* Check the path belongs to this domain. */
    if (!(actual = virDomainDiskPathByName(vm->def, path))) {
        umlReportError(VIR_ERR_INVALID_ARG,
                       _("invalid path '%s'"), path);
        goto cleanup;
    }
    path = actual;

    /* The path is correct, now try to open it and get its size. */
    fd = open(path, O_RDONLY);
    if (fd == -1) {
        virReportSystemError(errno,
                             _("cannot open %s"), path);
        goto cleanup;
    }

    /* Seek and read. */
    /* NB. Because we configure with AC_SYS_LARGEFILE, off_t should
     * be 64 bits on all platforms.
     */
    if (lseek(fd, offset, SEEK_SET) == (off_t) -1 ||
        saferead(fd, buffer, size) == (ssize_t) -1) {
        virReportSystemError(errno,
                             _("cannot read %s"), path);
        goto cleanup;
    }

    ret = 0;

cleanup:
    VIR_FORCE_CLOSE(fd);
    if (vm)
        virDomainObjUnlock(vm);
    return ret;
}


static int
umlDomainOpenConsole(virDomainPtr dom,
                     const char *dev_name,
                     virStreamPtr st,
                     unsigned int flags)
{
    struct uml_driver *driver = dom->conn->privateData;
    virDomainObjPtr vm = NULL;
    char uuidstr[VIR_UUID_STRING_BUFLEN];
    int ret = -1;
    virDomainChrDefPtr chr = NULL;
    size_t i;

    virCheckFlags(0, -1);

    umlDriverLock(driver);
    virUUIDFormat(dom->uuid, uuidstr);
    vm = virDomainFindByUUID(&driver->domains, dom->uuid);
    if (!vm) {
        umlReportError(VIR_ERR_NO_DOMAIN,
                       _("no domain with matching uuid '%s'"), uuidstr);
        goto cleanup;
    }

    if (!virDomainObjIsActive(vm)) {
        umlReportError(VIR_ERR_OPERATION_INVALID,
                        "%s", _("domain is not running"));
        goto cleanup;
    }

    if (dev_name) {
        for (i = 0 ; i < vm->def->nconsoles ; i++) {
            if (vm->def->consoles[i]->info.alias &&
                STREQ(vm->def->consoles[i]->info.alias, dev_name)) {
                chr = vm->def->consoles[i];
                break;
            }
        }
    } else {
        if (vm->def->nconsoles)
            chr = vm->def->consoles[0];
        else if (vm->def->nserials)
            chr = vm->def->serials[0];
    }

    if (!chr) {
        umlReportError(VIR_ERR_INTERNAL_ERROR,
                       _("cannot find console device '%s'"),
                       dev_name ? dev_name : _("default"));
        goto cleanup;
    }

    if (chr->source.type != VIR_DOMAIN_CHR_TYPE_PTY) {
        umlReportError(VIR_ERR_INTERNAL_ERROR,
                        _("character device %s is not using a PTY"), dev_name);
        goto cleanup;
    }

    if (virFDStreamOpenFile(st, chr->source.data.file.path,
                            0, 0, O_RDWR) < 0)
        goto cleanup;

    ret = 0;
cleanup:
    if (vm)
        virDomainObjUnlock(vm);
    umlDriverUnlock(driver);
    return ret;
}


static int
umlDomainEventRegister(virConnectPtr conn,
                       virConnectDomainEventCallback callback,
                       void *opaque,
                       virFreeCallback freecb)
{
    struct uml_driver *driver = conn->privateData;
    int ret;

    umlDriverLock(driver);
    ret = virDomainEventStateRegister(conn,
                                      driver->domainEventState,
                                      callback, opaque, freecb);
    umlDriverUnlock(driver);

    return ret;
}

static int
umlDomainEventDeregister(virConnectPtr conn,
                         virConnectDomainEventCallback callback)
{
    struct uml_driver *driver = conn->privateData;
    int ret;

    umlDriverLock(driver);
    ret = virDomainEventStateDeregister(conn,
                                        driver->domainEventState,
                                        callback);
    umlDriverUnlock(driver);

    return ret;
}

static int
umlDomainEventRegisterAny(virConnectPtr conn,
                          virDomainPtr dom,
                          int eventID,
                          virConnectDomainEventGenericCallback callback,
                          void *opaque,
                          virFreeCallback freecb)
{
    struct uml_driver *driver = conn->privateData;
    int ret;

    umlDriverLock(driver);
    if (virDomainEventStateRegisterID(conn,
                                      driver->domainEventState,
                                      dom, eventID,
                                      callback, opaque, freecb, &ret) < 0)
        ret = -1;
    umlDriverUnlock(driver);

    return ret;
}


static int
umlDomainEventDeregisterAny(virConnectPtr conn,
                            int callbackID)
{
    struct uml_driver *driver = conn->privateData;
    int ret;

    umlDriverLock(driver);
    ret = virDomainEventStateDeregisterID(conn,
                                          driver->domainEventState,
                                          callbackID);
    umlDriverUnlock(driver);

    return ret;
}


/* driver must be locked before calling */
static void umlDomainEventQueue(struct uml_driver *driver,
                                virDomainEventPtr event)
{
    virDomainEventStateQueue(driver->domainEventState, event);
}



static virDriver umlDriver = {
    .no = VIR_DRV_UML,
    .name = "UML",
    .open = umlOpen, /* 0.5.0 */
    .close = umlClose, /* 0.5.0 */
    .type = umlGetType, /* 0.5.0 */
    .version = umlGetVersion, /* 0.5.0 */
    .getHostname = virGetHostname, /* 0.5.0 */
    .nodeGetInfo = nodeGetInfo, /* 0.5.0 */
    .getCapabilities = umlGetCapabilities, /* 0.5.0 */
    .listDomains = umlListDomains, /* 0.5.0 */
    .numOfDomains = umlNumDomains, /* 0.5.0 */
    .domainCreateXML = umlDomainCreate, /* 0.5.0 */
    .domainLookupByID = umlDomainLookupByID, /* 0.5.0 */
    .domainLookupByUUID = umlDomainLookupByUUID, /* 0.5.0 */
    .domainLookupByName = umlDomainLookupByName, /* 0.5.0 */
    .domainShutdown = umlDomainShutdown, /* 0.5.0 */
    .domainShutdownFlags = umlDomainShutdownFlags, /* 0.9.10 */
    .domainDestroy = umlDomainDestroy, /* 0.5.0 */
    .domainDestroyFlags = umlDomainDestroyFlags, /* 0.9.4 */
    .domainGetOSType = umlDomainGetOSType, /* 0.5.0 */
    .domainGetMaxMemory = umlDomainGetMaxMemory, /* 0.5.0 */
    .domainSetMaxMemory = umlDomainSetMaxMemory, /* 0.5.0 */
    .domainSetMemory = umlDomainSetMemory, /* 0.5.0 */
    .domainGetInfo = umlDomainGetInfo, /* 0.5.0 */
    .domainGetState = umlDomainGetState, /* 0.9.2 */
    .domainGetXMLDesc = umlDomainGetXMLDesc, /* 0.5.0 */
    .listDefinedDomains = umlListDefinedDomains, /* 0.5.0 */
    .numOfDefinedDomains = umlNumDefinedDomains, /* 0.5.0 */
    .domainCreate = umlDomainStart, /* 0.5.0 */
    .domainCreateWithFlags = umlDomainStartWithFlags, /* 0.8.2 */
    .domainDefineXML = umlDomainDefine, /* 0.5.0 */
    .domainUndefine = umlDomainUndefine, /* 0.5.0 */
    .domainUndefineFlags = umlDomainUndefineFlags, /* 0.9.4 */
    .domainAttachDevice = umlDomainAttachDevice, /* 0.8.4 */
    .domainAttachDeviceFlags = umlDomainAttachDeviceFlags, /* 0.8.4 */
    .domainDetachDevice = umlDomainDetachDevice, /* 0.8.4 */
    .domainDetachDeviceFlags = umlDomainDetachDeviceFlags, /* 0.8.4 */
    .domainGetAutostart = umlDomainGetAutostart, /* 0.5.0 */
    .domainSetAutostart = umlDomainSetAutostart, /* 0.5.0 */
    .domainBlockPeek = umlDomainBlockPeek, /* 0.5.0 */
    .nodeGetCPUStats = nodeGetCPUStats, /* 0.9.3 */
    .nodeGetMemoryStats = nodeGetMemoryStats, /* 0.9.3 */
    .nodeGetCellsFreeMemory = nodeGetCellsFreeMemory, /* 0.5.0 */
    .nodeGetFreeMemory = nodeGetFreeMemory, /* 0.5.0 */
    .domainEventRegister = umlDomainEventRegister, /* 0.9.4 */
    .domainEventDeregister = umlDomainEventDeregister, /* 0.9.4 */
    .isEncrypted = umlIsEncrypted, /* 0.7.3 */
    .isSecure = umlIsSecure, /* 0.7.3 */
    .domainIsActive = umlDomainIsActive, /* 0.7.3 */
    .domainIsPersistent = umlDomainIsPersistent, /* 0.7.3 */
    .domainIsUpdated = umlDomainIsUpdated, /* 0.8.6 */
    .domainEventRegisterAny = umlDomainEventRegisterAny, /* 0.9.4 */
    .domainEventDeregisterAny = umlDomainEventDeregisterAny, /* 0.9.4 */
    .domainOpenConsole = umlDomainOpenConsole, /* 0.8.6 */
    .isAlive = umlIsAlive, /* 0.9.8 */
    .nodeSuspendForDuration = nodeSuspendForDuration, /* 0.9.8 */
};

static int
umlVMFilterRebuild(virConnectPtr conn ATTRIBUTE_UNUSED,
                   virHashIterator iter, void *data)
{
    virHashForEach(uml_driver->domains.objs, iter, data);

    return 0;
}

static virStateDriver umlStateDriver = {
    .name = "UML",
    .initialize = umlStartup,
    .cleanup = umlShutdown,
    .reload = umlReload,
    .active = umlActive,
};

static void
umlVMDriverLock(void)
{
    umlDriverLock(uml_driver);
}

static void
umlVMDriverUnlock(void)
{
    umlDriverUnlock(uml_driver);
}

static virNWFilterCallbackDriver umlCallbackDriver = {
    .name = "UML",
    .vmFilterRebuild = umlVMFilterRebuild,
    .vmDriverLock = umlVMDriverLock,
    .vmDriverUnlock = umlVMDriverUnlock,
};

int umlRegister(void) {
    virRegisterDriver(&umlDriver);
    virRegisterStateDriver(&umlStateDriver);
    virNWFilterRegisterCallbackDriver(&umlCallbackDriver);
    return 0;
}
