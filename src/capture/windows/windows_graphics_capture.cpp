#include "capture/windows/windows_graphics_capture.h"

#include <d3d11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <windows.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/base.h>

#include <chrono>
#include <cstring>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace cloud_stream::capture::windows {
namespace {

using winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame;
using winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool;
using winrt::Windows::Graphics::Capture::GraphicsCaptureItem;
using winrt::Windows::Graphics::Capture::GraphicsCaptureSession;
using winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice;
using winrt::Windows::Graphics::DirectX::DirectXPixelFormat;
using DxgiInterfaceAccess =
    ::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess;

[[nodiscard]] IDirect3DDevice create_winrt_device(winrt::com_ptr<ID3D11Device>& d3d_device,
                                                  winrt::com_ptr<ID3D11DeviceContext>& context) {
    constexpr UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL feature_level{};
    winrt::check_hresult(::D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION,
        d3d_device.put(), &feature_level, context.put()));
    static_cast<void>(feature_level);

    const auto dxgi_device = d3d_device.as<IDXGIDevice>();
    winrt::com_ptr<IInspectable> inspectable;
    winrt::check_hresult(
        ::CreateDirect3D11DeviceFromDXGIDevice(dxgi_device.get(), inspectable.put()));
    return inspectable.as<IDirect3DDevice>();
}

[[nodiscard]] GraphicsCaptureItem create_capture_item(const HWND window) {
    auto interop =
        winrt::get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    GraphicsCaptureItem item{nullptr};
    winrt::check_hresult(interop->CreateForWindow(
        window, winrt::guid_of<GraphicsCaptureItem>(), winrt::put_abi(item)));
    return item;
}

} // namespace

class WindowsGraphicsCaptureSource::Impl {
public:
    explicit Impl(WindowsGraphicsCaptureConfig config) : config_(config) {
        if (config_.window_id == 0 || config_.maximum_frames_per_second == 0) {
            throw std::invalid_argument("invalid Windows capture configuration");
        }
    }

    ~Impl() {
        stop();
    }

    void start(FrameHandler on_frame, CompletionHandler on_complete) {
        std::lock_guard lock(mutex_);
        if (started_) {
            throw std::logic_error("Windows capture source has already been started");
        }
        if (!GraphicsCaptureSession::IsSupported()) {
            throw std::runtime_error("Windows Graphics Capture is not supported");
        }
        const auto window = reinterpret_cast<HWND>(config_.window_id);
        if (!::IsWindow(window)) {
            throw std::runtime_error("selected window no longer exists");
        }

        on_frame_ = std::move(on_frame);
        on_complete_ = std::move(on_complete);
        device_ = create_winrt_device(d3d_device_, d3d_context_);
        item_ = create_capture_item(window);
        current_size_ = item_.Size();
        if (current_size_.Width <= 0 || current_size_.Height <= 0) {
            throw std::runtime_error("selected window has invalid capture dimensions");
        }
        frame_pool_ = Direct3D11CaptureFramePool::CreateFreeThreaded(
            device_, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, current_size_);
        frame_arrived_token_ = frame_pool_.FrameArrived(
            [this](const Direct3D11CaptureFramePool& sender, const auto&) {
                try {
                    handle_frame(sender);
                } catch (const std::exception& error) {
                    std::cerr << "capture_error=" << error.what() << '\n';
                    stop();
                } catch (...) {
                    std::cerr << "capture_error=unknown Windows Graphics Capture error\n";
                    stop();
                }
            });
        closed_token_ = item_.Closed([this](const auto&, const auto&) { complete(); });
        session_ = frame_pool_.CreateCaptureSession(item_);
        started_ = true;
        session_.StartCapture();
    }

    void stop() {
        CompletionHandler completion;
        {
            std::lock_guard lock(mutex_);
            if (stopping_) {
                return;
            }
            stopping_ = true;
            if (frame_pool_) {
                frame_pool_.FrameArrived(frame_arrived_token_);
            }
            if (item_) {
                item_.Closed(closed_token_);
            }
            if (session_) {
                session_.Close();
                session_ = nullptr;
            }
            if (frame_pool_) {
                frame_pool_.Close();
                frame_pool_ = nullptr;
            }
            item_ = nullptr;
            staging_texture_ = nullptr;
            completion = take_completion_locked();
        }
        if (completion) {
            completion();
        }
    }

private:
    void handle_frame(const Direct3D11CaptureFramePool& sender) {
        FrameHandler handler;
        RawFrame output;
        bool recreate = false;
        winrt::Windows::Graphics::SizeInt32 next_size{};
        {
            std::lock_guard lock(mutex_);
            if (stopping_) {
                return;
            }
            const Direct3D11CaptureFrame frame = sender.TryGetNextFrame();
            if (!frame) {
                return;
            }
            const auto now = std::chrono::steady_clock::now();
            const auto minimum_interval =
                std::chrono::milliseconds(1000 / config_.maximum_frames_per_second);
            if (last_frame_at_.time_since_epoch().count() != 0 &&
                now - last_frame_at_ < minimum_interval) {
                return;
            }

            const auto access = frame.Surface().as<DxgiInterfaceAccess>();
            winrt::com_ptr<ID3D11Texture2D> texture;
            winrt::check_hresult(access->GetInterface(winrt::guid_of<ID3D11Texture2D>(),
                                                      texture.put_void()));
            D3D11_TEXTURE2D_DESC description{};
            texture->GetDesc(&description);
            constexpr std::uint32_t kMaximumCaptureDimension = 16384;
            if (description.Width == 0 || description.Height == 0 ||
                description.Width > kMaximumCaptureDimension ||
                description.Height > kMaximumCaptureDimension ||
                static_cast<std::size_t>(description.Width) * 4U >
                    std::numeric_limits<std::size_t>::max() / description.Height) {
                throw std::runtime_error("captured texture dimensions are invalid");
            }
            ensure_staging_texture(description);
            d3d_context_->CopyResource(staging_texture_.get(), texture.get());

            const auto width = description.Width;
            const auto height = description.Height;
            const auto stride = width * 4U;
            output = RawFrame{
                .frame_id = next_frame_id_++,
                .capture_timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                            now.time_since_epoch())
                                            .count(),
                .width = width,
                .height = height,
                .stride = stride,
                .pixel_format = PixelFormat::bgra32,
                .pixels = std::vector<std::uint8_t>(static_cast<std::size_t>(stride) * height),
            };
            D3D11_MAPPED_SUBRESOURCE mapped{};
            winrt::check_hresult(d3d_context_->Map(staging_texture_.get(), 0, D3D11_MAP_READ, 0,
                                                  &mapped));
            for (std::uint32_t row = 0; row < height; ++row) {
                std::memcpy(output.pixels.data() + static_cast<std::size_t>(row) * stride,
                            static_cast<const std::uint8_t*>(mapped.pData) +
                                static_cast<std::size_t>(row) * mapped.RowPitch,
                            stride);
            }
            d3d_context_->Unmap(staging_texture_.get(), 0);
            last_frame_at_ = now;
            handler = on_frame_;

            next_size = frame.ContentSize();
            recreate = next_size.Width > 0 && next_size.Height > 0 &&
                       (next_size.Width != current_size_.Width ||
                        next_size.Height != current_size_.Height);
            if (recreate) {
                current_size_ = next_size;
                staging_texture_ = nullptr;
            }
        }

        if (handler) {
            handler(std::move(output));
        }
        if (recreate) {
            std::lock_guard lock(mutex_);
            if (!stopping_) {
                frame_pool_.Recreate(device_, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2,
                                     next_size);
            }
        }
    }

    void ensure_staging_texture(const D3D11_TEXTURE2D_DESC& source) {
        if (staging_texture_) {
            D3D11_TEXTURE2D_DESC current{};
            staging_texture_->GetDesc(&current);
            if (current.Width == source.Width && current.Height == source.Height &&
                current.Format == source.Format) {
                return;
            }
        }
        D3D11_TEXTURE2D_DESC staging = source;
        staging.BindFlags = 0;
        staging.MiscFlags = 0;
        staging.Usage = D3D11_USAGE_STAGING;
        staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        winrt::check_hresult(d3d_device_->CreateTexture2D(&staging, nullptr,
                                                         staging_texture_.put()));
    }

    void complete() {
        CompletionHandler completion;
        {
            std::lock_guard lock(mutex_);
            completion = take_completion_locked();
        }
        if (completion) {
            completion();
        }
    }

    CompletionHandler take_completion_locked() {
        if (completion_delivered_) {
            return {};
        }
        completion_delivered_ = true;
        return std::move(on_complete_);
    }

    WindowsGraphicsCaptureConfig config_;
    std::mutex mutex_;
    FrameHandler on_frame_;
    CompletionHandler on_complete_;
    winrt::com_ptr<ID3D11Device> d3d_device_;
    winrt::com_ptr<ID3D11DeviceContext> d3d_context_;
    winrt::com_ptr<ID3D11Texture2D> staging_texture_;
    IDirect3DDevice device_{nullptr};
    GraphicsCaptureItem item_{nullptr};
    Direct3D11CaptureFramePool frame_pool_{nullptr};
    GraphicsCaptureSession session_{nullptr};
    winrt::event_token frame_arrived_token_{};
    winrt::event_token closed_token_{};
    winrt::Windows::Graphics::SizeInt32 current_size_{};
    std::chrono::steady_clock::time_point last_frame_at_{};
    std::uint64_t next_frame_id_{0};
    bool started_{false};
    bool stopping_{false};
    bool completion_delivered_{false};
};

WindowsGraphicsCaptureSource::WindowsGraphicsCaptureSource(WindowsGraphicsCaptureConfig config)
    : impl_(std::make_unique<Impl>(config)) {}

WindowsGraphicsCaptureSource::~WindowsGraphicsCaptureSource() = default;

void WindowsGraphicsCaptureSource::start(FrameHandler on_frame,
                                         CompletionHandler on_complete) {
    impl_->start(std::move(on_frame), std::move(on_complete));
}

void WindowsGraphicsCaptureSource::stop() {
    impl_->stop();
}

} // namespace cloud_stream::capture::windows
