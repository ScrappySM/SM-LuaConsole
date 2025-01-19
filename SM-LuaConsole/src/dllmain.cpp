#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#undef WARN
#undef ERROR

#include <d3d11.h>
#include <dxgi.h>
#include <thread>
#include <vector>

#include <kiero/kiero.h>

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <carbon/tools.h>

typedef HRESULT(__stdcall* Present) (IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags);
typedef HRESULT(__stdcall* ResizeBuffers) (IDXGISwapChain* pSwapChain, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags);
typedef LRESULT(CALLBACK* WNDPROC)(HWND, UINT, WPARAM, LPARAM);

extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

using namespace Carbon::SM;

namespace G {
	Present oPresent;
	ResizeBuffers oResizeBuffers;
	WNDPROC oWndProc;

	ID3D11Device* pDevice = NULL;
	ID3D11DeviceContext* pContext = NULL;
	ID3D11RenderTargetView* mainRenderTargetView;

	bool presentReady = false;
	bool showMenu = true;

	GameStateType* gameStateType;
} // namespace Globals


static LRESULT WINAPI WndProc(const HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
	if (uMsg == WM_SIZE || uMsg == WM_MOVE || uMsg == WM_ENTERSIZEMOVE || uMsg == WM_EXITSIZEMOVE || uMsg == WM_PAINT || uMsg == WM_NCPAINT || uMsg == WM_ERASEBKGND || uMsg == WM_NCCALCSIZE)
		return CallWindowProc(G::oWndProc, hWnd, uMsg, wParam, lParam);

	// If we're not in a world, only capture input if the mouse is on top of the menu
	// Otherwise, capture input if the menu is open always (so we don't look around when trying to move the mouse to the menu)
	bool captureInput = (*G::gameStateType == Play) ? G::showMenu : (G::showMenu && ImGui::GetIO().WantCaptureMouse);
	if (ImGui_ImplWin32_WndProcHandler(hWnd, uMsg, wParam, lParam) || captureInput)
		return true;

	return CallWindowProc(G::oWndProc, hWnd, uMsg, wParam, lParam);
}

static HRESULT __stdcall hkResizeBuffers(IDXGISwapChain* pSwapChain, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags) {
	// Cleanup our old (hooked) resources
	if (G::mainRenderTargetView) G::mainRenderTargetView->Release();
	if (G::pContext) G::pContext->Release();
	if (G::pDevice) G::pDevice->Release();

	// Cleanup ImGui resources
	ImGui_ImplDX11_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();

	// Reset our state
	G::pDevice = NULL;
	G::pContext = NULL;
	G::mainRenderTargetView = NULL;

	// Tell hkPresent to reinitialize everything
	G::presentReady = false;

	// Restore the original WndProc
	SetWindowLongPtr(GetForegroundWindow(), GWLP_WNDPROC, (LONG_PTR)G::oWndProc);

	INFO("[SM-LuaConsole] Cleaned up resources due to swap chain recreation (due to resizing window)");

	// Call the original function
	return G::oResizeBuffers(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);
}

static HRESULT __stdcall hkPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags) {
#define CHECK_HRES(x) if (x != S_OK) { ERROR("HRESULT failed (__LINE__)"); return G::oPresent(pSwapChain, SyncInterval, Flags); }
	if (!G::presentReady) {
		CHECK_HRES(pSwapChain->GetDevice(__uuidof(G::pDevice), (void**)&G::pDevice))
		G::pDevice->GetImmediateContext(&G::pContext);

		DXGI_SWAP_CHAIN_DESC sd;
		CHECK_HRES(pSwapChain->GetDesc(&sd))
		ID3D11Texture2D* pBackBuffer = NULL;
		CHECK_HRES(pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (LPVOID*)&pBackBuffer));
		if (pBackBuffer == 0) return G::oPresent(pSwapChain, SyncInterval, Flags);
		CHECK_HRES(G::pDevice->CreateRenderTargetView(pBackBuffer, NULL, &G::mainRenderTargetView));
		pBackBuffer->Release();

		G::oWndProc = (WNDPROC)SetWindowLongPtr(sd.OutputWindow, GWLP_WNDPROC, (LONG_PTR)WndProc);

		IMGUI_CHECKVERSION();
		ImGui::CreateContext();
		ImGuiIO& io = ImGui::GetIO();
		io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
		io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
		io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
		io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
		ImGui_ImplWin32_Init(sd.OutputWindow);
		ImGui_ImplDX11_Init(G::pDevice, G::pContext);

		INFO("[SM-LuaConsole] Initialized DX11 resources, ImGui and the WndProc");
		G::presentReady = true;
	}

	if (G::showMenu) {
		ImGui_ImplDX11_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();

		ImGui::ShowDemoWindow(&G::showMenu);

		if (*G::gameStateType == Play) {
			ImDrawList* fg = ImGui::GetForegroundDrawList();
			fg->AddCircleFilled(ImGui::GetMousePos(), 5, IM_COL32(245, 245, 245, 128), 12);
		}

		ImGui::Render();

		G::pContext->OMSetRenderTargets(1, &G::mainRenderTargetView, NULL);
		ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
	}

	return G::oPresent(pSwapChain, SyncInterval, Flags);
}

DWORD WINAPI MainThread(LPVOID lpReserved) {
	if (kiero::init(kiero::RenderType::D3D11) == kiero::Status::Success) {
		kiero::bind(8, (void**)&G::oPresent, hkPresent);
		kiero::bind(13, (void**)&G::oResizeBuffers, hkResizeBuffers);
		INFO("[SM-LuaConsole] Successfully hooked D3D11 Present");
	}
	else {
		ERROR("[SM-LuaConsole] Failed to hook D3D11 Present\n");
		return FALSE;
	}

	while (!GetAsyncKeyState(VK_NEXT)) {
		if (GetAsyncKeyState(VK_INSERT) & 1) {
			G::showMenu = !G::showMenu;
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}

	FreeLibraryAndExitThread(static_cast<HMODULE>(lpReserved), 0);
	return TRUE;
}

BOOLEAN WINAPI DllMain(
	IN HMODULE hMod,
	IN DWORD   dwReason,
	IN LPVOID  lpReserved
) {
	static HANDLE hMainThread = NULL;

	switch (dwReason) {
	case DLL_PROCESS_ATTACH:
		DisableThreadLibraryCalls(hMod);

		static auto contraption = Contraption::GetInstance();
		G::gameStateType = &contraption->gameState;

		hMainThread = CreateThread(nullptr, 0, MainThread, hMod, 0, nullptr);
		break;

	case DLL_PROCESS_DETACH:
		if (hMainThread != NULL) {
			// All handling of shutdown is done outside of the thread
			// This means we can just terminate the thread, and it will clean up after itself
			TerminateThread(hMainThread, 0);
			CloseHandle(hMainThread);
		}

		// Unhook
		kiero::unbind(8);
		kiero::unbind(13);

		static auto window = FindWindowA("CONTRAPTION_WINDOWS_CLASS", nullptr);
		SetWindowLongPtr(window, GWLP_WNDPROC, (LONG_PTR)G::oWndProc);

		// Reset everything
		if (G::mainRenderTargetView) G::mainRenderTargetView->Release();
		if (G::pContext) G::pContext->Release();
		if (G::pDevice) G::pDevice->Release();
		if (G::presentReady) {
			ImGui_ImplDX11_Shutdown();
			ImGui_ImplWin32_Shutdown();
			ImGui::DestroyContext();
		}

		kiero::shutdown();
		break;
	}

	return TRUE;
}