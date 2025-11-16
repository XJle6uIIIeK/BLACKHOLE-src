#include "lib/Helpers.hpp"
#include "resource.h"

#include <Windows.h>
#include <ctime>
#include <filesystem>
#include <fstream>

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previousInstance, LPSTR cmdLine, int showCmd)
{
	wchar_t tempPath[MAX_PATH];
	std::size_t charCount = GetTempPath(MAX_PATH, tempPath);

	for (const auto &entry : std::filesystem::directory_iterator(tempPath))
	{
		if (entry.is_regular_file() && !Helpers::fileInUse(entry.path()) && entry.path().filename().wstring().ends_with(L"-BLACKHOLE-tmp.dll"))
			std::filesystem::remove(entry.path());
	}

	const auto csgoPid = Helpers::findPid(L"csgo.exe");
	if (!csgoPid)
		exit(-1);
		//MessageBoxA(0, "Failed to load BLACKHOLE.\nYou need to run CS:GO before running the loader.", "BLACKHOLE", MB_OK | MB_ICONERROR);
	else
	{
		auto csgoProcess = OpenProcess(PROCESS_ALL_ACCESS, false, csgoPid);
		if (!csgoProcess)
			exit(-1);
		//MessageBoxA(0, "Failed to load BLACKHOLE.\nFailed to open CS:GO process. Try running NLoader with administrator privileges.", "BLACKHOLE", MB_OK | MB_ICONERROR);
		else
		{
			if (!Helpers::bypassCsgoInject(csgoProcess))
				exit(-1);
			//MessageBoxA(0, "Failed to load BLACKHOLE.\nFailed to bypass CS:GO library inject protection.", "BLACKHOLE", MB_OK | MB_ICONERROR);
			else
			{
				wchar_t tempFilePath[MAX_PATH];
				std::srand(static_cast<int>(std::time(nullptr)));
				std::swprintf(tempFilePath, MAX_PATH, L"%ls\\%i-BLACKHOLE-tmp.dll", tempPath, std::rand());

				std::ofstream tempFile{tempFilePath, std::ios::binary | std::ios::out};
				if (tempFile.is_open())
				{
					std::size_t size;
					const auto resource = Helpers::loadFromResource(&size);
					const void *dllData = LockResource(resource);

					tempFile.write(reinterpret_cast<const char *>(dllData), size);
					tempFile.close();

					std::uintptr_t dllModuleHandle = 0;
					if (!Helpers::loadLibreryRemote(csgoProcess, tempFilePath, &dllModuleHandle))
						exit(-1);
					//MessageBoxA(0, "Failed to load BLACKHOLE.\nUnknown error has occured while loading library.", "BLACKHOLE", MB_OK | MB_ICONERROR);
					else
					{
						if (!dllModuleHandle)
							exit(-1);
						//MessageBoxA(0, "Failed to load BLACKHOLE.\nDllMain returned false or load library failed.", "BLACKHOLE", MB_OK | MB_ICONERROR);
#ifndef BLACKHOLE_DEBUG
						else
							Sleep(1);
							//MessageBoxA(0, "Success! BLACKHOLE is now loaded.", "BLACKHOLE", MB_OK | MB_ICONINFORMATION);
						#endif // !BLACKHOLE_DEBUG
					}
		
					FreeResource(resource);
				} else
					exit(-1);
				//MessageBoxA(0, "Failed to load BLACKHOLE.\nFailed to create temporary file.", "BLACKHOLE", MB_OK | MB_ICONERROR);
			}

			CloseHandle(csgoProcess);
		}
	}

	return TRUE;
}
