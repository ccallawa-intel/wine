/*
 * XeSS Unix library
 *
 * Copyright 2025 the Wine project
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <stdarg.h>
#include <stdlib.h>
#include <dlfcn.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winternl.h"

#include "wine/debug.h"
#include "unixlib.h"

WINE_DEFAULT_DEBUG_CHANNEL(xess);
WINE_DECLARE_DEBUG_CHANNEL(winediag);

/* helper functions to unwrap handles */
static VkInstance get_host_instance(VkInstance wine_instance)
{
    struct vulkan_instance *instance;
    if (!wine_instance) return NULL;
    instance = vulkan_instance_from_handle(wine_instance);
    return instance->host.instance;
}

static VkPhysicalDevice get_host_physical_device(VkPhysicalDevice wine_phys_dev)
{
    struct vulkan_physical_device *phys_dev;
    if (!wine_phys_dev) return NULL;
    phys_dev = vulkan_physical_device_from_handle(wine_phys_dev);
    return phys_dev->host.physical_device;
}

static VkDevice get_host_device(VkDevice wine_device)
{
    struct vulkan_device *device;
    if (!wine_device) return NULL;
    device = vulkan_device_from_handle(wine_device);
    return device->host.device;
}

static VkCommandBuffer get_host_command_buffer(VkCommandBuffer wine_cmd_buffer)
{
    struct vulkan_command_buffer *cmd_buffer;
    if (!wine_cmd_buffer) return NULL;
    cmd_buffer = vulkan_command_buffer_from_handle(wine_cmd_buffer);
    return cmd_buffer->host.command_buffer;
}

static VkDeviceMemory get_host_device_memory(VkDeviceMemory wine_device_memory)
{
    struct vulkan_device_memory *device_memory;
    if (!wine_device_memory) return VK_NULL_HANDLE;
    device_memory = vulkan_device_memory_from_handle(wine_device_memory);
    return device_memory->host.device_memory;
}

static void *vulkan_library = NULL;

static xess_result_t (*p_xessDestroyContext)(xess_context_handle_t);
static xess_result_t (*p_xessGetVersion)(xess_version_t *);
static xess_result_t (*p_xessGetIntelXeFXVersion)(xess_context_handle_t, xess_version_t *);
static xess_result_t (*p_xessGetProperties)(xess_context_handle_t, const xess_2d_t *, xess_properties_t *);
static xess_result_t (*p_xessGetInputResolution)(xess_context_handle_t, const xess_2d_t *, xess_quality_settings_t, xess_2d_t *);
static xess_result_t (*p_xessGetOptimalInputResolution)(xess_context_handle_t, const xess_2d_t *, xess_quality_settings_t, xess_2d_t *, xess_2d_t *, xess_2d_t *);
static xess_result_t (*p_xessGetJitterScale)(xess_context_handle_t, float *, float *);
static xess_result_t (*p_xessGetVelocityScale)(xess_context_handle_t, float *, float *);
static xess_result_t (*p_xessGetExposureMultiplier)(xess_context_handle_t, float *);
static xess_result_t (*p_xessGetMaxResponsiveMaskValue)(xess_context_handle_t, float *);
static xess_result_t (*p_xessSetVelocityScale)(xess_context_handle_t, float, float);
static xess_result_t (*p_xessSetJitterScale)(xess_context_handle_t, float, float);
static xess_result_t (*p_xessSetExposureMultiplier)(xess_context_handle_t, float);
static xess_result_t (*p_xessSetMaxResponsiveMaskValue)(xess_context_handle_t, float);
static xess_result_t (*p_xessSetLoggingCallback)(xess_context_handle_t, xess_logging_level_t, xess_app_log_callback_t);
static xess_result_t (*p_xessIsOptimalDriver)(xess_context_handle_t);
static xess_result_t (*p_xessForceLegacyScaleFactors)(xess_context_handle_t, uint32_t);
static xess_result_t (*p_xessGetPipelineBuildStatus)(xess_context_handle_t);
static xess_result_t (*p_xessSelectNetworkModel)(xess_context_handle_t, xess_network_model_t);
static xess_result_t (*p_xessStartDump)(xess_context_handle_t, const xess_dump_parameters_t *);
static xess_result_t (*p_xessGetProfilingData)(xess_context_handle_t, xess_profiling_data_t **);

static xess_result_t (*p_xessVKGetRequiredInstanceExtensions)(uint32_t*, const char* const**, uint32_t*);
static xess_result_t (*p_xessVKGetRequiredDeviceExtensions)(VkInstance, VkPhysicalDevice, uint32_t*, const char* const**);
static xess_result_t (*p_xessVKGetRequiredDeviceFeatures)(VkInstance, VkPhysicalDevice, void**);
static xess_result_t (*p_xessVKCreateContext)(VkInstance, VkPhysicalDevice, VkDevice, xess_context_handle_t *);
static xess_result_t (*p_xessVKBuildPipelines)(xess_context_handle_t, VkPipelineCache, bool, uint32_t);
static xess_result_t (*p_xessVKInit)(xess_context_handle_t, const xess_vk_init_params_t *);
static xess_result_t (*p_xessVKGetInitParams)(xess_context_handle_t, xess_vk_init_params_t *);
static xess_result_t (*p_xessVKExecute)(xess_context_handle_t, VkCommandBuffer, const xess_vk_execute_params_t*);

static NTSTATUS xess_init( void *args )
{
    struct xess_init_params *params = args;
    const char *xess_vulkan_lib;

    TRACE("Initializing XeSS translation...\n");

    if (vulkan_library)
    {
        params->result = XESS_RESULT_SUCCESS;
        return STATUS_SUCCESS;
    }

    /* Allow user to specify custom Vulkan XeSS implementation */
    xess_vulkan_lib = getenv("XESS_VULKAN_LIB");
    if (!xess_vulkan_lib)
        xess_vulkan_lib = "libxess_vulkan.so";

    TRACE("Loading Vulkan XeSS implementation: %s\n", xess_vulkan_lib);

    vulkan_library = dlopen(xess_vulkan_lib, RTLD_NOW);
    if (!vulkan_library)
    {
        ERR_(winediag)("Failed to load Vulkan XeSS library '%s': %s\n", xess_vulkan_lib, dlerror());
        ERR_(winediag)("Please set XESS_VULKAN_LIB environment variable or install libxess_vulkan.so\n");
        params->result = XESS_RESULT_ERROR_CANT_LOAD_LIBRARY;
        return STATUS_SUCCESS;
    }

#define LOAD_FUNCPTR(f) \
    if (!(p_##f = dlsym( vulkan_library, #f ))) \
    { \
        ERR("Failed to load function '%s': %s\n", #f, dlerror()); \
        goto fail; \
    }

    /* Common API functions */
    LOAD_FUNCPTR(xessDestroyContext)
    LOAD_FUNCPTR(xessGetVersion)
    LOAD_FUNCPTR(xessGetIntelXeFXVersion)
    LOAD_FUNCPTR(xessGetProperties)
    LOAD_FUNCPTR(xessGetInputResolution)
    LOAD_FUNCPTR(xessGetOptimalInputResolution)
    LOAD_FUNCPTR(xessGetJitterScale)
    LOAD_FUNCPTR(xessGetVelocityScale)
    LOAD_FUNCPTR(xessGetExposureMultiplier)
    LOAD_FUNCPTR(xessGetMaxResponsiveMaskValue)
    LOAD_FUNCPTR(xessSetVelocityScale)
    LOAD_FUNCPTR(xessSetJitterScale)
    LOAD_FUNCPTR(xessSetExposureMultiplier)
    LOAD_FUNCPTR(xessSetMaxResponsiveMaskValue)
    LOAD_FUNCPTR(xessSetLoggingCallback)
    LOAD_FUNCPTR(xessIsOptimalDriver)
    LOAD_FUNCPTR(xessForceLegacyScaleFactors)
    LOAD_FUNCPTR(xessGetPipelineBuildStatus)
    LOAD_FUNCPTR(xessSelectNetworkModel)
    LOAD_FUNCPTR(xessStartDump)
    LOAD_FUNCPTR(xessGetProfilingData)

    /* Vulkan-specific functions */
    LOAD_FUNCPTR(xessVKGetRequiredInstanceExtensions)
    LOAD_FUNCPTR(xessVKGetRequiredDeviceExtensions)
    LOAD_FUNCPTR(xessVKGetRequiredDeviceFeatures)
    LOAD_FUNCPTR(xessVKCreateContext)
    LOAD_FUNCPTR(xessVKBuildPipelines)
    LOAD_FUNCPTR(xessVKInit)
    LOAD_FUNCPTR(xessVKGetInitParams)
    LOAD_FUNCPTR(xessVKExecute)
#undef LOAD_FUNCPTR

    TRACE("Vulkan XeSS implementation loaded successfully\n");
    params->result = XESS_RESULT_SUCCESS;
    return STATUS_SUCCESS;

fail:
    dlclose(vulkan_library);
    vulkan_library = NULL;
    params->result = XESS_RESULT_ERROR_CANT_LOAD_LIBRARY;
    return STATUS_SUCCESS;
}

static NTSTATUS xess_destroy_context( void *args )
{
    struct xess_destroy_context_params *params = args;
    if (!vulkan_library) { params->result = XESS_RESULT_ERROR_CANT_LOAD_LIBRARY; return STATUS_SUCCESS; }
    params->result = p_xessDestroyContext( params->hContext );
    return STATUS_SUCCESS;
}

static NTSTATUS xess_get_version( void *args )
{
    struct xess_get_version_params *params = args;
    if (!vulkan_library) { params->result = XESS_RESULT_ERROR_CANT_LOAD_LIBRARY; return STATUS_SUCCESS; }
    params->result = p_xessGetVersion( params->pVersion );
    return STATUS_SUCCESS;
}

static NTSTATUS xess_get_intel_xefx_version( void *args )
{
    struct xess_get_intel_xefx_version_params *params = args;
    if (!vulkan_library) { params->result = XESS_RESULT_ERROR_CANT_LOAD_LIBRARY; return STATUS_SUCCESS; }
    params->result = p_xessGetIntelXeFXVersion( params->hContext, params->pVersion );
    return STATUS_SUCCESS;
}

static NTSTATUS xess_get_properties( void *args )
{
    struct xess_get_properties_params *params = args;
    if (!vulkan_library) { params->result = XESS_RESULT_ERROR_CANT_LOAD_LIBRARY; return STATUS_SUCCESS; }
    params->result = p_xessGetProperties( params->hContext, params->pOutputResolution, params->pProperties );
    return STATUS_SUCCESS;
}

static NTSTATUS xess_get_input_resolution( void *args )
{
    struct xess_get_input_resolution_params *params = args;
    if (!vulkan_library) { params->result = XESS_RESULT_ERROR_CANT_LOAD_LIBRARY; return STATUS_SUCCESS; }
    params->result = p_xessGetInputResolution( params->hContext, params->pOutputResolution, params->qualitySetting, params->pInputResolution );
    return STATUS_SUCCESS;
}

static NTSTATUS xess_get_optimal_input_resolution( void *args )
{
    struct xess_get_optimal_input_resolution_params *params = args;
    if (!vulkan_library) { params->result = XESS_RESULT_ERROR_CANT_LOAD_LIBRARY; return STATUS_SUCCESS; }
    params->result = p_xessGetOptimalInputResolution( params->hContext, params->pOutputResolution, params->qualitySetting,
                                                       params->pMinResolution, params->pMaxResolution, params->pOptimalResolution );
    return STATUS_SUCCESS;
}

static NTSTATUS xess_get_jitter_scale( void *args )
{
    struct xess_get_jitter_scale_params *params = args;
    if (!vulkan_library) { params->result = XESS_RESULT_ERROR_CANT_LOAD_LIBRARY; return STATUS_SUCCESS; }
    params->result = p_xessGetJitterScale( params->hContext, params->pX, params->pY );
    return STATUS_SUCCESS;
}

static NTSTATUS xess_get_velocity_scale( void *args )
{
    struct xess_get_velocity_scale_params *params = args;
    if (!vulkan_library) { params->result = XESS_RESULT_ERROR_CANT_LOAD_LIBRARY; return STATUS_SUCCESS; }
    params->result = p_xessGetVelocityScale( params->hContext, params->pX, params->pY );
    return STATUS_SUCCESS;
}

static NTSTATUS xess_get_exposure_multiplier( void *args )
{
    struct xess_get_exposure_multiplier_params *params = args;
    if (!vulkan_library) { params->result = XESS_RESULT_ERROR_CANT_LOAD_LIBRARY; return STATUS_SUCCESS; }
    params->result = p_xessGetExposureMultiplier( params->hContext, params->pScale );
    return STATUS_SUCCESS;
}

static NTSTATUS xess_get_max_responsive_mask_value( void *args )
{
    struct xess_get_max_responsive_mask_value_params *params = args;
    if (!vulkan_library) { params->result = XESS_RESULT_ERROR_CANT_LOAD_LIBRARY; return STATUS_SUCCESS; }
    params->result = p_xessGetMaxResponsiveMaskValue( params->hContext, params->pMaxValue );
    return STATUS_SUCCESS;
}

static NTSTATUS xess_set_velocity_scale( void *args )
{
    struct xess_set_velocity_scale_params *params = args;
    if (!vulkan_library) { params->result = XESS_RESULT_ERROR_CANT_LOAD_LIBRARY; return STATUS_SUCCESS; }
    params->result = p_xessSetVelocityScale( params->hContext, params->x, params->y );
    return STATUS_SUCCESS;
}

static NTSTATUS xess_set_jitter_scale( void *args )
{
    struct xess_set_jitter_scale_params *params = args;
    if (!vulkan_library) { params->result = XESS_RESULT_ERROR_CANT_LOAD_LIBRARY; return STATUS_SUCCESS; }
    params->result = p_xessSetJitterScale( params->hContext, params->x, params->y );
    return STATUS_SUCCESS;
}

static NTSTATUS xess_set_exposure_multiplier( void *args )
{
    struct xess_set_exposure_multiplier_params *params = args;
    if (!vulkan_library) { params->result = XESS_RESULT_ERROR_CANT_LOAD_LIBRARY; return STATUS_SUCCESS; }
    params->result = p_xessSetExposureMultiplier( params->hContext, params->scale );
    return STATUS_SUCCESS;
}

static NTSTATUS xess_set_max_responsive_mask_value( void *args )
{
    struct xess_set_max_responsive_mask_value_params *params = args;
    if (!vulkan_library) { params->result = XESS_RESULT_ERROR_CANT_LOAD_LIBRARY; return STATUS_SUCCESS; }
    params->result = p_xessSetMaxResponsiveMaskValue( params->hContext, params->maxValue );
    return STATUS_SUCCESS;
}

static NTSTATUS xess_set_logging_callback( void *args )
{
    struct xess_set_logging_callback_params *params = args;
    if (!vulkan_library) { params->result = XESS_RESULT_ERROR_CANT_LOAD_LIBRARY; return STATUS_SUCCESS; }
    params->result = p_xessSetLoggingCallback( params->hContext, params->loggingLevel, params->loggingFunction );
    return STATUS_SUCCESS;
}

static NTSTATUS xess_is_optimal_driver( void *args )
{
    struct xess_is_optimal_driver_params *params = args;
    if (!vulkan_library) { params->result = XESS_RESULT_ERROR_CANT_LOAD_LIBRARY; return STATUS_SUCCESS; }
    params->result = p_xessIsOptimalDriver( params->hContext );
    return STATUS_SUCCESS;
}

static NTSTATUS xess_force_legacy_scale_factors( void *args )
{
    struct xess_force_legacy_scale_factors_params *params = args;
    if (!vulkan_library) { params->result = XESS_RESULT_ERROR_CANT_LOAD_LIBRARY; return STATUS_SUCCESS; }
    params->result = p_xessForceLegacyScaleFactors( params->hContext, params->force );
    return STATUS_SUCCESS;
}

static NTSTATUS xess_get_pipeline_build_status( void *args )
{
    struct xess_get_pipeline_build_status_params *params = args;
    if (!vulkan_library) { params->result = XESS_RESULT_ERROR_CANT_LOAD_LIBRARY; return STATUS_SUCCESS; }
    params->result = p_xessGetPipelineBuildStatus( params->hContext );
    return STATUS_SUCCESS;
}

static NTSTATUS xess_select_network_model( void *args )
{
    struct xess_select_network_model_params *params = args;
    if (!vulkan_library) { params->result = XESS_RESULT_ERROR_CANT_LOAD_LIBRARY; return STATUS_SUCCESS; }
    params->result = p_xessSelectNetworkModel( params->hContext, params->network );
    return STATUS_SUCCESS;
}

static NTSTATUS xess_start_dump( void *args )
{
    struct xess_start_dump_params *params = args;
    if (!vulkan_library) { params->result = XESS_RESULT_ERROR_CANT_LOAD_LIBRARY; return STATUS_SUCCESS; }
    params->result = p_xessStartDump( params->hContext, params->dump_parameters );
    return STATUS_SUCCESS;
}

static NTSTATUS xess_get_profiling_data( void *args )
{
    struct xess_get_profiling_data_params *params = args;
    if (!vulkan_library) { params->result = XESS_RESULT_ERROR_CANT_LOAD_LIBRARY; return STATUS_SUCCESS; }
    params->result = p_xessGetProfilingData( params->hContext, params->pProfilingData );
    return STATUS_SUCCESS;
}

static NTSTATUS xess_vk_get_required_instance_extensions( void *args )
{
    struct xess_vk_get_required_instance_extensions_params *params = args;
    if (!vulkan_library) { params->result = XESS_RESULT_ERROR_CANT_LOAD_LIBRARY; return STATUS_SUCCESS; }
    params->result = p_xessVKGetRequiredInstanceExtensions( params->instanceExtensionsCount, params->instanceExtensions, params->minVkApiVersion );
    return STATUS_SUCCESS;
}

static NTSTATUS xess_vk_get_required_device_extensions( void *args )
{
    struct xess_vk_get_required_device_extensions_params *params = args;
    if (!vulkan_library) { params->result = XESS_RESULT_ERROR_CANT_LOAD_LIBRARY; return STATUS_SUCCESS; }
    params->result = p_xessVKGetRequiredDeviceExtensions( get_host_instance(params->instance), get_host_physical_device(params->physicalDevice), params->deviceExtensionsCount, params->deviceExtensions );
    return STATUS_SUCCESS;
}

static NTSTATUS xess_vk_get_required_device_features( void *args )
{
    struct xess_vk_get_required_device_features_params *params = args;
    if (!vulkan_library) { params->result = XESS_RESULT_ERROR_CANT_LOAD_LIBRARY; return STATUS_SUCCESS; }
    params->result = p_xessVKGetRequiredDeviceFeatures( get_host_instance(params->instance), get_host_physical_device(params->physicalDevice), params->features );
    return STATUS_SUCCESS;
}

static NTSTATUS xess_vk_create_context( void *args )
{
    struct xess_vk_create_context_params *params = args;
    if (!vulkan_library) { params->result = XESS_RESULT_ERROR_CANT_LOAD_LIBRARY; return STATUS_SUCCESS; }
    params->result = p_xessVKCreateContext( get_host_instance(params->instance), get_host_physical_device(params->physicalDevice), get_host_device(params->device), params->phContext );
    return STATUS_SUCCESS;
}

static NTSTATUS xess_vk_build_pipelines( void *args )
{
    struct xess_vk_build_pipelines_params *params = args;
    if (!vulkan_library) { params->result = XESS_RESULT_ERROR_CANT_LOAD_LIBRARY; return STATUS_SUCCESS; }
    params->result = p_xessVKBuildPipelines( params->hContext, params->pipelineCache, params->blocking, params->initFlags );
    return STATUS_SUCCESS;
}

static NTSTATUS xess_vk_init( void *args )
{
    struct xess_vk_init_params *params = args;
    const xess_vk_init_params_t *init_params;
    xess_vk_init_params_t host_init_params;

    if (!vulkan_library) { params->result = XESS_RESULT_ERROR_CANT_LOAD_LIBRARY; return STATUS_SUCCESS; }

    init_params = params->pInitParams;
    if (init_params)
    {
        host_init_params = *init_params;
        host_init_params.tempBufferHeap = get_host_device_memory( init_params->tempBufferHeap );
        host_init_params.tempTextureHeap = get_host_device_memory( init_params->tempTextureHeap );
        init_params = &host_init_params;
    }

    params->result = p_xessVKInit( params->hContext, init_params );
    return STATUS_SUCCESS;
}

static NTSTATUS xess_vk_get_init_params( void *args )
{
    struct xess_vk_get_init_params_params *params = args;
    if (!vulkan_library) { params->result = XESS_RESULT_ERROR_CANT_LOAD_LIBRARY; return STATUS_SUCCESS; }
    params->result = p_xessVKGetInitParams( params->hContext, params->pInitParams );
    return STATUS_SUCCESS;
}

static NTSTATUS xess_vk_execute( void *args )
{
    struct xess_vk_execute_params *params = args;
    if (!vulkan_library) { params->result = XESS_RESULT_ERROR_CANT_LOAD_LIBRARY; return STATUS_SUCCESS; }
    params->result = p_xessVKExecute( params->hContext, get_host_command_buffer(params->pCommandBuffer), params->pExecParams );
    return STATUS_SUCCESS;
}

/* mapping from xess_funcs enum to implementations */
const unixlib_entry_t __wine_unix_call_funcs[] =
{
    xess_init,
    xess_destroy_context,
    xess_get_version,
    xess_get_intel_xefx_version,
    xess_get_properties,
    xess_get_input_resolution,
    xess_get_optimal_input_resolution,
    xess_get_jitter_scale,
    xess_get_velocity_scale,
    xess_get_exposure_multiplier,
    xess_get_max_responsive_mask_value,
    xess_set_velocity_scale,
    xess_set_jitter_scale,
    xess_set_exposure_multiplier,
    xess_set_max_responsive_mask_value,
    xess_set_logging_callback,
    xess_is_optimal_driver,
    xess_force_legacy_scale_factors,
    xess_get_pipeline_build_status,
    xess_select_network_model,
    xess_start_dump,
    xess_get_profiling_data,
    xess_vk_get_required_instance_extensions,
    xess_vk_get_required_device_extensions,
    xess_vk_get_required_device_features,
    xess_vk_create_context,
    xess_vk_build_pipelines,
    xess_vk_init,
    xess_vk_get_init_params,
    xess_vk_execute,
};

#ifdef _WIN64

typedef ULONG PTR32;

static NTSTATUS wow64_xess_init( void *args ) { return xess_init( args ); }
static NTSTATUS wow64_xess_destroy_context( void *args ) { return xess_destroy_context( args ); }
static NTSTATUS wow64_xess_get_version( void *args ) { return xess_get_version( args ); }
static NTSTATUS wow64_xess_get_intel_xefx_version( void *args ) { return xess_get_intel_xefx_version( args ); }
static NTSTATUS wow64_xess_get_properties( void *args ) { return xess_get_properties( args ); }
static NTSTATUS wow64_xess_get_input_resolution( void *args ) { return xess_get_input_resolution( args ); }
static NTSTATUS wow64_xess_get_optimal_input_resolution( void *args ) { return xess_get_optimal_input_resolution( args ); }
static NTSTATUS wow64_xess_get_jitter_scale( void *args ) { return xess_get_jitter_scale( args ); }
static NTSTATUS wow64_xess_get_velocity_scale( void *args ) { return xess_get_velocity_scale( args ); }
static NTSTATUS wow64_xess_get_exposure_multiplier( void *args ) { return xess_get_exposure_multiplier( args ); }
static NTSTATUS wow64_xess_get_max_responsive_mask_value( void *args ) { return xess_get_max_responsive_mask_value( args ); }
static NTSTATUS wow64_xess_set_velocity_scale( void *args ) { return xess_set_velocity_scale( args ); }
static NTSTATUS wow64_xess_set_jitter_scale( void *args ) { return xess_set_jitter_scale( args ); }
static NTSTATUS wow64_xess_set_exposure_multiplier( void *args ) { return xess_set_exposure_multiplier( args ); }
static NTSTATUS wow64_xess_set_max_responsive_mask_value( void *args ) { return xess_set_max_responsive_mask_value( args ); }
static NTSTATUS wow64_xess_set_logging_callback( void *args ) { return xess_set_logging_callback( args ); }
static NTSTATUS wow64_xess_is_optimal_driver( void *args ) { return xess_is_optimal_driver( args ); }
static NTSTATUS wow64_xess_force_legacy_scale_factors( void *args ) { return xess_force_legacy_scale_factors( args ); }
static NTSTATUS wow64_xess_get_pipeline_build_status( void *args ) { return xess_get_pipeline_build_status( args ); }
static NTSTATUS wow64_xess_select_network_model( void *args ) { return xess_select_network_model( args ); }
static NTSTATUS wow64_xess_start_dump( void *args ) { return xess_start_dump( args ); }
static NTSTATUS wow64_xess_get_profiling_data( void *args ) { return xess_get_profiling_data( args ); }
static NTSTATUS wow64_xess_vk_get_required_instance_extensions( void *args ) { return xess_vk_get_required_instance_extensions( args ); }
static NTSTATUS wow64_xess_vk_get_required_device_extensions( void *args ) { return xess_vk_get_required_device_extensions( args ); }
static NTSTATUS wow64_xess_vk_get_required_device_features( void *args ) { return xess_vk_get_required_device_features( args ); }
static NTSTATUS wow64_xess_vk_create_context( void *args ) { return xess_vk_create_context( args ); }
static NTSTATUS wow64_xess_vk_build_pipelines( void *args ) { return xess_vk_build_pipelines( args ); }
static NTSTATUS wow64_xess_vk_init( void *args ) { return xess_vk_init( args ); }
static NTSTATUS wow64_xess_vk_get_init_params( void *args ) { return xess_vk_get_init_params( args ); }
static NTSTATUS wow64_xess_vk_execute( void *args ) { return xess_vk_execute( args ); }

const unixlib_entry_t __wine_unix_call_wow64_funcs[] =
{
    wow64_xess_init,
    wow64_xess_destroy_context,
    wow64_xess_get_version,
    wow64_xess_get_intel_xefx_version,
    wow64_xess_get_properties,
    wow64_xess_get_input_resolution,
    wow64_xess_get_optimal_input_resolution,
    wow64_xess_get_jitter_scale,
    wow64_xess_get_velocity_scale,
    wow64_xess_get_exposure_multiplier,
    wow64_xess_get_max_responsive_mask_value,
    wow64_xess_set_velocity_scale,
    wow64_xess_set_jitter_scale,
    wow64_xess_set_exposure_multiplier,
    wow64_xess_set_max_responsive_mask_value,
    wow64_xess_set_logging_callback,
    wow64_xess_is_optimal_driver,
    wow64_xess_force_legacy_scale_factors,
    wow64_xess_get_pipeline_build_status,
    wow64_xess_select_network_model,
    wow64_xess_start_dump,
    wow64_xess_get_profiling_data,
    wow64_xess_vk_get_required_instance_extensions,
    wow64_xess_vk_get_required_device_extensions,
    wow64_xess_vk_get_required_device_features,
    wow64_xess_vk_create_context,
    wow64_xess_vk_build_pipelines,
    wow64_xess_vk_init,
    wow64_xess_vk_get_init_params,
    wow64_xess_vk_execute,
};

#endif  /* _WIN64 */
