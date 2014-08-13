#include <Python.h>
#include <structmember.h>

#include <libnetconf.h>

#include "netconf.h"

typedef struct {
	PyObject_HEAD
	struct nc_session* session;
} ncSessionObject;

/* from netconf.c */
extern PyObject *libnetconfError;

static PyMemberDef ncSessionMembers[] = {
	{NULL}  /* Sentinel */
};

static void ncSessionFree(ncSessionObject *self)
{
	PyObject *err_type, *err_value, *err_traceback;

	/* save the current exception state */
	PyErr_Fetch(&err_type, &err_value, &err_traceback);

	nc_session_free(self->session);

	/* restore the saved exception state */
	PyErr_Restore(err_type, err_value, err_traceback);

	Py_TYPE(self)->tp_free((PyObject*)self);
}

static PyObject *ncSessionNew(PyTypeObject *type, PyObject *args, PyObject *keywords)
{
	ncSessionObject *self;

	self = (ncSessionObject *)type->tp_alloc(type, 0);
	if (self != NULL) {
		self->session = NULL;
	}

	return (PyObject *)self;
}

/* rpc parameter is freed after the function call */
static int op_send_recv(ncSessionObject *self, nc_rpc* rpc, char **data)
{
	nc_reply *reply = NULL;
	int ret = EXIT_SUCCESS;

	/* send the request and get the reply */
	switch (nc_session_send_recv(self->session, rpc, &reply)) {
	case NC_MSG_UNKNOWN:
		if (nc_session_get_status(self->session) != NC_SESSION_STATUS_WORKING) {
			PyErr_SetString(libnetconfError, "Session damaged, closing.");
			ncSessionFree(self);
		}
		ret = EXIT_FAILURE;
		break;
	case NC_MSG_NONE:
		/* error occurred, but processed by callback */
		break;
	case NC_MSG_REPLY:
		switch (nc_reply_get_type(reply)) {
		case NC_REPLY_OK:
			break;
		case NC_REPLY_DATA:
			if (data) {
				*data = nc_reply_get_data (reply);
			}
			break;
		case NC_REPLY_ERROR:
			ret = EXIT_FAILURE;
			break;
		default:
			PyErr_SetString(libnetconfError, "Unexpected operation result.");
			ret = EXIT_FAILURE;
			break;
		}
		break;
	default:
		PyErr_SetString(libnetconfError, "Unknown error occurred.");
		ret = EXIT_FAILURE;
		break;
	}
	nc_rpc_free(rpc);
	nc_reply_free(reply);

	return (ret);
}

static PyObject *get_common(ncSessionObject *self, const char *filter, int wdmode, int datastore)
{
	char *data = NULL;
	struct nc_filter *st_filter = NULL;
	nc_rpc *rpc = NULL;
	PyObject *result = NULL;

	/* create filter if specified */
	if (filter) {
		if ((st_filter = nc_filter_new(NC_FILTER_SUBTREE, filter)) == NULL) {
			return (NULL);
		}
	}

	/* check datastore */
	switch(datastore) {
	case NC_DATASTORE_STARTUP:
		if (!nc_cpblts_enabled(self->session, NETCONF_CAP_STARTUP)) {
			PyErr_SetString(libnetconfError, ":startup capability not supported.");
			return (NULL);
		}
		break;
	case NC_DATASTORE_CANDIDATE:
		if (!nc_cpblts_enabled(self->session, NETCONF_CAP_CANDIDATE)) {
			PyErr_SetString(libnetconfError, ":candidate capability not supported.");
			return (NULL);
		}
		break;
	}

	/* create RPC */
	if (datastore == NC_DATASTORE_ERROR) {
		rpc = nc_rpc_get(st_filter);
	} else {
		rpc = nc_rpc_getconfig(datastore, st_filter);
	}
	nc_filter_free(st_filter);
	if (!rpc) {
		return(NULL);
	}

	/* set with defaults settings */
	if (wdmode) {
		if (nc_rpc_capability_attr(rpc, NC_CAP_ATTR_WITHDEFAULTS_MODE, wdmode) != EXIT_SUCCESS) {
			nc_rpc_free(rpc);
			return (NULL);
		}
	}

	/* send request ... */
	if (op_send_recv(self, rpc, &data) == EXIT_SUCCESS && data != NULL) {
		/* ... and prepare the result */
		result = PyUnicode_FromString(data);
		free(data);
	}

	return (result);
}

static PyObject *ncOpGet(ncSessionObject *self, PyObject *args, PyObject *keywords)
{
	const char *filter = NULL;
	int wdmode = NCWD_MODE_NOTSET;

	char *kwlist[] = {"filter", "wd", NULL};

	/* Get input parameters */
	if (! PyArg_ParseTupleAndKeywords(args, keywords, "|zi", kwlist, &filter, &wdmode)) {
		return (NULL);
	}

	return (get_common(self, filter, wdmode, NC_DATASTORE_ERROR));
}

static PyObject *ncOpGetConfig(ncSessionObject *self, PyObject *args, PyObject *keywords)
{
	const char *filter = NULL;
	int wdmode = NCWD_MODE_NOTSET;
	int datastore = NC_DATASTORE_ERROR;
	char *kwlist[] = {"datastore", "filter", "wd", NULL};

	/* Get input parameters */
	if (! PyArg_ParseTupleAndKeywords(args, keywords, "i|zi", kwlist, &datastore, &filter, &wdmode)) {
		return (NULL);
	}

	if (datastore != NC_DATASTORE_RUNNING &&
			datastore != NC_DATASTORE_STARTUP &&
			datastore != NC_DATASTORE_CANDIDATE) {
		PyErr_SetString(PyExc_ValueError, "Invalid \'datastore\' value.");
		return (NULL);
	}

	return (get_common(self, filter, wdmode, datastore));
}

static PyObject *ncOpDeleteConfig(ncSessionObject *self, PyObject *args, PyObject *keywords)
{
	int target = NC_DATASTORE_ERROR;
	PyObject *PyTarget;
	nc_rpc *rpc = NULL;
	char *url = NULL;
	char *kwlist[] = {"target", NULL};

	/* Get input parameters */
	if (! PyArg_ParseTupleAndKeywords(args, keywords, "O", kwlist, &PyTarget)) {
		return (NULL);
	}

	/* get target type */
	if (strcmp(Py_TYPE(PyTarget)->tp_name, "int") == 0) {
		if (!PyArg_Parse(PyTarget, "i", &target)) {
			return (NULL);
		}
	} else if (strcmp(Py_TYPE(PyTarget)->tp_name, "str") == 0) {
		if (!PyArg_Parse(PyTarget, "s", &url)) {
			return (NULL);
		}
		if (strcasestr(url, "://") != NULL) {
			target = NC_DATASTORE_URL;
		} else {
			PyErr_SetString(PyExc_ValueError, "Invalid \'target\' value.");
			return (NULL);
		}
	}

	/* create RPC */
	rpc = nc_rpc_deleteconfig(target, url);

	/* send request ... */
	if (op_send_recv(self, rpc, NULL) == EXIT_SUCCESS) {
		/* ... and return the result */
		Py_RETURN_TRUE;
	} else {
		Py_RETURN_FALSE;
	}
}

static PyObject *ncOpCopyConfig(ncSessionObject *self, PyObject *args, PyObject *keywords)
{
	int wdmode = NCWD_MODE_NOTSET;
	int source = NC_DATASTORE_ERROR, target = NC_DATASTORE_ERROR;
	char *data1 = NULL, *data2 = NULL;
	int i;
	PyObject *PySource, *PyTarget;
	nc_rpc *rpc = NULL;
	char *kwlist[] = {"source", "target", "wd", NULL};

	/* Get input parameters */
	if (! PyArg_ParseTupleAndKeywords(args, keywords, "OO|i", kwlist, &PySource, &PyTarget, &wdmode)) {
		return (NULL);
	}

	/* get source type */
	if (strcmp(Py_TYPE(PySource)->tp_name, "int") == 0) {
		if (!PyArg_Parse(PySource, "i", &source)) {
			return (NULL);
		}
	} else if (strcmp(Py_TYPE(PySource)->tp_name, "str") == 0) {
		if (!PyArg_Parse(PySource, "s", &data1)) {
			return (NULL);
		}
		for (i = 0; data1[i] != '\0' && isspace(data1[i]); i++);
		if (data1[i] == '<') {
			source = NC_DATASTORE_CONFIG;
		} else {
			if (strcasestr(data1, "://") != NULL) {
				source = NC_DATASTORE_URL;
			} else {
				PyErr_SetString(PyExc_ValueError, "Invalid \'source\' value.");
				return (NULL);
			}
		}
	}

	/* get target type */
	if (strcmp(Py_TYPE(PyTarget)->tp_name, "int") == 0) {
		if (!PyArg_Parse(PyTarget, "i", &target)) {
			return (NULL);
		}
	} else if (strcmp(Py_TYPE(PyTarget)->tp_name, "str") == 0) {
		if (!PyArg_Parse(PyTarget, "s", &data2)) {
			return (NULL);
		}
		if (strcasestr(data2, "://") != NULL) {
			target = NC_DATASTORE_URL;
		} else {
			PyErr_SetString(PyExc_ValueError, "Invalid \'target\' value.");
			return (NULL);
		}
		if (data1 == NULL) {
			/* 3rd and 4th argument to nc_rpc_copyconfig() are variadic - if the
			 * source is not config or URL, the target URL must be placed as
			 * 3rd argument, not 4th
			 */
			data1 = data2;
			data2 = NULL;
		}
	}

	/* create RPC */
	rpc = nc_rpc_copyconfig(source, target, data1, data2);

	/* set with defaults settings */
	if (wdmode) {
		if (nc_rpc_capability_attr(rpc, NC_CAP_ATTR_WITHDEFAULTS_MODE, wdmode) != EXIT_SUCCESS) {
			nc_rpc_free(rpc);
			return (NULL);
		}
	}

	/* send request ... */
	if (op_send_recv(self, rpc, NULL) == EXIT_SUCCESS) {
		/* ... and return the result */
		Py_RETURN_TRUE;
	} else {
		Py_RETURN_FALSE;
	}
}

static PyObject *lock_common(ncSessionObject *self, PyObject *args, PyObject *keywords, nc_rpc* (func)(NC_DATASTORE))
{
	int datastore = NC_DATASTORE_ERROR;
	nc_rpc *rpc = NULL;
	char *kwlist[] = {"datastore", NULL};

	/* Get input parameters */
	if (! PyArg_ParseTupleAndKeywords(args, keywords, "i|zi", kwlist, &datastore)) {
		return (NULL);
	}

	/* check datastore */
	switch(datastore) {
	case NC_DATASTORE_STARTUP:
		if (!nc_cpblts_enabled(self->session, NETCONF_CAP_STARTUP)) {
			PyErr_SetString(libnetconfError, ":startup capability not supported.");
			return (NULL);
		}
		break;
	case NC_DATASTORE_CANDIDATE:
		if (!nc_cpblts_enabled(self->session, NETCONF_CAP_CANDIDATE)) {
			PyErr_SetString(libnetconfError, ":candidate capability not supported.");
			return (NULL);
		}
		break;
	}

	/* create RPC */
	rpc = func(datastore);

	/* send request ... */
	if (op_send_recv(self, rpc, NULL) == EXIT_SUCCESS) {
		/* ... and return the result */
		Py_RETURN_TRUE;
	} else {
		Py_RETURN_FALSE;
	}
}

static PyObject *ncOpLock(ncSessionObject *self, PyObject *args, PyObject *keywords)
{
	return (lock_common(self, args, keywords, nc_rpc_lock));
}

static PyObject *ncOpUnlock(ncSessionObject *self, PyObject *args, PyObject *keywords)
{
	return (lock_common(self, args, keywords, nc_rpc_unlock));
}


static PyObject *ncSessionConnect(PyObject *cls, PyObject *args, PyObject *keywords)
{
	PyObject *result = NULL;
	char *host = NULL, *user = NULL, *transport_s = "ssh", *version = NULL;
	unsigned short port = 830;
	PyObject *PyCpblts = NULL;

	char *kwlist[] = {"host", "port", "user", "transport", "version", NULL};

	/* Get input parameters */
	if (! PyArg_ParseTupleAndKeywords(args, keywords, "s|Hzzs", kwlist, &host, &port, &user, &transport_s, &version)) {
		return (NULL);
	}

	if (!version) {
		/* by default, support all versions */
		PyCpblts = PyList_New(2);
		PyList_SET_ITEM(PyCpblts, 0, PyUnicode_FromString(NETCONF_CAP_BASE10));
		PyList_SET_ITEM(PyCpblts, 1, PyUnicode_FromString(NETCONF_CAP_BASE11));
	} else {
		PyCpblts = PyList_New(1);
		PyList_SET_ITEM(PyCpblts, 0, PyUnicode_FromString(version));
	}

	result = PyObject_CallFunction(cls, "sHssO", host, port, user, transport_s, PyCpblts);

	return(result);
}

static PyObject *ncSessionAccept(PyObject *cls, PyObject *args, PyObject *keywords)
{
	PyObject *result = NULL;
	PyObject *PyCpblts = NULL;
	const char *user = NULL;
	int fd_in = STDIN_FILENO, fd_out = STDOUT_FILENO;

	char *kwlist[] = {"user", "capabilities", "fd_in", "fd_out", NULL};

	/* Get input parameters */
	if (! PyArg_ParseTupleAndKeywords(args, keywords, "zOii", kwlist, &user, &PyCpblts, &fd_in, &fd_out)) {
		return (NULL);
	}

	result = PyObject_CallFunction(cls, "sHssOii", NULL, 0, user, NULL, PyCpblts, fd_in, fd_out);

	return(result);
}

static int ncSessionInit(ncSessionObject *self, PyObject *args, PyObject *keywords)
{
	const char *host = NULL, *user = NULL, *transport_s = NULL;
	unsigned short port = 830;
	PyObject *PyCpblts = NULL;
	struct nc_session *session;
	struct nc_cpblts *cpblts = NULL;
	char* item = NULL;
	Py_ssize_t l, i;
	int ret;
	int fd_in = STDIN_FILENO, fd_out = STDOUT_FILENO;

	char *kwlist[] = {"host", "port", "user", "transport", "capabilities", "fd_in", "fd_out", NULL};

	/* Get input parameters */
	if (! PyArg_ParseTupleAndKeywords(args, keywords, "|zHzzO!ii", kwlist, &host, &port, &user, &transport_s, &PyList_Type, &PyCpblts, &fd_in, &fd_out)) {
		return -1;
	}

	if (host != NULL) {
		/* Client side */
		if (transport_s && strcasecmp(transport_s, NETCONF_TRANSPORT_TLS) == 0) {
			ret = nc_session_transport(NC_TRANSPORT_TLS);
		} else {
			ret = nc_session_transport(NC_TRANSPORT_SSH);
		}
		if (ret != EXIT_SUCCESS) {
			return -1;
		}
	}

	if (PyCpblts != NULL) {
		cpblts = nc_cpblts_new(NULL);
		if (PyList_Check(PyCpblts) && ((l = PyList_Size(PyCpblts)) > 0)) {
			for (i = 0; i < l; i++) {
				PyObject *PyUni = PyList_GetItem(PyCpblts, i);
				Py_INCREF(PyUni);
				if (! PyUnicode_Check(PyUni)) {
					PyErr_SetString(PyExc_TypeError, "Capabilities list must contain strings.");
					nc_cpblts_free(cpblts);
					Py_DECREF(PyUni);
					return -1;
				}
				PyObject *PyStr = PyUnicode_AsEncodedString(PyUni, "UTF-8", NULL);
				Py_DECREF(PyUni);
				if (PyStr == NULL) {
					nc_cpblts_free(cpblts);
					return -1;
				}
				item = PyBytes_AsString(PyStr);
				if (item == NULL) {
					nc_cpblts_free(cpblts);
					Py_DECREF(PyStr);
					return -1;
				}
				ret = nc_cpblts_add(cpblts, item);
				Py_DECREF(PyStr);
				if (ret != EXIT_SUCCESS) {
					nc_cpblts_free(cpblts);
					return -1;
				}
			}
		}
	}

	if (host != NULL) {
		/* Client side */
		session = nc_session_connect(host, port, user, cpblts);
	} else {
		/* Server side */
		session = nc_session_accept_inout(cpblts, user, fd_in, fd_out);
	}

	nc_cpblts_free(cpblts);

	if (session == NULL) {
		return -1;
	}

	nc_session_free(self->session);
	self->session = session;

	return 0;
}

static PyObject *ncSessionGetId(ncSessionObject *self, void *closure)
{
	return PyUnicode_FromFormat("%s", nc_session_get_id(self->session));
}

static PyObject *ncSessionGetHost(ncSessionObject *self, void *closure)
{
	return PyUnicode_FromFormat("%s", nc_session_get_host(self->session));
}

static PyObject *ncSessionGetPort(ncSessionObject *self, void *closure)
{
	return PyUnicode_FromFormat("%s", nc_session_get_port(self->session));
}

static PyObject *ncSessionGetUser(ncSessionObject *self, void *closure)
{
	return PyUnicode_FromFormat("%s", nc_session_get_user(self->session));
}

static PyObject *ncSessionGetTransport(ncSessionObject *self, void *closure)
{
	NC_TRANSPORT t = nc_session_get_transport(self->session);
	return PyUnicode_FromFormat("%s", (t == NC_TRANSPORT_TLS) ? "TLS" : "SSH");
}

static PyObject *ncSessionStr(ncSessionObject *self)
{
	return PyUnicode_FromFormat("NETCONF Session %s to %s:%s",
			nc_session_get_id(self->session),
			nc_session_get_host(self->session),
			nc_session_get_port(self->session));
}

static PyObject *ncSessionGetCapabilities(ncSessionObject *self, void *closure)
{
	struct nc_cpblts* cpblts;
	PyObject *list;
	const char *item;
	ssize_t pos;

	cpblts = nc_session_get_cpblts(self->session);
	if (cpblts == NULL) {
		return (NULL);
	}

	list = PyList_New(nc_cpblts_count(cpblts));
	nc_cpblts_iter_start(cpblts);
	for (pos = 0; pos < nc_cpblts_count(cpblts); pos++) {
		item = nc_cpblts_iter_next(cpblts);
		PyList_SetItem(list, pos, PyUnicode_FromFormat("%s", item));
	}

	return list;
}

static PyObject *ncSessionGetVersion(ncSessionObject *self, void *closure)
{
	int ver;
	const char* ver_s;

	ver = nc_session_get_version(self->session);
	switch(ver) {
	case 0:
		ver_s = "1.0";
		break;
	case 1:
		ver_s = "1.1";
		break;
	default:
		ver_s = "unknown";
		break;
	}

	return PyUnicode_FromFormat("%s", ver_s);
}

static PyGetSetDef ncSessionGetSeters[] = {
    {"id", (getter)ncSessionGetId, NULL, "NETCONF Session id.", NULL},
    {"host", (getter)ncSessionGetHost, NULL, "Host where the NETCONF Session is connected.", NULL},
    {"port", (getter)ncSessionGetPort, NULL, "Port number where the NETCONF Session is connected.", NULL},
    {"user", (getter)ncSessionGetUser, NULL, "Username of the user connected with the NETCONF Session.", NULL},
    {"transport", (getter)ncSessionGetTransport, NULL, "Transport protocol used for the NETCONF Session.", NULL},
    {"version", (getter)ncSessionGetVersion, NULL, "NETCONF Protocol version used for the NETCONF Session.", NULL},
    {"capabilities", (getter)ncSessionGetCapabilities, NULL, "Capabilities of the NETCONF Session.", NULL},
    {NULL}  /* Sentinel */
};

static PyMethodDef ncSessionMethods[] = {
	{"connect", (PyCFunction)ncSessionConnect,
		METH_VARARGS | METH_KEYWORDS | METH_CLASS,
		PyDoc_STR("ncSessionConnect(host, port, username, transport, version)-> ncSessionObject\n\n"
				"Create NETCONF session connecting to a NETCONF server.")},
	{"accept", (PyCFunction)ncSessionAccept,
		METH_VARARGS | METH_KEYWORDS | METH_CLASS,
		PyDoc_STR("Create NETCONF session accepting connection from a NETCONF client.")},
	{"get", (PyCFunction)ncOpGet,
		METH_VARARGS | METH_KEYWORDS,
		PyDoc_STR("Execute NETCONF <get> RPC.")},
	{"getConfig", (PyCFunction)ncOpGetConfig,
		METH_VARARGS | METH_KEYWORDS,
		PyDoc_STR("Execute NETCONF <get-config> RPC.")},
	{"lock", (PyCFunction)ncOpLock,
		METH_VARARGS | METH_KEYWORDS,
		PyDoc_STR("Execute NETCONF <lock> RPC.")},
	{"unlock", (PyCFunction)ncOpUnlock,
		METH_VARARGS | METH_KEYWORDS,
		PyDoc_STR("Execute NETCONF <unlock> RPC.")},
	{"copyConfig", (PyCFunction)ncOpCopyConfig,
		METH_VARARGS | METH_KEYWORDS,
		PyDoc_STR("Execute NETCONF <copy-config> RPC.")},
	{"deleteConfig", (PyCFunction)ncOpDeleteConfig,
		METH_VARARGS | METH_KEYWORDS,
		PyDoc_STR("Execute NETCONF <delete-config> RPC.")},
	{NULL, NULL, 0, NULL}
};

PyDoc_STRVAR(sessionDoc,
"Create the NETCONF Session\n"
"netconf.Session(host, [port, user, transport, capabilities]) -> connect to the NETCONF server\n"
"netconf.Session([user, capabilities, fd_in, fd_out]) -> accept incoming connection from fd_in (stdin)\n\n"
"Arguments:\n"
"\thost         - hostname or address of the server\n"
"\tport         - port where to connect to, by default 830\n"
"\tuser         - username, by default the currently logged user\n"
"\ttransport    - NETCONF transport protocol, by default SSH\n"
"\tcapbilities  - list of strings with the supported NETCONF capabilities,\n"
"\tfd_in        - input file descriptor for reading decrypted data on server side (stdin by default),\n"
"\tfd_out       - output file descriptor for writing unencrypted data on server side (stdout by default),\n"
"\t               by default generated by the libnetconf\n");

PyTypeObject ncSessionType = {
		PyVarObject_HEAD_INIT(NULL, 0)
		"netconf.Session", /* tp_name */
		sizeof(ncSessionObject), /* tp_basicsize */
		0, /* tp_itemsize */
		(destructor) ncSessionFree, /* tp_dealloc */
		0, /* tp_print */
		0, /* tp_getattr */
		0, /* tp_setattr */
		0, /* tp_reserved */
		(reprfunc)ncSessionStr, /* tp_repr */
		0, /* tp_as_number */
		0, /* tp_as_sequence */
		0, /* tp_as_mapping */
		0, /* tp_hash  */
		0, /* tp_call */
		(reprfunc)ncSessionStr, /* tp_str */
		0, /* tp_getattro */
		0, /* tp_setattro */
		0, /* tp_as_buffer */
		Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE, /* tp_flags */
		sessionDoc, /* tp_doc */
		0, /* tp_traverse */
		0, /* tp_clear */
		0, /* tp_richcompare */
		0, /* tp_weaklistoffset */
		0, /* tp_iter */
		0, /* tp_iternext */
		ncSessionMethods, /* tp_methods */
		ncSessionMembers, /* tp_members */
		ncSessionGetSeters, /* tp_getset */
		0, /* tp_base */
		0, /* tp_dict */
		0, /* tp_descr_get */
		0, /* tp_descr_set */
		0, /* tp_dictoffset */
		(initproc) ncSessionInit, /* tp_init */
		0, /* tp_alloc */
		ncSessionNew, /* tp_new */
};

