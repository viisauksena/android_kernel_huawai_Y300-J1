/* Copyright (c) 2013, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/export.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/moduleparam.h>
#include <linux/printk.h>

#include <mach/board.h>
#include <mach/msm_iomap.h>
#include <mach/msm_smem.h>
#include <mach/ramdump.h>
#include <mach/subsystem_notif.h>

#include "smem_private.h"

/**
 * OVERFLOW_ADD_UNSIGNED() - check for unsigned overflow
 *
 * @type: type to check for overflow
 * @a: left value to use
 * @b: right value to use
 * @returns: true if a + b will result in overflow; false otherwise
 */
#define OVERFLOW_ADD_UNSIGNED(type, a, b) \
	(((type)~0 - (a)) < (b) ? true : false)

enum {
	MSM_SMEM_DEBUG = 1U << 0,
	MSM_SMEM_INFO = 1U << 1,
};

static int msm_smem_debug_mask;
module_param_named(debug_mask, msm_smem_debug_mask,
			int, S_IRUGO | S_IWUSR | S_IWGRP);

#define SMEM_DBG(x...) do {                               \
		if (msm_smem_debug_mask & MSM_SMEM_DEBUG) \
			pr_debug(x);                      \
	} while (0)

remote_spinlock_t remote_spinlock;
int spinlocks_initialized;
uint32_t num_smem_areas;
struct smem_area *smem_areas;
struct ramdump_segment *smem_ramdump_segments;

static void *smem_ramdump_dev;

struct restart_notifier_block {
	unsigned processor;
	char *name;
	struct notifier_block nb;
};

static int restart_notifier_cb(struct notifier_block *this,
				unsigned long code,
				void *data);

static struct restart_notifier_block restart_notifiers[] = {
	{SMEM_MODEM, "modem", .nb.notifier_call = restart_notifier_cb},
	{SMEM_Q6, "lpass", .nb.notifier_call = restart_notifier_cb},
	{SMEM_WCNSS, "wcnss", .nb.notifier_call = restart_notifier_cb},
	{SMEM_DSPS, "dsps", .nb.notifier_call = restart_notifier_cb},
	{SMEM_MODEM, "gss", .nb.notifier_call = restart_notifier_cb},
	{SMEM_Q6, "adsp", .nb.notifier_call = restart_notifier_cb},
};

/**
 * smem_phys_to_virt() - Convert a physical base and offset to virtual address
 *
 * @base: physical base address to check
 * @offset: offset from the base to get the final address
 * @returns: virtual SMEM address; NULL for failure
 *
 * Takes a physical address and an offset and checks if the resulting physical
 * address would fit into one of the smem regions.  If so, returns the
 * corresponding virtual address.  Otherwise returns NULL.
 */
static void *smem_phys_to_virt(phys_addr_t base, unsigned offset)
{
	int i;
	phys_addr_t phys_addr;
	resource_size_t size;

	if (OVERFLOW_ADD_UNSIGNED(phys_addr_t, base, offset))
		return NULL;

	if (!smem_areas) {
		/*
		 * Early boot - no area configuration yet, so default
		 * to using the main memory region.
		 *
		 * To remove the MSM_SHARED_RAM_BASE and the static
		 * mapping of SMEM in the future, add dump_stack()
		 * to identify the early callers of smem_get_entry()
		 * (which calls this function) and replace those calls
		 * with a new function that knows how to lookup the
		 * SMEM base address before SMEM has been probed.
		 */
		phys_addr = msm_shared_ram_phys;
		size = MSM_SHARED_RAM_SIZE;

		if (base >= phys_addr && base + offset < phys_addr + size) {
			if (OVERFLOW_ADD_UNSIGNED(uintptr_t,
				(uintptr_t)MSM_SHARED_RAM_BASE, offset)) {
				pr_err("%s: overflow %p %x\n", __func__,
					MSM_SHARED_RAM_BASE, offset);
				return NULL;
			}

			return MSM_SHARED_RAM_BASE + offset;
		} else {
			return NULL;
		}
	}
	for (i = 0; i < num_smem_areas; ++i) {
		phys_addr = smem_areas[i].phys_addr;
		size = smem_areas[i].size;

		if (base < phys_addr || base + offset >= phys_addr + size)
			continue;

		if (OVERFLOW_ADD_UNSIGNED(uintptr_t,
				(uintptr_t)smem_areas[i].virt_addr, offset)) {
			pr_err("%s: overflow %p %x\n", __func__,
				smem_areas[i].virt_addr, offset);
			return NULL;
		}

		return smem_areas[i].virt_addr + offset;
	}

	return NULL;
}

/**
 * smem_virt_to_phys() - Convert SMEM address to physical address.
 *
 * @smem_address: Address of SMEM item (returned by smem_alloc(), etc)
 * @returns: Physical address (or NULL if there is a failure)
 *
 * This function should only be used if an SMEM item needs to be handed
 * off to a DMA engine.
 */
phys_addr_t smem_virt_to_phys(void *smem_address)
{
	phys_addr_t phys_addr = 0;
	int i;
	void *vend;

	if (!smem_areas)
		return phys_addr;

	for (i = 0; i < num_smem_areas; ++i) {
		vend = (void *)(smem_areas[i].virt_addr + smem_areas[i].size);

		if (smem_address >= smem_areas[i].virt_addr &&
				smem_address < vend) {
			phys_addr = smem_address - smem_areas[i].virt_addr;
			phys_addr +=  smem_areas[i].phys_addr;
			break;
		}
	}

	return phys_addr;
}
EXPORT_SYMBOL(smem_virt_to_phys);

/* smem_alloc returns the pointer to smem item if it is already allocated.
 * Otherwise, it returns NULL.
 */
void *smem_alloc(unsigned id, unsigned size)
{
	return smem_find(id, size);
}
EXPORT_SYMBOL(smem_alloc);

void *smem_find(unsigned id, unsigned size_in)
{
	unsigned size;
	void *ptr;

	ptr = smem_get_entry(id, &size);
	if (!ptr)
		return 0;

	size_in = ALIGN(size_in, 8);
	if (size_in != size) {
		pr_err("smem_find(%d, %d): wrong size %d\n",
			id, size_in, size);
		return 0;
	}

	return ptr;
}
EXPORT_SYMBOL(smem_find);

/* smem_alloc2 returns the pointer to smem item.  If it is not allocated,
 * it allocates it and then returns the pointer to it.
 */
void *smem_alloc2(unsigned id, unsigned size_in)
{
	struct smem_shared *shared = (void *) MSM_SHARED_RAM_BASE;
	struct smem_heap_entry *toc = shared->heap_toc;
	unsigned long flags;
	void *ret = NULL;

	if (!shared->heap_info.initialized) {
		pr_err("%s: smem heap info not initialized\n", __func__);
		return NULL;
	}

	if (id >= SMEM_NUM_ITEMS)
		return NULL;

	size_in = ALIGN(size_in, 8);
	remote_spin_lock_irqsave(&remote_spinlock, flags);
	if (toc[id].allocated) {
		SMEM_DBG("%s: %u already allocated\n", __func__, id);
		if (size_in != toc[id].size)
			pr_err("%s: wrong size %u (expected %u)\n",
				__func__, toc[id].size, size_in);
		else
			ret = (void *)(MSM_SHARED_RAM_BASE + toc[id].offset);
	} else if (id > SMEM_FIXED_ITEM_LAST) {
		SMEM_DBG("%s: allocating %u\n", __func__, id);
		if (shared->heap_info.heap_remaining >= size_in) {
			toc[id].offset = shared->heap_info.free_offset;
			toc[id].size = size_in;
			wmb();
			toc[id].allocated = 1;

			shared->heap_info.free_offset += size_in;
			shared->heap_info.heap_remaining -= size_in;
			ret = (void *)(MSM_SHARED_RAM_BASE + toc[id].offset);
		} else
			pr_err("%s: not enough memory %u (required %u)\n",
				__func__, shared->heap_info.heap_remaining,
				size_in);
	}
	wmb();
	remote_spin_unlock_irqrestore(&remote_spinlock, flags);
	return ret;
}
EXPORT_SYMBOL(smem_alloc2);

void *smem_get_entry(unsigned id, unsigned *size)
{
	struct smem_shared *shared = (void *) MSM_SHARED_RAM_BASE;
	struct smem_heap_entry *toc = shared->heap_toc;
	int use_spinlocks = spinlocks_initialized;
	void *ret = 0;
	unsigned long flags = 0;

	if (id >= SMEM_NUM_ITEMS)
		return ret;

	if (use_spinlocks)
		remote_spin_lock_irqsave(&remote_spinlock, flags);
	/* toc is in device memory and cannot be speculatively accessed */
	if (toc[id].allocated) {
		phys_addr_t phys_base;

		*size = toc[id].size;
		barrier();

		phys_base = toc[id].reserved & BASE_ADDR_MASK;
		if (!phys_base)
			phys_base = (phys_addr_t)msm_shared_ram_phys;
		ret = smem_phys_to_virt(phys_base, toc[id].offset);
	} else {
		*size = 0;
	}
	if (use_spinlocks)
		remote_spin_unlock_irqrestore(&remote_spinlock, flags);

	return ret;
}
EXPORT_SYMBOL(smem_get_entry);


/**
 * smem_get_remote_spinlock - Remote spinlock pointer for unit testing.
 *
 * @returns: pointer to SMEM remote spinlock
 */
remote_spinlock_t *smem_get_remote_spinlock(void)
{
	return &remote_spinlock;
}
EXPORT_SYMBOL(smem_get_remote_spinlock);

static int restart_notifier_cb(struct notifier_block *this,
				unsigned long code,
				void *data)
{
	if (code == SUBSYS_AFTER_SHUTDOWN) {
		struct restart_notifier_block *notifier;

		notifier = container_of(this,
					struct restart_notifier_block, nb);
		SMEM_DBG("%s: ssrestart for processor %d ('%s')\n",
				__func__, notifier->processor,
				notifier->name);

		remote_spin_release(&remote_spinlock, notifier->processor);
		remote_spin_release_all(notifier->processor);

		if (smem_ramdump_dev) {
			int ret;

			SMEM_DBG("%s: saving ramdump\n", __func__);
			/*
			 * XPU protection does not currently allow the
			 * auxiliary memory regions to be dumped.  If this
			 * changes, then num_smem_areas + 1 should be passed
			 * into do_elf_ramdump() to dump all regions.
			 */
			ret = do_elf_ramdump(smem_ramdump_dev,
					smem_ramdump_segments, 1);
			if (ret < 0)
				pr_err("%s: unable to dump smem %d\n", __func__,
						ret);
		}
	}

	return NOTIFY_DONE;
}

static __init int modem_restart_late_init(void)
{
	int i;
	void *handle;
	struct restart_notifier_block *nb;

	smem_ramdump_dev = create_ramdump_device("smem", NULL);
	if (IS_ERR_OR_NULL(smem_ramdump_dev)) {
		pr_err("%s: Unable to create smem ramdump device.\n",
			__func__);
		smem_ramdump_dev = NULL;
	}

	for (i = 0; i < ARRAY_SIZE(restart_notifiers); i++) {
		nb = &restart_notifiers[i];
		handle = subsys_notif_register_notifier(nb->name, &nb->nb);
		SMEM_DBG("%s: registering notif for '%s', handle=%p\n",
				__func__, nb->name, handle);
	}

	return 0;
}
late_initcall(modem_restart_late_init);
