#pragma once
#include <coalpy.core/GenericHandle.h>
#include <string>

namespace coalpy
{
namespace render
{

struct WorkHandle : GenericHandle<unsigned int> { };

enum class ScheduleErrorType
{
    BadTableInfo,
    ResourceStateNotFound,
    CorruptedCommandListSentinel,
    Ok
};

enum class WaitErrorType
{
    Ok
};

enum class DownloadResult
{
    Ok,
    NotReady,
    Invalid
};

enum ScheduleFlags : int
{
    ScheduleFlags_None = 0,
    ScheduleFlags_GetWorkHandle = 1 << 0,
};

struct ScheduleStatus
{
    bool success() const { return type == ScheduleErrorType::Ok; }
    WorkHandle workHandle;
    ScheduleErrorType type;
    std::string message;
};

struct WaitStatus
{
    bool success() const { return type == WaitErrorType::Ok; }
    WaitErrorType type;
    std::string message;
};

struct DownloadStatus
{
    bool success() const { return result == DownloadResult::Ok; }

    DownloadResult result;
    void* downloadPtr = nullptr;
    int downloadByteSize = 0;
};



}
}
