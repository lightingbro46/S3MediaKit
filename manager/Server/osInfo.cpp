#include <string>
#include <fstream>
#include <array>
#include <memory>
#include <cstdio>
#include "osInfo.h"

using namespace std;
using namespace mediakit;

#if defined(_WIN32)
#include <windows.h>
#include <comdef.h>
#include <Wbemidl.h>
#pragma comment(lib, "wbemuuid.lib")

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


#elif defined(__linux__) || defined(__linux)
#include <fstream>

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

#elif defined(__APPLE__)
#include <array>
#include <memory>
#include <cstdio>

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

#else

std::string getHardwareUUID() {
    return "Unsupported platform";
}

#endif

