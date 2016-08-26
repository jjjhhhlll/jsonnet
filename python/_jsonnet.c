/*
Copyright 2015 Google Inc. All rights reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#include <stdlib.h>
#include <stdio.h>

#include <Python.h>

#include "libjsonnet.h"

#if PY_MAJOR_VERSION >= 3
#define IS_PY3K
#endif

#ifdef IS_PY3K

static const char *PyString_AsString(PyObject *o) {
	if (PyUnicode_Check(o)) {
		PyObject * temp_bytes = PyUnicode_AsUTF8String(o);
		if (temp_bytes != NULL) {
			char * result = strdup(PyBytes_AS_STRING(temp_bytes));
			Py_DECREF(temp_bytes);
			return result;
		} 
	} 
	return NULL;
}

// in python3 we need an intermediate object to support PyString_AsString
// conversion, so we strdup and then use this function to free the
// value later
static void free_PyString_AsString(const char *tmp) {
	if (tmp) free((void*)tmp);
}

static PyObject *PyString_FromString(const char *str) {
	return PyUnicode_FromString(str);
}

static int PyString_Check(PyObject *o) {
	return PyUnicode_Check(o);
}

static int PyInt_Check(PyObject *o) {
	return PyLong_Check(o);
}

static long PyInt_AsLong(PyObject *io) {
	return PyLong_AsLong(io);
}

#else

// in python2.7 we don't need to free values, so this is an
// empty function.
static void free_PyString_AsString(const char *tmp) { }

#endif

static char *jsonnet_str(struct JsonnetVm *vm, const char *str)
{
    char *out = jsonnet_realloc(vm, NULL, strlen(str) + 1);
    memcpy(out, str, strlen(str) + 1);
    return out;
}

static const char *exc_to_str(void)
{
    PyObject *ptype, *pvalue, *ptraceback;
    PyErr_Fetch(&ptype, &pvalue, &ptraceback);
    PyObject *exc_str = PyObject_Str(pvalue);
    return PyString_AsString(exc_str);
}

struct NativeCtx {
    struct JsonnetVm *vm;
    PyObject *callback;
    size_t argc;
};

static struct JsonnetJsonValue *python_to_jsonnet_json(struct JsonnetVm *vm, PyObject *v,
                                                       const char **err_msg)
{
    if (PyString_Check(v)) {
		const char *v_ = PyString_AsString(v);
		struct JsonnetJsonValue *r = jsonnet_json_make_string(vm, v_);
		free_PyString_AsString(v_);
		return r;

    } else if (PyUnicode_Check(v)) {
	 	struct JsonnetJsonValue *r;
		PyObject *str = PyUnicode_AsUTF8String(v);
#ifdef IS_PY3K
		const char *str_ = PyBytes_AsString(str);
#else
		const char *str_ = PyString_AsString(str);
#endif
		r = jsonnet_json_make_string(vm, str_);
		Py_DECREF(str);
		return r;

	} else if (PyBool_Check(v)) {
        return jsonnet_json_make_bool(vm, PyObject_IsTrue(v));
    } else if (PyFloat_Check(v)) {
        return jsonnet_json_make_number(vm, PyFloat_AsDouble(v));
    } else if (PyInt_Check(v)) {
        return jsonnet_json_make_number(vm, (double)(PyInt_AsLong(v)));
    } else if (v == Py_None) {
        return jsonnet_json_make_null(vm);
    } else if (PySequence_Check(v)) {
        Py_ssize_t len, i;
        struct JsonnetJsonValue *arr;
        // Convert it to a O(1) indexable form if necessary.
        PyObject *fast = PySequence_Fast(v, "python_to_jsonnet_json internal error: not sequence");
        len = PySequence_Fast_GET_SIZE(fast);
        arr = jsonnet_json_make_array(vm);
        for (i = 0; i < len; ++i) {
            struct JsonnetJsonValue *json_el;
            PyObject *el = PySequence_Fast_GET_ITEM(fast, i);
            json_el = python_to_jsonnet_json(vm, el, err_msg);
            if (json_el == NULL) {
                Py_DECREF(fast);
                jsonnet_json_destroy(vm, arr);
                return NULL;
            }
            jsonnet_json_array_append(vm, arr, json_el);
        }
        Py_DECREF(fast);
        return arr;
    } else if (PyDict_Check(v)) {
        struct JsonnetJsonValue *obj;
        PyObject *key, *val;
        Py_ssize_t pos = 0;
        obj = jsonnet_json_make_object(vm);
        while (PyDict_Next(v, &pos, &key, &val)) {
            struct JsonnetJsonValue *json_val;
            const char *key_ = PyString_AsString(key);
            if (key_ == NULL) {
                *err_msg = "Non-string key in dict returned from Python Jsonnet native extension.";
                jsonnet_json_destroy(vm, obj);
                return NULL;
            }
            json_val = python_to_jsonnet_json(vm, val, err_msg);
            if (json_val == NULL) {
                jsonnet_json_destroy(vm, obj);
                return NULL;
            }
            jsonnet_json_object_append(vm, obj, key_, json_val);
			free_PyString_AsString(key_);

        }
        return obj;
    } else {
        *err_msg = "Unrecognized type return from Python Jsonnet native extension.";
        return NULL;
    }
}

/* This function is bound for every native callback, but with a different 
 * context.
 */
static struct JsonnetJsonValue *cpython_native_callback(
    void *ctx_, const struct JsonnetJsonValue * const *argv, int *succ)
{
    const struct NativeCtx *ctx = ctx_;
    int i;

    PyObject *arglist;  // Will hold a tuple of strings.
    PyObject *result;  // Will hold a string.

    // Populate python function args.
    arglist = PyTuple_New(ctx->argc);
    for (i = 0; i < ctx->argc; ++i) {
        double d;
        const char *param_str = jsonnet_json_extract_string(ctx->vm, argv[i]);
        int param_null = jsonnet_json_extract_null(ctx->vm, argv[i]);
        int param_bool = jsonnet_json_extract_bool(ctx->vm, argv[i]);
        int param_num = jsonnet_json_extract_number(ctx->vm, argv[i], &d);
        PyObject *pyobj;
        if (param_str != NULL) {
            pyobj = PyString_FromString(param_str);
        } else if (param_null) {
            pyobj = Py_None;
        } else if (param_bool != 2) {
            pyobj = PyBool_FromLong(param_bool);
        } else if (param_num) {
            pyobj = PyFloat_FromDouble(d);
        } else {
            // TODO(dcunnin): Support arrays (to tuples).
            // TODO(dcunnin): Support objects (to dicts).
            Py_DECREF(arglist);
            *succ = 0;
            return jsonnet_json_make_string(ctx->vm, "Non-primitive param.");
        }
        PyTuple_SetItem(arglist, i, pyobj);
    }

    // Call python function.
    result = PyEval_CallObject(ctx->callback, arglist);
    Py_DECREF(arglist);

    if (result == NULL) {
        // Get string from exception.
		const char *exc_str = exc_to_str();
        struct JsonnetJsonValue *r = jsonnet_json_make_string(ctx->vm, exc_str);
        *succ = 0;
        PyErr_Clear();
		free_PyString_AsString(exc_str);
        return r;
    }

    const char *err_msg;
    struct JsonnetJsonValue *r = python_to_jsonnet_json(ctx->vm, result, &err_msg);
    if (r != NULL) {
        *succ = 1;
    } else {
        *succ = 0;
        r = jsonnet_json_make_string(ctx->vm, err_msg);
    }
    return r;
}


struct ImportCtx {
    struct JsonnetVm *vm;
    PyObject *callback;
};

static char *cpython_import_callback(void *ctx_, const char *base, const char *rel,
                                     char **found_here, int *success)
{
    const struct ImportCtx *ctx = ctx_;
    PyObject *arglist, *result;
    char *out;

    arglist = Py_BuildValue("(s, s)", base, rel);
    result = PyEval_CallObject(ctx->callback, arglist);
    Py_DECREF(arglist);

    if (result == NULL) {
        // Get string from exception
        char *out = jsonnet_str(ctx->vm, exc_to_str());
        *success = 0;
        PyErr_Clear();
        return out;
    }

    if (!PyTuple_Check(result)) {
        out = jsonnet_str(ctx->vm, "import_callback did not return a tuple");
        *success = 0;
    } else if (PyTuple_Size(result) != 2) {
        out = jsonnet_str(ctx->vm, "import_callback did not return a tuple (size 2)");
        *success = 0;
    } else {
        PyObject *file_name = PyTuple_GetItem(result, 0);
        PyObject *file_content = PyTuple_GetItem(result, 1);
        if (!PyString_Check(file_name) || !PyString_Check(file_content)) {
            out = jsonnet_str(ctx->vm, "import_callback did not return a pair of strings");
            *success = 0;
        } else {
            const char *found_here_cstr = PyString_AsString(file_name);
            const char *content_cstr = PyString_AsString(file_content);
            *found_here = jsonnet_str(ctx->vm, found_here_cstr);
            out = jsonnet_str(ctx->vm, content_cstr);
            *success = 1;
			free_PyString_AsString(found_here_cstr);
			free_PyString_AsString(content_cstr);
        }
    }

    Py_DECREF(result);

    return out;
}

static PyObject *handle_result(struct JsonnetVm *vm, char *out, int error)
{
    if (error) {
        PyErr_SetString(PyExc_RuntimeError, out);
        jsonnet_realloc(vm, out, 0);
        jsonnet_destroy(vm);
        return NULL;
    } else {
        PyObject *ret = PyString_FromString(out);
        jsonnet_realloc(vm, out, 0);
        jsonnet_destroy(vm);
        return ret;
    }
}

int handle_vars(struct JsonnetVm *vm, PyObject *map, int code, int tla)
{
    if (map == NULL) return 1;

    PyObject *key, *val;
    Py_ssize_t pos = 0;
    
    while (PyDict_Next(map, &pos, &key, &val)) {
        const char *key_ = PyString_AsString(key);
        if (key_ == NULL) {
            jsonnet_destroy(vm);
			free_PyString_AsString(key_);
            return 0;
        }
        const char *val_ = PyString_AsString(val);
        if (val_ == NULL) {
            jsonnet_destroy(vm);
			free_PyString_AsString(val_);
            return 0;
        }
        if (!tla && !code) {
            jsonnet_ext_var(vm, key_, val_);
        } else if (!tla && code) {
            jsonnet_ext_code(vm, key_, val_);
        } else if (tla && !code) {
            jsonnet_tla_var(vm, key_, val_);
        } else {
            jsonnet_tla_code(vm, key_, val_);
        }
		free_PyString_AsString(key_);
		free_PyString_AsString(val_);
    }
    return 1;
}


int handle_import_callback(struct ImportCtx *ctx, PyObject *import_callback)
{
    if (import_callback == NULL) return 1;

    if (!PyCallable_Check(import_callback)) {
        jsonnet_destroy(ctx->vm);
        PyErr_SetString(PyExc_TypeError, "import_callback must be callable");
        return 0;
    }

    jsonnet_import_callback(ctx->vm, cpython_import_callback, ctx);

    return 1;
}


/** Register native callbacks with Jsonnet VM.
 *
 * Example native_callbacks = { 'name': (('p1', 'p2', 'p3'), func) }
 *
 * May set *ctxs, in which case it should be free()'d by caller.
 *
 * \returns 1 on success, 0 with exception set upon failure.
 */
static int handle_native_callbacks(struct JsonnetVm *vm, PyObject *native_callbacks,
                                   struct NativeCtx **ctxs)
{
    size_t num_natives = 0;
    PyObject *key, *val;
    Py_ssize_t pos = 0;

    if (native_callbacks == NULL) return 1;

    /* Verify the input before we allocate memory, throw all errors at this point.
     * Also, count the callbacks to see how much memory we need.
     */
    while (PyDict_Next(native_callbacks, &pos, &key, &val)) {
        Py_ssize_t i;
        Py_ssize_t num_params;
        PyObject *params;
        const char *key_ = PyString_AsString(key);
        if (key_ == NULL) {
            PyErr_SetString(PyExc_TypeError, "native callback dict keys must be string");
            goto bad;
        }
        if (!PyTuple_Check(val)) {
            PyErr_SetString(PyExc_TypeError, "native callback dict values must be tuples");
            goto bad;
        } else if (PyTuple_Size(val) != 2) {
            PyErr_SetString(PyExc_TypeError, "native callback tuples must have size 2");
            goto bad;
        }
        params = PyTuple_GetItem(val, 0);
        if (!PyTuple_Check(params)) {
            PyErr_SetString(PyExc_TypeError, "native callback params must be a tuple");
            goto bad;
        }
        /* Check the params are all strings */
        num_params = PyTuple_Size(params);
        for (i = 0; i < num_params ; ++i) {
            PyObject *param = PyTuple_GetItem(params, 0);
            if (!PyString_Check(param)) {
                PyErr_SetString(PyExc_TypeError, "native callback param must be string");
                goto bad;
            }
        }
        if (!PyCallable_Check(PyTuple_GetItem(val, 1))) {
            PyErr_SetString(PyExc_TypeError, "native callback must be callable");
            goto bad;
        }

        num_natives++;
		free_PyString_AsString(key_);
        continue;

        bad:
        jsonnet_destroy(vm);
		free_PyString_AsString(key_);
        return 0;
    }

    if (num_natives == 0) {
        return 1;
    }

    *ctxs = malloc(sizeof(struct NativeCtx) * num_natives);
    
    /* Re-use num_natives but just as a counter this time. */
    num_natives = 0;
    pos = 0;
    while (PyDict_Next(native_callbacks, &pos, &key, &val)) {
        Py_ssize_t i;
        Py_ssize_t num_params;
        PyObject *params;
        const char *key_ = PyString_AsString(key);
        params = PyTuple_GetItem(val, 0);
        num_params = PyTuple_Size(params);
        /* Include space for terminating NULL. */
        const char **params_c = malloc(sizeof(const char*) * (num_params + 1));
        for (i = 0; i < num_params ; ++i) {
            params_c[i] = PyString_AsString(PyTuple_GetItem(params, i));
        }
        params_c[num_params] = NULL;
        (*ctxs)[num_natives].vm = vm;
        (*ctxs)[num_natives].callback = PyTuple_GetItem(val, 1);
        (*ctxs)[num_natives].argc = num_params;
        jsonnet_native_callback(vm, key_, cpython_native_callback, &(*ctxs)[num_natives],
                                params_c);
		for (i=0; i < num_params; ++i) {
			free_PyString_AsString(params_c[i]);
		}
        free(params_c);
        num_natives++;
		free_PyString_AsString(key_);
    }

    return 1;
}


static PyObject* evaluate_file(PyObject* self, PyObject* args, PyObject *keywds)
{
    const char *filename;
    char *out;
    unsigned max_stack = 500, gc_min_objects = 1000, max_trace = 20;
    double gc_growth_trigger = 2;
    int error;
    PyObject *ext_vars = NULL, *ext_codes = NULL;
    PyObject *tla_vars = NULL, *tla_codes = NULL;
    PyObject *import_callback = NULL;
    PyObject *native_callbacks = NULL;
    struct JsonnetVm *vm;
    static char *kwlist[] = {
        "filename",
        "max_stack", "gc_min_objects", "gc_growth_trigger", "ext_vars",
        "ext_codes", "tla_vars", "tla_codes", "max_trace", "import_callback",
        "native_callbacks",
        NULL
    };

    (void) self;

    if (!PyArg_ParseTupleAndKeywords(
        args, keywds, "s|IIdOOOOIOO", kwlist,
        &filename,
        &max_stack, &gc_min_objects, &gc_growth_trigger, &ext_vars,
        &ext_codes, &tla_vars, &tla_codes, &max_trace, &import_callback,
        &native_callbacks)) {
        return NULL;
    }

    vm = jsonnet_make();
    jsonnet_max_stack(vm, max_stack);
    jsonnet_gc_min_objects(vm, gc_min_objects);
    jsonnet_max_trace(vm, max_trace);
    jsonnet_gc_growth_trigger(vm, gc_growth_trigger);
    if (!handle_vars(vm, ext_vars, 0, 0)) return NULL;
    if (!handle_vars(vm, ext_codes, 1, 0)) return NULL;
    if (!handle_vars(vm, tla_vars, 0, 1)) return NULL;
    if (!handle_vars(vm, tla_codes, 1, 1)) return NULL;
    struct ImportCtx ctx = { vm, import_callback };
    if (!handle_import_callback(&ctx, import_callback)) {
        return NULL;
    }
    struct NativeCtx *ctxs = NULL;
    if (!handle_native_callbacks(vm, native_callbacks, &ctxs)) {
        free(ctxs);
        return NULL;
    }
    out = jsonnet_evaluate_file(vm, filename, &error);
    free(ctxs);
    return handle_result(vm, out, error);
}

static PyObject* evaluate_snippet(PyObject* self, PyObject* args, PyObject *keywds)
{
    const char *filename, *src;
    char *out;
    unsigned max_stack = 500, gc_min_objects = 1000, max_trace = 20;
    double gc_growth_trigger = 2;
    int error;
    PyObject *ext_vars = NULL, *ext_codes = NULL;
    PyObject *tla_vars = NULL, *tla_codes = NULL;
    PyObject *import_callback = NULL;
    PyObject *native_callbacks = NULL;
    struct JsonnetVm *vm;
    static char *kwlist[] = {
        "filename", "src",
        "max_stack", "gc_min_objects", "gc_growth_trigger", "ext_vars",
        "ext_codes", "tla_vars", "tla_codes", "max_trace", "import_callback",
        "native_callbacks",
        NULL
    };

    (void) self;

    if (!PyArg_ParseTupleAndKeywords(
        args, keywds, "ss|IIdOOOOIOO", kwlist,
        &filename, &src,
        &max_stack, &gc_min_objects, &gc_growth_trigger, &ext_vars,
        &ext_codes, &tla_vars, &tla_codes, &max_trace, &import_callback,
        &native_callbacks)) {
        return NULL;
    }

    vm = jsonnet_make();
    jsonnet_max_stack(vm, max_stack);
    jsonnet_gc_min_objects(vm, gc_min_objects);
    jsonnet_max_trace(vm, max_trace);
    jsonnet_gc_growth_trigger(vm, gc_growth_trigger);
    if (!handle_vars(vm, ext_vars, 0, 0)) return NULL;
    if (!handle_vars(vm, ext_codes, 1, 0)) return NULL;
    if (!handle_vars(vm, tla_vars, 0, 1)) return NULL;
    if (!handle_vars(vm, tla_codes, 1, 1)) return NULL;
    struct ImportCtx ctx = { vm, import_callback };
    if (!handle_import_callback(&ctx, import_callback)) {
        return NULL;
    }
    struct NativeCtx *ctxs = NULL;
    if (!handle_native_callbacks(vm, native_callbacks, &ctxs)) {
        free(ctxs);
        return NULL;
    }
    out = jsonnet_evaluate_snippet(vm, filename, src, &error);
    free(ctxs);
    return handle_result(vm, out, error);
}

struct module_state {
	PyObject *error;
};

static PyMethodDef module_methods[] = {
    {"evaluate_file", (PyCFunction)evaluate_file, METH_VARARGS | METH_KEYWORDS,
     "Interpret the given Jsonnet file."},
    {"evaluate_snippet", (PyCFunction)evaluate_snippet, METH_VARARGS | METH_KEYWORDS,
     "Interpret the given Jsonnet code."},
    {NULL, NULL, 0, NULL}
};

#define MODULE_NAME "_jsonnet"
#define MODULE_DOC "A Python interface to Jsonnet."

#ifdef IS_PY3K
static struct PyModuleDef moduledef = {
	PyModuleDef_HEAD_INIT,
	MODULE_NAME,
	MODULE_DOC,
	0, // no module state
	module_methods,
	NULL, // no slots
	NULL, // no module state, so no traverse needed
	NULL, // no module state, so no clear function needed
	NULL  // no module state, no free function needed
};
#endif

#ifdef IS_PY3K
PyMODINIT_FUNC PyInit__jsonnet(void) 
{
    return PyModule_Create(&moduledef);
}
#else
PyMODINIT_FUNC init_jsonnet(void) 
{
    Py_InitModule3(MODULE_NAME, module_methods, MODULE_DOC);
}
#endif
