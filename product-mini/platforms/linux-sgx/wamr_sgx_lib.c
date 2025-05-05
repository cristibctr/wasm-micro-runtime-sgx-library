/*
 * Copyright (C) 2019 Intel Corporation.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include "wamr_sgx_lib.h"
#include "enclave-sample/App/Enclave_u.h"
#include "sgx_urts.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>

/* Opaque structs implementation */
struct WamrSgxContext {
    sgx_enclave_id_t eid;
    pthread_mutex_t mutex;
    char error_buf[256];
};

struct WamrSgxModule {
    struct WamrSgxContext* context;
    uint64_t handle;
};

struct WamrSgxInstance {
    struct WamrSgxContext* context;
    uint64_t handle;
};

int
ocall_print(const char *str)
{
    return printf("%s", str);
}

/* Helper function to load and initialize the enclave */
static sgx_status_t 
initialize_enclave(sgx_enclave_id_t* p_eid) 
{
    sgx_launch_token_t token = { 0 };
    int updated = 0;
    sgx_status_t ret;
    char cwd[1024] = {0};
    
    if (getcwd(cwd, sizeof(cwd)) == NULL) {
        printf("[SGX Init] ERROR: Failed to get current working directory\n");
    } else {
        printf("[SGX Init] Current working directory: %s\n", cwd);
    }
    
    const char *env_path = getenv("WAMR_ENCLAVE_PATH");
    printf("[SGX Init] WAMR_ENCLAVE_PATH environment variable: %s\n", 
           env_path ? env_path : "not set");
    
    const char *path = env_path;
    if (!path) {
        printf("[SGX Init] Using default enclave path: enclave.signed.so\n");
        path = "enclave.signed.so";
    } else {
        printf("[SGX Init] Using enclave path from environment: %s\n", path);
    }
    
    void* *file = fopen(path, "r");
    if (file) {
        printf("[SGX Init] Enclave file exists at: %s\n", path);
        fclose(file);
    } else {
        printf("[SGX Init] ERROR: Enclave file does not exist at: %s\n", path);
        printf("[SGX Init] Error details: %s\n", strerror(errno));
        
        const char *alt_paths[] = {
            "enclave-sample/enclave.signed.so",
            "../enclave-sample/enclave.signed.so",
            "product-mini/platforms/linux-sgx/enclave-sample/enclave.signed.so"
        };
        
        printf("[SGX Init] Checking alternative locations...\n");
        for (int i = 0; i < sizeof(alt_paths)/sizeof(alt_paths[0]); i++) {
            file = fopen(alt_paths[i], "r");
            if (file) {
                fclose(file);
                printf("[SGX Init] Found enclave at alternative location: %s\n", alt_paths[i]);
                printf("[SGX Init] Set WAMR_ENCLAVE_PATH environment variable to use this path\n");
                break;
            }
        }
    }
    
    printf("[SGX Init] Attempting to load enclave from: %s\n", path);
    ret = sgx_create_enclave(path, SGX_DEBUG_FLAG,
                           &token, &updated, p_eid, NULL);
    
    printf("[SGX Init] sgx_create_enclave returned: %d (0x%x)\n", ret, ret);
    if (ret != SGX_SUCCESS) {
        printf("[SGX Init] ERROR: Failed to create enclave. Error code: %d (0x%x)\n", ret, ret);
        
        switch(ret) {
            case SGX_ERROR_INVALID_ENCLAVE:
                printf("[SGX Init] Error details: Invalid enclave image\n");
                break;
            case SGX_ERROR_INVALID_PARAMETER:
                printf("[SGX Init] Error details: Invalid parameter\n");
                break;
            case SGX_ERROR_OUT_OF_MEMORY:
                printf("[SGX Init] Error details: Out of memory\n");
                break;
            case SGX_ERROR_ENCLAVE_FILE_ACCESS:
                printf("[SGX Init] Error details: Can't open enclave file\n");
                break;
            case SGX_ERROR_INVALID_METADATA:
                printf("[SGX Init] Error details: Invalid metadata\n");
                break;
            default:
                printf("[SGX Init] Error details: Unknown SGX error\n");
                break;
        }
    } else {
        printf("[SGX Init] Enclave created successfully with ID: %lu\n", *p_eid);
    }
    
    return ret;
}

/* Implementation of public API */
wamr_sgx_result_t 
wamr_sgx_init(wamr_sgx_context_t* context)
{
    struct WamrSgxContext* ctx;
    sgx_enclave_id_t eid;
    sgx_status_t sgx_ret;
    int enclave_ret;
    
    if (!context) {
        return WAMR_SGX_ERROR_INVALID_ARGUMENT;
    }
    
    ctx = (struct WamrSgxContext*)malloc(sizeof(struct WamrSgxContext));
    if (!ctx) {
        return WAMR_SGX_ERROR_MEMORY;
    }
    
    memset(ctx, 0, sizeof(struct WamrSgxContext));
    
    if (pthread_mutex_init(&ctx->mutex, NULL) != 0) {
        free(ctx);
        return WAMR_SGX_ERROR_RUNTIME_INIT;
    }
    
    sgx_ret = initialize_enclave(&eid);
    if (sgx_ret != SGX_SUCCESS) {
        pthread_mutex_destroy(&ctx->mutex);
        free(ctx);
        return WAMR_SGX_ERROR_ENCLAVE_INIT;
    }
    
    ctx->eid = eid;
    
    sgx_ret = ecall_init_runtime(eid, &enclave_ret, 4);
    if (sgx_ret != SGX_SUCCESS || enclave_ret != 0) {
        sgx_destroy_enclave(eid);
        pthread_mutex_destroy(&ctx->mutex);
        free(ctx);
        return WAMR_SGX_ERROR_RUNTIME_INIT;
    }
    
    *context = ctx;
    return WAMR_SGX_SUCCESS;
}

wamr_sgx_result_t 
wamr_sgx_destroy(wamr_sgx_context_t context)
{
    sgx_status_t sgx_ret;
    int enclave_ret;
    
    if (!context) {
        return WAMR_SGX_ERROR_INVALID_ARGUMENT;
    }
    
    sgx_ret = ecall_destroy_runtime(context->eid, &enclave_ret);
    
    sgx_destroy_enclave(context->eid);
    
    pthread_mutex_destroy(&context->mutex);
    
    free(context);
    
    return (sgx_ret == SGX_SUCCESS && enclave_ret == 0) ? 
           WAMR_SGX_SUCCESS : WAMR_SGX_ERROR_RUNTIME_INIT;
}

wamr_sgx_result_t 
wamr_sgx_load_module(
    wamr_sgx_context_t context,
    const uint8_t* wasm_bytes,
    size_t wasm_size,
    wamr_sgx_module_t* module)
{
    struct WamrSgxModule* mod;
    sgx_status_t sgx_ret;
    int enclave_ret;
    uint64_t module_handle = 0;
    
    if (!context || !wasm_bytes || wasm_size == 0 || !module) {
        return WAMR_SGX_ERROR_INVALID_ARGUMENT;
    }
    
    pthread_mutex_lock(&context->mutex);
    
    sgx_ret = ecall_load_module(
        context->eid, 
        &enclave_ret,
        (uint8_t*)wasm_bytes,
        (uint32_t)wasm_size, 
        context->error_buf, 
        sizeof(context->error_buf), 
        &module_handle
    );
    
    pthread_mutex_unlock(&context->mutex);
    
    if (sgx_ret != SGX_SUCCESS || enclave_ret != 0 || module_handle == 0) {
        return WAMR_SGX_ERROR_LOAD_MODULE;
    }
    
    mod = (struct WamrSgxModule*)malloc(sizeof(struct WamrSgxModule));
    if (!mod) {
        return WAMR_SGX_ERROR_MEMORY;
    }
    
    mod->context = context;
    mod->handle = module_handle;
    
    *module = mod;
    return WAMR_SGX_SUCCESS;
}

wamr_sgx_result_t 
wamr_sgx_unload_module(
    wamr_sgx_context_t context,
    wamr_sgx_module_t module)
{
    sgx_status_t sgx_ret;
    int enclave_ret;
    
    if (!context || !module || module->context != context) {
        return WAMR_SGX_ERROR_INVALID_ARGUMENT;
    }
    
    pthread_mutex_lock(&context->mutex);
    
    sgx_ret = ecall_unload_module(context->eid, &enclave_ret, module->handle);
    
    pthread_mutex_unlock(&context->mutex);
    
    free(module);
    
    return (sgx_ret == SGX_SUCCESS && enclave_ret == 0) ? 
           WAMR_SGX_SUCCESS : WAMR_SGX_ERROR_LOAD_MODULE;
}

wamr_sgx_result_t 
wamr_sgx_instantiate(
    wamr_sgx_context_t context,
    wamr_sgx_module_t module,
    uint32_t stack_size,
    uint32_t heap_size,
    wamr_sgx_instance_t* instance)
{
    struct WamrSgxInstance* inst;
    sgx_status_t sgx_ret;
    int enclave_ret;
    uint64_t instance_handle = 0;
    
    if (!context || !module || module->context != context || !instance) {
        return WAMR_SGX_ERROR_INVALID_ARGUMENT;
    }
    
    pthread_mutex_lock(&context->mutex);
    
    sgx_ret = ecall_instantiate_module(
        context->eid, 
        &enclave_ret,
        module->handle, 
        stack_size, 
        heap_size, 
        context->error_buf, 
        sizeof(context->error_buf), 
        &instance_handle
    );
    
    pthread_mutex_unlock(&context->mutex);
    
    if (sgx_ret != SGX_SUCCESS || enclave_ret != 0 || instance_handle == 0) {
        return WAMR_SGX_ERROR_INSTANTIATE;
    }
    
    inst = (struct WamrSgxInstance*)malloc(sizeof(struct WamrSgxInstance));
    if (!inst) {
        return WAMR_SGX_ERROR_MEMORY;
    }
    
    inst->context = context;
    inst->handle = instance_handle;
    
    *instance = inst;
    return WAMR_SGX_SUCCESS;
}

wamr_sgx_result_t 
wamr_sgx_destroy_instance(
    wamr_sgx_context_t context,
    wamr_sgx_instance_t instance)
{
    sgx_status_t sgx_ret;
    int enclave_ret;
    
    if (!context || !instance || instance->context != context) {
        return WAMR_SGX_ERROR_INVALID_ARGUMENT;
    }
    
    pthread_mutex_lock(&context->mutex);
    printf("[SGX Call] Mutex acquired, calling function in enclave\n");

    printf("C sizeof(wamr_sgx_val_t) = %zu, align = %zu\n",
           sizeof(wamr_sgx_val_t),
           _Alignof(wamr_sgx_val_t));
    printf("C sizeof(wamr_sgx_context_t) = %zu, align = %zu\n",
           sizeof(wamr_sgx_context_t),
           _Alignof(wamr_sgx_context_t));
    printf("C sizeof(wamr_sgx_instance_t) = %zu, align = %zu\n",
           sizeof(wamr_sgx_instance_t),
           _Alignof(wamr_sgx_instance_t));
    sgx_ret = ecall_destroy_instance(context->eid, &enclave_ret, instance->handle);
    
    pthread_mutex_unlock(&context->mutex);
    
    free(instance);
    
    return (sgx_ret == SGX_SUCCESS && enclave_ret == 0) ? 
           WAMR_SGX_SUCCESS : WAMR_SGX_ERROR_INSTANTIATE;
}

wamr_sgx_result_t 
wamr_sgx_call_function(
    wamr_sgx_context_t context, 
    wamr_sgx_instance_t instance,
    const char* function_name,
    const wamr_sgx_val_t* params,
    size_t param_count,
    wamr_sgx_val_t* results,
    size_t result_count)
{
    sgx_status_t sgx_ret;
    int enclave_ret;
    
    printf("[SGX Call] Entering wamr_sgx_call_function for function: %s\n", function_name);
    printf("[SGX Call] Parameters: count=%zu, results expected: %zu\n", param_count, result_count);
    
    if (!context || !instance || instance->context != context || !function_name) {
        printf("[SGX Call] ERROR: Invalid arguments\n");
        return WAMR_SGX_ERROR_INVALID_ARGUMENT;
    }
    
    if ((param_count > 0 && !params) || (result_count > 0 && !results)) {
        printf("[SGX Call] ERROR: Invalid params/results arrays\n");
        return WAMR_SGX_ERROR_INVALID_ARGUMENT;
    }
    
    void *stack_ptr = NULL;
    size_t stack_size = 0;
    #if defined(__linux__) && defined(__GLIBC__)
    pthread_attr_t attr;
    void *stackaddr;
    pthread_getattr_np(pthread_self(), &attr);
    pthread_attr_getstack(&attr, &stackaddr, &stack_size);
    stack_ptr = __builtin_frame_address(0);
    printf("[SGX Call] Stack info: current=%p, base=%p, size=%zu, used=%zu\n", 
           stack_ptr, stackaddr, stack_size, 
           (size_t)((char*)stackaddr + stack_size - (char*)stack_ptr));
    pthread_attr_destroy(&attr);
    #endif
    
    printf("[SGX Call] Acquiring mutex\n");
    pthread_mutex_lock(&context->mutex);
    
    printf("C sizeof(wamr_sgx_val_t) = %zu, align = %zu\n",
           sizeof(wamr_sgx_val_t),
           _Alignof(wamr_sgx_val_t));
    printf("C sizeof(wamr_sgx_context_t) = %zu, align = %zu\n",
           sizeof(wamr_sgx_context_t),
           _Alignof(wamr_sgx_context_t));
    printf("C sizeof(wamr_sgx_instance_t) = %zu, align = %zu\n",
           sizeof(wamr_sgx_instance_t),
           _Alignof(wamr_sgx_instance_t));
           
    uint8_t *params_buf = NULL;
    uint8_t *results_buf = NULL;
    
    if (param_count > 0) {
        size_t params_size = param_count * sizeof(wamr_sgx_val_t);
        params_buf = (uint8_t*)malloc(params_size);
        if (!params_buf) {
            pthread_mutex_unlock(&context->mutex);
            printf("[SGX Call] ERROR: Failed to allocate memory for parameters\n");
            return WAMR_SGX_ERROR_MEMORY;
        }
        memcpy(params_buf, params, params_size);
    }
    
    if (result_count > 0) {
        size_t results_size = result_count * sizeof(wamr_sgx_val_t);
        results_buf = (uint8_t*)malloc(results_size);
        if (!results_buf) {
            if (params_buf) {
                free(params_buf);
            }
            pthread_mutex_unlock(&context->mutex);
            printf("[SGX Call] ERROR: Failed to allocate memory for results\n");
            return WAMR_SGX_ERROR_MEMORY;
        }
        memset(results_buf, 0, results_size);
    }
    
    sgx_ret = ecall_call_function(
        context->eid, 
        &enclave_ret,
        instance->handle, 
        function_name, 
        params_buf, 
        (uint32_t)(param_count * sizeof(wamr_sgx_val_t)), 
        results_buf, 
        (uint32_t)(result_count * sizeof(wamr_sgx_val_t)),
        context->error_buf, 
        sizeof(context->error_buf)
    );
    
    if (sgx_ret == SGX_SUCCESS && result_count > 0 && results) {
        memcpy(results, results_buf, result_count * sizeof(wamr_sgx_val_t));
    }
    
    if (params_buf) {
        free(params_buf);
    }
    
    if (results_buf) {
        free(results_buf);
    }
    
    printf("[SGX Call] Function call completed, SGX status: %d, Enclave return: %d\n", 
           sgx_ret, enclave_ret);
    
    pthread_mutex_unlock(&context->mutex);
    printf("[SGX Call] Mutex released\n");
    
    if (sgx_ret != SGX_SUCCESS) {
        printf("[SGX Call] ERROR: SGX error occurred: %d (0x%x)\n", sgx_ret, sgx_ret);
        switch(sgx_ret) {
            case SGX_ERROR_STACK_OVERRUN:
                printf("[SGX Call] ERROR: Stack overflow detected in the enclave\n");
                break;
            case SGX_ERROR_OUT_OF_MEMORY:
                printf("[SGX Call] ERROR: Out of memory in the enclave\n");
                break;
            case SGX_ERROR_ENCLAVE_CRASHED:
                printf("[SGX Call] ERROR: Enclave crashed\n");
                break;
            default:
                printf("[SGX Call] ERROR: Unknown SGX error\n");
                break;
        }
        return WAMR_SGX_ERROR_EXECUTION;
    }
    
    if (enclave_ret != 0) {
        printf("[SGX Call] ERROR: Function execution failed with code: %d\n", enclave_ret);
        printf("[SGX Call] Error message: %s\n", 
               context->error_buf[0] ? context->error_buf : "No error message");
        return WAMR_SGX_ERROR_EXECUTION;
    }
    
    printf("[SGX Call] Function executed successfully\n");
    return WAMR_SGX_SUCCESS;
}

const char* 
wamr_sgx_get_error(wamr_sgx_context_t context)
{
    if (!context || context->error_buf[0] == '\0') {
        return NULL;
    }
    
    return context->error_buf;
}