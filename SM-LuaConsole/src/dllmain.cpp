#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#undef WARN
#undef ERROR

#include <d3d11.h>
#include <dxgi.h>
#include <thread>
#include <vector>

#include <kiero/kiero.h>
#include <MinHook.h>

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <carbon/tools.h>
#include <carbon/lua/lua.hpp>

#include <TextEditor.h>
#include "font.h"

using namespace Carbon::SM;

typedef HRESULT(__stdcall* Present) (IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags);
typedef HRESULT(__stdcall* ResizeBuffers) (IDXGISwapChain* pSwapChain, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags);
typedef LRESULT(CALLBACK* WNDPROC)(HWND, UINT, WPARAM, LPARAM);

// Log_t = void(__int64 UTILS::Console* thisPtr, const std::string& message, WORD colour, int type)
typedef void(__fastcall* Log_t)(UTILS::Console*, const std::string&, UTILS::Colour, UTILS::LogType);

extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace G {
	Present oPresent;
	ResizeBuffers oResizeBuffers;
	WNDPROC oWndProc;
	Log_t oLog = nullptr;

	ID3D11Device* pDevice = NULL;
	ID3D11DeviceContext* pContext = NULL;
	ID3D11RenderTargetView* mainRenderTargetView = NULL;

	bool presentReady = false;
	bool showMenu = true;

	GameStateType* gameStateType = nullptr;
	TextEditor editor = TextEditor();

	std::vector<std::string> logMessages = {};
} // namespace Globals

static void LogWrapper(UTILS::Console* console, const std::string& message, UTILS::Colour colour, UTILS::LogType type) {
	if (type != UTILS::LogType::Lua && message.find("SM-LuaConsole") == std::string::npos) {
		return G::oLog(console, message, colour, type);
	}

	if (G::logMessages.size() > 1000)
		G::logMessages.erase(G::logMessages.begin());

	std::string msg = fmt::format("[Lua] {}", message);
	G::logMessages.emplace_back(msg);

	return G::oLog(console, message, colour, type);
}

static LRESULT WINAPI WndProc(const HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
	if (uMsg == WM_SIZE || uMsg == WM_MOVE || uMsg == WM_ENTERSIZEMOVE || uMsg == WM_EXITSIZEMOVE || uMsg == WM_PAINT || uMsg == WM_NCPAINT || uMsg == WM_ERASEBKGND || uMsg == WM_NCCALCSIZE)
		return CallWindowProc(G::oWndProc, hWnd, uMsg, wParam, lParam);

	// If we're not in a world, only capture input if the mouse is on top of the menu
	// Otherwise, capture input if the menu is open always (so we don't look around when trying to move the mouse to the menu)
	bool captureInput = (*G::gameStateType == Play) ? G::showMenu : (G::showMenu && ImGui::GetIO().WantCaptureMouse);
	/*if (ImGui_ImplWin32_WndProcHandler(hWnd, uMsg, wParam, lParam) || captureInput)
		return true;*/

	if (captureInput) {
		ImGui_ImplWin32_WndProcHandler(hWnd, uMsg, wParam, lParam);
		return true;
	}

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

		// Setup Dear ImGui style
		ImGui::StyleColorsDark();

		ImGuiStyle& style = ImGui::GetStyle();
		style.WindowRounding = 8.0f;
		style.FrameRounding = 4.0f;
		style.GrabRounding = 4.0f;
		style.PopupRounding = 4.0f;
		style.ScrollbarRounding = 4.0f;
		style.TabRounding = 4.0f;
		style.WindowBorderSize = 0.0f;
		style.FrameBorderSize = 0.0f;
		style.PopupBorderSize = 0.0f;
		style.ChildBorderSize = 0.0f;
		style.GrabMinSize = 8.0f;
		style.GrabRounding = 4.0f;

		// Load font (C:\Windows\Fonts\consola.ttf)
		//io.Fonts->AddFontFromMemoryCompressedTTF(JetBrainsMonoNF_compressed_data, JetBrainsMonoNF_compressed_size, 16.0f);
		ImFontConfig config;
		config.FontDataOwnedByAtlas = false;
		io.Fonts->AddFontFromMemoryCompressedTTF(JetBrainsMonoNF_compressed_data, JetBrainsMonoNF_compressed_size, 18.0f, &config);
		io.Fonts->Build();

		ImGui_ImplWin32_Init(sd.OutputWindow);
		ImGui_ImplDX11_Init(G::pDevice, G::pContext);

		INFO("[SM-LuaConsole] Initialized DX11 resources, ImGui and the WndProc");
		G::presentReady = true;
	}

	if (G::showMenu) {
		ImGui_ImplDX11_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();

		ImGui::Begin("SM-LuaConsole");

		auto region = ImGui::GetContentRegionAvail();
		region.y -= 64;
		G::editor.Render("##Editor", region);

		static bool removeAfterExecution = true;
		if (ImGui::Button("Execute")) {
			LuaExecutor::GetInstance()->OnUpdate([](lua_State* L) {
				luaL_dostring(L, G::editor.GetText().c_str());
				}, removeAfterExecution);
		}
		ImGui::SameLine();
		ImGui::Checkbox("Remove after execution", &removeAfterExecution);
		ImGui::SetItemTooltip("If checked, the function will only be called once");

		static bool immediate = true;
		if (ImGui::Button("Add to init")) {
			LuaExecutor::GetInstance()->OnInitialize([](lua_State* L) {
				luaL_dostring(L, G::editor.GetText().c_str());
				}, immediate);
		}

		ImGui::SameLine();
		ImGui::Checkbox("Immediate", &immediate);
		ImGui::SetItemTooltip("If checked, the function will be called immediately if the game is already playing");

		ImGui::End();

		ImGui::Begin("Log");

		// Colour and padding (make text pushed away from the border)
		ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(G::editor.GetPalette()[(int)TextEditor::PaletteIndex::Background]));

		ImGui::BeginChild("LogInner", ImVec2(0, 0), false);
		ImGui::Dummy(ImVec2(8, 8));
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.2f, 1.0f, 0.23f, 1.0f));
		for (const auto& message : G::logMessages) {
			ImGui::TextWrapped(message.c_str());
		}
		ImGui::PopStyleColor();

		if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
			ImGui::SetScrollHereY(1.0f);
		}
		ImGui::EndChild();
		ImGui::PopStyleColor();

		ImGui::End();

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

void DllCleanup();
DWORD WINAPI MainThread(LPVOID lpReserved) {
	if (kiero::init(kiero::RenderType::D3D11) == kiero::Status::Success) {
		kiero::bind(8, (void**)&G::oPresent, hkPresent);
		kiero::bind(13, (void**)&G::oResizeBuffers, hkResizeBuffers);
		INFO("[SM-LuaConsole] Successfully hooked D3D11 Present");

		if (MH_CreateHook(reinterpret_cast<LPVOID>(Carbon::Offsets::Rebased::LogFunc), LogWrapper, reinterpret_cast<LPVOID*>(&G::oLog)) != MH_OK) {
			ERROR("[SM-LuaConsole] Failed to hook Contraption::Console::Log\n");
			return FALSE;
		}

		if (MH_EnableHook(reinterpret_cast<LPVOID>(Carbon::Offsets::Rebased::LogFunc)) != MH_OK) {
			ERROR("[SM-LuaConsole] Failed to enable Contraption::Console::Log hook\n");
			return FALSE;
		}

		INFO("[SM-LuaConsole] Successfully hooked Contraption::Console::Log");
	}
	else {
		ERROR("[SM-LuaConsole] Failed to hook D3D11 Present\n");
		return FALSE;
	}

	G::editor.SetLanguageDefinition(TextEditor::LanguageDefinition::Lua());
	G::editor.SetShowWhitespaces(false);
	G::editor.SetPalette(TextEditor::GetDarkPalette());
	G::editor.SetText(R"(-- This is a Lua script
print("Hello, world!")
)");

	while (!GetAsyncKeyState(VK_NEXT)) {
		if (GetAsyncKeyState(VK_INSERT) & 1) {
			G::showMenu = !G::showMenu;
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}

	INFO("[SM-LuaConsole] Attempting to cleanup, uninjecting");
	DllCleanup();
	FreeLibraryAndExitThread(static_cast<HMODULE>(lpReserved), 0);
	return TRUE;
}

void DllCleanup() {
	// Unhook
	kiero::unbind(8);
	kiero::unbind(13);

	MH_DisableHook(reinterpret_cast<LPVOID>(Carbon::Offsets::Rebased::LogFunc));
	MH_RemoveHook(reinterpret_cast<LPVOID>(Carbon::Offsets::Rebased::LogFunc));

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

		DllCleanup();

		break;
	}

	return TRUE;
}