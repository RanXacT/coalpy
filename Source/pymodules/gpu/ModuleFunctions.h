#pragma once

#define PY_SSIZE_T_CLEAN
#include <Python.h>

namespace coalpy
{

namespace gpu
{

namespace methods
{

PyMethodDef* get();

//Module functions
void freeModule(void* modulePtr); 

// coalpy::gpu
PyObject* loadShader(PyObject* self, PyObject* args, PyObject* kwds);
PyObject* inlineShader(PyObject* self, PyObject* args, PyObject* kwds);

}

}

}
