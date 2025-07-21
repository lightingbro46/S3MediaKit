#include <string>
#include <fstream>
#include <array>
#include <memory>
#include <cstdio>
#include "osInfo.h"

using namespace std;

namespace managerkit {

#if defined(_WIN32)
#include <windows.h>
#include <comdef.h>
#include <Wbemidl.h>
#pragma comment(lib, "wbemuuid.lib")
#include <VersionHelpers.h>

std::string getHardwareUUID() {
    HRESULT hres;
    std::string uuid = "Unavailable";

    hres = CoInitializeEx(0, COINIT_MULTITHREADED);
    if (FAILED(hres)) return uuid;

    hres = CoInitializeSecurity(NULL, -1, NULL, NULL,
                                RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE,
                                NULL, EOAC_NONE, NULL);
    if (FAILED(hres)) {
        CoUninitialize();
        return uuid;
    }

    IWbemLocator *pLoc = nullptr;
    hres = CoCreateInstance(CLSID_WbemLocator, 0, CLSCTX_INPROC_SERVER,
                            IID_IWbemLocator, (LPVOID *)&pLoc);
    if (FAILED(hres)) {
        CoUninitialize();
        return uuid;
    }

    IWbemServices *pSvc = nullptr;
    hres = pLoc->ConnectServer(_bstr_t(L"ROOT\\CIMV2"), NULL, NULL, 0, NULL, 0, 0, &pSvc);
    if (FAILED(hres)) {
        pLoc->Release();
        CoUninitialize();
        return uuid;
    }

    hres = CoSetProxyBlanket(pSvc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE,
                             NULL, RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE,
                             NULL, EOAC_NONE);

    IEnumWbemClassObject* pEnumerator = nullptr;
    hres = pSvc->ExecQuery(bstr_t("WQL"),
                           bstr_t("SELECT UUID FROM Win32_ComputerSystemProduct"),
                           WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                           NULL, &pEnumerator);

    if (FAILED(hres)) {
        pSvc->Release();
        pLoc->Release();
        CoUninitialize();
        return uuid;
    }

    IWbemClassObject *pclsObj = nullptr;
    ULONG uReturn = 0;
    while (pEnumerator) {
        HRESULT hr = pEnumerator->Next(WBEM_INFINITE, 1, &pclsObj, &uReturn);
        if (!uReturn) break;

        VARIANT vtProp;
        hr = pclsObj->Get(L"UUID", 0, &vtProp, 0, 0);
        if (SUCCEEDED(hr)) {
            _bstr_t bstr(vtProp.bstrVal);
            uuid = (const char*)bstr;
        }
        VariantClear(&vtProp);
        pclsObj->Release();
    }

    pSvc->Release();
    pLoc->Release();
    pEnumerator->Release();
    CoUninitialize();
    return uuid;
}

OSInfo get_os_info() {
    OSInfo info;
    info.platform = "Windows";

    // Lấy version từ registry (vì GetVersionEx bị hạn chế sau Windows 8.1)
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        char productName[256];
        DWORD size = sizeof(productName);
        if (RegQueryValueExA(hKey, "ProductName", nullptr, nullptr, (LPBYTE)productName, &size) == ERROR_SUCCESS) {
            info.variant = productName;
        }

        char buildNumber[256];
        size = sizeof(buildNumber);
        if (RegQueryValueExA(hKey, "CurrentBuildNumber", nullptr, nullptr, (LPBYTE)buildNumber, &size) == ERROR_SUCCESS) {
            info.variant_version = buildNumber;
        }

        RegCloseKey(hKey);
    }
    return info;
}

#elif defined(__linux__) || defined(__linux)
#include <fstream>
#include <sys/utsname.h>

std::string getHardwareUUID() {
    std::ifstream uuidFile("/sys/class/dmi/id/product_uuid");
    std::string uuid;
    if (uuidFile.is_open()) {
        getline(uuidFile, uuid);
        uuidFile.close();
    } else {
        uuid = "Unavailable";
    }
    return uuid;
}

OSInfo get_os_info() {
    OSInfo info;
    info.platform = "Linux";
    std::ifstream os_release("/etc/os-release");
    std::string line;

    while (std::getline(os_release, line)) {
        if (line.find("NAME=") == 0) {
            size_t first = line.find("\""), last = line.rfind("\"");
            if (first != std::string::npos && last != std::string::npos && last > first)
                info.variant = line.substr(first + 1, last - first - 1);
        } else if (line.find("VERSION_ID=") == 0) {
            size_t first = line.find("\""), last = line.rfind("\"");
            if (first != std::string::npos && last != std::string::npos && last > first)
                info.variant_version = line.substr(first + 1, last - first - 1);
        }
    }
    return info;
}

#elif defined(__APPLE__)
#include <array>
#include <memory>
#include <cstdio>
#include <sys/utsname.h>

std::string execCommand(const char* cmd) {
    std::array<char, 128> buffer;
    std::string result;
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd, "r"), pclose);
    if (!pipe) return "Unavailable";
    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
        result += buffer.data();
    }
    return result;
}

std::string getHardwareUUID() {
    std::string output = execCommand("ioreg -rd1 -c IOPlatformExpertDevice | grep IOPlatformUUID");
    std::size_t firstQuote = output.find('"');
    std::size_t secondQuote = output.find('"', firstQuote + 1);
    std::size_t thirdQuote = output.find('"', secondQuote + 1);
    std::size_t fourthQuote = output.find('"', thirdQuote + 1);
    if (thirdQuote != std::string::npos && fourthQuote != std::string::npos) {
        return output.substr(thirdQuote + 1, fourthQuote - thirdQuote - 1);
    }
    return "Unavailable";
}

OSInfo get_os_info() {
    OSInfo info;
    info.platform = "macOS";
    struct utsname uts{};
    if (uname(&uts) == 0) {
        info.variant = "macOS";
        info.variant_version = uts.release;  // ex: 22.6.0 (macOS Ventura)
    }
    return info;
}

#else

std::string getHardwareUUID() {
    return "Unsupported platform";
}

OSInfo get_os_info() {
    OSInfo info;
    info.platform = "Unsupported platform";
    return info;
}

#endif

} // namespace managerkit
