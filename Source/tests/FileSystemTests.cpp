#include "testsystem.h"
#include <coalpy.core/Assert.h>
#include <coalpy.tasks/ITaskSystem.h>
#include <coalpy.files/IFileSystem.h>
#include <iostream>

namespace coalpy
{

class FileSystemContext : public TestContext
{
public:
    ITaskSystem* ts = nullptr;
    IFileSystem* fs = nullptr;

    void begin()
    {
        ts->start();
    }

    void end()
    {
        ts->signalStop();
        ts->join();
        ts->cleanFinishedTasks();
    }
};

void testFileRead(TestContext& ctx)
{
    auto& testContext = (FileSystemContext&)ctx;
    testContext.begin();

    IFileSystem& fs = *testContext.fs;
    
    bool success = false;
    AsyncFileHandle h = fs.read(FileReadRequest {
        "test.txt",
        [&success](FileReadResponse& response)
        {
            if (response.status == FileStatus::ReadingSuccess)
                success = true;
        }
    });

    fs.wait(h);

    CPY_ASSERT(success);
    
    testContext.end();
}

class FileSystemTestSuite : public TestSuite
{
public:
    virtual const char* name() const { return "filesystem"; }
    virtual const TestCase* getCases(int& caseCounts) const
    {
        static TestCase sCases[] = {
            { "fileRead", testFileRead }
        };

        caseCounts = (int)(sizeof(sCases) / sizeof(TestCase));
        return sCases;
    }

    virtual TestContext* createContext()
    {
        auto testContext = new FileSystemContext();

        {
            TaskSystemDesc desc;
            desc.threadPoolSize = 8;
            testContext->ts = ITaskSystem::create(desc);
        }

        {
            FileSystemDesc desc { testContext->ts };
            testContext->fs = IFileSystem::create(desc);
        }

        return testContext;
    }

    virtual void destroyContext(TestContext* context)
    {
        auto testContext = static_cast<FileSystemContext*>(context);
        delete testContext;
    }
};

TestSuite* fileSystemSuite()
{
    return new FileSystemTestSuite;
}

}
