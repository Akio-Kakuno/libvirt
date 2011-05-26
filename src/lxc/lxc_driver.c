/*
 * Copyright (C) 2010-2011 Red Hat, Inc.
 * Copyright IBM Corp. 2008
 *
 * lxc_driver.c: linux container driver functions
 *
 * Authors:
 *  David L. Leskovec <dlesko at linux.vnet.ibm.com>
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
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

#include <config.h>

#include <fcntl.h>
#include <sched.h>
#include <sys/utsname.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/poll.h>
#include <unistd.h>
#include <wait.h>

#include "virterror_internal.h"
#include "logging.h"
#include "datatypes.h"
#include "lxc_conf.h"
#include "lxc_container.h"
#include "lxc_driver.h"
#include "memory.h"
#include "util.h"
#include "bridge.h"
#include "veth.h"
#include "event.h"
#include "nodeinfo.h"
#include "uuid.h"
#include "stats_linux.h"
#include "hooks.h"
#include "files.h"
#include "fdstream.h"


#define VIR_FROM_THIS VIR_FROM_LXC

#define START_POSTFIX ": starting up\n"

#define LXC_NB_MEM_PARAM  3

typedef struct _lxcDomainObjPrivate lxcDomainObjPrivate;
typedef lxcDomainObjPrivate *lxcDomainObjPrivatePtr;
struct _lxcDomainObjPrivate {
    int monitor;
    int monitorWatch;
};


static int lxcStartup(int privileged);
static int lxcShutdown(void);
static lxc_driver_t *lxc_driver = NULL;

/* Functions */

static void lxcDriverLock(lxc_driver_t *driver)
{
    virMutexLock(&driver->lock);
}
static void lxcDriverUnlock(lxc_driver_t *driver)
{
    virMutexUnlock(&driver->lock);
}

static void *lxcDomainObjPrivateAlloc(void)
{
    lxcDomainObjPrivatePtr priv;

    if (VIR_ALLOC(priv) < 0)
        return NULL;

    priv->monitor = -1;
    priv->monitorWatch = -1;

    return priv;
}

static void lxcDomainObjPrivateFree(void *data)
{
    lxcDomainObjPrivatePtr priv = data;

    VIR_FREE(priv);
}


static void lxcDomainEventFlush(int timer, void *opaque);
static void lxcDomainEventQueue(lxc_driver_t *driver,
                                virDomainEventPtr event);


static virDrvOpenStatus lxcOpen(virConnectPtr conn,
                                virConnectAuthPtr auth ATTRIBUTE_UNUSED,
                                int flags ATTRIBUTE_UNUSED)
{
    /* Verify uri was specified */
    if (conn->uri == NULL) {
        if (lxc_driver == NULL)
            return VIR_DRV_OPEN_DECLINED;

        conn->uri = xmlParseURI("lxc:///");
        if (!conn->uri) {
            virReportOOMError();
            return VIR_DRV_OPEN_ERROR;
        }
    } else {
        if (conn->uri->scheme == NULL ||
            STRNEQ(conn->uri->scheme, "lxc"))
            return VIR_DRV_OPEN_DECLINED;

        /* Leave for remote driver */
        if (conn->uri->server != NULL)
            return VIR_DRV_OPEN_DECLINED;

        /* If path isn't '/' then they typoed, tell them correct path */
        if (conn->uri->path != NULL &&
            STRNEQ(conn->uri->path, "/")) {
            lxcError(VIR_ERR_INTERNAL_ERROR,
                     _("Unexpected LXC URI path '%s', try lxc:///"),
                     conn->uri->path);
            return VIR_DRV_OPEN_ERROR;
        }

        /* URI was good, but driver isn't active */
        if (lxc_driver == NULL) {
            lxcError(VIR_ERR_INTERNAL_ERROR,
                     "%s", _("lxc state driver is not active"));
            return VIR_DRV_OPEN_ERROR;
        }
    }

    conn->privateData = lxc_driver;

    return VIR_DRV_OPEN_SUCCESS;
}

static int lxcClose(virConnectPtr conn)
{
    lxc_driver_t *driver = conn->privateData;

    lxcDriverLock(driver);
    virDomainEventCallbackListRemoveConn(conn,
                                         driver->domainEventState->callbacks);
    lxcDriverUnlock(driver);

    conn->privateData = NULL;
    return 0;
}


static int lxcIsSecure(virConnectPtr conn ATTRIBUTE_UNUSED)
{
    /* Trivially secure, since always inside the daemon */
    return 1;
}


static int lxcIsEncrypted(virConnectPtr conn ATTRIBUTE_UNUSED)
{
    /* Not encrypted, but remote driver takes care of that */
    return 0;
}


static char *lxcGetCapabilities(virConnectPtr conn) {
    lxc_driver_t *driver = conn->privateData;
    char *xml;

    lxcDriverLock(driver);
    if ((xml = virCapabilitiesFormatXML(driver->caps)) == NULL)
        virReportOOMError();
    lxcDriverUnlock(driver);

    return xml;
}


static virDomainPtr lxcDomainLookupByID(virConnectPtr conn,
                                        int id)
{
    lxc_driver_t *driver = conn->privateData;
    virDomainObjPtr vm;
    virDomainPtr dom = NULL;

    lxcDriverLock(driver);
    vm = virDomainFindByID(&driver->domains, id);
    lxcDriverUnlock(driver);

    if (!vm) {
        lxcError(VIR_ERR_NO_DOMAIN,
                 _("No domain with matching id %d"), id);
        goto cleanup;
    }

    dom = virGetDomain(conn, vm->def->name, vm->def->uuid);
    if (dom)
        dom->id = vm->def->id;

cleanup:
    if (vm)
        virDomainObjUnlock(vm);
    return dom;
}

static virDomainPtr lxcDomainLookupByUUID(virConnectPtr conn,
                                          const unsigned char *uuid)
{
    lxc_driver_t *driver = conn->privateData;
    virDomainObjPtr vm;
    virDomainPtr dom = NULL;

    lxcDriverLock(driver);
    vm = virDomainFindByUUID(&driver->domains, uuid);
    lxcDriverUnlock(driver);

    if (!vm) {
        char uuidstr[VIR_UUID_STRING_BUFLEN];
        virUUIDFormat(uuid, uuidstr);
        lxcError(VIR_ERR_NO_DOMAIN,
                 _("No domain with matching uuid '%s'"), uuidstr);
        goto cleanup;
    }

    dom = virGetDomain(conn, vm->def->name, vm->def->uuid);
    if (dom)
        dom->id = vm->def->id;

cleanup:
    if (vm)
        virDomainObjUnlock(vm);
    return dom;
}

static virDomainPtr lxcDomainLookupByName(virConnectPtr conn,
                                          const char *name)
{
    lxc_driver_t *driver = conn->privateData;
    virDomainObjPtr vm;
    virDomainPtr dom = NULL;

    lxcDriverLock(driver);
    vm = virDomainFindByName(&driver->domains, name);
    lxcDriverUnlock(driver);
    if (!vm) {
        lxcError(VIR_ERR_NO_DOMAIN,
                 _("No domain with matching name '%s'"), name);
        goto cleanup;
    }

    dom = virGetDomain(conn, vm->def->name, vm->def->uuid);
    if (dom)
        dom->id = vm->def->id;

cleanup:
    if (vm)
        virDomainObjUnlock(vm);
    return dom;
}


static int lxcDomainIsActive(virDomainPtr dom)
{
    lxc_driver_t *driver = dom->conn->privateData;
    virDomainObjPtr obj;
    int ret = -1;

    lxcDriverLock(driver);
    obj = virDomainFindByUUID(&driver->domains, dom->uuid);
    lxcDriverUnlock(driver);
    if (!obj) {
        char uuidstr[VIR_UUID_STRING_BUFLEN];
        virUUIDFormat(dom->uuid, uuidstr);
        lxcError(VIR_ERR_NO_DOMAIN,
                 _("No domain with matching uuid '%s'"), uuidstr);
        goto cleanup;
    }
    ret = virDomainObjIsActive(obj);

cleanup:
    if (obj)
        virDomainObjUnlock(obj);
    return ret;
}


static int lxcDomainIsPersistent(virDomainPtr dom)
{
    lxc_driver_t *driver = dom->conn->privateData;
    virDomainObjPtr obj;
    int ret = -1;

    lxcDriverLock(driver);
    obj = virDomainFindByUUID(&driver->domains, dom->uuid);
    lxcDriverUnlock(driver);
    if (!obj) {
        char uuidstr[VIR_UUID_STRING_BUFLEN];
        virUUIDFormat(dom->uuid, uuidstr);
        lxcError(VIR_ERR_NO_DOMAIN,
                 _("No domain with matching uuid '%s'"), uuidstr);
        goto cleanup;
    }
    ret = obj->persistent;

cleanup:
    if (obj)
        virDomainObjUnlock(obj);
    return ret;
}

static int lxcDomainIsUpdated(virDomainPtr dom)
{
    lxc_driver_t *driver = dom->conn->privateData;
    virDomainObjPtr obj;
    int ret = -1;

    lxcDriverLock(driver);
    obj = virDomainFindByUUID(&driver->domains, dom->uuid);
    lxcDriverUnlock(driver);
    if (!obj) {
        char uuidstr[VIR_UUID_STRING_BUFLEN];
        virUUIDFormat(dom->uuid, uuidstr);
        lxcError(VIR_ERR_NO_DOMAIN,
                 _("No domain with matching uuid '%s'"), uuidstr);
        goto cleanup;
    }
    ret = obj->updated;

cleanup:
    if (obj)
        virDomainObjUnlock(obj);
    return ret;
}

static int lxcListDomains(virConnectPtr conn, int *ids, int nids) {
    lxc_driver_t *driver = conn->privateData;
    int n;

    lxcDriverLock(driver);
    n = virDomainObjListGetActiveIDs(&driver->domains, ids, nids);
    lxcDriverUnlock(driver);

    return n;
}

static int lxcNumDomains(virConnectPtr conn) {
    lxc_driver_t *driver = conn->privateData;
    int n;

    lxcDriverLock(driver);
    n = virDomainObjListNumOfDomains(&driver->domains, 1);
    lxcDriverUnlock(driver);

    return n;
}

static int lxcListDefinedDomains(virConnectPtr conn,
                                 char **const names, int nnames) {
    lxc_driver_t *driver = conn->privateData;
    int n;

    lxcDriverLock(driver);
    n = virDomainObjListGetInactiveNames(&driver->domains, names, nnames);
    lxcDriverUnlock(driver);

    return n;
}


static int lxcNumDefinedDomains(virConnectPtr conn) {
    lxc_driver_t *driver = conn->privateData;
    int n;

    lxcDriverLock(driver);
    n = virDomainObjListNumOfDomains(&driver->domains, 0);
    lxcDriverUnlock(driver);

    return n;
}



static virDomainPtr lxcDomainDefine(virConnectPtr conn, const char *xml)
{
    lxc_driver_t *driver = conn->privateData;
    virDomainDefPtr def = NULL;
    virDomainObjPtr vm = NULL;
    virDomainPtr dom = NULL;
    virDomainEventPtr event = NULL;
    int dupVM;

    lxcDriverLock(driver);
    if (!(def = virDomainDefParseString(driver->caps, xml,
                                        VIR_DOMAIN_XML_INACTIVE)))
        goto cleanup;

   if ((dupVM = virDomainObjIsDuplicate(&driver->domains, def, 0)) < 0)
        goto cleanup;

    if ((def->nets != NULL) && !(driver->have_netns)) {
        lxcError(VIR_ERR_NO_SUPPORT,
                 "%s", _("System lacks NETNS support"));
        goto cleanup;
    }

    if (!(vm = virDomainAssignDef(driver->caps,
                                  &driver->domains, def, false)))
        goto cleanup;
    def = NULL;
    vm->persistent = 1;

    if (virDomainSaveConfig(driver->configDir,
                            vm->newDef ? vm->newDef : vm->def) < 0) {
        virDomainRemoveInactive(&driver->domains, vm);
        vm = NULL;
        goto cleanup;
    }

    event = virDomainEventNewFromObj(vm,
                                     VIR_DOMAIN_EVENT_DEFINED,
                                     !dupVM ?
                                     VIR_DOMAIN_EVENT_DEFINED_ADDED :
                                     VIR_DOMAIN_EVENT_DEFINED_UPDATED);

    dom = virGetDomain(conn, vm->def->name, vm->def->uuid);
    if (dom)
        dom->id = vm->def->id;

cleanup:
    virDomainDefFree(def);
    if (vm)
        virDomainObjUnlock(vm);
    if (event)
        lxcDomainEventQueue(driver, event);
    lxcDriverUnlock(driver);
    return dom;
}

static int lxcDomainUndefine(virDomainPtr dom)
{
    lxc_driver_t *driver = dom->conn->privateData;
    virDomainObjPtr vm;
    virDomainEventPtr event = NULL;
    int ret = -1;

    lxcDriverLock(driver);
    vm = virDomainFindByUUID(&driver->domains, dom->uuid);
    if (!vm) {
        char uuidstr[VIR_UUID_STRING_BUFLEN];
        virUUIDFormat(dom->uuid, uuidstr);
        lxcError(VIR_ERR_NO_DOMAIN,
                 _("No domain with matching uuid '%s'"), uuidstr);
        goto cleanup;
    }

    if (virDomainObjIsActive(vm)) {
        lxcError(VIR_ERR_OPERATION_INVALID,
                 "%s", _("Cannot delete active domain"));
        goto cleanup;
    }

    if (!vm->persistent) {
        lxcError(VIR_ERR_OPERATION_INVALID,
                 "%s", _("Cannot undefine transient domain"));
        goto cleanup;
    }

    if (virDomainDeleteConfig(driver->configDir,
                              driver->autostartDir,
                              vm) < 0)
        goto cleanup;

    event = virDomainEventNewFromObj(vm,
                                     VIR_DOMAIN_EVENT_UNDEFINED,
                                     VIR_DOMAIN_EVENT_UNDEFINED_REMOVED);

    virDomainRemoveInactive(&driver->domains, vm);
    vm = NULL;
    ret = 0;

cleanup:
    if (vm)
        virDomainObjUnlock(vm);
    if (event)
        lxcDomainEventQueue(driver, event);
    lxcDriverUnlock(driver);
    return ret;
}

static int lxcDomainGetInfo(virDomainPtr dom,
                            virDomainInfoPtr info)
{
    lxc_driver_t *driver = dom->conn->privateData;
    virDomainObjPtr vm;
    virCgroupPtr cgroup = NULL;
    int ret = -1, rc;

    lxcDriverLock(driver);
    vm = virDomainFindByUUID(&driver->domains, dom->uuid);

    if (!vm) {
        char uuidstr[VIR_UUID_STRING_BUFLEN];
        virUUIDFormat(dom->uuid, uuidstr);
        lxcError(VIR_ERR_NO_DOMAIN,
                 _("No domain with matching uuid '%s'"), uuidstr);
        goto cleanup;
    }

    info->state = virDomainObjGetState(vm, NULL);

    if (!virDomainObjIsActive(vm) || driver->cgroup == NULL) {
        info->cpuTime = 0;
        info->memory = vm->def->mem.cur_balloon;
    } else {
        if (virCgroupForDomain(driver->cgroup, vm->def->name, &cgroup, 0) != 0) {
            lxcError(VIR_ERR_INTERNAL_ERROR,
                     _("Unable to get cgroup for %s"), vm->def->name);
            goto cleanup;
        }

        if (virCgroupGetCpuacctUsage(cgroup, &(info->cpuTime)) < 0) {
            lxcError(VIR_ERR_OPERATION_FAILED,
                     "%s", _("Cannot read cputime for domain"));
            goto cleanup;
        }
        if ((rc = virCgroupGetMemoryUsage(cgroup, &(info->memory))) < 0) {
            lxcError(VIR_ERR_OPERATION_FAILED,
                     "%s", _("Cannot read memory usage for domain"));
            if (rc == -ENOENT) {
                /* Don't fail if we can't read memory usage due to a lack of
                 * kernel support */
                info->memory = 0;
            } else
                goto cleanup;
        }
    }

    info->maxMem = vm->def->mem.max_balloon;
    info->nrVirtCpu = 1;
    ret = 0;

cleanup:
    lxcDriverUnlock(driver);
    if (cgroup)
        virCgroupFree(&cgroup);
    if (vm)
        virDomainObjUnlock(vm);
    return ret;
}

static int
lxcDomainGetState(virDomainPtr dom,
                  int *state,
                  int *reason,
                  unsigned int flags)
{
    lxc_driver_t *driver = dom->conn->privateData;
    virDomainObjPtr vm;
    int ret = -1;

    virCheckFlags(0, -1);

    lxcDriverLock(driver);
    vm = virDomainFindByUUID(&driver->domains, dom->uuid);
    lxcDriverUnlock(driver);

    if (!vm) {
        char uuidstr[VIR_UUID_STRING_BUFLEN];
        virUUIDFormat(dom->uuid, uuidstr);
        lxcError(VIR_ERR_NO_DOMAIN,
                 _("No domain with matching uuid '%s'"), uuidstr);
        goto cleanup;
    }

    *state = virDomainObjGetState(vm, reason);
    ret = 0;

cleanup:
    if (vm)
        virDomainObjUnlock(vm);
    return ret;
}

static char *lxcGetOSType(virDomainPtr dom)
{
    lxc_driver_t *driver = dom->conn->privateData;
    virDomainObjPtr vm;
    char *ret = NULL;

    lxcDriverLock(driver);
    vm = virDomainFindByUUID(&driver->domains, dom->uuid);
    lxcDriverUnlock(driver);

    if (!vm) {
        char uuidstr[VIR_UUID_STRING_BUFLEN];
        virUUIDFormat(dom->uuid, uuidstr);
        lxcError(VIR_ERR_NO_DOMAIN,
                 _("No domain with matching uuid '%s'"), uuidstr);
        goto cleanup;
    }

    ret = strdup(vm->def->os.type);

    if (ret == NULL)
        virReportOOMError();

cleanup:
    if (vm)
        virDomainObjUnlock(vm);
    return ret;
}

/* Returns max memory in kb, 0 if error */
static unsigned long lxcDomainGetMaxMemory(virDomainPtr dom) {
    lxc_driver_t *driver = dom->conn->privateData;
    virDomainObjPtr vm;
    unsigned long ret = 0;

    lxcDriverLock(driver);
    vm = virDomainFindByUUID(&driver->domains, dom->uuid);
    lxcDriverUnlock(driver);

    if (!vm) {
        char uuidstr[VIR_UUID_STRING_BUFLEN];
        virUUIDFormat(dom->uuid, uuidstr);
        lxcError(VIR_ERR_NO_DOMAIN,
                         _("No domain with matching uuid '%s'"), uuidstr);
        goto cleanup;
    }

    ret = vm->def->mem.max_balloon;

cleanup:
    if (vm)
        virDomainObjUnlock(vm);
    return ret;
}

static int lxcDomainSetMaxMemory(virDomainPtr dom, unsigned long newmax) {
    lxc_driver_t *driver = dom->conn->privateData;
    virDomainObjPtr vm;
    int ret = -1;

    lxcDriverLock(driver);
    vm = virDomainFindByUUID(&driver->domains, dom->uuid);
    lxcDriverUnlock(driver);

    if (!vm) {
        char uuidstr[VIR_UUID_STRING_BUFLEN];
        virUUIDFormat(dom->uuid, uuidstr);
        lxcError(VIR_ERR_NO_DOMAIN,
                         _("No domain with matching uuid '%s'"), uuidstr);
        goto cleanup;
    }

    if (newmax < vm->def->mem.cur_balloon) {
        lxcError(VIR_ERR_INVALID_ARG,
                         "%s", _("Cannot set max memory lower than current memory"));
        goto cleanup;
    }

    vm->def->mem.max_balloon = newmax;
    ret = 0;

cleanup:
    if (vm)
        virDomainObjUnlock(vm);
    return ret;
}

static int lxcDomainSetMemory(virDomainPtr dom, unsigned long newmem) {
    lxc_driver_t *driver = dom->conn->privateData;
    virDomainObjPtr vm;
    virCgroupPtr cgroup = NULL;
    int ret = -1;

    lxcDriverLock(driver);
    vm = virDomainFindByUUID(&driver->domains, dom->uuid);
    lxcDriverUnlock(driver);
    if (!vm) {
        char uuidstr[VIR_UUID_STRING_BUFLEN];
        virUUIDFormat(dom->uuid, uuidstr);
        lxcError(VIR_ERR_NO_DOMAIN,
                 _("No domain with matching uuid '%s'"), uuidstr);
        goto cleanup;
    }

    if (newmem > vm->def->mem.max_balloon) {
        lxcError(VIR_ERR_INVALID_ARG,
                 "%s", _("Cannot set memory higher than max memory"));
        goto cleanup;
    }

    if (!virDomainObjIsActive(vm)) {
        lxcError(VIR_ERR_OPERATION_INVALID,
                 "%s", _("Domain is not running"));
        goto cleanup;
    }

    if (driver->cgroup == NULL) {
        lxcError(VIR_ERR_NO_SUPPORT,
                 "%s", _("cgroups must be configured on the host"));
        goto cleanup;
    }

    if (virCgroupForDomain(driver->cgroup, vm->def->name, &cgroup, 0) != 0) {
        lxcError(VIR_ERR_INTERNAL_ERROR,
                 _("Unable to get cgroup for %s"), vm->def->name);
        goto cleanup;
    }

    if (virCgroupSetMemory(cgroup, newmem) < 0) {
        lxcError(VIR_ERR_OPERATION_FAILED,
                 "%s", _("Failed to set memory for domain"));
        goto cleanup;
    }

    ret = 0;

cleanup:
    if (vm)
        virDomainObjUnlock(vm);
    if (cgroup)
        virCgroupFree(&cgroup);
    return ret;
}

static int lxcDomainSetMemoryParameters(virDomainPtr dom,
                                        virTypedParameterPtr params,
                                        int nparams,
                                        unsigned int flags ATTRIBUTE_UNUSED)
{
    lxc_driver_t *driver = dom->conn->privateData;
    int i;
    virCgroupPtr cgroup = NULL;
    virDomainObjPtr vm = NULL;
    int ret = -1;

    lxcDriverLock(driver);
    vm = virDomainFindByUUID(&driver->domains, dom->uuid);

    if (vm == NULL) {
        char uuidstr[VIR_UUID_STRING_BUFLEN];
        virUUIDFormat(dom->uuid, uuidstr);
        lxcError(VIR_ERR_NO_DOMAIN,
                 _("No domain with matching uuid '%s'"), uuidstr);
        goto cleanup;
    }

    if (virCgroupForDomain(driver->cgroup, vm->def->name, &cgroup, 0) != 0) {
        lxcError(VIR_ERR_INTERNAL_ERROR,
                 _("cannot find cgroup for domain %s"), vm->def->name);
        goto cleanup;
    }

    ret = 0;
    for (i = 0; i < nparams; i++) {
        virTypedParameterPtr param = &params[i];

        if (STREQ(param->field, VIR_DOMAIN_MEMORY_HARD_LIMIT)) {
            int rc;
            if (param->type != VIR_TYPED_PARAM_ULLONG) {
                lxcError(VIR_ERR_INVALID_ARG, "%s",
                         _("invalid type for memory hard_limit tunable, expected a 'ullong'"));
                ret = -1;
                continue;
            }

            rc = virCgroupSetMemoryHardLimit(cgroup, params[i].value.ul);
            if (rc != 0) {
                virReportSystemError(-rc, "%s",
                                     _("unable to set memory hard_limit tunable"));
                ret = -1;
            }
        } else if (STREQ(param->field, VIR_DOMAIN_MEMORY_SOFT_LIMIT)) {
            int rc;
            if (param->type != VIR_TYPED_PARAM_ULLONG) {
                lxcError(VIR_ERR_INVALID_ARG, "%s",
                         _("invalid type for memory soft_limit tunable, expected a 'ullong'"));
                ret = -1;
                continue;
            }

            rc = virCgroupSetMemorySoftLimit(cgroup, params[i].value.ul);
            if (rc != 0) {
                virReportSystemError(-rc, "%s",
                                     _("unable to set memory soft_limit tunable"));
                ret = -1;
            }
        } else if (STREQ(param->field, VIR_DOMAIN_MEMORY_SWAP_HARD_LIMIT)) {
            int rc;
            if (param->type != VIR_TYPED_PARAM_ULLONG) {
                lxcError(VIR_ERR_INVALID_ARG, "%s",
                         _("invalid type for swap_hard_limit tunable, expected a 'ullong'"));
                ret = -1;
                continue;
            }

            rc = virCgroupSetMemSwapHardLimit(cgroup, params[i].value.ul);
            if (rc != 0) {
                virReportSystemError(-rc, "%s",
                                     _("unable to set swap_hard_limit tunable"));
                ret = -1;
            }
        } else if (STREQ(param->field, VIR_DOMAIN_MEMORY_MIN_GUARANTEE)) {
            lxcError(VIR_ERR_INVALID_ARG,
                     _("Memory tunable `%s' not implemented"), param->field);
            ret = -1;
        } else {
            lxcError(VIR_ERR_INVALID_ARG,
                     _("Parameter `%s' not supported"), param->field);
            ret = -1;
        }
    }

cleanup:
    if (cgroup)
        virCgroupFree(&cgroup);
    if (vm)
        virDomainObjUnlock(vm);
    lxcDriverUnlock(driver);
    return ret;
}

static int lxcDomainGetMemoryParameters(virDomainPtr dom,
                                        virTypedParameterPtr params,
                                        int *nparams,
                                        unsigned int flags ATTRIBUTE_UNUSED)
{
    lxc_driver_t *driver = dom->conn->privateData;
    int i;
    virCgroupPtr cgroup = NULL;
    virDomainObjPtr vm = NULL;
    unsigned long long val;
    int ret = -1;
    int rc;

    lxcDriverLock(driver);
    vm = virDomainFindByUUID(&driver->domains, dom->uuid);

    if (vm == NULL) {
        char uuidstr[VIR_UUID_STRING_BUFLEN];
        virUUIDFormat(dom->uuid, uuidstr);
        lxcError(VIR_ERR_NO_DOMAIN,
                 _("No domain with matching uuid '%s'"), uuidstr);
        goto cleanup;
    }

    if ((*nparams) == 0) {
        /* Current number of memory parameters supported by cgroups */
        *nparams = LXC_NB_MEM_PARAM;
        ret = 0;
        goto cleanup;
    }
    if ((*nparams) < LXC_NB_MEM_PARAM) {
        lxcError(VIR_ERR_INVALID_ARG,
                 "%s", _("Invalid parameter count"));
        goto cleanup;
    }

    if (virCgroupForDomain(driver->cgroup, vm->def->name, &cgroup, 0) != 0) {
        lxcError(VIR_ERR_INTERNAL_ERROR,
                 _("Unable to get cgroup for %s"), vm->def->name);
        goto cleanup;
    }

    for (i = 0; i < LXC_NB_MEM_PARAM; i++) {
        virTypedParameterPtr param = &params[i];
        val = 0;
        param->value.ul = 0;
        param->type = VIR_TYPED_PARAM_ULLONG;

        switch(i) {
        case 0: /* fill memory hard limit here */
            rc = virCgroupGetMemoryHardLimit(cgroup, &val);
            if (rc != 0) {
                virReportSystemError(-rc, "%s",
                                     _("unable to get memory hard limit"));
                goto cleanup;
            }
            if (virStrcpyStatic(param->field, VIR_DOMAIN_MEMORY_HARD_LIMIT) == NULL) {
                lxcError(VIR_ERR_INTERNAL_ERROR,
                         "%s", _("Field memory hard limit too long for destination"));
                goto cleanup;
            }
            param->value.ul = val;
            break;

        case 1: /* fill memory soft limit here */
            rc = virCgroupGetMemorySoftLimit(cgroup, &val);
            if (rc != 0) {
                virReportSystemError(-rc, "%s",
                                     _("unable to get memory soft limit"));
                goto cleanup;
            }
            if (virStrcpyStatic(param->field, VIR_DOMAIN_MEMORY_SOFT_LIMIT) == NULL) {
                lxcError(VIR_ERR_INTERNAL_ERROR,
                         "%s", _("Field memory soft limit too long for destination"));
                goto cleanup;
            }
            param->value.ul = val;
            break;

        case 2: /* fill swap hard limit here */
            rc = virCgroupGetMemSwapHardLimit(cgroup, &val);
            if (rc != 0) {
                virReportSystemError(-rc, "%s",
                                     _("unable to get swap hard limit"));
                goto cleanup;
            }
            if (virStrcpyStatic(param->field, VIR_DOMAIN_MEMORY_SWAP_HARD_LIMIT) == NULL) {
                lxcError(VIR_ERR_INTERNAL_ERROR,
                         "%s", _("Field swap hard limit too long for destination"));
                goto cleanup;
            }
            param->value.ul = val;
            break;

        default:
            break;
            /* should not hit here */
        }
    }

    *nparams = LXC_NB_MEM_PARAM;
    ret = 0;

cleanup:
    if (cgroup)
        virCgroupFree(&cgroup);
    if (vm)
        virDomainObjUnlock(vm);
    lxcDriverUnlock(driver);
    return ret;
}

static char *lxcDomainGetXMLDesc(virDomainPtr dom,
                                 int flags)
{
    lxc_driver_t *driver = dom->conn->privateData;
    virDomainObjPtr vm;
    char *ret = NULL;

    lxcDriverLock(driver);
    vm = virDomainFindByUUID(&driver->domains, dom->uuid);
    lxcDriverUnlock(driver);

    if (!vm) {
        char uuidstr[VIR_UUID_STRING_BUFLEN];
        virUUIDFormat(dom->uuid, uuidstr);
        lxcError(VIR_ERR_NO_DOMAIN,
                 _("No domain with matching uuid '%s'"), uuidstr);
        goto cleanup;
    }

    ret = virDomainDefFormat((flags & VIR_DOMAIN_XML_INACTIVE) &&
                             vm->newDef ? vm->newDef : vm->def,
                             flags);

cleanup:
    if (vm)
        virDomainObjUnlock(vm);
    return ret;
}


/**
 * lxcVmCleanup:
 * @driver: pointer to driver structure
 * @vm: pointer to VM to clean up
 * @reason: reason for switching the VM to shutoff state
 *
 * Cleanout resources associated with the now dead VM
 *
 */
static void lxcVmCleanup(lxc_driver_t *driver,
                         virDomainObjPtr vm,
                         virDomainShutoffReason reason)
{
    virCgroupPtr cgroup;
    int i;
    lxcDomainObjPrivatePtr priv = vm->privateData;

    /* now that we know it's stopped call the hook if present */
    if (virHookPresent(VIR_HOOK_DRIVER_LXC)) {
        char *xml = virDomainDefFormat(vm->def, 0);

        /* we can't stop the operation even if the script raised an error */
        virHookCall(VIR_HOOK_DRIVER_LXC, vm->def->name,
                    VIR_HOOK_LXC_OP_STOPPED, VIR_HOOK_SUBOP_END, NULL, xml);
        VIR_FREE(xml);
    }

    virEventRemoveHandle(priv->monitorWatch);
    VIR_FORCE_CLOSE(priv->monitor);

    virFileDeletePid(driver->stateDir, vm->def->name);
    virDomainDeleteConfig(driver->stateDir, NULL, vm);

    virDomainObjSetState(vm, VIR_DOMAIN_SHUTOFF, reason);
    vm->pid = -1;
    vm->def->id = -1;
    priv->monitor = -1;
    priv->monitorWatch = -1;

    for (i = 0 ; i < vm->def->nnets ; i++) {
        vethInterfaceUpOrDown(vm->def->nets[i]->ifname, 0);
        vethDelete(vm->def->nets[i]->ifname);
    }

    if (driver->cgroup &&
        virCgroupForDomain(driver->cgroup, vm->def->name, &cgroup, 0) == 0) {
        virCgroupRemove(cgroup);
        virCgroupFree(&cgroup);
    }

    if (vm->newDef) {
        virDomainDefFree(vm->def);
        vm->def = vm->newDef;
        vm->def->id = -1;
        vm->newDef = NULL;
    }
}

/**
 * lxcSetupInterfaces:
 * @conn: pointer to connection
 * @def: pointer to virtual machine structure
 * @nveths: number of interfaces
 * @veths: interface names
 *
 * Sets up the container interfaces by creating the veth device pairs and
 * attaching the parent end to the appropriate bridge.  The container end
 * will moved into the container namespace later after clone has been called.
 *
 * Returns 0 on success or -1 in case of error
 */
static int lxcSetupInterfaces(virConnectPtr conn,
                              virDomainDefPtr def,
                              unsigned int *nveths,
                              char ***veths)
{
    int rc = -1, i;
    char *bridge = NULL;
    brControl *brctl = NULL;
    int ret;

    if ((ret = brInit(&brctl)) != 0) {
        virReportSystemError(ret, "%s",
                             _("Unable to initialize bridging"));
        return -1;
    }

    for (i = 0 ; i < def->nnets ; i++) {
        char *parentVeth;
        char *containerVeth = NULL;

        switch (def->nets[i]->type) {
        case VIR_DOMAIN_NET_TYPE_NETWORK:
        {
            virNetworkPtr network;

            network = virNetworkLookupByName(conn,
                                             def->nets[i]->data.network.name);
            if (!network) {
                goto error_exit;
            }

            bridge = virNetworkGetBridgeName(network);

            virNetworkFree(network);
            break;
        }
        case VIR_DOMAIN_NET_TYPE_BRIDGE:
            bridge = def->nets[i]->data.bridge.brname;
            break;

        case VIR_DOMAIN_NET_TYPE_USER:
        case VIR_DOMAIN_NET_TYPE_ETHERNET:
        case VIR_DOMAIN_NET_TYPE_SERVER:
        case VIR_DOMAIN_NET_TYPE_CLIENT:
        case VIR_DOMAIN_NET_TYPE_MCAST:
        case VIR_DOMAIN_NET_TYPE_INTERNAL:
        case VIR_DOMAIN_NET_TYPE_DIRECT:
        case VIR_DOMAIN_NET_TYPE_LAST:
            break;
        }

        VIR_DEBUG("bridge: %s", bridge);
        if (NULL == bridge) {
            lxcError(VIR_ERR_INTERNAL_ERROR,
                     "%s", _("Failed to get bridge for interface"));
            goto error_exit;
        }

        VIR_DEBUG("calling vethCreate()");
        parentVeth = def->nets[i]->ifname;
        if (vethCreate(&parentVeth, &containerVeth) < 0)
            goto error_exit;
        VIR_DEBUG("parentVeth: %s, containerVeth: %s", parentVeth, containerVeth);

        if (NULL == def->nets[i]->ifname) {
            def->nets[i]->ifname = parentVeth;
        }

        if (VIR_REALLOC_N(*veths, (*nveths)+1) < 0) {
            virReportOOMError();
            VIR_FREE(containerVeth);
            goto error_exit;
        }
        (*veths)[(*nveths)] = containerVeth;
        (*nveths)++;

        {
            char macaddr[VIR_MAC_STRING_BUFLEN];
            virFormatMacAddr(def->nets[i]->mac, macaddr);
            if (setMacAddr(containerVeth, macaddr) < 0)
                goto error_exit;
        }

        if ((ret = brAddInterface(brctl, bridge, parentVeth)) != 0) {
            virReportSystemError(ret,
                                 _("Failed to add %s device to %s"),
                                 parentVeth, bridge);
            goto error_exit;
        }

        if (vethInterfaceUpOrDown(parentVeth, 1) < 0)
            goto error_exit;
    }

    rc = 0;

error_exit:
    brShutdown(brctl);
    return rc;
}


static int lxcMonitorClient(lxc_driver_t * driver,
                            virDomainObjPtr vm)
{
    char *sockpath = NULL;
    int fd;
    struct sockaddr_un addr;

    if (virAsprintf(&sockpath, "%s/%s.sock",
                    driver->stateDir, vm->def->name) < 0) {
        virReportOOMError();
        return -1;
    }

    if ((fd = socket(PF_UNIX, SOCK_STREAM, 0)) < 0) {
        virReportSystemError(errno, "%s",
                             _("Failed to create client socket"));
        goto error;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (virStrcpyStatic(addr.sun_path, sockpath) == NULL) {
        lxcError(VIR_ERR_INTERNAL_ERROR,
                 _("Socket path %s too big for destination"), sockpath);
        goto error;
    }

    if (connect(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
        virReportSystemError(errno, "%s",
                             _("Failed to connect to client socket"));
        goto error;
    }

    VIR_FREE(sockpath);
    return fd;

error:
    VIR_FREE(sockpath);
    VIR_FORCE_CLOSE(fd);
    return -1;
}


static int lxcVmTerminate(lxc_driver_t *driver,
                          virDomainObjPtr vm,
                          virDomainShutoffReason reason)
{
    virCgroupPtr group = NULL;
    int rc;

    if (vm->pid <= 0) {
        lxcError(VIR_ERR_INTERNAL_ERROR,
                 _("Invalid PID %d for container"), vm->pid);
        return -1;
    }

    if (virCgroupForDomain(driver->cgroup, vm->def->name, &group, 0) != 0)
        return -1;

    rc = virCgroupKillPainfully(group);
    if (rc < 0) {
        virReportSystemError(-rc, "%s",
                             _("Failed to kill container PIDs"));
        rc = -1;
        goto cleanup;
    }
    if (rc == 1) {
        lxcError(VIR_ERR_INTERNAL_ERROR, "%s",
                 _("Some container PIDs refused to die"));
        rc = -1;
        goto cleanup;
    }
    lxcVmCleanup(driver, vm, reason);

    rc = 0;

cleanup:
    virCgroupFree(&group);
    return rc;
}

static void lxcMonitorEvent(int watch,
                            int fd,
                            int events ATTRIBUTE_UNUSED,
                            void *data)
{
    lxc_driver_t *driver = lxc_driver;
    virDomainObjPtr vm = data;
    virDomainEventPtr event = NULL;
    lxcDomainObjPrivatePtr priv;

    lxcDriverLock(driver);
    virDomainObjLock(vm);
    lxcDriverUnlock(driver);

    priv = vm->privateData;

    if (priv->monitor != fd || priv->monitorWatch != watch) {
        virEventRemoveHandle(watch);
        goto cleanup;
    }

    if (lxcVmTerminate(driver, vm, VIR_DOMAIN_SHUTOFF_SHUTDOWN) < 0) {
        virEventRemoveHandle(watch);
    } else {
        event = virDomainEventNewFromObj(vm,
                                         VIR_DOMAIN_EVENT_STOPPED,
                                         VIR_DOMAIN_EVENT_STOPPED_SHUTDOWN);
    }
    if (!vm->persistent) {
        virDomainRemoveInactive(&driver->domains, vm);
        vm = NULL;
    }

cleanup:
    if (vm)
        virDomainObjUnlock(vm);
    if (event) {
        lxcDriverLock(driver);
        lxcDomainEventQueue(driver, event);
        lxcDriverUnlock(driver);
    }
}


static int lxcControllerStart(lxc_driver_t *driver,
                              virDomainObjPtr vm,
                              int nveths,
                              char **veths,
                              int appPty,
                              int logfile)
{
    int i;
    int ret = -1;
    char *filterstr;
    char *outputstr;
    virCommandPtr cmd;
    off_t pos = -1;
    char ebuf[1024];
    char *timestamp;

    cmd = virCommandNew(vm->def->emulator);

    /* The controller may call ip command, so we have to retain PATH. */
    virCommandAddEnvPass(cmd, "PATH");

    virCommandAddEnvFormat(cmd, "LIBVIRT_DEBUG=%d",
                           virLogGetDefaultPriority());

    if (virLogGetNbFilters() > 0) {
        filterstr = virLogGetFilters();
        if (!filterstr) {
            virReportOOMError();
            goto cleanup;
        }

        virCommandAddEnvPair(cmd, "LIBVIRT_LOG_FILTERS", filterstr);
        VIR_FREE(filterstr);
    }

    if (driver->log_libvirtd) {
        if (virLogGetNbOutputs() > 0) {
            outputstr = virLogGetOutputs();
            if (!outputstr) {
                virReportOOMError();
                goto cleanup;
            }

            virCommandAddEnvPair(cmd, "LIBVIRT_LOG_OUTPUTS", outputstr);
            VIR_FREE(outputstr);
        }
    } else {
        virCommandAddEnvFormat(cmd,
                               "LIBVIRT_LOG_OUTPUTS=%d:stderr",
                               virLogGetDefaultPriority());
    }

    virCommandAddArgList(cmd, "--name", vm->def->name, "--console", NULL);
    virCommandAddArgFormat(cmd, "%d", appPty);
    virCommandAddArg(cmd, "--background");

    for (i = 0 ; i < nveths ; i++) {
        virCommandAddArgList(cmd, "--veth", veths[i], NULL);
    }

    /* now that we know it is about to start call the hook if present */
    if (virHookPresent(VIR_HOOK_DRIVER_LXC)) {
        char *xml = virDomainDefFormat(vm->def, 0);
        int hookret;

        hookret = virHookCall(VIR_HOOK_DRIVER_LXC, vm->def->name,
                    VIR_HOOK_LXC_OP_START, VIR_HOOK_SUBOP_BEGIN, NULL, xml);
        VIR_FREE(xml);

        /*
         * If the script raised an error abort the launch
         */
        if (hookret < 0)
            goto cleanup;
    }

    /* Log timestamp */
    if ((timestamp = virTimestamp()) == NULL) {
        virReportOOMError();
        goto cleanup;
    }
    if (safewrite(logfile, timestamp, strlen(timestamp)) < 0 ||
        safewrite(logfile, START_POSTFIX, strlen(START_POSTFIX)) < 0) {
        VIR_WARN("Unable to write timestamp to logfile: %s",
                 virStrerror(errno, ebuf, sizeof ebuf));
    }
    VIR_FREE(timestamp);

    /* Log generated command line */
    virCommandWriteArgLog(cmd, logfile);
    if ((pos = lseek(logfile, 0, SEEK_END)) < 0)
        VIR_WARN("Unable to seek to end of logfile: %s",
                 virStrerror(errno, ebuf, sizeof ebuf));

    virCommandPreserveFD(cmd, appPty);
    virCommandSetOutputFD(cmd, &logfile);
    virCommandSetErrorFD(cmd, &logfile);

    ret = virCommandRun(cmd, NULL);

cleanup:
    virCommandFree(cmd);
    return ret;
}


/**
 * lxcVmStart:
 * @conn: pointer to connection
 * @driver: pointer to driver structure
 * @vm: pointer to virtual machine structure
 * @reason: reason for switching vm to running state
 *
 * Starts a vm
 *
 * Returns 0 on success or -1 in case of error
 */
static int lxcVmStart(virConnectPtr conn,
                      lxc_driver_t * driver,
                      virDomainObjPtr vm,
                      virDomainRunningReason reason)
{
    int rc = -1, r;
    unsigned int i;
    int parentTty;
    char *parentTtyPath = NULL;
    char *logfile = NULL;
    int logfd = -1;
    unsigned int nveths = 0;
    char **veths = NULL;
    lxcDomainObjPrivatePtr priv = vm->privateData;

    if (!lxc_driver->cgroup) {
        lxcError(VIR_ERR_INTERNAL_ERROR, "%s",
                 _("The 'cpuacct', 'devices' & 'memory' cgroups controllers must be mounted"));
        return -1;
    }

    if (!virCgroupMounted(lxc_driver->cgroup,
                          VIR_CGROUP_CONTROLLER_CPUACCT)) {
        lxcError(VIR_ERR_INTERNAL_ERROR, "%s",
                 _("Unable to find 'cpuacct' cgroups controller mount"));
        return -1;
    }
    if (!virCgroupMounted(lxc_driver->cgroup,
                          VIR_CGROUP_CONTROLLER_DEVICES)) {
        lxcError(VIR_ERR_INTERNAL_ERROR, "%s",
                 _("Unable to find 'devices' cgroups controller mount"));
        return -1;
    }
    if (!virCgroupMounted(lxc_driver->cgroup,
                          VIR_CGROUP_CONTROLLER_MEMORY)) {
        lxcError(VIR_ERR_INTERNAL_ERROR, "%s",
                 _("Unable to find 'memory' cgroups controller mount"));
        return -1;
    }

    if ((r = virFileMakePath(driver->logDir)) != 0) {
        virReportSystemError(r,
                             _("Cannot create log directory '%s'"),
                             driver->logDir);
        return -1;
    }

    if (virAsprintf(&logfile, "%s/%s.log",
                    driver->logDir, vm->def->name) < 0) {
        virReportOOMError();
        return -1;
    }

    /* open parent tty */
    if (virFileOpenTty(&parentTty, &parentTtyPath, 1) < 0) {
        virReportSystemError(errno, "%s",
                             _("Failed to allocate tty"));
        goto cleanup;
    }
    if (vm->def->console &&
        vm->def->console->source.type == VIR_DOMAIN_CHR_TYPE_PTY) {
        VIR_FREE(vm->def->console->source.data.file.path);
        vm->def->console->source.data.file.path = parentTtyPath;
    } else {
        VIR_FREE(parentTtyPath);
    }

    if (lxcSetupInterfaces(conn, vm->def, &nveths, &veths) != 0)
        goto cleanup;

    /* Save the configuration for the controller */
    if (virDomainSaveConfig(driver->stateDir, vm->def) < 0)
        goto cleanup;

    if ((logfd = open(logfile, O_WRONLY | O_APPEND | O_CREAT,
             S_IRUSR|S_IWUSR)) < 0) {
        virReportSystemError(errno,
                             _("Failed to open '%s'"),
                             logfile);
        goto cleanup;
    }

    if (lxcControllerStart(driver,
                           vm,
                           nveths, veths,
                           parentTty, logfd) < 0)
        goto cleanup;

    /* Connect to the controller as a client *first* because
     * this will block until the child has written their
     * pid file out to disk */
    if ((priv->monitor = lxcMonitorClient(driver, vm)) < 0)
        goto cleanup;

    /* And get its pid */
    if ((r = virFileReadPid(driver->stateDir, vm->def->name, &vm->pid)) != 0) {
        virReportSystemError(r,
                             _("Failed to read pid file %s/%s.pid"),
                             driver->stateDir, vm->def->name);
        goto cleanup;
    }

    vm->def->id = vm->pid;
    virDomainObjSetState(vm, VIR_DOMAIN_RUNNING, reason);

    if ((priv->monitorWatch = virEventAddHandle(
             priv->monitor,
             VIR_EVENT_HANDLE_ERROR | VIR_EVENT_HANDLE_HANGUP,
             lxcMonitorEvent,
             vm, NULL)) < 0) {
        lxcVmTerminate(driver, vm, VIR_DOMAIN_SHUTOFF_FAILED);
        goto cleanup;
    }

    /*
     * Again, need to save the live configuration, because the function
     * requires vm->def->id != -1 to save tty info surely.
     */
    if (virDomainSaveConfig(driver->stateDir, vm->def) < 0)
        goto cleanup;

    if (virDomainObjSetDefTransient(driver->caps, vm, false) < 0)
        goto cleanup;

    rc = 0;

cleanup:
    if (VIR_CLOSE(logfd) < 0) {
        virReportSystemError(errno, "%s", _("could not close logfile"));
        rc = -1;
    }
    for (i = 0 ; i < nveths ; i++) {
        if (rc != 0)
            vethDelete(veths[i]);
        VIR_FREE(veths[i]);
    }
    if (rc != 0)
        VIR_FORCE_CLOSE(priv->monitor);
    VIR_FORCE_CLOSE(parentTty);
    VIR_FREE(logfile);
    return rc;
}

/**
 * lxcDomainStartWithFlags:
 * @dom: domain to start
 * @flags: Must be 0 for now
 *
 * Looks up domain and starts it.
 *
 * Returns 0 on success or -1 in case of error
 */
static int lxcDomainStartWithFlags(virDomainPtr dom, unsigned int flags)
{
    lxc_driver_t *driver = dom->conn->privateData;
    virDomainObjPtr vm;
    virDomainEventPtr event = NULL;
    int ret = -1;

    virCheckFlags(0, -1);

    lxcDriverLock(driver);
    vm = virDomainFindByUUID(&driver->domains, dom->uuid);
    if (!vm) {
        char uuidstr[VIR_UUID_STRING_BUFLEN];
        virUUIDFormat(dom->uuid, uuidstr);
        lxcError(VIR_ERR_NO_DOMAIN,
                 _("No domain with matching uuid '%s'"), uuidstr);
        goto cleanup;
    }

    if ((vm->def->nets != NULL) && !(driver->have_netns)) {
        lxcError(VIR_ERR_NO_SUPPORT,
                 "%s", _("System lacks NETNS support"));
        goto cleanup;
    }

    if (virDomainObjIsActive(vm)) {
        lxcError(VIR_ERR_OPERATION_INVALID,
                 "%s", _("Domain is already running"));
        goto cleanup;
    }

    ret = lxcVmStart(dom->conn, driver, vm, VIR_DOMAIN_RUNNING_BOOTED);

    if (ret == 0)
        event = virDomainEventNewFromObj(vm,
                                         VIR_DOMAIN_EVENT_STARTED,
                                         VIR_DOMAIN_EVENT_STARTED_BOOTED);

cleanup:
    if (vm)
        virDomainObjUnlock(vm);
    if (event)
        lxcDomainEventQueue(driver, event);
    lxcDriverUnlock(driver);
    return ret;
}

/**
 * lxcDomainStart:
 * @dom: domain to start
 *
 * Looks up domain and starts it.
 *
 * Returns 0 on success or -1 in case of error
 */
static int lxcDomainStart(virDomainPtr dom)
{
    return lxcDomainStartWithFlags(dom, 0);
}

/**
 * lxcDomainCreateAndStart:
 * @conn: pointer to connection
 * @xml: XML definition of domain
 * @flags: Must be 0 for now
 *
 * Creates a domain based on xml and starts it
 *
 * Returns 0 on success or -1 in case of error
 */
static virDomainPtr
lxcDomainCreateAndStart(virConnectPtr conn,
                        const char *xml,
                        unsigned int flags) {
    lxc_driver_t *driver = conn->privateData;
    virDomainObjPtr vm = NULL;
    virDomainDefPtr def;
    virDomainPtr dom = NULL;
    virDomainEventPtr event = NULL;

    virCheckFlags(0, NULL);

    lxcDriverLock(driver);
    if (!(def = virDomainDefParseString(driver->caps, xml,
                                        VIR_DOMAIN_XML_INACTIVE)))
        goto cleanup;

    if (virDomainObjIsDuplicate(&driver->domains, def, 1) < 0)
        goto cleanup;

    if ((def->nets != NULL) && !(driver->have_netns)) {
        lxcError(VIR_ERR_NO_SUPPORT,
                 "%s", _("System lacks NETNS support"));
        goto cleanup;
    }


    if (!(vm = virDomainAssignDef(driver->caps,
                                  &driver->domains, def, false)))
        goto cleanup;
    def = NULL;

    if (lxcVmStart(conn, driver, vm, VIR_DOMAIN_RUNNING_BOOTED) < 0) {
        virDomainRemoveInactive(&driver->domains, vm);
        vm = NULL;
        goto cleanup;
    }

    event = virDomainEventNewFromObj(vm,
                                     VIR_DOMAIN_EVENT_STARTED,
                                     VIR_DOMAIN_EVENT_STARTED_BOOTED);

    dom = virGetDomain(conn, vm->def->name, vm->def->uuid);
    if (dom)
        dom->id = vm->def->id;

cleanup:
    virDomainDefFree(def);
    if (vm)
        virDomainObjUnlock(vm);
    if (event)
        lxcDomainEventQueue(driver, event);
    lxcDriverUnlock(driver);
    return dom;
}


static int
lxcDomainEventRegister(virConnectPtr conn,
                       virConnectDomainEventCallback callback,
                       void *opaque,
                       virFreeCallback freecb)
{
    lxc_driver_t *driver = conn->privateData;
    int ret;

    lxcDriverLock(driver);
    ret = virDomainEventCallbackListAdd(conn,
                                        driver->domainEventState->callbacks,
                                        callback, opaque, freecb);
    lxcDriverUnlock(driver);

    return ret;
}


static int
lxcDomainEventDeregister(virConnectPtr conn,
                         virConnectDomainEventCallback callback)
{
    lxc_driver_t *driver = conn->privateData;
    int ret;

    lxcDriverLock(driver);
    ret = virDomainEventStateDeregister(conn,
                                        driver->domainEventState,
                                        callback);
    lxcDriverUnlock(driver);

    return ret;
}


static int
lxcDomainEventRegisterAny(virConnectPtr conn,
                          virDomainPtr dom,
                          int eventID,
                          virConnectDomainEventGenericCallback callback,
                          void *opaque,
                          virFreeCallback freecb)
{
    lxc_driver_t *driver = conn->privateData;
    int ret;

    lxcDriverLock(driver);
    ret = virDomainEventCallbackListAddID(conn,
                                          driver->domainEventState->callbacks,
                                          dom, eventID,
                                          callback, opaque, freecb);
    lxcDriverUnlock(driver);

    return ret;
}


static int
lxcDomainEventDeregisterAny(virConnectPtr conn,
                            int callbackID)
{
    lxc_driver_t *driver = conn->privateData;
    int ret;

    lxcDriverLock(driver);
    ret = virDomainEventStateDeregisterAny(conn,
                                           driver->domainEventState,
                                           callbackID);
    lxcDriverUnlock(driver);

    return ret;
}


static void lxcDomainEventDispatchFunc(virConnectPtr conn,
                                       virDomainEventPtr event,
                                       virConnectDomainEventGenericCallback cb,
                                       void *cbopaque,
                                       void *opaque)
{
    lxc_driver_t *driver = opaque;

    /* Drop the lock whle dispatching, for sake of re-entrancy */
    lxcDriverUnlock(driver);
    virDomainEventDispatchDefaultFunc(conn, event, cb, cbopaque, NULL);
    lxcDriverLock(driver);
}


static void lxcDomainEventFlush(int timer ATTRIBUTE_UNUSED, void *opaque)
{
    lxc_driver_t *driver = opaque;

    lxcDriverLock(driver);
    virDomainEventStateFlush(driver->domainEventState,
                             lxcDomainEventDispatchFunc,
                             driver);
    lxcDriverUnlock(driver);
}


/* driver must be locked before calling */
static void lxcDomainEventQueue(lxc_driver_t *driver,
                                 virDomainEventPtr event)
{
    virDomainEventStateQueue(driver->domainEventState, event);
}

/**
 * lxcDomainDestroy:
 * @dom: pointer to domain to destroy
 *
 * Sends SIGKILL to container root process to terminate the container
 *
 * Returns 0 on success or -1 in case of error
 */
static int lxcDomainDestroy(virDomainPtr dom)
{
    lxc_driver_t *driver = dom->conn->privateData;
    virDomainObjPtr vm;
    virDomainEventPtr event = NULL;
    int ret = -1;

    lxcDriverLock(driver);
    vm = virDomainFindByUUID(&driver->domains, dom->uuid);
    if (!vm) {
        char uuidstr[VIR_UUID_STRING_BUFLEN];
        virUUIDFormat(dom->uuid, uuidstr);
        lxcError(VIR_ERR_NO_DOMAIN,
                 _("No domain with matching uuid '%s'"), uuidstr);
        goto cleanup;
    }

    if (!virDomainObjIsActive(vm)) {
        lxcError(VIR_ERR_OPERATION_INVALID,
                 "%s", _("Domain is not running"));
        goto cleanup;
    }

    ret = lxcVmTerminate(driver, vm, VIR_DOMAIN_SHUTOFF_DESTROYED);
    event = virDomainEventNewFromObj(vm,
                                     VIR_DOMAIN_EVENT_STOPPED,
                                     VIR_DOMAIN_EVENT_STOPPED_DESTROYED);
    if (!vm->persistent) {
        virDomainRemoveInactive(&driver->domains, vm);
        vm = NULL;
    }

cleanup:
    if (vm)
        virDomainObjUnlock(vm);
    if (event)
        lxcDomainEventQueue(driver, event);
    lxcDriverUnlock(driver);
    return ret;
}

static int lxcCheckNetNsSupport(void)
{
    const char *argv[] = {"ip", "link", "set", "lo", "netns", "-1", NULL};
    int ip_rc;

    if (virRun(argv, &ip_rc) < 0 ||
        !(WIFEXITED(ip_rc) && (WEXITSTATUS(ip_rc) != 255)))
        return 0;

    if (lxcContainerAvailable(LXC_CONTAINER_FEATURE_NET) < 0)
        return 0;

    return 1;
}


struct lxcAutostartData {
    lxc_driver_t *driver;
    virConnectPtr conn;
};

static void
lxcAutostartDomain(void *payload, const void *name ATTRIBUTE_UNUSED, void *opaque)
{
    virDomainObjPtr vm = payload;
    const struct lxcAutostartData *data = opaque;

    virDomainObjLock(vm);
    if (vm->autostart &&
        !virDomainObjIsActive(vm)) {
        int ret = lxcVmStart(data->conn, data->driver, vm,
                             VIR_DOMAIN_RUNNING_BOOTED);
        if (ret < 0) {
            virErrorPtr err = virGetLastError();
            VIR_ERROR(_("Failed to autostart VM '%s': %s"),
                      vm->def->name,
                      err ? err->message : "");
        } else {
            virDomainEventPtr event =
                virDomainEventNewFromObj(vm,
                                         VIR_DOMAIN_EVENT_STARTED,
                                         VIR_DOMAIN_EVENT_STARTED_BOOTED);
            if (event)
                lxcDomainEventQueue(data->driver, event);
        }
    }
    virDomainObjUnlock(vm);
}

static void
lxcAutostartConfigs(lxc_driver_t *driver) {
    /* XXX: Figure out a better way todo this. The domain
     * startup code needs a connection handle in order
     * to lookup the bridge associated with a virtual
     * network
     */
    virConnectPtr conn = virConnectOpen("lxc:///");
    /* Ignoring NULL conn which is mostly harmless here */

    struct lxcAutostartData data = { driver, conn };

    lxcDriverLock(driver);
    virHashForEach(driver->domains.objs, lxcAutostartDomain, &data);
    lxcDriverUnlock(driver);

    if (conn)
        virConnectClose(conn);
}

static void
lxcReconnectVM(void *payload, const void *name ATTRIBUTE_UNUSED, void *opaque)
{
    virDomainObjPtr vm = payload;
    lxc_driver_t *driver = opaque;
    char *config = NULL;
    virDomainDefPtr tmp;
    lxcDomainObjPrivatePtr priv;

    virDomainObjLock(vm);

    priv = vm->privateData;
    if ((priv->monitor = lxcMonitorClient(driver, vm)) < 0) {
        goto cleanup;
    }

    /* Read pid from controller */
    if ((virFileReadPid(lxc_driver->stateDir, vm->def->name, &vm->pid)) != 0) {
        VIR_FORCE_CLOSE(priv->monitor);
        goto cleanup;
    }

    if ((config = virDomainConfigFile(driver->stateDir,
                                      vm->def->name)) == NULL)
        goto cleanup;

    /* Try and load the live config */
    tmp = virDomainDefParseFile(driver->caps, config, 0);
    VIR_FREE(config);
    if (tmp) {
        vm->newDef = vm->def;
        vm->def = tmp;
    }

    if (vm->pid != 0) {
        vm->def->id = vm->pid;
        virDomainObjSetState(vm, VIR_DOMAIN_RUNNING,
                             VIR_DOMAIN_RUNNING_UNKNOWN);

        if ((priv->monitorWatch = virEventAddHandle(
                 priv->monitor,
                 VIR_EVENT_HANDLE_ERROR | VIR_EVENT_HANDLE_HANGUP,
                 lxcMonitorEvent,
                 vm, NULL)) < 0) {
            lxcVmTerminate(driver, vm, VIR_DOMAIN_SHUTOFF_FAILED);
            goto cleanup;
        }
    } else {
        vm->def->id = -1;
        VIR_FORCE_CLOSE(priv->monitor);
    }

cleanup:
    virDomainObjUnlock(vm);
}


static int lxcStartup(int privileged)
{
    char *ld;
    int rc;

    /* Valgrind gets very annoyed when we clone containers, so
     * disable LXC when under valgrind
     * XXX remove this when valgrind is fixed
     */
    ld = getenv("LD_PRELOAD");
    if (ld && strstr(ld, "vgpreload")) {
        VIR_INFO("Running under valgrind, disabling driver");
        return 0;
    }

    /* Check that the user is root, silently disable if not */
    if (!privileged) {
        VIR_INFO("Not running privileged, disabling driver");
        return 0;
    }

    /* Check that this is a container enabled kernel */
    if (lxcContainerAvailable(0) < 0) {
        VIR_INFO("LXC support not available in this kernel, disabling driver");
        return 0;
    }

    if (VIR_ALLOC(lxc_driver) < 0) {
        return -1;
    }
    if (virMutexInit(&lxc_driver->lock) < 0) {
        VIR_FREE(lxc_driver);
        return -1;
    }
    lxcDriverLock(lxc_driver);

    if (virDomainObjListInit(&lxc_driver->domains) < 0)
        goto cleanup;

    lxc_driver->domainEventState = virDomainEventStateNew(lxcDomainEventFlush,
                                                          lxc_driver,
                                                          NULL,
                                                          true);
    if (!lxc_driver->domainEventState)
        goto cleanup;

    lxc_driver->log_libvirtd = 0; /* by default log to container logfile */
    lxc_driver->have_netns = lxcCheckNetNsSupport();

    rc = virCgroupForDriver("lxc", &lxc_driver->cgroup, privileged, 1);
    if (rc < 0) {
        char buf[1024];
        VIR_DEBUG("Unable to create cgroup for LXC driver: %s",
                  virStrerror(-rc, buf, sizeof(buf)));
        /* Don't abort startup. We will explicitly report to
         * the user when they try to start a VM
         */
    }

    /* Call function to load lxc driver configuration information */
    if (lxcLoadDriverConfig(lxc_driver) < 0)
        goto cleanup;

    if ((lxc_driver->caps = lxcCapsInit()) == NULL)
        goto cleanup;

    lxc_driver->caps->privateDataAllocFunc = lxcDomainObjPrivateAlloc;
    lxc_driver->caps->privateDataFreeFunc = lxcDomainObjPrivateFree;

    if (virDomainLoadAllConfigs(lxc_driver->caps,
                                &lxc_driver->domains,
                                lxc_driver->configDir,
                                lxc_driver->autostartDir,
                                0, NULL, NULL) < 0)
        goto cleanup;

    virHashForEach(lxc_driver->domains.objs, lxcReconnectVM, lxc_driver);

    lxcDriverUnlock(lxc_driver);

    lxcAutostartConfigs(lxc_driver);

    return 0;

cleanup:
    lxcDriverUnlock(lxc_driver);
    lxcShutdown();
    return -1;
}

static void lxcNotifyLoadDomain(virDomainObjPtr vm, int newVM, void *opaque)
{
    lxc_driver_t *driver = opaque;

    if (newVM) {
        virDomainEventPtr event =
            virDomainEventNewFromObj(vm,
                                     VIR_DOMAIN_EVENT_DEFINED,
                                     VIR_DOMAIN_EVENT_DEFINED_ADDED);
        if (event)
            lxcDomainEventQueue(driver, event);
    }
}

/**
 * lxcReload:
 *
 * Function to restart the LXC driver, it will recheck the configuration
 * files and perform autostart
 */
static int
lxcReload(void) {
    if (!lxc_driver)
        return 0;

    lxcDriverLock(lxc_driver);
    virDomainLoadAllConfigs(lxc_driver->caps,
                            &lxc_driver->domains,
                            lxc_driver->configDir,
                            lxc_driver->autostartDir,
                            0, lxcNotifyLoadDomain, lxc_driver);
    lxcDriverUnlock(lxc_driver);

    lxcAutostartConfigs(lxc_driver);

    return 0;
}

static int lxcShutdown(void)
{
    if (lxc_driver == NULL)
        return(-1);

    lxcDriverLock(lxc_driver);
    virDomainObjListDeinit(&lxc_driver->domains);
    virDomainEventStateFree(lxc_driver->domainEventState);

    virCapabilitiesFree(lxc_driver->caps);
    VIR_FREE(lxc_driver->configDir);
    VIR_FREE(lxc_driver->autostartDir);
    VIR_FREE(lxc_driver->stateDir);
    VIR_FREE(lxc_driver->logDir);
    lxcDriverUnlock(lxc_driver);
    virMutexDestroy(&lxc_driver->lock);
    VIR_FREE(lxc_driver);

    return 0;
}

/**
 * lxcActive:
 *
 * Checks if the LXC daemon is active, i.e. has an active domain
 *
 * Returns 1 if active, 0 otherwise
 */
static int
lxcActive(void) {
    int active;

    if (lxc_driver == NULL)
        return(0);

    lxcDriverLock(lxc_driver);
    active = virDomainObjListNumOfDomains(&lxc_driver->domains, 1);
    lxcDriverUnlock(lxc_driver);

    return active;
}

static int lxcVersion(virConnectPtr conn ATTRIBUTE_UNUSED, unsigned long *version)
{
    struct utsname ver;

    uname(&ver);

    if (virParseVersionString(ver.release, version) < 0) {
        lxcError(VIR_ERR_INTERNAL_ERROR, _("Unknown release: %s"), ver.release);
        return -1;
    }

    return 0;
}

static char *lxcGetSchedulerType(virDomainPtr domain ATTRIBUTE_UNUSED,
                                 int *nparams)
{
    char *schedulerType = NULL;

    if (nparams)
        *nparams = 1;

    schedulerType = strdup("posix");

    if (schedulerType == NULL)
        virReportOOMError();

    return schedulerType;
}

static int lxcSetSchedulerParameters(virDomainPtr domain,
                                     virTypedParameterPtr params,
                                     int nparams)
{
    lxc_driver_t *driver = domain->conn->privateData;
    int i;
    virCgroupPtr group = NULL;
    virDomainObjPtr vm = NULL;
    int ret = -1;

    if (driver->cgroup == NULL)
        return -1;

    lxcDriverLock(driver);
    vm = virDomainFindByUUID(&driver->domains, domain->uuid);

    if (vm == NULL) {
        char uuidstr[VIR_UUID_STRING_BUFLEN];
        virUUIDFormat(domain->uuid, uuidstr);
        lxcError(VIR_ERR_NO_DOMAIN,
                 _("No domain with matching uuid '%s'"), uuidstr);
        goto cleanup;
    }

    if (virCgroupForDomain(driver->cgroup, vm->def->name, &group, 0) != 0)
        goto cleanup;

    for (i = 0; i < nparams; i++) {
        virTypedParameterPtr param = &params[i];

        if (STRNEQ(param->field, "cpu_shares")) {
            lxcError(VIR_ERR_INVALID_ARG,
                     _("Invalid parameter `%s'"), param->field);
            goto cleanup;
        }

        if (param->type != VIR_TYPED_PARAM_ULLONG) {
            lxcError(VIR_ERR_INVALID_ARG, "%s",
                 _("Invalid type for cpu_shares tunable, expected a 'ullong'"));
            goto cleanup;
        }

        int rc = virCgroupSetCpuShares(group, params[i].value.ul);
        if (rc != 0) {
            virReportSystemError(-rc, _("failed to set cpu_shares=%llu"),
                                 params[i].value.ul);
            goto cleanup;
        }

        vm->def->cputune.shares = params[i].value.ul;
    }
    ret = 0;

cleanup:
    lxcDriverUnlock(driver);
    virCgroupFree(&group);
    if (vm)
        virDomainObjUnlock(vm);
    return ret;
}

static int lxcGetSchedulerParameters(virDomainPtr domain,
                                     virTypedParameterPtr params,
                                     int *nparams)
{
    lxc_driver_t *driver = domain->conn->privateData;
    virCgroupPtr group = NULL;
    virDomainObjPtr vm = NULL;
    unsigned long long val;
    int ret = -1;

    if (driver->cgroup == NULL)
        return -1;

    if (*nparams < 1) {
        lxcError(VIR_ERR_INVALID_ARG,
                 "%s", _("Invalid parameter count"));
        return -1;
    }

    lxcDriverLock(driver);
    vm = virDomainFindByUUID(&driver->domains, domain->uuid);

    if (vm == NULL) {
        char uuidstr[VIR_UUID_STRING_BUFLEN];
        virUUIDFormat(domain->uuid, uuidstr);
        lxcError(VIR_ERR_NO_DOMAIN,
                 _("No domain with matching uuid '%s'"), uuidstr);
        goto cleanup;
    }

    if (virCgroupForDomain(driver->cgroup, vm->def->name, &group, 0) != 0)
        goto cleanup;

    if (virCgroupGetCpuShares(group, &val) != 0)
        goto cleanup;
    params[0].value.ul = val;
    if (virStrcpyStatic(params[0].field, "cpu_shares") == NULL) {
        lxcError(VIR_ERR_INTERNAL_ERROR,
                 "%s", _("Field cpu_shares too big for destination"));
        goto cleanup;
    }
    params[0].type = VIR_TYPED_PARAM_ULLONG;

    *nparams = 1;
    ret = 0;

cleanup:
    lxcDriverUnlock(driver);
    virCgroupFree(&group);
    if (vm)
        virDomainObjUnlock(vm);
    return ret;
}

#ifdef __linux__
static int
lxcDomainInterfaceStats(virDomainPtr dom,
                        const char *path,
                        struct _virDomainInterfaceStats *stats)
{
    lxc_driver_t *driver = dom->conn->privateData;
    virDomainObjPtr vm;
    int i;
    int ret = -1;

    lxcDriverLock(driver);
    vm = virDomainFindByUUID(&driver->domains, dom->uuid);
    lxcDriverUnlock(driver);

    if (!vm) {
        char uuidstr[VIR_UUID_STRING_BUFLEN];
        virUUIDFormat(dom->uuid, uuidstr);
        lxcError(VIR_ERR_NO_DOMAIN,
                 _("No domain with matching uuid '%s'"), uuidstr);
        goto cleanup;
    }

    if (!virDomainObjIsActive(vm)) {
        lxcError(VIR_ERR_OPERATION_INVALID,
                 "%s", _("Domain is not running"));
        goto cleanup;
    }

    /* Check the path is one of the domain's network interfaces. */
    for (i = 0 ; i < vm->def->nnets ; i++) {
        if (vm->def->nets[i]->ifname &&
            STREQ(vm->def->nets[i]->ifname, path)) {
            ret = 0;
            break;
        }
    }

    if (ret == 0)
        ret = linuxDomainInterfaceStats(path, stats);
    else
        lxcError(VIR_ERR_INVALID_ARG,
                 _("Invalid path, '%s' is not a known interface"), path);

cleanup:
    if (vm)
        virDomainObjUnlock(vm);
    return ret;
}
#else
static int
lxcDomainInterfaceStats(virDomainPtr dom,
                        const char *path ATTRIBUTE_UNUSED,
                        struct _virDomainInterfaceStats *stats ATTRIBUTE_UNUSED)
    lxcError(VIR_ERR_NO_SUPPORT, "%s", __FUNCTION__);
    return -1;
}
#endif

static int lxcDomainGetAutostart(virDomainPtr dom,
                                   int *autostart) {
    lxc_driver_t *driver = dom->conn->privateData;
    virDomainObjPtr vm;
    int ret = -1;

    lxcDriverLock(driver);
    vm = virDomainFindByUUID(&driver->domains, dom->uuid);
    lxcDriverUnlock(driver);

    if (!vm) {
        char uuidstr[VIR_UUID_STRING_BUFLEN];
        virUUIDFormat(dom->uuid, uuidstr);
        lxcError(VIR_ERR_NO_DOMAIN,
                 _("No domain with matching uuid '%s'"), uuidstr);
        goto cleanup;
    }

    *autostart = vm->autostart;
    ret = 0;

cleanup:
    if (vm)
        virDomainObjUnlock(vm);
    return ret;
}

static int lxcDomainSetAutostart(virDomainPtr dom,
                                   int autostart) {
    lxc_driver_t *driver = dom->conn->privateData;
    virDomainObjPtr vm;
    char *configFile = NULL, *autostartLink = NULL;
    int ret = -1;

    lxcDriverLock(driver);
    vm = virDomainFindByUUID(&driver->domains, dom->uuid);

    if (!vm) {
        char uuidstr[VIR_UUID_STRING_BUFLEN];
        virUUIDFormat(dom->uuid, uuidstr);
        lxcError(VIR_ERR_NO_DOMAIN,
                 _("No domain with matching uuid '%s'"), uuidstr);
        goto cleanup;
    }

    if (!vm->persistent) {
        lxcError(VIR_ERR_OPERATION_INVALID,
                 "%s", _("Cannot set autostart for transient domain"));
        goto cleanup;
    }

    autostart = (autostart != 0);

    if (vm->autostart == autostart) {
        ret = 0;
        goto cleanup;
    }

    configFile = virDomainConfigFile(driver->configDir,
                                     vm->def->name);
    if (configFile == NULL)
        goto cleanup;
    autostartLink = virDomainConfigFile(driver->autostartDir,
                                        vm->def->name);
    if (autostartLink == NULL)
        goto cleanup;

    if (autostart) {
        int err;

        if ((err = virFileMakePath(driver->autostartDir))) {
            virReportSystemError(err,
                                 _("Cannot create autostart directory %s"),
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
    ret = 0;

cleanup:
    VIR_FREE(configFile);
    VIR_FREE(autostartLink);
    if (vm)
        virDomainObjUnlock(vm);
    lxcDriverUnlock(driver);
    return ret;
}

static int lxcFreezeContainer(lxc_driver_t *driver, virDomainObjPtr vm)
{
    int timeout = 1000; /* In milliseconds */
    int check_interval = 1; /* In milliseconds */
    int exp = 10;
    int waited_time = 0;
    int ret = -1;
    char *state = NULL;
    virCgroupPtr cgroup = NULL;

    if (!(driver->cgroup &&
          virCgroupForDomain(driver->cgroup, vm->def->name, &cgroup, 0) == 0))
        return -1;

    /* From here on, we know that cgroup != NULL.  */

    while (waited_time < timeout) {
        int r;
        /*
         * Writing "FROZEN" to the "freezer.state" freezes the group,
         * i.e., the container, temporarily transiting "FREEZING" state.
         * Once the freezing is completed, the state of the group transits
         * to "FROZEN".
         * (see linux-2.6/Documentation/cgroups/freezer-subsystem.txt)
         */
        r = virCgroupSetFreezerState(cgroup, "FROZEN");

        /*
         * Returning EBUSY explicitly indicates that the group is
         * being freezed but incomplete and other errors are true
         * errors.
         */
        if (r < 0 && r != -EBUSY) {
            VIR_DEBUG("Writing freezer.state failed with errno: %d", r);
            goto error;
        }
        if (r == -EBUSY)
            VIR_DEBUG("Writing freezer.state gets EBUSY");

        /*
         * Unfortunately, returning 0 (success) is likely to happen
         * even when the freezing has not been completed. Sometimes
         * the state of the group remains "FREEZING" like when
         * returning -EBUSY and even worse may never transit to
         * "FROZEN" even if writing "FROZEN" again.
         *
         * So we don't trust the return value anyway and always
         * decide that the freezing has been complete only with
         * the state actually transit to "FROZEN".
         */
        usleep(check_interval * 1000);

        r = virCgroupGetFreezerState(cgroup, &state);

        if (r < 0) {
            VIR_DEBUG("Reading freezer.state failed with errno: %d", r);
            goto error;
        }
        VIR_DEBUG("Read freezer.state: %s", state);

        if (STREQ(state, "FROZEN")) {
            ret = 0;
            goto cleanup;
        }

        waited_time += check_interval;
        /*
         * Increasing check_interval exponentially starting with
         * small initial value treats nicely two cases; One is
         * a container is under no load and waiting for long period
         * makes no sense. The other is under heavy load. The container
         * may stay longer time in FREEZING or never transit to FROZEN.
         * In that case, eager polling will just waste CPU time.
         */
        check_interval *= exp;
        VIR_FREE(state);
    }
    VIR_DEBUG("lxcFreezeContainer timeout");
error:
    /*
     * If timeout or an error on reading the state occurs,
     * activate the group again and return an error.
     * This is likely to fall the group back again gracefully.
     */
    virCgroupSetFreezerState(cgroup, "THAWED");
    ret = -1;

cleanup:
    virCgroupFree(&cgroup);
    VIR_FREE(state);
    return ret;
}

static int lxcDomainSuspend(virDomainPtr dom)
{
    lxc_driver_t *driver = dom->conn->privateData;
    virDomainObjPtr vm;
    virDomainEventPtr event = NULL;
    int ret = -1;

    lxcDriverLock(driver);
    vm = virDomainFindByUUID(&driver->domains, dom->uuid);

    if (!vm) {
        char uuidstr[VIR_UUID_STRING_BUFLEN];
        virUUIDFormat(dom->uuid, uuidstr);
        lxcError(VIR_ERR_NO_DOMAIN,
                 _("No domain with matching uuid '%s'"), uuidstr);
        goto cleanup;
    }

    if (!virDomainObjIsActive(vm)) {
        lxcError(VIR_ERR_OPERATION_INVALID,
                 "%s", _("Domain is not running"));
        goto cleanup;
    }

    if (virDomainObjGetState(vm, NULL) != VIR_DOMAIN_PAUSED) {
        if (lxcFreezeContainer(driver, vm) < 0) {
            lxcError(VIR_ERR_OPERATION_FAILED,
                     "%s", _("Suspend operation failed"));
            goto cleanup;
        }
        virDomainObjSetState(vm, VIR_DOMAIN_PAUSED, VIR_DOMAIN_PAUSED_USER);

        event = virDomainEventNewFromObj(vm,
                                         VIR_DOMAIN_EVENT_SUSPENDED,
                                         VIR_DOMAIN_EVENT_SUSPENDED_PAUSED);
    }

    if (virDomainSaveStatus(driver->caps, driver->stateDir, vm) < 0)
        goto cleanup;
    ret = 0;

cleanup:
    if (event)
        lxcDomainEventQueue(driver, event);
    if (vm)
        virDomainObjUnlock(vm);
    lxcDriverUnlock(driver);
    return ret;
}

static int lxcUnfreezeContainer(lxc_driver_t *driver, virDomainObjPtr vm)
{
    int ret;
    virCgroupPtr cgroup = NULL;

    if (!(driver->cgroup &&
        virCgroupForDomain(driver->cgroup, vm->def->name, &cgroup, 0) == 0))
        return -1;

    ret = virCgroupSetFreezerState(cgroup, "THAWED");

    virCgroupFree(&cgroup);
    return ret;
}

static int lxcDomainResume(virDomainPtr dom)
{
    lxc_driver_t *driver = dom->conn->privateData;
    virDomainObjPtr vm;
    virDomainEventPtr event = NULL;
    int ret = -1;

    lxcDriverLock(driver);
    vm = virDomainFindByUUID(&driver->domains, dom->uuid);

    if (!vm) {
        char uuidstr[VIR_UUID_STRING_BUFLEN];
        virUUIDFormat(dom->uuid, uuidstr);
        lxcError(VIR_ERR_NO_DOMAIN,
                 _("No domain with matching uuid '%s'"), uuidstr);
        goto cleanup;
    }

    if (!virDomainObjIsActive(vm)) {
        lxcError(VIR_ERR_OPERATION_INVALID,
                 "%s", _("Domain is not running"));
        goto cleanup;
    }

    if (virDomainObjGetState(vm, NULL) == VIR_DOMAIN_PAUSED) {
        if (lxcUnfreezeContainer(driver, vm) < 0) {
            lxcError(VIR_ERR_OPERATION_FAILED,
                     "%s", _("Resume operation failed"));
            goto cleanup;
        }
        virDomainObjSetState(vm, VIR_DOMAIN_RUNNING,
                             VIR_DOMAIN_RUNNING_UNPAUSED);

        event = virDomainEventNewFromObj(vm,
                                         VIR_DOMAIN_EVENT_RESUMED,
                                         VIR_DOMAIN_EVENT_RESUMED_UNPAUSED);
    }

    if (virDomainSaveStatus(driver->caps, driver->stateDir, vm) < 0)
        goto cleanup;
    ret = 0;

cleanup:
    if (event)
        lxcDomainEventQueue(driver, event);
    if (vm)
        virDomainObjUnlock(vm);
    lxcDriverUnlock(driver);
    return ret;
}

static int
lxcDomainOpenConsole(virDomainPtr dom,
                      const char *devname,
                      virStreamPtr st,
                      unsigned int flags)
{
    lxc_driver_t *driver = dom->conn->privateData;
    virDomainObjPtr vm = NULL;
    char uuidstr[VIR_UUID_STRING_BUFLEN];
    int ret = -1;
    virDomainChrDefPtr chr = NULL;

    virCheckFlags(0, -1);

    lxcDriverLock(driver);
    virUUIDFormat(dom->uuid, uuidstr);
    vm = virDomainFindByUUID(&driver->domains, dom->uuid);
    if (!vm) {
        lxcError(VIR_ERR_NO_DOMAIN,
                 _("no domain with matching uuid '%s'"), uuidstr);
        goto cleanup;
    }

    if (!virDomainObjIsActive(vm)) {
        lxcError(VIR_ERR_OPERATION_INVALID,
                 "%s", _("domain is not running"));
        goto cleanup;
    }

    if (devname) {
        /* XXX support device aliases in future */
        lxcError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                 _("Named device aliases are not supported"));
        goto cleanup;
    } else {
        if (vm->def->console)
            chr = vm->def->console;
        else if (vm->def->nserials)
            chr = vm->def->serials[0];
    }

    if (!chr) {
        lxcError(VIR_ERR_INTERNAL_ERROR, "%s",
                 _("cannot find default console device"));
        goto cleanup;
    }

    if (chr->source.type != VIR_DOMAIN_CHR_TYPE_PTY) {
        lxcError(VIR_ERR_INTERNAL_ERROR,
                 _("character device %s is not using a PTY"), devname);
        goto cleanup;
    }

    if (virFDStreamOpenFile(st, chr->source.data.file.path,
                            0, 0, O_RDWR, false) < 0)
        goto cleanup;

    ret = 0;
cleanup:
    if (vm)
        virDomainObjUnlock(vm);
    lxcDriverUnlock(driver);
    return ret;
}


/* Function Tables */
static virDriver lxcDriver = {
    .no = VIR_DRV_LXC,
    .name = "LXC",
    .open = lxcOpen, /* 0.4.2 */
    .close = lxcClose, /* 0.4.2 */
    .version = lxcVersion, /* 0.4.6 */
    .getHostname = virGetHostname, /* 0.6.3 */
    .nodeGetInfo = nodeGetInfo, /* 0.6.5 */
    .getCapabilities = lxcGetCapabilities, /* 0.6.5 */
    .listDomains = lxcListDomains, /* 0.4.2 */
    .numOfDomains = lxcNumDomains, /* 0.4.2 */
    .domainCreateXML = lxcDomainCreateAndStart, /* 0.4.4 */
    .domainLookupByID = lxcDomainLookupByID, /* 0.4.2 */
    .domainLookupByUUID = lxcDomainLookupByUUID, /* 0.4.2 */
    .domainLookupByName = lxcDomainLookupByName, /* 0.4.2 */
    .domainSuspend = lxcDomainSuspend, /* 0.7.2 */
    .domainResume = lxcDomainResume, /* 0.7.2 */
    .domainDestroy = lxcDomainDestroy, /* 0.4.4 */
    .domainGetOSType = lxcGetOSType, /* 0.4.2 */
    .domainGetMaxMemory = lxcDomainGetMaxMemory, /* 0.7.2 */
    .domainSetMaxMemory = lxcDomainSetMaxMemory, /* 0.7.2 */
    .domainSetMemory = lxcDomainSetMemory, /* 0.7.2 */
    .domainSetMemoryParameters = lxcDomainSetMemoryParameters, /* 0.8.5 */
    .domainGetMemoryParameters = lxcDomainGetMemoryParameters, /* 0.8.5 */
    .domainGetInfo = lxcDomainGetInfo, /* 0.4.2 */
    .domainGetState = lxcDomainGetState, /* 0.9.2 */
    .domainGetXMLDesc = lxcDomainGetXMLDesc, /* 0.4.2 */
    .listDefinedDomains = lxcListDefinedDomains, /* 0.4.2 */
    .numOfDefinedDomains = lxcNumDefinedDomains, /* 0.4.2 */
    .domainCreate = lxcDomainStart, /* 0.4.4 */
    .domainCreateWithFlags = lxcDomainStartWithFlags, /* 0.8.2 */
    .domainDefineXML = lxcDomainDefine, /* 0.4.2 */
    .domainUndefine = lxcDomainUndefine, /* 0.4.2 */
    .domainGetAutostart = lxcDomainGetAutostart, /* 0.7.0 */
    .domainSetAutostart = lxcDomainSetAutostart, /* 0.7.0 */
    .domainGetSchedulerType = lxcGetSchedulerType, /* 0.5.0 */
    .domainGetSchedulerParameters = lxcGetSchedulerParameters, /* 0.5.0 */
    .domainSetSchedulerParameters = lxcSetSchedulerParameters, /* 0.5.0 */
    .domainInterfaceStats = lxcDomainInterfaceStats, /* 0.7.3 */
    .nodeGetCellsFreeMemory = nodeGetCellsFreeMemory, /* 0.6.5 */
    .nodeGetFreeMemory = nodeGetFreeMemory, /* 0.6.5 */
    .domainEventRegister = lxcDomainEventRegister, /* 0.7.0 */
    .domainEventDeregister = lxcDomainEventDeregister, /* 0.7.0 */
    .isEncrypted = lxcIsEncrypted, /* 0.7.3 */
    .isSecure = lxcIsSecure, /* 0.7.3 */
    .domainIsActive = lxcDomainIsActive, /* 0.7.3 */
    .domainIsPersistent = lxcDomainIsPersistent, /* 0.7.3 */
    .domainIsUpdated = lxcDomainIsUpdated, /* 0.8.6 */
    .domainEventRegisterAny = lxcDomainEventRegisterAny, /* 0.8.0 */
    .domainEventDeregisterAny = lxcDomainEventDeregisterAny, /* 0.8.0 */
    .domainOpenConsole = lxcDomainOpenConsole, /* 0.8.6 */
};

static virStateDriver lxcStateDriver = {
    .name = "LXC",
    .initialize = lxcStartup,
    .cleanup = lxcShutdown,
    .active = lxcActive,
    .reload = lxcReload,
};

int lxcRegister(void)
{
    virRegisterDriver(&lxcDriver);
    virRegisterStateDriver(&lxcStateDriver);
    return 0;
}
