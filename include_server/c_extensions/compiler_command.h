/* The native helpers consume the same command codec as the C client. */
#ifndef DCC_COMPILER_COMMAND_H
#define DCC_COMPILER_COMMAND_H
#include <Python.h>
#include "../../src/argv_file.h"

class CompilerCommand {
    const dcc_argv_file_api &api;
    dcc_argv_file file{};
public:
    explicit CompilerCommand(const dcc_argv_file_api &api) : api(api) {}
    CompilerCommand(const CompilerCommand &) = delete;
    CompilerCommand &operator=(const CompilerCommand &) = delete;
    ~CompilerCommand() { api.close(&file); }
    bool read(int fd, unsigned groups) {
        return !api.read(fd, groups, &file);
    }
    char **operator[](unsigned group) const { return file.groups[group]; }
    static const dcc_argv_file_api *load() {
        PyObject *module = PyImport_ImportModule("include_server.distcc_pump_c_extensions");
        if (!module) return nullptr;
        PyObject *capsule = PyObject_GetAttrString(module, "argv_file");
        Py_DECREF(module);
        if (!capsule) return nullptr;
        auto api = static_cast<const dcc_argv_file_api *>(PyCapsule_GetPointer(capsule, "distcc.argv_file"));
        Py_DECREF(capsule);
        return api;
    }
};
#endif
