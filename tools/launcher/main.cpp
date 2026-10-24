// 藏笺 单文件启动器
// 把整个运行目录（cangjian.exe + Qt DLL + qml + 插件）压成一个 zip 嵌进本 exe 的资源里，
// 首次运行解压到 %LOCALAPPDATA%\Cangjian\藏笺\bundle\<标记>，然后启动里面的 cangjian.exe。
// 之后再启动会直接复用已解压目录（按内嵌包大小做标记）。
#include <windows.h>
#include <shlobj.h>
#include <string>
#include <vector>
#include <cstdio>

#include "miniz.h"

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

static std::wstring moduleDir()
{
    wchar_t buf[MAX_PATH * 2] = {0};
    GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    std::wstring path(buf);
    const size_t pos = path.find_last_of(L'\\');
    return pos == std::wstring::npos ? std::wstring() : path.substr(0, pos);
}

static std::wstring localAppData()
{
    wchar_t *raw = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw)) && raw) {
        out.assign(raw);
        CoTaskMemFree(raw);
    }
    return out;
}

static void logFail(const wchar_t *stage, const std::wstring &detail)
{
    wchar_t tmp[MAX_PATH] = {0};
    GetTempPathW(MAX_PATH, tmp);
    std::wstring path = std::wstring(tmp) + L"cangjian-launcher.log";
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;
    wchar_t line[1024] = {0};
    swprintf_s(line, L"%s | %s | err=%lu\r\n", stage, detail.c_str(), GetLastError());
    DWORD written = 0;
    WriteFile(file, line, static_cast<DWORD>(wcslen(line) * sizeof(wchar_t)), &written, nullptr);
    CloseHandle(file);
}

static bool readPayload(std::vector<unsigned char> *out)
{
    HRSRC res = FindResourceW(nullptr, L"PAYLOAD", MAKEINTRESOURCEW(10));
    if (!res)
        return false;
    HGLOBAL handle = LoadResource(nullptr, res);
    if (!handle)
        return false;
    const DWORD size = SizeofResource(nullptr, res);
    const void *data = LockResource(handle);
    if (!data || size == 0)
        return false;
    out->assign(static_cast<const unsigned char *>(data),
                static_cast<const unsigned char *>(data) + size);
    return true;
}

static void ensureDir(const std::wstring &path)
{
    // 逐级创建，忽略已存在
    for (size_t i = 3; i <= path.size(); ++i) {
        if (i != path.size() && path[i] != L'\\' && path[i] != L'/')
            continue;
        const std::wstring part = path.substr(0, i);
        CreateDirectoryW(part.c_str(), nullptr);
    }
}

static bool extractAll(const std::vector<unsigned char> &payload, const std::wstring &target)
{
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_mem(&zip, payload.data(), payload.size(), 0))
        return false;

    const mz_uint count = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&zip, i, &stat)) {
            mz_zip_reader_end(&zip);
            return false;
        }
        const std::string nameUtf8(stat.m_filename);
        const int wideLen = MultiByteToWideChar(CP_UTF8, 0, nameUtf8.c_str(), -1, nullptr, 0);
        std::wstring rel(static_cast<size_t>(wideLen), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, nameUtf8.c_str(), -1, rel.data(), wideLen);
        rel.resize(wideLen > 0 ? wideLen - 1 : 0);
        for (wchar_t &c : rel) {
            if (c == L'/')
                c = L'\\';
        }
        // zip 里目录条目可能以 '\' 结尾但没打目录标记（Compress-Archive 就这样），按目录处理
        const bool isDir = stat.m_is_directory || (!rel.empty() && rel.back() == L'\\');
        if (isDir) {
            std::wstring dir = rel;
            while (!dir.empty() && dir.back() == L'\\')
                dir.pop_back();
            if (!dir.empty())
                ensureDir(target + L"\\" + dir);
            continue;
        }
        const std::wstring full = target + L"\\" + rel;
        const size_t slash = full.find_last_of(L'\\');
        if (slash != std::wstring::npos)
            ensureDir(full.substr(0, slash));

        size_t size = 0;
        void *data = mz_zip_reader_extract_to_heap(&zip, i, &size, 0);
        if (!data) {
            logFail(L"extract_to_heap", rel);
            mz_zip_reader_end(&zip);
            return false;
        }
        HANDLE file = CreateFileW(full.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        bool ok = file != INVALID_HANDLE_VALUE;
        if (ok) {
            DWORD written = 0;
            ok = WriteFile(file, data, static_cast<DWORD>(size), &written, nullptr) && written == size;
            CloseHandle(file);
        }
        mz_free(data);
        if (!ok) {
            logFail(L"write_file", rel);
            mz_zip_reader_end(&zip);
            return false;
        }
    }
    mz_zip_reader_end(&zip);
    return true;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    std::vector<unsigned char> payload;
    if (!readPayload(&payload)) {
        MessageBoxW(nullptr, L"内嵌数据损坏，请重新下载。", L"藏笺", MB_ICONERROR);
        return 1;
    }

    const std::wstring root = localAppData() + L"\\Cangjian\\藏笺\\bundle";
    wchar_t key[32] = {0};
    swprintf_s(key, L"%llu", static_cast<unsigned long long>(payload.size()));
    const std::wstring target = root + L"\\" + key;
    const std::wstring stamp = target + L"\\.payload";

    // 已解压过就不再解（用内嵌包大小 + 标记文件判断）
    bool ready = GetFileAttributesW(stamp.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (!ready) {
        ensureDir(target);
        if (!extractAll(payload, target)) {
            MessageBoxW(nullptr, L"解压失败，请检查磁盘空间或杀毒软件拦截。", L"藏笺", MB_ICONERROR);
            return 2;
        }
        HANDLE file = CreateFileW(stamp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE)
            CloseHandle(file);
    }

    const std::wstring exe = target + L"\\cangjian.exe";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + exe + L"\"";
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        target.c_str(), &si, &pi)) {
        MessageBoxW(nullptr, L"启动失败，可能被杀毒软件拦截。", L"藏笺", MB_ICONERROR);
        return 3;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}
