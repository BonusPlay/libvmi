/* The LibVMI Library is an introspection library that simplifies access to
 * memory in a target virtual machine or in a file containing a dump of
 * a system's physical memory.  LibVMI is based on the XenAccess Library.
 *
 * Copyright 2011 Sandia Corporation. Under the terms of Contract
 * DE-AC04-94AL85000 with Sandia Corporation, the U.S. Government
 * retains certain rights in this software.
 *
 * Author: Bryan D. Payne (bdpayne@acm.org)
 * Author: Tamas K Lengyel (tamas.lengyel@zentific.com)
 *
 * This file is part of LibVMI.
 *
 * LibVMI is free software: you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * LibVMI is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with LibVMI.  If not, see <http://www.gnu.org/licenses/>.
 */

#define _GNU_SOURCE
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <glib.h>
#include <math.h>
#include <glib/gstdio.h>
#include <libvirt/libvirt.h>
#include <libvirt/virterror.h>

#include "private.h"
#include "driver/driver_wrapper.h"
#include "driver/memory_cache.h"
#include "driver/kvm/kvm.h"
#include "driver/kvm/kvm_private.h"

#include <sys/time.h>
#include "driver/kvm/include/kvmi/libkvmi.h"

/*
 * Helpers
 */
static uint32_t
translate_msr_index(int index, int *err) {
    *err = 0;
    switch (index) {
    case MSR_EFER:                  return 0xc0000080;
    case MSR_STAR:                  return 0xc0000081;
    case MSR_LSTAR:                 return 0xc0000082;
    case MSR_CSTAR:                 return 0xc0000083;
    case MSR_SYSCALL_MASK:          return 0xc0000084;
    case MSR_SHADOW_GS_BASE:        return 0xc0000102;
    case MSR_TSC_AUX:               return 0xc0000103;
    case MSR_MTRRfix64K_00000:      return 0x00000250;
    case MSR_MTRRfix16K_80000:      return 0x00000258;
    case MSR_MTRRfix16K_A0000:      return 0x00000259;
    case MSR_MTRRfix4K_C0000:       return 0x00000268;
    case MSR_MTRRfix4K_C8000:       return 0x00000269;
    case MSR_MTRRfix4K_D0000:       return 0x0000026a;
    case MSR_MTRRfix4K_D8000:       return 0x0000026b;
    case MSR_MTRRfix4K_E0000:       return 0x0000026c;
    case MSR_MTRRfix4K_E8000:       return 0x0000026d;
    case MSR_MTRRfix4K_F0000:       return 0x0000026e;
    case MSR_MTRRfix4K_F8000:       return 0x0000026f;
    case MSR_MTRRdefType:           return 0x000002ff;
    case MSR_IA32_MC0_CTL:          return 0x00000400;
    case MSR_IA32_MC0_STATUS:       return 0x00000401;
    case MSR_IA32_MC0_ADDR:         return 0x00000402;
    case MSR_IA32_MC0_MISC:         return 0x00000403;
    case MSR_IA32_MC1_CTL:          return 0x00000404;
    case MSR_IA32_MC0_CTL2:         return 0x00000280;
    case MSR_AMD_PATCHLEVEL:        return 0x0000008b;
    case MSR_AMD64_TSC_RATIO:       return 0xc0000104;
    case MSR_IA32_P5_MC_ADDR:       return 0x00000000;
    case MSR_IA32_P5_MC_TYPE:       return 0x00000001;
    case MSR_IA32_TSC:              return 0x00000010;
    case MSR_IA32_PLATFORM_ID:      return 0x00000017;
    case MSR_IA32_EBL_CR_POWERON:   return 0x0000002a;
    case MSR_IA32_EBC_FREQUENCY_ID: return 0x0000002c;
    case MSR_IA32_FEATURE_CONTROL:  return 0x0000003a;
    case MSR_IA32_SYSENTER_CS:      return 0x00000174;
    case MSR_IA32_SYSENTER_ESP:     return 0x00000175;
    case MSR_IA32_SYSENTER_EIP:     return 0x00000176;
    case MSR_IA32_MISC_ENABLE:      return 0x000001a0;
    case MSR_HYPERVISOR:            return 0x40000000;
    default:
        *err = 1;
        return 0;
    }
}

static status_t
reply_continue(void *dom, struct kvmi_dom_event *ev)
{
    struct kvmi_event_reply rpl = {0};

    rpl.action = KVMI_EVENT_ACTION_CONTINUE;
    rpl.event = ev->event.common.event;

    if (kvmi_reply_event(dom, ev->seq, &rpl, sizeof(rpl)))
        return VMI_FAILURE;

    return VMI_SUCCESS;
}

static void
fill_ev_common_kvmi_to_libvmi(
        struct kvmi_dom_event *kvmi_event,
        vmi_event_t *libvmi_event)
{
    //      standard regs
    libvmi_event->x86_regs->rax = kvmi_event->event.common.regs.rax;
    libvmi_event->x86_regs->rbx = kvmi_event->event.common.regs.rbx;
    libvmi_event->x86_regs->rcx = kvmi_event->event.common.regs.rcx;
    libvmi_event->x86_regs->rdx = kvmi_event->event.common.regs.rdx;
    libvmi_event->x86_regs->rsi = kvmi_event->event.common.regs.rsi;
    libvmi_event->x86_regs->rdi = kvmi_event->event.common.regs.rdi;
    libvmi_event->x86_regs->rip = kvmi_event->event.common.regs.rip;
    libvmi_event->x86_regs->rsp = kvmi_event->event.common.regs.rsp;
    libvmi_event->x86_regs->rbp = kvmi_event->event.common.regs.rbp;
    libvmi_event->x86_regs->rflags = kvmi_event->event.common.regs.rflags;
    libvmi_event->x86_regs->r8 = kvmi_event->event.common.regs.r8;
    libvmi_event->x86_regs->r9 = kvmi_event->event.common.regs.r9;
    libvmi_event->x86_regs->r10 = kvmi_event->event.common.regs.r10;
    libvmi_event->x86_regs->r11 = kvmi_event->event.common.regs.r11;
    libvmi_event->x86_regs->r12 = kvmi_event->event.common.regs.r12;
    libvmi_event->x86_regs->r13 = kvmi_event->event.common.regs.r13;
    libvmi_event->x86_regs->r14 = kvmi_event->event.common.regs.r14;
    libvmi_event->x86_regs->r15 = kvmi_event->event.common.regs.r15;
    //      special regs
    //          Control Registers
    libvmi_event->x86_regs->cr0 = kvmi_event->event.common.sregs.cr0;
    libvmi_event->x86_regs->cr2 = kvmi_event->event.common.sregs.cr2;
    libvmi_event->x86_regs->cr3 = kvmi_event->event.common.sregs.cr3;
    libvmi_event->x86_regs->cr4 = kvmi_event->event.common.sregs.cr4;
    //          CS
    libvmi_event->x86_regs->cs_base = kvmi_event->event.common.sregs.cs.base;
    libvmi_event->x86_regs->cs_limit = kvmi_event->event.common.sregs.cs.limit;
    libvmi_event->x86_regs->cs_sel = kvmi_event->event.common.sregs.cs.selector;
    //          DS
    libvmi_event->x86_regs->ds_base = kvmi_event->event.common.sregs.ds.base;
    libvmi_event->x86_regs->ds_limit = kvmi_event->event.common.sregs.ds.limit;
    libvmi_event->x86_regs->ds_sel = kvmi_event->event.common.sregs.ds.selector;
    //          SS
    libvmi_event->x86_regs->ss_base = kvmi_event->event.common.sregs.ss.base;
    libvmi_event->x86_regs->ss_limit = kvmi_event->event.common.sregs.ss.limit;
    libvmi_event->x86_regs->ss_sel = kvmi_event->event.common.sregs.ss.selector;
    //          ES
    libvmi_event->x86_regs->es_base = kvmi_event->event.common.sregs.es.base;
    libvmi_event->x86_regs->es_limit = kvmi_event->event.common.sregs.es.limit;
    libvmi_event->x86_regs->es_sel = kvmi_event->event.common.sregs.es.selector;
    //          FS
    libvmi_event->x86_regs->fs_base = kvmi_event->event.common.sregs.fs.base;
    libvmi_event->x86_regs->fs_limit = kvmi_event->event.common.sregs.fs.limit;
    libvmi_event->x86_regs->fs_sel = kvmi_event->event.common.sregs.fs.selector;
    //          GS
    libvmi_event->x86_regs->gs_base = kvmi_event->event.common.sregs.gs.base;
    libvmi_event->x86_regs->gs_limit = kvmi_event->event.common.sregs.gs.limit;
    libvmi_event->x86_regs->gs_sel = kvmi_event->event.common.sregs.gs.selector;
    //      VCPU
    libvmi_event->vcpu_id = kvmi_event->event.common.vcpu;
}

static void
call_event_callback(
        vmi_instance_t vmi,
        vmi_event_t *event)
{
    vmi->event_callback = 1;
    // TODO: process callback event_response
    event->callback(vmi, event);
    vmi->event_callback = 0;
}

/*
 * VM event handlers (process_xxx)
 * called from kvm_events_listen
 */
static status_t
process_register(vmi_instance_t vmi, struct kvmi_dom_event *event)
{
#ifdef ENABLE_SAFETY_CHECKS
    if (!vmi || !event)
        return VMI_FAILURE;
#endif
    dbprint(VMI_DEBUG_KVM, "--Received register event\n");
    return VMI_SUCCESS;
}

static status_t
process_interrupt(vmi_instance_t vmi, struct kvmi_dom_event *kvmi_event)
{
#ifdef ENABLE_SAFETY_CHECKS
    if (!vmi || !kvmi_event)
        return VMI_FAILURE;
#endif
    dbprint(VMI_DEBUG_KVM, "--Received interrupt event\n");

    // lookup vmi_event
    gint key = INT3;
    vmi_event_t *libvmi_event = g_hash_table_lookup(vmi->interrupt_events, &key);
#ifdef ENABLE_SAFETY_CHECKS
    if ( !libvmi_event ) {
        errprint("%s error: no interrupt event handler is registered in LibVMI\n", __func__);
        return VMI_FAILURE;
    }
#endif

    // fill libvmi_event struct
    x86_registers_t regs = {0};
    libvmi_event->x86_regs = &regs;
    fill_ev_common_kvmi_to_libvmi(kvmi_event, libvmi_event);

    //      interrupt_event
    // TODO: hardcoded PAGE_SHIFT
    libvmi_event->interrupt_event.gfn = kvmi_event->event.breakpoint.gpa >> 12;
    // TODO: vector and type
    // event->interrupt_event.vector =
    // event->interrupt_event.type =
    libvmi_event->interrupt_event.cr2 = kvmi_event->event.common.sregs.cr2;
    libvmi_event->interrupt_event.offset = kvmi_event->event.common.regs.rip  & VMI_BIT_MASK(0,11);
    libvmi_event->interrupt_event.gla = kvmi_event->event.common.regs.rip;
    // default reinject behavior: invalid
    libvmi_event->interrupt_event.reinject = -1;

    // call user callback
    call_event_callback(vmi, libvmi_event);

    return VMI_SUCCESS;
}

static status_t
process_pagefault(vmi_instance_t vmi, struct kvmi_dom_event *kvmi_event)
{
#ifdef ENABLE_SAFETY_CHECKS
    if (!vmi || !kvmi_event)
        return VMI_FAILURE;
#endif
    dbprint(VMI_DEBUG_KVM, "--Received pagefault event\n");

    // build out_access
    vmi_mem_access_t out_access = VMI_MEMACCESS_INVALID;
    if (kvmi_event->event.page_fault.mode & KVMI_PAGE_ACCESS_R) out_access |= VMI_MEMACCESS_R;
    if (kvmi_event->event.page_fault.mode & KVMI_PAGE_ACCESS_W) out_access |= VMI_MEMACCESS_W;
    if (kvmi_event->event.page_fault.mode & KVMI_PAGE_ACCESS_X) out_access |= VMI_MEMACCESS_X;

    vmi_event_t *libvmi_event;
    addr_t gfn = kvmi_event->event.page_fault.gpa >> 12;
    // lookup vmi_event
    //      standard ?
    if ( g_hash_table_size(vmi->mem_events_on_gfn) ) {
        // TODO: hardcoded page shift

        libvmi_event = g_hash_table_lookup(vmi->mem_events_on_gfn, &gfn);
        if (libvmi_event && (libvmi_event->mem_event.in_access & out_access)) {
            // fill libvmi_event struct
            x86_registers_t regs = {0};
            libvmi_event->x86_regs = &regs;
            fill_ev_common_kvmi_to_libvmi(kvmi_event, libvmi_event);
            //      mem_event
            libvmi_event->mem_event.out_access = out_access;
            libvmi_event->mem_event.gla = kvmi_event->event.page_fault.gva;
            libvmi_event->mem_event.offset = kvmi_event->event.page_fault.gpa & VMI_BIT_MASK(0, 11);

            // call user callback
            call_event_callback(vmi, libvmi_event);

            return VMI_SUCCESS;
        }
    }
    //  generic ?
    if ( g_hash_table_size(vmi->mem_events_generic) ) {
        GHashTableIter i;
        vmi_mem_access_t *key = NULL;
        bool cb_issued = 0;

        ghashtable_foreach(vmi->mem_events_generic, i, &key, &libvmi_event) {
            if ( (*key) & out_access ) {
                // fill libvmi_event struct
                x86_registers_t regs = {0};
                libvmi_event->x86_regs = &regs;
                fill_ev_common_kvmi_to_libvmi(kvmi_event, libvmi_event);
                //      mem_event
                libvmi_event->mem_event.out_access = out_access;
                libvmi_event->mem_event.gla = kvmi_event->event.page_fault.gva;
                libvmi_event->mem_event.offset = kvmi_event->event.page_fault.gpa & VMI_BIT_MASK(0, 11);

                // call user callback
                call_event_callback(vmi, libvmi_event);

                cb_issued = 1;
            }
        }
        if ( cb_issued )
            return VMI_SUCCESS;
    }

    errprint("Caught a memory event that had no handler registered in LibVMI @ GFN 0x%" PRIx64 " (0x%" PRIx64 "), access: %u\n",
             gfn, kvmi_event->event.page_fault.gpa, out_access);
    return VMI_FAILURE;
}

static status_t
process_pause_event(vmi_instance_t vmi, struct kvmi_dom_event *kvmi_event)
{
#ifdef ENABLE_SAFETY_CHECKS
    if (!vmi || !kvmi_event)
        return VMI_FAILURE;
#endif

    // this shouldn't happen
    // the pause event should have been poped by kvm_resume_vm
    // report to the user
    errprint("Unexpected PAUSE event while listening. Did you forget to resume the VM ?\n");

    // always fail, so kvm_events_listen can fail too
    return VMI_FAILURE;
}

void *
kvm_get_memory_kvmi(vmi_instance_t vmi, addr_t paddr, uint32_t length) {
    kvm_instance_t *kvm = kvm_get_instance(vmi);
    void *buffer;

    if (!kvm->kvmi_dom)
        return NULL;

    buffer = g_malloc0(length);
    if (kvmi_read_physical(kvm->kvmi_dom, paddr, buffer, length) < 0) {
        g_free(buffer);
        return NULL;
    }

    return buffer;
}


void
kvm_release_memory(
    vmi_instance_t UNUSED(vmi),
    void *memory,
    size_t UNUSED(length))
{
    if (memory)
        free(memory);
}

status_t
kvm_put_memory(vmi_instance_t vmi,
                    addr_t paddr,
                    uint32_t length,
                    void *buf)
{
    kvm_instance_t *kvm = kvm_get_instance(vmi);

    if (!kvm->kvmi_dom)
        return VMI_FAILURE;

    if (kvmi_write_physical(kvm->kvmi_dom, paddr, buf, length) < 0)
        return VMI_FAILURE;

    return VMI_SUCCESS;
}

/**
 * Setup KVM live (i.e. KVM patch or KVM native) mode.
 * If KVM patch has been setup before, resume it.
 * If KVM patch hasn't been setup but is available, setup
 * KVM patch, otherwise setup KVM native.
 */
status_t
kvm_setup_live_mode(
    vmi_instance_t vmi)
{
    memory_cache_destroy(vmi);
    memory_cache_init(vmi, kvm_get_memory_kvmi, kvm_release_memory, 1);
    return VMI_SUCCESS;
}

//----------------------------------------------------------------------------
// KVMI-Specific Interface Functions (no direction mapping to driver_*)

static int
cb_kvmi_connect(
    void *dom,
    unsigned char (*uuid)[16],
    void *ctx)
{
    kvm_instance_t *kvm = ctx; 

    pthread_mutex_lock(&kvm->kvm_connect_mutex);
    /*
     * If kvmi_dom is not NULL it means this is a reconnection.
     * The previous connection was closed somehow.
     */
    kvmi_domain_close(kvm->kvmi_dom, true);
    kvm->kvmi_dom = dom;
    pthread_cond_signal(&kvm->kvm_start_cond);
    pthread_mutex_unlock(&kvm->kvm_connect_mutex);

    return 0;
}

static bool
init_kvmi(
    kvm_instance_t *kvm,
    const char *sock_path)
{
    int err = -1;

    pthread_mutex_init(&kvm->kvm_connect_mutex, NULL);
    pthread_cond_init(&kvm->kvm_start_cond, NULL);
    kvm->kvmi_dom = NULL;

    pthread_mutex_lock(&kvm->kvm_connect_mutex);
    kvm->kvmi = kvmi_init_unix_socket(sock_path, cb_kvmi_connect, NULL, kvm);
    if (kvm->kvmi) {
        struct timeval now;
        if (gettimeofday(&now, NULL) == 0) {
           struct timespec t = {};
           t.tv_sec = now.tv_sec + 10;
           err = pthread_cond_timedwait(&kvm->kvm_start_cond, &kvm->kvm_connect_mutex, &t);
        }
    }
    pthread_mutex_unlock(&kvm->kvm_connect_mutex);

    if (err) {
        /*
         * The libkvmi may accept the connection after timeout
         * and our callback can set kvm->kvmi_dom. So, we must
         * stop the accepting thread first.
         */
        kvmi_uninit(kvm->kvmi);
        kvm->kvmi = NULL;
        /* From this point, kvm->kvmi_dom won't be touched. */
        kvmi_domain_close(kvm->kvmi_dom, true);
        return false;
    }

    return true;
}

static bool
get_kvmi_registers(
    kvm_instance_t *kvm,
    reg_t reg,
    uint64_t *value)
{
    struct kvm_regs regs;
    struct kvm_sregs sregs;
    struct {
        struct kvm_msrs      msrs;
        struct kvm_msr_entry entries[2];
    } msrs = {0};
    unsigned int mode;
    unsigned short vcpu = 0;
    int err;

    if (!kvm->kvmi_dom)
        return false;

    msrs.msrs.nmsrs = sizeof(msrs.entries)/sizeof(msrs.entries[0]);
    msrs.entries[0].index = translate_msr_index(MSR_EFER, &err);
    msrs.entries[1].index = translate_msr_index(MSR_STAR, &err);

    err = kvmi_get_registers(kvm->kvmi_dom, vcpu, &regs, &sregs, &msrs.msrs, &mode);

    if (err != 0)
        return false;

    /* mode should be 8 if VMI_PM_IA32E == vmi->page_mode */

    switch (reg) {
    case RAX:
        *value = regs.rax;
        break;
    case RBX:
        *value = regs.rbx;
        break;
    case RCX:
        *value = regs.rcx;
        break;
    case RDX:
        *value = regs.rdx;
        break;
    case RBP:
        *value = regs.rbp;
        break;
    case RSI:
        *value = regs.rsi;
        break;
    case RDI:
        *value = regs.rdi;
        break;
    case RSP:
        *value = regs.rsp;
        break;
    case R8:
        *value = regs.r8;
        break;
    case R9:
        *value = regs.r9;
        break;
    case R10:
        *value = regs.r10;
        break;
    case R11:
        *value = regs.r11;
        break;
    case R12:
        *value = regs.r12;
        break;
    case R13:
        *value = regs.r13;
        break;
    case R14:
        *value = regs.r14;
        break;
    case R15:
        *value = regs.r15;
        break;
    case RIP:
        *value = regs.rip;
        break;
    case RFLAGS:
        *value = regs.rflags;
        break;
    case CR0:
        *value = sregs.cr0;
        break;
    case CR2:
        *value = sregs.cr2;
        break;
    case CR3:
        *value = sregs.cr3;
        break;
    case CR4:
        *value = sregs.cr4;
        break;
    case FS_BASE:
        *value = sregs.fs.base;
        break;
    case GS_BASE:
        *value = sregs.gs.base;
        break;
    case MSR_EFER:
        *value = msrs.entries[0].data;
        break;
    case MSR_STAR:
        *value = msrs.entries[1].data;
        break;
    default:
        dbprint(VMI_DEBUG_KVM, "--Reading register %"PRIu64" not implemented\n", reg);
        return false;
    }

    return true;
}

//----------------------------------------------------------------------------
// General Interface Functions (1-1 mapping to driver_* function)

status_t
kvm_init(
    vmi_instance_t vmi,
    uint32_t UNUSED(init_flags),
    vmi_init_data_t* UNUSED(init_data))
{
    kvm_instance_t *kvm = g_malloc0(sizeof(kvm_instance_t));
    if ( VMI_FAILURE == create_libvirt_wrapper(kvm) )
        return VMI_FAILURE;

    virConnectPtr conn = kvm->libvirt.virConnectOpenAuth("qemu:///system", kvm->libvirt.virConnectAuthPtrDefault, 0);
    if (NULL == conn) {
        dbprint(VMI_DEBUG_KVM, "--no connection to kvm hypervisor\n");
        free(kvm);
        return VMI_FAILURE;
    }

    kvm->conn = conn;

    vmi->driver.driver_data = (void*)kvm;

    return VMI_SUCCESS;
}

status_t
kvm_init_vmi(
    vmi_instance_t vmi,
    uint32_t init_flags,
    vmi_init_data_t* init_data)
{
    vmi_init_data_entry_t init_entry;
    char *socket_path = NULL;

    // a socket path is required to init kvmi
    if (!init_data) {
        dbprint(VMI_DEBUG_KVM, "--kvmi need a socket path to be specified\n");
        return VMI_FAILURE;
    }
    // check we have at least on entry
    if (init_data->count < 1) {
        dbprint(VMI_DEBUG_KVM, "--empty init data\n");
        return VMI_FAILURE;
    }
    init_entry = init_data->entry[0];
    // check init_data type
    if (init_entry.type != VMI_INIT_DATA_KVMI_SOCKET) {
        dbprint(VMI_DEBUG_KVM, "--wrong init data type\n");
        return VMI_FAILURE;
    }
    socket_path = (char*) init_entry.data;

    kvm_instance_t *kvm = kvm_get_instance(vmi);
    virDomainPtr dom = kvm->libvirt.virDomainLookupByID(kvm->conn, kvm->id);
    if (NULL == dom) {
        dbprint(VMI_DEBUG_KVM, "--failed to find kvm domain\n");
        return VMI_FAILURE;
    }

    // get the libvirt version
    unsigned long libVer = 0;

    if (kvm->libvirt.virConnectGetLibVersion(kvm->conn, &libVer) != 0) {
        dbprint(VMI_DEBUG_KVM, "--failed to get libvirt version\n");
        return VMI_FAILURE;
    }
    dbprint(VMI_DEBUG_KVM, "--libvirt version %lu\n", libVer);

    kvm->dom = dom;
    vmi->vm_type = NORMAL;

    dbprint(VMI_DEBUG_KVM, "--Connecting to KVMI...\n");
    if (!init_kvmi(kvm,  socket_path)) {
        dbprint(VMI_DEBUG_KVM, "--KVMI failed\n");
        return VMI_FAILURE;
    }
    dbprint(VMI_DEBUG_KVM, "--KVMI connected\n");

    // get VCPU count
    if (kvmi_get_vcpu_count(kvm->kvmi_dom, &vmi->num_vcpus)) {
        dbprint(VMI_DEBUG_KVM, "--Fail to get VCPU count: %s\n", strerror(errno));
        return VMI_FAILURE;
    }
    dbprint(VMI_DEBUG_KVM, "--VCPU count: %d\n", vmi->num_vcpus);

    // check init_flags
    if (init_flags & VMI_INIT_EVENTS) {
        // fill event dispatcher
        kvm->process_event[KVMI_EVENT_CR] = &process_register;
        kvm->process_event[KVMI_EVENT_BREAKPOINT] = &process_interrupt;
        kvm->process_event[KVMI_EVENT_PF] = &process_pagefault;
        kvm->process_event[KVMI_EVENT_PAUSE_VCPU] = &process_pause_event;
    }

    return kvm_setup_live_mode(vmi);
}

void
kvm_destroy(
    vmi_instance_t vmi)
{
    kvm_instance_t *kvm = kvm_get_instance(vmi);

    kvmi_uninit(kvm->kvmi); /* closes the accepting thread */
    kvm->kvmi = NULL;
    kvmi_domain_close(kvm->kvmi_dom, true);
    kvm->kvmi_dom = NULL;

    if (kvm->dom) {
        kvm->libvirt.virDomainFree(kvm->dom);
    }

    if (kvm->conn) {
        kvm->libvirt.virConnectClose(kvm->conn);
    }

    dlclose(kvm->libvirt.handle);
}

uint64_t
kvm_get_id_from_name(
    vmi_instance_t vmi,
    const char *name)
{
    virDomainPtr dom = NULL;
    uint64_t domainid = VMI_INVALID_DOMID;
    kvm_instance_t *kvm = kvm_get_instance(vmi);

    dom = kvm->libvirt.virDomainLookupByName(kvm->conn, name);
    if (NULL == dom) {
        dbprint(VMI_DEBUG_KVM, "--failed to find kvm domain\n");
        domainid = VMI_INVALID_DOMID;
    } else {

        domainid = (uint64_t) kvm->libvirt.virDomainGetID(dom);
        if (domainid == (uint64_t)-1) {
            dbprint(VMI_DEBUG_KVM, "--requested kvm domain may not be running\n");
            domainid = VMI_INVALID_DOMID;
        }
    }

    if (dom)
        kvm->libvirt.virDomainFree(dom);

    return domainid;
}

status_t
kvm_get_name_from_id(
    vmi_instance_t vmi,
    uint64_t domainid,
    char **name)
{
    virDomainPtr dom = NULL;
    const char* temp_name = NULL;
    kvm_instance_t *kvm = kvm_get_instance(vmi);

    dom = kvm->libvirt.virDomainLookupByID(kvm->conn, domainid);
    if (NULL == dom) {
        dbprint(VMI_DEBUG_KVM, "--failed to find kvm domain\n");
        return VMI_FAILURE;
    }

    temp_name = kvm->libvirt.virDomainGetName(dom);
    *name = temp_name ? strdup(temp_name) : NULL;

    if (dom)
        kvm->libvirt.virDomainFree(dom);

    if (*name) {
        return VMI_SUCCESS;
    }

    return VMI_FAILURE;
}

uint64_t
kvm_get_id(
    vmi_instance_t vmi)
{
    return kvm_get_instance(vmi)->id;
}

void
kvm_set_id(
    vmi_instance_t vmi,
    uint64_t domainid)
{
    kvm_get_instance(vmi)->id = domainid;
}

status_t
kvm_check_id(
    vmi_instance_t vmi,
    uint64_t domainid)
{
    virDomainPtr dom = NULL;
    kvm_instance_t *kvm = kvm_get_instance(vmi);

    dom = kvm->libvirt.virDomainLookupByID(kvm->conn, domainid);
    if (NULL == dom) {
        dbprint(VMI_DEBUG_KVM, "--failed to find kvm domain\n");
        return VMI_FAILURE;
    }

    if (dom)
        kvm->libvirt.virDomainFree(dom);

    return VMI_SUCCESS;
}

status_t
kvm_get_name(
    vmi_instance_t vmi,
    char **name)
{
    kvm_instance_t *kvm = kvm_get_instance(vmi);

    const char *tmpname = kvm->libvirt.virDomainGetName(kvm->dom);

    // don't need to deallocate the name, it will go away with the domain object

    if (NULL != tmpname) {
        *name = strdup(tmpname);
        return VMI_SUCCESS;
    } else {
        return VMI_FAILURE;
    }
}

void
kvm_set_name(
    vmi_instance_t vmi,
    const char *name)
{
    kvm_get_instance(vmi)->name = strndup(name, 500);
}

status_t
kvm_get_memsize(
    vmi_instance_t vmi,
    uint64_t *allocated_ram_size,
    addr_t *maximum_physical_address)
{
#ifdef ENABLE_SAFETY_CHECKS
    if (!allocated_ram_size || !maximum_physical_address)
        return VMI_FAILURE;
#endif
    virDomainInfo info = {0};
    kvm_instance_t *kvm = kvm_get_instance(vmi);

    if (-1 == kvm->libvirt.virDomainGetInfo(kvm->dom, &info))
    {
        dbprint(VMI_DEBUG_KVM, "--failed to get vm info\n");
        return VMI_FAILURE;
    }

    // maxMem is in KB
    *allocated_ram_size = info.maxMem * 1024;
    *maximum_physical_address = info.maxMem * 1024;
    return VMI_SUCCESS;
}

status_t kvm_request_page_fault (
    vmi_instance_t vmi,
    unsigned long vcpu,
    uint64_t virtual_address,
    uint32_t error_code)
{
#ifdef ENABLE_SAFETY_CHECKS
    if (!vmi) {
        errprint("Invalid vmi handle\n");
        return VMI_FAILURE;
    }
#endif
    kvm_instance_t *kvm = kvm_get_instance(vmi);
#ifdef ENABLE_SAFETY_CHECKS
    if (!kvm || !kvm->kvmi_dom) {
        errprint("Invalid kvm/kvmi handles\n");
        return VMI_FAILURE;
    }
#endif
    if (kvmi_inject_page_fault(kvm->kvmi_dom, vcpu, virtual_address, error_code))
        return VMI_FAILURE;

    dbprint(VMI_DEBUG_KVM, "--Page fault injected at 0x%"PRIx64"\n", virtual_address);
    return VMI_SUCCESS;
}

status_t
kvm_get_vcpuregs(
    vmi_instance_t vmi,
    registers_t *registers,
    unsigned long vcpu)
{
    struct kvm_regs regs;
    struct kvm_sregs sregs;
    struct {
        struct kvm_msrs msrs;
        struct kvm_msr_entry entries[6];
    } msrs = { 0 };
    int err;
    unsigned int mode;
    x86_registers_t *x86 = &registers->x86;
    kvm_instance_t *kvm = kvm_get_instance(vmi);

    msrs.msrs.nmsrs = sizeof(msrs.entries)/sizeof(msrs.entries[0]);
    msrs.entries[0].index = translate_msr_index(MSR_IA32_SYSENTER_CS, &err);
    msrs.entries[1].index = translate_msr_index(MSR_IA32_SYSENTER_ESP, &err);
    msrs.entries[2].index = translate_msr_index(MSR_IA32_SYSENTER_EIP, &err);
    msrs.entries[3].index = translate_msr_index(MSR_EFER, &err);
    msrs.entries[4].index = translate_msr_index(MSR_STAR, &err);
    msrs.entries[5].index = translate_msr_index(MSR_LSTAR, &err);

    if (!kvm->kvmi_dom)
        return VMI_FAILURE;

    err = kvmi_get_registers(kvm->kvmi_dom, vcpu, &regs, &sregs, &msrs.msrs, &mode);

    if (err != 0)
        return VMI_FAILURE;

    x86->rax = regs.rax;
    x86->rcx = regs.rcx;
    x86->rdx = regs.rdx;
    x86->rbx = regs.rbx;
    x86->rsp = regs.rsp;
    x86->rbp = regs.rbp;
    x86->rsi = regs.rsi;
    x86->rdi = regs.rdi;
    x86->r8 = regs.r8;
    x86->r9 = regs.r9;
    x86->r10 = regs.r10;
    x86->r11 = regs.r11;
    x86->r12 = regs.r12;
    x86->r13 = regs.r13;
    x86->r14 = regs.r14;
    x86->r15 = regs.r15;
    x86->rflags = regs.rflags;
    x86->dr7 = 0; // FIXME: where do I get this
    x86->rip = regs.rip;
    x86->cr0 = sregs.cr0;
    x86->cr2 = sregs.cr2;
    x86->cr3 = sregs.cr3;
    x86->cr4 = sregs.cr4;
    // Are these correct
    x86->sysenter_cs = msrs.entries[0].data;
    x86->sysenter_esp = msrs.entries[1].data;
    x86->sysenter_eip = msrs.entries[2].data;
    x86->msr_efer = msrs.entries[3].data;
    x86->msr_star = msrs.entries[4].data;
    x86->msr_lstar = msrs.entries[5].data;
    x86->fs_base = 0; // FIXME: Where do I get these
    x86->gs_base = 0;
    x86->cs_arbytes = 0;
    x86->_pad = 0;

    return VMI_SUCCESS;
}

status_t
kvm_set_vcpureg(vmi_instance_t vmi,
                uint64_t value,
                reg_t reg,
                unsigned long vcpu) {
    kvm_instance_t *kvm = kvm_get_instance(vmi);
    if (!kvm->kvmi_dom)
        return VMI_FAILURE;
    unsigned int mode = 0;
    struct kvm_regs regs = {0};
    struct kvm_sregs sregs = {0};
    struct {
        struct kvm_msrs msrs;
        struct kvm_msr_entry entries[0];
    } msrs = {0};
    msrs.msrs.nmsrs = 0;

    if (kvmi_get_registers(kvm->kvmi_dom, vcpu, &regs, &sregs, &msrs.msrs, &mode) < 0) {
        return VMI_FAILURE;
    }

    // This could use a macro or something
    switch (reg) {
    case RAX:
        regs.rax = value;
        break;
    case RBX:
        regs.rbx = value;
        break;
    case RCX:
        regs.rcx = value;
        break;
    case RDX:
        regs.rdx = value;
        break;
    case RSI:
        regs.rsi = value;
        break;
    case RDI:
        regs.rdi = value;
        break;
    case RSP:
        regs.rsp = value;
        break;
    case RBP:
        regs.rbp = value;
        break;
    case R8:
        regs.r8 = value;
        break;
    case R9:
        regs.r9 = value;
        break;
    case R10:
        regs.r10 = value;
        break;
    case R11:
        regs.r11 = value;
        break;
    case R12:
        regs.r12 = value;
        break;
    case R13:
        regs.r13 = value;
        break;
    case R14:
        regs.r14 = value;
        break;
    case R15:
        regs.r15 = value;
        break;
    case RIP:
        regs.rip = value;
        break;
    case RFLAGS:
        regs.rflags = value;
        break;
    default:
        return VMI_FAILURE;
    }

    if (kvmi_set_registers(kvm->kvmi_dom, vcpu, &regs) < 0) {
        return VMI_FAILURE;
    }

    return VMI_SUCCESS;
}

status_t kvm_set_vcpuregs(vmi_instance_t vmi,
                          registers_t *registers,
                          unsigned long vcpu) {
    kvm_instance_t *kvm = kvm_get_instance(vmi);
    if (!kvm->kvmi_dom)
        return VMI_FAILURE;
    struct x86_regs *x86 = &registers->x86;
    struct kvm_regs regs = {
        .rax = x86->rax,
        .rbx = x86->rbx,
        .rcx = x86->rcx,
        .rdx = x86->rdx,
        .rsi = x86->rsi,
        .rdi = x86->rdi,
        .rsp = x86->rsp,
        .rbp = x86->rbp,
        .r8  = x86->r8,
        .r9  = x86->r9,
        .r10 = x86->r10,
        .r11 = x86->r11,
        .r12 = x86->r12,
        .r13 = x86->r13,
        .r14 = x86->r14,
        .r15 = x86->r15,
        .rip = x86->rip,
        .rflags = x86->rflags
    };
    if (kvmi_set_registers(kvm->kvmi_dom, vcpu, &regs) < 0) {
        return VMI_FAILURE;
    }
    return VMI_SUCCESS;
}

status_t
kvm_get_vcpureg(
    vmi_instance_t vmi,
    uint64_t *value,
    reg_t reg,
    unsigned long UNUSED(vcpu))
{
    if (get_kvmi_registers(kvm_get_instance(vmi), reg, value))
        return VMI_SUCCESS;

    return VMI_FAILURE;
}

void *
kvm_read_page(
    vmi_instance_t vmi,
    addr_t page)
{
    addr_t paddr = page << vmi->page_shift;

    return memory_cache_insert(vmi, paddr);
}

status_t
kvm_write(
    vmi_instance_t vmi,
    addr_t paddr,
    void *buf,
    uint32_t length)
{
    return kvm_put_memory(vmi, paddr, length, buf);
}

int
kvm_is_pv(
    vmi_instance_t UNUSED(vmi))
{
    return 0;
}

status_t
kvm_test(
    uint64_t domainid,
    const char *name,
    uint64_t UNUSED(init_flags),
    vmi_init_data_t* UNUSED(init_data))
{
    struct vmi_instance _vmi = {0};
    vmi_instance_t vmi = &_vmi;

    if ( VMI_FAILURE == kvm_init(vmi, 0, NULL) )
        return VMI_FAILURE;

    if (name) {
        domainid = kvm_get_id_from_name(vmi, name);
        if (domainid != VMI_INVALID_DOMID)
            return VMI_SUCCESS;
    }

    if (domainid != VMI_INVALID_DOMID) {
        char *_name = NULL;
        status_t rc = kvm_get_name_from_id(vmi, domainid, &_name);
        free(_name);

        if ( VMI_SUCCESS == rc )
            return rc;
    }

    kvm_destroy(vmi);
    return VMI_FAILURE;
}

status_t
kvm_pause_vm(
    vmi_instance_t vmi)
{
    kvm_instance_t *kvm = kvm_get_instance(vmi);
    // already paused ?
    if (kvm->expected_pause_count)
        return VMI_SUCCESS;

    // pause vcpus
    if (kvmi_pause_all_vcpus(kvm->kvmi_dom, &kvm->expected_pause_count)) {
        errprint("%s: Failed to pause domain\n", __func__);
        return VMI_FAILURE;
    }

    dbprint(VMI_DEBUG_KVM, "--We should received %u pause events\n", kvm->expected_pause_count);

    return VMI_SUCCESS;
}

status_t
kvm_resume_vm(
    vmi_instance_t vmi)
{
    kvm_instance_t *kvm = kvm_get_instance(vmi);
    // already resumed ?
    if (!kvm->expected_pause_count)
        return VMI_SUCCESS;

    // wait to receive pause events
    while (kvm->expected_pause_count) {
        struct kvmi_dom_event *ev = NULL;
        unsigned int ev_id = 0;

        // wait
        if (kvmi_wait_event(kvm->kvmi_dom, 1000)) {
            errprint("%s: Failed to receive event\n", __func__);
            return VMI_FAILURE;
        }
        // pop
        if (kvmi_pop_event(kvm->kvmi_dom, &ev)) {
            errprint("%s: Failed to pop event\n", __func__);
            return VMI_FAILURE;
        }
        // handle event
        ev_id = ev->event.common.event;
        switch (ev_id) {
            case KVMI_EVENT_PAUSE_VCPU:
                dbprint(VMI_DEBUG_KVM, "--Received VCPU pause event\n");
                kvm->expected_pause_count--;
                if (reply_continue(kvm->kvmi_dom, ev) == VMI_FAILURE) {
                    errprint("%s: Fail to send continue reply", __func__);
                    free(ev);
                    return VMI_FAILURE;
                }
                free(ev);
                break;
            default:
                errprint("%s: Unexpected event %u\n", __func__, ev_id);
                free(ev);
                return VMI_FAILURE;
        }
    }

    return VMI_SUCCESS;
}


status_t kvm_events_listen(
    vmi_instance_t vmi,
    uint32_t timeout)
{
#ifdef ENABLE_SAFETY_CHECKS
    if (!vmi)
        return VMI_FAILURE;
#endif
    struct kvmi_dom_event *event = NULL;
    unsigned int ev_reason = 0;

    kvm_instance_t *kvm = kvm_get_instance(vmi);
#ifdef ENABLE_SAFETY_CHECKS
    if (!kvm || !kvm->kvmi_dom)
        return VMI_FAILURE;
#endif

    // wait next event
    if (kvmi_wait_event(kvm->kvmi_dom, timeout)) {
        if (errno == ETIMEDOUT) {
            // no events !
            return VMI_SUCCESS;
        }
        return VMI_FAILURE;
    }

    // pop event from queue
    if (kvmi_pop_event(kvm->kvmi_dom, &event)) {
        dbprint(VMI_DEBUG_KVM, "--Failed to pop event\n");
        return VMI_FAILURE;
    }

    // handle event
    ev_reason = event->event.common.event;
#ifdef ENABLE_SAFETY_CHECKS
    if ( ev_reason >= KVMI_NUM_EVENTS || !kvm->process_event[ev_reason] ) {
        errprint("Undefined handler for %u event reason", ev_reason);
        return VMI_FAILURE;
    }
#endif
    // call handler
    if (VMI_FAILURE == kvm->process_event[ev_reason](vmi, event))
        goto error_exit;


    // ack
    if (reply_continue(kvm->kvmi_dom, event) == VMI_FAILURE)
        goto error_exit;

    return VMI_SUCCESS;
error_exit:
    if (event)
        free(event);
    return VMI_FAILURE;
}


status_t kvm_set_reg_access(
    vmi_instance_t vmi,
    reg_event_t* event)
{
#ifdef ENABLE_SAFETY_CHECKS
    if (!vmi || !event)
        return VMI_FAILURE;
#endif
    unsigned int event_flags = 0;
    unsigned int kvmi_reg = 0;
    bool enable = false;

    kvm_instance_t *kvm = kvm_get_instance(vmi);
#ifdef ENABLE_SAFETY_CHECKS
    if (!kvm || !kvm->kvmi_dom)
        return VMI_FAILURE;
#endif

    // check reg
    switch (event->reg) {
        case CR0:
            event_flags |= KVMI_EVENT_CR_FLAG;
            kvmi_reg = 0;
            break;
        case CR3:
            event_flags |= KVMI_EVENT_CR_FLAG;
            kvmi_reg = 3;
            break;
        case CR4:
            event_flags |= KVMI_EVENT_CR_FLAG;
            kvmi_reg = 4;
            break;
        case MSR_STAR:
            event_flags |= KVMI_EVENT_MSR_FLAG;
            kvmi_reg = 0xc0000081;
            break;
        default:
            errprint("%s: unhandled register %" PRIu64"\n", __func__, event->reg);
            return VMI_FAILURE;
    }

    // check access type
    switch (event->in_access) {
        case VMI_REGACCESS_N:
            enable = false;
            break;
        case VMI_REGACCESS_W:
            enable = true;
            break;
        case VMI_REGACCESS_R:
        case VMI_REGACCESS_RW:
            errprint("Register read events are unavailable in KVM.\n");
            return VMI_FAILURE;
        default:
            errprint("Unknown register access mode: %d\n", event->in_access);
            return VMI_FAILURE;
    }

    // enable event monitoring for all vcpus
    for (unsigned int i = 0; i < vmi->num_vcpus; i++) {
        if (kvmi_control_events(kvm->kvmi_dom, i, event_flags)) {
            errprint("%s: kvmi_control_events failed\n", __func__);
            goto error_exit;
        }

        if (event_flags & KVMI_EVENT_CR_FLAG)
            if (kvmi_control_cr(kvm->kvmi_dom, i, kvmi_reg, enable)) {
                errprint("%s: kvmi_control_cr failed\n", __func__);
                goto error_exit;
            }
        if (event_flags & KVMI_EVENT_MSR_FLAG)
            if (kvmi_control_msr(kvm->kvmi_dom, i, kvmi_reg, enable)) {
                errprint("%s: kvmi_control_msr failed\n", __func__);
                goto error_exit;
            }
    }

    dbprint(VMI_DEBUG_KVM, "--Done %s monitoring on register %" PRIu64"\n",
            (enable ? "enabling" : "disabling"),
            event->reg);
    return VMI_SUCCESS;
error_exit:
    // disable monitoring
    for (unsigned int i = 0; i < vmi->num_vcpus; i++) {
        kvmi_control_events(kvm->kvmi_dom, i, 0);
        if (event_flags & KVMI_EVENT_CR_FLAG)
            kvmi_control_cr(kvm->kvmi_dom, i, kvmi_reg, false);
        if (event_flags & KVMI_EVENT_MSR_FLAG)
            kvmi_control_msr(kvm->kvmi_dom, i, kvmi_reg, false);
    }
    return VMI_FAILURE;
}

status_t
kvm_set_intr_access(
    vmi_instance_t vmi,
    interrupt_event_t* event,
    bool enabled)
{
#ifdef ENABLE_SAFETY_CHECKS
    if (!vmi || !event)
        return VMI_FAILURE;
#endif

    kvm_instance_t *kvm = kvm_get_instance(vmi);
#ifdef ENABLE_SAFETY_CHECKS
    if (!kvm || !kvm->kvmi_dom)
        return VMI_FAILURE;
#endif
    /* TODO: API
     * There is no way to query the old event flag from the VCPU
     * and unset KVMI_EVENT_BREAKPOINT_FLAG.
     * So we might disable CR/MSR or other events here...
     */
    unsigned int event_flag = (enabled) ? KVMI_EVENT_BREAKPOINT_FLAG : 0;

    switch (event->intr) {
        case INT3:
            for (unsigned int vcpu = 0; vcpu < vmi->num_vcpus; vcpu++)
                if (kvmi_control_events(kvm->kvmi_dom, vcpu, event_flag)) {
                    errprint("%s: failed to set event on VCPU %u\n", __func__, vcpu);
                    goto error_exit;
                }
            break;
        default:
            errprint("KVM driver does not support enabling events for interrupt: %"PRIu32"\n", event->intr);
            return VMI_FAILURE;
    }

    dbprint(VMI_DEBUG_KVM, "--%s interrupt %"PRIu32" monitoring\n",
            (enabled) ? "Enabled" : "Disabled", event->intr);

    return VMI_SUCCESS;
error_exit:
    // disable monitoring for all vcpus
    for (unsigned int i = 0; i < vmi->num_vcpus; i++) {
        kvmi_control_events(kvm->kvmi_dom, i, 0);
    }
    return VMI_FAILURE;
}


status_t
kvm_set_mem_access(
        vmi_instance_t vmi,
        addr_t gpfn,
        vmi_mem_access_t page_access_flag,
        uint16_t UNUSED(vmm_pagetable_id))
{
#ifdef ENABLE_SAFETY_CHECKS
    if (!vmi) {
        errprint("%s: invalid vmi handle\n", __func__);
        return VMI_FAILURE;
    }
#endif
    static bool pf_enabled = false;
    unsigned char kvmi_access, kvmi_orig_access;
    kvm_instance_t *kvm = kvm_get_instance(vmi);
#ifdef ENABLE_SAFETY_CHECKS
    if (!kvm || !kvm->kvmi_dom) {
        errprint("%s: invalid kvm handle\n", __func__);
        return VMI_FAILURE;
    }
#endif
    // enable PF events the first time we call this function
    // this avoids enabling them at kvm_init_vmi, since we don't
    // know if the app is going to use mem_events at all
    if (!pf_enabled) {
        bool pf_enabled_succeeded = true;
        for (unsigned int vcpu = 0; vcpu < vmi->num_vcpus; vcpu++) {
            if (kvmi_control_events(kvm->kvmi_dom, vcpu, KVMI_EVENT_PF_FLAG)) {
                pf_enabled_succeeded = false;
                errprint("%s: Fail to enable PF events on VCPU %u\n", __func__, vcpu);
                break;
            }
        }
        if (!pf_enabled_succeeded) {
            // disable PF for all vcpu and fail
            for (unsigned int vcpu = 0; vcpu < vmi->num_vcpus; vcpu++)
                kvmi_control_events(kvm->kvmi_dom, vcpu, 0);
            return VMI_FAILURE;
        }
        pf_enabled = true;
    }

    // get previous access type
    if (kvmi_get_page_access(kvm->kvmi_dom, 0, gpfn, &kvmi_orig_access))
        return VMI_FAILURE;

    // check access type and convert to KVMI
    switch (page_access_flag) {
        case VMI_MEMACCESS_N:
            kvmi_access = 0;
            break;
        case VMI_MEMACCESS_R:
            kvmi_access = kvmi_orig_access & ~KVMI_PAGE_ACCESS_R;
            break;
        case VMI_MEMACCESS_W:
            kvmi_access = kvmi_orig_access & ~KVMI_PAGE_ACCESS_W;
            break;
        case VMI_MEMACCESS_X:
            kvmi_access = kvmi_orig_access & ~KVMI_PAGE_ACCESS_X;
            break;
        case VMI_MEMACCESS_RW:
            kvmi_access = kvmi_orig_access & ~(KVMI_PAGE_ACCESS_R | KVMI_PAGE_ACCESS_W);
            break;
        case VMI_MEMACCESS_WX:
            kvmi_access = kvmi_orig_access & ~(KVMI_PAGE_ACCESS_W | KVMI_PAGE_ACCESS_X);
            break;
        case VMI_MEMACCESS_RWX:
            kvmi_access = 0;
            break;
        default:
            errprint("%s error: invalid memaccess setting requested\n", __func__);
            return VMI_FAILURE;
    }

    // set page access
    bool page_access_succeeded = true;
    long long unsigned int gpa = gpfn << 12;
    for (unsigned int vcpu=0; vcpu < vmi->num_vcpus; vcpu++) {
        if (kvmi_set_page_access(kvm->kvmi_dom, vcpu, &gpa, &kvmi_access, 1)) {
            page_access_succeeded = false;
            errprint("%s error: unable to set page access on GPFN 0x%" PRIx64 "\n", __func__, gpfn);
            break;
        }
    }
    if (!page_access_succeeded) {
        for (unsigned int vcpu=0; vcpu < vmi->num_vcpus; vcpu++)
            kvmi_set_page_access(kvm->kvmi_dom, vcpu, &gpa, &kvmi_orig_access, 1);
    }

    dbprint(VMI_DEBUG_KVM, "--Done setting memaccess on GPFN: 0x%" PRIx64 "\n", gpfn);
    return VMI_SUCCESS;
}
