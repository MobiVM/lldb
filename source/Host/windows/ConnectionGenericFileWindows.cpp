//===-- ConnectionGenericFileWindows.cpp ------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "lldb/Core/Error.h"
#include "lldb/Core/Log.h"
#include "lldb/Host/TimeValue.h"
#include "lldb/Host/windows/ConnectionGenericFileWindows.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringRef.h"

using namespace lldb;
using namespace lldb_private;

namespace
{
// This is a simple helper class to package up the information needed to return from a Read/Write
// operation function.  Since there is alot of code to be run before exit regardless of whether the
// operation succeeded or failed, combined with many possible return paths, this is the cleanest
// way to represent it.
class ReturnInfo
{
  public:
    void
    Set(size_t bytes, ConnectionStatus status, DWORD error_code)
    {
        m_error.SetError(error_code, eErrorTypeWin32);
        m_bytes = bytes;
        m_status = status;
    }

    void
    Set(size_t bytes, ConnectionStatus status, llvm::StringRef error_msg)
    {
        m_error.SetErrorString(error_msg.data());
        m_bytes = bytes;
        m_status = status;
    }

    size_t
    GetBytes() const
    {
        return m_bytes;
    }
    ConnectionStatus
    GetStatus() const
    {
        return m_status;
    }
    const Error &
    GetError() const
    {
        return m_error;
    }

  private:
    Error m_error;
    size_t m_bytes;
    ConnectionStatus m_status;
};
}

ConnectionGenericFile::ConnectionGenericFile()
    : m_file(INVALID_HANDLE_VALUE)
    , m_owns_file(false)
{
    ::ZeroMemory(&m_overlapped, sizeof(m_overlapped));
    ::ZeroMemory(&m_file_position, sizeof(m_file_position));
    InitializeEventHandles();
}

ConnectionGenericFile::ConnectionGenericFile(lldb::file_t file, bool owns_file)
    : m_file(file)
    , m_owns_file(owns_file)
{
    ::ZeroMemory(&m_overlapped, sizeof(m_overlapped));
    ::ZeroMemory(&m_file_position, sizeof(m_file_position));
    InitializeEventHandles();
}

ConnectionGenericFile::~ConnectionGenericFile()
{
    if (m_owns_file && IsConnected())
        ::CloseHandle(m_file);

    ::CloseHandle(m_event_handles[kBytesAvailableEvent]);
    ::CloseHandle(m_event_handles[kInterruptEvent]);
}

void
ConnectionGenericFile::InitializeEventHandles()
{
    m_event_handles[kInterruptEvent] = CreateEvent(NULL, FALSE, FALSE, NULL);

    // Note, we should use a manual reset event for the hEvent argument of the OVERLAPPED.  This
    // is because both WaitForMultipleObjects and GetOverlappedResult (if you set the bWait
    // argument to TRUE) will wait for the event to be signalled.  If we use an auto-reset event,
    // WaitForMultipleObjects will reset the event, return successfully, and then
    // GetOverlappedResult will block since the event is no longer signalled.
    m_event_handles[kBytesAvailableEvent] = ::CreateEvent(NULL, TRUE, FALSE, NULL);
}

bool
ConnectionGenericFile::IsConnected() const
{
    return m_file && (m_file != INVALID_HANDLE_VALUE);
}

lldb::ConnectionStatus
ConnectionGenericFile::Connect(const char *s, Error *error_ptr)
{
    Log *log(lldb_private::GetLogIfAnyCategoriesSet(LIBLLDB_LOG_CONNECTION));
    if (log)
        log->Printf("%p ConnectionGenericFile::Connect (url = '%s')", static_cast<void *>(this), s);

    if (strstr(s, "file://") != s)
    {
        if (error_ptr)
            error_ptr->SetErrorStringWithFormat("unsupported connection URL: '%s'", s);
        return eConnectionStatusError;
    }

    if (IsConnected())
    {
        ConnectionStatus status = Disconnect(error_ptr);
        if (status != eConnectionStatusSuccess)
            return status;
    }

    // file://PATH
    const char *path = s + strlen("file://");
    // Open the file for overlapped access.  If it does not exist, create it.  We open it overlapped
    // so that we can issue asynchronous reads and then use WaitForMultipleObjects to allow the read
    // to be interrupted by an event object.
    m_file = ::CreateFile(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_FLAG_OVERLAPPED, NULL);
    if (m_file == INVALID_HANDLE_VALUE)
    {
        if (error_ptr)
            error_ptr->SetError(::GetLastError(), eErrorTypeWin32);
        return eConnectionStatusError;
    }

    m_owns_file = true;
    return eConnectionStatusSuccess;
}

lldb::ConnectionStatus
ConnectionGenericFile::Disconnect(Error *error_ptr)
{
    Log *log(lldb_private::GetLogIfAnyCategoriesSet(LIBLLDB_LOG_CONNECTION));
    if (log)
        log->Printf("%p ConnectionGenericFile::Disconnect ()", static_cast<void *>(this));

    if (!IsConnected())
        return eConnectionStatusSuccess;

    // Reset the handle so that after we unblock any pending reads, subsequent calls to Read() will
    // see a disconnected state.
    HANDLE old_file = m_file;
    m_file = INVALID_HANDLE_VALUE;

    // Set the disconnect event so that any blocking reads unblock, then cancel any pending IO operations.
    ::CancelIoEx(old_file, &m_overlapped);

    // Close the file handle if we owned it, but don't close the event handles.  We could always
    // reconnect with the same Connection instance.
    if (m_owns_file)
        ::CloseHandle(old_file);

    ::ZeroMemory(&m_file_position, sizeof(m_file_position));
    m_owns_file = false;
    return eConnectionStatusSuccess;
}

size_t
ConnectionGenericFile::Read(void *dst, size_t dst_len, uint32_t timeout_usec, lldb::ConnectionStatus &status, Error *error_ptr)
{
    ReturnInfo return_info;
    BOOL result = 0;
    DWORD bytes_read = 0;

    if (error_ptr)
        error_ptr->Clear();

    if (!IsConnected())
    {
        return_info.Set(0, eConnectionStatusNoConnection, ERROR_INVALID_HANDLE);
        goto finish;
    }

    m_overlapped.hEvent = m_event_handles[kBytesAvailableEvent];

    result = ::ReadFile(m_file, dst, dst_len, NULL, &m_overlapped);
    if (result || ::GetLastError() == ERROR_IO_PENDING)
    {
        if (!result)
        {
            // The expected return path.  The operation is pending.  Wait for the operation to complete
            // or be interrupted.
            TimeValue time_value;
            time_value.OffsetWithMicroSeconds(timeout_usec);
            DWORD milliseconds = time_value.milliseconds();
            result = ::WaitForMultipleObjects(llvm::array_lengthof(m_event_handles), m_event_handles, FALSE, milliseconds);
            // All of the events are manual reset events, so make sure we reset them to non-signalled.
            switch (result)
            {
                case WAIT_OBJECT_0 + kBytesAvailableEvent:
                    break;
                case WAIT_OBJECT_0 + kInterruptEvent:
                    return_info.Set(0, eConnectionStatusInterrupted, 0);
                    goto finish;
                case WAIT_TIMEOUT:
                    return_info.Set(0, eConnectionStatusTimedOut, 0);
                    goto finish;
                case WAIT_FAILED:
                    return_info.Set(0, eConnectionStatusError, ::GetLastError());
                    goto finish;
            }
        }
        // The data is ready.  Figure out how much was read and return;
        if (!::GetOverlappedResult(m_file, &m_overlapped, &bytes_read, FALSE))
        {
            DWORD result_error = ::GetLastError();
            // ERROR_OPERATION_ABORTED occurs when someone calls Disconnect() during a blocking read.
            // This triggers a call to CancelIoEx, which causes the operation to complete and the
            // result to be ERROR_OPERATION_ABORTED.
            if (result_error == ERROR_HANDLE_EOF || result_error == ERROR_OPERATION_ABORTED || result_error == ERROR_BROKEN_PIPE)
                return_info.Set(bytes_read, eConnectionStatusEndOfFile, 0);
            else
                return_info.Set(bytes_read, eConnectionStatusError, result_error);
        }
        else if (bytes_read == 0)
            return_info.Set(bytes_read, eConnectionStatusEndOfFile, 0);
        else
            return_info.Set(bytes_read, eConnectionStatusSuccess, 0);

        goto finish;
    }
    else if (::GetLastError() == ERROR_BROKEN_PIPE)
    {
        // The write end of a pipe was closed.  This is equivalent to EOF.
        return_info.Set(0, eConnectionStatusEndOfFile, 0);
    }
    else
    {
        // An unknown error occured.  Fail out.
        return_info.Set(0, eConnectionStatusError, ::GetLastError());
    }
    goto finish;

finish:
    status = return_info.GetStatus();
    if (error_ptr)
        *error_ptr = return_info.GetError();

    // kBytesAvailableEvent is a manual reset event.  Make sure it gets reset here so that any
    // subsequent operations don't immediately see bytes available.
    ResetEvent(m_event_handles[kBytesAvailableEvent]);

    IncrementFilePointer(return_info.GetBytes());
    Log *log(lldb_private::GetLogIfAnyCategoriesSet(LIBLLDB_LOG_CONNECTION));
    if (log)
    {
        log->Printf("%" PRIxPTR " ConnectionGenericFile::Read()  handle = %" PRIxPTR ", dst = %" PRIxPTR ", dst_len = %" PRIu64
                    ") => %" PRIu64 ", error = %s",
                    this, m_file, dst, static_cast<uint64_t>(dst_len), static_cast<uint64_t>(return_info.GetBytes()),
                    return_info.GetError().AsCString());
    }

    return return_info.GetBytes();
}

size_t
ConnectionGenericFile::Write(const void *src, size_t src_len, lldb::ConnectionStatus &status, Error *error_ptr)
{
    ReturnInfo return_info;
    DWORD bytes_written = 0;
    BOOL result = 0;

    if (error_ptr)
        error_ptr->Clear();

    if (!IsConnected())
    {
        return_info.Set(0, eConnectionStatusNoConnection, ERROR_INVALID_HANDLE);
        goto finish;
    }

    m_overlapped.hEvent = NULL;

    // Writes are not interruptible like reads are, so just block until it's done.
    result = ::WriteFile(m_file, src, src_len, NULL, &m_overlapped);
    if (!result && ::GetLastError() != ERROR_IO_PENDING)
    {
        return_info.Set(0, eConnectionStatusError, ::GetLastError());
        goto finish;
    }

    if (!::GetOverlappedResult(m_file, &m_overlapped, &bytes_written, TRUE))
    {
        return_info.Set(bytes_written, eConnectionStatusError, ::GetLastError());
        goto finish;
    }

    return_info.Set(bytes_written, eConnectionStatusSuccess, 0);
    goto finish;

finish:
    status = return_info.GetStatus();
    if (error_ptr)
        *error_ptr = return_info.GetError();

    IncrementFilePointer(return_info.GetBytes());
    Log *log(lldb_private::GetLogIfAnyCategoriesSet(LIBLLDB_LOG_CONNECTION));
    if (log)
    {
        log->Printf("%" PRIxPTR " ConnectionGenericFile::Write()  handle = %" PRIxPTR ", src = %" PRIxPTR ", src_len = %" PRIu64
                    ") => %" PRIu64 ", error = %s",
                    this, m_file, src, static_cast<uint64_t>(src_len), static_cast<uint64_t>(return_info.GetBytes()),
                    return_info.GetError().AsCString());
    }
    return return_info.GetBytes();
}

bool
ConnectionGenericFile::InterruptRead()
{
    return ::SetEvent(m_event_handles[kInterruptEvent]);
}

void
ConnectionGenericFile::IncrementFilePointer(DWORD amount)
{
    LARGE_INTEGER old_pos;
    old_pos.HighPart = m_overlapped.OffsetHigh;
    old_pos.LowPart = m_overlapped.Offset;
    old_pos.QuadPart += amount;
    m_overlapped.Offset = old_pos.LowPart;
    m_overlapped.OffsetHigh = old_pos.HighPart;
}
