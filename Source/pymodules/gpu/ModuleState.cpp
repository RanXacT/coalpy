#include "ModuleState.h"
#include <coalpy.core/Assert.h>
#include <coalpy.render/IShaderDb.h>
#include <coalpy.files/IFileSystem.h>
#include <coalpy.tasks/ITaskSystem.h>
#include <coalpy.render/IDevice.h>
#include <coalpy.render/CommandList.h>
#include "CoalpyTypeObject.h"
#include "Window.h"
#include <iostream>

extern coalpy::ModuleOsHandle g_ModuleInstance;

namespace coalpy
{

namespace gpu
{

std::set<ModuleState*> ModuleState::s_allModules;

ModuleState::ModuleState(CoalpyTypeObject** types, int typesCount)
: m_fs(nullptr), m_ts(nullptr)
{
    {
        TaskSystemDesc desc;
        desc.threadPoolSize = 16;
        m_ts = ITaskSystem::create(desc);
    }

    {
        FileSystemDesc desc;
        desc.taskSystem = m_ts;
        m_fs = IFileSystem::create(desc);
    }

    {
        ShaderDbDesc desc;
        desc.resolveOnDestruction = true; //this is so there is a nice clean destruction
        desc.fs = m_fs;
        desc.ts = m_ts;
        desc.enableLiveEditing = true;
        desc.onErrorFn = [this](ShaderHandle handle, const char* shaderName, const char* shaderErrorStr)
        {
            onShaderCompileError(handle, shaderName, shaderErrorStr);
        };
        m_db = IShaderDb::create(desc);
    }

    {
        render::DeviceConfig devConfig;
        devConfig.moduleHandle = g_ModuleInstance;
        devConfig.shaderDb = m_db;
        m_device = render::IDevice::create(devConfig);
    }

    {
        m_windowListener = Window::createWindowListener(*this);
    }

    s_allModules.insert(this);
    registerTypes(types, typesCount);
}

void ModuleState::registerTypes(CoalpyTypeObject** types, int typesCount)
{
    for (auto& t : m_types)
        t = nullptr;

    for (int i = 0; i < (int)TypeId::Counts; ++i)
    {
        CoalpyTypeObject* obj = types[i];
        auto typeIndex = (int)obj->typeId;
        if (typeIndex >= (int)TypeId::Counts)
            continue;

        CPY_ASSERT(m_types[typeIndex] == nullptr);
        m_types[typeIndex] = types[i];
        m_types[typeIndex]->moduleState = this;
    }

    for (int i = 0; i < (int)TypeId::Counts; ++i)
        CPY_ASSERT_MSG(m_types[i] != nullptr, "Missing type");
}

bool ModuleState::checkValidDevice()
{
    if (m_device && m_device->info().valid)
        return true;

    PyErr_SetString(exObj(),
        "Current gpu device used is invalid. "
        "Check coalpy.gpu.get_adapters and select "
        "a valid adapter using coalpy.gpu.set_current_adapter.");

    return false;
}

ModuleState::~ModuleState()
{
    s_allModules.erase(this);
    for (auto w : m_windows)
        w->display = nullptr;

    for (auto* cmdList : m_commandListPool)
        delete cmdList;
    m_commandListPool.clear();

    delete m_windowListener;
    delete m_device;
    delete m_db;
    delete m_fs;
    delete m_ts;
}

void ModuleState::startServices()
{
    m_ts->start();
}

void ModuleState::stopServices()
{
    m_ts->signalStop();
    m_ts->join();
}

bool ModuleState::selectAdapter(int index)
{
    std::vector<render::DeviceInfo> allAdapters;
    render::IDevice::enumerate(render::DevicePlat::Dx12, allAdapters);

    if (index < 0 || index >= (int)allAdapters.size())
    {
        PyErr_SetString(exObj(), "Invalid adapter index selected.");
        return false;
    }

    delete m_device;
    render::DeviceConfig devConfig;
    devConfig.moduleHandle = g_ModuleInstance;
    devConfig.shaderDb = m_db;
    devConfig.index = index;
    m_device = render::IDevice::create(devConfig);
    if (!m_device || !m_device->info().valid)
    {
        PyErr_SetString(exObj(), "Invalid adapter index selected, current gpu device is not valid.");
        return false;
    }

    return true;
}

void ModuleState::onShaderCompileError(ShaderHandle handle, const char* shaderName, const char* shaderErrorString)
{
    std::lock_guard lock(m_shaderErrorMutex);
    std::cerr << "[" << shaderName << "] " << shaderErrorString << std::endl;
}

render::CommandList* ModuleState::newCommandList()
{
    if (m_commandListPool.empty())
    {
        return new render::CommandList();
    }
    else
    {
        auto* cmdList = m_commandListPool.back();
        m_commandListPool.pop_back();
        cmdList->reset();
        return cmdList;
    }
}

void ModuleState::deleteCommandList(render::CommandList* cmdList)
{
    cmdList->reset();
    m_commandListPool.push_back(cmdList);
}

void ModuleState::clean()
{
    for (auto* m : s_allModules)
        delete m;
    
    s_allModules.clear();
}

}
}
