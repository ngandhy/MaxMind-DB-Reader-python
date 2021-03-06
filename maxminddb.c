#include <Python.h>
#include <maxminddb.h>
#include "structmember.h"

#define __STDC_FORMAT_MACROS
#include <inttypes.h>

static PyTypeObject Reader_Type;
static PyTypeObject Metadata_Type;
static PyObject *MaxMindDB_error;

typedef struct {
    PyObject_HEAD               /* no semicolon */
    MMDB_s *mmdb;
} Reader_obj;

typedef struct {
    PyObject_HEAD               /* no semicolon */
    PyObject *binary_format_major_version;
    PyObject *binary_format_minor_version;
    PyObject *build_epoch;
    PyObject *database_type;
    PyObject *description;
    PyObject *ip_version;
    PyObject *languages;
    PyObject *node_count;
    PyObject *record_size;
} Metadata_obj;

static PyObject *from_entry_data_list(MMDB_entry_data_list_s **entry_data_list);
static PyObject *from_map(MMDB_entry_data_list_s **entry_data_list);
static PyObject *from_array(MMDB_entry_data_list_s **entry_data_list);
static PyObject *from_uint128(const MMDB_entry_data_list_s *entry_data_list);

#if PY_MAJOR_VERSION >= 3
    #define MOD_INIT(name) PyMODINIT_FUNC PyInit_ ## name(void)
    #define RETURN_MOD_INIT(m) return (m)
#else
    #define MOD_INIT(name) PyMODINIT_FUNC init ## name(void)
    #define RETURN_MOD_INIT(m) return
    #define PyInt_FromLong PyLong_FromLong
#endif

#ifdef __GNUC__
    #  define UNUSED(x) UNUSED_ ## x __attribute__((__unused__))
#else
    #  define UNUSED(x) UNUSED_ ## x
#endif

static PyObject *Reader_constructor(PyObject *UNUSED(self), PyObject *args)
{
    char *filename;

    if (!PyArg_ParseTuple(args, "s", &filename)) {
        return NULL;
    }

    if (0 != access(filename, R_OK)) {
        PyErr_Format(PyExc_ValueError,
                     "The file \"%s\" does not exist or is not readable.",
                     filename);
        return NULL;
    }

    MMDB_s *mmdb = (MMDB_s *)malloc(sizeof(MMDB_s));
    if (NULL == mmdb) {
        PyErr_NoMemory();
        return NULL;
    }

    Reader_obj *obj = PyObject_New(Reader_obj, &Reader_Type);
    if (!obj) {
        PyErr_NoMemory();
        return NULL;
    }

    uint16_t status = MMDB_open(filename, MMDB_MODE_MMAP, mmdb);

    if (MMDB_SUCCESS != status) {
        free(mmdb);
        PyObject_Del(obj);
        return PyErr_Format(
                   MaxMindDB_error,
                   "Error opening database file (%s). Is this a valid MaxMind DB file?",
                   filename
                   );
    }

    obj->mmdb = mmdb;
    return (PyObject *)obj;
}

static PyObject *Reader_get(PyObject *self, PyObject *args)
{
    char *ip_address = NULL;

    Reader_obj *mmdb_obj = (Reader_obj *)self;
    if (!PyArg_ParseTuple(args, "s", &ip_address)) {
        return NULL;
    }

    MMDB_s *mmdb = mmdb_obj->mmdb;

    if (NULL == mmdb) {
        PyErr_SetString(PyExc_IOError,
                        "Attempt to read from a closed MaxMind DB.");
        return NULL;
    }

    int gai_error = 0;
    int mmdb_error = MMDB_SUCCESS;
    MMDB_lookup_result_s result =
        MMDB_lookup_string(mmdb, ip_address, &gai_error,
                           &mmdb_error);

    if (0 != gai_error) {
        PyErr_Format(PyExc_ValueError,
                     "The value \"%s\" is not a valid IP address.",
                     ip_address);
        return NULL;
    }

    if (MMDB_SUCCESS != mmdb_error) {
        PyObject *exception;
        if (MMDB_IPV6_LOOKUP_IN_IPV4_DATABASE_ERROR == mmdb_error) {
            exception = PyExc_ValueError;
        } else {
            exception = MaxMindDB_error;
        }
        PyErr_Format(exception, "Error looking up %s. %s",
                     ip_address, MMDB_strerror(mmdb_error));
        return NULL;
    }

    if (!result.found_entry) {
        Py_RETURN_NONE;
    }

    MMDB_entry_data_list_s *entry_data_list = NULL;
    int status = MMDB_get_entry_data_list(&result.entry, &entry_data_list);
    if (MMDB_SUCCESS != status) {
        PyErr_Format(MaxMindDB_error,
                     "Error while looking up data for %s. %s",
                     ip_address, MMDB_strerror(status));
        MMDB_free_entry_data_list(entry_data_list);
        return NULL;
    } else if (NULL == entry_data_list) {
        PyErr_Format(
            MaxMindDB_error,
            "Error while looking up data for %s. Your database may be corrupt or you have found a bug in libmaxminddb.",
            ip_address);
        return NULL;
    }

    MMDB_entry_data_list_s *original_entry_data_list = entry_data_list;
    PyObject *py_obj = from_entry_data_list(&entry_data_list);
    MMDB_free_entry_data_list(original_entry_data_list);
    return py_obj;
}

static PyObject *Reader_metadata(PyObject *self, PyObject *UNUSED(args))
{
    Reader_obj *mmdb_obj = (Reader_obj *)self;

    if (NULL == mmdb_obj->mmdb) {
        PyErr_SetString(PyExc_IOError,
                        "Attempt to read from a closed MaxMind DB.");
        return NULL;
    }

    Metadata_obj *obj = PyObject_New(Metadata_obj, &Metadata_Type);
    if (!obj) {
        return NULL;
    }

    MMDB_entry_data_list_s *entry_data_list;
    MMDB_get_metadata_as_entry_data_list(mmdb_obj->mmdb, &entry_data_list);
    MMDB_entry_data_list_s *original_entry_data_list = entry_data_list;

    PyObject *metadata_dict = from_entry_data_list(&entry_data_list);
    MMDB_free_entry_data_list(original_entry_data_list);
    if (NULL == metadata_dict || !PyDict_Check(metadata_dict)) {
        PyErr_SetString(MaxMindDB_error,
                        "Error decoding metadata.");
        PyObject_Del(obj);
        return NULL;
    }

    obj->binary_format_major_version = PyDict_GetItemString(
        metadata_dict, "binary_format_major_version");
    obj->binary_format_minor_version = PyDict_GetItemString(
        metadata_dict, "binary_format_minor_version");
    obj->build_epoch = PyDict_GetItemString(metadata_dict, "build_epoch");
    obj->database_type = PyDict_GetItemString(metadata_dict, "database_type");
    obj->description = PyDict_GetItemString(metadata_dict, "description");
    obj->ip_version = PyDict_GetItemString(metadata_dict, "ip_version");
    obj->languages = PyDict_GetItemString(metadata_dict, "languages");
    obj->node_count = PyDict_GetItemString(metadata_dict, "node_count");
    obj->record_size = PyDict_GetItemString(metadata_dict, "record_size");

    if (NULL == obj->binary_format_major_version ||
        NULL == obj->binary_format_minor_version ||
        NULL == obj->build_epoch ||
        NULL == obj->database_type ||
        NULL == obj->description ||
        NULL == obj->ip_version ||
        NULL == obj->languages ||
        NULL == obj->node_count ||
        NULL == obj->record_size) {
        PyErr_SetString(MaxMindDB_error,
                        "Error decoding metadata.");
        PyObject_Del(obj);
        return NULL;
    }

    Py_INCREF(obj->binary_format_major_version);
    Py_INCREF(obj->binary_format_minor_version);
    Py_INCREF(obj->build_epoch);
    Py_INCREF(obj->database_type);
    Py_INCREF(obj->description);
    Py_INCREF(obj->ip_version);
    Py_INCREF(obj->languages);
    Py_INCREF(obj->node_count);
    Py_INCREF(obj->record_size);

    Py_DECREF(metadata_dict);

    return (PyObject *)obj;
}

static PyObject *Reader_close(PyObject *self, PyObject *UNUSED(args))
{
    Reader_obj *mmdb_obj = (Reader_obj *)self;

    if (NULL == mmdb_obj->mmdb) {
        PyErr_SetString(PyExc_IOError,
                        "Attempt to close a closed MaxMind DB.");
        return NULL;
    }
    MMDB_close(mmdb_obj->mmdb);
    free(mmdb_obj->mmdb);
    mmdb_obj->mmdb = NULL;

    Py_RETURN_NONE;
}

static void Reader_dealloc(PyObject *self)
{
    Reader_obj *obj = (Reader_obj *)self;
    if (NULL != obj->mmdb) {
        Reader_close(self, NULL);
    }

    PyObject_Del(self);
}

static void Metadata_dealloc(PyObject *self)
{
    Metadata_obj *obj = (Metadata_obj *)self;
    Py_DECREF(obj->binary_format_major_version);
    Py_DECREF(obj->binary_format_minor_version);
    Py_DECREF(obj->build_epoch);
    Py_DECREF(obj->database_type);
    Py_DECREF(obj->description);
    Py_DECREF(obj->ip_version);
    Py_DECREF(obj->languages);
    Py_DECREF(obj->node_count);
    Py_DECREF(obj->record_size);
    PyObject_Del(self);
}

static PyObject *from_entry_data_list(MMDB_entry_data_list_s **entry_data_list)
{
    switch ((*entry_data_list)->entry_data.type) {
    case MMDB_DATA_TYPE_MAP:
        return from_map(entry_data_list);
    case MMDB_DATA_TYPE_ARRAY:
        return from_array(entry_data_list);
    case MMDB_DATA_TYPE_UTF8_STRING:
        return PyUnicode_FromStringAndSize(
                   (*entry_data_list)->entry_data.utf8_string,
                   (*entry_data_list)->entry_data.data_size
                   );
    case MMDB_DATA_TYPE_BYTES:
        return PyByteArray_FromStringAndSize(
                   (const char *)(*entry_data_list)->entry_data.bytes,
                   (Py_ssize_t)(*entry_data_list)->entry_data.data_size);
    case MMDB_DATA_TYPE_DOUBLE:
        return PyFloat_FromDouble((*entry_data_list)->entry_data.double_value);
    case MMDB_DATA_TYPE_FLOAT:
        return PyFloat_FromDouble((*entry_data_list)->entry_data.float_value);
    case MMDB_DATA_TYPE_UINT16:
        return PyLong_FromLong( (*entry_data_list)->entry_data.uint16);
    case MMDB_DATA_TYPE_UINT32:
        return PyLong_FromLong((*entry_data_list)->entry_data.uint32);
    case MMDB_DATA_TYPE_BOOLEAN:
        return PyBool_FromLong((*entry_data_list)->entry_data.boolean);
    case MMDB_DATA_TYPE_UINT64:
        return PyLong_FromUnsignedLongLong(
                   (*entry_data_list)->entry_data.uint64);
    case MMDB_DATA_TYPE_UINT128:
        return from_uint128(*entry_data_list);
    case MMDB_DATA_TYPE_INT32:
        return PyLong_FromLong((*entry_data_list)->entry_data.int32);
    default:
        PyErr_Format(MaxMindDB_error,
                     "Invalid data type arguments: %d",
                     (*entry_data_list)->entry_data.type);
        return NULL;
    }
    return NULL;
}

static PyObject *from_map(MMDB_entry_data_list_s **entry_data_list)
{
    PyObject *py_obj = PyDict_New();
    if (NULL == py_obj) {
        PyErr_NoMemory();
        return NULL;
    }

    const uint32_t map_size = (*entry_data_list)->entry_data.data_size;

    uint i;
    for (i = 0; i < map_size && entry_data_list; i++) {
        *entry_data_list = (*entry_data_list)->next;

        PyObject *key = PyUnicode_FromStringAndSize(
            (char *)(*entry_data_list)->entry_data.utf8_string,
            (*entry_data_list)->entry_data.data_size
            );

        *entry_data_list = (*entry_data_list)->next;

        PyObject *value = from_entry_data_list(entry_data_list);
        if (NULL == value) {
            Py_DECREF(key);
            Py_DECREF(py_obj);
            return NULL;
        }
        PyDict_SetItem(py_obj, key, value);
        Py_DECREF(value);
        Py_DECREF(key);
    }

    return py_obj;
}

static PyObject *from_array(MMDB_entry_data_list_s **entry_data_list)
{
    const uint32_t size = (*entry_data_list)->entry_data.data_size;

    PyObject *py_obj = PyList_New(size);
    if (NULL == py_obj) {
        PyErr_NoMemory();
        return NULL;
    }

    uint i;
    for (i = 0; i < size && entry_data_list; i++) {
        *entry_data_list = (*entry_data_list)->next;
        PyObject *value = from_entry_data_list(entry_data_list);
        if (NULL == value) {
            Py_DECREF(py_obj);
            return NULL;
        }
        // PyList_SetItem 'steals' the reference
        PyList_SetItem(py_obj, i, value);
    }
    return py_obj;
}

static PyObject *from_uint128(const MMDB_entry_data_list_s *entry_data_list)
{
    uint64_t high = 0;
    uint64_t low = 0;
#if MMDB_UINT128_IS_BYTE_ARRAY
    int i;
    for (i = 0; i < 8; i++) {
        high = (high << 8) | entry_data_list->entry_data.uint128[i];
    }

    for (i = 8; i < 16; i++) {
        low = (low << 8) | entry_data_list->entry_data.uint128[i];
    }
#else
    high = entry_data_list->entry_data.uint128 >> 64;
    low = (uint64_t)entry_data_list->entry_data.uint128;
#endif

    char *num_str = malloc(33);
    if (NULL == num_str) {
        PyErr_NoMemory();
        return NULL;
    }

    snprintf(num_str, 33, "%016" PRIX64 "%016" PRIX64, high, low);

    PyObject *py_obj = PyLong_FromString(num_str, NULL, 16);

    free(num_str);
    return py_obj;
}

static PyMethodDef Reader_methods[] = {
    { "get",      Reader_get,      METH_VARARGS,
      "Get record for IP address" },
    { "metadata", Reader_metadata, METH_NOARGS,
      "Returns metadata object for database" },
    { "close",    Reader_close,    METH_NOARGS, "Closes database"},
    { NULL,       NULL,            0,           NULL        }
};

static PyTypeObject Reader_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_basicsize = sizeof(Reader_obj),
    .tp_dealloc = Reader_dealloc,
    .tp_doc = "maxminddb.Reader object",
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_methods = Reader_methods,
    .tp_name = "maxminddb.Reader",
};

static PyMethodDef Metadata_methods[] = {
    { NULL, NULL, 0, NULL }
};

/* *INDENT-OFF* */
static PyMemberDef Metadata_members[] = {
    { "binary_format_major_version", T_OBJECT, offsetof(
          Metadata_obj, binary_format_major_version), READONLY, NULL },
    { "binary_format_minor_version", T_OBJECT, offsetof(
          Metadata_obj, binary_format_minor_version), READONLY, NULL },
    { "build_epoch", T_OBJECT, offsetof(Metadata_obj, build_epoch),
          READONLY, NULL },
    { "database_type", T_OBJECT, offsetof(Metadata_obj, database_type),
          READONLY, NULL },
    { "description", T_OBJECT, offsetof(Metadata_obj, description),
          READONLY, NULL },
    { "ip_version", T_OBJECT, offsetof(Metadata_obj, ip_version),
          READONLY, NULL },
    { "languages", T_OBJECT, offsetof(Metadata_obj, languages), READONLY,
          NULL },
    { "node_count", T_OBJECT, offsetof(Metadata_obj, node_count),
          READONLY, NULL },
    { "record_size", T_OBJECT, offsetof(Metadata_obj, record_size),
          READONLY, NULL },
    { NULL, 0, 0, 0, NULL }
};
/* *INDENT-ON* */

static PyTypeObject Metadata_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_basicsize = sizeof(Metadata_obj),
    .tp_dealloc = Metadata_dealloc,
    .tp_doc = "maxminddb.Metadata object",
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_members = Metadata_members,
    .tp_methods = Metadata_methods,
    .tp_name = "maxminddb.Metadata",
};

static PyMethodDef MaxMindDB_methods[] = {
    { "Reader", Reader_constructor, METH_VARARGS,
      "Creates a new maxminddb.Reader object" },
    { NULL,     NULL,               0,           NULL}
};

#if PY_MAJOR_VERSION >= 3
static struct PyModuleDef MaxMindDB_module = {
    PyModuleDef_HEAD_INIT,
    .m_name = "maxminddb",
    .m_doc = "This is a module to read MaxMind DB file format",
    .m_methods = MaxMindDB_methods,
};
#endif

static void init_type(PyObject *m, PyTypeObject *type)
{
    Metadata_Type.tp_new = PyType_GenericNew;

    if (PyType_Ready(type) == 0) {
        Py_INCREF(type);
        PyModule_AddObject(m, "maxminddb", (PyObject *)type);
    }
}

MOD_INIT(maxminddb){
    PyObject *m;

#if PY_MAJOR_VERSION >= 3
    m = PyModule_Create(&MaxMindDB_module);
#else
    m = Py_InitModule("maxminddb", MaxMindDB_methods);
#endif

    if (!m) {
        RETURN_MOD_INIT(NULL);
    }

    init_type(m, &Reader_Type);
    init_type(m, &Metadata_Type);

    MaxMindDB_error = PyErr_NewException("maxminddb.InvalidDatabaseError", NULL,
                                         NULL);
    if (MaxMindDB_error == NULL) {
        RETURN_MOD_INIT(NULL);
    }

    Py_INCREF(MaxMindDB_error);
    PyModule_AddObject(m, "InvalidDatabaseError", MaxMindDB_error);

    RETURN_MOD_INIT(m);
}
