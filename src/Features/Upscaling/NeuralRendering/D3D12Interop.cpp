#include "D3D12Interop.h"

#include "Globals.h"
#include "Runtime.h"
#include "Utils/D3D.h"

#include <format>

namespace NR
{
	namespace
	{
		constexpr uint32_t kNvidiaVendorId = 0x10DEu;
	}

	void D3D12Interop::Check(HRESULT result)
	{
		if (SUCCEEDED(result))
			return;
		if (DeviceRemoved())
			throw std::runtime_error(std::format("NR D3D12 device removed (0x{:08X})", static_cast<uint32_t>(device->GetDeviceRemovedReason())));
		throw std::runtime_error(std::format("{}D3D12 call failed (HRESULT 0x{:08X})", kInitializationPrefix, static_cast<uint32_t>(result)));
	}

	void D3D12Interop::Initialize()
	{
		if (device)
			return;
		try {
			Check(globals::d3d::device->QueryInterface(device11.put()));
			Check(globals::d3d::context->QueryInterface(context.put()));
			winrt::com_ptr<IDXGIDevice> dxgi;
			Check(device11->QueryInterface(dxgi.put()));
			winrt::com_ptr<IDXGIAdapter> adapter;
			Check(dxgi->GetAdapter(adapter.put()));
			DXGI_ADAPTER_DESC desc{};
			Check(adapter->GetDesc(&desc));
			if (desc.VendorId != kNvidiaVendorId)
				throw std::runtime_error("requires an NVIDIA RTX GPU");
			// The level is a minimum: the runtime hands back the adapter's existing device.
			Check(D3D12CreateDevice(adapter.get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(device.put())));
			D3D12_COMMAND_QUEUE_DESC queueDesc{};
			queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
			Check(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(queue.put())));
			Check(queue->SetName(L"NeuralRendering::Queue"));
			for (auto& slot : commands) {
				Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(slot.allocator.put())));
				Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
					slot.allocator.get(), nullptr, IID_PPV_ARGS(slot.list.put())));
				Check(slot.allocator->SetName(L"NeuralRendering::Allocator"));
				Check(slot.list->SetName(L"NeuralRendering::Commands"));
				Check(slot.list->Close());
			}

			// The NT handle is closed by its owner whether or not the D3D11 side opens it.
			winrt::handle sharedFence;
			Check(device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(fence12.put())));
			Check(device->CreateSharedHandle(fence12.get(), nullptr, GENERIC_ALL, nullptr, sharedFence.put()));
			Check(device11->OpenSharedFence(sharedFence.get(), IID_PPV_ARGS(fence11.put())));
			Check(fence12->SetName(L"NeuralRendering::Fence"));
			Util::SetResourceName(fence11.get(), "NeuralRendering::Fence");
			fenceValue = 0;
			adapterLuid = desc.AdapterLuid;
		} catch (...) {
			// A half-built interop would make the next attempt reuse a device with no queue behind it.
			Reset();
			throw;
		}
	}

	void D3D12Interop::Reset()
	{
		commands = {};
		fence12 = nullptr;
		fence11 = nullptr;
		fenceValue = 0;
		queue = nullptr;
		device = nullptr;
		context = nullptr;
		device11 = nullptr;
		adapterLuid = {};
		cursor = 0;
	}

	std::unique_ptr<WrappedResource> D3D12Interop::CreateTexture(uint32_t width, uint32_t height, DXGI_FORMAT format, const std::string& name)
	{
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = width;
		desc.Height = height;
		desc.Format = format;
		desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		return std::make_unique<WrappedResource>(desc, device11.get(), device.get(), name);
	}

	void D3D12Interop::Wait(uint64_t completion)
	{
		if (!fence12 || completion == 0)
			return;
		Check(device->GetDeviceRemovedReason());
		if (fence12->GetCompletedValue() >= completion)
			return;

		winrt::handle fenceEvent(CreateEventW(nullptr, FALSE, FALSE, nullptr));
		if (!fenceEvent)
			throw std::runtime_error(std::format("NR GPU fence {} wait failed (error 0x{:08X})", completion, GetLastError()));
		if (const HRESULT registration = fence12->SetEventOnCompletion(completion, fenceEvent.get()); FAILED(registration))
			throw std::runtime_error(std::format("NR GPU fence {} wait failed (error 0x{:08X})", completion, static_cast<uint32_t>(registration)));

		DWORD waitedMs = 0;
		while (waitedMs < kFenceTimeoutMs) {
			const DWORD sliceMs = std::min<DWORD>(kRemovalPollMs, kFenceTimeoutMs - waitedMs);
			const DWORD waitResult = WaitForSingleObject(fenceEvent.get(), sliceMs);
			if (waitResult == WAIT_OBJECT_0)
				return;
			if (waitResult != WAIT_TIMEOUT)
				throw std::runtime_error(std::format("NR GPU fence {} wait failed (error 0x{:08X})", completion, GetLastError()));
			waitedMs += sliceMs;
			Check(device->GetDeviceRemovedReason());
		}
		throw std::runtime_error(std::format("NR GPU fence {} did not retire within {} ms", completion, kFenceTimeoutMs));
	}

	ID3D12GraphicsCommandList* D3D12Interop::Begin()
	{
		auto& slot = commands[cursor];
		Wait(slot.completion);
		winrt::check_hresult(slot.allocator->Reset());
		winrt::check_hresult(slot.list->Reset(slot.allocator.get(), nullptr));
		const auto ready = NextFenceValue();
		winrt::check_hresult(context->Signal(fence11.get(), ready));
		context->Flush();
		winrt::check_hresult(queue->Wait(fence12.get(), ready));
		return slot.list.get();
	}

	void D3D12Interop::End()
	{
		auto& slot = commands[cursor];
		winrt::check_hresult(slot.list->Close());
		ID3D12CommandList* lists[]{ slot.list.get() };
		queue->ExecuteCommandLists(1, lists);
		const auto complete = NextFenceValue();
		winrt::check_hresult(queue->Signal(fence12.get(), complete));
		slot.completion = complete;
		winrt::check_hresult(context->Wait(fence11.get(), complete));
		cursor = (cursor + 1) % static_cast<uint32_t>(commands.size());
	}

	void D3D12Interop::Drain()
	{
		if (!fence12 || !fence11)
			return;
		const auto ready = NextFenceValue();
		winrt::check_hresult(context->Signal(fence11.get(), ready));
		context->Flush();
		winrt::check_hresult(queue->Wait(fence12.get(), ready));
		const auto complete = NextFenceValue();
		winrt::check_hresult(queue->Signal(fence12.get(), complete));
		Wait(complete);
	}
}
