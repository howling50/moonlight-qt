// minwindef.h defines min() and max() macros that conflict with
// std::numeric_limits, which Qt uses in some of its headers.
#define NOMINMAX

#include <initguid.h>
#include "dxva2.h"
#include "../ffmpeg.h"
#include <streaming/streamutils.h>
#include <streaming/session.h>

#include <SDL_syswm.h>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <VersionHelpers.h>
#include <dwmapi.h>

#include <Limelight.h>

DEFINE_GUID(DXVADDI_Intel_ModeH264_E, 0x604F8E68,0x4951,0x4C54,0x88,0xFE,0xAB,0xD2,0x5C,0x15,0xB3,0xD6);

#define SAFE_COM_RELEASE(x) if (x) { (x)->Release(); }

typedef struct _VERTEX
{
    float x, y, z, rhw;
    float tu, tv;
} VERTEX, *PVERTEX;

DXVA2Renderer::DXVA2Renderer() :
    m_DecService(nullptr),
    m_Decoder(nullptr),
    m_SurfacesUsed(0),
    m_Pool(nullptr),
    m_OverlayLock(0),
    m_Device(nullptr),
    m_RenderTarget(nullptr),
    m_ProcService(nullptr),
    m_Processor(nullptr),
    m_FrameIndex(0),
    m_BlockingPresent(false)
{
    RtlZeroMemory(m_DecSurfaces, sizeof(m_DecSurfaces));
    RtlZeroMemory(&m_DXVAContext, sizeof(m_DXVAContext));

    RtlZeroMemory(m_OverlayVertexBuffers, sizeof(m_OverlayVertexBuffers));
    RtlZeroMemory(m_OverlayTextures, sizeof(m_OverlayTextures));

    // Use MMCSS scheduling for lower scheduling latency while we're streaming
    DwmEnableMMCSS(TRUE);
}

DXVA2Renderer::~DXVA2Renderer()
{
    DwmEnableMMCSS(FALSE);

    SAFE_COM_RELEASE(m_DecService);
    SAFE_COM_RELEASE(m_Decoder);
    SAFE_COM_RELEASE(m_Device);
    SAFE_COM_RELEASE(m_RenderTarget);
    SAFE_COM_RELEASE(m_ProcService);
    SAFE_COM_RELEASE(m_Processor);

    for (int i = 0; i < ARRAYSIZE(m_OverlayVertexBuffers); i++) {
        SAFE_COM_RELEASE(m_OverlayVertexBuffers[i]);
    }

    for (int i = 0; i < ARRAYSIZE(m_OverlayTextures); i++) {
        SAFE_COM_RELEASE(m_OverlayTextures[i]);
    }

    for (int i = 0; i < ARRAYSIZE(m_DecSurfaces); i++) {
        SAFE_COM_RELEASE(m_DecSurfaces[i]);
    }

    if (m_Pool != nullptr) {
        av_buffer_pool_uninit(&m_Pool);
    }
}

void DXVA2Renderer::ffPoolDummyDelete(void*, uint8_t*)
{
    /* Do nothing */
}

AVBufferRef* DXVA2Renderer::ffPoolAlloc(void* opaque, int)
{
    DXVA2Renderer* me = reinterpret_cast<DXVA2Renderer*>(opaque);

    if (me->m_SurfacesUsed < ARRAYSIZE(me->m_DecSurfaces)) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "DXVA2 decoder surface high-water mark: %d",
                    me->m_SurfacesUsed);
        return av_buffer_create((uint8_t*)me->m_DecSurfaces[me->m_SurfacesUsed++],
                                sizeof(*me->m_DecSurfaces), ffPoolDummyDelete, 0, 0);
    }

    return NULL;
}

bool DXVA2Renderer::prepareDecoderContext(AVCodecContext* context, AVDictionary**)
{
    // m_DXVAContext.workaround and report_id already initialized elsewhere
    m_DXVAContext.decoder = m_Decoder;
    m_DXVAContext.cfg = &m_Config;
    m_DXVAContext.surface = m_DecSurfaces;
    m_DXVAContext.surface_count = ARRAYSIZE(m_DecSurfaces);

    context->hwaccel_context = &m_DXVAContext;

    context->get_buffer2 = ffGetBuffer2;
#if LIBAVCODEC_VERSION_MAJOR < 60
    AV_NOWARN_DEPRECATED(
        context->thread_safe_callbacks = 1;
    )
#endif

    m_Pool = av_buffer_pool_init2(ARRAYSIZE(m_DecSurfaces), this, ffPoolAlloc, nullptr);
    if (!m_Pool) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Failed create buffer pool");
        return false;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Using DXVA2 accelerated renderer");

    return true;
}

int DXVA2Renderer::ffGetBuffer2(AVCodecContext* context, AVFrame* frame, int)
{
    DXVA2Renderer* me = (DXVA2Renderer*)((FFmpegVideoDecoder*)context->opaque)->getBackendRenderer();

    frame->buf[0] = av_buffer_pool_get(me->m_Pool);
    if (!frame->buf[0]) {
        return AVERROR(ENOMEM);
    }

    frame->data[3] = frame->buf[0]->data;
    frame->format = AV_PIX_FMT_DXVA2_VLD;
    frame->width = me->m_VideoWidth;
    frame->height = me->m_VideoHeight;

    return 0;
}

bool DXVA2Renderer::initializeDecoder()
{
    HRESULT hr;

    if (isDecoderBlacklisted()) {
        return false;
    }

    hr = DXVA2CreateVideoService(m_Device, IID_IDirectXVideoDecoderService,
                                 reinterpret_cast<void**>(&m_DecService));
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "DXVA2CreateVideoService(IID_IDirectXVideoDecoderService) failed: %x",
                     hr);
        return false;
    }

    GUID* guids;
    GUID chosenDeviceGuid;
    UINT guidCount;
    hr = m_DecService->GetDecoderDeviceGuids(&guidCount, &guids);
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "GetDecoderDeviceGuids() failed: %x",
                     hr);
        return false;
    }

    UINT i;
    for (i = 0; i < guidCount; i++) {
        if (m_VideoFormat == VIDEO_FORMAT_H264) {
            if (IsEqualGUID(guids[i], DXVA2_ModeH264_E) ||
                    IsEqualGUID(guids[i], DXVA2_ModeH264_F)) {
                chosenDeviceGuid = guids[i];
                break;
            }
            else if (IsEqualGUID(guids[i], DXVADDI_Intel_ModeH264_E)) {
                chosenDeviceGuid = guids[i];
                m_DXVAContext.workaround |= FF_DXVA2_WORKAROUND_INTEL_CLEARVIDEO;
                break;
            }
        }
        else if (m_VideoFormat == VIDEO_FORMAT_H265) {
            if (IsEqualGUID(guids[i], DXVA2_ModeHEVC_VLD_Main)) {
                chosenDeviceGuid = guids[i];
                break;
            }
        }
        else if (m_VideoFormat == VIDEO_FORMAT_H265_MAIN10) {
            if (IsEqualGUID(guids[i], DXVA2_ModeHEVC_VLD_Main10)) {
                chosenDeviceGuid = guids[i];
                break;
            }
        }
    }

    CoTaskMemFree(guids);

    if (i == guidCount) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "No matching decoder device GUIDs");
        return false;
    }

    DXVA2_ConfigPictureDecode* configs;
    UINT configCount;
    hr = m_DecService->GetDecoderConfigurations(chosenDeviceGuid, &m_Desc, nullptr, &configCount, &configs);
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "GetDecoderConfigurations() failed: %x",
                     hr);
        return false;
    }

    for (i = 0; i < configCount; i++) {
        if ((configs[i].ConfigBitstreamRaw == 1 || configs[i].ConfigBitstreamRaw == 2) &&
                IsEqualGUID(configs[i].guidConfigBitstreamEncryption, DXVA2_NoEncrypt)) {
            m_Config = configs[i];
            break;
        }
    }

    CoTaskMemFree(configs);

    if (i == configCount) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "No matching decoder configurations");
        return false;
    }

    // Alignment was already taken care of
    SDL_assert(m_Desc.SampleWidth % 16 == 0);
    SDL_assert(m_Desc.SampleHeight % 16 == 0);
    hr = m_DecService->CreateSurface(m_Desc.SampleWidth,
                                     m_Desc.SampleHeight,
                                     ARRAYSIZE(m_DecSurfaces) - 1,
                                     m_Desc.Format,
                                     D3DPOOL_DEFAULT,
                                     0,
                                     DXVA2_VideoDecoderRenderTarget,
                                     m_DecSurfaces,
                                     nullptr);
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "CreateSurface() failed: %x",
                     hr);
        return false;
    }

    hr = m_DecService->CreateVideoDecoder(chosenDeviceGuid, &m_Desc, &m_Config,
                                          m_DecSurfaces, ARRAYSIZE(m_DecSurfaces),
                                          &m_Decoder);
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "CreateVideoDecoder() failed: %x",
                     hr);
        return false;
    }

    return true;
}

bool DXVA2Renderer::initializeRenderer()
{
    HRESULT hr;

    hr = m_Device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &m_RenderTarget);
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "GetBackBuffer() failed: %x",
                     hr);
        return false;
    }

    D3DSURFACE_DESC renderTargetDesc;
    m_RenderTarget->GetDesc(&renderTargetDesc);

    m_DisplayWidth = renderTargetDesc.Width;
    m_DisplayHeight = renderTargetDesc.Height;

    if (!isDXVideoProcessorAPIBlacklisted()) {
        hr = DXVA2CreateVideoService(m_Device, IID_IDirectXVideoProcessorService,
                                     reinterpret_cast<void**>(&m_ProcService));

        if (FAILED(hr)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "DXVA2CreateVideoService(IID_IDirectXVideoProcessorService) failed: %x",
                         hr);
            return false;
        }

        DXVA2_VideoProcessorCaps caps;
        hr = m_ProcService->GetVideoProcessorCaps(DXVA2_VideoProcProgressiveDevice, &m_Desc, renderTargetDesc.Format, &caps);
        if (FAILED(hr)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "GetVideoProcessorCaps() failed for DXVA2_VideoProcProgressiveDevice: %x",
                         hr);
            return false;
        }

        if (!(caps.DeviceCaps & DXVA2_VPDev_HardwareDevice)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "DXVA2_VideoProcProgressiveDevice is not hardware: %x",
                         caps.DeviceCaps);
            return false;
        }
        else if (!(caps.VideoProcessorOperations & DXVA2_VideoProcess_YUV2RGB) &&
                 !(caps.VideoProcessorOperations & DXVA2_VideoProcess_YUV2RGBExtended)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "DXVA2_VideoProcProgressiveDevice can't convert YUV2RGB: %x",
                         caps.VideoProcessorOperations);
            return false;
        }
        else if (!(caps.VideoProcessorOperations & DXVA2_VideoProcess_StretchX) ||
                 !(caps.VideoProcessorOperations & DXVA2_VideoProcess_StretchY)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "DXVA2_VideoProcProgressiveDevice can't stretch video: %x",
                         caps.VideoProcessorOperations);
            return false;
        }

        if (caps.DeviceCaps & DXVA2_VPDev_EmulatedDXVA1) {
            // DXVA2 over DXVA1 may have bad performance
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "DXVA2_VideoProcProgressiveDevice is DXVA1");
        }

        m_ProcService->GetProcAmpRange(DXVA2_VideoProcProgressiveDevice, &m_Desc, renderTargetDesc.Format, DXVA2_ProcAmp_Brightness, &m_BrightnessRange);
        m_ProcService->GetProcAmpRange(DXVA2_VideoProcProgressiveDevice, &m_Desc, renderTargetDesc.Format, DXVA2_ProcAmp_Contrast, &m_ContrastRange);
        m_ProcService->GetProcAmpRange(DXVA2_VideoProcProgressiveDevice, &m_Desc, renderTargetDesc.Format, DXVA2_ProcAmp_Hue, &m_HueRange);
        m_ProcService->GetProcAmpRange(DXVA2_VideoProcProgressiveDevice, &m_Desc, renderTargetDesc.Format, DXVA2_ProcAmp_Saturation, &m_SaturationRange);

        hr = m_ProcService->CreateVideoProcessor(DXVA2_VideoProcProgressiveDevice, &m_Desc, renderTargetDesc.Format, 0, &m_Processor);
        if (FAILED(hr)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "CreateVideoProcessor() failed for DXVA2_VideoProcProgressiveDevice: %x",
                         hr);
            return false;
        }
    }

    m_Device->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    m_Device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    m_Device->SetRenderState(D3DRS_LIGHTING, FALSE);

    m_Device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    m_Device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    m_Device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);

    m_Device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    m_Device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);

    m_Device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    m_Device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    m_Device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);

    m_Device->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);

    return true;
}

bool DXVA2Renderer::isDXVideoProcessorAPIBlacklisted()
{
    IDirect3D9* d3d9;
    HRESULT hr;
    bool result = false;

    if (qgetenv("DXVA2_DISABLE_VIDPROC_BLACKLIST") == "1") {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "IDirectXVideoProcessor blacklist is disabled");
        return false;
    }

    hr = m_Device->GetDirect3D(&d3d9);
    if (SUCCEEDED(hr)) {
        D3DCAPS9 caps;

        hr = m_Device->GetDeviceCaps(&caps);
        if (SUCCEEDED(hr)) {
            D3DADAPTER_IDENTIFIER9 id;

            hr = d3d9->GetAdapterIdentifier(caps.AdapterOrdinal, 0, &id);
            if (SUCCEEDED(hr)) {
                if (id.VendorId == 0x8086) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "Avoiding IDirectXVideoProcessor API on Intel GPU");

                    // On Intel GPUs, we can get unwanted video "enhancements" due to post-processing
                    // effects that the GPU driver forces on us. In many cases, this makes the video
                    // actually look worse. We can avoid these by using StretchRect() instead on these
                    // platforms.
                    result = true;
                }
                else if (id.VendorId == 0x4d4f4351) { // QCOM in ASCII
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "Avoiding IDirectXVideoProcessor API on Qualcomm GPU");

                    // On Qualcomm GPUs (all D3D9on12 GPUs?), the scaling quality of VideoProcessBlt()
                    // is absolutely horrible. StretchRect() is much much better.
                    result = true;
                }
                else {
                    result = false;
                }
            }
            else {
                result = false;
            }
        }
        else {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "GetDeviceCaps() failed: %x", hr);
        }

        d3d9->Release();
    }
    else {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "GetDirect3D() failed: %x", hr);
    }

    return result;
}

bool DXVA2Renderer::isDecoderBlacklisted()
{
    IDirect3D9* d3d9;
    HRESULT hr;
    bool result = false;

    if (qgetenv("DXVA2_DISABLE_DECODER_BLACKLIST") == "1") {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "DXVA2 decoder blacklist is disabled");
        return false;
    }

    hr = m_Device->GetDirect3D(&d3d9);
    if (SUCCEEDED(hr)) {
        D3DCAPS9 caps;

        hr = m_Device->GetDeviceCaps(&caps);
        if (SUCCEEDED(hr)) {
            D3DADAPTER_IDENTIFIER9 id;

            hr = d3d9->GetAdapterIdentifier(caps.AdapterOrdinal, 0, &id);
            if (SUCCEEDED(hr)) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Detected GPU: %s (%x:%x)",
                            id.Description,
                            id.VendorId,
                            id.DeviceId);
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "GPU driver: %s %d.%d.%d.%d",
                            id.Driver,
                            HIWORD(id.DriverVersion.HighPart),
                            LOWORD(id.DriverVersion.HighPart),
                            HIWORD(id.DriverVersion.LowPart),
                            LOWORD(id.DriverVersion.LowPart));

                if (id.VendorId == 0x8086) {
                    // Intel seems to encode the series in the high byte of
                    // the device ID. We want to avoid the "Partial" acceleration
                    // support explicitly. Those will claim to have HW acceleration
                    // but perform badly.
                    // https://en.wikipedia.org/wiki/Intel_Graphics_Technology#Capabilities_(GPU_video_acceleration)
                    // https://raw.githubusercontent.com/GameTechDev/gpudetect/master/IntelGfx.cfg
                    switch (id.DeviceId & 0xFF00) {
                    case 0x0400: // Haswell
                    case 0x0A00: // Haswell
                    case 0x0D00: // Haswell
                    case 0x1600: // Broadwell
                    case 0x2200: // Cherry Trail and Braswell
                        // Blacklist these for HEVC to avoid hybrid decode
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                    "GPU blacklisted for HEVC due to hybrid decode");
                        result = (m_VideoFormat & VIDEO_FORMAT_MASK_H265) != 0;
                        break;
                    case 0x1900: // Skylake
                        // Blacklist these for HEVC Main10 to avoid hybrid decode.
                        // Regular HEVC Main is fine though.
                        if (m_VideoFormat == VIDEO_FORMAT_H265_MAIN10) {
                            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                        "GPU blacklisted for HEVC Main10 due to hybrid decode");
                            result = false;
                            break;
                        }
                    default:
                        // Intel drivers from before late-2017 had a bug that caused some strange artifacts
                        // when decoding HEVC. Avoid HEVC on drivers prior to build 4836 which I confirmed
                        // is not affected on my Intel HD 515. Also account for the driver version rollover
                        // that happened with the 101.1069 series.
                        // https://github.com/moonlight-stream/moonlight-qt/issues/32
                        // https://www.intel.com/content/www/us/en/support/articles/000005654/graphics-drivers.html
                        if (HIWORD(id.DriverVersion.LowPart) < 100 && LOWORD(id.DriverVersion.LowPart) < 4836) {
                            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                        "Detected buggy Intel GPU driver installed. Update your Intel GPU driver to enable HEVC!");
                            result = (m_VideoFormat & VIDEO_FORMAT_MASK_H265) != 0;
                        }
                        else {
                            // Everything else is fine with whatever it says it supports
                            result = false;
                        }
                        break;
                    }
                }
                else if (id.VendorId == 0x10DE) {
                    // For NVIDIA, we wait to avoid those GPUs with Feature Set E
                    // for HEVC decoding, since that's hybrid. It appears that Kepler GPUs
                    // also had some hybrid decode support (per DXVA2 Checker) so we'll
                    // blacklist those too.
                    // https://en.wikipedia.org/wiki/Nvidia_PureVideo
                    // https://bluesky23.yukishigure.com/en/dxvac/deviceInfo/decoder.html
                    // http://envytools.readthedocs.io/en/latest/hw/pciid.html (missing GM200)
                    if ((id.DeviceId >= 0x1180 && id.DeviceId <= 0x11BF) || // GK104
                            (id.DeviceId >= 0x11C0 && id.DeviceId <= 0x11FF) || // GK106
                            (id.DeviceId >= 0x0FC0 && id.DeviceId <= 0x0FFF) || // GK107
                            (id.DeviceId >= 0x1000 && id.DeviceId <= 0x103F) || // GK110/GK110B
                            (id.DeviceId >= 0x1280 && id.DeviceId <= 0x12BF) || // GK208
                            (id.DeviceId >= 0x1340 && id.DeviceId <= 0x137F) || // GM108
                            (id.DeviceId >= 0x1380 && id.DeviceId <= 0x13BF) || // GM107
                            (id.DeviceId >= 0x13C0 && id.DeviceId <= 0x13FF) || // GM204
                            (id.DeviceId >= 0x1617 && id.DeviceId <= 0x161A) || // GM204
                            (id.DeviceId == 0x1667) || // GM204
                            (id.DeviceId >= 0x17C0 && id.DeviceId <= 0x17FF)) { // GM200
                        // Avoid HEVC on Feature Set E GPUs
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                    "GPU blacklisted for HEVC due to hybrid decode");
                        result = (m_VideoFormat & VIDEO_FORMAT_MASK_H265) != 0;
                    }
                    else {
                        result = false;
                    }
                }
                else if (id.VendorId == 0x1002) {
                    // AMD doesn't seem to do hybrid acceleration?
                    result = false;
                }
                else {
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                                "Unrecognized vendor ID: %x",
                                id.VendorId);
                }
            }
        }
        else {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "GetDeviceCaps() failed: %x", hr);
        }

        d3d9->Release();
    }
    else {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "GetDirect3D() failed: %x", hr);
    }

    if (result) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "GPU blacklisted for format %x",
                    m_VideoFormat);
    }

    return result;
}

bool DXVA2Renderer::initializeDevice(SDL_Window* window, bool enableVsync)
{
    SDL_SysWMinfo info;

    SDL_VERSION(&info.version);
    SDL_GetWindowWMInfo(window, &info);

    IDirect3D9Ex* d3d9ex;
    HRESULT hr = Direct3DCreate9Ex(D3D_SDK_VERSION, &d3d9ex);
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Direct3DCreate9Ex() failed: %x",
                     hr);
        return false;
    }

    int adapterIndex = SDL_Direct3D9GetAdapterIndex(SDL_GetWindowDisplayIndex(window));
    Uint32 windowFlags = SDL_GetWindowFlags(window);

    D3DCAPS9 deviceCaps;
    d3d9ex->GetDeviceCaps(adapterIndex, D3DDEVTYPE_HAL, &deviceCaps);

    D3DDISPLAYMODEEX currentMode;
    currentMode.Size = sizeof(currentMode);
    d3d9ex->GetAdapterDisplayModeEx(adapterIndex, &currentMode, nullptr);

    D3DPRESENT_PARAMETERS d3dpp = {};
    d3dpp.hDeviceWindow = info.info.win.window;
    d3dpp.Flags = D3DPRESENTFLAG_VIDEO;

    if (m_VideoFormat == VIDEO_FORMAT_H265_MAIN10) {
        // Verify 10-bit A2R10G10B10 color support. This is only available
        // as a display format in full-screen exclusive mode on DX9.
        hr = d3d9ex->CheckDeviceType(adapterIndex,
                                     D3DDEVTYPE_HAL,
                                     D3DFMT_A2R10G10B10,
                                     D3DFMT_A2R10G10B10,
                                     FALSE);
        if (FAILED(hr)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "GPU/driver doesn't support A2R10G10B10");
            d3d9ex->Release();
            return false;
        }
    }

    if ((windowFlags & SDL_WINDOW_FULLSCREEN_DESKTOP) == SDL_WINDOW_FULLSCREEN) {
        d3dpp.Windowed = false;
        d3dpp.BackBufferWidth = currentMode.Width;
        d3dpp.BackBufferHeight = currentMode.Height;
        d3dpp.FullScreen_RefreshRateInHz = currentMode.RefreshRate;

        if (m_VideoFormat == VIDEO_FORMAT_H265_MAIN10) {
            d3dpp.BackBufferFormat = currentMode.Format = D3DFMT_A2R10G10B10;
        }
        else {
            d3dpp.BackBufferFormat = currentMode.Format;
        }
    }
    else {
        d3dpp.Windowed = true;
        d3dpp.BackBufferFormat = D3DFMT_UNKNOWN;

        SDL_GetWindowSize(window, (int*)&d3dpp.BackBufferWidth, (int*)&d3dpp.BackBufferHeight);
    }

    BOOL dwmEnabled;
    DwmIsCompositionEnabled(&dwmEnabled);
    if (d3dpp.Windowed && dwmEnabled) {
        // If composition enabled, disable v-sync and let DWM manage things
        // to reduce latency by avoiding double v-syncing.
        d3dpp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

        // If V-sync is enabled (not rendering faster than display),
        // we can use FlipEx for more efficient swapping.
        if (enableVsync) {
            // D3DSWAPEFFECT_FLIPEX requires at least 2 back buffers to allow us to
            // continue while DWM is waiting to render the surface to the display.
            d3dpp.SwapEffect = D3DSWAPEFFECT_FLIPEX;
            d3dpp.BackBufferCount = 2;
        }
        else {
            // With V-sync off, we won't use FlipEx because that will block while
            // DWM is waiting to render our surface (effectively behaving like V-Sync).
            d3dpp.SwapEffect = D3DSWAPEFFECT_DISCARD;
            d3dpp.BackBufferCount = 1;
        }

        m_BlockingPresent = false;

        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Windowed mode with DWM running");
    }
    else if (enableVsync) {
        // Uncomposited desktop or full-screen exclusive mode with V-sync enabled
        // We will enable V-sync in this scenario to avoid tearing.
        d3dpp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
        d3dpp.SwapEffect = D3DSWAPEFFECT_DISCARD;
        d3dpp.BackBufferCount = 1;
        m_BlockingPresent = true;

        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "V-Sync enabled");
    }
    else {
        // Uncomposited desktop or full-screen exclusive mode with V-sync disabled
        // We will allowing tearing for lowest latency.
        d3dpp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
        d3dpp.SwapEffect = D3DSWAPEFFECT_DISCARD;
        d3dpp.BackBufferCount = 1;
        m_BlockingPresent = false;

        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "V-Sync disabled in tearing mode");
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Windowed: %d | Present Interval: %x",
                d3dpp.Windowed, d3dpp.PresentationInterval);

    // FFmpeg requires this attribute for doing asynchronous decoding
    // in a separate thread with this device.
    int deviceFlags = D3DCREATE_MULTITHREADED;

    if (deviceCaps.DevCaps & D3DDEVCAPS_HWTRANSFORMANDLIGHT) {
        deviceFlags |= D3DCREATE_HARDWARE_VERTEXPROCESSING;
    }
    else {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "No hardware vertex processing support!");
        deviceFlags |= D3DCREATE_SOFTWARE_VERTEXPROCESSING;
    }

    hr = d3d9ex->CreateDeviceEx(adapterIndex,
                                D3DDEVTYPE_HAL,
                                d3dpp.hDeviceWindow,
                                deviceFlags,
                                &d3dpp,
                                d3dpp.Windowed ? nullptr : &currentMode,
                                &m_Device);

    d3d9ex->Release();

    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "CreateDeviceEx() failed: %x",
                     hr);
        return false;
    }

    hr = m_Device->SetMaximumFrameLatency(1);
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "SetMaximumFrameLatency() failed: %x",
                     hr);
        return false;
    }

    return true;
}

bool DXVA2Renderer::initialize(PDECODER_PARAMETERS params)
{
    m_VideoFormat = params->videoFormat;
    m_VideoWidth = params->width;
    m_VideoHeight = params->height;

    RtlZeroMemory(&m_Desc, sizeof(m_Desc));

    int alignment;

    // HEVC using DXVA requires 128 pixel alignment, however Intel GPUs decoding HEVC
    // using StretchRect() to render draw a translucent green line at the top of
    // the screen in full-screen mode at 720p/1080p unless we use 32 pixel alignment.
    // This appears to work without issues on AMD and Nvidia GPUs too, so we will
    // do it unconditionally for now.
    if (m_VideoFormat & VIDEO_FORMAT_MASK_H265) {
        alignment = 32;
    }
    else {
        alignment = 16;
    }

    m_Desc.SampleWidth = FFALIGN(m_VideoWidth, alignment);
    m_Desc.SampleHeight = FFALIGN(m_VideoHeight, alignment);
    m_Desc.SampleFormat.VideoChromaSubsampling = DXVA2_VideoChromaSubsampling_Unknown;
    m_Desc.SampleFormat.NominalRange = DXVA2_NominalRange_Unknown;
    m_Desc.SampleFormat.VideoTransferMatrix = DXVA2_VideoTransferMatrix_Unknown;
    m_Desc.SampleFormat.VideoLighting = DXVA2_VideoLighting_Unknown;
    m_Desc.SampleFormat.VideoPrimaries = DXVA2_VideoPrimaries_Unknown;
    m_Desc.SampleFormat.VideoTransferFunction = DXVA2_VideoTransFunc_Unknown;
    m_Desc.SampleFormat.SampleFormat = DXVA2_SampleProgressiveFrame;

    if (m_VideoFormat == VIDEO_FORMAT_H265_MAIN10) {
        m_Desc.Format = (D3DFORMAT)MAKEFOURCC('P','0','1','0');
    }
    else {
        m_Desc.Format = (D3DFORMAT)MAKEFOURCC('N','V','1','2');
    }

    if (!initializeDevice(params->window, params->enableVsync)) {
        return false;
    }

    if (!initializeDecoder()) {
        return false;
    }

    if (!initializeRenderer()) {
        return false;
    }

    // For some reason, using Direct3D9Ex breaks this with multi-monitor setups.
    // When focus is lost, the window is minimized then immediately restored without
    // input focus. This glitches out the renderer and a bunch of other stuff.
    // Direct3D9Ex itself seems to have this minimize on focus loss behavior on its
    // own, so just disable SDL's handling of the focus loss event.
    SDL_SetHintWithPriority(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0", SDL_HINT_OVERRIDE);

    return true;
}

void DXVA2Renderer::notifyOverlayUpdated(Overlay::OverlayType type)
{
    HRESULT hr;

    SDL_Surface* newSurface = Session::get()->getOverlayManager().getUpdatedOverlaySurface(type);
    if (newSurface == nullptr && Session::get()->getOverlayManager().isOverlayEnabled(type)) {
        // The overlay is enabled and there is no new surface. Leave the old texture alone.
        return;
    }

    SDL_AtomicLock(&m_OverlayLock);
    IDirect3DTexture9* oldTexture = m_OverlayTextures[type];
    m_OverlayTextures[type] = nullptr;

    IDirect3DVertexBuffer9* oldVertexBuffer = m_OverlayVertexBuffers[type];
    m_OverlayVertexBuffers[type] = nullptr;
    SDL_AtomicUnlock(&m_OverlayLock);

    SAFE_COM_RELEASE(oldTexture);
    SAFE_COM_RELEASE(oldVertexBuffer);

    // If the overlay is disabled, we're done
    if (!Session::get()->getOverlayManager().isOverlayEnabled(type)) {
        SDL_FreeSurface(newSurface);
        return;
    }

    // Create a dynamic texture to populate with our pixel data
    SDL_assert(!SDL_MUSTLOCK(newSurface));
    IDirect3DTexture9* newTexture = nullptr;
    hr = m_Device->CreateTexture(newSurface->w,
                                 newSurface->h,
                                 1,
                                 D3DUSAGE_DYNAMIC,
                                 D3DFMT_A8R8G8B8,
                                 D3DPOOL_DEFAULT,
                                 &newTexture,
                                 nullptr);
    if (FAILED(hr)) {
        SDL_FreeSurface(newSurface);
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "CreateTexture() failed: %x",
                     hr);
        return;
    }

    D3DLOCKED_RECT lockedRect;
    hr = newTexture->LockRect(0, &lockedRect, nullptr, D3DLOCK_DISCARD);
    if (FAILED(hr)) {
        SDL_FreeSurface(newSurface);
        SAFE_COM_RELEASE(newTexture);
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "IDirect3DTexture9::LockRect() failed: %x",
                     hr);
        return;
    }

    if (newSurface->pitch == lockedRect.Pitch) {
        // If the pitch matches, we can take the fast path and use a single copy to transfer the pixels
        RtlCopyMemory(lockedRect.pBits, newSurface->pixels, newSurface->pitch * newSurface->h);
    }
    else {
        // If the pitch doesn't match, we'll need to copy each row separately
        int pitch = SDL_min(newSurface->pitch, lockedRect.Pitch);
        for (int i = 0; i < newSurface->h; i++) {
            RtlCopyMemory(((PUCHAR)lockedRect.pBits) + (lockedRect.Pitch * i),
                          ((PUCHAR)newSurface->pixels) + (newSurface->pitch * i),
                          pitch);
        }
    }

    newTexture->UnlockRect(0);

    SDL_FRect renderRect = {};

    if (type == Overlay::OverlayStatusUpdate) {
        // Bottom Left
        renderRect.x = 0;
        renderRect.y = m_DisplayHeight - newSurface->h;
    }
    else if (type == Overlay::OverlayDebug) {
        // Top left
        renderRect.x = 0;
        renderRect.y = 0;
    }

    renderRect.w = newSurface->w;
    renderRect.h = newSurface->h;

    // The surface is no longer required
    SDL_FreeSurface(newSurface);
    newSurface = nullptr;

    VERTEX verts[] =
    {
        {renderRect.x, renderRect.y, 0, 1, 0, 0},
        {renderRect.x, renderRect.y+renderRect.h, 0, 1, 0, 1},
        {renderRect.x+renderRect.w, renderRect.y+renderRect.h, 0, 1, 1, 1},
        {renderRect.x+renderRect.w, renderRect.y, 0, 1, 1, 0}
    };

    IDirect3DVertexBuffer9* newVertexBuffer = nullptr;
    hr = m_Device->CreateVertexBuffer(sizeof(verts),
                                      D3DUSAGE_WRITEONLY,
                                      D3DFVF_XYZRHW | D3DFVF_TEX1,
                                      D3DPOOL_DEFAULT,
                                      &newVertexBuffer,
                                      nullptr);
    if (FAILED(hr)) {
        SAFE_COM_RELEASE(newTexture);
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "CreateVertexBuffer() failed: %x",
                     hr);
        return;
    }

    PVOID targetVertexBuffer = nullptr;
    hr = newVertexBuffer->Lock(0, 0, &targetVertexBuffer, 0);
    if (FAILED(hr)) {
        SAFE_COM_RELEASE(newTexture);
        SAFE_COM_RELEASE(newVertexBuffer);
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "IDirect3DVertexBuffer9::Lock() failed: %x",
                     hr);
        return;
    }

    RtlCopyMemory(targetVertexBuffer, verts, sizeof(verts));

    newVertexBuffer->Unlock();

    SDL_AtomicLock(&m_OverlayLock);
    m_OverlayVertexBuffers[type] = newVertexBuffer;
    m_OverlayTextures[type] = newTexture;
    SDL_AtomicUnlock(&m_OverlayLock);
}

void DXVA2Renderer::renderOverlay(Overlay::OverlayType type)
{
    HRESULT hr;

    if (!Session::get()->getOverlayManager().isOverlayEnabled(type)) {
        return;
    }

    // If the overlay is being updated, just skip rendering it this frame
    if (!SDL_AtomicTryLock(&m_OverlayLock)) {
        return;
    }

    IDirect3DTexture9* overlayTexture = m_OverlayTextures[type];
    IDirect3DVertexBuffer9* overlayVertexBuffer = m_OverlayVertexBuffers[type];

    if (overlayTexture == nullptr) {
        SDL_AtomicUnlock(&m_OverlayLock);
        return;
    }

    // Reference these objects so they don't immediately go away if the
    // overlay update thread tries to release them.
    SDL_assert(overlayVertexBuffer != nullptr);
    overlayTexture->AddRef();
    overlayVertexBuffer->AddRef();

    SDL_AtomicUnlock(&m_OverlayLock);

    hr = m_Device->SetTexture(0, overlayTexture);
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "SetTexture() failed: %x",
                     hr);
        goto Exit;
    }

    hr = m_Device->SetStreamSource(0, overlayVertexBuffer, 0, sizeof(VERTEX));
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "SetStreamSource() failed: %x",
                     hr);
        goto Exit;
    }

    hr = m_Device->DrawPrimitive(D3DPT_TRIANGLEFAN, 0, 2);
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "DrawPrimitive() failed: %x",
                     hr);
        goto Exit;
    }

Exit:
    overlayTexture->Release();
    overlayVertexBuffer->Release();
}

int DXVA2Renderer::getDecoderColorspace()
{
    if (isDXVideoProcessorAPIBlacklisted()) {
        // StretchRect() assumes Rec 601 on Intel and Qualcomm GPUs.
        return COLORSPACE_REC_601;
    }
    else {
        // VideoProcessBlt() *should* properly handle whatever, since
        // we provide colorspace information. However, AMD GPUs seem to
        // always assume Rec 709, so we'll use that as our default.
        return COLORSPACE_REC_709;
    }
}

void DXVA2Renderer::renderFrame(AVFrame *frame)
{
    if (frame == nullptr) {
        // End of stream - nothing to do for us
        return;
    }

    IDirect3DSurface9* surface = reinterpret_cast<IDirect3DSurface9*>(frame->data[3]);
    HRESULT hr;

    switch (frame->color_range) {
    case AVCOL_RANGE_JPEG:
        m_Desc.SampleFormat.NominalRange = DXVA2_NominalRange_0_255;
        break;
    case AVCOL_RANGE_MPEG:
        m_Desc.SampleFormat.NominalRange = DXVA2_NominalRange_16_235;
        break;
    default:
        m_Desc.SampleFormat.NominalRange = DXVA2_NominalRange_Unknown;
        break;
    }

    switch (frame->color_primaries) {
    case AVCOL_PRI_BT709:
        m_Desc.SampleFormat.VideoPrimaries = DXVA2_VideoPrimaries_BT709;
        break;
    case AVCOL_PRI_BT470M:
        m_Desc.SampleFormat.VideoPrimaries = DXVA2_VideoPrimaries_BT470_2_SysM;
        break;
    case AVCOL_PRI_BT470BG:
        m_Desc.SampleFormat.VideoPrimaries = DXVA2_VideoPrimaries_BT470_2_SysBG;
        break;
    case AVCOL_PRI_SMPTE170M:
        m_Desc.SampleFormat.VideoPrimaries = DXVA2_VideoPrimaries_SMPTE170M;
        break;
    case AVCOL_PRI_SMPTE240M:
        m_Desc.SampleFormat.VideoPrimaries = DXVA2_VideoPrimaries_SMPTE240M;
        break;
    default:
        m_Desc.SampleFormat.VideoPrimaries = DXVA2_VideoPrimaries_Unknown;
        break;
    }

    switch (frame->color_trc) {
    case AVCOL_TRC_SMPTE170M:
    case AVCOL_TRC_BT709:
        m_Desc.SampleFormat.VideoTransferFunction = DXVA2_VideoTransFunc_709;
        break;
    case AVCOL_TRC_LINEAR:
        m_Desc.SampleFormat.VideoTransferFunction = DXVA2_VideoTransFunc_10;
        break;
    case AVCOL_TRC_GAMMA22:
        m_Desc.SampleFormat.VideoTransferFunction = DXVA2_VideoTransFunc_22;
        break;
    case AVCOL_TRC_GAMMA28:
        m_Desc.SampleFormat.VideoTransferFunction = DXVA2_VideoTransFunc_28;
        break;
    case AVCOL_TRC_SMPTE240M:
        m_Desc.SampleFormat.VideoTransferFunction = DXVA2_VideoTransFunc_240M;
        break;
    case AVCOL_TRC_IEC61966_2_1:
        m_Desc.SampleFormat.VideoTransferFunction = DXVA2_VideoTransFunc_sRGB;
        break;
    default:
        m_Desc.SampleFormat.VideoTransferFunction = DXVA2_VideoTransFunc_Unknown;
        break;
    }

    switch (frame->colorspace) {
    case AVCOL_SPC_BT709:
        m_Desc.SampleFormat.VideoTransferMatrix = DXVA2_VideoTransferMatrix_BT709;
        break;
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M:
        m_Desc.SampleFormat.VideoTransferMatrix = DXVA2_VideoTransferMatrix_BT601;
        break;
    case AVCOL_SPC_SMPTE240M:
        m_Desc.SampleFormat.VideoTransferMatrix = DXVA2_VideoTransferMatrix_SMPTE240M;
        break;
    default:
        m_Desc.SampleFormat.VideoTransferMatrix = DXVA2_VideoTransferMatrix_Unknown;
        break;
    }

    switch (frame->chroma_location) {
    case AVCHROMA_LOC_LEFT:
        m_Desc.SampleFormat.VideoChromaSubsampling = DXVA2_VideoChromaSubsampling_Horizontally_Cosited |
                                                     DXVA2_VideoChromaSubsampling_Vertically_AlignedChromaPlanes;
        break;
    case AVCHROMA_LOC_CENTER:
        m_Desc.SampleFormat.VideoChromaSubsampling = DXVA2_VideoChromaSubsampling_Vertically_AlignedChromaPlanes;
        break;
    case AVCHROMA_LOC_TOPLEFT:
        m_Desc.SampleFormat.VideoChromaSubsampling = DXVA2_VideoChromaSubsampling_Horizontally_Cosited |
                                                     DXVA2_VideoChromaSubsampling_Vertically_Cosited;
        break;
    default:
        m_Desc.SampleFormat.VideoChromaSubsampling = DXVA2_VideoChromaSubsampling_Unknown;
        break;
    }

    DXVA2_VideoSample sample = {};
    sample.Start = m_FrameIndex;
    sample.End = m_FrameIndex + 1;
    sample.SrcSurface = surface;
    sample.SrcRect.right = m_VideoWidth;
    sample.SrcRect.bottom = m_VideoHeight;
    sample.SampleFormat = m_Desc.SampleFormat;
    sample.PlanarAlpha = DXVA2_Fixed32OpaqueAlpha();

    // Center in frame and preserve aspect ratio
    SDL_Rect src, dst;
    src.x = src.y = 0;
    src.w = m_VideoWidth;
    src.h = m_VideoHeight;
    dst.x = dst.y = 0;
    dst.w = m_DisplayWidth;
    dst.h = m_DisplayHeight;

    StreamUtils::scaleSourceToDestinationSurface(&src, &dst);

    sample.DstRect.left = dst.x;
    sample.DstRect.right = dst.x + dst.w;
    sample.DstRect.top = dst.y;
    sample.DstRect.bottom = dst.y + dst.h;

    DXVA2_VideoProcessBltParams bltParams = {};

    bltParams.TargetFrame = m_FrameIndex++;
    bltParams.TargetRect = sample.DstRect;
    bltParams.BackgroundColor.Alpha = 0xFFFF;

    bltParams.DestFormat.SampleFormat = DXVA2_SampleProgressiveFrame;

    bltParams.ProcAmpValues.Brightness = m_BrightnessRange.DefaultValue;
    bltParams.ProcAmpValues.Contrast = m_ContrastRange.DefaultValue;
    bltParams.ProcAmpValues.Hue = m_HueRange.DefaultValue;
    bltParams.ProcAmpValues.Saturation = m_SaturationRange.DefaultValue;

    bltParams.Alpha = DXVA2_Fixed32OpaqueAlpha();

    hr = m_Device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB(255, 0, 0, 0), 0.0f, 0);
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Clear() failed: %x",
                     hr);
        SDL_Event event;
        event.type = SDL_RENDER_TARGETS_RESET;
        SDL_PushEvent(&event);
        return;
    }

    hr = m_Device->BeginScene();
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "BeginScene() failed: %x",
                     hr);
        SDL_Event event;
        event.type = SDL_RENDER_TARGETS_RESET;
        SDL_PushEvent(&event);
        return;
    }

    if (m_Processor) {
        hr = m_Processor->VideoProcessBlt(m_RenderTarget, &bltParams, &sample, 1, nullptr);
        if (FAILED(hr)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "VideoProcessBlt() failed, falling back to StretchRect(): %x",
                         hr);
            m_Processor->Release();
            m_Processor = nullptr;
        }
    }

    if (!m_Processor) {
        // This function doesn't trigger any of Intel's garbage video "enhancements"
        hr = m_Device->StretchRect(surface, &sample.SrcRect, m_RenderTarget, &sample.DstRect, D3DTEXF_NONE);
        if (FAILED(hr)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "StretchRect() failed: %x",
                         hr);
            SDL_Event event;
            event.type = SDL_RENDER_TARGETS_RESET;
            SDL_PushEvent(&event);
            return;
        }
    }

    // Render overlays on top of the video stream
    for (int i = 0; i < Overlay::OverlayMax; i++) {
        renderOverlay((Overlay::OverlayType)i);
    }

    hr = m_Device->EndScene();
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "EndScene() failed: %x",
                     hr);
        SDL_Event event;
        event.type = SDL_RENDER_TARGETS_RESET;
        SDL_PushEvent(&event);
        return;
    }

    do {
        // Use D3DPRESENT_DONOTWAIT if present may block in order to avoid holding the giant
        // lock around this D3D device for excessive lengths of time (blocking concurrent decoding tasks).
        hr = m_Device->PresentEx(nullptr, nullptr, nullptr, nullptr, m_BlockingPresent ? D3DPRESENT_DONOTWAIT : 0);
        if (hr == D3DERR_WASSTILLDRAWING) {
            SDL_Delay(1);
        }
    } while (hr == D3DERR_WASSTILLDRAWING);
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "PresentEx() failed: %x",
                     hr);
        SDL_Event event;
        event.type = SDL_RENDER_TARGETS_RESET;
        SDL_PushEvent(&event);
        return;
    }
}
