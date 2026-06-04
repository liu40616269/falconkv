// FalconKV C Extension Module
// Raw Python C API binding, following FalconFS's _pyfalconfs_internal pattern.
//
// Layer 2 in the FalconKV Python stack:
//   Python → _pyfalconkv_internal → FalconKVBridge → FalconKVClientImpl
//
// This module provides module-level functions with an opaque handle parameter
// to manage FalconKVBridge instances. Supports multi-instance scenarios.

// Python.h must be included first before any standard headers
#include <Python.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "falconkv_bridge.h"

// =================== Handle Management ===================
//
// Opaque handle pattern: Bridge instances are stored in a global registry
// keyed by void*. Python side holds an integer handle.
// Thread-safe via g_bridges_mutex.

static std::unordered_map<void*, std::unique_ptr<falconkv::FalconKVBridge>> g_bridges;
static std::mutex g_bridges_mutex;
static std::atomic<uintptr_t> g_next_handle{1};

static PyObject* bridge_to_handle(falconkv::FalconKVBridge* bridge) {
    void* handle = reinterpret_cast<void*>(g_next_handle.fetch_add(1));
    std::lock_guard<std::mutex> lock(g_bridges_mutex);
    g_bridges[handle] =
        std::unique_ptr<falconkv::FalconKVBridge>(bridge);
    return PyLong_FromVoidPtr(handle);
}

static falconkv::FalconKVBridge* handle_to_bridge(PyObject* handle_obj) {
    void* handle = PyLong_AsVoidPtr(handle_obj);
    if (handle == nullptr && PyErr_Occurred()) return nullptr;
    std::lock_guard<std::mutex> lock(g_bridges_mutex);
    auto it = g_bridges.find(handle);
    if (it == g_bridges.end()) {
        PyErr_SetString(PyExc_RuntimeError, "Invalid FalconKV handle");
        return nullptr;
    }
    return it->second.get();
}

// =================== Init ===================

static PyObject* PyWrapper_Init(PyObject* /*self*/, PyObject* args) {
    const char* config_file = nullptr;
    Py_ssize_t cache_capacity = 100000;
    int worker_id = -1;
    if (!PyArg_ParseTuple(args, "s|ni", &config_file, &cache_capacity, &worker_id))
        return nullptr;

    falconkv::FalconKVBridge::Config config;
    config.config_file = config_file ? config_file : "";
    config.cache_capacity = static_cast<size_t>(cache_capacity);
    config.worker_id = worker_id;

    falconkv::FalconKVBridge* bridge = nullptr;
    try {
        bridge = new falconkv::FalconKVBridge(config);
    } catch (const std::exception& e) {
        PyErr_SetString(PyExc_RuntimeError, e.what());
        return nullptr;
    }

    return bridge_to_handle(bridge);
}

// =================== BatchExistSync ===================

static PyObject* PyWrapper_BatchExistSync(PyObject* /*self*/, PyObject* args) {
    PyObject* handle_obj = nullptr;
    PyObject* keys_list = nullptr;
    if (!PyArg_ParseTuple(args, "OO", &handle_obj, &keys_list))
        return nullptr;

    auto* bridge = handle_to_bridge(handle_obj);
    if (!bridge) return nullptr;

    Py_ssize_t n = PyList_Size(keys_list);
    std::vector<std::string> keys;
    keys.reserve(static_cast<size_t>(n));
    for (Py_ssize_t i = 0; i < n; ++i) {
        PyObject* item = PyList_GetItem(keys_list, i);  // borrowed ref
        const char* key = PyUnicode_AsUTF8(item);
        if (!key) return nullptr;
        keys.emplace_back(key);
    }

    int result = 0;
    {
        Py_BEGIN_ALLOW_THREADS
        result = bridge->BatchExistSync(keys);
        Py_END_ALLOW_THREADS
    }

    return PyLong_FromLong(result);
}

// =================== BatchPutSync ===================

static PyObject* PyWrapper_BatchPutSync(PyObject* /*self*/, PyObject* args) {
    PyObject* handle_obj = nullptr;
    PyObject* keys_list = nullptr;
    PyObject* ptrs_list = nullptr;
    PyObject* sizes_list = nullptr;
    if (!PyArg_ParseTuple(args, "OOOO", &handle_obj, &keys_list,
                          &ptrs_list, &sizes_list))
        return nullptr;

    auto* bridge = handle_to_bridge(handle_obj);
    if (!bridge) return nullptr;

    Py_ssize_t n = PyList_Size(keys_list);

    std::vector<std::string> keys;
    keys.reserve(static_cast<size_t>(n));
    for (Py_ssize_t i = 0; i < n; ++i) {
        const char* key =
            PyUnicode_AsUTF8(PyList_GetItem(keys_list, i));
        if (!key) return nullptr;
        keys.emplace_back(key);
    }

    std::vector<falconkv::BridgeBuffer> buffers;
    buffers.reserve(static_cast<size_t>(n));
    for (Py_ssize_t i = 0; i < n; ++i) {
        void* ptr =
            PyLong_AsVoidPtr(PyList_GetItem(ptrs_list, i));
        if (PyErr_Occurred()) return nullptr;
        long size =
            PyLong_AsLong(PyList_GetItem(sizes_list, i));
        if (PyErr_Occurred()) return nullptr;
        buffers.push_back({ptr, static_cast<uint32_t>(size)});
    }

    int result = 0;
    {
        Py_BEGIN_ALLOW_THREADS
        result = bridge->BatchPutSync(keys, buffers);
        Py_END_ALLOW_THREADS
    }

    if (result != 0) {
        PyErr_SetString(PyExc_RuntimeError, "BatchPutSync failed");
        return nullptr;
    }
    Py_RETURN_NONE;
}

// =================== BatchGetSync ===================

static PyObject* PyWrapper_BatchGetSync(PyObject* /*self*/, PyObject* args) {
    PyObject* handle_obj = nullptr;
    PyObject* keys_list = nullptr;
    PyObject* ptrs_list = nullptr;
    PyObject* sizes_list = nullptr;
    if (!PyArg_ParseTuple(args, "OOOO", &handle_obj, &keys_list,
                          &ptrs_list, &sizes_list))
        return nullptr;

    auto* bridge = handle_to_bridge(handle_obj);
    if (!bridge) return nullptr;

    Py_ssize_t n = PyList_Size(keys_list);

    std::vector<std::string> keys;
    std::vector<falconkv::BridgeBuffer> buffers;
    keys.reserve(static_cast<size_t>(n));
    buffers.reserve(static_cast<size_t>(n));

    for (Py_ssize_t i = 0; i < n; ++i) {
        const char* key =
            PyUnicode_AsUTF8(PyList_GetItem(keys_list, i));
        if (!key) return nullptr;
        keys.emplace_back(key);

        void* ptr =
            PyLong_AsVoidPtr(PyList_GetItem(ptrs_list, i));
        if (PyErr_Occurred()) return nullptr;
        long size =
            PyLong_AsLong(PyList_GetItem(sizes_list, i));
        if (PyErr_Occurred()) return nullptr;
        buffers.push_back({ptr, static_cast<uint32_t>(size)});
    }

    std::vector<int32_t> results;
    {
        Py_BEGIN_ALLOW_THREADS
        results = bridge->BatchGetSync(keys, buffers);
        Py_END_ALLOW_THREADS
    }

    PyObject* result_list = PyList_New(static_cast<Py_ssize_t>(results.size()));
    if (!result_list) return nullptr;
    for (size_t i = 0; i < results.size(); ++i) {
        PyObject* val = PyLong_FromLong(results[i]);
        if (!val) {
            Py_DECREF(result_list);
            return nullptr;
        }
        PyList_SET_ITEM(result_list, static_cast<Py_ssize_t>(i), val);
    }
    return result_list;
}

// =================== Fire-and-Forget Put ===================

// Callback data passed to Bridge. The C Extension INCREFs the Python
// MemoryObj objects so they survive until the C++ worker thread finishes.
struct FireAndForgetData {
    PyObject** py_objs;  // Array of Python MemoryObj objects
    size_t count;        // Array length
};

// Called by Bridge's worker thread (no GIL held).
// Must acquire GIL before calling Python API.
static void fire_and_forget_callback(void* user_data) {
    auto* data = static_cast<FireAndForgetData*>(user_data);

    PyGILState_STATE gstate = PyGILState_Ensure();
    for (size_t i = 0; i < data->count; ++i) {
        // 调用 MemoryObj.ref_count_down() 释放内存池引用
        PyObject* r = PyObject_CallMethod(data->py_objs[i],
                                          "ref_count_down", nullptr);
        Py_XDECREF(r);
        // 释放 C Extension 在提交时 INCREF 的引用
        Py_DECREF(data->py_objs[i]);
    }
    PyGILState_Release(gstate);

    delete[] data->py_objs;
    delete data;
}

static PyObject* PyWrapper_FireAndForgetPut(PyObject* /*self*/,
                                             PyObject* args) {
    PyObject* handle_obj = nullptr;
    PyObject* keys_list = nullptr;
    PyObject* ptrs_list = nullptr;
    PyObject* sizes_list = nullptr;
    PyObject* memobjs_list = nullptr;
    if (!PyArg_ParseTuple(args, "OOOOO", &handle_obj, &keys_list,
                          &ptrs_list, &sizes_list, &memobjs_list))
        return nullptr;

    auto* bridge = handle_to_bridge(handle_obj);
    if (!bridge) return nullptr;

    Py_ssize_t n = PyList_Size(keys_list);

    std::vector<std::string> keys;
    std::vector<falconkv::BridgeBuffer> buffers;
    keys.reserve(static_cast<size_t>(n));
    buffers.reserve(static_cast<size_t>(n));

    for (Py_ssize_t i = 0; i < n; ++i) {
        const char* key =
            PyUnicode_AsUTF8(PyList_GetItem(keys_list, i));
        if (!key) return nullptr;
        keys.emplace_back(key);

        void* ptr =
            PyLong_AsVoidPtr(PyList_GetItem(ptrs_list, i));
        if (PyErr_Occurred()) return nullptr;
        long size =
            PyLong_AsLong(PyList_GetItem(sizes_list, i));
        if (PyErr_Occurred()) return nullptr;
        buffers.push_back({ptr, static_cast<uint32_t>(size)});
    }

    // 准备回调数据：INCREF 每个 memobj 防止 GC 回收
    auto* cb_data = new FireAndForgetData();
    cb_data->count = static_cast<size_t>(n);
    cb_data->py_objs = new PyObject*[static_cast<size_t>(n)];

    for (Py_ssize_t i = 0; i < n; ++i) {
        PyObject* memobj = PyList_GetItem(memobjs_list, i);  // borrowed
        Py_INCREF(memobj);
        cb_data->py_objs[i] = memobj;
    }

    // 提交到 Bridge，后台线程完成后调用 fire_and_forget_callback
    bridge->FireAndForgetPut(keys, buffers,
                             fire_and_forget_callback, cb_data);

    Py_RETURN_NONE;
}

// =================== Close ===================

static PyObject* PyWrapper_Close(PyObject* /*self*/, PyObject* args) {
    PyObject* handle_obj = nullptr;
    if (!PyArg_ParseTuple(args, "O", &handle_obj))
        return nullptr;

    void* handle = PyLong_AsVoidPtr(handle_obj);
    if (handle == nullptr && PyErr_Occurred()) return nullptr;

    {
        std::lock_guard<std::mutex> lock(g_bridges_mutex);
        auto it = g_bridges.find(handle);
        if (it != g_bridges.end()) {
            // 释放 GIL，Close 可能等待后台任务完成
            Py_BEGIN_ALLOW_THREADS
            it->second->Close();
            g_bridges.erase(it);
            Py_END_ALLOW_THREADS
        }
    }

    Py_RETURN_NONE;
}

// =================== Module Definition ===================

static PyMethodDef PyFalconKVInternalMethods[] = {
    {"Init",
     PyWrapper_Init,
     METH_VARARGS,
     "Initialize FalconKV bridge.\n"
     "Args: config_file (str), [cache_capacity (int), [worker_id (int)]]\n"
     "Returns: handle (int)"},
    {"BatchExistSync",
     PyWrapper_BatchExistSync,
     METH_VARARGS,
     "Batch check key existence.\n"
     "Args: handle, keys (List[str])\n"
     "Returns: int (hit count)"},
    {"BatchPutSync",
     PyWrapper_BatchPutSync,
     METH_VARARGS,
     "Batch put key-value pairs (synchronous).\n"
     "Args: handle, keys (List[str]), data_ptrs (List[int]), sizes (List[int])\n"
     "Returns: None"},
    {"BatchGetSync",
     PyWrapper_BatchGetSync,
     METH_VARARGS,
     "Batch get into pre-allocated buffers.\n"
     "Args: handle, keys (List[str]), data_ptrs (List[int]), sizes (List[int])\n"
     "Returns: List[int] (bytes read per key, <=0 means failure)"},
    {"FireAndForgetPut",
     PyWrapper_FireAndForgetPut,
     METH_VARARGS,
     "Fire-and-forget batch put with Python ref management.\n"
     "Args: handle, keys, data_ptrs, sizes, memobjs\n"
     "Returns: None"},
    {"Close",
     PyWrapper_Close,
     METH_VARARGS,
     "Close FalconKV bridge and release resources.\n"
     "Args: handle\n"
     "Returns: None"},
    {nullptr, nullptr, 0, nullptr}  // Sentinel
};

static struct PyModuleDef PyFalconKVInternalModule = {
    PyModuleDef_HEAD_INIT,
    "_pyfalconkv_internal",                              // m_name
    "FalconKV C extension (internal, use pyfalconkv)",   // m_doc
    -1,                                                   // m_size
    PyFalconKVInternalMethods,                            // m_methods
    nullptr,                                              // m_slots
    nullptr,                                              // m_traverse
    nullptr,                                              // m_clear
    nullptr                                               // m_free
};

extern "C" PyMODINIT_FUNC PyInit__pyfalconkv_internal(void) {
    return PyModule_Create(&PyFalconKVInternalModule);
}
