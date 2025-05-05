/*
 * Copyright (C) 2019 Intel Corporation.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <stdbool.h>

#include "Enclave_t.h"
#include "wasm_export.h"
#include "bh_platform.h"

extern "C" {
typedef int (*os_print_function_t)(const char *message);
extern void os_set_print_function(os_print_function_t pf);

int enclave_print(const char *message);
}

/* Defined in Enclave.cpp */
extern bool runtime_inited;

/* ---------------------------------------------------------------------
 *  Persistent module wrapper so we can free the copied-in WASM bytes
 *  together with the WAMR module.
 * -------------------------------------------------------------------*/
typedef struct {
    wasm_module_t   mod;        /* real WAMR module                 */
    uint8_t        *wasm_buf;   /* heap copy of the original bytes  */
    uint32_t        wasm_size;  /* just for completeness/debug      */
} enclave_module_t;

/* Value type for Rust FFI */
typedef enum {
    WAMR_SGX_VAL_TYPE_I32 = 0,
    WAMR_SGX_VAL_TYPE_I64 = 1,
    WAMR_SGX_VAL_TYPE_F32 = 2,
    WAMR_SGX_VAL_TYPE_F64 = 3,
    WAMR_SGX_VAL_TYPE_BUFFER = 4
} wamr_sgx_val_type_t;

/* Value structure for passing arguments and receiving results */
typedef struct {
    wamr_sgx_val_type_t type;
    union {
        int32_t i32;
        int64_t i64;
        float f32;
        double f64;
        struct {
            void* data;
            size_t size;
        } buffer;
    } value;
} wamr_sgx_val_t;

/* Implementation of new ECALLs for Rust FFI library interface */

int 
ecall_init_runtime(uint32_t max_thread_num)
{
    RuntimeInitArgs init_args;
    
    /* avoid duplicated init */
    if (runtime_inited) {
        return 1;
    }

    os_set_print_function(enclave_print);

    memset(&init_args, 0, sizeof(RuntimeInitArgs));
    init_args.max_thread_num = max_thread_num;

#if WASM_ENABLE_GLOBAL_HEAP_POOL != 0
    extern char global_heap_buf[];
    init_args.mem_alloc_type = Alloc_With_Pool;
    init_args.mem_alloc_option.pool.heap_buf = global_heap_buf;
    init_args.mem_alloc_option.pool.heap_size = sizeof(global_heap_buf);
#else
    init_args.mem_alloc_type = Alloc_With_System_Allocator;
#endif

    /* initialize runtime environment */
    if (!wasm_runtime_full_init(&init_args)) {
        LOG_ERROR("Init runtime environment failed.\n");
        return -1;
    }

    runtime_inited = true;
    return 0;
}

int
ecall_destroy_runtime()
{
    if (!runtime_inited) {
        return 0;
    }

    wasm_runtime_destroy();
    runtime_inited = false;
    return 0;
}

int
ecall_load_module(uint8_t  *wasm_buffer,
                  uint32_t  wasm_buffer_size,
                  char     *error_buf,
                  uint32_t  error_buf_size,
                  uint64_t *module_handle)
{
    if (!runtime_inited) {
        snprintf(error_buf, error_buf_size, "Runtime not initialized");
        return -1;
    }

    /* ---------------- 2a.  Make a persistent copy ------------------ */
    uint8_t *buf_copy = (uint8_t *)wasm_runtime_malloc(wasm_buffer_size);
    if (!buf_copy) {
        snprintf(error_buf, error_buf_size, "Out of enclave memory");
        return -1;
    }
    memcpy(buf_copy, wasm_buffer, wasm_buffer_size);

    /* ---------------- 2b.  Load the module from that copy ---------- */
    wasm_module_t mod =
        wasm_runtime_load(buf_copy, wasm_buffer_size, error_buf, error_buf_size);
    if (!mod) {
        wasm_runtime_free(buf_copy);           /* rollback */
        return -1;
    }

    /* ---------------- 2c.  Wrap everything & hand it back ---------- */
    enclave_module_t *wrapper =
        (enclave_module_t *)wasm_runtime_malloc(sizeof(enclave_module_t));
    if (!wrapper) {
        wasm_runtime_unload(mod);
        wasm_runtime_free(buf_copy);
        snprintf(error_buf, error_buf_size, "Out of enclave memory");
        return -1;
    }

    wrapper->mod       = mod;
    wrapper->wasm_buf  = buf_copy;
    wrapper->wasm_size = wasm_buffer_size;

    *module_handle = (uint64_t)(uintptr_t)wrapper;
    return 0;
}

int
ecall_unload_module(uint64_t module_handle)
{
    enclave_module_t *wrapper = (enclave_module_t *)(uintptr_t)module_handle;
    if (!runtime_inited || !wrapper)
        return -1;

    /* Unload the WAMR module itself */
    wasm_runtime_unload(wrapper->mod);

    /* Free the persistent copy of the WASM file */
    wasm_runtime_free(wrapper->wasm_buf);

    /* …and finally the wrapper */
    wasm_runtime_free(wrapper);
    return 0;
}

int
ecall_instantiate_module(uint64_t module_handle,
                      uint32_t stack_size,
                      uint32_t heap_size,
                      char *error_buf,
                      uint32_t error_buf_size,
                      uint64_t *instance_handle)
{
    enclave_module_t *wrapper = (enclave_module_t *)(uintptr_t)module_handle;
    wasm_module_t     module  = wrapper->mod;
    wasm_module_inst_t module_inst;
    
    if (!runtime_inited || !module) {
        snprintf(error_buf, error_buf_size, "Invalid module or runtime not initialized");
        return -1;
    }
    
    if (!(module_inst = wasm_runtime_instantiate(module, stack_size, heap_size,
                                               error_buf, error_buf_size))) {
        return -1;
    }
    
    *instance_handle = (uint64_t)(uintptr_t)module_inst;
    return 0;
}

int
ecall_destroy_instance(uint64_t instance_handle)
{
    wasm_module_inst_t module_inst = (wasm_module_inst_t)(uintptr_t)instance_handle;
    
    if (!runtime_inited || !module_inst) {
        return -1;
    }
    
    wasm_runtime_deinstantiate(module_inst);
    return 0;
}

int
ecall_call_function(uint64_t instance_handle,
                 const char *function_name,
                 uint8_t *params,
                 uint32_t params_size,
                 uint8_t *results,
                 uint32_t results_size,
                 char *error_buf,
                 uint32_t error_buf_size)
{
    wasm_module_inst_t module_inst = (wasm_module_inst_t)(uintptr_t)instance_handle;
    wasm_exec_env_t exec_env = NULL;
    wasm_function_inst_t func = NULL;
    wamr_sgx_val_t *param_values = NULL, *result_values = NULL;
    wasm_val_t *wasm_params = NULL, *wasm_results = NULL;
    uint32_t param_count = 0, result_count = 0, i;
    bool ret = false;
    
    void *stack_ptr = __builtin_frame_address(0);
    enclave_print("[SGX Enclave] ecall_call_function entry\n");
    
    if (!runtime_inited || !module_inst) {
        if (error_buf && error_buf_size > 0) {
            const char *error_msg = "Invalid module instance or runtime not initialized";
            size_t msg_len = strlen(error_msg);
            size_t copy_len = (msg_len < error_buf_size - 1) ? msg_len : error_buf_size - 1;
            memcpy(error_buf, error_msg, copy_len);
            error_buf[copy_len] = '\0';
        }
        return -1;
    }
    
    param_values = (wamr_sgx_val_t*)params;
    param_count = params_size / sizeof(wamr_sgx_val_t);
    result_values = (wamr_sgx_val_t*)results;
    result_count = results_size / sizeof(wamr_sgx_val_t);
    
    enclave_print("[SGX Enclave] Looking up function\n");
    
    char *func_name_copy = NULL;
    size_t func_name_len = strlen(function_name);
    
    func_name_copy = (char*)wasm_runtime_malloc(func_name_len + 1);
    if (!func_name_copy) {
        if (error_buf && error_buf_size > 0) {
            const char *error_msg = "Failed to allocate memory for function name";
            size_t msg_len = strlen(error_msg);
            size_t copy_len = (msg_len < error_buf_size - 1) ? msg_len : error_buf_size - 1;
            memcpy(error_buf, error_msg, copy_len);
            error_buf[copy_len] = '\0';
        }
        return -1;
    }
    
    memcpy(func_name_copy, function_name, func_name_len + 1);

    enclave_print("[SGX Enclave] calling wasm_runtime_lookup_function\n");
    func = wasm_runtime_lookup_function(module_inst, func_name_copy);
    wasm_runtime_free(func_name_copy);
    
    if (!func) {
        if (error_buf && error_buf_size > 0) {
            const char *error_msg = "Function not found";
            size_t msg_len = strlen(error_msg);
            size_t copy_len = (msg_len < error_buf_size - 1) ? msg_len : error_buf_size - 1;
            memcpy(error_buf, error_msg, copy_len);
            error_buf[copy_len] = '\0';
        }
        return -1;
    }
    
    enclave_print("[SGX Enclave] Creating execution environment\n");
    
    if (!(exec_env = wasm_runtime_create_exec_env(module_inst, 256 * 1024))) {
        if (error_buf && error_buf_size > 0) {
            const char *error_msg = "Failed to create execution environment";
            size_t msg_len = strlen(error_msg);
            size_t copy_len = (msg_len < error_buf_size - 1) ? msg_len : error_buf_size - 1;
            memcpy(error_buf, error_msg, copy_len);
            error_buf[copy_len] = '\0';
        }
        return -1;
    }
    
    enclave_print("[SGX Enclave] Setting up parameters\n");
    
    if (param_count > 0) {
        wasm_params = (wasm_val_t*)wasm_runtime_malloc(sizeof(wasm_val_t) * param_count);
        if (!wasm_params) {
            if (error_buf && error_buf_size > 0) {
                const char *error_msg = "Failed to allocate memory for parameters";
                size_t msg_len = strlen(error_msg);
                size_t copy_len = (msg_len < error_buf_size - 1) ? msg_len : error_buf_size - 1;
                memcpy(error_buf, error_msg, copy_len);
                error_buf[copy_len] = '\0';
            }
            wasm_runtime_destroy_exec_env(exec_env);
            return -1;
        }
        
        memset(wasm_params, 0, sizeof(wasm_val_t) * param_count);
        
        for (i = 0; i < param_count; i++) {
            switch (param_values[i].type) {
                case WAMR_SGX_VAL_TYPE_I32:
                    wasm_params[i].kind = WASM_I32;
                    wasm_params[i].of.i32 = param_values[i].value.i32;
                    break;
                case WAMR_SGX_VAL_TYPE_I64:
                    wasm_params[i].kind = WASM_I64;
                    wasm_params[i].of.i64 = param_values[i].value.i64;
                    break;
                case WAMR_SGX_VAL_TYPE_F32:
                    wasm_params[i].kind = WASM_F32;
                    wasm_params[i].of.f32 = param_values[i].value.f32;
                    break;
                case WAMR_SGX_VAL_TYPE_F64:
                    wasm_params[i].kind = WASM_F64;
                    wasm_params[i].of.f64 = param_values[i].value.f64;
                    break;
                default:
                    if (error_buf && error_buf_size > 0) {
                        const char *error_msg = "Unsupported parameter type";
                        size_t msg_len = strlen(error_msg);
                        size_t copy_len = (msg_len < error_buf_size - 1) ? msg_len : error_buf_size - 1;
                        memcpy(error_buf, error_msg, copy_len);
                        error_buf[copy_len] = '\0';
                    }
                    wasm_runtime_free(wasm_params);
                    wasm_runtime_destroy_exec_env(exec_env);
                    return -1;
            }
        }
    }
    
    enclave_print("[SGX Enclave] Setting up result buffer\n");
    
    if (result_count > 0) {
        wasm_results = (wasm_val_t*)wasm_runtime_malloc(sizeof(wasm_val_t) * result_count);
        if (!wasm_results) {
            if (error_buf && error_buf_size > 0) {
                const char *error_msg = "Failed to allocate memory for results";
                size_t msg_len = strlen(error_msg);
                size_t copy_len = (msg_len < error_buf_size - 1) ? msg_len : error_buf_size - 1;
                memcpy(error_buf, error_msg, copy_len);
                error_buf[copy_len] = '\0';
            }
            if (wasm_params)
                wasm_runtime_free(wasm_params);
            wasm_runtime_destroy_exec_env(exec_env);
            return -1;
        }
        
        memset(wasm_results, 0, sizeof(wasm_val_t) * result_count);
    }
    
    enclave_print("[SGX Enclave] Calling WASM function\n");
    
    ret = wasm_runtime_call_wasm_a(exec_env, func, result_count, wasm_results, param_count, wasm_params);
    
    if (ret) {
        enclave_print("[SGX Enclave] Function call succeeded\n");
    }
    else {
        enclave_print("[SGX Enclave] Function call failed\n");
    }
    
    if (!ret) {
        const char *exception = wasm_runtime_get_exception(module_inst);
        if (exception && error_buf && error_buf_size > 0) {
            size_t exc_len = strlen(exception);
            size_t copy_len = (exc_len < error_buf_size - 1) ? exc_len : error_buf_size - 1;
            memcpy(error_buf, exception, copy_len);
            error_buf[copy_len] = '\0';
            
            enclave_print("[SGX Enclave] Exception occurred\n");
        } 
        else if (error_buf && error_buf_size > 0) {
            const char *error_msg = "Unknown error calling function";
            size_t msg_len = strlen(error_msg);
            size_t copy_len = (msg_len < error_buf_size - 1) ? msg_len : error_buf_size - 1;
            memcpy(error_buf, error_msg, copy_len);
            error_buf[copy_len] = '\0';
            
            enclave_print("[SGX Enclave] Unknown error calling function\n");
        }
        
        if (wasm_params)
            wasm_runtime_free(wasm_params);
        if (wasm_results)
            wasm_runtime_free(wasm_results);
        wasm_runtime_destroy_exec_env(exec_env);
        return -1;
    }
    
    enclave_print("[SGX Enclave] Processing results\n");
    
    for (i = 0; i < result_count; i++) {
        switch (wasm_results[i].kind) {
            case WASM_I32:
                result_values[i].type = WAMR_SGX_VAL_TYPE_I32;
                result_values[i].value.i32 = wasm_results[i].of.i32;
                break;
            case WASM_I64:
                result_values[i].type = WAMR_SGX_VAL_TYPE_I64;
                result_values[i].value.i64 = wasm_results[i].of.i64;
                break;
            case WASM_F32:
                result_values[i].type = WAMR_SGX_VAL_TYPE_F32;
                result_values[i].value.f32 = wasm_results[i].of.f32;
                break;
            case WASM_F64:
                result_values[i].type = WAMR_SGX_VAL_TYPE_F64;
                result_values[i].value.f64 = wasm_results[i].of.f64;
                break;
            default:
                if (error_buf && error_buf_size > 0) {
                    const char *error_msg = "Unexpected result type";
                    size_t msg_len = strlen(error_msg);
                    size_t copy_len = (msg_len < error_buf_size - 1) ? msg_len : error_buf_size - 1;
                    memcpy(error_buf, error_msg, copy_len);
                    error_buf[copy_len] = '\0';
                }
                break;
        }
    }
    
    enclave_print("[SGX Enclave] Cleanup and exit\n");
    
    if (wasm_params)
        wasm_runtime_free(wasm_params);
    if (wasm_results)
        wasm_runtime_free(wasm_results);
    wasm_runtime_destroy_exec_env(exec_env);
    return 0;
}