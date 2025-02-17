#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include <sys/stat.h>
#include <pwd.h>

#include <mach-o/loader.h>
#include <elf.h>

#include <linux/time_types.h>

#if __x86_64__
#include "x86_64.h"
#else
#error Not implemented
#endif

#include <darling-config.h>

// interestingly enough, there are no existing tools that can perform this conversion (ELF coredump to Mach-O coredump).
// neither objcopy nor llvm-objcopy support Mach-O conversion like that, nor does objconv (it considers coredumps to be executables and refuses to operate on them).
// porting our existing code from the LKM is simple enough, so that's what we do here.

struct vm_area {
	const char* filename;
	size_t filename_length;
	uintptr_t memory_address;
	size_t memory_size;
	size_t file_offset;
	size_t file_size;
	uint8_t protection;
	bool valid;
	uint64_t expected_offset;
};

struct nt_file_header {
	uint64_t count;
	uint64_t page_size;
};

struct nt_file_entry {
	uint64_t start;
	uint64_t end;
	uint64_t offset;
};

struct nt_prstatus {
	int signal_number;
	int signal_code;
	int signal_errno;
	short current_signal;
	unsigned long pending_signals;
	unsigned long held_signals;
	pid_t pid;
	pid_t ppid;
	pid_t pgrp;
	pid_t sid;
	struct __kernel_old_timeval user_time;
	struct __kernel_old_timeval system_time;
	struct __kernel_old_timeval cumulative_user_time;
	struct __kernel_old_timeval cumulative_system_time;
	struct nt_prstatus_registers general_registers;
};

struct thread_info {
	struct nt_prstatus* prstatus;
};

struct coredump_params {
	int input_corefile;
	int output_corefile;
	size_t input_corefile_size;
	const void* input_corefile_mapping;
	const Elf64_Ehdr* input_header;
	const Elf64_Phdr* input_program_headers;
	const Elf64_Phdr* input_program_headers_end;
	const Elf64_Nhdr* input_notes;
	size_t input_notes_size;
	struct vm_area* vm_areas;
	size_t vm_area_count;
	struct nt_file_header* nt_file;
	const char** nt_file_filenames;
	struct thread_info* thread_infos;
	size_t thread_info_count;
	size_t written;
	const char* prefix;
	size_t prefix_length;
	const char* main_executable_path;
	size_t main_executable_path_length;
};

static char default_output_name[4096];
static char default_prefix[4096];

static uint64_t round_up_pow2(uint64_t number, uint64_t multiple) {
	return (number + (multiple - 1)) & -multiple;
};

static uint64_t round_down_pow2(uint64_t number, uint64_t multiple) {
	return (number) & -multiple;
};

static const char* note_name(const Elf64_Nhdr* note) {
	return (const char*)note + sizeof(*note);
};

static const void* note_data(const Elf64_Nhdr* note) {
	return note_name(note) + round_up_pow2(note->n_namesz, 4);
};

static const Elf64_Nhdr* find_next_note(const Elf64_Nhdr* note) {
	uint64_t length = sizeof(*note) + round_up_pow2(note->n_namesz, 4) + round_up_pow2(note->n_descsz, 4);
	return (Elf64_Nhdr*)((char*)note + length);
};

// first tries to open the file directly, then tries to open the file in the lower layer of the overlay
// (because if we're outside the container, the overlay won't be mounted, but the core dump paths would refer to it)
static int open_file(struct coredump_params* cprm, const char* filename, size_t filename_length) {
	int fd = -1;

	// try to open it directly first
	fd = open(filename, O_RDONLY);

	// if that fails, try to see if it refers to the mounted prefix; if it does, try the lower layer
	if (fd < 0 && filename_length >= cprm->prefix_length && strncmp(filename, cprm->prefix, cprm->prefix_length) == 0) {
		char* temp_filename = malloc(sizeof(LIBEXEC_PATH "/") + (filename_length - cprm->prefix_length));
		sprintf(temp_filename, "%s/%s", LIBEXEC_PATH, &filename[cprm->prefix_length]);
		fd = open(temp_filename, O_RDONLY);
		free(temp_filename);
	}

	return fd;
};

void macho_coredump(struct coredump_params* cprm);

int main(int argc, char** argv) {
	if (argc < 2) {
		fprintf(stderr, "Usage: %s <input-coredump> [output-coredump]\n", (argc > 0 ? argv[0] : "darling-coredump"));
		return 1;
	}

	if (snprintf(default_output_name, sizeof(default_output_name), "darlingcore-%s", argv[1]) < 0) {
		perror("snprintf");
		return 1;
	}

	const char* homedir = NULL;

	if (getenv("HOME")) {
		homedir = getenv("HOME");
	} else {
		struct passwd* pwd = getpwuid(getuid());
		homedir = pwd->pw_dir;
	}

	if (snprintf(default_prefix, sizeof(default_prefix), "%s/.darling", homedir) < 0) {
		perror("snprintf");
		return 1;
	}

	struct coredump_params cprm = {0};

	cprm.prefix = getenv("DPREFIX");

	if (!cprm.prefix) {
		cprm.prefix = default_prefix;
	}

	cprm.prefix_length = strlen(cprm.prefix);

	cprm.input_corefile = open(argv[1], O_RDONLY);
	if (cprm.input_corefile < 0) {
		perror("open input");
		return 1;
	}

	struct stat input_corefile_stats;
	if (fstat(cprm.input_corefile, &input_corefile_stats) < 0) {
		perror("fstat");
		return 1;
	}
	cprm.input_corefile_size = input_corefile_stats.st_size;
	cprm.input_corefile_mapping = mmap(NULL, cprm.input_corefile_size, PROT_READ, MAP_PRIVATE, cprm.input_corefile, 0);

	cprm.output_corefile = open((argc > 2 ? argv[2] : default_output_name), O_WRONLY | O_TRUNC | O_CREAT, 0644);
	if (cprm.output_corefile < 0) {
		perror("open output");
		return 1;
	}

	cprm.input_header = cprm.input_corefile_mapping;

	if (
		cprm.input_header->e_ident[EI_MAG0] != ELFMAG0 ||
		cprm.input_header->e_ident[EI_MAG1] != ELFMAG1 ||
		cprm.input_header->e_ident[EI_MAG2] != ELFMAG2 ||
		cprm.input_header->e_ident[EI_MAG3] != ELFMAG3 ||
		cprm.input_header->e_ident[EI_CLASS] != ELFCLASS64 ||
		cprm.input_header->e_ident[EI_VERSION] != EV_CURRENT ||
		cprm.input_header->e_version != EV_CURRENT ||
		cprm.input_header->e_type != ET_CORE
	) {
		fprintf(stderr, "Input file is not a valid corefile\n");
		return 1;
	}

#if __x86_64__
	if (cprm.input_header->e_machine != EM_X86_64) {
		fprintf(stderr, "Input file is not a valid x86_64 corefile\n");
		return 1;
	}
#else
	#error Not implemented!
#endif

	cprm.input_program_headers = (const void*)((const char*)cprm.input_corefile_mapping + cprm.input_header->e_phoff);
	cprm.input_program_headers_end = (const Elf64_Phdr*)((const char*)cprm.input_program_headers + (cprm.input_header->e_phentsize * cprm.input_header->e_phnum));

	// first, count how many VM areas we have
	for (const Elf64_Phdr* program_header = cprm.input_program_headers; program_header < cprm.input_program_headers_end; program_header = (const Elf64_Phdr*)((const char*)program_header + cprm.input_header->e_phentsize)) {
		if (program_header->p_type == PT_LOAD) {
			++cprm.vm_area_count;
		} else if (program_header->p_type == PT_NOTE) {
			// while we're at it, also load the NOTE segment

			// XXX: ignoring it is probably not the best choice
			if (cprm.input_notes) {
				printf("warning: ignoring extra PT_NOTE segment\n");
				continue;
			}

			cprm.input_notes = (const void*)((const char*)cprm.input_corefile_mapping + program_header->p_offset); 
			cprm.input_notes_size = program_header->p_filesz;
		}
	}

	if (!cprm.input_notes) {
		fprintf(stderr, "Input corefile does not contain PT_NOTE segment\n");
		return 1;
	}

	for (const Elf64_Nhdr* note_header = cprm.input_notes; note_header < (const Elf64_Nhdr*)((const char*)cprm.input_notes + cprm.input_notes_size); note_header = find_next_note(note_header)) {
		if (note_header->n_type == NT_FILE) {
			// allocate a copy for alignment purposes
			cprm.nt_file = malloc(note_header->n_descsz);
			if (!cprm.nt_file) {
				perror("malloc");
				return 1;
			}
			memcpy(cprm.nt_file, note_data(note_header), note_header->n_descsz);

			cprm.nt_file_filenames = malloc(cprm.nt_file->count * sizeof(const char*));
			if (!cprm.nt_file_filenames) {
				perror("malloc");
				return 1;
			}

			const char* filenames = (const char*)cprm.nt_file + sizeof(struct nt_file_header) + (sizeof(struct nt_file_entry) * cprm.nt_file->count);
			for (size_t i = 0, offset = 0; i < cprm.nt_file->count; ++i) {
				cprm.nt_file_filenames[i] = &filenames[offset];
				offset += strlen(&filenames[offset]) + 1;
			}
		} else if (note_header->n_type == NT_PRSTATUS) {
			++cprm.thread_info_count;
		} else {
			continue;
		}
	}

	if (!cprm.nt_file) {
		fprintf(stderr, "Input corefile does not contain NT_FILE note\n");
		return 1;
	}

	cprm.thread_infos = malloc(sizeof(struct thread_info) * cprm.thread_info_count);
	if (!cprm.thread_infos) {
		perror("malloc");
		return 1;
	}

	size_t thread_info_index = 0;
	for (const Elf64_Nhdr* note_header = cprm.input_notes; note_header < (const Elf64_Nhdr*)((const char*)cprm.input_notes + cprm.input_notes_size); note_header = find_next_note(note_header)) {
		if (note_header->n_type == NT_PRSTATUS) {
			// allocate a copy for alignment purposes
			struct nt_prstatus* prstatus = malloc(note_header->n_descsz);
			if (!prstatus) {
				perror("malloc");
				return 1;
			}
			cprm.thread_infos[thread_info_index++].prstatus = prstatus;
			memcpy(prstatus, note_data(note_header), note_header->n_descsz);
		} else {
			continue;
		}
	}

	// determine if we have extra mappings in NT_FILE that aren't present in the program headers (gcore tends to do this)
	// also try to determine which file is our main executable
	for (size_t i = 0; i < cprm.nt_file->count; ++i) {
		const struct nt_file_entry* entry = &((const struct nt_file_entry*)((const char*)cprm.nt_file + sizeof(struct nt_file_header)))[i];
		const char* filename = cprm.nt_file_filenames[i];
		bool found = false;

		// try to determine if this is the main executable file
		if (strncmp(filename, "/dev/", sizeof("/dev/") - 1) != 0) {
			int tmpfd = open_file(&cprm, filename, strlen(filename));
			if (tmpfd >= 0) {
				struct mach_header_64 mh;
				if (read(tmpfd, &mh, sizeof(mh)) == sizeof(mh) && mh.filetype == MH_EXECUTE) {
					if (cprm.main_executable_path) {
						if (strcmp(cprm.main_executable_path, filename) != 0) {
							printf("Already had main executable (\"%s\") but found another? (\"%s\"). Continuing anyways...\n", cprm.main_executable_path, filename);
							//exit(EXIT_FAILURE);
						}
					} else {
						cprm.main_executable_path = filename;
						cprm.main_executable_path_length = strlen(cprm.main_executable_path);
						printf("Found main executable: %s\n", cprm.main_executable_path);
					}
				}
				close(tmpfd);
			}
		}

		for (const Elf64_Phdr* program_header = cprm.input_program_headers; program_header < cprm.input_program_headers_end; program_header = (const Elf64_Phdr*)((const char*)program_header + cprm.input_header->e_phentsize)) {
			if (program_header->p_type != PT_LOAD) {
				continue;
			}
		
			if (entry->start == program_header->p_vaddr) {
				found = true;
				break;
			}
		}

		if (found) {
			continue;
		}

		++cprm.vm_area_count;

		break;
	}

	// now allocate the VM area array
	cprm.vm_areas = malloc(sizeof(*cprm.vm_areas) * cprm.vm_area_count);
	if (!cprm.vm_areas) {
		perror("malloc");
		return 1;
	}

	memset(cprm.vm_areas, 0, sizeof(*cprm.vm_areas) * cprm.vm_area_count);

	// now load up the VM area array
	size_t vm_area_index = 0;
	for (const Elf64_Phdr* program_header = cprm.input_program_headers; program_header < cprm.input_program_headers_end; program_header = (const Elf64_Phdr*)((const char*)program_header + cprm.input_header->e_phentsize)) {
		if (program_header->p_type == PT_LOAD) {
			struct vm_area* vm_area = &cprm.vm_areas[vm_area_index++];

			vm_area->valid = true;
			vm_area->memory_address = program_header->p_vaddr;
			vm_area->memory_size = program_header->p_memsz;
			vm_area->file_offset = program_header->p_offset;

			if (!cprm.main_executable_path && vm_area->memory_address == 0) {
				// if this is PAGEZERO and we don't yet have a main executable registered,
				// the file containing this segment is most likely the main executable
				cprm.main_executable_path = vm_area->filename;
				cprm.main_executable_path_length = vm_area->filename_length;
				printf("Found main executable (maybe): %s\n", cprm.main_executable_path);
			}

			vm_area->protection = 0;
			if (program_header->p_flags & PF_R) {
				vm_area->protection |= PROT_READ;
			}
			if (program_header->p_flags & PF_W) {
				vm_area->protection |= PROT_WRITE;
			}
			if (program_header->p_flags & PF_X) {
				vm_area->protection |= PROT_EXEC;
			}

			if (program_header->p_filesz == 0) {
				// contents contained within original file

				// in this case, the file size *should* be the same as the size in memory
				vm_area->file_size = vm_area->memory_size;

				// let's look for the NT_FILE entry
				const struct nt_file_entry* entries = (const struct nt_file_entry*)((const char*)cprm.nt_file + sizeof(struct nt_file_header));
				for (size_t i = 0; i < cprm.nt_file->count; ++i) {
					const struct nt_file_entry* entry = &entries[i];
					const char* filename = cprm.nt_file_filenames[i];

					if (entry->start > vm_area->memory_address || entry->end < vm_area->memory_address + vm_area->memory_size) {
						continue;
					}

					vm_area->filename = filename;
					vm_area->filename_length = strlen(vm_area->filename);
					vm_area->file_offset = (entry->offset * cprm.nt_file->page_size) + (vm_area->memory_address - entry->start);
					vm_area->file_size = entry->end - entry->start;

					break;
				}

				if (!vm_area->filename) {
					if (vm_area->protection == 0) {
						// if the area has no protection flags, it most likely wasn't included anywhere.
						// no need to include it in the core dump. this also handles the case of __PAGEZERO.
						vm_area->valid = false;
					}

					// otherwise, it's valid, just not present anywhere.
					// this can occur for pages that are allocated but never accessed, so there would be no reason to include them in the core dump.
					// in this case, the program header literally indicates that the region's file size is 0, since it's unnecessary to include anywhere,
					// not necessarily that it resides in a file.

					vm_area->file_size = 0;
				}
			} else {
				// contents contained within this corefile
				vm_area->filename = NULL;
				vm_area->filename_length = 0;
				vm_area->file_size = program_header->p_filesz;
			}
		}
	}

	// load in extra mappings from NT_FILE
	for (size_t i = 0; i < cprm.nt_file->count; ++i) {
		const struct nt_file_entry* entry = &((const struct nt_file_entry*)((const char*)cprm.nt_file + sizeof(struct nt_file_header)))[i];
		const char* filename = cprm.nt_file_filenames[i];
		bool found = false;

		for (const Elf64_Phdr* program_header = cprm.input_program_headers; program_header < cprm.input_program_headers_end; program_header = (const Elf64_Phdr*)((const char*)program_header + cprm.input_header->e_phentsize)) {
			if (program_header->p_type != PT_LOAD) {
				continue;
			}
		
			if (entry->start == program_header->p_vaddr) {
				found = true;
				break;
			}
		}

		if (found) {
			continue;
		}

		struct vm_area* vm_area = &cprm.vm_areas[vm_area_index++];

		vm_area->filename = filename;
		vm_area->filename_length = strlen(vm_area->filename);
		vm_area->memory_address = entry->start;
		vm_area->memory_size = entry->end - entry->start;
		vm_area->file_size = vm_area->memory_size;
		vm_area->file_offset = entry->offset * cprm.nt_file->page_size;
		vm_area->protection = VM_PROT_READ;
		vm_area->valid = true;

		break;
	}

	macho_coredump(&cprm);

	// now let's clean-up

	for (size_t i = 0; i < cprm.thread_info_count; ++i) {
		free(cprm.thread_infos[i].prstatus);
	}

	free(cprm.thread_infos);
	free(cprm.vm_areas);
	free(cprm.nt_file_filenames);
	free(cprm.nt_file);

	return 0;
};

static bool dump_emit(struct coredump_params* cprm, const void* buffer, size_t buffer_size) {
	return write(cprm->output_corefile, buffer, buffer_size) == buffer_size;
};

static bool dump_skip(struct coredump_params* cprm, size_t size) {
	// leaving holes seems to confuse LLDB
#if 0
	if (lseek(cprm->output_corefile, size, SEEK_CUR) < 0) {
		perror("lseek");
		return false;
	}
#else
	static char zeros[4096];
	while (sizeof(zeros) < size) {
		if (!dump_emit(cprm, zeros, sizeof(zeros)))
			return false;
		size -= sizeof(zeros);
	}
	if (!dump_emit(cprm, zeros, size))
		return false;
#endif
	return true;
};

static uint64_t dump_offset_get(struct coredump_params* cprm) {
	return lseek(cprm->output_corefile, 0, SEEK_CUR);
};

// the following coredump code has been imported from the LKM and adapted for use in userspace

struct thread_flavor
{
	// preceded by struct thread_command and other flavors
	uint32_t flavor;
	uint32_t count;
	char state[0];
	// followed by x86_thread_state32_t, for example
};

static
void fill_thread_state32(x86_thread_state32_t* state, const struct thread_info* info)
{
	// TODO
	memset(state, 0, sizeof(*state));
}

static
void fill_float_state32(x86_float_state32_t* state, const struct thread_info* info)
{
	// TODO
	memset(state, 0, sizeof(*state));
}

static
void fill_thread_state64(x86_thread_state64_t* state, const struct thread_info* info)
{
	state->rax    = info->prstatus->general_registers.ax;
	state->rbx    = info->prstatus->general_registers.bx;
	state->rcx    = info->prstatus->general_registers.cx;
	state->rdx    = info->prstatus->general_registers.dx;
	state->rdi    = info->prstatus->general_registers.di;
	state->rsi    = info->prstatus->general_registers.si;
	state->rbp    = info->prstatus->general_registers.bp;
	state->rsp    = info->prstatus->general_registers.sp;
	state->r8     = info->prstatus->general_registers.r8;
	state->r9     = info->prstatus->general_registers.r9;
	state->r10    = info->prstatus->general_registers.r10;
	state->r11    = info->prstatus->general_registers.r11;
	state->r12    = info->prstatus->general_registers.r12;
	state->r13    = info->prstatus->general_registers.r13;
	state->r14    = info->prstatus->general_registers.r14;
	state->r15    = info->prstatus->general_registers.r15;
	state->rip    = info->prstatus->general_registers.ip;
	state->rflags = info->prstatus->general_registers.flags;
	state->cs     = info->prstatus->general_registers.cs;
	state->fs     = info->prstatus->general_registers.fs;
	state->gs     = info->prstatus->general_registers.gs;
}

static
void fill_float_state64(x86_float_state64_t* state, const struct thread_info* info)
{
	// TODO
	memset(state, 0, sizeof(*state));
}

static
bool macho_dump_headers32(struct coredump_params* cprm)
{
	// Count memory segments and threads
	unsigned int segs = cprm->vm_area_count;
	unsigned int threads = cprm->thread_info_count;

	for (size_t i = 0; i < cprm->vm_area_count; ++i) {
		struct vm_area* vma = &cprm->vm_areas[i];

		if (!vma->valid) {
			// we don't dump inaccessible/invalid regions, so remove them from the count
			--segs;
		}
	}

	struct mach_header mh;

	mh.magic = MH_MAGIC;
#ifdef __x86_64__
	mh.cputype = CPU_TYPE_X86;
	mh.cpusubtype = CPU_SUBTYPE_X86_ALL;
#else
	#error Missing code for this arch
#endif
	mh.filetype = MH_CORE;
	mh.ncmds = segs + threads;

	const int statesize = sizeof(x86_thread_state32_t) + sizeof(x86_float_state32_t) + sizeof(struct thread_flavor) * 2;

	mh.sizeofcmds = segs * sizeof(struct segment_command) + threads * (sizeof(struct thread_command) + statesize);

	if (!dump_emit(cprm, &mh, sizeof(mh)))
		goto fail;

	uint32_t file_offset = mh.sizeofcmds + sizeof(mh);

	// TODO: maybe align the initial offset to 0x1000

	for (size_t i = 0; i < cprm->vm_area_count; ++i) {
		const struct vm_area* vma = &cprm->vm_areas[i];
		struct segment_command sc;

		if (!vma->valid) {
			// ignore inaccessible/invalid regions (like the __PAGEZERO segment, which may be really large)
			continue;
		}

		sc.cmd = LC_SEGMENT;
		sc.cmdsize = sizeof(sc);
		sc.segname[0] = 0;
		sc.nsects = 0;
		sc.flags = 0;
		sc.vmaddr = vma->memory_address;
		sc.vmsize = vma->memory_size;
		sc.fileoff = file_offset;

		sc.filesize = (vma->file_size == 0) ? vma->memory_size : vma->file_size;
		sc.initprot = 0;

		if (vma->protection & PROT_READ)
			sc.initprot |= VM_PROT_READ;
		if (vma->protection & PROT_WRITE)
			sc.initprot |= VM_PROT_WRITE;
		if (vma->protection & PROT_EXEC)
			sc.initprot |= VM_PROT_EXECUTE;
		sc.maxprot = sc.initprot;

		if (!dump_emit(cprm, &sc, sizeof(sc)))
			goto fail;

		file_offset += sc.filesize;
	}

	const int memsize = sizeof(struct thread_command) + statesize;
	uint8_t* buffer = malloc(memsize);

	for (size_t i = 0; i < cprm->thread_info_count; ++i) {
		const struct thread_info* thread_info = &cprm->thread_infos[i];
		struct thread_command* tc = (struct thread_command*) buffer;
		struct thread_flavor* tf = (struct thread_flavor*)(tc+1);

		tc->cmd = LC_THREAD;
		tc->cmdsize = memsize;

		// General registers
		tf->flavor = x86_THREAD_STATE32;
		tf->count = x86_THREAD_STATE32_COUNT;

		fill_thread_state32((x86_thread_state32_t*)tf->state, thread_info);

		// Float registers
		tf = (struct thread_flavor*) (((char*) tf) + sizeof(x86_thread_state32_t));
		tf->flavor = x86_FLOAT_STATE32;
		tf->count = x86_FLOAT_STATE32_COUNT;

		fill_float_state32((x86_float_state32_t*)tf->state, thread_info);

		if (!dump_emit(cprm, buffer, memsize))
		{
			free(buffer);
			goto fail;
		}
	}
	free(buffer);

	return true;
fail:
	return false;
}

static
bool macho_dump_headers64(struct coredump_params* cprm)
{
	// Count memory segments and threads
	unsigned int segs = cprm->vm_area_count;
	unsigned int threads = cprm->thread_info_count;
	struct mach_header_64 mh;

	for (size_t i = 0; i < cprm->vm_area_count; ++i) {
		struct vm_area* vma = &cprm->vm_areas[i];

		if (!vma->valid) {
			// we don't dump inaccessible/invalid regions, so remove them from the count
			--segs;
		}
	}

	mh.magic = MH_MAGIC_64;
#ifdef __x86_64__
	mh.cputype = CPU_TYPE_X86_64;
	mh.cpusubtype = CPU_SUBTYPE_X86_64_ALL;
#else
#warning Missing code for this arch
#endif
	mh.filetype = MH_CORE;
	mh.ncmds = segs + threads;

	const int statesize = sizeof(x86_thread_state64_t) + sizeof(x86_float_state64_t) + sizeof(struct thread_flavor) * 2;
	mh.sizeofcmds = segs * sizeof(struct segment_command_64) + threads * (sizeof(struct thread_command) + statesize);
	mh.flags = 0;
	mh.reserved = 0;

	if (cprm->main_executable_path) {
		int tmpfd = open_file(cprm, cprm->main_executable_path, cprm->main_executable_path_length);
		if (tmpfd >= 0) {
			struct mach_header_64 that_mh;
			if (read(tmpfd, &that_mh, sizeof(that_mh)) == sizeof(that_mh)) {
				mh.flags = that_mh.flags;
			}
			close(tmpfd);
		}
	} else {
		fprintf(stderr, "No main executable detected?\n");
	}

	if (!dump_emit(cprm, &mh, sizeof(mh)))
		goto fail;

	struct vm_area_struct* vma;
	uint64_t file_offset = mh.sizeofcmds + sizeof(mh);

	for (size_t i = 0; i < cprm->vm_area_count; ++i) {
		struct vm_area* vma = &cprm->vm_areas[i];
		struct segment_command_64 sc;

		if (!vma->valid) {
			// ignore inaccessible/invalid regions (like the __PAGEZERO segment, which may be really large)
			continue;
		}

		sc.cmd = LC_SEGMENT_64;
		sc.cmdsize = sizeof(sc);
		sc.segname[0] = 0;
		sc.nsects = 0;
		sc.flags = 0;
		sc.vmaddr = vma->memory_address;
		sc.vmsize = vma->memory_size;
		sc.fileoff = file_offset;

		vma->expected_offset = sc.fileoff;

		sc.filesize = (vma->file_size == 0) ? vma->memory_size : vma->file_size;
		sc.initprot = 0;

		if (vma->protection & PROT_READ)
			sc.initprot |= VM_PROT_READ;
		if (vma->protection & PROT_WRITE)
			sc.initprot |= VM_PROT_WRITE;
		if (vma->protection & PROT_EXEC)
			sc.initprot |= VM_PROT_EXECUTE;
		sc.maxprot = sc.initprot;

		if (!dump_emit(cprm, &sc, sizeof(sc)))
			goto fail;

		file_offset += sc.filesize;
	}

	const int memsize = sizeof(struct thread_command) + statesize;
	uint8_t* buffer = malloc(memsize);

	for (size_t i = 0; i < cprm->thread_info_count; ++i) {
		const struct thread_info* thread_info = &cprm->thread_infos[i];
		struct thread_command* tc = (struct thread_command*) buffer;
		struct thread_flavor* tf = (struct thread_flavor*)(tc+1);

		tc->cmd = LC_THREAD;
		tc->cmdsize = memsize;

		// General registers
		tf->flavor = x86_THREAD_STATE64;
		tf->count = x86_THREAD_STATE64_COUNT;

		fill_thread_state64((x86_thread_state64_t*)tf->state, thread_info);

		// Float registers
		tf = (struct thread_flavor*) (tf->state + sizeof(x86_thread_state64_t));
		tf->flavor = x86_FLOAT_STATE64;
		tf->count = x86_FLOAT_STATE64_COUNT;

		fill_float_state64((x86_float_state64_t*)tf->state, thread_info);

		if (!dump_emit(cprm, buffer, memsize))
		{
			free(buffer);
			goto fail;
		}
	}
	free(buffer);

	return true;
fail:
	return false;
}

void macho_coredump(struct coredump_params* cprm)
{
#warning TODO: 32-bit coredump support
#if TODO
	// Write the Mach-O header and loader commands
	if (!check_64bit_mode(current_pt_regs()))
	{
		// 32-bit executables
		if (!macho_dump_headers32(cprm))
			exit(EXIT_FAILURE);
	}
	else
#endif
	{
		// 64-bit executables
		if (!macho_dump_headers64(cprm))
			exit(EXIT_FAILURE);
	}

	// Dump memory contents

	// Inspired by elf_core_dump()
	for (size_t i = 0; i < cprm->vm_area_count; ++i) {
		const struct vm_area* vma = &cprm->vm_areas[i];
		unsigned long addr;

		if (!vma->valid) {
			continue;
		}

		uint64_t curr = dump_offset_get(cprm);
		if (curr != vma->expected_offset) {
			fprintf(stderr, "Expected offset %lx, got offset %lx\n", vma->expected_offset, curr);
			exit(EXIT_FAILURE);
		}

		if (vma->file_size == 0) {
			// this region needs to be zeroed out
			//
			// it's likely a region that was allocated with mmap but never accessed,
			// so it remained zero-filled and there was no need to include it in the ELF core file.
			if (!dump_skip(cprm, vma->memory_size))
				exit(EXIT_FAILURE);
			continue;
		}

		if (vma->filename) {
			if (strncmp(vma->filename, "/dev/", sizeof("/dev/") - 1) == 0) {
				printf("Warning: skipping device \"%s\"\n", vma->filename);
				if (!dump_skip(cprm, vma->file_size)) {
					exit(EXIT_FAILURE);
				}
				continue;
			}

			int fd = open_file(cprm, vma->filename, vma->filename_length);
			if (fd < 0) {
				fprintf(stderr, "Warning: failed to open %s: %d (%s)\n", vma->filename, errno, strerror(errno));
				//exit(EXIT_FAILURE);
				// just zero it out
				if (!dump_skip(cprm, vma->file_size)) {
					exit(EXIT_FAILURE);
				}
				continue;
			}

			uintptr_t aligned_offset = round_down_pow2(vma->file_offset, sysconf(_SC_PAGESIZE));
			size_t aligned_size = round_up_pow2(vma->file_size + (vma->file_offset - aligned_offset), sysconf(_SC_PAGESIZE));
			void* mapping = mmap(NULL, aligned_size, PROT_READ, MAP_PRIVATE, fd, aligned_offset);
			if (mapping == MAP_FAILED) {
				perror("mmap");
				exit(EXIT_FAILURE);
			}
			const void* start = (const char*)mapping + (vma->file_offset - aligned_offset);

			if (!dump_emit(cprm, start, vma->file_size))
				exit(EXIT_FAILURE);

			if (munmap(mapping, aligned_size) < 0) {
				perror("munmap");
				exit(EXIT_FAILURE);
			}

			close(fd);
		} else {
			if (!dump_emit(cprm, (const char*)cprm->input_corefile_mapping + vma->file_offset, vma->file_size))
				exit(EXIT_FAILURE);
		}
	}
}
