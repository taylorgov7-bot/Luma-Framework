#define GAME_DEUS_EX_MANKIND_DIVIDED 1

#define CHECK_GRAPHICS_API_COMPATIBILITY 1

// Any message box breaks the input of the game forever
#define DISABLE_AUTO_DEBUGGER 1

#define DISABLE_FOCUS_LOSS_SUPPRESSION 1

#define AVOID_INPUT_LOSS 1

#define ENABLE_AUTO_CBUFFER_RESTORATION 1

#include "..\..\Core\core.hpp"

#include <fstream>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// TODO: Fix this globaly? Define NOMINMAX before including windows.h.
#undef min
#undef max

// Original DLSS integration by Garamond.
// This version is DLSS-only: all HDR, swapchain and texture format upgrades are disabled so the game keeps its vanilla look.

struct alignas(16) cbSharedPerViewData
{
    float4x4    mProjection;
    float4x4    mProjectionInverse;
    float4x4    mViewToViewport;
    float4x4    mWorldToView;
    float4x4    mViewToWorld;
    float4    vViewRemap;
    float4    vViewDepthRemap;
    float4    vEyeVectorUL;
    float4    vEyeVectorLR;
    float4    vViewSpaceUpVector;
    float4    vCheckerModeParams;
    float4    vViewportSize;
    float4    vEngineTime;
    float4    vPrecipitations;
    float4    vClipPlane;
    float4    vExtraParams;
    float4    vScatteringParams;
    float4    vStereoscopic3DCorrectionParams;
    float4    vCHSParams;
};

namespace
{
    // TAA compute shader bindings (from the decompiled shader):
    // t0 = depth, t1 = scene color (sqrt encoded), t2 = motion vectors (UV space), t3 = history, u0 = output (linear, color^2), cb11[0] = (1/size, size).
    const ShaderHashesList shader_hashes_TAA = { .compute_shaders = { 0x84EF14ED } };
    const ShaderHashesList shader_hashes_LastKnownLocation { .pixel_shaders = { 0xA6A0E453, 0xE30A40E6 } };

    void* g_cbSharedPerViewData_mapped_data;
    uintptr_t g_cbSharedPerViewData_handle;
    cbSharedPerViewData g_cbSharedPerViewData_data;

    bool g_disable_last_known_location;

    // Per frame status, shown in the settings UI
    std::atomic<bool> g_taa_seen_this_frame = false;
    std::atomic<bool> g_taa_seen_last_frame = false;
    std::atomic<bool> g_dlss_ran_last_frame = false;
    std::atomic<bool> g_dlss_ran_this_frame = false;
    uint2 g_last_render_size = { 0, 0 };
    uint2 g_last_output_size = { 0, 0 };

    // Render resolution scaling (experimental)
    //
    // The game has no resolution scaling of its own, so we emulate it:
    // - Every screen-proportional render target/depth/UAV texture (full, 1/2, 1/4, 1/8 of the swapchain size) is created smaller,
    //   so everything before TAA renders at the lower resolution. Viewports/scissors are scaled to match,
    //   and the screen size constants the game uploads (e.g. 1/2560, 1/1440, 2560, 1440) are patched to the new sizes.
    // - DLSS upscales into a full resolution substitute of the TAA output.
    // - After TAA (UI, film grain, final), every resized texture the game binds is swapped for a full resolution substitute.
    // The scale is decided when textures are created, so changes need a game restart.
    struct ScalePresetInfo { const char* name; float scale; };
    constexpr ScalePresetInfo k_scale_presets[] = {
        { "DLAA (native resolution)", 1.f },
        { "Quality (67%)", 2.f / 3.f },
        { "Balanced (58%)", 0.58f },
        { "Performance (50%)", 0.5f },
        { "Ultra Performance (33%)", 1.f / 3.f },
    };
    constexpr int k_scale_presets_count = (int)std::size(k_scale_presets);
    int g_selected_scale_preset = 0; // What the user picked (saved in the config)
    int g_active_scale_preset = 0; // What the textures were created with this session (applied at startup)

    float GetActiveScale() { return k_scale_presets[std::clamp(g_active_scale_preset, 0, k_scale_presets_count - 1)].scale; }
    bool IsScalingActive() { return GetActiveScale() < 1.f; }

    // The new size of a screen-proportional dimension (consistent between textures, viewports and constants)
    uint32_t ScaleDimension(uint32_t value, uint32_t full, uint32_t scaled_full)
    {
        if (full == 0) return value;
        return (std::max)(1u, (uint32_t)std::lround((double)value * (double)scaled_full / (double)full));
    }
    uint32_t GetScaledFullDimension(uint32_t full)
    {
        return (std::max)(1u, (uint32_t)std::lround((double)full * (double)GetActiveScale()));
    }

    // Returns the scaled size of a texture dimension if it's derived from the screen size "full" in one of the ways the game does it, 0 otherwise:
    // - fractions of the screen: full * (1, 2 or 3/4) / 2^k (k = 0..7), rounded down or up (render targets, bloom/depth of field chains, atlases)
    // - tile rows: m * ceil(full / (16 * 2^k)), with m = 4 or 5 (depth of field tile data, which stores 4 or 5 rows per 16 pixels tile)
    // Some values are ambiguous (e.g. 180 is both 1440/8 and 4 * ceil(1440/32)), so integer textures (the tile data) try the tile rows rule first.
    uint32_t MatchScreenDerivedDimension(uint32_t value, uint32_t full, bool prefer_tiles)
    {
        if (value == 0 || full == 0) return 0;
        const uint32_t scaled_full = GetScaledFullDimension(full);

        auto MatchFractions = [&]() -> uint32_t
        {
            struct Multiplier { uint32_t num, den; };
            constexpr Multiplier multipliers[] = { { 1, 1 }, { 2, 1 }, { 3, 4 } };
            for (uint32_t k = 0; k <= 7; ++k)
            {
                const uint64_t div = 1ull << k;
                for (const auto& m : multipliers)
                {
                    const uint64_t numerator = (uint64_t)full * m.num, denominator = (uint64_t)m.den * div;
                    const uint64_t v_floor = numerator / denominator, v_ceil = (numerator + denominator - 1) / denominator;
                    if (value == v_floor || value == v_ceil)
                        return (std::max)(1u, (uint32_t)std::lround((double)value * (double)scaled_full / (double)full));
                }
            }
            return 0;
        };
        auto MatchTiles = [&]() -> uint32_t
        {
            for (uint32_t k = 0; k <= 7; ++k)
            {
                const uint64_t tile = 16ull << k;
                const uint64_t tiles = ((uint64_t)full + tile - 1) / tile;
                const uint64_t scaled_tiles = ((uint64_t)scaled_full + tile - 1) / tile;
                for (uint32_t rows : { 4u, 5u })
                {
                    if (value == rows * tiles)
                        return (uint32_t)(rows * scaled_tiles);
                }
            }
            return 0;
        };
        if (prefer_tiles)
        {
            if (uint32_t result = MatchTiles()) return result;
            return MatchFractions();
        }
        if (uint32_t result = MatchFractions()) return result;
        return MatchTiles();
    }

    // Base (swapchain) size the resizing was computed against
    std::atomic<uint32_t> g_scale_base_width = 0;
    std::atomic<uint32_t> g_scale_base_height = 0;

    struct ResizedResourceInfo
    {
        uint32_t original_width = 0, original_height = 0;
        uint32_t scaled_width = 0, scaled_height = 0;
    };
    std::shared_mutex g_resized_mutex;
    std::unordered_map<uint64_t, ResizedResourceInfo> g_resized_resources;
    std::atomic<uint32_t> g_resized_count = 0;
    thread_local bool t_pending_resize = false;
    thread_local ResizedResourceInfo t_pending_resize_info;

    bool GetResizedInfo(ID3D11Resource* resource, ResizedResourceInfo& out)
    {
        if (!resource) return false;
        std::shared_lock lock(g_resized_mutex);
        auto it = g_resized_resources.find((uint64_t)resource);
        if (it == g_resized_resources.end()) return false;
        out = it->second;
        return true;
    }

    // Full resolution substitutes of resized textures, used after TAA
    struct Substitute
    {
        ComPtr<ID3D11Texture2D> texture;
        uint64_t written_frame = UINT64_MAX;
        uint64_t cleared_frame = UINT64_MAX;
    };
    std::mutex g_substitutes_mutex; // Only used on the immediate context thread, but resources can be destroyed from any thread
    std::unordered_map<uint64_t, Substitute> g_substitutes; // By original resource
    // Substitute views, by original view (each also remembers its original resource, to clean up)
    template <typename T> struct SubstituteView { ComPtr<T> view; uint64_t original_resource = 0; };
    std::unordered_map<uint64_t, SubstituteView<ID3D11RenderTargetView>> g_substitute_rtvs;
    std::unordered_map<uint64_t, SubstituteView<ID3D11DepthStencilView>> g_substitute_dsvs;
    std::unordered_map<uint64_t, SubstituteView<ID3D11ShaderResourceView>> g_substitute_srvs;
    std::unordered_map<uint64_t, SubstituteView<ID3D11UnorderedAccessView>> g_substitute_uavs;

    std::atomic<uint64_t> g_frame_index = 0;
    // True on the immediate context between the game's TAA pass and the end of the frame
    std::atomic<bool> g_post_taa_phase = false;
    std::atomic<DWORD> g_immediate_thread_id = 0;

    // Constant buffer writes are patched everywhere, except on the immediate context thread after TAA (post processing/UI run at full resolution).
    bool ShouldPatchConstants()
    {
        if (!IsScalingActive()) return false;
        return !(g_post_taa_phase && GetCurrentThreadId() == g_immediate_thread_id);
    }

    // Replaces adjacent (width, height) and (1/width, 1/height) pairs of screen-proportional sizes with the scaled ones
    void PatchScreenSizeConstants(float* data, size_t float_count)
    {
        const uint32_t base_w = g_scale_base_width, base_h = g_scale_base_height;
        if (base_w == 0 || base_h == 0 || float_count < 2) return;
        const uint32_t scaled_w = GetScaledFullDimension(base_w), scaled_h = GetScaledFullDimension(base_h);

        auto NearlyEqual = [](float a, float b) { return std::abs(a - b) <= std::abs(b) * 1e-5f; };
        for (size_t i = 0; i + 1 < float_count; ++i)
        {
            const float x = data[i], y = data[i + 1];
            if (x <= 0.f || y <= 0.f) continue;
            for (uint32_t k = 0; k <= 5 /*k_max_patched_fraction_shift*/; ++k)
            {
                const uint32_t w = base_w >> k, h = base_h >> k;
                if (w == 0 || h == 0) break;
                const uint32_t new_w = ScaleDimension(w, base_w, scaled_w), new_h = ScaleDimension(h, base_h, scaled_h);
                if (x == (float)w && y == (float)h)
                {
                    data[i] = (float)new_w; data[i + 1] = (float)new_h;
                    ++i; break;
                }
                if (NearlyEqual(x, 1.f / (float)w) && NearlyEqual(y, 1.f / (float)h))
                {
                    data[i] = 1.f / (float)new_w; data[i + 1] = 1.f / (float)new_h;
                    ++i; break;
                }
            }
        }
    }

    // Pending constant buffer maps, per thread (Map/Unmap pairs happen on the same thread)
    struct PendingMap { uint64_t resource; void* data; uint32_t size; };
    thread_local std::vector<PendingMap> t_pending_maps;
    constexpr uint32_t k_max_patched_cbuffer_size = 1024;
    // Textures are resized down to 1/128 of the screen, constants are only patched down to 1/32 (smaller pairs like (20, 11) are too likely to be unrelated values)
    constexpr uint32_t k_max_scaled_fraction_shift = 7;
    constexpr uint32_t k_max_patched_fraction_shift = 5;

    // Diagnostics capture (records every draw/dispatch of one frame, so we can design render scaling)
    enum class CaptureState : int { Idle, Requested, Capturing };
    std::atomic<CaptureState> g_capture_state = CaptureState::Idle;
    std::mutex g_capture_mutex;
    std::filesystem::path g_addon_directory;
    std::string g_capture_text;
    std::string g_capture_status;

    struct PassRecord
    {
        bool immediate = true;
        bool is_dispatch = false;
        bool is_taa = false;
        uint32_t vs = 0, ps = 0, cs = 0;
        uint32_t rt_count = 0;
        uint32_t rt0_w = 0, rt0_h = 0, rt0_fmt = 0, rt0_array = 0;
        uint32_t dsv_w = 0, dsv_h = 0, dsv_fmt = 0;
        float vp_x = 0, vp_y = 0, vp_w = 0, vp_h = 0;
        uint32_t uav0_w = 0, uav0_h = 0, uav0_fmt = 0;
        uint32_t srv0_w = 0, srv0_h = 0, srv0_fmt = 0;
    };
    std::vector<PassRecord> g_capture_records;
    constexpr size_t k_max_capture_records = 8000;

    // Bytecode of every shader the game creates (by Luma hash), so the diagnostics can include the disassembly of the shaders used in the captured frame.
    std::mutex g_shader_bytecode_mutex;
    std::unordered_map<uint32_t, std::vector<uint8_t>> g_shader_bytecode;

    struct TexInfo
    {
        uint32_t w = 0, h = 0, fmt = 0, array = 0, mips = 0, bind = 0;
        bool valid = false;
    };

    TexInfo GetTexInfo(ID3D11Resource* resource)
    {
        TexInfo info;
        if (!resource)
            return info;
        ComPtr<ID3D11Texture2D> tex;
        if (FAILED(resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(tex.put()))) || !tex)
            return info;
        D3D11_TEXTURE2D_DESC desc;
        tex->GetDesc(&desc);
        info.w = desc.Width;
        info.h = desc.Height;
        info.fmt = (uint32_t)desc.Format;
        info.array = desc.ArraySize;
        info.mips = desc.MipLevels;
        info.bind = desc.BindFlags;
        info.valid = true;
        return info;
    }

    template <typename ViewT>
    TexInfo GetViewTexInfo(ViewT* view)
    {
        if (!view)
            return {};
        ComPtr<ID3D11Resource> resource;
        view->GetResource(resource.put());
        return GetTexInfo(resource.get());
    }

    std::string FormatFloat4(const float4& v)
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "(%.6g, %.6g, %.6g, %.6g)", v.x, v.y, v.z, v.w);
        return buf;
    }
}

struct GameDeviceDataDeusExMankindDivided final : GameDeviceData
{
};

class GameDeusExMankindDivided final : public Game
{
public:

    static GameDeviceDataDeusExMankindDivided& GetGameDeviceData(DeviceData& device_data)
    {
        return *(GameDeviceDataDeusExMankindDivided*)device_data.game;
    }

    void OnLoad(std::filesystem::path& file_path, bool failed) override
    {
        if (!failed)
        {
            g_addon_directory = file_path.parent_path();
            reshade::register_event<reshade::addon_event::map_buffer_region>(GameDeusExMankindDivided::OnMapBufferRegion);
            reshade::register_event<reshade::addon_event::unmap_buffer_region>(GameDeusExMankindDivided::OnUnmapBufferRegion);
            reshade::register_event<reshade::addon_event::init_pipeline>(GameDeusExMankindDivided::OnInitPipelineStoreBytecode);
            reshade::register_event<reshade::addon_event::create_resource>(GameDeusExMankindDivided::OnCreateResourceScale);
            reshade::register_event<reshade::addon_event::init_resource>(GameDeusExMankindDivided::OnInitResourceScale);
            reshade::register_event<reshade::addon_event::destroy_resource>(GameDeusExMankindDivided::OnDestroyResourceScale);
            reshade::register_event<reshade::addon_event::clear_render_target_view>(GameDeusExMankindDivided::OnClearRenderTargetView);
            reshade::register_event<reshade::addon_event::clear_depth_stencil_view>(GameDeusExMankindDivided::OnClearDepthStencilView);
            reshade::register_event<reshade::addon_event::clear_unordered_access_view_float>(GameDeusExMankindDivided::OnClearUnorderedAccessViewFloat);
            reshade::register_event<reshade::addon_event::clear_unordered_access_view_uint>(GameDeusExMankindDivided::OnClearUnorderedAccessViewUint);
        }
    }

    // Shrinks screen-proportional render targets, depth buffers and UAV textures at creation
    static bool OnCreateResourceScale(reshade::api::device* device, reshade::api::resource_desc& desc, reshade::api::subresource_data* initial_data, reshade::api::resource_usage initial_state)
    {
        t_pending_resize = false;
        if (device->get_api() != reshade::api::device_api::d3d11) return false;
        if (!IsScalingActive()) return false;
        if (desc.type != reshade::api::resource_type::texture_2d) return false;
        if (desc.texture.depth_or_layers != 1 || desc.texture.levels != 1 || desc.texture.samples != 1) return false;
        if (initial_data && initial_data->data) return false;
        if (desc.heap != reshade::api::memory_heap::gpu_only) return false;
        const bool is_target = (desc.usage & reshade::api::resource_usage::render_target) == reshade::api::resource_usage::render_target
            || (desc.usage & reshade::api::resource_usage::depth_stencil) == reshade::api::resource_usage::depth_stencil
            || (desc.usage & reshade::api::resource_usage::unordered_access) == reshade::api::resource_usage::unordered_access;
        if (!is_target) return false;

        DeviceData* device_data = device->get_private_data<DeviceData>();
        if (!device_data) return false;
        const uint32_t base_w = (uint32_t)device_data->output_resolution.x, base_h = (uint32_t)device_data->output_resolution.y;
        if (base_w <= 16 || base_h <= 16) return false; // Swapchain not known yet

        const uint32_t w = desc.texture.width, h = desc.texture.height;
        // Integer formats are the depth of field tile data
        const bool is_integer = desc.texture.format == reshade::api::format::r32_uint || desc.texture.format == reshade::api::format::r32g32_uint || desc.texture.format == reshade::api::format::r16_uint || desc.texture.format == reshade::api::format::r8_uint;
        const uint32_t new_w = MatchScreenDerivedDimension(w, base_w, is_integer), new_h = MatchScreenDerivedDimension(h, base_h, is_integer);
        if (new_w != 0 && new_h != 0)
        {
            g_scale_base_width = base_w;
            g_scale_base_height = base_h;
            t_pending_resize_info = { w, h, new_w, new_h };
            desc.texture.width = new_w;
            desc.texture.height = new_h;
            t_pending_resize = true;
            return true;
        }
        return false;
    }

    static void OnInitResourceScale(reshade::api::device* device, const reshade::api::resource_desc& desc, const reshade::api::subresource_data* initial_data, reshade::api::resource_usage initial_state, reshade::api::resource resource)
    {
        if (!t_pending_resize) return;
        t_pending_resize = false;
        if (desc.type != reshade::api::resource_type::texture_2d || desc.texture.width != t_pending_resize_info.scaled_width || desc.texture.height != t_pending_resize_info.scaled_height) return;
        std::unique_lock lock(g_resized_mutex);
        g_resized_resources[resource.handle] = t_pending_resize_info;
        g_resized_count = (uint32_t)g_resized_resources.size();
    }

    static void OnDestroyResourceScale(reshade::api::device* device, reshade::api::resource resource)
    {
        {
            std::unique_lock lock(g_resized_mutex);
            if (g_resized_resources.erase(resource.handle) == 0) return;
            g_resized_count = (uint32_t)g_resized_resources.size();
        }
        std::scoped_lock lock(g_substitutes_mutex);
        g_substitutes.erase(resource.handle);
        auto EraseViews = [&](auto& views) { std::erase_if(views, [&](const auto& pair) { return pair.second.original_resource == resource.handle; }); };
        EraseViews(g_substitute_rtvs);
        EraseViews(g_substitute_dsvs);
        EraseViews(g_substitute_srvs);
        EraseViews(g_substitute_uavs);
    }

    // Clears of resized textures are mirrored on their full resolution substitutes (immediate context only, that's the only one that uses them).
    // After TAA, a clear also creates the substitute and counts as writing it; before TAA it only clears substitutes that already exist,
    // so post processing doesn't start reading a cleared substitute instead of the real (low resolution) content.
    static ID3D11DeviceContext* GetImmediateNativeContext(reshade::api::command_list* cmd_list)
    {
        if (!IsScalingActive() || cmd_list->get_device()->get_api() != reshade::api::device_api::d3d11) return nullptr;
        auto* ctx = reinterpret_cast<ID3D11DeviceContext*>(cmd_list->get_native());
        if (!ctx || ctx->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) return nullptr;
        return ctx;
    }

    template <typename ViewT, typename DescT, typename CreateFunc>
    static ViewT* GetSubstituteViewForClear(ID3D11Device* dev, ViewT* view, std::unordered_map<uint64_t, SubstituteView<ViewT>>& cache, CreateFunc create)
    {
        if (g_post_taa_phase)
            return GetSubstituteView<ViewT, DescT>(dev, view, cache, false, true, create);
        // Before TAA: only existing substitutes
        ComPtr<ID3D11Resource> resource;
        view->GetResource(resource.put());
        if (!g_substitutes.contains((uint64_t)resource.get())) return nullptr;
        return GetSubstituteView<ViewT, DescT>(dev, view, cache, false, false, create);
    }

    static bool OnClearRenderTargetView(reshade::api::command_list* cmd_list, reshade::api::resource_view rtv, const float color[4], uint32_t rect_count, const reshade::api::rect* rects)
    {
        ID3D11DeviceContext* ctx = GetImmediateNativeContext(cmd_list);
        if (!ctx || rtv.handle == 0) return false;
        ComPtr<ID3D11Device> dev;
        ctx->GetDevice(dev.put());
        std::scoped_lock lock(g_substitutes_mutex);
        auto Create = [&](ID3D11Resource* r, const D3D11_RENDER_TARGET_VIEW_DESC* d, ID3D11RenderTargetView** o) { return dev->CreateRenderTargetView(r, d, o); };
        if (auto* substitute = GetSubstituteViewForClear<ID3D11RenderTargetView, D3D11_RENDER_TARGET_VIEW_DESC>(dev.get(), reinterpret_cast<ID3D11RenderTargetView*>(rtv.handle), g_substitute_rtvs, Create))
            ctx->ClearRenderTargetView(substitute, color);
        return false; // The original still gets cleared too
    }

    static bool OnClearDepthStencilView(reshade::api::command_list* cmd_list, reshade::api::resource_view dsv, const float* depth, const uint8_t* stencil, uint32_t rect_count, const reshade::api::rect* rects)
    {
        ID3D11DeviceContext* ctx = GetImmediateNativeContext(cmd_list);
        if (!ctx || dsv.handle == 0 || (!depth && !stencil)) return false;
        ComPtr<ID3D11Device> dev;
        ctx->GetDevice(dev.put());
        std::scoped_lock lock(g_substitutes_mutex);
        auto Create = [&](ID3D11Resource* r, const D3D11_DEPTH_STENCIL_VIEW_DESC* d, ID3D11DepthStencilView** o) { return dev->CreateDepthStencilView(r, d, o); };
        // Depth substitutes are never "written" (the scene depth isn't upscaled), so this doesn't use the post TAA creation path
        ComPtr<ID3D11Resource> resource;
        reinterpret_cast<ID3D11DepthStencilView*>(dsv.handle)->GetResource(resource.put());
        if (!g_substitutes.contains((uint64_t)resource.get())) return false;
        if (auto* substitute = GetSubstituteView<ID3D11DepthStencilView, D3D11_DEPTH_STENCIL_VIEW_DESC>(dev.get(), reinterpret_cast<ID3D11DepthStencilView*>(dsv.handle), g_substitute_dsvs, false, false, Create))
        {
            UINT flags = (depth ? D3D11_CLEAR_DEPTH : 0) | (stencil ? D3D11_CLEAR_STENCIL : 0);
            ctx->ClearDepthStencilView(substitute, flags, depth ? *depth : 0.f, stencil ? *stencil : 0);
        }
        return false;
    }

    static bool OnClearUnorderedAccessViewFloat(reshade::api::command_list* cmd_list, reshade::api::resource_view uav, const float values[4], uint32_t rect_count, const reshade::api::rect* rects)
    {
        ID3D11DeviceContext* ctx = GetImmediateNativeContext(cmd_list);
        if (!ctx || uav.handle == 0) return false;
        ComPtr<ID3D11Device> dev;
        ctx->GetDevice(dev.put());
        std::scoped_lock lock(g_substitutes_mutex);
        auto Create = [&](ID3D11Resource* r, const D3D11_UNORDERED_ACCESS_VIEW_DESC* d, ID3D11UnorderedAccessView** o) { return dev->CreateUnorderedAccessView(r, d, o); };
        if (auto* substitute = GetSubstituteViewForClear<ID3D11UnorderedAccessView, D3D11_UNORDERED_ACCESS_VIEW_DESC>(dev.get(), reinterpret_cast<ID3D11UnorderedAccessView*>(uav.handle), g_substitute_uavs, Create))
            ctx->ClearUnorderedAccessViewFloat(substitute, values);
        return false;
    }

    static bool OnClearUnorderedAccessViewUint(reshade::api::command_list* cmd_list, reshade::api::resource_view uav, const uint32_t values[4], uint32_t rect_count, const reshade::api::rect* rects)
    {
        ID3D11DeviceContext* ctx = GetImmediateNativeContext(cmd_list);
        if (!ctx || uav.handle == 0) return false;
        ComPtr<ID3D11Device> dev;
        ctx->GetDevice(dev.put());
        std::scoped_lock lock(g_substitutes_mutex);
        auto Create = [&](ID3D11Resource* r, const D3D11_UNORDERED_ACCESS_VIEW_DESC* d, ID3D11UnorderedAccessView** o) { return dev->CreateUnorderedAccessView(r, d, o); };
        if (auto* substitute = GetSubstituteViewForClear<ID3D11UnorderedAccessView, D3D11_UNORDERED_ACCESS_VIEW_DESC>(dev.get(), reinterpret_cast<ID3D11UnorderedAccessView*>(uav.handle), g_substitute_uavs, Create))
            ctx->ClearUnorderedAccessViewUint(substitute, values);
        return false;
    }

    // Full resolution substitute texture for a resized resource (created on demand, immediate context thread only)
    static Substitute* GetSubstitute(ID3D11Device* native_device, ID3D11Resource* original, const ResizedResourceInfo& info)
    {
        auto it = g_substitutes.find((uint64_t)original);
        if (it != g_substitutes.end())
            return &it->second;
        ComPtr<ID3D11Texture2D> original_texture;
        if (FAILED(original->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(original_texture.put()))) || !original_texture)
            return nullptr;
        D3D11_TEXTURE2D_DESC desc;
        original_texture->GetDesc(&desc);
        desc.Width = info.original_width;
        desc.Height = info.original_height;
        Substitute substitute;
        if (FAILED(native_device->CreateTexture2D(&desc, nullptr, substitute.texture.put())))
            return nullptr;
        return &g_substitutes.emplace((uint64_t)original, std::move(substitute)).first->second;
    }

    // Returns the substitute view for "view" if its resource was resized (nullptr otherwise).
    // "require_written" is for reads: only substitute if the full resolution copy has content this frame.
    template <typename ViewT, typename DescT, typename CreateFunc>
    static ViewT* GetSubstituteView(ID3D11Device* native_device, ViewT* view, std::unordered_map<uint64_t, SubstituteView<ViewT>>& cache, bool require_written, bool mark_written, CreateFunc create)
    {
        if (!view) return nullptr;
        ComPtr<ID3D11Resource> resource;
        view->GetResource(resource.put());
        ResizedResourceInfo info;
        if (!GetResizedInfo(resource.get(), info)) return nullptr;

        const uint64_t frame = g_frame_index;
        Substitute* substitute = nullptr;
        if (require_written)
        {
            // Reads never create a substitute, the small original is still sampled correctly (by UV)
            auto existing = g_substitutes.find((uint64_t)resource.get());
            if (existing == g_substitutes.end() || existing->second.written_frame != frame) return nullptr;
            substitute = &existing->second;
        }
        else
        {
            substitute = GetSubstitute(native_device, resource.get(), info);
            if (!substitute) return nullptr;
        }
        if (mark_written) substitute->written_frame = frame;

        auto it = cache.find((uint64_t)view);
        if (it != cache.end() && it->second.original_resource == (uint64_t)resource.get())
            return it->second.view.get();

        DescT view_desc;
        view->GetDesc(&view_desc);
        SubstituteView<ViewT> entry;
        entry.original_resource = (uint64_t)resource.get();
        if (FAILED(create(substitute->texture.get(), &view_desc, entry.view.put())))
            return nullptr;
        ViewT* result = entry.view.get();
        cache[(uint64_t)view] = std::move(entry);
        return result;
    }

    static void OnInitPipelineStoreBytecode(reshade::api::device* device, reshade::api::pipeline_layout layout, uint32_t subobject_count, const reshade::api::pipeline_subobject* subobjects, reshade::api::pipeline pipeline)
    {
        SKIP_UNSUPPORTED_DEVICE_API(device->get_api());

        for (uint32_t i = 0; i < subobject_count; ++i)
        {
            const auto& subobject = subobjects[i];
            if (subobject.type != reshade::api::pipeline_subobject_type::vertex_shader
                && subobject.type != reshade::api::pipeline_subobject_type::pixel_shader
                && subobject.type != reshade::api::pipeline_subobject_type::compute_shader)
                continue;
            const auto* shader_desc = static_cast<const reshade::api::shader_desc*>(subobject.data);
            if (!shader_desc || !shader_desc->code || shader_desc->code_size == 0)
                continue;
            const uint8_t* code = static_cast<const uint8_t*>(shader_desc->code);
            const uint32_t hash = Shader::BinToHash(code, shader_desc->code_size);
            std::scoped_lock lock(g_shader_bytecode_mutex);
            if (!g_shader_bytecode.contains(hash))
                g_shader_bytecode.emplace(hash, std::vector<uint8_t>(code, code + shader_desc->code_size));
        }
    }

    void OnInit(bool async) override
    {
        // The game uses all CB slots and none of our shaders need the Luma cbuffers (there's no HDR/display composition anymore), so don't bind them.
        luma_settings_cbuffer_index = -1;
        luma_data_cbuffer_index = -1;

        native_shaders_definitions.emplace(CompileTimeStringHash("DeusExMD Post DLSS CS"), ShaderDefinition("Luma_DeusExMD_Post_DLSS_CS", reshade::api::pipeline_subobject_type::compute_shader));
    }

    void LoadConfigs() override
    {
        reshade::get_config_value(nullptr, NAME, "DisableLastKnownLocation", g_disable_last_known_location);

        int preset = 0;
        reshade::get_config_value(nullptr, NAME, "RenderScalePreset", preset);
        g_selected_scale_preset = std::clamp(preset, 0, k_scale_presets_count - 1);
        // The textures are sized when the game creates them, so the active preset is only taken from the config once, at boot
        static bool s_active_preset_loaded = false;
        if (!s_active_preset_loaded)
        {
            g_active_scale_preset = g_selected_scale_preset;
            s_active_preset_loaded = true;
        }
    }

    void DrawImGuiSettings(DeviceData& device_data) override
    {
        ImGui::NewLine();

        // Status
        if (device_data.sr_type == SR::Type::None)
        {
            ImGui::TextUnformatted("DLSS: off (select DLSS under \"Super Resolution\" above).");
        }
        else if (!g_taa_seen_last_frame)
        {
            ImGui::TextColored(ImVec4(1.f, 0.6f, 0.2f, 1.f), "DLSS: waiting - the game's TAA pass isn't running.");
            ImGui::TextUnformatted("Make sure the game runs in DirectX 11, with MSAA off and Temporal Anti-Aliasing on.");
        }
        else if (g_dlss_ran_last_frame)
        {
            ImGui::Text("DLSS: active  (render %ux%u -> output %ux%u)", g_last_render_size.x, g_last_render_size.y, g_last_output_size.x, g_last_output_size.y);
        }
        else
        {
            ImGui::TextColored(ImVec4(1.f, 0.6f, 0.2f, 1.f), "DLSS: TAA found but DLSS couldn't run this frame (camera data missing).");
        }
        ImGui::NewLine();
        ImGui::SetNextItemWidth(260.f);
        if (ImGui::BeginCombo("Upscaling (experimental)", k_scale_presets[g_selected_scale_preset].name))
        {
            for (int i = 0; i < k_scale_presets_count; ++i)
            {
                const bool selected = i == g_selected_scale_preset;
                if (ImGui::Selectable(k_scale_presets[i].name, selected))
                {
                    g_selected_scale_preset = i;
                    reshade::set_config_value(nullptr, NAME, "RenderScalePreset", g_selected_scale_preset);
                }
                if (selected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Renders the 3D scene at a lower resolution and lets DLSS upscale it. The UI and final image stay at full resolution.");
        if (g_selected_scale_preset != g_active_scale_preset)
            ImGui::TextColored(ImVec4(1.f, 0.8f, 0.2f, 1.f), "Restart the game to apply (currently running: %s).", k_scale_presets[g_active_scale_preset].name);
        if (IsScalingActive())
        {
            ImGui::Text("Resized textures: %u", g_resized_count.load());
            if (device_data.sr_type == SR::Type::None)
                ImGui::TextColored(ImVec4(1.f, 0.6f, 0.2f, 1.f), "DLSS is off, so the scene is just stretched from the lower resolution.");
        }

        ImGui::NewLine();

        if (ImGui::Checkbox("Disable last known location", &g_disable_last_known_location))
        {
            reshade::set_config_value(nullptr, NAME, "DisableLastKnownLocation", g_disable_last_known_location);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Hides the \"last known location\" ghost marker shown when enemies lose sight of you.");

        ImGui::NewLine();

        // Diagnostics
        if (ImGui::CollapsingHeader("Diagnostics (for developing upscaling)"))
        {
            ImGui::TextWrapped("Load a save, stand somewhere in normal gameplay (not a menu), then press the button. It records one frame and saves Luma_DXMD_Diagnostics.txt and Luma_DXMD_Shaders.txt next to the game's exe.");
            const bool busy = g_capture_state != CaptureState::Idle;
            ImGui::BeginDisabled(busy);
            if (ImGui::Button("Capture diagnostics"))
            {
                g_capture_state = CaptureState::Requested;
                std::scoped_lock lock(g_capture_mutex);
                g_capture_status = "Capturing...";
            }
            ImGui::EndDisabled();

            std::scoped_lock lock(g_capture_mutex);
            if (!g_capture_text.empty())
            {
                ImGui::SameLine();
                if (ImGui::Button("Copy to clipboard"))
                    ImGui::SetClipboardText(g_capture_text.c_str());
            }
            if (!g_capture_status.empty())
                ImGui::TextWrapped("%s", g_capture_status.c_str());
        }
    }

    void OnCreateDevice(ID3D11Device* native_device, DeviceData& device_data) override
    {
        device_data.game = new GameDeviceDataDeusExMankindDivided;
    }

    static void OnMapBufferRegion(reshade::api::device* device, reshade::api::resource resource, uint64_t offset, uint64_t size, reshade::api::map_access access, void** data)
    {
        SKIP_UNSUPPORTED_DEVICE_API(device->get_api());

        auto buffer = (ID3D11Buffer*)resource.handle;
        D3D11_BUFFER_DESC desc;
        buffer->GetDesc(&desc);
        if (desc.BindFlags != D3D11_BIND_CONSTANT_BUFFER || !data || !*data)
            return;

        if (desc.ByteWidth == 544)
        {
            g_cbSharedPerViewData_handle = resource.handle;
            g_cbSharedPerViewData_mapped_data = *data;
        }

        // Remember writes to small constant buffers, to patch screen sizes in them on unmap
        if (IsScalingActive() && access != reshade::api::map_access::read_only && desc.ByteWidth <= k_max_patched_cbuffer_size)
        {
            t_pending_maps.push_back({ resource.handle, *data, desc.ByteWidth });
        }
    }

    static void OnUnmapBufferRegion(reshade::api::device* device, reshade::api::resource resource)
    {
        SKIP_UNSUPPORTED_DEVICE_API(device->get_api());

        for (size_t i = 0; i < t_pending_maps.size(); ++i)
        {
            if (t_pending_maps[i].resource != resource.handle) continue;
            const PendingMap pending = t_pending_maps[i];
            t_pending_maps.erase(t_pending_maps.begin() + i);
            if (ShouldPatchConstants())
                PatchScreenSizeConstants(static_cast<float*>(pending.data), pending.size / sizeof(float));
            break;
        }

        // Not thread safe, but it looks like it's fine.
        if (g_cbSharedPerViewData_handle == resource.handle)
        {
            auto data = (float*)g_cbSharedPerViewData_mapped_data;

            // This should be reliable.
            if (data && data[8] && data[9] && data[8] == data[129])
            {
                std::memcpy(&g_cbSharedPerViewData_data, data, sizeof(g_cbSharedPerViewData_data));
                g_cbSharedPerViewData_handle = 0;
            }
        }
    }

    // Before TAA: draws into resized targets need their (full resolution) viewports and scissors scaled down
    static void ScaleViewportsForResizedTargets(ID3D11DeviceContext* ctx, CommandListData& cmd_list_data)
    {
        if (cmd_list_data.viewports_scaled && cmd_list_data.scissors_scaled)
            return;

        ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
        ID3D11DepthStencilView* dsv = nullptr;
        ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);
        ResizedResourceInfo info;
        bool found = false;
        auto Check = [&](ID3D11View* view)
        {
            if (!view || found) return;
            ComPtr<ID3D11Resource> resource;
            view->GetResource(resource.put());
            found = GetResizedInfo(resource.get(), info);
        };
        for (auto* rtv : rtvs) Check(rtv);
        Check(dsv);
        for (auto* rtv : rtvs) if (rtv) rtv->Release();
        if (dsv) dsv->Release();
        if (!found || info.original_width == 0 || info.original_height == 0)
            return;

        const float scale_x = (float)info.scaled_width / (float)info.original_width;
        const float scale_y = (float)info.scaled_height / (float)info.original_height;

        if (!cmd_list_data.viewports_scaled)
        {
            D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
            UINT num_viewports = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
            ctx->RSGetViewports(&num_viewports, viewports);
            bool changed = false;
            for (UINT i = 0; i < num_viewports; i++)
            {
                if (viewports[i].Width == 0.f || viewports[i].Height == 0.f) continue;
                // Scale the edges and round them, so tiled viewports stay tiled
                const float left = std::round(viewports[i].TopLeftX * scale_x);
                const float top = std::round(viewports[i].TopLeftY * scale_y);
                const float right = std::round((viewports[i].TopLeftX + viewports[i].Width) * scale_x);
                const float bottom = std::round((viewports[i].TopLeftY + viewports[i].Height) * scale_y);
                viewports[i].TopLeftX = left;
                viewports[i].TopLeftY = top;
                viewports[i].Width = (std::max)(1.f, right - left);
                viewports[i].Height = (std::max)(1.f, bottom - top);
                changed = true;
            }
            if (changed)
                ctx->RSSetViewports(num_viewports, viewports);
            cmd_list_data.viewports_scaled = true;
        }
        if (!cmd_list_data.scissors_scaled)
        {
            D3D11_RECT scissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
            UINT num_scissors = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
            ctx->RSGetScissorRects(&num_scissors, scissors);
            bool changed = false;
            for (UINT i = 0; i < num_scissors; i++)
            {
                if (scissors[i].right == scissors[i].left || scissors[i].bottom == scissors[i].top) continue;
                scissors[i].left = std::lround(scissors[i].left * scale_x);
                scissors[i].top = std::lround(scissors[i].top * scale_y);
                scissors[i].right = std::lround(scissors[i].right * scale_x);
                scissors[i].bottom = std::lround(scissors[i].bottom * scale_y);
                changed = true;
            }
            if (changed)
                ctx->RSSetScissorRects(num_scissors, scissors);
            cmd_list_data.scissors_scaled = true;
        }
    }

    // After TAA: swap every resized texture the game binds for its full resolution substitute
    static void ApplyPostTaaSubstitutes(ID3D11Device* dev, ID3D11DeviceContext* ctx, bool is_dispatch)
    {
        std::scoped_lock lock(g_substitutes_mutex);
        const uint64_t frame = g_frame_index;

        auto CreateRTV = [dev](ID3D11Resource* r, const D3D11_RENDER_TARGET_VIEW_DESC* d, ID3D11RenderTargetView** o) { return dev->CreateRenderTargetView(r, d, o); };
        auto CreateDSV = [dev](ID3D11Resource* r, const D3D11_DEPTH_STENCIL_VIEW_DESC* d, ID3D11DepthStencilView** o) { return dev->CreateDepthStencilView(r, d, o); };
        auto CreateSRV = [dev](ID3D11Resource* r, const D3D11_SHADER_RESOURCE_VIEW_DESC* d, ID3D11ShaderResourceView** o) { return dev->CreateShaderResourceView(r, d, o); };
        auto CreateUAV = [dev](ID3D11Resource* r, const D3D11_UNORDERED_ACCESS_VIEW_DESC* d, ID3D11UnorderedAccessView** o) { return dev->CreateUnorderedAccessView(r, d, o); };

        // Shader resources (only substituted if the full resolution copy was written this frame, otherwise the small one is still sampled correctly by UV)
        auto SubstituteSRVs = [&](auto get, auto set)
        {
            constexpr UINT count = D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT;
            ID3D11ShaderResourceView* srvs[count] = {};
            get(0, count, srvs);
            bool changed = false;
            for (UINT i = 0; i < count; ++i)
            {
                if (!srvs[i]) continue;
                if (auto* substitute = GetSubstituteView<ID3D11ShaderResourceView, D3D11_SHADER_RESOURCE_VIEW_DESC>(dev, srvs[i], g_substitute_srvs, true, false, CreateSRV))
                {
                    srvs[i]->Release();
                    srvs[i] = substitute;
                    substitute->AddRef();
                    changed = true;
                }
            }
            if (changed)
                set(0, count, srvs);
            for (auto* srv : srvs) if (srv) srv->Release();
        };

        if (!is_dispatch)
        {
            ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
            ID3D11DepthStencilView* dsv = nullptr;
            ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);
            ID3D11RenderTargetView* new_rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
            ID3D11DepthStencilView* new_dsv = dsv;
            UINT rtv_count = 0;
            bool changed = false;
            ResizedResourceInfo substituted_info;
            bool has_substituted_info = false;
            auto RememberInfo = [&](ID3D11View* view)
            {
                if (has_substituted_info) return;
                ComPtr<ID3D11Resource> resource;
                view->GetResource(resource.put());
                has_substituted_info = GetResizedInfo(resource.get(), substituted_info);
            };
            for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
            {
                new_rtvs[i] = rtvs[i];
                if (!rtvs[i]) continue;
                rtv_count = i + 1;
                if (auto* substitute = GetSubstituteView<ID3D11RenderTargetView, D3D11_RENDER_TARGET_VIEW_DESC>(dev, rtvs[i], g_substitute_rtvs, false, true, CreateRTV))
                {
                    RememberInfo(rtvs[i]);
                    new_rtvs[i] = substitute;
                    changed = true;
                }
            }
            if (dsv)
            {
                if (auto* substitute = GetSubstituteView<ID3D11DepthStencilView, D3D11_DEPTH_STENCIL_VIEW_DESC>(dev, dsv, g_substitute_dsvs, false, false, CreateDSV))
                {
                    RememberInfo(dsv);
                    new_dsv = substitute;
                    changed = true;
                    // The scene depth isn't upscaled (yet), so start from a cleared one (inverted depth: 0 is far) once per frame
                    ComPtr<ID3D11Resource> resource;
                    dsv->GetResource(resource.put());
                    auto it = g_substitutes.find((uint64_t)resource.get());
                    if (it != g_substitutes.end() && it->second.cleared_frame != frame)
                    {
                        ctx->ClearDepthStencilView(substitute, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 0.f, 0);
                        it->second.cleared_frame = frame;
                    }
                }
            }
            if (changed)
            {
                ctx->OMSetRenderTargetsAndUnorderedAccessViews(rtv_count, new_rtvs, new_dsv, D3D11_KEEP_UNORDERED_ACCESS_VIEWS, 0, nullptr, nullptr);

                // The game sizes some post processing viewports from its (now small) textures: expand those to the full resolution substitute,
                // otherwise they'd only fill its top left corner.
                if (has_substituted_info)
                {
                    const float sw = (float)substituted_info.scaled_width, sh = (float)substituted_info.scaled_height;
                    const float ow = (float)substituted_info.original_width, oh = (float)substituted_info.original_height;
                    D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
                    UINT num_viewports = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
                    ctx->RSGetViewports(&num_viewports, viewports);
                    bool viewports_changed = false;
                    for (UINT i = 0; i < num_viewports; ++i)
                    {
                        if (viewports[i].TopLeftX == 0.f && viewports[i].TopLeftY == 0.f && viewports[i].Width == sw && viewports[i].Height == sh)
                        {
                            viewports[i].Width = ow;
                            viewports[i].Height = oh;
                            viewports_changed = true;
                        }
                    }
                    if (viewports_changed)
                        ctx->RSSetViewports(num_viewports, viewports);

                    D3D11_RECT scissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
                    UINT num_scissors = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
                    ctx->RSGetScissorRects(&num_scissors, scissors);
                    bool scissors_changed = false;
                    for (UINT i = 0; i < num_scissors; ++i)
                    {
                        if (scissors[i].left == 0 && scissors[i].top == 0 && scissors[i].right == (LONG)substituted_info.scaled_width && scissors[i].bottom == (LONG)substituted_info.scaled_height)
                        {
                            scissors[i].right = (LONG)substituted_info.original_width;
                            scissors[i].bottom = (LONG)substituted_info.original_height;
                            scissors_changed = true;
                        }
                    }
                    if (scissors_changed)
                        ctx->RSSetScissorRects(num_scissors, scissors);
                }
            }
            for (auto* rtv : rtvs) if (rtv) rtv->Release();
            if (dsv) dsv->Release();

            SubstituteSRVs([ctx](UINT s, UINT n, ID3D11ShaderResourceView** v) { ctx->PSGetShaderResources(s, n, v); }, [ctx](UINT s, UINT n, ID3D11ShaderResourceView** v) { ctx->PSSetShaderResources(s, n, v); });
            SubstituteSRVs([ctx](UINT s, UINT n, ID3D11ShaderResourceView** v) { ctx->VSGetShaderResources(s, n, v); }, [ctx](UINT s, UINT n, ID3D11ShaderResourceView** v) { ctx->VSSetShaderResources(s, n, v); });
        }
        else
        {
            constexpr UINT uav_count = D3D11_PS_CS_UAV_REGISTER_COUNT;
            ID3D11UnorderedAccessView* uavs[uav_count] = {};
            ctx->CSGetUnorderedAccessViews(0, uav_count, uavs);
            bool changed = false;
            for (UINT i = 0; i < uav_count; ++i)
            {
                if (!uavs[i]) continue;
                if (auto* substitute = GetSubstituteView<ID3D11UnorderedAccessView, D3D11_UNORDERED_ACCESS_VIEW_DESC>(dev, uavs[i], g_substitute_uavs, false, true, CreateUAV))
                {
                    uavs[i]->Release();
                    uavs[i] = substitute;
                    substitute->AddRef();
                    changed = true;
                }
            }
            if (changed)
                ctx->CSSetUnorderedAccessViews(0, uav_count, uavs, nullptr);
            for (auto* uav : uavs) if (uav) uav->Release();

            SubstituteSRVs([ctx](UINT s, UINT n, ID3D11ShaderResourceView** v) { ctx->CSGetShaderResources(s, n, v); }, [ctx](UINT s, UINT n, ID3D11ShaderResourceView** v) { ctx->CSSetShaderResources(s, n, v); });
        }
    }

    static void RecordPass(ID3D11DeviceContext* ctx, const ShaderHashesList<OneShaderPerPipeline>& hashes, bool is_dispatch, bool is_taa)
    {
        PassRecord r;
        r.immediate = ctx->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE;
        r.is_dispatch = is_dispatch;
        r.is_taa = is_taa;
        auto ToHash32 = [](uint64_t hash) { return hash == Shader::SHADER_HASH_NONE ? 0u : (uint32_t)hash; };
        r.vs = ToHash32(hashes.vertex_shaders[0]);
        r.ps = ToHash32(hashes.pixel_shaders[0]);
        r.cs = ToHash32(hashes.compute_shaders[0]);

        if (is_dispatch)
        {
            ComPtr<ID3D11UnorderedAccessView> uav;
            ctx->CSGetUnorderedAccessViews(0, 1, uav.put());
            const TexInfo uav_info = GetViewTexInfo(uav.get());
            r.uav0_w = uav_info.w; r.uav0_h = uav_info.h; r.uav0_fmt = uav_info.fmt;

            ComPtr<ID3D11ShaderResourceView> srv;
            ctx->CSGetShaderResources(0, 1, srv.put());
            const TexInfo srv_info = GetViewTexInfo(srv.get());
            r.srv0_w = srv_info.w; r.srv0_h = srv_info.h; r.srv0_fmt = srv_info.fmt;
        }
        else
        {
            ComPtr<ID3D11RenderTargetView> rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
            ComPtr<ID3D11DepthStencilView> dsv;
            ID3D11RenderTargetView* raw_rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
            ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, raw_rtvs, dsv.put());
            for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
            {
                *rtvs[i].put() = raw_rtvs[i]; // Takes ownership of the reference
                if (raw_rtvs[i])
                    r.rt_count = i + 1;
            }
            const TexInfo rt_info = GetViewTexInfo(rtvs[0].get());
            r.rt0_w = rt_info.w; r.rt0_h = rt_info.h; r.rt0_fmt = rt_info.fmt; r.rt0_array = rt_info.array;
            const TexInfo dsv_info = GetViewTexInfo(dsv.get());
            r.dsv_w = dsv_info.w; r.dsv_h = dsv_info.h; r.dsv_fmt = dsv_info.fmt;

            UINT num_viewports = 1;
            D3D11_VIEWPORT vp = {};
            ctx->RSGetViewports(&num_viewports, &vp);
            if (num_viewports > 0)
            {
                r.vp_x = vp.TopLeftX; r.vp_y = vp.TopLeftY; r.vp_w = vp.Width; r.vp_h = vp.Height;
            }

            ComPtr<ID3D11ShaderResourceView> srv;
            ctx->PSGetShaderResources(0, 1, srv.put());
            const TexInfo srv_info = GetViewTexInfo(srv.get());
            r.srv0_w = srv_info.w; r.srv0_h = srv_info.h; r.srv0_fmt = srv_info.fmt;
        }

        std::scoped_lock lock(g_capture_mutex);
        if (g_capture_records.size() < k_max_capture_records)
            g_capture_records.push_back(r);
    }

    static void FinishCapture(DeviceData& device_data)
    {
        std::ostringstream ss;
        ss << "Luma DXMD diagnostics\n";
        ss << "Output resolution: " << device_data.output_resolution.x << "x" << device_data.output_resolution.y << "\n";
        ss << "Render resolution (core): " << device_data.render_resolution.x << "x" << device_data.render_resolution.y << "\n";
        ss << "Scale preset (active): " << g_active_scale_preset << "  resized textures: " << g_resized_count.load() << "  scale base: " << g_scale_base_width.load() << "x" << g_scale_base_height.load() << "\n";
        ss << "SR type: " << (int)device_data.sr_type << "  TAA seen: " << (g_taa_seen_this_frame ? 1 : 0) << "\n";
        ss << "Last DLSS sizes: render " << g_last_render_size.x << "x" << g_last_render_size.y << " output " << g_last_output_size.x << "x" << g_last_output_size.y << "\n";
        const auto& cb = g_cbSharedPerViewData_data;
        ss << "vViewRemap: " << FormatFloat4(cb.vViewRemap) << "\n";
        ss << "vViewDepthRemap: " << FormatFloat4(cb.vViewDepthRemap) << "\n";
        ss << "vViewportSize: " << FormatFloat4(cb.vViewportSize) << "\n";
        ss << "vCheckerModeParams: " << FormatFloat4(cb.vCheckerModeParams) << "\n";
        ss << "vCHSParams: " << FormatFloat4(cb.vCHSParams) << "\n";
        ss << "vExtraParams: " << FormatFloat4(cb.vExtraParams) << "\n";
        ss << "mProjection row2: (" << cb.mProjection.m20 << ", " << cb.mProjection.m21 << ", " << cb.mProjection.m22 << ", " << cb.mProjection.m23 << ")\n";
        ss << "mViewToViewport row0: (" << cb.mViewToViewport.m00 << ", " << cb.mViewToViewport.m01 << ", " << cb.mViewToViewport.m02 << ", " << cb.mViewToViewport.m03 << ")\n";
        ss << "mViewToViewport row1: (" << cb.mViewToViewport.m10 << ", " << cb.mViewToViewport.m11 << ", " << cb.mViewToViewport.m12 << ", " << cb.mViewToViewport.m13 << ")\n";
        ss << "mViewToViewport row3: (" << cb.mViewToViewport.m30 << ", " << cb.mViewToViewport.m31 << ", " << cb.mViewToViewport.m32 << ", " << cb.mViewToViewport.m33 << ")\n";
        ss << "\n# idx ctx type VS PS CS | RTs RT0(WxH fmt arr) | DSV(WxH fmt) | VP(x,y WxH) | UAV0(WxH fmt) | SRV0(WxH fmt)\n";

        std::scoped_lock lock(g_capture_mutex);
        char line[512];
        for (size_t i = 0; i < g_capture_records.size(); ++i)
        {
            const auto& r = g_capture_records[i];
            std::snprintf(line, sizeof(line), "%zu %c %s %08X %08X %08X | %u %ux%u f%u a%u | %ux%u f%u | %.0f,%.0f %.0fx%.0f | %ux%u f%u | %ux%u f%u%s\n",
                i, r.immediate ? 'I' : 'D', r.is_dispatch ? "CS" : "DR", r.vs, r.ps, r.cs,
                r.rt_count, r.rt0_w, r.rt0_h, r.rt0_fmt, r.rt0_array,
                r.dsv_w, r.dsv_h, r.dsv_fmt,
                r.vp_x, r.vp_y, r.vp_w, r.vp_h,
                r.uav0_w, r.uav0_h, r.uav0_fmt,
                r.srv0_w, r.srv0_h, r.srv0_fmt,
                r.is_taa ? "  <== TAA" : "");
            ss << line;
        }
        if (g_capture_records.size() >= k_max_capture_records)
            ss << "(truncated at " << k_max_capture_records << " passes)\n";

        // Unique shaders used this frame, in first-use order, tagged with their stage and first pass index
        std::vector<std::tuple<uint32_t, const char*, size_t>> used_shaders;
        {
            std::unordered_set<uint32_t> seen;
            for (size_t i = 0; i < g_capture_records.size(); ++i)
            {
                const auto& r = g_capture_records[i];
                const std::pair<uint32_t, const char*> stages[] = { { r.vs, "vs" }, { r.ps, "ps" }, { r.cs, "cs" } };
                for (const auto& [hash, stage] : stages)
                {
                    if (hash != 0 && seen.insert(hash).second)
                        used_shaders.emplace_back(hash, stage, i);
                }
            }
        }

        g_capture_text = ss.str();
        g_capture_records.clear();

        const std::filesystem::path out_directory = g_addon_directory.empty() ? std::filesystem::path(".") : g_addon_directory;
        const std::filesystem::path out_path = out_directory / "Luma_DXMD_Diagnostics.txt";
        std::ofstream file(out_path, std::ios::out | std::ios::trunc);
        if (file)
        {
            file << g_capture_text;
            g_capture_status = "Saved " + out_path.string();
        }
        else
        {
            g_capture_status = "Couldn't write the diagnostics file, use \"Copy to clipboard\" instead.";
        }

        // Shader disassembly, all in one file
        const std::filesystem::path shaders_path = out_directory / "Luma_DXMD_Shaders.txt";
        std::ofstream shaders_file(shaders_path, std::ios::out | std::ios::trunc);
        if (shaders_file)
        {
            size_t written = 0, missing = 0;
            std::scoped_lock bytecode_lock(g_shader_bytecode_mutex);
            for (const auto& [hash, stage, first_pass] : used_shaders)
            {
                char header[128];
                std::snprintf(header, sizeof(header), "\n//////// %s %08X (first used at pass %zu)\n", stage, hash, first_pass);
                shaders_file << header;
                auto it = g_shader_bytecode.find(hash);
                if (it == g_shader_bytecode.end())
                {
                    shaders_file << "// bytecode not found\n";
                    ++missing;
                    continue;
                }
                auto disasm = Shader::DisassembleShader(it->second.data(), it->second.size());
                if (disasm.has_value())
                {
                    shaders_file << disasm.value();
                    ++written;
                }
                else
                {
                    shaders_file << "// disassembly failed\n";
                    ++missing;
                }
            }
            char summary[160];
            std::snprintf(summary, sizeof(summary), "\nSaved %zu shaders (%zu missing) to ", written, missing);
            g_capture_status += summary + shaders_path.string();
        }
    }

    void OnPresent(ID3D11Device* native_device, DeviceData& device_data) override
    {
        // Diagnostics capture state machine: start on the frame after the request, end at the following present.
        if (g_capture_state == CaptureState::Capturing)
        {
            FinishCapture(device_data);
            g_capture_state = CaptureState::Idle;
        }
        else if (g_capture_state == CaptureState::Requested)
        {
            {
                std::scoped_lock lock(g_capture_mutex);
                g_capture_records.clear();
            }
            g_capture_state = CaptureState::Capturing;
        }

        // Frames without TAA (e.g. menus with no 3D scene) are all UI/2D, so we treat them as "post TAA" (full resolution) from the start.
        // When the scene comes back, at most one frame has its pre TAA immediate passes at full resolution.
        g_post_taa_phase = !g_taa_seen_this_frame.load();
        ++g_frame_index;

        g_taa_seen_last_frame = g_taa_seen_this_frame.load();
        g_taa_seen_this_frame = false;
        g_dlss_ran_last_frame = g_dlss_ran_this_frame.load();
        g_dlss_ran_this_frame = false;

        // If DLSS didn't run this frame (menus, loading screens, TAA off), restart its history next time to avoid ghosting from stale frames.
        if (!device_data.has_drawn_sr)
        {
            device_data.force_reset_sr = true;
        }

        // Negative texture mip bias, as recommended by DLSS (-1 at native, more negative when upscaling).
        if (!custom_texture_mip_lod_bias_offset)
        {
            std::shared_lock shared_lock_samplers(s_mutex_samplers);
            if (device_data.sr_type != SR::Type::None && !device_data.sr_suppressed && g_last_output_size.y > 0)
            {
                device_data.texture_mip_lod_bias_offset = SR::GetMipLODBias(g_last_render_size.y, g_last_output_size.y);
            }
            else
            {
                device_data.texture_mip_lod_bias_offset = 0.f;
            }
        }

        device_data.has_drawn_sr = false;
    }

    DrawOrDispatchOverrideType OnDrawOrDispatch(ID3D11Device* native_device, ID3D11DeviceContext* native_device_context, CommandListData& cmd_list_data, DeviceData& device_data, reshade::api::shader_stage stages, const ShaderHashesList<OneShaderPerPipeline>& original_shader_hashes, bool is_custom_pass, bool& updated_cbuffers, std::function<void()>* original_draw_dispatch_func) override
    {
        const bool is_dispatch = (stages & reshade::api::shader_stage::compute) == reshade::api::shader_stage::compute;
        const bool is_taa = original_shader_hashes.Contains(shader_hashes_TAA);

        if (g_capture_state == CaptureState::Capturing && !is_custom_pass)
        {
            RecordPass(native_device_context, original_shader_hashes, is_dispatch, is_taa);
        }

        const bool is_immediate = native_device_context->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE;
        if (is_immediate)
            g_immediate_thread_id = GetCurrentThreadId();

        if (IsScalingActive() && !is_taa && !is_custom_pass)
        {
            if (is_immediate && g_post_taa_phase)
                ApplyPostTaaSubstitutes(native_device, native_device_context, is_dispatch);
            else if (!is_dispatch)
                ScaleViewportsForResizedTargets(native_device_context, cmd_list_data);
        }

        if (original_shader_hashes.Contains(shader_hashes_LastKnownLocation))
        {
            if (g_disable_last_known_location)
            {
                return DrawOrDispatchOverrideType::Replaced;
            }
            return DrawOrDispatchOverrideType::None;
        }

        if (!is_taa)
        {
            return DrawOrDispatchOverrideType::None;
        }

        g_taa_seen_this_frame = true;
        device_data.taa_detected = true;
        // Everything after this on the immediate context is post processing/UI, at full resolution
        if (is_immediate)
            g_post_taa_phase = true;

        if (device_data.sr_type == SR::Type::None || device_data.sr_suppressed)
        {
            return DrawOrDispatchOverrideType::None;
        }

        // DLSS requires an immediate context for execution!
        if (native_device_context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE)
        {
            return DrawOrDispatchOverrideType::None;
        }

        auto* sr_instance_data = device_data.GetSRInstanceData();
        if (!sr_instance_data)
        {
            return DrawOrDispatchOverrideType::None;
        }

        // No camera data yet (e.g. first frames): let the native TAA run, it doesn't need it.
        const auto& view = g_cbSharedPerViewData_data;
        if (view.mProjection.m11 == 0.f || view.mProjection.m22 == 0.f)
        {
            return DrawOrDispatchOverrideType::None;
        }

        // Get the TAA inputs and output.
        ComPtr<ID3D11ShaderResourceView> srvs[4];
        for (UINT i = 0; i < 4; ++i)
            native_device_context->CSGetShaderResources(i, 1, srvs[i].put());
        ComPtr<ID3D11UnorderedAccessView> uav;
        native_device_context->CSGetUnorderedAccessViews(0, 1, uav.put());
        if (!srvs[0] || !srvs[1] || !srvs[2] || !uav)
        {
            return DrawOrDispatchOverrideType::None;
        }

        ComPtr<ID3D11Resource> resource_depth;
        srvs[0]->GetResource(resource_depth.put());
        ComPtr<ID3D11Resource> resource_scene;
        srvs[1]->GetResource(resource_scene.put());
        ComPtr<ID3D11Resource> resource_mvs;
        srvs[2]->GetResource(resource_mvs.put());
        ComPtr<ID3D11Resource> resource_output;
        uav->GetResource(resource_output.put());

        // When the scene renders at a lower resolution, DLSS writes into the full resolution substitute of the TAA output,
        // which the post processing/UI then reads (see "ApplyPostTaaSubstitutes()").
        ID3D11Resource* dlss_output = resource_output.get();
        ID3D11UnorderedAccessView* dlss_output_uav = uav.get();
        {
            ResizedResourceInfo resized_info;
            if (GetResizedInfo(resource_output.get(), resized_info))
            {
                std::scoped_lock lock(g_substitutes_mutex);
                auto CreateUAV = [native_device](ID3D11Resource* r, const D3D11_UNORDERED_ACCESS_VIEW_DESC* d, ID3D11UnorderedAccessView** o) { return native_device->CreateUnorderedAccessView(r, d, o); };
                ID3D11UnorderedAccessView* substitute_uav = GetSubstituteView<ID3D11UnorderedAccessView, D3D11_UNORDERED_ACCESS_VIEW_DESC>(native_device, uav.get(), g_substitute_uavs, false, true, CreateUAV);
                auto it = g_substitutes.find((uint64_t)resource_output.get());
                if (!substitute_uav || it == g_substitutes.end())
                {
                    return DrawOrDispatchOverrideType::None; // Couldn't create it, let the native TAA run (lower resolution output)
                }
                dlss_output = it->second.texture.get();
                dlss_output_uav = substitute_uav;
            }
        }

        // Use the real texture sizes rather than the swapchain size, as the scene can render at a lower resolution.
        const TexInfo scene_info = GetTexInfo(resource_scene.get());
        const TexInfo output_info = GetTexInfo(dlss_output);
        if (!scene_info.valid || !output_info.valid)
        {
            return DrawOrDispatchOverrideType::None;
        }
        const uint32_t render_width = scene_info.w;
        const uint32_t render_height = scene_info.h;
        const uint32_t output_width = output_info.w;
        const uint32_t output_height = output_info.h;

        // Save the game's compute state so we can restore it after DLSS and our post pass.
        ComPtr<ID3D11ComputeShader> original_cs;
        native_device_context->CSGetShader(original_cs.put(), nullptr, nullptr);

        // DLSS pass
        //
        SR::SettingsData settings_data;
        settings_data.output_width = output_width;
        settings_data.output_height = output_height;
        settings_data.render_width = render_width;
        settings_data.render_height = render_height;
        settings_data.dynamic_resolution = false;
        settings_data.hdr = true;
        settings_data.inverted_depth = true;
        settings_data.mvs_jittered = false;
        // MVs are in UV space so we need to scale them to (render) pixels for DLSS.
        // Also for DLSS we need to flip the sign for both x and y.
        settings_data.mvs_x_scale = -(float)render_width;
        settings_data.mvs_y_scale = -(float)render_height;
        settings_data.render_preset = dlss_render_preset;
        settings_data.auto_exposure = true;

        sr_implementations[device_data.sr_type]->UpdateSettings(sr_instance_data, native_device_context, settings_data);

        SR::SuperResolutionImpl::DrawData draw_data;
        draw_data.source_color = resource_scene.get();
        draw_data.output_color = dlss_output;
        draw_data.motion_vectors = resource_mvs.get();
        draw_data.depth_buffer = resource_depth.get();
        draw_data.reset = device_data.force_reset_sr;

        // Jitters are in clip space offsets (in the projection matrix), so we need to scale them to pixel offsets for DLSS.
        draw_data.jitter_x = view.mProjection.m20 * (float)render_width * -0.5f;
        draw_data.jitter_y = view.mProjection.m21 * (float)render_height * 0.5f;

        draw_data.render_width = render_width;
        draw_data.render_height = render_height;
        draw_data.vert_fov = std::atan(1.0f / view.mProjection.m11) * 2.0f;
        draw_data.near_plane = view.mProjection.m32 / (1.0f - view.mProjection.m22);
        draw_data.far_plane = view.mProjection.m32 / view.mProjection.m22;

        const bool dlss_succeeded = sr_implementations[device_data.sr_type]->Draw(sr_instance_data, native_device_context, draw_data);
        if (!dlss_succeeded)
        {
            // Restore the game's state and let the native TAA run instead.
            native_device_context->CSSetShader(original_cs.get(), nullptr, 0);
            ID3D11ShaderResourceView* raw_srvs[4] = { srvs[0].get(), srvs[1].get(), srvs[2].get(), srvs[3].get() };
            native_device_context->CSSetShaderResources(0, 4, raw_srvs);
            ID3D11UnorderedAccessView* raw_uav = uav.get();
            native_device_context->CSSetUnorderedAccessViews(0, 1, &raw_uav, nullptr);
            device_data.force_reset_sr = true;
            if (dlss_output != resource_output.get())
            {
                // The substitute has no content this frame, so post processing keeps reading the native (low resolution) TAA output
                std::scoped_lock lock(g_substitutes_mutex);
                auto it = g_substitutes.find((uint64_t)resource_output.get());
                if (it != g_substitutes.end())
                    it->second.written_frame = UINT64_MAX;
            }
            return DrawOrDispatchOverrideType::None;
        }

        device_data.has_drawn_sr = true;
        device_data.force_reset_sr = false;
        g_dlss_ran_this_frame = true;
        g_last_render_size = { render_width, render_height };
        g_last_output_size = { output_width, output_height };

        // PostDLSS pass (converts the sqrt encoded output back to linear, as the game's TAA does)
        //
        {
            ID3D11ShaderResourceView* null_srvs[4] = {};
            native_device_context->CSSetShaderResources(0, 4, null_srvs); // Avoid read/write hazards on the output texture
            ID3D11UnorderedAccessView* raw_uav = dlss_output_uav;
            native_device_context->CSSetUnorderedAccessViews(0, 1, &raw_uav, nullptr);
            native_device_context->CSSetShader(device_data.native_compute_shaders[CompileTimeStringHash("DeusExMD Post DLSS CS")].get(), nullptr, 0);
            native_device_context->Dispatch((output_width + 8 - 1) / 8, (output_height + 8 - 1) / 8, 1);
        }

        // Restore the game's compute state
        {
            native_device_context->CSSetShader(original_cs.get(), nullptr, 0);
            ID3D11ShaderResourceView* raw_srvs[4] = { srvs[0].get(), srvs[1].get(), srvs[2].get(), srvs[3].get() };
            native_device_context->CSSetShaderResources(0, 4, raw_srvs);
            ID3D11UnorderedAccessView* raw_uav = uav.get();
            native_device_context->CSSetUnorderedAccessViews(0, 1, &raw_uav, nullptr);
        }

        return DrawOrDispatchOverrideType::Replaced;
    }

    void PrintImGuiAbout() override
    {
        ImGui::Text("Deus Ex: Mankind Divided Luma mod - DLSS. Original DLSS integration by Garamond.", "");
    }
};

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
    if (ul_reason_for_call == DLL_PROCESS_ATTACH)
    {
        Globals::SetGlobals(PROJECT_NAME, "Deus Ex: Mankind Divided Luma mod");
        Globals::VERSION = 7;

        // DLSS only: keep the game's original swapchain, texture formats and colors (no HDR, no display composition).
        swapchain_format_upgrade_type = TextureFormatUpgradesType::None;
        swapchain_upgrade_type        = SwapchainUpgradeType::None;
        texture_format_upgrades_type  = TextureFormatUpgradesType::None;
        force_disable_display_composition = true;

        // TODO: Remove this later!
        Globals::DEVELOPMENT_STATE = Globals::ModDevelopmentState::WorkInProgress;

        #if DEVELOPMENT
        forced_shader_names.emplace(0xDA65F8ED, "Sharpen");
        forced_shader_names.emplace(0x84EF14ED, "TAA");
        forced_shader_names.emplace(0xA6A0E453, "LastKnownLocation");
        forced_shader_names.emplace(0xE30A40E6, "LastKnownLocation");
        #endif

        game = new GameDeusExMankindDivided();
    }

    CoreMain(hModule, ul_reason_for_call, lpReserved);

    return TRUE;
}
