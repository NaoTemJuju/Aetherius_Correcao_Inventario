#include "Menus/NativeSurfaceMenu.h"

#include "Render/CheckerboardTexture.h"

#include <chrono>

namespace Meridian::Menus
{
    namespace
    {
        constexpr std::uint32_t TEXTURE_SIZE = 64;
        constexpr std::uint32_t TILE_SIZE = 8;
        constexpr Meridian::Render::RGBA8 DARK_TILE{30, 34, 43, 255};
        constexpr Meridian::Render::RGBA8 LIGHT_TILE{88, 101, 119, 255};
        // Aetherius: janela de graca (ms) para absorver ciclos transitorios de
        // previewHide -> previewShow disparados quando o inventario re-renderiza
        // o DOM a cada snapshot periodico (~5s), eliminando pisca-pisca.
        // ClearNif() (fechar inventario) cancela essa janela imediatamente (0ms).
        constexpr std::int64_t HIDE_DEBOUNCE_MS = 250;
        // Aetherius: sensibilidade e margem inferior do trackball 360 graus direto no
        // preview nativo (evita a faixa dos botoes inferiores de rotacao/zoom).
        constexpr float TRACKBALL_SENSITIVITY_RAD_PER_PX = 0.0085f;
        constexpr int PREVIEW_CONTROLS_BOTTOM_MARGIN_PX = 44;

        std::int64_t SteadyNowMs()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                .count();
        }
    }

    NativeSurfaceMenu::NativeSurfaceMenu(int a_x,
                                         int a_y,
                                         int a_width,
                                         int a_height,
                                         int a_zOrder,
                                         bool a_initiallyVisible)
        : m_geometryHolder(std::make_shared<LayerGeometryHolder>())
    {
        m_geometryHolder->SetRect(a_x, a_y, a_width, a_height);
        m_geometryHolder->SetZOrder(a_zOrder);
        SetVisible(a_initiallyVisible);
    }

    NativeSurfaceMenu::~NativeSurfaceMenu()
    {
        BeginShutdown();
        std::lock_guard lock(m_resourceMutex);
        m_shaderResourceView.Reset();
        m_checkerboardView.Reset();
        m_texture.Reset();
    }

    void NativeSurfaceMenu::Init(Meridian::Render::RenderData* a_renderData)
    {
        IRenderLayer::Init(a_renderData);
        m_geometryHolder->SetFromResolution(static_cast<int>(a_renderData->width),
                                            static_cast<int>(a_renderData->height));

        const auto pixels = Meridian::Render::BuildCheckerboardRGBA8(
            TEXTURE_SIZE, TEXTURE_SIZE, TILE_SIZE, DARK_TILE, LIGHT_TILE);
        if (pixels.empty() || a_renderData->device == nullptr)
        {
            spdlog::error("{}: cannot create checkerboard texture", NameOf(NativeSurfaceMenu));
            return;
        }

        D3D11_TEXTURE2D_DESC textureDesc{};
        textureDesc.Width = TEXTURE_SIZE;
        textureDesc.Height = TEXTURE_SIZE;
        textureDesc.MipLevels = 1;
        textureDesc.ArraySize = 1;
        textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        textureDesc.SampleDesc.Count = 1;
        textureDesc.Usage = D3D11_USAGE_IMMUTABLE;
        textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA initialData{};
        initialData.pSysMem = pixels.data();
        initialData.SysMemPitch = TEXTURE_SIZE * 4;

        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        auto result = a_renderData->device->CreateTexture2D(&textureDesc, &initialData, texture.GetAddressOf());
        if (FAILED(result))
        {
            spdlog::error("{}: CreateTexture2D failed (0x{:08X})",
                          NameOf(NativeSurfaceMenu), static_cast<std::uint32_t>(result));
            return;
        }

        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> shaderResourceView;
        result = a_renderData->device->CreateShaderResourceView(
            texture.Get(), nullptr, shaderResourceView.GetAddressOf());
        if (FAILED(result))
        {
            spdlog::error("{}: CreateShaderResourceView failed (0x{:08X})",
                          NameOf(NativeSurfaceMenu), static_cast<std::uint32_t>(result));
            return;
        }

        {
            std::lock_guard lock(m_resourceMutex);
            m_texture = std::move(texture);
            m_checkerboardView = shaderResourceView;
            m_shaderResourceView = std::move(shaderResourceView);
        }
        m_ready.store(true, std::memory_order_release);
    }

    void NativeSurfaceMenu::Prepare()
    {
        if (!m_ready.load(std::memory_order_acquire) || m_renderData == nullptr)
        {
            return;
        }

        const auto geometry = m_geometryHolder->Get();
        m_nifPreviewRenderer.Prepare(
            *m_renderData,
            static_cast<std::uint32_t>(std::max(1, geometry.width)),
            static_cast<std::uint32_t>(std::max(1, geometry.height)));

        // The checkerboard is a diagnostic for an empty native surface, not
        // inventory content. Pending/failed models leave the game visible.
        const auto status = m_nifPreviewRenderer.GetStatus();
        if (status == Meridian::UI::NifView::Status::Loading)
        {
            // Aetherius: preserva o ultimo frame valido enquanto um reload/troca esta em
            // andamento, evitando que o item pisque durante sincronizacoes periodicas.
            return;
        }
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
        if (status == Meridian::UI::NifView::Status::Empty)
            view = m_checkerboardView;
        if (status == Meridian::UI::NifView::Status::Ready)
        {
            const auto modelView = m_nifPreviewRenderer.GetSurfaceView();
            if (modelView != nullptr)
            {
                view = modelView;
            }
        }
        std::lock_guard lock(m_resourceMutex);
        m_shaderResourceView = std::move(view);
    }

    void NativeSurfaceMenu::SetVisible(bool a_visible)
    {
        const bool wasVisible = m_isVisible.load(std::memory_order_acquire);
        if (a_visible)
        {
            m_hiddenSinceMs.store(0, std::memory_order_release);
        }
        else if (wasVisible &&
                 m_nifPreviewRenderer.GetStatus() == Meridian::UI::NifView::Status::Ready)
        {
            m_hiddenSinceMs.store(SteadyNowMs(), std::memory_order_release);
        }
        IRenderLayer::SetVisible(a_visible);
    }

    void NativeSurfaceMenu::Draw()
    {
        if (!m_ready.load(std::memory_order_acquire) || m_renderData == nullptr)
        {
            return;
        }
        bool shouldDraw = GetVisible();
        if (!shouldDraw)
        {
            const auto hiddenSince = m_hiddenSinceMs.load(std::memory_order_acquire);
            if (hiddenSince > 0 &&
                m_nifPreviewRenderer.GetStatus() == Meridian::UI::NifView::Status::Ready &&
                (SteadyNowMs() - hiddenSince) < HIDE_DEBOUNCE_MS)
            {
                shouldDraw = true;
            }
        }
        if (!shouldDraw)
        {
            return;
        }

        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> shaderResourceView;
        {
            std::lock_guard lock(m_resourceMutex);
            shaderResourceView = m_shaderResourceView;
        }
        if (shaderResourceView == nullptr)
        {
            return;
        }

        const auto geometry = m_geometryHolder->Get();
        const RECT destination{
            geometry.x,
            geometry.y,
            geometry.x + geometry.width,
            geometry.y + geometry.height,
        };
        m_renderData->spriteBatch->Draw(
            shaderResourceView.Get(), destination, ::DirectX::Colors::White);
    }

    void NativeSurfaceMenu::BeginShutdown()
    {
        m_hiddenSinceMs.store(0, std::memory_order_release);
        m_pointerPressed.store(false, std::memory_order_release);
        if (m_dragActivated.exchange(false, std::memory_order_acq_rel))
        {
            m_nifPreviewRenderer.SetPointerDragActive(false);
        }
        IRenderLayer::BeginShutdown();
        m_ready.store(false, std::memory_order_release);
        m_nifPreviewRenderer.BeginShutdown();
        // Resource destruction is intentionally deferred to the destructor.
        // RenderHost snapshots hold this menu alive through SpriteBatch::End,
        // so the SRV cannot disappear while a deferred draw still references it.
    }

    bool NativeSurfaceMenu::CanProcess(RE::InputEvent*)
    {
        return false;
    }

    SubMenuType NativeSurfaceMenu::GetMenuType()
    {
        return SubMenuType::NativeSurface;
    }

    std::shared_ptr<LayerGeometryHolder> NativeSurfaceMenu::GetGeometryHolder() const
    {
        return m_geometryHolder;
    }

    void NativeSurfaceMenu::OnResolutionChanged(int a_oldW, int a_oldH, int a_newW, int a_newH)
    {
        m_geometryHolder->Rescale(a_oldW, a_oldH, a_newW, a_newH);
    }

    bool NativeSurfaceMenu::IsReady() const
    {
        return m_ready.load(std::memory_order_acquire);
    }

    void NativeSurfaceMenu::BeginNifLoad(std::uint64_t a_generation)
    {
        m_pointerPressed.store(false, std::memory_order_release);
        if (m_dragActivated.exchange(false, std::memory_order_acq_rel))
        {
            m_nifPreviewRenderer.SetPointerDragActive(false);
        }
        m_nifPreviewRenderer.BeginLoad(a_generation);
    }

    void NativeSurfaceMenu::SubmitNifScene(std::uint64_t a_generation,
                                           RE::NiPointer<RE::NiNode> a_scene,
                                           bool a_frameOnLoad)
    {
        m_nifPreviewRenderer.SubmitScene(a_generation, std::move(a_scene), a_frameOnLoad);
    }

    void NativeSurfaceMenu::SubmitNifSceneBatch(
        std::uint64_t a_generation,
        std::vector<Meridian::Render::NifPreview::SceneSubmissionObject> a_scenes,
        bool a_frameOnLoad)
    {
        m_nifPreviewRenderer.SubmitSceneBatch(
            a_generation, std::move(a_scenes), a_frameOnLoad);
    }

    void NativeSurfaceMenu::FailNifLoad(std::uint64_t a_generation,
                                        Meridian::UI::NifView::Status a_status)
    {
        m_nifPreviewRenderer.FailLoad(a_generation, a_status);
    }

    void NativeSurfaceMenu::ClearNif(std::uint64_t a_generation)
    {
        m_hiddenSinceMs.store(0, std::memory_order_release);
        m_pointerPressed.store(false, std::memory_order_release);
        if (m_dragActivated.exchange(false, std::memory_order_acq_rel))
        {
            m_nifPreviewRenderer.SetPointerDragActive(false);
        }
        m_nifPreviewRenderer.Clear(a_generation);
    }

    Meridian::UI::NifView::Status NativeSurfaceMenu::GetNifStatus() const
    {
        return m_nifPreviewRenderer.GetStatus();
    }

    bool NativeSurfaceMenu::SetNifCamera(const Meridian::UI::NifView::CameraState& a_camera)
    {
        return m_nifPreviewRenderer.SetCamera(a_camera);
    }

    bool NativeSurfaceMenu::SetNifObjectVisible(
        Meridian::UI::NifScene::ObjectHandle a_object,
        bool a_visible)
    {
        return m_nifPreviewRenderer.SetObjectVisible(a_object, a_visible);
    }

    bool NativeSurfaceMenu::FrameNif()
    {
        return m_nifPreviewRenderer.FrameModel();
    }

    void NativeSurfaceMenu::ObserveMouseButton(RE::ButtonEvent* a_button)
    {
        if (a_button == nullptr ||
            a_button->GetDevice() != RE::INPUT_DEVICE::kMouse ||
            a_button->GetIDCode() != RE::BSWin32MouseDevice::Key::kLeftButton)
        {
            return;
        }

        if (a_button->IsDown())
        {
            if (!m_ready.load(std::memory_order_acquire) ||
                !GetVisible() ||
                m_nifPreviewRenderer.GetStatus() != Meridian::UI::NifView::Status::Ready)
            {
                return;
            }
            const auto menuCursor = RE::MenuCursor::GetSingleton();
            if (menuCursor == nullptr)
            {
                return;
            }
            const auto geometry = m_geometryHolder->Get();
            const int bottomMargin = std::max(PREVIEW_CONTROLS_BOTTOM_MARGIN_PX, geometry.height / 10);
            const int usableHeight = std::max(0, geometry.height - bottomMargin);
            const int cursorX = static_cast<int>(menuCursor->cursorPosX);
            const int cursorY = static_cast<int>(menuCursor->cursorPosY);
            if (cursorX >= geometry.x && cursorX < geometry.x + geometry.width &&
                cursorY >= geometry.y && cursorY < geometry.y + usableHeight)
            {
                m_pointerPressed.store(true, std::memory_order_release);
                m_dragActivated.store(false, std::memory_order_release);
            }
        }
        else if (a_button->IsUp())
        {
            m_pointerPressed.store(false, std::memory_order_release);
            if (m_dragActivated.exchange(false, std::memory_order_acq_rel))
            {
                m_nifPreviewRenderer.SetPointerDragActive(false);
            }
        }
    }

    void NativeSurfaceMenu::ObserveMouseMove(RE::MouseMoveEvent* a_event)
    {
        if (a_event == nullptr || !m_pointerPressed.load(std::memory_order_acquire))
        {
            return;
        }
        if (!m_ready.load(std::memory_order_acquire) ||
            m_nifPreviewRenderer.GetStatus() != Meridian::UI::NifView::Status::Ready)
        {
            m_pointerPressed.store(false, std::memory_order_release);
            if (m_dragActivated.exchange(false, std::memory_order_acq_rel))
            {
                m_nifPreviewRenderer.SetPointerDragActive(false);
            }
            return;
        }
        const float dx = static_cast<float>(a_event->mouseInputX);
        const float dy = static_cast<float>(a_event->mouseInputY);
        if (dx == 0.0f && dy == 0.0f)
        {
            return;
        }
        if (!m_dragActivated.exchange(true, std::memory_order_acq_rel))
        {
            m_nifPreviewRenderer.SetPointerDragActive(true);
        }
        m_nifPreviewRenderer.RotateByScreenDelta(
            dx * TRACKBALL_SENSITIVITY_RAD_PER_PX,
            dy * TRACKBALL_SENSITIVITY_RAD_PER_PX);
    }
}
