/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <helper/log.h>
#include <helper/command.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "target.h"
#include "target_type.h"
#include "target_mem_cache.h"

#define MEMCACHE_DEFAULT_PAGE	1024
/* Cap total cached region, not page count — small pages must not shrink the
 * allowed region, and huge regions must not exhaust host RAM / RTT on miss. */
#define MEMCACHE_MAX_BYTES	(2u * 1024u * 1024u)

struct memcache_page {
	bool valid;
	uint8_t *data;
};

static bool memcache_enabled;
static bool memcache_filling;			/* bypass cache while filling a page */
static target_addr_t memcache_base;
static uint32_t memcache_size;			/* bytes covered */
static uint32_t memcache_page_size = MEMCACHE_DEFAULT_PAGE;
static struct memcache_page *memcache_pages;
static unsigned int memcache_num_pages;
static uint64_t memcache_hits;
static uint64_t memcache_miss_pages;
static uint64_t memcache_bypasses;

static void memcache_free_pages(void)
{
	if (!memcache_pages)
		return;
	for (unsigned int i = 0; i < memcache_num_pages; i++)
		free(memcache_pages[i].data);
	free(memcache_pages);
	memcache_pages = NULL;
	memcache_num_pages = 0;
}

static int memcache_alloc_pages(void)
{
	memcache_free_pages();
	if (memcache_size == 0 || memcache_page_size == 0)
		return ERROR_OK;

	if (memcache_size > MEMCACHE_MAX_BYTES) {
		LOG_ERROR("memcache: region size 0x%" PRIx32 " exceeds max 0x%x bytes",
				memcache_size, MEMCACHE_MAX_BYTES);
		return ERROR_FAIL;
	}

	unsigned int n = (memcache_size + memcache_page_size - 1) / memcache_page_size;
	if (n == 0) {
		LOG_ERROR("memcache: page count invalid (size=0x%" PRIx32
				" page=0x%" PRIx32 ")",
				memcache_size, memcache_page_size);
		return ERROR_FAIL;
	}

	memcache_pages = calloc(n, sizeof(*memcache_pages));
	if (!memcache_pages)
		return ERROR_FAIL;
	memcache_num_pages = n;
	return ERROR_OK;
}

void target_mem_cache_invalidate(void)
{
	if (!memcache_pages)
		return;
	for (unsigned int i = 0; i < memcache_num_pages; i++) {
		memcache_pages[i].valid = false;
		/* keep allocated data buffers for reuse */
	}
}

static bool memcache_in_region(target_addr_t address, uint32_t byte_count)
{
	if (!memcache_enabled || memcache_size == 0 || !memcache_pages)
		return false;
	if (byte_count == 0)
		return false;
	if (address < memcache_base)
		return false;
	if ((address - memcache_base) + (target_addr_t)byte_count > memcache_size)
		return false;
	return true;
}

static int memcache_fill_page(struct target *target, unsigned int page_idx)
{
	if (page_idx >= memcache_num_pages)
		return ERROR_FAIL;

	struct memcache_page *p = &memcache_pages[page_idx];
	if (!p->data) {
		p->data = malloc(memcache_page_size);
		if (!p->data)
			return ERROR_FAIL;
	}

	target_addr_t addr = memcache_base + (target_addr_t)page_idx * memcache_page_size;
	uint32_t len = memcache_page_size;
	/* last page may be partial relative to region end */
	target_addr_t end = memcache_base + memcache_size;
	if (addr + len > end)
		len = (uint32_t)(end - addr);

	if (!target->type->read_buffer && !target->type->read_memory)
		return ERROR_FAIL;

	int retval;
	/* Prefer bulk buffer read to amortize DAP RTT; call driver directly. */
	/*
	 * Driver read_buffer often ends up in target_read_memory() via the
	 * default helper — which would re-enter this cache. Bypass while filling.
	 *
	 * read_memory fallback must not round length up: that can touch memory
	 * past the configured region (or unmapped flash/SRAM holes).
	 */
	memcache_filling = true;
	if (target->type->read_buffer) {
		retval = target->type->read_buffer(target, addr, len, p->data);
	} else {
		uint32_t words = len / 4;
		uint32_t rem = len % 4;

		retval = ERROR_OK;
		if (words > 0)
			retval = target->type->read_memory(target, addr, 4, words, p->data);
		if (retval == ERROR_OK && rem > 0)
			retval = target->type->read_memory(target, addr + words * 4, 1,
					rem, p->data + words * 4);
	}
	memcache_filling = false;

	if (retval != ERROR_OK) {
		p->valid = false;
		return retval;
	}

	/* if len < page_size, zero the unused tail so reads don't see garbage */
	if (len < memcache_page_size)
		memset(p->data + len, 0, memcache_page_size - len);

	p->valid = true;
	memcache_miss_pages++;
	LOG_DEBUG("memcache: filled page %u @ " TARGET_ADDR_FMT " (%" PRIu32 " bytes)",
			page_idx, addr, len);
	return ERROR_OK;
}

int target_mem_cache_read(struct target *target, target_addr_t address,
		uint32_t byte_count, uint8_t *buffer)
{
	if (memcache_filling) {
		memcache_bypasses++;
		return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
	}

	if (!memcache_in_region(address, byte_count)) {
		memcache_bypasses++;
		return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
	}

	if (target->state != TARGET_HALTED) {
		memcache_bypasses++;
		return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
	}

	target_addr_t offset = address - memcache_base;
	uint32_t done = 0;

	while (done < byte_count) {
		unsigned int page_idx = (unsigned int)((offset + done) / memcache_page_size);
		uint32_t page_off = (uint32_t)((offset + done) % memcache_page_size);
		uint32_t chunk = memcache_page_size - page_off;
		if (chunk > byte_count - done)
			chunk = byte_count - done;

		if (page_idx >= memcache_num_pages)
			return ERROR_FAIL;

		if (!memcache_pages[page_idx].valid) {
			int retval = memcache_fill_page(target, page_idx);
			if (retval != ERROR_OK)
				return retval;
		}

		memcpy(buffer + done, memcache_pages[page_idx].data + page_off, chunk);
		done += chunk;
		memcache_hits++;
	}

	return ERROR_OK;
}

void target_mem_cache_write_update(struct target *target, target_addr_t address,
		uint32_t byte_count, const uint8_t *buffer)
{
	(void)target;
	if (!memcache_in_region(address, byte_count))
		return;

	target_addr_t offset = address - memcache_base;
	uint32_t done = 0;
	while (done < byte_count) {
		unsigned int page_idx = (unsigned int)((offset + done) / memcache_page_size);
		uint32_t page_off = (uint32_t)((offset + done) % memcache_page_size);
		uint32_t chunk = memcache_page_size - page_off;
		if (chunk > byte_count - done)
			chunk = byte_count - done;

		if (page_idx < memcache_num_pages && memcache_pages[page_idx].valid &&
				memcache_pages[page_idx].data)
			memcpy(memcache_pages[page_idx].data + page_off, buffer + done, chunk);
		done += chunk;
	}
}

static int memcache_event(struct target *target, enum target_event event, void *priv)
{
	(void)target;
	(void)priv;
	switch (event) {
	case TARGET_EVENT_RESUMED:
	case TARGET_EVENT_DEBUG_RESUMED:
	case TARGET_EVENT_RESET_START:
		target_mem_cache_invalidate();
		break;
	default:
		break;
	}
	return ERROR_OK;
}

void target_mem_cache_init(void)
{
	static bool inited;
	if (inited)
		return;
	inited = true;
	target_register_event_callback(memcache_event, NULL);
}

COMMAND_HANDLER(handle_memcache_command)
{
	if (CMD_ARGC < 1)
		return ERROR_COMMAND_SYNTAX_ERROR;

	if (strcmp(CMD_ARGV[0], "enable") == 0) {
		if (memcache_size == 0) {
			command_print(CMD, "memcache: set region first "
					"(memcache region <base> <size>)");
			return ERROR_FAIL;
		}
		if (!memcache_pages) {
			int retval = memcache_alloc_pages();
			if (retval != ERROR_OK)
				return retval;
		}
		memcache_enabled = true;
		target_mem_cache_invalidate();
		command_print(CMD, "memcache: enabled "
				TARGET_ADDR_FMT "+0x%" PRIx32 " page=%" PRIu32,
				memcache_base, memcache_size, memcache_page_size);
		return ERROR_OK;
	}

	if (strcmp(CMD_ARGV[0], "disable") == 0) {
		memcache_enabled = false;
		target_mem_cache_invalidate();
		command_print(CMD, "memcache: disabled");
		return ERROR_OK;
	}

	if (strcmp(CMD_ARGV[0], "flush") == 0) {
		target_mem_cache_invalidate();
		command_print(CMD, "memcache: flushed");
		return ERROR_OK;
	}

	if (strcmp(CMD_ARGV[0], "info") == 0) {
		command_print(CMD, "memcache: %s base=" TARGET_ADDR_FMT
				" size=0x%" PRIx32 " page=%" PRIu32 " pages=%u",
				memcache_enabled ? "enabled" : "disabled",
				memcache_base, memcache_size, memcache_page_size,
				memcache_num_pages);
		command_print(CMD, "memcache stats: hits=%" PRIu64
				" miss_pages=%" PRIu64 " bypass=%" PRIu64,
				memcache_hits, memcache_miss_pages, memcache_bypasses);
		unsigned int valid = 0;
		for (unsigned int i = 0; i < memcache_num_pages; i++)
			if (memcache_pages && memcache_pages[i].valid)
				valid++;
		command_print(CMD, "memcache valid_pages=%u", valid);
		return ERROR_OK;
	}

	if (strcmp(CMD_ARGV[0], "page") == 0) {
		if (CMD_ARGC != 2)
			return ERROR_COMMAND_SYNTAX_ERROR;
		uint32_t psz;
		COMMAND_PARSE_NUMBER(u32, CMD_ARGV[1], psz);
		if (psz < 64 || (psz & (psz - 1)) != 0) {
			command_print(CMD, "memcache: page size must be power of 2, >= 64");
			return ERROR_FAIL;
		}
		memcache_page_size = psz;
		if (memcache_size) {
			int retval = memcache_alloc_pages();
			if (retval != ERROR_OK)
				return retval;
		}
		target_mem_cache_invalidate();
		command_print(CMD, "memcache: page size %" PRIu32, memcache_page_size);
		return ERROR_OK;
	}

	if (strcmp(CMD_ARGV[0], "region") == 0) {
		if (CMD_ARGC != 3)
			return ERROR_COMMAND_SYNTAX_ERROR;
		target_addr_t base;
		uint32_t size;
		COMMAND_PARSE_ADDRESS(CMD_ARGV[1], base);
		COMMAND_PARSE_NUMBER(u32, CMD_ARGV[2], size);
		if (size == 0) {
			command_print(CMD, "memcache: size must be > 0");
			return ERROR_FAIL;
		}
		memcache_base = base;
		memcache_size = size;
		int retval = memcache_alloc_pages();
		if (retval != ERROR_OK)
			return retval;
		target_mem_cache_invalidate();
		command_print(CMD, "memcache: region " TARGET_ADDR_FMT
				" size 0x%" PRIx32 " (%u pages)",
				memcache_base, memcache_size, memcache_num_pages);
		return ERROR_OK;
	}

	return ERROR_COMMAND_SYNTAX_ERROR;
}

static const struct command_registration memcache_command_handlers[] = {
	{
		.name = "memcache",
		.handler = handle_memcache_command,
		.mode = COMMAND_ANY,
		.help = "halt-scoped SRAM page cache (for high-RTT DAP/TCP)",
		.usage = "region <base> <size>|page <bytes>|enable|disable|flush|info",
	},
	COMMAND_REGISTRATION_DONE
};

int target_mem_cache_register_commands(struct command_context *cmd_ctx)
{
	target_mem_cache_init();
	return register_commands(cmd_ctx, NULL, memcache_command_handlers);
}
