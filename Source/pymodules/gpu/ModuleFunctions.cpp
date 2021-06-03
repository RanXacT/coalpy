#include "ModuleFunctions.h"
#include "ModuleState.h"
#include <coalpy.files/IFileSystem.h>
#include <coalpy.files/Utils.h>
#include <coalpy.render/IShaderDb.h>
#include <string>

#define PY_SSIZE_T_CLEAN
#include <Python.h>

namespace {

#define KW_FN(name, desc) \
    { #name, (PyCFunction)(coalpy::gpu::methods::##name), METH_VARARGS | METH_KEYWORDS, desc }

#define VA_FN(name, desc) \
    { #name, (coalpy::gpu::methods::##name), METH_VARARGS, desc }

#define FN_END \
    {NULL, NULL, 0, NULL}

PyMethodDef g_defs[] = {
    KW_FN(loadShader,   "Loads a shader from a file"),
    KW_FN(inlineShader, "Create an inline shader"),
    FN_END
};

coalpy::gpu::ModuleState& getState(PyObject* module)
{
    return *((coalpy::gpu::ModuleState*)PyModule_GetState(module));
}

}

namespace coalpy
{
namespace gpu
{
namespace methods
{

PyMethodDef* get()
{
    return g_defs;
}

void freeModule(void* modulePtr)
{
    PyObject* moduleObj = (PyObject*)modulePtr;
    auto state = (ModuleState*)PyModule_GetState(moduleObj);
    if (state)
        state->~ModuleState();
}

PyObject* loadShader(PyObject* self, PyObject* args, PyObject* kwds)
{
    static char* argnames[] = { "fileName", "mainFunction", "shaderName" };
    ModuleState& state = getState(self);
    const char* fileName = nullptr;
    const char* mainFunction = "csMain";
    const char* shaderName = nullptr;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "s|ss", argnames, &fileName, &mainFunction, &shaderName))
    {
        PyErr_SetString(state.exObj(), "failed loading arguments for loadShader.");
        return nullptr;
    }

    std::string sshaderName = shaderName ? shaderName : "";
    if (sshaderName == "")
    {
        std::string filePath = fileName;
        FileUtils::getFileName(filePath, sshaderName);
    }
    
    ShaderDesc desc;
    desc.type = ShaderType::Compute;
    desc.name = sshaderName.c_str();
    desc.mainFn = mainFunction;
    desc.path = fileName;
    ShaderHandle handle = state.db().requestCompile(desc);
    Py_RETURN_NONE;
}

PyObject* inlineShader(PyObject* self, PyObject* args, PyObject* kwds)
{
    Py_RETURN_NONE;
}

}
}
}
