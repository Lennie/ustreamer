#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/mman.h>

#include <Python.h>

#include "../src/libs/tools.h" // Just a header without C-sources
#include "../src/libs/memsinksh.h" // No sources again


typedef struct {
	uint64_t	id;
	long double	ts;

	uint8_t	*data;
	size_t	used;
	size_t	allocated;

	unsigned width;
	unsigned height;
	unsigned format;
	unsigned stride;

	bool online;
	bool key;

	long double	grab_ts;
	long double	encode_begin_ts;
	long double	encode_end_ts;
} tmp_frame_s;

typedef struct {
	PyObject_HEAD

	char	*obj;
	double	lock_timeout;
	double	wait_timeout;
	double	drop_same_frames;

	int					fd;
	memsink_shared_s	*mem;

	tmp_frame_s	*tmp_frame;
} MemsinkObject;


#define MEM(_next) self->mem->_next
#define TMP(_next) self->tmp_frame->_next


static void MemsinkObject_destroy_internals(MemsinkObject *self) {
	if (self->mem != NULL) {
		munmap(self->mem, sizeof(memsink_shared_s));
		self->mem = NULL;
	}
	if (self->fd > 0) {
		close(self->fd);
		self->fd = -1;
	}
	if (self->tmp_frame) {
		if (TMP(data)) {
			free(TMP(data));
		}
		free(self->tmp_frame);
		self->tmp_frame = NULL;
	}
}

static int MemsinkObject_init(MemsinkObject *self, PyObject *args, PyObject *kwargs) {
	self->lock_timeout = 1;
	self->wait_timeout = 1;

	static char *kws[] = {"obj", "lock_timeout", "wait_timeout", "drop_same_frames", NULL};
	if (!PyArg_ParseTupleAndKeywords(
		args, kwargs, "s|ddd", kws,
		&self->obj, &self->lock_timeout, &self->wait_timeout, &self->drop_same_frames)) {
		return -1;
	}

#	define SET_DOUBLE(_field, _cond) { \
			if (!(self->_field _cond)) { \
				PyErr_SetString(PyExc_ValueError, #_field " must be " #_cond); \
				return -1; \
			} \
		}

	SET_DOUBLE(lock_timeout, > 0);
	SET_DOUBLE(wait_timeout, > 0);
	SET_DOUBLE(drop_same_frames, >= 0);

#	undef SET_DOUBLE

	A_CALLOC(self->tmp_frame, 1);
	TMP(allocated) = 512 * 1024;
	A_REALLOC(TMP(data), TMP(allocated));

	if ((self->fd = shm_open(self->obj, O_RDWR, 0)) == -1) {
		PyErr_SetFromErrno(PyExc_OSError);
		goto error;
	}

	if ((self->mem = mmap(
		NULL,
		sizeof(memsink_shared_s),
		PROT_READ | PROT_WRITE,
		MAP_SHARED,
		self->fd,
		0
	)) == MAP_FAILED) {
		PyErr_SetFromErrno(PyExc_OSError);
		self->mem = NULL;
		goto error;
	}
	if (self->mem == NULL) {
		PyErr_SetString(PyExc_RuntimeError, "Memory mapping is NULL"); \
		goto error;
	}

	return 0;

	error:
		MemsinkObject_destroy_internals(self);
		return -1;
}

static PyObject *MemsinkObject_repr(MemsinkObject *self) {
	char repr[1024];
	snprintf(repr, 1023, "<Memsink(%s)>", self->obj);
	return Py_BuildValue("s", repr);
}

static void MemsinkObject_dealloc(MemsinkObject *self) {
	MemsinkObject_destroy_internals(self);
	PyObject_Del(self);
}

static PyObject *MemsinkObject_close(MemsinkObject *self, PyObject *Py_UNUSED(ignored)) {
	MemsinkObject_destroy_internals(self);
	Py_RETURN_NONE;
}

static PyObject *MemsinkObject_enter(MemsinkObject *self, PyObject *Py_UNUSED(ignored)) {
	Py_INCREF(self);
	return (PyObject *)self;
}

static PyObject *MemsinkObject_exit(MemsinkObject *self, PyObject *Py_UNUSED(ignored)) {
	return PyObject_CallMethod((PyObject *)self, "close", "");
}

static int wait_frame(MemsinkObject *self) {
	long double deadline_ts = get_now_monotonic() + self->wait_timeout;

#	define RETURN_OS_ERROR { \
			Py_BLOCK_THREADS \
			PyErr_SetFromErrno(PyExc_OSError); \
			return -1; \
		}

	long double now;
	do {
		Py_BEGIN_ALLOW_THREADS

		int retval = flock_timedwait_monotonic(self->fd, self->lock_timeout);
		now = get_now_monotonic();

		if (retval < 0 && errno != EWOULDBLOCK) {
			RETURN_OS_ERROR;

		} else if (retval == 0) {
			if (MEM(magic) == MEMSINK_MAGIC && MEM(version) == MEMSINK_VERSION && TMP(id) != MEM(id)) {
				if (self->drop_same_frames > 0) {
#					define CMP(_field) (TMP(_field) == MEM(_field))
					if (
						CMP(used)
						&& CMP(width)
						&& CMP(height)
						&& CMP(format)
						&& CMP(stride)
						&& CMP(online)
						&& CMP(key)
						&& (TMP(ts) + self->drop_same_frames > now)
						&& !memcmp(TMP(data), MEM(data), MEM(used))
					) {
						TMP(id) = MEM(id);
						goto drop;
					}
#					undef CMP
				}

				Py_BLOCK_THREADS
				return 0;
			}

			if (flock(self->fd, LOCK_UN) < 0) {
				RETURN_OS_ERROR;
			}
		}

		drop:

		if (usleep(1000) < 0) {
			RETURN_OS_ERROR;
		}

		Py_END_ALLOW_THREADS

		if (PyErr_CheckSignals() < 0) {
			return -1;
		}
	} while (now < deadline_ts);

#	undef RETURN_OS_ERROR

	return -2;
}

static PyObject *MemsinkObject_wait_frame(MemsinkObject *self, PyObject *Py_UNUSED(ignored)) {
	if (self->mem == NULL || self->fd <= 0) {
		PyErr_SetString(PyExc_RuntimeError, "Closed");
		return NULL;
	}

	switch (wait_frame(self)) {
		case 0: break;
		case -2: Py_RETURN_NONE;
		default: return NULL;
	}

#	define COPY(_field) TMP(_field) = MEM(_field)
	COPY(width);
	COPY(height);
	COPY(format);
	COPY(stride);
	COPY(online);
	COPY(key);
	COPY(grab_ts);
	COPY(encode_begin_ts);
	COPY(encode_end_ts);
	COPY(used);
#	undef COPY

	if (TMP(allocated) < MEM(used)) {
		size_t size = MEM(used) + (512 * 1024);
		A_REALLOC(TMP(data), size);
		TMP(allocated) = size;
	}
	memcpy(TMP(data), MEM(data), MEM(used));
	TMP(used) = MEM(used);

	TMP(id) = MEM(id);
	TMP(ts) = get_now_monotonic();
	MEM(last_client_ts) = TMP(ts);

	if (flock(self->fd, LOCK_UN) < 0) {
		return PyErr_SetFromErrno(PyExc_OSError);
	}

	PyObject *dict_frame = PyDict_New();
	if (dict_frame  == NULL) {
		return NULL;
	}

#	define SET_VALUE(_key, _maker) { \
			PyObject *_tmp = _maker; \
			if (_tmp == NULL) { \
				return NULL; \
			} \
			if (PyDict_SetItemString(dict_frame, _key, _tmp) < 0) { \
				Py_DECREF(_tmp); \
				return NULL; \
			} \
			Py_DECREF(_tmp); \
		}
#	define SET_NUMBER(_key, _from, _to) SET_VALUE(#_key, Py##_to##_From##_from(TMP(_key)))

	SET_NUMBER(width, Long, Long);
	SET_NUMBER(height, Long, Long);
	SET_NUMBER(format, Long, Long);
	SET_NUMBER(stride, Long, Long);
	SET_NUMBER(online, Long, Bool);
	SET_NUMBER(key, Long, Bool);
	SET_NUMBER(grab_ts, Double, Float);
	SET_NUMBER(encode_begin_ts, Double, Float);
	SET_NUMBER(encode_end_ts, Double, Float);
	SET_VALUE("data", PyBytes_FromStringAndSize((const char *)TMP(data), TMP(used)));

#	undef SET_NUMBER
#	undef SET_VALUE

	return dict_frame;
}

static PyObject *MemsinkObject_is_opened(MemsinkObject *self, PyObject *Py_UNUSED(ignored)) {
	return PyBool_FromLong(self->mem != NULL && self->fd > 0);
}

#define FIELD_GETTER(_field, _from, _to) \
	static PyObject *MemsinkObject_getter_##_field(MemsinkObject *self, void *Py_UNUSED(closure)) { \
		return Py##_to##_From##_from(self->_field); \
	}

FIELD_GETTER(obj, String, Unicode)
FIELD_GETTER(lock_timeout, Double, Float)
FIELD_GETTER(wait_timeout, Double, Float)
FIELD_GETTER(drop_same_frames, Double, Float)

#undef FIELD_GETTER

static PyMethodDef MemsinkObject_methods[] = {
#	define ADD_METHOD(_name, _method, _flags) \
		{.ml_name = _name, .ml_meth = (PyCFunction)MemsinkObject_##_method, .ml_flags = (_flags)}
	ADD_METHOD("close", close, METH_NOARGS),
	ADD_METHOD("__enter__", enter, METH_NOARGS),
	ADD_METHOD("__exit__", exit, METH_VARARGS),
	ADD_METHOD("wait_frame", wait_frame, METH_NOARGS),
	ADD_METHOD("is_opened", is_opened, METH_NOARGS),
	{},
#	undef ADD_METHOD
};

static PyGetSetDef MemsinkObject_getsets[] = {
#	define ADD_GETTER(_field) {.name = #_field, .get = (getter)MemsinkObject_getter_##_field}
	ADD_GETTER(obj),
	ADD_GETTER(lock_timeout),
	ADD_GETTER(wait_timeout),
	ADD_GETTER(drop_same_frames),
	{},
#	undef ADD_GETTER
};

static PyTypeObject MemsinkType = {
	PyVarObject_HEAD_INIT(NULL, 0)
	.tp_name		= "ustreamer.Memsink",
	.tp_basicsize	= sizeof(MemsinkObject),
	.tp_flags		= Py_TPFLAGS_DEFAULT,
	.tp_new			= PyType_GenericNew,
	.tp_init		= (initproc)MemsinkObject_init,
	.tp_dealloc		= (destructor)MemsinkObject_dealloc,
	.tp_repr		= (reprfunc)MemsinkObject_repr,
	.tp_methods		= MemsinkObject_methods,
	.tp_getset		= MemsinkObject_getsets,
};

static PyModuleDef ustreamer_Module = {
	PyModuleDef_HEAD_INIT,
	.m_name = "ustreamer",
	.m_size = -1,
};

PyMODINIT_FUNC PyInit_ustreamer(void) { // cppcheck-suppress unusedFunction
	PyObject *module = PyModule_Create(&ustreamer_Module);
	if (module == NULL) {
		return NULL;
	}

	if (PyType_Ready(&MemsinkType) < 0) {
		return NULL;
	}

	Py_INCREF(&MemsinkType);

	if (PyModule_AddObject(module, "Memsink", (PyObject *)&MemsinkType) < 0) {
		return NULL;
	}

	return module;
}

#undef TMP
#undef MEM
