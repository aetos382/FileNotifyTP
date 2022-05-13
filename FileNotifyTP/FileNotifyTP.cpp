#include <cstddef>
#include <functional>
#include <iostream>
#include <locale>
#include <string>

#include <Windows.h>

#include <gsl/gsl>

#include <wil/filesystem.h>
#include <wil/result.h>
#include <wil/resource.h>

static wil::unique_hfile OpenDirectory(
    gsl::not_null<gsl::cwzstring> const & directoryName)
{
    CREATEFILE2_EXTENDED_PARAMETERS params = {};
    params.dwSize = sizeof(CREATEFILE2_EXTENDED_PARAMETERS);
    params.dwFileFlags = FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED;

    auto const dirHandle = CreateFile2(
        directoryName,
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        OPEN_EXISTING,
        &params);

    auto handle = wil::unique_hfile(dirHandle);

    THROW_LAST_ERROR_IF(!handle.is_valid());

    return handle;
}

static wil::slim_event g_event = wil::slim_event(false);

struct CallbackParams
{
    gsl::not_null<HANDLE> FileHandle;
    gsl::span<std::byte> Buffer;
};

static void PrintInfo(
    FILE_NOTIFY_EXTENDED_INFORMATION const & info)
{
    switch (info.Action)
    {
        case FILE_ACTION_ADDED:
            std::wcout << L"Added ";
            break;

        case FILE_ACTION_REMOVED:
            std::wcout << L"Removed ";
            break;

        case FILE_ACTION_MODIFIED:
            std::wcout << L"Modified ";
            break;

        case FILE_ACTION_RENAMED_OLD_NAME:
            std::wcout << L"Renamed from ";
            break;

        case FILE_ACTION_RENAMED_NEW_NAME:
            std::wcout << L"Renamed to ";
            break;
    }

    std::wstring_view fileName(info.FileName, (info.FileNameLength / sizeof(wchar_t)));
    std::wcout << fileName << std::endl;
}

static BOOL StartWatchingDirectoryChange(
    gsl::not_null<PTP_IO> const io,
    gsl::not_null<HANDLE> const directoryHandle,
    gsl::span<std::byte> const buffer,
    gsl::not_null<LPOVERLAPPED> const overlapped)
{
    StartThreadpoolIo(io);

    auto const ok = ReadDirectoryChangesExW(
        directoryHandle,
        buffer.data(),
        gsl::narrow<DWORD>(buffer.size()),
        TRUE,
        FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME,
        nullptr,
        overlapped,
        nullptr,
        ReadDirectoryNotifyExtendedInformation);

    return ok;
}

static VOID CALLBACK IoCompletionCallback(
    PTP_CALLBACK_INSTANCE,
    PVOID context,
    PVOID overlapped,
    ULONG ioResult,
    ULONG_PTR numberOfBytesTransferred,
    PTP_IO io)
{
    if (ioResult != ERROR_SUCCESS || numberOfBytesTransferred == 0)
    {
        g_event.SetEvent();
        return;
    }

    auto const params = static_cast<CallbackParams *>(context);
    auto const info = reinterpret_cast<FILE_NOTIFY_EXTENDED_INFORMATION *>(params->Buffer.data());

    auto iterator = wil::create_next_entry_offset_iterator(info);
    for (auto const & e : iterator)
    {
        PrintInfo(e);
    }

    auto const ok = StartWatchingDirectoryChange(
        io,
        params->FileHandle,
        params->Buffer,
        static_cast<LPOVERLAPPED>(overlapped));

    if (!ok)
    {
        g_event.SetEvent();
    }
}

int wmain(
    int const argc,
    wchar_t * argv[])
{
    if (argc < 2)
    {
        return EXIT_FAILURE;
    }

    std::locale::global(std::locale(""));

    wil::SetResultLoggingCallback(
        [](wil::FailureInfo const & failure) noexcept
        {
            auto message = std::wstring(1024, L'\0');
            wil::GetFailureLogString(message.data(), message.size(), failure);

            std::wcerr << message << std::endl;
        });

    SetConsoleCtrlHandler(
        [] (DWORD) noexcept
        {
            g_event.SetEvent();
            return TRUE;
        },
        TRUE);

    try
    {
        auto const handle = OpenDirectory(argv[1]);

        alignas(sizeof(DWORD)) auto buffer = std::array<std::byte, 1024>();

        OVERLAPPED overlapped = {};

        CallbackParams params = {
            .FileHandle = handle.get(),
            .Buffer = buffer
        };

        auto const io = wil::unique_threadpool_io(CreateThreadpoolIo(
            handle.get(),
            IoCompletionCallback,
            &params,
            nullptr));

        auto const ok = StartWatchingDirectoryChange(
            io.get(),
            handle.get(),
            buffer,
            &overlapped);

        THROW_LAST_ERROR_IF(!ok);

        g_event.wait();
    }
    catch (...)
    {
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
