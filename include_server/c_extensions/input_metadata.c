/* Compiler input metadata, not preprocessing syntax. SPDX-License-Identifier: GPL-2.0-or-later */
#include <Python.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static PyObject *stamp(const char *text)
{
    unsigned long long values[4];
    long long times[2];
    char *end;
    for (int i = 0; i < 6; ++i) {
        errno = 0;
        if (i < 4) {
            if (*text < '0' || *text > '9') goto malformed;
            values[i] = strtoull(text, &end, 10);
        } else {
            times[i - 4] = strtoll(text, &end, 10);
        }
        if (errno || end == text || *end != (i == 5 ? '\0' : ' ')) goto malformed;
        text = end + (i < 5);
    }
    if (S_ISDIR(values[2]))
        return Py_BuildValue("(KKK)", values[0], values[1], values[2]);
    return Py_BuildValue("(KKKKLL)", values[0], values[1], values[2], values[3], times[0], times[1]);
malformed:
    PyErr_SetString(PyExc_ValueError, "Malformed GCC dependency metadata");
    return NULL;
}

PyObject *dcc_py_parse_gcc_inputs(PyObject *self, PyObject *args)
{
    const char *data, *cursor, *end;
    Py_ssize_t length;
    PyObject *cwd, *prefix = NULL, *lookups = NULL, *directories = NULL, *files = NULL;
    PyObject *path = NULL, *value = NULL, *result = NULL;
    (void)self;
    if (!PyArg_ParseTuple(args, "y#U", &data, &length, &cwd)) return NULL;
    if (length < 7 || memcmp(data, "DCGS2", 6) || data[length - 1]) {
        PyErr_SetString(PyExc_ValueError, "Incomplete or unsupported GCC dependency scan");
        return NULL;
    }
    prefix = PyUnicode_GetLength(cwd) && PyUnicode_ReadChar(cwd, PyUnicode_GetLength(cwd) - 1) == '/'
             ? Py_NewRef(cwd) : PyUnicode_FromFormat("%U/", cwd);
    lookups = PyDict_New();
    directories = PyDict_New();
    files = PyList_New(0);
    if (!prefix || !lookups || !directories || !files) goto done;
    cursor = data + 6;
    end = data + length - 1;
    while (cursor < end) {
        const char *name_end = memchr(cursor, 0, end - cursor);
        const char *metadata, *metadata_end;
        if (!name_end || name_end == cursor) goto malformed;
        metadata = name_end + 1;
        metadata_end = memchr(metadata, 0, end - metadata);
        if (!metadata_end) goto malformed;
        path = PyUnicode_DecodeFSDefaultAndSize(cursor, name_end - cursor);
        if (!path) goto done;
        if (*cursor != '/') {
            PyObject *absolute = PyUnicode_Concat(prefix, path);
            Py_SETREF(path, absolute);
            if (!path) goto done;
        }
        if (metadata_end - metadata == 2 && metadata[0] == 'D' &&
            metadata[1] >= '0' && metadata[1] <= '2') {
            value = PyLong_FromLong(metadata[1] - '0');
            if (!value || PyDict_SetItem(directories, path, value)) goto done;
        } else {
            value = metadata_end - metadata == 1 && *metadata == '-' ? Py_NewRef(Py_None) : stamp(metadata);
            if (!value) goto done;
            PyObject *previous = PyDict_GetItemWithError(lookups, path);
            if (previous) {
                int equal = PyObject_RichCompareBool(previous, value, Py_EQ);
                if (equal < 0) goto done;
                if (!equal) {
                    PyErr_Format(PyExc_ValueError, "Input changed during GCC dependency scan: %U", path);
                    goto done;
                }
            } else if (PyErr_Occurred() || PyDict_SetItem(lookups, path, value)) goto done;
        }
        Py_CLEAR(path);
        Py_CLEAR(value);
        cursor = metadata_end + 1;
    }
    Py_ssize_t index = 0;
    PyObject *key, *entry;
    while (PyDict_Next(lookups, &index, &key, &entry)) {
        if (entry == Py_None || PyTuple_GET_SIZE(entry) == 3) continue;
        if (PyList_Append(files, key)) goto done;
    }
    if (!PyList_GET_SIZE(files)) {
        PyErr_SetString(PyExc_ValueError, "GCC dependency scan returned no inputs");
        goto done;
    }
    result = Py_BuildValue("{s:O,s:O,s:O,s:O}", "files", Py_None, "dependencies", files,
                           "lookups", lookups, "directories", directories);
    goto done;
malformed:
    PyErr_SetString(PyExc_ValueError, "Malformed GCC dependency scan");
done:
    Py_XDECREF(path);
    Py_XDECREF(value);
    Py_XDECREF(prefix);
    Py_XDECREF(lookups);
    Py_XDECREF(directories);
    Py_XDECREF(files);
    return result;
}

/* Keep cache ownership and watch installation in Python. Return only entries
 * requiring observation or generation validation, plus current positive paths.
 * This also serves Clang's native scanner and never retains a second cache. */
PyObject *dcc_py_input_changes(PyObject *self, PyObject *args)
{
    PyObject *lookups, *observed, *positive = NULL, *changed = NULL, *result = NULL;
    PyObject *path, *expected;
    Py_ssize_t index = 0;
    (void)self;
    if (!PyArg_ParseTuple(args, "O!O!", &PyDict_Type, &lookups, &PyDict_Type, &observed)) return NULL;
    positive = PyList_New(0);
    changed = PyDict_New();
    if (!positive || !changed) goto done;
    while (PyDict_Next(lookups, &index, &path, &expected)) {
        if (expected != Py_None && PyList_Append(positive, path)) goto done;
        PyObject *key = PyTuple_Pack(2, path, Py_False);
        if (!key) goto done;
        PyObject *previous = PyDict_GetItemWithError(observed, key);
        Py_DECREF(key);
        if (PyErr_Occurred()) goto done;
        int equal = 0;
        if (previous) {
            if (expected == Py_None && previous != Py_None) {
                PyObject *mode = PyTuple_GetItem(previous, 2);
                if (!mode) goto done;
                unsigned long bits = PyLong_AsUnsignedLong(mode);
                if (PyErr_Occurred()) goto done;
                equal = S_ISDIR(bits);
            } else equal = PyObject_RichCompareBool(previous, expected, Py_EQ);
            if (equal < 0) goto done;
        }
        if (!equal && PyDict_SetItem(changed, path, expected)) goto done;
    }
    result = PyTuple_Pack(2, positive, changed);
done:
    Py_XDECREF(positive);
    Py_XDECREF(changed);
    return result;
}
