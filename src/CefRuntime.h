#pragma once

#include <string>

namespace cef_runtime {

int ExecuteSubprocess(void* nativeApplicationHandle);
bool Initialize(void* nativeApplicationHandle,
                const std::wstring& userDataPath,
                const std::wstring& locale,
                std::wstring* errorMessage);
void Shutdown(bool browserClosedCleanly = true);

}  // namespace cef_runtime
