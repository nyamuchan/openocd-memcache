/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef OPENOCD_TARGET_TARGET_MEM_CACHE_H
#define OPENOCD_TARGET_TARGET_MEM_CACHE_H

#include <helper/types.h>
#include <target/target.h>

/**
 * Halt-scoped SRAM page cache for high-RTT links (CMSIS-DAP TCP / Wi-Fi).
 *
 * On miss within the configured region, fetch a whole page via the target
 * driver's bulk read (not via target_read_* wrappers — avoids recursion).
 * Invalidate on resume/reset. Writes update cached pages (write-through).
 *
 * Tcl:
 *   memcache region <base> <size>  (size capped at 2 MiB)
 *   memcache page <bytes>          (default 1024, power of two >= 64)
 *   memcache enable|disable
 *   memcache info
 *   memcache flush
 *
 * Invalidate on resume/reset (including debug-resume / single-step).
 */

int target_mem_cache_register_commands(struct command_context *cmd_ctx);
void target_mem_cache_init(void);

/* Returns ERROR_OK if the request was fully served from cache (after any
 * miss-fills). Returns ERROR_TARGET_RESOURCE_NOT_AVAILABLE if the cache
 * should not handle this access (disabled / wrong state / out of region).
 * Other ERROR_* means a fill/read failed. */
int target_mem_cache_read(struct target *target, target_addr_t address,
		uint32_t byte_count, uint8_t *buffer);

/* Update cache after a successful live write; never fails the write. */
void target_mem_cache_write_update(struct target *target, target_addr_t address,
		uint32_t byte_count, const uint8_t *buffer);

void target_mem_cache_invalidate(void);

#endif /* OPENOCD_TARGET_TARGET_MEM_CACHE_H */
