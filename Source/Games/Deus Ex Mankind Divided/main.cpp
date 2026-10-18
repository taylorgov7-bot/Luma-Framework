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
#include <sstream>
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
        ImGui::TextDisabled("Upscaling presets (Quality/Balanced/Performance) are coming in the next build.");

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
            ImGui::TextWrapped("Load a save, stand somewhere in normal gameplay (not a menu), then press the button. It records one frame and saves Luma_DXMD_Diagnostics.txt next to the game's exe.");
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
        if (desc.BindFlags == D3D11_BIND_CONSTANT_BUFFER && desc.ByteWidth == 544)
        {
            g_cbSharedPerViewData_handle = resource.handle;
            g_cbSharedPerViewData_mapped_data = *data;
        }
    }

    static void OnUnmapBufferRegion(reshade::api::device* device, reshade::api::resource resource)
    {
        SKIP_UNSUPPORTED_DEVICE_API(device->get_api());

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

        g_capture_text = ss.str();
        g_capture_records.clear();

        const std::filesystem::path out_path = (g_addon_directory.empty() ? std::filesystem::path(".") : g_addon_directory) / "Luma_DXMD_Diagnostics.txt";
        std::ofstream file(out_path, std::ios::out | std::ios::trunc);
        if (file)
        {
            file << g_capture_text;
            g_capture_status = "Saved to " + out_path.string() + " (you can also copy it to the clipboard).";
        }
        else
        {
            g_capture_status = "Couldn't write the file, use \"Copy to clipboard\" instead.";
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

        // Use the real texture sizes rather than the swapchain size, so this keeps working once the scene renders at a lower resolution.
        const TexInfo scene_info = GetTexInfo(resource_scene.get());
        const TexInfo output_info = GetTexInfo(resource_output.get());
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
        draw_data.output_color = resource_output.get();
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
            ID3D11UnorderedAccessView* raw_uav = uav.get();
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
        Globals::VERSION = 2;

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
