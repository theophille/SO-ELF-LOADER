/*
 * Loader Implementation
 *
 * 2022, Operating Systems
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>

#include "exec_parser.h"

#define ALLOC_STEP  10
#define PAGE_SIZE 4096

struct da_array {
	uint32_t size;
	uint32_t capacity;
	uintptr_t *arr;
};

int fd;
static so_exec_t *exec;
static struct da_array *already_mapped;

// dynamically allocated vector initializator
void init_da_array(struct da_array *daa)
{
	daa->size = 0;
	daa->capacity = ALLOC_STEP;
	daa->arr = (uintptr_t *)malloc(sizeof(uintptr_t) * ALLOC_STEP);
}

// default handler caller
void trigger_default_handler(void)
{
	struct sigaction default_handler;

	memset(&default_handler, 0, sizeof(default_handler));
	default_handler.sa_handler = SIG_DFL;
	int rc = sigaction(SIGSEGV, &default_handler, NULL);

	if (rc < 0) {
		perror("sigaction");
		exit(-1);
	}
}

// SIGSEGV handler
static void segv_handler(int signum, siginfo_t *info, void *context)
{
	void *fault_addr = info->si_addr;
	unsigned int seg_index = -1;

	// searching the segment in which segfault occured
	for (int i = 0; i < exec->segments_no; i++) {
		int size = exec->segments[i].mem_size;
		int offset = PAGE_SIZE * (size / PAGE_SIZE + 1);
		void *seg_start = (void *)exec->segments[i].vaddr;
		void *seg_end = seg_start + offset;

		if (fault_addr >= seg_start && fault_addr < seg_end) {
			seg_index = i;
			break;
		}
	}

	// if the address doesn't belong to the address space of the process, the default handler will be called
	// an invalid memory access occured
	if (seg_index == -1)
		trigger_default_handler();

	// finding the starting address of the page
	uintptr_t to_be_loaded = (uintptr_t)info->si_addr & 0xffff000;

	// checking if the page has already been mapped
	// if true, an illegal memory access is attempted and the default handler will be called
	for (int i = 0; i < already_mapped->size; i++)
		if (already_mapped->arr[i] == to_be_loaded)
			trigger_default_handler();

	// pushing the starting address of the page in the already_mapped->arr array
	already_mapped->arr[already_mapped->size++] = to_be_loaded;

	if (already_mapped->size > already_mapped->capacity) {
		already_mapped->capacity += ALLOC_STEP;
		already_mapped->arr = (uintptr_t *)realloc(already_mapped->arr, already_mapped->capacity * sizeof(uintptr_t));
	}

	so_seg_t s = exec->segments[seg_index];
	uintptr_t last_segment_address_in_file = s.vaddr + s.file_size;

	void *result;

	// if the starting address of the page is in the file content side, there will be three cases
	if (to_be_loaded < last_segment_address_in_file) {
		uint32_t page_addr_to_file_seg_end = last_segment_address_in_file - to_be_loaded;
		uint32_t off = to_be_loaded - s.vaddr;
		uint32_t page_addr_to_mem_seg_end = (s.vaddr + s.mem_size) - to_be_loaded;

		if (page_addr_to_file_seg_end > PAGE_SIZE) {
			// if the distance between the starting address of the page and the last segment address in the file
			// is greater than 4096, then a mapping between the content of size 4096 which starts
			// at s.offset + off from the begining of the file and the addresses that correspond to the page will occur
			result = mmap((void *)to_be_loaded, PAGE_SIZE, s.perm, MAP_FIXED | MAP_PRIVATE, fd, s.offset + off);
		} else if (s.mem_size - s.file_size == 0) {
			// else if the distance between the starting address of the page and the last segment address in the file
			// is smaller than 4096 and the difference between mem_size and file_size is zero, then a mapping between the
			// content of size page_addr_to_file_seg_end which start at s.offset + off from relative to the beginning of the file
			// and the addresses which correspond to the page will occur
			result = mmap((void *)to_be_loaded, page_addr_to_file_seg_end, s.perm, MAP_FIXED | MAP_PRIVATE, fd, s.offset + off);
		} else {
			// the difference is greater than 0
			// annonymous mapping will be used to zeroize the rest of mem_size - file_size bytes
			if (page_addr_to_mem_seg_end > PAGE_SIZE)
				// an annonymous mapping of size 4096 occurs
				result = mmap((void *)to_be_loaded, PAGE_SIZE, s.perm, MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED, 0, 0);
			else
				// an annonymous mapping of size < 4096 size occurs
				result = mmap((void *)to_be_loaded, page_addr_to_mem_seg_end, s.perm, MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED, 0, 0);

			// placing the file pointer at offset s.offset + off
			lseek(fd, s.offset + off, SEEK_SET);
			void *read_buffer = malloc(page_addr_to_file_seg_end);

			// reading the file data in a buffer and copying it at the address to which the mapping was done
			read(fd, read_buffer, page_addr_to_file_seg_end);
			memcpy(result, read_buffer, page_addr_to_file_seg_end);
			free(read_buffer);
		}
	} else {
		// else if the starting address of the page isn't in the file content side, zeroized page will be mapped
		uint32_t page_addr_to_mem_seg_end = (s.vaddr + s.mem_size) - (uintptr_t)to_be_loaded;

		if (page_addr_to_mem_seg_end > PAGE_SIZE)
			// an annonymous mapping of size 4096 occurs
			result = mmap((void *)to_be_loaded, PAGE_SIZE, s.perm, MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED, 0, 0);
		else
			// an annonymous mapping of size < 4096 size occurs
			result = mmap((void *)to_be_loaded, page_addr_to_mem_seg_end, s.perm, MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED, 0, 0);
	}
}

// loader initializer
int so_init_loader(void)
{
	int rc;
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = segv_handler;
	sa.sa_flags = SA_SIGINFO;
	rc = sigaction(SIGSEGV, &sa, NULL);

	if (rc < 0) {
		perror("sigaction");
		return -1;
	}
	return 0;
}

int so_execute(char *path, char *argv[])
{
	// opening the executable
	fd = open(*(argv), O_RDONLY);

	if (fd == -1) {
		write(2, "Could not open the executable...\n", 33);
		exit(1);
	}

	// parsing the executable
	exec = so_parse_exec(path);
	if (!exec)
		return -1;

	// allocating and initializing a dynamically allocated vector
	already_mapped = (struct da_array *)malloc(sizeof(struct da_array));
	init_da_array(already_mapped);

	// the execution starts
	so_start_exec(exec, argv);

	close(fd);
	free(already_mapped->arr);
	free(already_mapped);

	return -1;
}
