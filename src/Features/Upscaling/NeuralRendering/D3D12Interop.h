#pragma once

#include "../DX12SwapChain.h"

#include <array>
#include <cstdint>
#include <d3d11_4.h>
#include <d3d12.h>

namespace NR
{
	/**
	 * @brief Owns the same-adapter D3D12 queue Neural Rendering evaluates on, and the fence that
	 *        orders it against the game's D3D11 context.
	 *
	 * Ported from Open Shaders (#723). Open Shaders shares a SharedFence type with its runtime
	 * upscaler; this fork has no such type, so the fence lives here.
	 */
	class D3D12Interop
	{
	public:
		/** @brief Command-allocator ring depth; a slot is reused once its submission retires. */
		static constexpr uint32_t kFramesInFlight = 3;
		/** @brief CPU bound for one fence value; a wedged GPU must not hang the render thread. */
		static constexpr DWORD kFenceTimeoutMs = 5000;
		/** @brief Interval at which a CPU wait checks whether the device was removed. */
		static constexpr DWORD kRemovalPollMs = 100;

		/** @brief Creates the device, queue, ring and shared fence on the renderer's adapter. */
		void Initialize();
		/** @brief Creates a named, view-bearing D3D11 texture shared with the NR device. */
		std::unique_ptr<WrappedResource> CreateTexture(uint32_t width, uint32_t height, DXGI_FORMAT format, const std::string& name);
		/** @brief Acquires a retired allocator and queues the D3D11 input dependency. */
		ID3D12GraphicsCommandList* Begin();
		/** @brief Submits NR commands and queues the D3D11 output dependency. */
		void End();
		/**
		 * @brief Retires all submitted work on both APIs.
		 *        Throws rather than releasing anything: on a failure the caller must keep the
		 *        resources alive, because the GPU may still reference them.
		 */
		void Drain();

		/** @brief Returns the device used by NGX. */
		ID3D12Device* Device() const { return device.get(); }
		/** @brief LUID of the adapter that device was created on, for the caller's success log. */
		[[nodiscard]] LUID AdapterLuid() const { return adapterLuid; }
		/** @brief Asks the device whether it was removed, so a caller can release instead of waiting. */
		[[nodiscard]] bool DeviceRemoved() const { return device && FAILED(device->GetDeviceRemovedReason()); }

	private:
		struct Commands
		{
			winrt::com_ptr<ID3D12CommandAllocator> allocator;
			winrt::com_ptr<ID3D12GraphicsCommandList> list;
			uint64_t completion = 0;
		};

		/** @brief Throws for a failed HRESULT, naming device removal when the device reports it. */
		void Check(HRESULT result);
		/** @brief Blocks the CPU until completion retires; throws on a timeout or a removed device. */
		void Wait(uint64_t completion);
		/** @brief Releases every interop resource; drain first or the GPU may still read them. */
		void Reset();
		/** @brief Returns the next fence value to signal. */
		uint64_t NextFenceValue() { return ++fenceValue; }

		winrt::com_ptr<ID3D11Device5> device11;
		winrt::com_ptr<ID3D11DeviceContext4> context;
		winrt::com_ptr<ID3D12Device> device;
		winrt::com_ptr<ID3D12CommandQueue> queue;
		winrt::com_ptr<ID3D12Fence> fence12;
		winrt::com_ptr<ID3D11Fence> fence11;
		uint64_t fenceValue = 0;
		std::array<Commands, kFramesInFlight> commands;
		LUID adapterLuid{};
		uint32_t cursor = 0;
	};
}
