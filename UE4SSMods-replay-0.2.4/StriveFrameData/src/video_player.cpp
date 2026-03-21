#include "video_player.h"
#include "combo_log.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>

// Media Foundation
#include <mfapi.h>
#include <mfmediaengine.h>
#include <mferror.h>

// Polyhook for Present detour
#include <polyhook2/Detour/x64Detour.hpp>

#include <thread>
#include <mutex>
#include <atomic>
#include <string>
#include <filesystem>
#include <sstream>

// ============================================================
// Shader source
// ============================================================
static const char* g_vsSource = R"(
struct VS_IN  { float2 pos : POSITION; float2 uv : TEXCOORD; };
struct VS_OUT { float4 pos : SV_POSITION; float2 uv : TEXCOORD; };
VS_OUT main(VS_IN i) {
  VS_OUT o;
  o.pos = float4(i.pos, 0.0, 1.0);
  o.uv  = i.uv;
  return o;
}
)";

static const char* g_psSource = R"(
Texture2D    tex  : register(t0);
SamplerState samp : register(s0);
float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD) : SV_TARGET {
  return tex.Sample(samp, uv);
}
)";

// ============================================================
// Utility: extract YouTube video ID from URL
// ============================================================
static std::string extractVideoId(const std::string& url) {
  // youtu.be/ID
  auto pos = url.find("youtu.be/");
  if (pos != std::string::npos) {
    auto id = url.substr(pos + 9);
    auto end = id.find_first_of("?&#");
    return end != std::string::npos ? id.substr(0, end) : id;
  }
  // youtube.com/watch?v=ID or youtube.com/...?v=ID
  pos = url.find("v=");
  if (pos != std::string::npos) {
    auto id = url.substr(pos + 2);
    auto end = id.find_first_of("?&#");
    return end != std::string::npos ? id.substr(0, end) : id;
  }
  // youtube.com/shorts/ID
  pos = url.find("/shorts/");
  if (pos != std::string::npos) {
    auto id = url.substr(pos + 8);
    auto end = id.find_first_of("?&#");
    return end != std::string::npos ? id.substr(0, end) : id;
  }
  return {};
}

// ============================================================
// Utility: find yt-dlp executable
// ============================================================
static std::filesystem::path findYtDlp(const std::filesystem::path& mod_dir,
                                        const std::filesystem::path& override_path) {
  if (!override_path.empty() && std::filesystem::exists(override_path))
    return override_path;

  // Check mod folder
  auto local = mod_dir / "yt-dlp.exe";
  if (std::filesystem::exists(local)) return local;

  // Check PATH via where command
  char buf[MAX_PATH];
  if (SearchPathA(nullptr, "yt-dlp.exe", nullptr, MAX_PATH, buf, nullptr))
    return std::filesystem::path(buf);

  return {};
}

// ============================================================
// Vertex struct for textured quad
// ============================================================
struct QuadVertex {
  float x, y;   // NDC position
  float u, v;   // texture coordinates
};

// ============================================================
// IMFMediaEngineNotify callback
// ============================================================
class MediaEngineNotify : public IMFMediaEngineNotify {
public:
  using Handler = std::function<void(DWORD event, DWORD_PTR param1, DWORD param2)>;

  explicit MediaEngineNotify(Handler handler) : handler_(std::move(handler)) {}

  // IUnknown
  STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
    if (!ppv) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFMediaEngineNotify)) {
      *ppv = static_cast<IMFMediaEngineNotify*>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refCount_); }
  STDMETHODIMP_(ULONG) Release() override {
    long c = InterlockedDecrement(&refCount_);
    if (c == 0) delete this;
    return c;
  }

  // IMFMediaEngineNotify
  STDMETHODIMP EventNotify(DWORD event, DWORD_PTR param1, DWORD param2) override {
    if (handler_) handler_(event, param1, param2);
    return S_OK;
  }

private:
  long refCount_ = 1;
  Handler handler_;
};

// ============================================================
// Present hook globals
// ============================================================
using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
static PresentFn g_origPresent = nullptr;

static HRESULT STDMETHODCALLTYPE hookPresent(IDXGISwapChain* sc, UINT syncInterval, UINT flags) {
  VideoPlayer::instance().onPresent(sc);
  return g_origPresent(sc, syncInterval, flags);
}

// ============================================================
// VideoPlayer::Impl
// ============================================================
struct VideoPlayer::Impl {
  // State
  std::atomic<VideoState> state{VideoState::Idle};
  std::atomic<float> dlProgress{0.0f};
  std::wstring statusStr = L"";
  mutable std::mutex statusMtx;

  // Paths
  std::filesystem::path cacheDir;
  std::filesystem::path ytdlpPath;
  std::filesystem::path currentVideoFile;
  std::atomic<bool> pendingPlay{false}; // signal render thread to call playFile()

  // D3D11 resources (created on render thread)
  ID3D11Device* device = nullptr;
  ID3D11DeviceContext* ctx = nullptr;
  ID3D11Texture2D* videoTex = nullptr;
  ID3D11ShaderResourceView* videoSRV = nullptr;
  ID3D11VertexShader* vs = nullptr;
  ID3D11PixelShader* ps = nullptr;
  ID3D11InputLayout* inputLayout = nullptr;
  ID3D11Buffer* quadVB = nullptr;
  ID3D11SamplerState* sampler = nullptr;
  ID3D11BlendState* blendState = nullptr;
  bool d3dReady = false;

  // Media Foundation
  IMFMediaEngine* engine = nullptr;
  IMFDXGIDeviceManager* dxgiMgr = nullptr;
  MediaEngineNotify* notify = nullptr;
  UINT dxgiResetToken = 0;
  bool mfStarted = false;
  bool engineReady = false;

  // Video info
  DWORD vidW = 0, vidH = 0;
  bool texCreated = false;

  // Audio
  bool muted = true;

  // Fullscreen
  bool fullscreen = true;  // videos start fullscreen
  bool escWasDown = false;   // edge detection for ESC key
  bool spaceWasDown = false; // edge detection for spacebar
  bool rightWasDown = false; // edge detection for right arrow (fullscreen toggle)

  // Present hook
  PLH::x64Detour* presentDetour = nullptr;
  bool hookInstalled = false;

  // Threading
  std::thread dlThread;

  // ------- Methods -------

  void setStatus(const std::wstring& s) {
    std::lock_guard<std::mutex> lk(statusMtx);
    statusStr = s;
  }

  std::wstring getStatus() const {
    std::lock_guard<std::mutex> lk(statusMtx);
    return statusStr;
  }

  // Initialize D3D11 rendering resources from the game's swap chain
  void initD3D(IDXGISwapChain* swapchain) {
    if (d3dReady) return;

    HRESULT hr = swapchain->GetDevice(__uuidof(ID3D11Device), (void**)&device);
    if (FAILED(hr) || !device) {
      COMBO_LOG("VideoPlayer: failed to get D3D11 device from swap chain");
      return;
    }
    device->GetImmediateContext(&ctx);

    // Compile shaders
    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* psBlob = nullptr;
    ID3DBlob* errBlob = nullptr;

    hr = D3DCompile(g_vsSource, strlen(g_vsSource), nullptr, nullptr, nullptr,
                    "main", "vs_5_0", 0, 0, &vsBlob, &errBlob);
    if (FAILED(hr)) {
      if (errBlob) {
        COMBO_LOG("VS compile error: " + std::string((char*)errBlob->GetBufferPointer()));
        errBlob->Release();
      }
      return;
    }

    hr = D3DCompile(g_psSource, strlen(g_psSource), nullptr, nullptr, nullptr,
                    "main", "ps_5_0", 0, 0, &psBlob, &errBlob);
    if (FAILED(hr)) {
      if (errBlob) {
        COMBO_LOG("PS compile error: " + std::string((char*)errBlob->GetBufferPointer()));
        errBlob->Release();
      }
      vsBlob->Release();
      return;
    }

    device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vs);
    device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &ps);

    // Input layout
    D3D11_INPUT_ELEMENT_DESC layoutDesc[] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8,  D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    device->CreateInputLayout(layoutDesc, 2, vsBlob->GetBufferPointer(),
                              vsBlob->GetBufferSize(), &inputLayout);
    vsBlob->Release();
    psBlob->Release();

    // Sampler
    D3D11_SAMPLER_DESC sampDesc = {};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    device->CreateSamplerState(&sampDesc, &sampler);

    // Blend state (premultiplied alpha)
    D3D11_BLEND_DESC blendDesc = {};
    blendDesc.RenderTarget[0].BlendEnable = TRUE;
    blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    device->CreateBlendState(&blendDesc, &blendState);

    // Vertex buffer (6 verts for 2 triangles, will be updated per-frame with correct aspect)
    D3D11_BUFFER_DESC vbDesc = {};
    vbDesc.ByteWidth = sizeof(QuadVertex) * 6;
    vbDesc.Usage = D3D11_USAGE_DYNAMIC;
    vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    vbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    device->CreateBuffer(&vbDesc, nullptr, &quadVB);

    d3dReady = true;
    COMBO_LOG("VideoPlayer: D3D11 resources created");
  }

  // Initialize Media Foundation and create the media engine
  void initMF() {
    if (mfStarted) return;
    if (!device) return;

    HRESULT hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
      COMBO_LOG("VideoPlayer: MFStartup failed: " + std::to_string(hr));
      return;
    }
    mfStarted = true;

    // Create DXGI device manager
    hr = MFCreateDXGIDeviceManager(&dxgiResetToken, &dxgiMgr);
    if (FAILED(hr)) {
      COMBO_LOG("VideoPlayer: MFCreateDXGIDeviceManager failed");
      return;
    }

    // Give it our D3D11 device
    // Need to enable multithread protection first
    ID3D10Multithread* mt = nullptr;
    device->QueryInterface(__uuidof(ID3D10Multithread), (void**)&mt);
    if (mt) {
      mt->SetMultithreadProtected(TRUE);
      mt->Release();
    }

    hr = dxgiMgr->ResetDevice(device, dxgiResetToken);
    if (FAILED(hr)) {
      COMBO_LOG("VideoPlayer: DXGI manager ResetDevice failed");
      return;
    }

    // Create notify callback
    notify = new MediaEngineNotify([this](DWORD event, DWORD_PTR p1, DWORD p2) {
      onMediaEvent(event, p1, p2);
    });

    // Create media engine
    IMFMediaEngineClassFactory* factory = nullptr;
    hr = CoCreateInstance(CLSID_MFMediaEngineClassFactory, nullptr,
                          CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
      COMBO_LOG("VideoPlayer: CoCreateInstance MFMediaEngineClassFactory failed: " + std::to_string(hr));
      return;
    }

    IMFAttributes* attrs = nullptr;
    MFCreateAttributes(&attrs, 3);
    attrs->SetUnknown(MF_MEDIA_ENGINE_DXGI_MANAGER, dxgiMgr);
    attrs->SetUnknown(MF_MEDIA_ENGINE_CALLBACK, notify);
    attrs->SetUINT32(MF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT, DXGI_FORMAT_B8G8R8A8_UNORM);

    hr = factory->CreateInstance(0, attrs, &engine);
    attrs->Release();
    factory->Release();

    if (FAILED(hr) || !engine) {
      COMBO_LOG("VideoPlayer: CreateInstance IMFMediaEngine failed: " + std::to_string(hr));
      return;
    }

    engine->SetMuted(TRUE);
    engineReady = true;
    COMBO_LOG("VideoPlayer: Media engine created OK");
  }

  void onMediaEvent(DWORD event, DWORD_PTR param1, DWORD param2) {
    switch (event) {
      case MF_MEDIA_ENGINE_EVENT_LOADEDMETADATA: {
        DWORD w = 0, h = 0;
        if (engine) engine->GetNativeVideoSize(&w, &h);
        vidW = w;
        vidH = h;
        COMBO_LOG("VideoPlayer: metadata loaded " + std::to_string(w) + "x" + std::to_string(h));
        break;
      }
      case MF_MEDIA_ENGINE_EVENT_CANPLAY:
        COMBO_LOG("VideoPlayer: can play");
        if (engine && state.load() == VideoState::Buffering) {
          engine->SetMuted(muted ? TRUE : FALSE);
          engine->Play();
          state.store(VideoState::Playing);
          setStatus(L"Playing");
        }
        break;
      case MF_MEDIA_ENGINE_EVENT_PLAYING:
        COMBO_LOG("VideoPlayer: playing");
        break;
      case MF_MEDIA_ENGINE_EVENT_ENDED:
        COMBO_LOG("VideoPlayer: ended");
        state.store(VideoState::Finished);
        setStatus(L"Video ended");
        break;
      case MF_MEDIA_ENGINE_EVENT_ERROR: {
        auto s = state.load();
        // param1 = MF_MEDIA_ENGINE_ERR code, param2 = HRESULT
        COMBO_LOG("VideoPlayer: error event err=" + std::to_string(param1) +
                  " hr=0x" + ([&]{ std::ostringstream ss; ss << std::hex << param2; return ss.str(); })() +
                  " state=" + std::to_string((int)s));
        // Also query the engine's error object for more detail
        if (engine) {
          IMFMediaError* err = nullptr;
          engine->GetError(&err);
          if (err) {
            USHORT code = err->GetErrorCode();
            HRESULT extCode = err->GetExtendedErrorCode();
            COMBO_LOG("VideoPlayer: MF error code=" + std::to_string(code) +
                      " ext=0x" + ([&]{ std::ostringstream ss; ss << std::hex << extCode; return ss.str(); })());
            err->Release();
          }
        }
        // Only set error if we were actually trying to play
        if (s == VideoState::Buffering || s == VideoState::Playing || s == VideoState::Paused) {
          state.store(VideoState::Error);
          setStatus(L"Playback error");
        }
        break;
      }
      default:
        break;
    }
  }

  // Create the video texture (called once we know dimensions)
  void createVideoTexture() {
    if (texCreated || vidW == 0 || vidH == 0 || !device) return;

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = vidW;
    desc.Height = vidH;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = device->CreateTexture2D(&desc, nullptr, &videoTex);
    if (FAILED(hr)) {
      COMBO_LOG("VideoPlayer: CreateTexture2D failed");
      return;
    }

    hr = device->CreateShaderResourceView(videoTex, nullptr, &videoSRV);
    if (FAILED(hr)) {
      COMBO_LOG("VideoPlayer: CreateSRV failed");
      return;
    }

    texCreated = true;
    COMBO_LOG("VideoPlayer: video texture created " + std::to_string(vidW) + "x" + std::to_string(vidH));
  }

  // Update the vertex buffer for the video quad position
  void updateQuad(IDXGISwapChain* swapchain) {
    if (!quadVB || !ctx) return;

    // Get back buffer dimensions
    ID3D11Texture2D* backBuf = nullptr;
    swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuf);
    if (!backBuf) return;
    D3D11_TEXTURE2D_DESC bbDesc;
    backBuf->GetDesc(&bbDesc);
    backBuf->Release();

    float screenW = (float)bbDesc.Width;
    float screenH = (float)bbDesc.Height;
    float vidAspect = (vidW > 0 && vidH > 0) ? (float)vidW / (float)vidH : 16.0f / 9.0f;

    float left, right, top, bottom;

    if (fullscreen) {
      // Fullscreen with letterboxing to maintain aspect ratio
      float screenAspect = screenW / screenH;
      if (vidAspect > screenAspect) {
        // Video is wider — letterbox top/bottom
        float h = 1.0f / vidAspect * screenAspect;
        left = 0.0f;
        right = 1.0f;
        top = (1.0f - h) * 0.5f;
        bottom = top + h;
      } else {
        // Video is taller — pillarbox left/right
        float w = vidAspect / screenAspect;
        top = 0.0f;
        bottom = 1.0f;
        left = (1.0f - w) * 0.5f;
        right = left + w;
      }
    } else {
      // Small overlay: right side, 28% width
      float vidFracW = 0.28f;
      float margin = 0.02f;
      left = 1.0f - vidFracW - margin;
      right = 1.0f - margin;
      top = margin;
      float pixW = vidFracW * screenW;
      float pixH = pixW / vidAspect;
      bottom = top + pixH / screenH;
      if (bottom > 1.0f - margin) bottom = 1.0f - margin;
    }

    // Convert to NDC: x_ndc = x_frac * 2 - 1, y_ndc = -(y_frac * 2 - 1)
    float ndcL = left * 2.0f - 1.0f;
    float ndcR = right * 2.0f - 1.0f;
    float ndcT = -(top * 2.0f - 1.0f);
    float ndcB = -(bottom * 2.0f - 1.0f);

    QuadVertex verts[6] = {
      {ndcL, ndcT, 0.0f, 0.0f},  // top-left
      {ndcR, ndcT, 1.0f, 0.0f},  // top-right
      {ndcL, ndcB, 0.0f, 1.0f},  // bottom-left
      {ndcR, ndcT, 1.0f, 0.0f},  // top-right
      {ndcR, ndcB, 1.0f, 1.0f},  // bottom-right
      {ndcL, ndcB, 0.0f, 1.0f},  // bottom-left
    };

    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(ctx->Map(quadVB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
      memcpy(mapped.pData, verts, sizeof(verts));
      ctx->Unmap(quadVB, 0);
    }
  }

  // Render the video frame as a textured quad
  void renderVideoFrame(IDXGISwapChain* swapchain) {
    if (!d3dReady || !texCreated || !engine || !videoSRV) return;

    VideoState s = state.load();
    if (s != VideoState::Playing && s != VideoState::Paused) return;

    // Transfer current video frame to our texture
    if (vidW > 0 && vidH > 0) {
      LONGLONG pts;
      if (engine->OnVideoStreamTick(&pts) == S_OK) {
        MFVideoNormalizedRect srcRect = {0.0f, 0.0f, 1.0f, 1.0f};
        RECT dstRect = {0, 0, (LONG)vidW, (LONG)vidH};
        MFARGB borderColor = {0, 0, 0, 255};
        HRESULT hr = engine->TransferVideoFrame(videoTex, &srcRect, &dstRect, &borderColor);
        if (FAILED(hr)) return; // frame not ready yet, skip
      }
    }

    // Save current D3D11 state so we don't break the game's rendering
    ID3D11RenderTargetView* oldRTV = nullptr;
    ID3D11DepthStencilView* oldDSV = nullptr;
    ctx->OMGetRenderTargets(1, &oldRTV, &oldDSV);

    D3D11_VIEWPORT oldVP;
    UINT numVP = 1;
    ctx->RSGetViewports(&numVP, &oldVP);

    ID3D11BlendState* oldBlend = nullptr;
    float oldBlendFactor[4];
    UINT oldSampleMask;
    ctx->OMGetBlendState(&oldBlend, oldBlendFactor, &oldSampleMask);

    // Set render target to the back buffer
    ID3D11Texture2D* backBuf = nullptr;
    swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuf);
    if (!backBuf) return;

    ID3D11RenderTargetView* rtv = nullptr;
    device->CreateRenderTargetView(backBuf, nullptr, &rtv);
    D3D11_TEXTURE2D_DESC bbDesc;
    backBuf->GetDesc(&bbDesc);
    backBuf->Release();

    if (!rtv) return;

    ctx->OMSetRenderTargets(1, &rtv, nullptr);

    // Set viewport to full screen
    D3D11_VIEWPORT vp = {};
    vp.Width = (float)bbDesc.Width;
    vp.Height = (float)bbDesc.Height;
    vp.MaxDepth = 1.0f;
    ctx->RSSetViewports(1, &vp);

    // In fullscreen mode, clear to black for letterboxing
    if (fullscreen) {
      float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
      ctx->ClearRenderTargetView(rtv, black);
    }

    // Update quad vertices
    updateQuad(swapchain);

    // Set pipeline state
    ctx->OMSetBlendState(blendState, nullptr, 0xFFFFFFFF);
    ctx->IASetInputLayout(inputLayout);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    UINT stride = sizeof(QuadVertex);
    UINT offset = 0;
    ctx->IASetVertexBuffers(0, 1, &quadVB, &stride, &offset);
    ctx->VSSetShader(vs, nullptr, 0);
    ctx->PSSetShader(ps, nullptr, 0);
    ctx->PSSetShaderResources(0, 1, &videoSRV);
    ctx->PSSetSamplers(0, 1, &sampler);

    // Draw
    ctx->Draw(6, 0);

    // Unbind SRV to avoid hazards
    ID3D11ShaderResourceView* nullSRV = nullptr;
    ctx->PSSetShaderResources(0, 1, &nullSRV);

    // Restore state
    ctx->OMSetRenderTargets(1, &oldRTV, oldDSV);
    ctx->RSSetViewports(1, &oldVP);
    ctx->OMSetBlendState(oldBlend, oldBlendFactor, oldSampleMask);

    if (rtv) rtv->Release();
    if (oldRTV) oldRTV->Release();
    if (oldDSV) oldDSV->Release();
    if (oldBlend) oldBlend->Release();
  }

  // yt-dlp download on background thread
  void downloadVideo(const std::string& url) {
    auto videoId = extractVideoId(url);
    if (videoId.empty()) {
      state.store(VideoState::Error);
      setStatus(L"Invalid YouTube URL");
      COMBO_LOG("VideoPlayer: invalid URL: " + url);
      return;
    }

    // Check cache
    auto cachedFile = cacheDir / (videoId + ".mp4");
    if (std::filesystem::exists(cachedFile)) {
      COMBO_LOG("VideoPlayer: using cached file: " + cachedFile.string());
      currentVideoFile = cachedFile;
      state.store(VideoState::Buffering);
      setStatus(L"Opening cached video...");
      pendingPlay.store(true);
      return;
    }

    // Find yt-dlp
    auto ytdlp = findYtDlp(cacheDir.parent_path(), ytdlpPath);
    if (ytdlp.empty()) {
      state.store(VideoState::Error);
      setStatus(L"yt-dlp not found");
      COMBO_LOG("VideoPlayer: yt-dlp not found");
      return;
    }

    COMBO_LOG("VideoPlayer: downloading with " + ytdlp.string());
    state.store(VideoState::Downloading);
    dlProgress.store(0.0f);
    setStatus(L"Downloading...");

    // Create cache directory
    std::filesystem::create_directories(cacheDir);

    // Build output template - yt-dlp will determine the extension
    auto outputTmpl = (cacheDir / (videoId + ".%(ext)s")).string();

    // Build command line
    std::string cmdLine = "\"" + ytdlp.string() + "\" "
      "--no-playlist "
      "-f \"bv*[height<=720]+ba/b[height<=720]/b\" "
      "--merge-output-format mp4 "
      "--ppa \"ffmpeg:-c:v copy -c:a aac -b:a 128k\" "
      "-o \"" + outputTmpl + "\" "
      "--newline --progress "
      "\"" + url + "\"";

    COMBO_LOG("VideoPlayer: cmd: " + cmdLine);

    // Create pipes for stdout
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE hReadPipe, hWritePipe;
    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) {
      state.store(VideoState::Error);
      setStatus(L"Failed to create pipe");
      return;
    }
    SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = hWritePipe;
    si.hStdError = hWritePipe;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi = {};
    BOOL ok = CreateProcessA(nullptr, const_cast<char*>(cmdLine.c_str()),
                             nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                             nullptr, nullptr, &si, &pi);
    CloseHandle(hWritePipe);

    if (!ok) {
      CloseHandle(hReadPipe);
      state.store(VideoState::Error);
      setStatus(L"Failed to start yt-dlp");
      COMBO_LOG("VideoPlayer: CreateProcess failed");
      return;
    }

    // Read output and parse progress
    char buf[4096];
    DWORD bytesRead;
    std::string lineBuf;

    while (ReadFile(hReadPipe, buf, sizeof(buf) - 1, &bytesRead, nullptr) && bytesRead > 0) {
      if (state.load() == VideoState::Idle) {
        // User cancelled
        TerminateProcess(pi.hProcess, 1);
        break;
      }

      buf[bytesRead] = '\0';
      lineBuf += buf;

      // Process complete lines
      size_t pos;
      while ((pos = lineBuf.find('\n')) != std::string::npos) {
        std::string line = lineBuf.substr(0, pos);
        lineBuf.erase(0, pos + 1);

        // Log errors/warnings from yt-dlp
        if (line.find("ERROR") != std::string::npos ||
            line.find("WARNING") != std::string::npos) {
          COMBO_LOG("yt-dlp: " + line);
        }

        // Parse "[download]  XX.X% of ..."
        auto dlPos = line.find("[download]");
        if (dlPos != std::string::npos) {
          auto pctPos = line.find('%');
          if (pctPos != std::string::npos) {
            // Find the number before %
            auto numStart = pctPos;
            while (numStart > dlPos && (line[numStart - 1] == '.' || isdigit(line[numStart - 1])))
              numStart--;
            try {
              float pct = std::stof(line.substr(numStart, pctPos - numStart));
              dlProgress.store(pct);
              setStatus(L"Downloading " + std::to_wstring((int)pct) + L"%");
            } catch (...) {}
          }
        }
      }
    }

    CloseHandle(hReadPipe);
    WaitForSingleObject(pi.hProcess, 10000);

    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (exitCode != 0 || state.load() == VideoState::Idle) {
      if (state.load() != VideoState::Idle) {
        state.store(VideoState::Error);
        setStatus(L"Download failed");
        COMBO_LOG("VideoPlayer: yt-dlp exit code " + std::to_string(exitCode));
      }
      return;
    }

    // Find the downloaded file (yt-dlp may have chosen a different extension)
    currentVideoFile.clear();
    if (std::filesystem::exists(cachedFile)) {
      currentVideoFile = cachedFile;
    } else {
      // Search for any file with the video ID
      for (auto& entry : std::filesystem::directory_iterator(cacheDir)) {
        if (entry.path().stem().string() == videoId) {
          currentVideoFile = entry.path();
          break;
        }
      }
    }

    if (currentVideoFile.empty() || !std::filesystem::exists(currentVideoFile)) {
      state.store(VideoState::Error);
      setStatus(L"Downloaded file not found");
      COMBO_LOG("VideoPlayer: file not found after download");
      return;
    }

    COMBO_LOG("VideoPlayer: downloaded to " + currentVideoFile.string());
    dlProgress.store(100.0f);
    state.store(VideoState::Buffering);
    setStatus(L"Opening video...");
    pendingPlay.store(true);
  }

  // Start playback of the local file via Media Foundation
  void playFile() {
    if (!engineReady) {
      // MF not ready yet, will retry when D3D init completes
      COMBO_LOG("VideoPlayer: engine not ready, deferring playback");
      return;
    }

    auto wpath = currentVideoFile.wstring();
    // Convert to file:// URL with proper encoding
    std::wstring fileUrl = L"file:///";
    for (auto& c : wpath) {
      if (c == L'\\') fileUrl += L'/';
      else if (c == L' ') fileUrl += L"%20";
      else if (c == L'(') fileUrl += L"%28";
      else if (c == L')') fileUrl += L"%29";
      else fileUrl += c;
    }

    // Log the URL for debugging
    std::string urlNarrow(fileUrl.begin(), fileUrl.end());
    COMBO_LOG("VideoPlayer: SetSource URL=" + urlNarrow);

    BSTR bstr = SysAllocString(fileUrl.c_str());
    HRESULT hr = engine->SetSource(bstr);
    SysFreeString(bstr);

    if (FAILED(hr)) {
      COMBO_LOG("VideoPlayer: SetSource failed hr=0x" +
        ([&]{ std::ostringstream ss; ss << std::hex << (unsigned)hr; return ss.str(); })());
      state.store(VideoState::Error);
      setStatus(L"Failed to open video");
      return;
    }

    COMBO_LOG("VideoPlayer: source set OK");
  }

  void stopPlayback() {
    state.store(VideoState::Idle);
    setStatus(L"");
    dlProgress.store(0.0f);
    if (engine) {
      engine->Pause();
    }

    // Release texture
    if (videoSRV) { videoSRV->Release(); videoSRV = nullptr; }
    if (videoTex) { videoTex->Release(); videoTex = nullptr; }
    texCreated = false;
    vidW = vidH = 0;
  }

  void cleanup() {
    stopPlayback();

    if (engine) { engine->Shutdown(); engine->Release(); engine = nullptr; }
    if (notify) { notify->Release(); notify = nullptr; }
    if (dxgiMgr) { dxgiMgr->Release(); dxgiMgr = nullptr; }

    if (blendState) { blendState->Release(); blendState = nullptr; }
    if (sampler) { sampler->Release(); sampler = nullptr; }
    if (quadVB) { quadVB->Release(); quadVB = nullptr; }
    if (inputLayout) { inputLayout->Release(); inputLayout = nullptr; }
    if (ps) { ps->Release(); ps = nullptr; }
    if (vs) { vs->Release(); vs = nullptr; }
    if (ctx) { ctx->Release(); ctx = nullptr; }
    // Don't release device - it's the game's device

    if (mfStarted) {
      MFShutdown();
      mfStarted = false;
    }

    d3dReady = false;
    engineReady = false;
  }
};

// ============================================================
// VideoPlayer public interface
// ============================================================

VideoPlayer::VideoPlayer() : impl_(new Impl()) {}
VideoPlayer::~VideoPlayer() { delete impl_; }

VideoPlayer& VideoPlayer::instance() {
  static VideoPlayer inst;
  return inst;
}

void VideoPlayer::setCacheDir(const std::filesystem::path& dir) {
  impl_->cacheDir = dir / "combo_cache";
}

void VideoPlayer::setYtDlpPath(const std::filesystem::path& path) {
  impl_->ytdlpPath = path;
}

void VideoPlayer::initPresentHook() {
  if (impl_->hookInstalled) return;

  COMBO_LOG("VideoPlayer: setting up Present hook...");

  // Find the game's window
  HWND hwnd = FindWindowA("UnrealWindow", nullptr);
  if (!hwnd) hwnd = GetForegroundWindow();
  if (!hwnd) {
    COMBO_LOG("VideoPlayer: no window found for Present hook");
    return;
  }

  // Create temporary D3D11 device + swap chain to find Present vtable address
  DXGI_SWAP_CHAIN_DESC sd = {};
  sd.BufferCount = 1;
  sd.BufferDesc.Width = 2;
  sd.BufferDesc.Height = 2;
  sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.OutputWindow = hwnd;
  sd.SampleDesc.Count = 1;
  sd.Windowed = TRUE;
  sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

  IDXGISwapChain* tmpSC = nullptr;
  ID3D11Device* tmpDev = nullptr;
  ID3D11DeviceContext* tmpCtx = nullptr;
  D3D_FEATURE_LEVEL fl;

  HRESULT hr = D3D11CreateDeviceAndSwapChain(
    nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
    nullptr, 0, D3D11_SDK_VERSION,
    &sd, &tmpSC, &tmpDev, &fl, &tmpCtx);

  if (FAILED(hr) || !tmpSC) {
    COMBO_LOG("VideoPlayer: D3D11CreateDeviceAndSwapChain failed: " + std::to_string(hr));
    if (tmpCtx) tmpCtx->Release();
    if (tmpDev) tmpDev->Release();
    if (tmpSC) tmpSC->Release();
    return;
  }

  // Read Present address from vtable (index 8)
  void** vtable = *reinterpret_cast<void***>(tmpSC);
  uint64_t presentAddr = reinterpret_cast<uint64_t>(vtable[8]);

  tmpSC->Release();
  tmpDev->Release();
  tmpCtx->Release();

  COMBO_LOG("VideoPlayer: Present at 0x" + ([&]{
    std::ostringstream ss;
    ss << std::hex << presentAddr;
    return ss.str();
  })());

  // Hook with Polyhook
  impl_->presentDetour = new PLH::x64Detour(
    presentAddr,
    reinterpret_cast<uint64_t>(&hookPresent),
    reinterpret_cast<uint64_t*>(&g_origPresent));

  if (impl_->presentDetour->hook()) {
    impl_->hookInstalled = true;
    COMBO_LOG("VideoPlayer: Present hook installed");
  } else {
    COMBO_LOG("VideoPlayer: Present hook FAILED");
    delete impl_->presentDetour;
    impl_->presentDetour = nullptr;
  }
}

void VideoPlayer::setFullscreen(bool fs) {
  impl_->fullscreen = fs;
}

bool VideoPlayer::isFullscreen() const {
  return impl_->fullscreen;
}

void VideoPlayer::startVideo(const std::string& youtube_url) {
  // Stop any current playback
  stop();

  // Videos start fullscreen
  impl_->fullscreen = true;

  COMBO_LOG("VideoPlayer: startVideo " + youtube_url);

  // Launch download on background thread
  if (impl_->dlThread.joinable()) impl_->dlThread.join();
  impl_->dlThread = std::thread([this, youtube_url]() {
    impl_->downloadVideo(youtube_url);
  });
}

void VideoPlayer::stop() {
  impl_->stopPlayback();
  if (impl_->dlThread.joinable()) {
    // Signal download to stop
    impl_->state.store(VideoState::Idle);
    impl_->dlThread.join();
  }
}

void VideoPlayer::togglePause() {
  VideoState s = impl_->state.load();
  if (s == VideoState::Playing && impl_->engine) {
    impl_->engine->Pause();
    impl_->state.store(VideoState::Paused);
    impl_->setStatus(L"Paused");
  } else if (s == VideoState::Paused && impl_->engine) {
    impl_->engine->Play();
    impl_->state.store(VideoState::Playing);
    impl_->setStatus(L"Playing");
  }
}

void VideoPlayer::toggleMute() {
  impl_->muted = !impl_->muted;
  if (impl_->engine) {
    impl_->engine->SetMuted(impl_->muted ? TRUE : FALSE);
  }
  COMBO_LOG(std::string("VideoPlayer: muted=") + (impl_->muted ? "true" : "false"));
}

VideoState VideoPlayer::state() const {
  return impl_->state.load();
}

bool VideoPlayer::isActive() const {
  auto s = impl_->state.load();
  return s != VideoState::Idle && s != VideoState::Error && s != VideoState::Finished;
}

bool VideoPlayer::isMuted() const {
  return impl_->muted;
}

float VideoPlayer::downloadProgress() const {
  return impl_->dlProgress.load();
}

std::wstring VideoPlayer::statusText() const {
  return impl_->getStatus();
}

void VideoPlayer::onPresent(IDXGISwapChain* swapchain) {
  // Lazy-init D3D resources on first Present call
  if (!impl_->d3dReady) {
    impl_->initD3D(swapchain);
    if (impl_->d3dReady) {
      impl_->initMF();
    }
  }

  // Key input for video controls (edge-triggered)
  auto s = impl_->state.load();
  bool videoUp = (s == VideoState::Playing || s == VideoState::Paused || s == VideoState::Finished);

  // ESC — stop and exit video
  bool escDown = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
  if (escDown && !impl_->escWasDown && videoUp) {
    stop();
    COMBO_LOG("VideoPlayer: ESC — stopped video");
  }
  impl_->escWasDown = escDown;

  // Spacebar — pause / resume
  bool spaceDown = (GetAsyncKeyState(VK_SPACE) & 0x8000) != 0;
  if (spaceDown && !impl_->spaceWasDown && videoUp) {
    if (s == VideoState::Finished) {
      // Replay from the start
      if (impl_->engine) {
        impl_->engine->SetCurrentTime(0.0);
        impl_->engine->Play();
        impl_->state.store(VideoState::Playing);
        impl_->setStatus(L"Playing");
      }
    } else {
      togglePause();
    }
    COMBO_LOG("VideoPlayer: Space — toggle pause");
  }
  impl_->spaceWasDown = spaceDown;

  // Right arrow — toggle fullscreen / windowed
  bool rightDown = (GetAsyncKeyState(VK_RIGHT) & 0x8000) != 0;
  if (rightDown && !impl_->rightWasDown && videoUp) {
    impl_->fullscreen = !impl_->fullscreen;
    COMBO_LOG(std::string("VideoPlayer: Right — fullscreen=") + (impl_->fullscreen ? "true" : "false"));
  }
  impl_->rightWasDown = rightDown;

  // Check if download thread wants us to start playback (must happen on render thread)
  if (impl_->pendingPlay.exchange(false)) {
    if (impl_->engineReady && !impl_->currentVideoFile.empty()) {
      impl_->playFile();
    }
  }

  // Create video texture when dimensions become known
  if (!impl_->texCreated && impl_->vidW > 0 && impl_->vidH > 0) {
    impl_->createVideoTexture();
  }

  // Render video frame
  impl_->renderVideoFrame(swapchain);
}

void VideoPlayer::shutdown() {
  stop();
  impl_->cleanup();
  if (impl_->presentDetour) {
    impl_->presentDetour->unHook();
    delete impl_->presentDetour;
    impl_->presentDetour = nullptr;
    impl_->hookInstalled = false;
  }
}
