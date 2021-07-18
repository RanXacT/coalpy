#include "CommandList.h"
#include "ModuleState.h"
#include "Shader.h"
#include "CoalpyTypeObject.h"
#include "HelperMacros.h"
#include "Resources.h"
#include "TypeIds.h"
#include <coalpy.render/IDevice.h>
#include <coalpy.render/Resources.h>
#include <coalpy.render/CommandList.h>

namespace coalpy
{
namespace gpu
{

namespace methods
{
    static PyObject* cmdDispatch(PyObject* self, PyObject* vargs, PyObject* kwds);
    static PyObject* cmdCopyResource(PyObject* self, PyObject* vargs, PyObject* kwds);
    static PyObject* cmdUploadResource(PyObject* self, PyObject* vargs, PyObject* kwds);
    static PyObject* cmdDownloadResource(PyObject* self, PyObject* vargs, PyObject* kwds);
}

static PyMethodDef g_cmdListMethods[] = {
    KW_FN(dispatch, cmdDispatch, ""),
    KW_FN(copy_resource, cmdCopyResource, ""),
    KW_FN(upload_resource, cmdUploadResource, ""),
    KW_FN(download_resource, cmdDownloadResource, ""),
    FN_END
};

void CommandList::constructType(PyTypeObject& t)
{
    t.tp_name = "gpu.CommandList";
    t.tp_basicsize = sizeof(CommandList);
    t.tp_doc   = "";
    t.tp_flags = Py_TPFLAGS_DEFAULT;
    t.tp_new = PyType_GenericNew;
    t.tp_init = CommandList::init;
    t.tp_dealloc = CommandList::destroy;
    t.tp_methods = g_cmdListMethods;
}

int CommandList::init(PyObject* self, PyObject * vargs, PyObject* kwds)
{
    ModuleState& moduleState = parentModule(self);
    if (!moduleState.checkValidDevice())
        return -1;

    auto& pycmdList = *((CommandList*)self);
    pycmdList.cmdList = moduleState.newCommandList();

    return 0;
}

void CommandList::destroy(PyObject* self)
{
    ModuleState& moduleState = parentModule(self);
    auto& pycmdList = *((CommandList*)self);
    moduleState.deleteCommandList(pycmdList.cmdList);

    for (auto* r : pycmdList.references)
        Py_DECREF(r);

    pycmdList.~CommandList();
    Py_TYPE(self)->tp_free(self);
}

static bool getListOfBuffers(
    ModuleState& moduleState,
    PyObject* opaqueList,
    std::vector<render::Buffer>& bufferList,
    std::vector<PyObject*>& references)
{
    PyTypeObject* bufferType = moduleState.getType(TypeId::Buffer);
    if (!PyList_Check(opaqueList))
    {
        if (opaqueList->ob_type == bufferType)
        {
            Buffer& buff = *((Buffer*)opaqueList);
            bufferList.push_back(buff.buffer);
            references.push_back(opaqueList);
            return true;
        }
        return false;
    }

    auto& listObj = *((PyListObject*)opaqueList);
    int listSize = Py_SIZE(opaqueList);
    for (int i = 0; i < listSize; ++i)
    {
        PyObject* obj = listObj.ob_item[i];
        if (obj->ob_type != bufferType)
            return false;

        Buffer& buff = *((Buffer*)obj);
        bufferList.push_back(buff.buffer);
        references.push_back(obj);
    }

    return true;
}

template<typename PyTableType, typename TableType>
static bool getListOfTables(
    ModuleState& moduleState,
    PyObject* opaqueList,
    std::vector<TableType>& bufferList,
    std::vector<PyObject*>& references)
{
    PyTypeObject* pyObjType = moduleState.getType(PyTableType::s_typeId);
    if (!PyList_Check(opaqueList))
    {
        if (opaqueList->ob_type == pyObjType)
        {
            PyTableType& tobj = *((PyTableType*)opaqueList);
            bufferList.push_back(tobj.table);
            references.push_back(opaqueList);
            return true;
        }
        return false;
    }

    auto& listObj = *((PyListObject*)opaqueList);
    int listSize = Py_SIZE(opaqueList);
    for (int i = 0; i < listSize; ++i)
    {
        PyObject* obj = listObj.ob_item[i];
        if (obj->ob_type != pyObjType)
            return false;

        PyTableType& tobj = *((PyTableType*)obj);
        bufferList.push_back(tobj.table);
        references.push_back(obj);
    }

    return true;
}

static bool getBufferProtocolObject(
    ModuleState& moduleState,
    PyObject* constants, 
    char*& outBufferProtocolPtr, 
    int& outBufferProtocolSize, 
    std::vector<Py_buffer>& references)
{
    if (!PyObject_CheckBuffer(constants))
        return false;

    references.emplace_back();
    auto& view = references.back();
    if (PyObject_GetBuffer(constants, &view, 0) == -1)
    {
        references.pop_back();
        return false;
    }
    outBufferProtocolPtr = (char*)view.buf;
    outBufferProtocolSize = (int)view.len;
    return true;
}

static bool getArrayOfNums(ModuleState& moduleState, PyObject* constants, std::vector<int>& rawNums)
{
    if (!PyList_Check(constants))
        return false;

    auto& listObj = *((PyListObject*)constants);
    int listSize = Py_SIZE(constants);
    rawNums.reserve(listSize);
    for (int i = 0; i < listSize; ++i)
    {
        PyObject* obj = listObj.ob_item[i];
        if (PyLong_Check(obj))
        {
            rawNums.push_back((int)PyLong_AsLong(obj));
        }
        else if (PyFloat_Check(obj))
        {
            float f = (float)(PyFloat_AS_DOUBLE(obj));
            rawNums.push_back(*reinterpret_cast<int*>((&f)));
        }
        else
            return false;
    }

    return true;
}

namespace methods
{
    PyObject* cmdDispatch(PyObject* self, PyObject* vargs, PyObject* kwds)
    {
        ModuleState& moduleState = parentModule(self);
        static char* arguments[] = { "x", "y", "z", "shader", "name", "constants", "input_tables", "output_tables", nullptr };
        int x = 1;
        int y = 1;
        int z = 1;
        const char* name = nullptr;
        PyObject* shader = nullptr;
        PyObject* constants = nullptr;
        PyObject* input_tables = nullptr;
        PyObject* output_tables = nullptr;
        if (!PyArg_ParseTupleAndKeywords(vargs, kwds, "iiiO|sOOO", arguments, &x, &y, &z, &shader, &name, &constants, &input_tables, &output_tables))
            return nullptr;
        
        if (x <= 0 || y <= 0 || z <= 0)
        {
            PyErr_SetString(moduleState.exObj(), "x, y and z arguments of dispatch must be greater or equal to 1");
            return nullptr;
        }

        auto& cmdList = *(CommandList*)self;

        render::ComputeCommand cmd;
        cmd.setDispatch(name ? name : "", x, y, z);

        //for constants / inline constants. Must be in this scope, because these pointers are needed when writeCommand is called / flushed.
        std::vector<Py_buffer> bufferViews;
        std::vector<int> rawNums;
        std::vector<render::Buffer> bufferList;
        std::vector<render::InResourceTable> inTables;
        std::vector<render::OutResourceTable> outTables;

        PyTypeObject* shaderType = moduleState.getType(Shader::s_typeId);
        if (shader->ob_type != shaderType)
        {
            PyErr_SetString(moduleState.exObj(), "shader parameter must be of type coalpy.gpu.Shader");
            return nullptr;
        }

        Shader& shaderObj = *((Shader*)shader);
        cmdList.references.push_back(shader);
        cmd.setShader(shaderObj.handle);

        if (constants)
        {
            char* bufferProtocolPtr = nullptr;
            int bufferProtocolSize = 0;
            if (getListOfBuffers(moduleState, constants, bufferList, cmdList.references))
            {
                if (!bufferList.empty())
                    cmd.setConstants(bufferList.data(), bufferList.size());
            }
            else if (getBufferProtocolObject(moduleState, constants, bufferProtocolPtr, bufferProtocolSize, bufferViews))
            {
                if (bufferProtocolPtr != nullptr)
                    cmd.setInlineConstant(bufferProtocolPtr, bufferProtocolSize);
            }
            else 
            {
                if (!getArrayOfNums(moduleState, constants, rawNums))
                {
                    PyErr_SetString(moduleState.exObj(), "Constant buffer must be: a list of Buffer objects, an array of [int|float], an array.array() or any object that follows the python Buffer protocol.");
                    return nullptr;
                }

                if (rawNums.empty())
                {
                    PyErr_SetString(moduleState.exObj(), "Constant buffer list cannot be empty");
                    return nullptr;
                }

                cmd.setInlineConstant((const char*)rawNums.data(), (int)rawNums.size() * (int)sizeof(int));
            }
        }

        if (input_tables)
        {
            if (!getListOfTables<InResourceTable, render::InResourceTable>(moduleState, input_tables, inTables, cmdList.references))
            {
                PyErr_SetString(moduleState.exObj(), "input_tables argument must be a list of InResourceTable");
                return nullptr;
            }

            if (!inTables.empty())
            {
                cmd.setInResources(inTables.data(), (int)inTables.size());
            }
        }

        if (output_tables)
        {
            if (!getListOfTables<OutResourceTable, render::OutResourceTable>(moduleState, output_tables, outTables, cmdList.references))
            {
                PyErr_SetString(moduleState.exObj(), "output_tables argument must be a list of OutResourceTable");
                return nullptr;
            }

            if (!outTables.empty())
            {
                cmd.setOutResources(outTables.data(), (int)outTables.size());
            }
        }

        cmdList.cmdList->writeCommand(cmd);

        for (auto* r : cmdList.references)
            Py_INCREF(r);

        for (auto& v : bufferViews)
            PyBuffer_Release(&v);

        Py_RETURN_NONE;
    }

    PyObject* cmdCopyResource(PyObject* self, PyObject* vargs, PyObject* kwds)
    {
        Py_RETURN_NONE;
    }

    PyObject* cmdUploadResource(PyObject* self, PyObject* vargs, PyObject* kwds)
    {
        Py_RETURN_NONE;
    }

    PyObject* cmdDownloadResource(PyObject* self, PyObject* vargs, PyObject* kwds)
    {
        Py_RETURN_NONE;
    }
}

}
}
