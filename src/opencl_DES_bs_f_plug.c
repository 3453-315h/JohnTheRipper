/*
 * This software is Copyright (c) 2012 Sayantan Datta <std2048 at gmail dot com>
 * and it is hereby released to the general public under the following terms:
 * Redistribution and use in source and binary forms, with or without modification, are permitted.
 * Based on Solar Designer implementation of DES_bs_b.c in jtr-v1.7.9
 */

#ifdef HAVE_OPENCL

#include <assert.h>
#include <string.h>
#include <sys/time.h>

#include "options.h"
#include "opencl_DES_bs.h"
#include "opencl_DES_hst_dev_shared.h"
#include "memdbg.h"

#define PADDING 	2048

static cl_kernel **kernels;
static cl_mem buffer_raw_keys, buffer_bs_keys, buffer_unchecked_hashes;
static WORD *marked_salts = NULL, current_salt = 0;
static unsigned int *processed_salts = NULL;
static unsigned int save_binary = 1;
static int num_compiled_salt = 0;

static int des_crypt_25(int *pcount, struct db_salt *salt);

static void create_clobj_kpc(size_t gws)
{
	opencl_DES_bs_all = (opencl_DES_bs_combined *) mem_alloc((gws + PADDING) * sizeof(opencl_DES_bs_combined));
	opencl_DES_bs_keys = (opencl_DES_bs_transfer *) mem_alloc((gws + PADDING) * sizeof(opencl_DES_bs_transfer));

	buffer_raw_keys = clCreateBuffer(context[gpu_id], CL_MEM_READ_ONLY, (gws + PADDING) * sizeof(opencl_DES_bs_transfer), NULL, &ret_code);
	HANDLE_CLERROR(ret_code, "Create buffer_raw_keys failed.\n");

	buffer_bs_keys = clCreateBuffer(context[gpu_id], CL_MEM_READ_WRITE, (gws + PADDING) * sizeof(DES_bs_vector) * 56, NULL, &ret_code);
	HANDLE_CLERROR(ret_code, "Create buffer_bs_keys failed.\n");

	buffer_unchecked_hashes = clCreateBuffer(context[gpu_id], CL_MEM_READ_WRITE, (gws + PADDING) * sizeof(DES_bs_vector) * 64, NULL, &ret_code);
	HANDLE_CLERROR(ret_code, "Create buffer_unchecked_hashes failed.\n");
}

static void release_clobj_kpc()
{
	if (buffer_raw_keys != (cl_mem)0) {
		MEM_FREE(opencl_DES_bs_all);
		MEM_FREE(opencl_DES_bs_keys);
		HANDLE_CLERROR(clReleaseMemObject(buffer_raw_keys), "Release buffer_raw_keys failed.\n");
		HANDLE_CLERROR(clReleaseMemObject(buffer_bs_keys), "Release buffer_bs_keys failed.\n");
		HANDLE_CLERROR(clReleaseMemObject(buffer_unchecked_hashes), "Release buffer_unchecked_hashes failed.\n");
		buffer_raw_keys = (cl_mem)0;
	}
}

static void create_clobj(struct db_main *db)
{
	int i;

	marked_salts = (WORD *) mem_alloc(4096 * sizeof(WORD));

	for (i = 0; i < 4096; i++)
		marked_salts[i] = 0x7fffffff;

	build_tables(db);
}

static void release_clobj()
{
	int i;

	if (marked_salts) {
		MEM_FREE(marked_salts);
		release_tables();
		marked_salts = 0;
	}

	for (i = 0; i < 4096; i++)
		if (kernels[gpu_id][i]) {
			HANDLE_CLERROR(clReleaseKernel(kernels[gpu_id][i]), "Release kernel(crypt(i)) failed.\n");
			kernels[gpu_id][i] = 0;
		}
}

static void clean_all_buffers()
{
	int i;

	release_clobj();
	release_clobj_kpc();

	for( i = 0; i < 4098; i++)
		if (kernels[gpu_id][i])
		HANDLE_CLERROR(clReleaseKernel(kernels[gpu_id][i]), "Error releasing kernel");

	HANDLE_CLERROR(clReleaseProgram(program[gpu_id]), "Error releasing Program");

	for (i = 0; i < MAX_GPU_DEVICES; i++)
		MEM_FREE(kernels[i]);

	MEM_FREE(kernels);
	MEM_FREE(num_uncracked_hashes);
	MEM_FREE(processed_salts);
}

/* First call must use salt = 0, to initialize processed_salts. */
static void build_salt(WORD salt)
{
	WORD new;
	static WORD old = 0xffffff;
	int dst;

	new = salt;
	for (dst = 0; dst < 24; dst++) {
		if ((new ^ old) & 1) {
			DES_bs_vector sp1, sp2;
			int src1 = dst;
			int src2 = dst + 24;
			if (new & 1) {
				src1 = src2;
				src2 = dst;
			}
			sp1 = opencl_DES_E[src1];
			sp2 = opencl_DES_E[src2];
			processed_salts[4096 * 96 + dst] = sp1;
			processed_salts[4096 * 96 + dst + 24] = sp2;
			processed_salts[4096 * 96 + dst + 48] = sp1 + 32;
			processed_salts[4096 * 96 + dst + 72] = sp2 + 32;
		}
		new >>= 1;
		old >>= 1;
		if (new == old)
			break;
	}
	old = salt;
	memcpy(&processed_salts[salt * 96], &processed_salts[4096 * 96], 96 * sizeof(unsigned int));
}

static void reset(struct db_main *db)
{
	static int initialized;

	if (initialized) {
		int i;

		release_clobj_kpc();
		release_clobj();
		memset(num_uncracked_hashes, 0, 4096 * sizeof(unsigned int));


		create_clobj(db);
		create_clobj_kpc(global_work_size);

		HANDLE_CLERROR(clSetKernelArg(kernels[gpu_id][4096], 0, sizeof(cl_mem), &buffer_raw_keys), "Failed setting kernel argument buffer_raw_keys, kernel DES_bs_finalize_keys.\n");
		HANDLE_CLERROR(clSetKernelArg(kernels[gpu_id][4096], 1, sizeof(cl_mem), &buffer_bs_keys), "Failed setting kernel argument buffer_bs_keys, kernel DES_bs_finalize_keys.\n");

		HANDLE_CLERROR(clSetKernelArg(kernels[gpu_id][4097], 0, sizeof(cl_mem), &buffer_unchecked_hashes), "Failed setting kernel argument buffer_unchecked_hashes, kernel DES_bs_cmp.\n");
		HANDLE_CLERROR(clSetKernelArg(kernels[gpu_id][4097], 3, sizeof(cl_mem), &buffer_hash_ids), "Failed setting kernel argument buffer_hash_ids, kernel DES_bs_cmp.\n");
		HANDLE_CLERROR(clSetKernelArg(kernels[gpu_id][4097], 4, sizeof(cl_mem), &buffer_bitmap_dupe), "Failed setting kernel argument buffer_bitmap_dupe, kernel DES_bs_cmp.\n");
		HANDLE_CLERROR(clSetKernelArg(kernels[gpu_id][4097], 5, sizeof(cl_mem), &buffer_cracked_hashes), "Failed setting kernel argument buffer_cracked_hashes, kernel DES_bs_cmp.\n");

		for (i = 0; i < global_work_size; i++)
		opencl_DES_bs_init(i);
	}
	else {
		int i;

		opencl_prepare_dev(gpu_id);

		opencl_read_source("$JOHN/kernels/DES_bs_finalize_keys_kernel.cl");
		opencl_build(gpu_id, NULL, 0, NULL);
		kernels[gpu_id][4096] = clCreateKernel(program[gpu_id], "DES_bs_finalize_keys", &ret_code);
		HANDLE_CLERROR(ret_code, "Failed creating kernel DES_bs_finalize_keys.\n");

		kernels[gpu_id][4097] = clCreateKernel(program[gpu_id], "DES_bs_cmp", &ret_code);
		HANDLE_CLERROR(ret_code, "Failed creating kernel DES_bs_cmp.\n");

		local_work_size = 64;
		global_work_size = 131072;

		fmt_opencl_DES.params.max_keys_per_crypt = global_work_size * DES_BS_DEPTH;
		fmt_opencl_DES.params.min_keys_per_crypt = local_work_size * DES_BS_DEPTH;

		create_clobj(NULL);
		create_clobj_kpc(global_work_size);

		HANDLE_CLERROR(clSetKernelArg(kernels[gpu_id][4096], 0, sizeof(cl_mem), &buffer_raw_keys), "Failed setting kernel argument buffer_raw_keys, kernel DES_bs_finalize_keys.\n");
		HANDLE_CLERROR(clSetKernelArg(kernels[gpu_id][4096], 1, sizeof(cl_mem), &buffer_bs_keys), "Failed setting kernel argument buffer_bs_keys, kernel DES_bs_finalize_keys.\n");

		HANDLE_CLERROR(clSetKernelArg(kernels[gpu_id][4097], 0, sizeof(cl_mem), &buffer_unchecked_hashes), "Failed setting kernel argument buffer_unchecked_hashes, kernel DES_bs_cmp.\n");
		HANDLE_CLERROR(clSetKernelArg(kernels[gpu_id][4097], 3, sizeof(cl_mem), &buffer_hash_ids), "Failed setting kernel argument buffer_hash_ids, kernel DES_bs_cmp.\n");
		HANDLE_CLERROR(clSetKernelArg(kernels[gpu_id][4097], 4, sizeof(cl_mem), &buffer_bitmap_dupe), "Failed setting kernel argument buffer_bitmap_dupe, kernel DES_bs_cmp.\n");
		HANDLE_CLERROR(clSetKernelArg(kernels[gpu_id][4097], 5, sizeof(cl_mem), &buffer_cracked_hashes), "Failed setting kernel argument buffer_cracked_hashes, kernel DES_bs_cmp.\n");

		for (i = 0; i < global_work_size; i++)
			opencl_DES_bs_init(i);

		for (i = 0; i < 4096; i++)
			build_salt((WORD)i);

		initialized++;
	}
}

static void init_global_variables()
{
	int i;

	processed_salts = (unsigned int *) mem_calloc(4097, 96 * sizeof(unsigned int));

	kernels = (cl_kernel **) mem_calloc(MAX_GPU_DEVICES, sizeof(cl_kernel *));
	for (i = 0; i < MAX_GPU_DEVICES; i++)
		kernels[i] = (cl_kernel *) mem_calloc(4098, sizeof(cl_kernel));

	num_uncracked_hashes = (unsigned int *) mem_calloc(4096, sizeof(unsigned int));
}

static void modify_src()
{
	  int i = 53, j = 1, tmp;
	  static char digits[10] = {'0','1','2','3','4','5','6','7','8','9'} ;
	  static unsigned int  index[48]  = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,
					     24,25,26,27,28,29,30,31,32,33,34,35,
					     48,49,50,51,52,53,54,55,56,57,58,59,
					     72,73,74,75,76,77,78,79,80,81,82,83 } ;
	  for (j = 1; j <= 48; j++) {
		tmp = processed_salts[current_salt * 96 + index[j - 1]] / 10;
		if (tmp == 0)
			kernel_source[i + j * 17] = ' ' ;
		else
			kernel_source[i + j * 17] = digits[tmp];
		tmp = processed_salts[current_salt * 96 + index[j - 1]] % 10;
	     ++i;
	     kernel_source[i + j * 17 ] = digits[tmp];
	     ++i;
	  }
}

static void set_salt(void *salt)
{
	current_salt = *(WORD *)salt;
}

static char *get_key(int index)
{
	static char out[PLAINTEXT_LENGTH + 1];
	unsigned int section, block;
	unsigned char *src;
	char *dst;

	if (hash_ids == NULL || hash_ids[0] == 0 ||
	    index > 32 * hash_ids[0] || hash_ids[0] > num_uncracked_hashes[current_salt])
		section = 0;
	else
		section = hash_ids[2 * (index/DES_BS_DEPTH) + 1];

	if (section > global_work_size) {
		fprintf(stderr, "Get key error! %d %zu\n", section,
			global_work_size);
		section = 0;
	}
	block  = index % DES_BS_DEPTH;

	src = opencl_DES_bs_all[section].pxkeys[block];
	dst = out;
	while (dst < &out[PLAINTEXT_LENGTH] && (*dst = *src)) {
		src += sizeof(DES_bs_vector) * 8;
		dst++;
	}
	*dst = 0;

	return out;
}

static void modify_build_save_restore(WORD cur_salt, int id_gpu) {
	char kernel_bin_name[200];
	FILE *file;

	sprintf(kernel_bin_name, "$JOHN/kernels/DES_bs_kernel_f_%d_%d.bin", cur_salt, id_gpu);

	file = fopen(path_expand(kernel_bin_name), "r");

	if (file == NULL) {
		char *build_opt = "-fno-bin-amdil -fno-bin-source -fbin-exe";
		opencl_read_source("$JOHN/kernels/DES_bs_kernel_f.cl");
		modify_src();
		if (get_platform_vendor_id(get_platform_id(id_gpu)) != DEV_AMD)
			build_opt = NULL;
		opencl_build(id_gpu, build_opt, save_binary, kernel_bin_name);
		fprintf(stderr, "Salt compiled from Source:%d\n", ++num_compiled_salt);
	}
	else {
		fclose(file);
		opencl_read_source(kernel_bin_name);
		opencl_build_from_binary(id_gpu);
		fprintf(stderr, "Salt compiled from Binary:%d\n", ++num_compiled_salt);
	}
}

static int des_crypt_25(int *pcount, struct db_salt *salt)
{
	const int count = (*pcount + DES_BS_DEPTH - 1) >> DES_LOG_DEPTH;
	size_t *lws = local_work_size ? &local_work_size : NULL;
	size_t current_gws = local_work_size ? (count + local_work_size - 1) / local_work_size * local_work_size : count;

	if (marked_salts[current_salt] != current_salt) {
		modify_build_save_restore(current_salt, gpu_id);

		kernels[gpu_id][current_salt] = clCreateKernel(program[gpu_id], "DES_bs_25", &ret_code);
		HANDLE_CLERROR(ret_code, "Create Kernel DES_bs_25 failed.\n");

		HANDLE_CLERROR(clSetKernelArg(kernels[gpu_id][current_salt], 0, sizeof(cl_mem), &buffer_bs_keys), "Failed setting kernel argument buffer_bs_keys, kernel DES_bs_25.\n");
		HANDLE_CLERROR(clSetKernelArg(kernels[gpu_id][current_salt], 1, sizeof(cl_mem), &buffer_unchecked_hashes), "Failed setting kernel argument buffer_unchecked_hashes, kernel DES_bs_25.\n");

		marked_salts[current_salt] = current_salt;
	}


	if (opencl_DES_bs_keys_changed) {
		HANDLE_CLERROR(clEnqueueWriteBuffer(queue[gpu_id], buffer_raw_keys, CL_TRUE, 0, current_gws * sizeof(opencl_DES_bs_transfer), opencl_DES_bs_keys, 0, NULL, NULL ), "Failed to write buffer buffer_raw_keys.\n");

		ret_code = clEnqueueNDRangeKernel(queue[gpu_id], kernels[gpu_id][4096], 1, NULL, &current_gws, lws, 0, NULL, NULL);
		HANDLE_CLERROR(ret_code, "Enque kernel DES_bs_finalize_keys failed.\n");

		opencl_DES_bs_keys_changed = 0;
	}

	if (salt && num_uncracked_hashes[current_salt] != salt -> count)
		update_buffer(salt);


	HANDLE_CLERROR(clSetKernelArg(kernels[gpu_id][4097], 1, sizeof(cl_mem), &buffer_uncracked_hashes[current_salt]), "Failed setting kernel argument buffer_uncracked_hashes, kernel DES_bs_25.\n");
	HANDLE_CLERROR(clSetKernelArg(kernels[gpu_id][4097], 2, sizeof(int), &num_uncracked_hashes[current_salt]), "Failed setting kernel argument num_uncracked_hashes, kernel DES_bs_25.\n");

	ret_code = clEnqueueNDRangeKernel(queue[gpu_id], kernels[gpu_id][current_salt], 1, NULL, &current_gws, lws, 0, NULL, NULL);
	HANDLE_CLERROR(ret_code, "Enque kernel DES_bs_25 failed.\n");

	ret_code = clEnqueueNDRangeKernel(queue[gpu_id], kernels[gpu_id][4097], 1, NULL, &current_gws, lws, 0, NULL, NULL);
	HANDLE_CLERROR(ret_code, "Enque kernel DES_bs_cmp failed.\n");

	HANDLE_CLERROR(clEnqueueReadBuffer(queue[gpu_id], buffer_hash_ids, CL_TRUE, 0, sizeof(unsigned int), hash_ids, 0, NULL, NULL), "Failed to read buffer buffer_hash_ids.\n");

	if (hash_ids[0] > num_uncracked_hashes[current_salt]) {
		fprintf(stderr, "Error, crypt_all kernel.\n");
		error();
	}

	if (hash_ids[0]) {
		HANDLE_CLERROR(clEnqueueReadBuffer(queue[gpu_id], buffer_hash_ids, CL_TRUE, 0, (2 * num_uncracked_hashes[current_salt] + 1) * sizeof(unsigned int), hash_ids, 0, NULL, NULL), "Failed to read buffer buffer_hash_ids.\n");
		HANDLE_CLERROR(clEnqueueReadBuffer(queue[gpu_id], buffer_cracked_hashes, CL_TRUE, 0, hash_ids[0] * 64 * sizeof(DES_bs_vector), opencl_DES_bs_cracked_hashes, 0, NULL, NULL), "Failed to read buffer buffer_cracked_hashes.\n");
		HANDLE_CLERROR(clEnqueueWriteBuffer(queue[gpu_id], buffer_bitmap_dupe, CL_TRUE, 0, ((num_uncracked_hashes[current_salt] - 1)/32 + 1) * sizeof(cl_uint), zero_buffer, 0, NULL, NULL), "Failed to write buffer buffer_bitmap_dupe.\n");
		HANDLE_CLERROR(clEnqueueWriteBuffer(queue[gpu_id], buffer_hash_ids, CL_TRUE, 0, sizeof(cl_uint), zero_buffer, 0, NULL, NULL), "Failed to write buffer buffer_hash_ids.\n");
	}

	return 32 * hash_ids[0];
}

void opencl_DES_bs_f_register_functions(struct fmt_main *fmt)
{
	fmt -> methods.done = &clean_all_buffers;
	fmt -> methods.reset = &reset;
	fmt -> methods.set_salt = &set_salt;
	fmt -> methods.get_key = &get_key;
	fmt -> methods.crypt_all = &des_crypt_25;

	opencl_DES_bs_init_global_variables = &init_global_variables;
}
#endif /* HAVE_OPENCL */
