#include "gepch.h"
#include "GanymedE/Utils/PlatformUtils.h"

#ifdef GE_PLATFORM_WINDOWS

#include <commdlg.h>
#include <shlobj.h>
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

#include "GanymedE/main/Application.h"

namespace GanymedE {
	std::string FileDialogs::OpenFile(const char* filter)
	{
		OPENFILENAMEA ofn;
		CHAR szFile[260] = { 0 };
		ZeroMemory(&ofn, sizeof(OPENFILENAME));
		ofn.lStructSize = sizeof(OPENFILENAME);
		ofn.hwndOwner = glfwGetWin32Window((GLFWwindow*)Application::Get().GetWindow().GetNativeWindow());
		ofn.lpstrFile = szFile;
		ofn.nMaxFile = sizeof(szFile);
		ofn.lpstrFilter = filter;
		ofn.nFilterIndex = 1;
		ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
		if (GetOpenFileNameA(&ofn) == TRUE)
		{
			return ofn.lpstrFile;
		}
		return std::string();
	}

	std::string FileDialogs::SaveFile(const char* filter)
	{
		OPENFILENAMEA ofn;
		CHAR szFile[260] = { 0 };
		ZeroMemory(&ofn, sizeof(OPENFILENAME));
		ofn.lStructSize = sizeof(OPENFILENAME);
		ofn.hwndOwner = glfwGetWin32Window((GLFWwindow*)Application::Get().GetWindow().GetNativeWindow());
		ofn.lpstrFile = szFile;
		ofn.nMaxFile = sizeof(szFile);
		ofn.lpstrFilter = filter;
		ofn.nFilterIndex = 1;
		ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
		if (GetSaveFileNameA(&ofn) == TRUE)
		{
			return ofn.lpstrFile;
		}
		return std::string();
	}

	bool DesktopShell::RevealInFileBrowser(const std::filesystem::path& path)
	{
		// SHOpenFolderAndSelectItems rather than `explorer /select,<path>`: it reuses an Explorer
		// window already showing that folder instead of opening one per call, and it takes an
		// ID list, so there is no command line to quote. With no child items it opens the parent
		// of `pidl` and selects it - which is why one call serves files and folders alike.
		std::error_code ec;
		const std::wstring native = std::filesystem::absolute(path, ec).lexically_normal().wstring();
		if (ec)
			return false;

		// Shell calls need COM on the calling thread. Balance only an init this call made.
		const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

		HRESULT hr = E_FAIL;
		if (PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(native.c_str()))
		{
			hr = SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
			ILFree(pidl);
		}

		if (SUCCEEDED(init))
			CoUninitialize();
		return SUCCEEDED(hr);
	}

	bool DesktopShell::OpenInVSCode(const std::filesystem::path& path)
	{
		// The installer puts <install>\bin on PATH, but bin\code.cmd is a batch file - launching
		// it would flash a console window. Code.exe sits one level above it, and starting that
		// with the file as the argument is exactly what Explorer's "Open with Code" verb does.
		wchar_t script[MAX_PATH];
		const DWORD found = SearchPathW(nullptr, L"code.cmd", nullptr, MAX_PATH, script, nullptr);
		if (found == 0 || found >= MAX_PATH)
			return false;

		std::error_code ec;
		const std::filesystem::path exe = std::filesystem::path(script).parent_path().parent_path() / L"Code.exe";
		if (!std::filesystem::is_regular_file(exe, ec))
			return false;

		const std::wstring native = std::filesystem::absolute(path, ec).lexically_normal().wstring();
		if (ec)
			return false;

		// Windows paths cannot contain '"', so plain quoting is enough. CreateProcessW may write
		// into the command line, hence the mutable string.
		std::wstring commandLine = L"\"" + exe.wstring() + L"\" \"" + native + L"\"";
		STARTUPINFOW startup = {};
		startup.cb = sizeof(startup);
		PROCESS_INFORMATION process = {};
		if (!CreateProcessW(exe.c_str(), commandLine.data(), nullptr, nullptr, FALSE, 0,
			nullptr, nullptr, &startup, &process))
			return false;

		CloseHandle(process.hThread);
		CloseHandle(process.hProcess);
		return true;
	}

}

#endif
