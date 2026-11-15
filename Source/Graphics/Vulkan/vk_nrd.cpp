//-----------------------------------------------------------------------------
//           Name: vk_nrd.cpp
//      Developer: Overgrowth Online (crxhvrd)
//    Description: See vk_nrd.h
//        License: Apache-2.0, like the rest of the engine (see LICENSE)
//-----------------------------------------------------------------------------
#include <volk.h>
#include <vk_mem_alloc.h>

#include "vk_nrd.h"

#include <Logging/logdata.h>

#if defined(OG_NRD)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <NRD.h>

#include <Graphics/Vulkan/vk_backend.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace VkNrd {
namespace {

// The denoisers (NRD identifiers)
const nrd::Identifier kSun = 1, kLights = 2, kAo = 3, kGi = 4;
// The constants' ring: four parts (frames in flight and then some), each this many passes
const int kRingParts = 4;
const uint32_t kPassesPerPart = 256;

struct Img {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation alloc = NULL;
    VkImageView view = VK_NULL_HANDLE;
};

struct Pipeline {
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;  // (the resources: pushed)
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
};

struct State {
    bool tried = false;
    bool available = false;
    nrd::Instance* instance = NULL;
    const nrd::LibraryDesc* library = NULL;
    const nrd::InstanceDesc* desc = NULL;
    std::vector<Pipeline> pipelines;
    VkSampler samplers[(size_t)nrd::Sampler::MAX_NUM] = {};
    VkDescriptorSetLayout shared_layout = VK_NULL_HANDLE;  // (the samplers and the constants)
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet shared_set = VK_NULL_HANDLE;
    VkBuffer constants = VK_NULL_HANDLE;
    VmaAllocation constants_alloc = NULL;
    uint8_t* constants_mapped = NULL;
    VkDeviceSize constants_stride = 0;
    uint32_t width = 0, height = 0;
    Img images[kImages];
    std::vector<Img> permanent, transient;
    bool fresh = false;         // images made, their layouts not set yet
    bool clear_history = true;  // (new images: their contents cleared too)
    uint32_t frame_index = 0;
};
State s;

VkFormat ImageFormat(Image image) {
    switch (image) {
        case kViewZ:
            return VK_FORMAT_R32_SFLOAT;
        case kNormalRoughness:
            return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        case kMotion:
            return VK_FORMAT_R16G16B16A16_SFLOAT;
        case kSunPenumbra:
        case kLightPenumbra:
        case kAoHitDist:
            return VK_FORMAT_R16_SFLOAT;
        case kLightTranslucency:
        case kLightShadow:
            return VK_FORMAT_R8G8B8A8_UNORM;
        case kSunShadow:
        case kAoOut:
            return VK_FORMAT_R8_UNORM;
        case kGiSh0In:
        case kGiSh1In:
        case kGiSh0Out:
        case kGiSh1Out:
            return VK_FORMAT_R16G16B16A16_SFLOAT;
        default:
            return VK_FORMAT_UNDEFINED;
    }
}

VkFormat PoolFormat(nrd::Format f) {
    switch (f) {
        case nrd::Format::R8_UNORM: return VK_FORMAT_R8_UNORM;
        case nrd::Format::R8_SNORM: return VK_FORMAT_R8_SNORM;
        case nrd::Format::R8_UINT: return VK_FORMAT_R8_UINT;
        case nrd::Format::R8_SINT: return VK_FORMAT_R8_SINT;
        case nrd::Format::RG8_UNORM: return VK_FORMAT_R8G8_UNORM;
        case nrd::Format::RG8_SNORM: return VK_FORMAT_R8G8_SNORM;
        case nrd::Format::RG8_UINT: return VK_FORMAT_R8G8_UINT;
        case nrd::Format::RG8_SINT: return VK_FORMAT_R8G8_SINT;
        case nrd::Format::RGBA8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
        case nrd::Format::RGBA8_SNORM: return VK_FORMAT_R8G8B8A8_SNORM;
        case nrd::Format::RGBA8_UINT: return VK_FORMAT_R8G8B8A8_UINT;
        case nrd::Format::RGBA8_SINT: return VK_FORMAT_R8G8B8A8_SINT;
        case nrd::Format::RGBA8_SRGB: return VK_FORMAT_R8G8B8A8_SRGB;
        case nrd::Format::R16_UNORM: return VK_FORMAT_R16_UNORM;
        case nrd::Format::R16_SNORM: return VK_FORMAT_R16_SNORM;
        case nrd::Format::R16_UINT: return VK_FORMAT_R16_UINT;
        case nrd::Format::R16_SINT: return VK_FORMAT_R16_SINT;
        case nrd::Format::R16_SFLOAT: return VK_FORMAT_R16_SFLOAT;
        case nrd::Format::RG16_UNORM: return VK_FORMAT_R16G16_UNORM;
        case nrd::Format::RG16_SNORM: return VK_FORMAT_R16G16_SNORM;
        case nrd::Format::RG16_UINT: return VK_FORMAT_R16G16_UINT;
        case nrd::Format::RG16_SINT: return VK_FORMAT_R16G16_SINT;
        case nrd::Format::RG16_SFLOAT: return VK_FORMAT_R16G16_SFLOAT;
        case nrd::Format::RGBA16_UNORM: return VK_FORMAT_R16G16B16A16_UNORM;
        case nrd::Format::RGBA16_SNORM: return VK_FORMAT_R16G16B16A16_SNORM;
        case nrd::Format::RGBA16_UINT: return VK_FORMAT_R16G16B16A16_UINT;
        case nrd::Format::RGBA16_SINT: return VK_FORMAT_R16G16B16A16_SINT;
        case nrd::Format::RGBA16_SFLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
        case nrd::Format::R32_UINT: return VK_FORMAT_R32_UINT;
        case nrd::Format::R32_SINT: return VK_FORMAT_R32_SINT;
        case nrd::Format::R32_SFLOAT: return VK_FORMAT_R32_SFLOAT;
        case nrd::Format::RG32_UINT: return VK_FORMAT_R32G32_UINT;
        case nrd::Format::RG32_SINT: return VK_FORMAT_R32G32_SINT;
        case nrd::Format::RG32_SFLOAT: return VK_FORMAT_R32G32_SFLOAT;
        case nrd::Format::RGB32_UINT: return VK_FORMAT_R32G32B32_UINT;
        case nrd::Format::RGB32_SINT: return VK_FORMAT_R32G32B32_SINT;
        case nrd::Format::RGB32_SFLOAT: return VK_FORMAT_R32G32B32_SFLOAT;
        case nrd::Format::RGBA32_UINT: return VK_FORMAT_R32G32B32A32_UINT;
        case nrd::Format::RGBA32_SINT: return VK_FORMAT_R32G32B32A32_SINT;
        case nrd::Format::RGBA32_SFLOAT: return VK_FORMAT_R32G32B32A32_SFLOAT;
        case nrd::Format::R10_G10_B10_A2_UNORM: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        case nrd::Format::R10_G10_B10_A2_UINT: return VK_FORMAT_A2B10G10R10_UINT_PACK32;
        case nrd::Format::R11_G11_B10_UFLOAT: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
        case nrd::Format::R9_G9_B9_E5_UFLOAT: return VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
        default: return VK_FORMAT_UNDEFINED;
    }
}

bool MakeImage(uint32_t width, uint32_t height, VkFormat format, Img* out) {
    VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = format;
    ii.extent = {std::max(1u, width), std::max(1u, height), 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo ai = {};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (vmaCreateImage(VkBackend::Allocator(), &ii, &ai, &out->image, &out->alloc, NULL) != VK_SUCCESS) {
        *out = Img();
        return false;
    }
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = out->image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(VkBackend::Device(), &vi, NULL, &out->view) != VK_SUCCESS) {
        vmaDestroyImage(VkBackend::Allocator(), out->image, out->alloc);
        *out = Img();
        return false;
    }
    return true;
}

// Once the GPU is done with it (now: at shutdown)
void FreeImage(Img* img, bool now) {
    if (img->image != VK_NULL_HANDLE) {
        VkImage image = img->image;
        VmaAllocation alloc = img->alloc;
        VkImageView view = img->view;
        auto destroy = [image, alloc, view]() {
            vkDestroyImageView(VkBackend::Device(), view, NULL);
            vmaDestroyImage(VkBackend::Allocator(), image, alloc);
        };
        if (now) {
            destroy();
        } else {
            VkBackend::DeferDestroy(destroy);
        }
    }
    *img = Img();
}

void FreeImages(bool now) {
    for (Img& img : s.images) {
        FreeImage(&img, now);
    }
    for (Img& img : s.permanent) {
        FreeImage(&img, now);
    }
    for (Img& img : s.transient) {
        FreeImage(&img, now);
    }
    s.permanent.clear();
    s.transient.clear();
    s.width = s.height = 0;
}

void Shutdown() {
    VkDevice dev = VkBackend::Device();
    FreeImages(true);
    for (Pipeline& p : s.pipelines) {
        if (p.pipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(dev, p.pipeline, NULL);
        }
        if (p.layout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(dev, p.layout, NULL);
        }
        if (p.set_layout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(dev, p.set_layout, NULL);
        }
    }
    s.pipelines.clear();
    if (s.pool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(dev, s.pool, NULL);
    }
    if (s.shared_layout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(dev, s.shared_layout, NULL);
    }
    for (VkSampler& sampler : s.samplers) {
        if (sampler != VK_NULL_HANDLE) {
            vkDestroySampler(dev, sampler, NULL);
        }
        sampler = VK_NULL_HANDLE;
    }
    if (s.constants != VK_NULL_HANDLE) {
        vmaDestroyBuffer(VkBackend::Allocator(), s.constants, s.constants_alloc);
    }
    if (s.instance != NULL) {
        nrd::DestroyInstance(*s.instance);
    }
    s = State();
    s.tried = true;  // (not again: the device is going)
}

// The library (delay-loaded: the game starts without it), the denoisers, their passes
bool Start() {
    s.tried = true;
    // (from the game's own folder, not the DLL search path: the delay-load helper then finds
    // this module already loaded by its name)
    char exe[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameA(NULL, exe, MAX_PATH);
    std::string dll = "NRD.dll";
    if (n > 0 && n < MAX_PATH) {
        dll = exe;
        const size_t slash = dll.find_last_of("\\/");
        dll = (slash == std::string::npos ? std::string() : dll.substr(0, slash + 1)) + "NRD.dll";
    }
    if (LoadLibraryExA(dll.c_str(), NULL, LOAD_WITH_ALTERED_SEARCH_PATH) == NULL) {
        LOGI << "NRD: no NRD.dll beside the game: our own denoiser" << std::endl;
        return false;
    }
    s.library = nrd::GetLibraryDesc();
    if (s.library == NULL || s.library->versionMajor != NRD_VERSION_MAJOR || s.library->versionMinor != NRD_VERSION_MINOR ||
        s.library->normalEncoding != nrd::NormalEncoding::R10_G10_B10_A2_UNORM || s.library->roughnessEncoding != nrd::RoughnessEncoding::LINEAR) {
        LOGE << "NRD: NRD.dll isn't the version or encoding built against (" << NRD_VERSION_MAJOR << "." << NRD_VERSION_MINOR
             << ", R10G10B10A2 normals, linear roughness)" << std::endl;
        return false;
    }
    if (VkBackend::Properties().limits.maxPushConstantsSize < 224) {
        LOGI << "NRD: the device's push constants are too small for our passes" << std::endl;
        return false;
    }
    const nrd::DenoiserDesc denoisers[] = {
        {kSun, nrd::Denoiser::SIGMA_SHADOW},
        {kLights, nrd::Denoiser::SIGMA_SHADOW_TRANSLUCENCY},
        {kAo, nrd::Denoiser::REBLUR_DIFFUSE_OCCLUSION},
        {kGi, nrd::Denoiser::REBLUR_DIFFUSE_SH},
    };
    nrd::InstanceCreationDesc ci = {};
    ci.denoisers = denoisers;
    ci.denoisersNum = 4;
    if (nrd::CreateInstance(ci, s.instance) != nrd::Result::SUCCESS || s.instance == NULL) {
        LOGE << "NRD: couldn't make its instance" << std::endl;
        s.instance = NULL;
        return false;
    }
    static bool hooked = false;
    if (!hooked) {
        hooked = true;
        VkBackend::OnShutdown(&Shutdown);
    }
    s.desc = nrd::GetInstanceDesc(*s.instance);
    const nrd::InstanceDesc& d = *s.desc;
    if (!((d.resourcesSpaceIndex == 0 && d.constantBufferAndSamplersSpaceIndex == 1) ||
          (d.resourcesSpaceIndex == 1 && d.constantBufferAndSamplersSpaceIndex == 0)) ||
        d.samplersNum > (uint32_t)nrd::Sampler::MAX_NUM) {
        LOGE << "NRD: register spaces not as expected" << std::endl;
        Shutdown();
        s.tried = true;
        return false;
    }
    VkDevice dev = VkBackend::Device();
    const nrd::SPIRVBindingOffsets& offsets = s.library->spirvBindingOffsets;

    // The samplers (immutable) and the constants (a dynamic offset into the ring)
    std::vector<VkDescriptorSetLayoutBinding> shared;
    for (uint32_t i = 0; i < d.samplersNum; ++i) {
        VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        const bool linear = d.samplers[i] == nrd::Sampler::LINEAR_CLAMP;
        si.magFilter = si.minFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.maxLod = 0.0f;
        if (vkCreateSampler(dev, &si, NULL, &s.samplers[i]) != VK_SUCCESS) {
            Shutdown();
            s.tried = true;
            return false;
        }
        VkDescriptorSetLayoutBinding b = {};
        b.binding = offsets.samplerOffset + d.samplersBaseRegisterIndex + i;
        b.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        b.descriptorCount = 1;
        b.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        b.pImmutableSamplers = &s.samplers[i];
        shared.push_back(b);
    }
    VkDescriptorSetLayoutBinding cb = {};
    cb.binding = offsets.constantBufferOffset + d.constantBufferRegisterIndex;
    cb.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    cb.descriptorCount = 1;
    cb.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    shared.push_back(cb);
    VkDescriptorSetLayoutCreateInfo sli = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sli.bindingCount = (uint32_t)shared.size();
    sli.pBindings = shared.data();
    VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_SAMPLER, std::max(1u, d.samplersNum)}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1}};
    VkDescriptorPoolCreateInfo pi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = 1;
    pi.poolSizeCount = 2;
    pi.pPoolSizes = sizes;
    const VkDeviceSize align = std::max<VkDeviceSize>(VkBackend::Properties().limits.minUniformBufferOffsetAlignment, 16);
    s.constants_stride = (std::max<VkDeviceSize>(d.constantBufferMaxDataSize, 16) + align - 1) / align * align;
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = s.constants_stride * kPassesPerPart * kRingParts;
    bi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    VmaAllocationCreateInfo bai = {};
    bai.usage = VMA_MEMORY_USAGE_AUTO;
    bai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo bainfo = {};
    if (vkCreateDescriptorSetLayout(dev, &sli, NULL, &s.shared_layout) != VK_SUCCESS || vkCreateDescriptorPool(dev, &pi, NULL, &s.pool) != VK_SUCCESS ||
        vmaCreateBuffer(VkBackend::Allocator(), &bi, &bai, &s.constants, &s.constants_alloc, &bainfo) != VK_SUCCESS || bainfo.pMappedData == NULL) {
        LOGE << "NRD: couldn't make its constants" << std::endl;
        Shutdown();
        s.tried = true;
        return false;
    }
    s.constants_mapped = (uint8_t*)bainfo.pMappedData;
    VkDescriptorSetAllocateInfo dai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool = s.pool;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &s.shared_layout;
    if (vkAllocateDescriptorSets(dev, &dai, &s.shared_set) != VK_SUCCESS) {
        Shutdown();
        s.tried = true;
        return false;
    }
    VkDescriptorBufferInfo cbi = {s.constants, 0, std::max<VkDeviceSize>(d.constantBufferMaxDataSize, 16)};
    VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = s.shared_set;
    w.dstBinding = cb.binding;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    w.pBufferInfo = &cbi;
    vkUpdateDescriptorSets(dev, 1, &w, 0, NULL);

    // Each pass: its textures (sampled) then its storage images, pushed
    s.pipelines.resize(d.pipelinesNum);
    for (uint32_t p = 0; p < d.pipelinesNum; ++p) {
        const nrd::PipelineDesc& pd = d.pipelines[p];
        Pipeline& out = s.pipelines[p];
        std::vector<VkDescriptorSetLayoutBinding> bindings;
        for (uint32_t r = 0; r < pd.resourceRangesNum; ++r) {
            const nrd::ResourceRangeDesc& range = pd.resourceRanges[r];
            const bool storage = range.descriptorType == nrd::DescriptorType::STORAGE_TEXTURE;
            for (uint32_t i = 0; i < range.descriptorsNum; ++i) {
                VkDescriptorSetLayoutBinding b = {};
                b.binding = (storage ? offsets.storageTextureAndBufferOffset : offsets.textureOffset) + d.resourcesBaseRegisterIndex + i;
                b.descriptorType = storage ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
                b.descriptorCount = 1;
                b.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
                bindings.push_back(b);
            }
        }
        if (bindings.size() > VkBackend::MaxPushDescriptors()) {
            LOGE << "NRD: a pass needs more pushed descriptors than the device has (" << bindings.size() << ")" << std::endl;
            Shutdown();
            s.tried = true;
            return false;
        }
        VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
        li.bindingCount = (uint32_t)bindings.size();
        li.pBindings = bindings.data();
        VkDescriptorSetLayout sets[2];
        sets[d.resourcesSpaceIndex] = VK_NULL_HANDLE;
        if (vkCreateDescriptorSetLayout(dev, &li, NULL, &out.set_layout) != VK_SUCCESS) {
            Shutdown();
            s.tried = true;
            return false;
        }
        sets[d.resourcesSpaceIndex] = out.set_layout;
        sets[d.constantBufferAndSamplersSpaceIndex] = s.shared_layout;
        VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 2;
        pli.pSetLayouts = sets;
        VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mi.codeSize = (size_t)pd.computeShaderSPIRV.size;
        mi.pCode = (const uint32_t*)pd.computeShaderSPIRV.bytecode;
        VkShaderModule module = VK_NULL_HANDLE;
        if (pd.computeShaderSPIRV.bytecode == NULL || vkCreatePipelineLayout(dev, &pli, NULL, &out.layout) != VK_SUCCESS ||
            vkCreateShaderModule(dev, &mi, NULL, &module) != VK_SUCCESS) {
            LOGE << "NRD: couldn't make pass " << pd.shaderIdentifier << std::endl;
            Shutdown();
            s.tried = true;
            return false;
        }
        VkComputePipelineCreateInfo cpi = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        cpi.stage.module = module;
        cpi.stage.pName = d.shaderEntryPoint;
        cpi.layout = out.layout;
        const VkResult made = vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, NULL, &out.pipeline);
        vkDestroyShaderModule(dev, module, NULL);
        if (made != VK_SUCCESS) {
            LOGE << "NRD: couldn't make pass " << pd.shaderIdentifier << std::endl;
            out.pipeline = VK_NULL_HANDLE;
            Shutdown();
            s.tried = true;
            return false;
        }
    }
    s.available = true;
    LOGI << "NRD " << (int)s.library->versionMajor << "." << (int)s.library->versionMinor << "." << (int)s.library->versionBuild << ": "
         << d.pipelinesNum << " passes (SIGMA for the sun's and the lights' shadows, REBLUR for the AO and GI)" << std::endl;
    return true;
}

// The image a pass's resource names (NULL: none)
VkImageView Resolve(nrd::Identifier id, const nrd::ResourceDesc& r) {
    switch (r.type) {
        case nrd::ResourceType::IN_MV:
            return s.images[kMotion].view;
        case nrd::ResourceType::IN_NORMAL_ROUGHNESS:
            return s.images[kNormalRoughness].view;
        case nrd::ResourceType::IN_VIEWZ:
            return s.images[kViewZ].view;
        case nrd::ResourceType::IN_PENUMBRA:
            return s.images[id == kSun ? kSunPenumbra : kLightPenumbra].view;
        case nrd::ResourceType::IN_TRANSLUCENCY:
            return s.images[kLightTranslucency].view;
        case nrd::ResourceType::OUT_SHADOW_TRANSLUCENCY:
            return s.images[id == kSun ? kSunShadow : kLightShadow].view;
        case nrd::ResourceType::IN_DIFF_HITDIST:
            return s.images[kAoHitDist].view;
        case nrd::ResourceType::OUT_DIFF_HITDIST:
            return s.images[kAoOut].view;
        case nrd::ResourceType::IN_DIFF_SH0:
            return s.images[kGiSh0In].view;
        case nrd::ResourceType::IN_DIFF_SH1:
            return s.images[kGiSh1In].view;
        case nrd::ResourceType::OUT_DIFF_SH0:
            return s.images[kGiSh0Out].view;
        case nrd::ResourceType::OUT_DIFF_SH1:
            return s.images[kGiSh1Out].view;
        case nrd::ResourceType::TRANSIENT_POOL:
            return r.indexInPool < s.transient.size() ? s.transient[r.indexInPool].view : VK_NULL_HANDLE;
        case nrd::ResourceType::PERMANENT_POOL:
            return r.indexInPool < s.permanent.size() ? s.permanent[r.indexInPool].view : VK_NULL_HANDLE;
        default:
            return VK_NULL_HANDLE;
    }
}

void ComputeBarrier(VkCommandBuffer cmd) {
    VkMemoryBarrier2 b = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    b.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    b.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
    VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}

}  // namespace

bool Available() {
    if (!s.tried) {
        Start();
    }
    return s.available;
}

bool Prepare(VkCommandBuffer cmd, uint32_t width, uint32_t height) {
    if (!Available() || width == 0 || height == 0) {
        return false;
    }
    if (width != s.width || height != s.height) {
        FreeImages(false);
        bool ok = true;
        for (int i = 0; i < kImages && ok; ++i) {
            ok = MakeImage(width, height, ImageFormat((Image)i), &s.images[i]);
        }
        const nrd::InstanceDesc& d = *s.desc;
        for (uint32_t i = 0; i < d.permanentPoolSize && ok; ++i) {
            const uint32_t f = std::max<uint32_t>(1, d.permanentPool[i].downsampleFactor);
            s.permanent.push_back(Img());
            ok = MakeImage((width + f - 1) / f, (height + f - 1) / f, PoolFormat(d.permanentPool[i].format), &s.permanent.back());
        }
        for (uint32_t i = 0; i < d.transientPoolSize && ok; ++i) {
            const uint32_t f = std::max<uint32_t>(1, d.transientPool[i].downsampleFactor);
            s.transient.push_back(Img());
            ok = MakeImage((width + f - 1) / f, (height + f - 1) / f, PoolFormat(d.transientPool[i].format), &s.transient.back());
        }
        if (!ok) {
            LOGE << "NRD: couldn't make its images at " << width << "x" << height << std::endl;
            FreeImages(false);
            return false;
        }
        s.width = width;
        s.height = height;
        s.fresh = true;
        s.clear_history = true;
    }
    if (s.fresh) {
        // (every image to GENERAL, where it stays; NRD clears its history the first time)
        std::vector<VkImageMemoryBarrier2> barriers;
        auto add = [&barriers](const Img& img) {
            VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            b.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
            b.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            b.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = img.image;
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            barriers.push_back(b);
        };
        for (const Img& img : s.images) {
            add(img);
        }
        for (const Img& img : s.permanent) {
            add(img);
        }
        for (const Img& img : s.transient) {
            add(img);
        }
        VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = (uint32_t)barriers.size();
        dep.pImageMemoryBarriers = barriers.data();
        vkCmdPipelineBarrier2(cmd, &dep);
        s.fresh = false;
    }
    return true;
}

VkImageView View(Image image) {
    return image >= 0 && image < kImages ? s.images[image].view : VK_NULL_HANDLE;
}

uint32_t FrameIndex() {
    return s.frame_index;
}

bool Denoise(VkCommandBuffer cmd, const Frame& f, int ring) {
    if (!s.available || s.width != f.width || s.height != f.height || (!f.sun_shadow && !f.ao && !f.lights && !f.gi)) {
        return false;
    }
    nrd::CommonSettings common = {};
    memcpy(common.viewToClipMatrix, f.proj, sizeof(common.viewToClipMatrix));
    memcpy(common.viewToClipMatrixPrev, f.proj_prev, sizeof(common.viewToClipMatrixPrev));
    memcpy(common.worldToViewMatrix, f.view, sizeof(common.worldToViewMatrix));
    memcpy(common.worldToViewMatrixPrev, f.view_prev, sizeof(common.worldToViewMatrixPrev));
    // (2.5D motion: uv as is, the view Z's change in z)
    common.motionVectorScale[0] = 1.0f;
    common.motionVectorScale[1] = 1.0f;
    common.motionVectorScale[2] = 1.0f;
    common.resourceSize[0] = common.resourceSizePrev[0] = common.rectSize[0] = common.rectSizePrev[0] = (uint16_t)f.width;
    common.resourceSize[1] = common.resourceSizePrev[1] = common.rectSize[1] = common.rectSizePrev[1] = (uint16_t)f.height;
    common.denoisingRange = f.denoising_range;
    common.frameIndex = s.frame_index++;
    common.accumulationMode = s.clear_history ? nrd::AccumulationMode::CLEAR_AND_RESTART
                                              : f.reset ? nrd::AccumulationMode::RESTART : nrd::AccumulationMode::CONTINUE;
    s.clear_history = false;
    if (nrd::SetCommonSettings(*s.instance, common) != nrd::Result::SUCCESS) {
        return false;
    }
    nrd::SigmaSettings sun = {};
    sun.lightDirection[0] = f.sun[0];
    sun.lightDirection[1] = f.sun[1];
    sun.lightDirection[2] = f.sun[2];
    nrd::SigmaSettings lights = {};  // (local lights: no direction)
    nrd::ReblurSettings ao = {};
    // (the hit distance normalized by the AO's reach alone: rt_shadow.comp's AO)
    ao.hitDistanceParameters.A = std::max(0.01f, f.ao_range);
    ao.hitDistanceParameters.B = 0.0f;
    ao.hitDistanceParameters.C = 1.0f;
    nrd::SetDenoiserSettings(*s.instance, kSun, &sun);
    nrd::SetDenoiserSettings(*s.instance, kLights, &lights);
    nrd::SetDenoiserSettings(*s.instance, kAo, &ao);
    nrd::ReblurSettings gi = {};  // (its hit distances normalized as rt_nrd.comp #PREPARE does: the defaults)
    gi.checkerboardMode = nrd::CheckerboardMode::BLACK;  // (rt_gi.comp traces half the pixels, packed to the left half)
    nrd::SetDenoiserSettings(*s.instance, kGi, &gi);

    nrd::Identifier ids[4];
    uint32_t n = 0;
    if (f.sun_shadow) {
        ids[n++] = kSun;
    }
    if (f.lights) {
        ids[n++] = kLights;
    }
    if (f.ao) {
        ids[n++] = kAo;
    }
    if (f.gi) {
        ids[n++] = kGi;
    }
    const nrd::DispatchDesc* dispatches = NULL;
    uint32_t count = 0;
    if (nrd::GetComputeDispatches(*s.instance, ids, n, dispatches, count) != nrd::Result::SUCCESS) {
        return false;
    }
    if (count > kPassesPerPart) {
        LOGE << "NRD: more passes than its constants hold (" << count << ")" << std::endl;
        return false;
    }
    const nrd::InstanceDesc& d = *s.desc;
    const nrd::SPIRVBindingOffsets& offsets = s.library->spirvBindingOffsets;
    const VkDeviceSize part = (VkDeviceSize)(ring % kRingParts) * kPassesPerPart * s.constants_stride;
    // (every pass's resources ours, checked before any is recorded: a pass left out halfway
    // would leave the ones after it reading what it never wrote)
    for (uint32_t i = 0; i < count; ++i) {
        const nrd::DispatchDesc& dd = dispatches[i];
        for (uint32_t r = 0; r < dd.resourcesNum; ++r) {
            if (Resolve(dd.identifier, dd.resources[r]) == VK_NULL_HANDLE) {
                static bool logged = false;
                if (!logged) {
                    logged = true;
                    LOGE << "NRD: pass " << (dd.name ? dd.name : "?") << " names a resource we don't have: our own denoiser" << std::endl;
                }
                return false;
            }
        }
    }
    VkBackend::BeginLabel("NRD");
    ComputeBarrier(cmd);  // (the inputs written)
    std::vector<VkDescriptorImageInfo> infos;
    std::vector<VkWriteDescriptorSet> writes;
    for (uint32_t i = 0; i < count; ++i) {
        const nrd::DispatchDesc& dd = dispatches[i];
        const Pipeline& p = s.pipelines[dd.pipelineIndex];
        const VkDeviceSize offset = part + i * s.constants_stride;
        if (dd.constantBufferDataSize > 0) {
            memcpy(s.constants_mapped + offset, dd.constantBufferData, dd.constantBufferDataSize);
        }
        infos.assign(dd.resourcesNum, VkDescriptorImageInfo());
        writes.assign(dd.resourcesNum, VkWriteDescriptorSet());
        uint32_t textures = 0, storages = 0;
        bool ok = true;
        for (uint32_t r = 0; r < dd.resourcesNum; ++r) {
            const nrd::ResourceDesc& rd = dd.resources[r];
            const bool storage = rd.descriptorType == nrd::DescriptorType::STORAGE_TEXTURE;
            infos[r] = {VK_NULL_HANDLE, Resolve(dd.identifier, rd), VK_IMAGE_LAYOUT_GENERAL};
            ok = ok && infos[r].imageView != VK_NULL_HANDLE;
            VkWriteDescriptorSet& w = writes[r];
            w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.dstBinding = (storage ? offsets.storageTextureAndBufferOffset + storages++ : offsets.textureOffset + textures++) + d.resourcesBaseRegisterIndex;
            w.descriptorCount = 1;
            w.descriptorType = storage ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            w.pImageInfo = &infos[r];
        }
        if (!ok) {
            static bool logged = false;
            if (!logged) {
                logged = true;
                LOGE << "NRD: pass " << (dd.name ? dd.name : "?") << " names a resource we don't have" << std::endl;
            }
            continue;
        }
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline);
        const uint32_t dynamic_offset = (uint32_t)offset;
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.layout, d.constantBufferAndSamplersSpaceIndex, 1, &s.shared_set, 1,
                                &dynamic_offset);
        if (!writes.empty()) {
            vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.layout, d.resourcesSpaceIndex, (uint32_t)writes.size(), writes.data());
        }
        vkCmdDispatch(cmd, dd.gridWidth, dd.gridHeight, 1);
        ComputeBarrier(cmd);
    }
    vmaFlushAllocation(VkBackend::Allocator(), s.constants_alloc, part, (VkDeviceSize)count * s.constants_stride);
    VkBackend::EndLabel();
    return true;
}

}  // namespace VkNrd

#else  // ^ OG_NRD: built without it

namespace VkNrd {

bool Available() {
    return false;
}

bool Prepare(VkCommandBuffer, uint32_t, uint32_t) {
    return false;
}

VkImageView View(Image) {
    return VK_NULL_HANDLE;
}

uint32_t FrameIndex() {
    return 0;
}

bool Denoise(VkCommandBuffer, const Frame&, int) {
    return false;
}

}  // namespace VkNrd

#endif
