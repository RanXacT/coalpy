#include "FileSystem.h"
#include <coalpy.tasks/ITaskSystem.h>
#include <coalpy.core/Assert.h>

#ifdef _WIN32 
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <coalpy.core/ClTokenizer.h>
#endif

namespace coalpy
{

#ifdef _WIN32
namespace InternalFileSystem
{
    enum {
        bufferSize = 16 * 1024 //16kb buffer size
    };

    struct WindowsFile
    {
        HANDLE h;
        unsigned int fileSize;
        OVERLAPPED overlapped;
        char buffer[bufferSize];
    };

    bool valid(FileSystem::OpaqueFileHandle h)
    {
        return h != nullptr;
    }

    FileSystem::OpaqueFileHandle openFile(const char* filename, FileSystem::RequestType request)
    {
        HANDLE h = CreateFileA(
            filename, //file name
            request == FileSystem::RequestType::Read ? GENERIC_READ : GENERIC_WRITE, //dwDesiredAccess
            0u, //dwShareMode
            NULL, //lpSecurityAttributes
            request == FileSystem::RequestType::Read ? OPEN_EXISTING : CREATE_ALWAYS,//dwCreationDisposition
            FILE_FLAG_OVERLAPPED, //dwFlagsAndAttributes
            NULL); //template attribute

        if (h == INVALID_HANDLE_VALUE)
            return nullptr;

        auto* wf = new WindowsFile;
        wf->h = h;
        wf->fileSize = GetFileSize(wf->h, NULL);
        wf->overlapped = {};
        wf->overlapped.hEvent = CreateEvent(
            NULL, //default security attribute
            TRUE, //manual reset event
            FALSE, //initial state = signaled
            NULL);//unnamed

        return (FileSystem::OpaqueFileHandle)wf;
    }

    bool readBytes(FileSystem::OpaqueFileHandle h, char*& outputBuffer, int& bytesRead, bool& isEof)
    {
        CPY_ASSERT(h != nullptr);
        isEof = false;
        if (h == nullptr)
            return false;

        auto* wf = (WindowsFile*)h;
        CPY_ASSERT(wf->h != INVALID_HANDLE_VALUE);

        DWORD dwordBytesRead;
        bool result = ReadFile(
            wf->h,
            wf->buffer,
            bufferSize,
            &dwordBytesRead,
            &wf->overlapped);

        if (!result)
        {
            auto dwError = GetLastError();
            switch (dwError)
            {
            case ERROR_HANDLE_EOF:
                {
                    isEof = true;
                    result = true;
                    break;
                }
            case ERROR_IO_PENDING:
                {
                    bool overlappedSuccess = GetOverlappedResult(wf->h,
                                                  &wf->overlapped,
                                                  &dwordBytesRead,
                                                  TRUE); 
                    if (!overlappedSuccess)
                    {
                        switch (dwError = GetLastError())
                        {
                        case ERROR_HANDLE_EOF:  
                            isEof = true;
                            result = true;
                            break;
                        case ERROR_IO_INCOMPLETE:
                            result = true;
                            break;
                        default:
                            result = false;
                            break;
                        }
                    }
                    else
                    {
                        result = true;
                        ResetEvent(wf->overlapped.hEvent);
                    }

                    break;
                }
            default:
                //file IO issue occured here
                result = false;
            }
        }

        if (result)
            wf->overlapped.Offset += dwordBytesRead;
        if (wf->overlapped.Offset >= wf->fileSize)
            isEof = true;

        bytesRead = (int)dwordBytesRead;
        outputBuffer = wf->buffer;
        return result;
    }

    void close(FileSystem::OpaqueFileHandle h)
    {
        CPY_ASSERT(h != nullptr);
        auto* wf = (WindowsFile*)h;
        CPY_ASSERT(wf->h != INVALID_HANDLE_VALUE);
        CloseHandle(wf->h);
        CloseHandle(wf->overlapped.hEvent);
        delete wf;
    }

    void fixStringPath(std::string& str)
    {
        for (auto& c : str)
        {
            if (c == '/')
                c = '\\';
        }
    }

    void getDirectoryList(const std::string& filePath, std::vector<std::string>& outDirectories, std::string& file)
    {
        auto dirCandidates = ClTokenizer::splitString(filePath, '\\');
        for (auto d : dirCandidates)
            if (d != "")
                outDirectories.push_back(d);

        if (outDirectories.empty())
            return;

        file = outDirectories.back();
        outDirectories.pop_back();
    }

    bool createDirectory(const char* str)
    {
        return CreateDirectoryA(str, nullptr);
    }

    bool deleteDirectory(const char* str)
    {
        return RemoveDirectoryA(str);
    }

    bool deleteFile(const char* str)
    {
        return DeleteFile(str);
    }

    void getAttributes(const std::string& dirName_in, bool& exists, bool& isDir)
    {
        exists = false;
        isDir = false;
        DWORD ftyp = GetFileAttributesA(dirName_in.c_str());
        if (ftyp == INVALID_FILE_ATTRIBUTES)
        {
            exists = false;
            isDir = false;
            return;
        }
        
        exists = true;
        if (ftyp & FILE_ATTRIBUTE_DIRECTORY)
        {
            isDir = true;
            return;
        }
        
        return;
    }

    bool carveFile(const std::string& path, bool lastIsFile = true)
    {
        bool exists, isDir;
        getAttributes(path, exists, isDir);
        if (exists)
            return lastIsFile ? !isDir : isDir;

        //ok so the path doesnt really exists, lets carve it.
        std::string filename;
        std::vector<std::string> directories;
        getDirectoryList(path, directories, filename);
        if (filename == "")
            return false;

        if (!lastIsFile)
            directories.push_back(filename);

        if (!directories.empty())
        {
            std::stringstream ss;
            for (auto& d : directories)
            {
                ss << d << "/";
                auto currentPath = ss.str();
                getAttributes(currentPath, exists, isDir);
                if (exists && !isDir)
                    return false;

                if (!exists && !createDirectory(currentPath.c_str()))
                    return false;
            }
        }

        return true;
    }
};
#else
    #error "Platform not supported"
#endif

FileSystem::FileSystem(const FileSystemDesc& desc)
: m_desc(desc)
, m_ts(*desc.taskSystem)
{
    
}

FileSystem::~FileSystem()
{
}

AsyncFileHandle FileSystem::read(const FileReadRequest& request)
{
    CPY_ASSERT_MSG(request.doneCallback, "File read request must provide a done callback.");

    AsyncFileHandle asyncHandle;
    Task task;
    
    {
        std::unique_lock lock(m_requestsMutex);
        Request*& requestData = m_requests.allocate(asyncHandle);
        requestData = new Request();
        requestData->type = RequestType::Read;
        requestData->filename = request.path;
        requestData->callback = request.doneCallback;
        requestData->opaqueHandle = {};
        requestData->fileStatus = FileStatus::Reading;
        requestData->task = m_ts.createTask(TaskDesc([this](TaskContext& ctx)
        {
            auto* requestData = (Request*)ctx.data;
            {
                requestData->fileStatus = FileStatus::Opening;
                FileReadResponse response;
                response.status = FileStatus::Opening;
                requestData->callback(response);
            }
            requestData->opaqueHandle = InternalFileSystem::openFile(requestData->filename.c_str(), RequestType::Read);
            if (!InternalFileSystem::valid(requestData->opaqueHandle))
            {
                {
                    requestData->fileStatus = FileStatus::OpenFail;
                    FileReadResponse response;
                    response.status = FileStatus::OpenFail;
                    requestData->callback(response);
                }
                return;
            }

            struct ReadState {
                char* output = nullptr;
                int bytesRead = 0;
                bool isEof = false;
                bool successRead = false;
            } readState;
            while (!readState.isEof)
            {
                TaskUtil::yieldUntil([&readState, requestData]() {
                    readState.successRead = InternalFileSystem::readBytes(
                        requestData->opaqueHandle, readState.output, readState.bytesRead, readState.isEof);
                });

                if (!readState.successRead)
                {
                    {
                        requestData->fileStatus = FileStatus::ReadingFail;
                        FileReadResponse response;
                        response.status = FileStatus::ReadingFail;
                        requestData->callback(response);
                    }
                    return;
                }
            }

            {
                requestData->fileStatus = FileStatus::ReadingSuccess;
                FileReadResponse response;
                response.status = FileStatus::ReadingSuccess;
                requestData->callback(response);
            }

        }), requestData);
        task = requestData->task;
    }

    m_ts.execute(task);
    return asyncHandle;
}

AsyncFileHandle FileSystem::write(const FileWriteRequest& request)
{
    return AsyncFileHandle();
}


void FileSystem::wait(AsyncFileHandle handle)
{
    Task task;
    {
        std::unique_lock lock(m_requestsMutex);
        Request* r = m_requests[handle];
        task = r->task;
    }

    m_ts.wait(task);
}

bool FileSystem::readStatus (AsyncFileHandle handle, FileReadResponse& response)
{
    return false;
}

bool FileSystem::writeStatus(AsyncFileHandle handle, FileWriteResponse& response)
{
    return false;
}

void FileSystem::closeHandle(AsyncFileHandle handle)
{
}

bool FileSystem::carveDirectoryPath(const char* directoryName)
{
    std::string dir = directoryName;
    InternalFileSystem::fixStringPath(dir);
    return InternalFileSystem::carveFile(dir, false);
}

bool FileSystem::enumerateFiles(std::vector<std::string>& dirList)
{
    return false;
}

bool FileSystem::deleteDirectory(const char* directoryName)
{
    return InternalFileSystem::deleteDirectory(directoryName);
}

bool FileSystem::deleteFile(const char* fileName)
{
    return InternalFileSystem::deleteFile(fileName);
}

IFileSystem* IFileSystem::create(const FileSystemDesc& desc)
{
    return new FileSystem(desc);
}

}