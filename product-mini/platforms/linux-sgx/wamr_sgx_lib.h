/*
 * Copyright (C) 2019 Intel Corporation.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#ifndef WAMR_SGX_LIB_H
#define WAMR_SGX_LIB_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct WamrSgxContext* wamr_sgx_context_t;
typedef struct WamrSgxModule* wamr_sgx_module_t;
typedef struct WamrSgxInstance* wamr_sgx_instance_t;

typedef enum {
    WAMR_SGX_SUCCESS = 0,
    WAMR_SGX_ERROR_ENCLAVE_INIT = -1,
    WAMR_SGX_ERROR_RUNTIME_INIT = -2,
    WAMR_SGX_ERROR_LOAD_MODULE = -3,
    WAMR_SGX_ERROR_INSTANTIATE = -4,
    WAMR_SGX_ERROR_FUNCTION_NOT_FOUND = -5,
    WAMR_SGX_ERROR_EXECUTION = -6,
    WAMR_SGX_ERROR_MEMORY = -7,
    WAMR_SGX_ERROR_INVALID_ARGUMENT = -8
} wamr_sgx_result_t;

typedef enum {
    WAMR_SGX_VAL_TYPE_I32 = 0,
    WAMR_SGX_VAL_TYPE_I64 = 1,
    WAMR_SGX_VAL_TYPE_F32 = 2,
    WAMR_SGX_VAL_TYPE_F64 = 3,
    WAMR_SGX_VAL_TYPE_BUFFER = 4
} wamr_sgx_val_type_t;

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

/**
 * Initialize SGX environment with WAMR runtime
 * 
 * @param context output pointer to receive created context
 * @return WAMR_SGX_SUCCESS on success, error code on failure
 */
wamr_sgx_result_t wamr_sgx_init(wamr_sgx_context_t* context);

/**
 * Clean up and destroy the SGX environment
 * 
 * @param context context handle
 * @return WAMR_SGX_SUCCESS on success, error code on failure
 */
wamr_sgx_result_t wamr_sgx_destroy(wamr_sgx_context_t context);

/**
 * Load a WASM module
 * 
 * @param context context handle
 * @param wasm_bytes pointer to WASM binary data
 * @param wasm_size size of WASM binary in bytes
 * @param module output pointer to receive module handle
 * @return WAMR_SGX_SUCCESS on success, error code on failure
 */
wamr_sgx_result_t wamr_sgx_load_module(
    wamr_sgx_context_t context,
    const uint8_t* wasm_bytes,
    size_t wasm_size,
    wamr_sgx_module_t* module
);

/**
 * Unload a WASM module
 * 
 * @param context context handle
 * @param module module handle
 * @return WAMR_SGX_SUCCESS on success, error code on failure
 */
wamr_sgx_result_t wamr_sgx_unload_module(
    wamr_sgx_context_t context,
    wamr_sgx_module_t module
);

/**
 * Instantiate a loaded module
 * 
 * @param context context handle
 * @param module module handle
 * @param stack_size stack size in bytes
 * @param heap_size heap size in bytes
 * @param instance output pointer to receive instance handle
 * @return WAMR_SGX_SUCCESS on success, error code on failure
 */
wamr_sgx_result_t wamr_sgx_instantiate(
    wamr_sgx_context_t context,
    wamr_sgx_module_t module,
    uint32_t stack_size,
    uint32_t heap_size,
    wamr_sgx_instance_t* instance
);

/**
 * Destroy a module instance
 * 
 * @param context context handle
 * @param instance instance handle
 * @return WAMR_SGX_SUCCESS on success, error code on failure
 */
wamr_sgx_result_t wamr_sgx_destroy_instance(
    wamr_sgx_context_t context,
    wamr_sgx_instance_t instance
);

/**
 * Call a function in a module instance
 * 
 * @param context context handle
 * @param instance instance handle
 * @param function_name name of function to call
 * @param params array of parameter values
 * @param param_count number of parameters
 * @param results array to store result values
 * @param result_count number of results expected
 * @return WAMR_SGX_SUCCESS on success, error code on failure
 */
wamr_sgx_result_t wamr_sgx_call_function(
    wamr_sgx_context_t context, 
    wamr_sgx_instance_t instance,
    const char* function_name,
    const wamr_sgx_val_t* params,
    size_t param_count,
    wamr_sgx_val_t* results,
    size_t result_count
);

/**
 * Get last error message
 * 
 * @param context context handle
 * @return error message string or NULL if no error
 */
const char* wamr_sgx_get_error(wamr_sgx_context_t context);

#ifdef __cplusplus
}
#endif

#endif