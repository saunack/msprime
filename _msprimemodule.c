/*
** Copyright (C) 2014-2020 University of Oxford
**
** This file is part of msprime.
**
** msprime is free software: you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation, either version 3 of the License, or
** (at your option) any later version.
**
** msprime is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with msprime.  If not, see <http://www.gnu.org/licenses/>.
*/

#define PY_SSIZE_T_CLEAN
#define NPY_NO_DEPRECATED_API NPY_1_7_API_VERSION

#include <Python.h>
#include <structmember.h>
#include <numpy/arrayobject.h>
#include <float.h>

#include <gsl/gsl_version.h>
#include <gsl/gsl_math.h>
#include <gsl/gsl_errno.h>
#include <gsl/gsl_randist.h>

#include "msprime.h"
#include "likelihood.h"

/* We keep a reference to the gsl_error_handler so it can be restored if needed */
static gsl_error_handler_t *old_gsl_error_handler;

static PyObject *MsprimeInputError;
static PyObject *MsprimeLibraryError;

/* A lightweight wrapper for a table collection. This serves only as a wrapper
 * around a pointer and a way move to data in-and-out of the low level structures
 * via the canonical dictionary encoding.
 *
 * Copied from _tskitmodule.c 2018-12-20.
 */
typedef struct {
    PyObject_HEAD
    tsk_table_collection_t *tables;
} LightweightTableCollection;

typedef struct {
    PyObject_HEAD
    unsigned long seed;
    gsl_rng* rng;
} RandomGenerator;

typedef struct {
    PyObject_HEAD
    interval_map_t *interval_map;
} IntervalMap;

typedef struct {
    PyObject_HEAD
    mutgen_t *mutgen;
    RandomGenerator *random_generator;
    IntervalMap *rate_map;
} MutationGenerator;

typedef struct {
    PyObject_HEAD
    recomb_map_t *recomb_map;
} RecombinationMap;

typedef struct {
    PyObject_HEAD
    msp_t *sim;
    RecombinationMap *recombination_map;
    RandomGenerator *random_generator;
    LightweightTableCollection *tables;
} Simulator;

static void
handle_library_error(int err)
{
    PyErr_SetString(MsprimeLibraryError, msp_strerror(err));
}

static void
handle_tskit_library_error(int err)
{
    PyErr_SetString(MsprimeLibraryError, tsk_strerror(err));
}

static void
handle_input_error(const char *section, int err)
{
    PyErr_Format(MsprimeInputError, "Input error in %s: %s", section, msp_strerror(err));
}

static int
double_PyArray_converter(PyObject *in, PyObject **out)
{
    PyObject *ret = PyArray_FROMANY(in, NPY_FLOAT64, 1, 1, NPY_ARRAY_IN_ARRAY);
    if (ret == NULL) {
        return NPY_FAIL;
    }
    *out = ret;
    return NPY_SUCCEED;
}

/*
 * Retrieves the PyObject* corresponding the specified key in the
 * specified dictionary.
 *
 * NB This returns a *borrowed reference*, so don't DECREF it!
 */
static PyObject *
get_dict_value(PyObject *dict, const char *key_str)
{
    PyObject *ret = NULL;

    ret = PyDict_GetItemString(dict, key_str);
    if (ret == NULL) {
        PyErr_Format(PyExc_ValueError, "'%s' not specified", key_str);
    }
    return ret;
}

/*
 * Retrieves a number value with the specified key from the specified
 * dictionary.
 */
static PyObject *
get_dict_number(PyObject *dict, const char *key_str)
{
    PyObject *ret = NULL;
    PyObject *value;

    value = get_dict_value(dict, key_str);
    if (value == NULL) {
        goto out;
    }
    if (!PyNumber_Check(value)) {
        PyErr_Format(PyExc_TypeError, "'%s' is not number", key_str);
        goto out;
    }
    ret = value;
out:
    return ret;
}

static int
parse_samples(PyObject *py_samples, Py_ssize_t num_populations,
        Py_ssize_t *num_samples, sample_t **samples)
{
    int ret = -1;
    long tmp_long;
    Py_ssize_t j, n;
    PyObject *sample, *value;
    sample_t *ret_samples = NULL;

    n = PyList_Size(py_samples);
    ret_samples = PyMem_Malloc(n * sizeof(sample_t));
    if (ret_samples == NULL) {
        PyErr_NoMemory();
        goto out;
    }
    for (j = 0; j < n; j++) {
        sample = PyList_GetItem(py_samples, j);
        if (!PyTuple_Check(sample)) {
            PyErr_SetString(PyExc_TypeError, "not a tuple");
            goto out;
        }
        if (PyTuple_Size(sample) != 2) {
            PyErr_SetString(PyExc_ValueError,
                    "sample must be (population,time) tuple");
            goto out;
        }
        value = PyTuple_GetItem(sample, 0);
        if (!PyNumber_Check(value)) {
            PyErr_Format(PyExc_TypeError, "'population' is not number");
            goto out;
        }
        tmp_long = PyLong_AsLong(value);
        if (tmp_long < 0) {
            PyErr_Format(PyExc_ValueError,
                "Negative population ID in sample at index %d", (int) j);
            goto out;
        }
        if (tmp_long >= num_populations) {
            PyErr_Format(PyExc_ValueError,
                "Invalid population reference '%d' in sample at index %d",
                (int) tmp_long, (int) j);
            goto out;
        }
        ret_samples[j].population_id = (population_id_t) tmp_long;
        value = PyTuple_GetItem(sample, 1);
        if (!PyNumber_Check(value)) {
            PyErr_Format(PyExc_TypeError, "'time' is not number");
            goto out;
        }
        ret_samples[j].time = PyFloat_AsDouble(value);
        if (ret_samples[j].time < 0) {
            PyErr_SetString(PyExc_ValueError, "negative times not valid");
            goto out;
        }
    }
    *samples = ret_samples;
    *num_samples = n;
    ret = 0;
    ret_samples = NULL;
out:
    if (ret_samples != NULL) {
        PyMem_Free(ret_samples);
    }
    return ret;
}

static PyObject *
make_metadata(const char *metadata, Py_ssize_t length)
{
    const char *m = metadata == NULL? "": metadata;
    return PyBytes_FromStringAndSize(m, length);
}

static PyObject *
make_node(tsk_node_t *r)
{
    PyObject *ret = NULL;
    PyObject* metadata = make_metadata(r->metadata, (Py_ssize_t) r->metadata_length);
    if (metadata == NULL) {
        goto out;
    }
    ret = Py_BuildValue("IdiiO",
        (unsigned int) r->flags, r->time, (int) r->population, (int) r->individual, metadata);
out:
    Py_XDECREF(metadata);
    return ret;
}

static PyObject *
make_edge(tsk_edge_t *edge)
{
    return Py_BuildValue("ddii",
            edge->left, edge->right, (int) edge->parent, (int) edge->child);
}

static PyObject *
make_migration(tsk_migration_t *r)
{
    int source = r->source == TSK_NULL ? -1: r->source;
    int dest = r->dest == TSK_NULL ? -1: r->dest;
    PyObject *ret = NULL;

    ret = Py_BuildValue("ddiiid",
            r->left, r->right, (int) r->node, source, dest, r->time);
    return ret;
}


/*===================================================================
 * General table code.
 *===================================================================
 */

/* NOTE: this code was copied from _tskitmodule as the efficient way to
 * import and export TableCollection data. It is unlikely to change
 * much over time, but if updates need to be made it would be better
 * to copy the code wholesale. The tests in ``test_dict_encoding.py``
 * are designed to test this code thoroughly, and also come from
 * tskit.
 */

/*
 * Retrieves the PyObject* corresponding the specified key in the
 * specified dictionary. If required is true, raise a TypeError if the
 * value is None.
 *
 * NB This returns a *borrowed reference*, so don't DECREF it!
 */
static PyObject *
get_table_dict_value(PyObject *dict, const char *key_str, bool required)
{
    PyObject *ret = NULL;

    ret = PyDict_GetItemString(dict, key_str);
    if (ret == NULL) {
        PyErr_Format(PyExc_ValueError, "'%s' not specified", key_str);
    }
    if (required && ret == Py_None) {
        PyErr_Format(PyExc_TypeError, "'%s' is required", key_str);
        ret = NULL;
    }
    return ret;
}

static PyArrayObject *
table_read_column_array(PyObject *input, int npy_type, size_t *num_rows, bool check_num_rows)
{
    PyArrayObject *ret = NULL;
    PyArrayObject *array = NULL;
    npy_intp *shape;

    array = (PyArrayObject *) PyArray_FROMANY(input, npy_type, 1, 1, NPY_ARRAY_IN_ARRAY);
    if (array == NULL) {
        goto out;
    }
    shape = PyArray_DIMS(array);
    if (check_num_rows) {
        if (*num_rows != (size_t) shape[0]) {
            PyErr_SetString(PyExc_ValueError, "Input array dimensions must be equal.");
            goto out;
        }
    } else {
        *num_rows = (size_t) shape[0];
    }
    ret = array;
    array = NULL;
out:
    Py_XDECREF(array);
    return ret;
}

static PyArrayObject *
table_read_offset_array(PyObject *input, size_t *num_rows, size_t length, bool check_num_rows)
{
    PyArrayObject *ret = NULL;
    PyArrayObject *array = NULL;
    npy_intp *shape;
    uint32_t *data;

    array = (PyArrayObject *) PyArray_FROMANY(input, NPY_UINT32, 1, 1, NPY_ARRAY_IN_ARRAY);
    if (array == NULL) {
        goto out;
    }
    shape = PyArray_DIMS(array);
    if (! check_num_rows) {
        *num_rows = (size_t) shape[0];
        if (*num_rows == 0) {
            PyErr_SetString(PyExc_ValueError, "Offset arrays must have at least one element");
            goto out;
        }
        *num_rows -= 1;
    }
    if (((size_t) shape[0]) != *num_rows + 1) {
        PyErr_SetString(PyExc_ValueError, "offset columns must have n + 1 rows.");
        goto out;
    }
    data = PyArray_DATA(array);
    if (data[*num_rows] != (uint32_t) length) {
        PyErr_SetString(PyExc_ValueError, "Bad offset column encoding");
        goto out;
    }
    ret = array;
out:
    if (ret == NULL) {
        Py_XDECREF(array);
    }
    return ret;
}

static int
parse_individual_table_dict(tsk_individual_table_t *table, PyObject *dict, bool clear_table)
{
    int err;
    int ret = -1;
    size_t num_rows, metadata_length, location_length;
    char *metadata_data = NULL;
    double *location_data = NULL;
    uint32_t *metadata_offset_data = NULL;
    uint32_t *location_offset_data = NULL;
    PyObject *flags_input = NULL;
    PyArrayObject *flags_array = NULL;
    PyObject *location_input = NULL;
    PyArrayObject *location_array = NULL;
    PyObject *location_offset_input = NULL;
    PyArrayObject *location_offset_array = NULL;
    PyObject *metadata_input = NULL;
    PyArrayObject *metadata_array = NULL;
    PyObject *metadata_offset_input = NULL;
    PyArrayObject *metadata_offset_array = NULL;

    /* Get the input values */
    flags_input = get_table_dict_value(dict, "flags", true);
    if (flags_input == NULL) {
        goto out;
    }
    location_input = get_table_dict_value(dict, "location", false);
    if (location_input == NULL) {
        goto out;
    }
    location_offset_input = get_table_dict_value(dict, "location_offset", false);
    if (location_offset_input == NULL) {
        goto out;
    }
    metadata_input = get_table_dict_value(dict, "metadata", false);
    if (metadata_input == NULL) {
        goto out;
    }
    metadata_offset_input = get_table_dict_value(dict, "metadata_offset", false);
    if (metadata_offset_input == NULL) {
        goto out;
    }

    /* Pull out the arrays */
    flags_array = table_read_column_array(flags_input, NPY_UINT32, &num_rows, false);
    if (flags_array == NULL) {
        goto out;
    }
    if ((location_input == Py_None) != (location_offset_input == Py_None)) {
        PyErr_SetString(PyExc_TypeError,
                "location and location_offset must be specified together");
        goto out;
    }
    if (location_input != Py_None) {
        location_array = table_read_column_array(location_input, NPY_FLOAT64,
                &location_length, false);
        if (location_array == NULL) {
            goto out;
        }
        location_data = PyArray_DATA(location_array);
        location_offset_array = table_read_offset_array(location_offset_input, &num_rows,
                location_length, true);
        if (location_offset_array == NULL) {
            goto out;
        }
        location_offset_data = PyArray_DATA(location_offset_array);
    }
    if ((metadata_input == Py_None) != (metadata_offset_input == Py_None)) {
        PyErr_SetString(PyExc_TypeError,
                "metadata and metadata_offset must be specified together");
        goto out;
    }
    if (metadata_input != Py_None) {
        metadata_array = table_read_column_array(metadata_input, NPY_INT8,
                &metadata_length, false);
        if (metadata_array == NULL) {
            goto out;
        }
        metadata_data = PyArray_DATA(metadata_array);
        metadata_offset_array = table_read_offset_array(metadata_offset_input, &num_rows,
                metadata_length, true);
        if (metadata_offset_array == NULL) {
            goto out;
        }
        metadata_offset_data = PyArray_DATA(metadata_offset_array);
    }

    if (clear_table) {
        err = tsk_individual_table_clear(table);
        if (err != 0) {
            handle_library_error(err);
            goto out;
        }
    }
    err = tsk_individual_table_append_columns(table, num_rows,
            PyArray_DATA(flags_array),
            location_data, location_offset_data,
            metadata_data, metadata_offset_data);
    if (err != 0) {
        handle_library_error(err);
        goto out;
    }
    ret = 0;
out:
    Py_XDECREF(flags_array);
    Py_XDECREF(location_array);
    Py_XDECREF(location_offset_array);
    Py_XDECREF(metadata_array);
    Py_XDECREF(metadata_offset_array);
    return ret;
}

static int
parse_node_table_dict(tsk_node_table_t *table, PyObject *dict, bool clear_table)
{
    int err;
    int ret = -1;
    size_t num_rows, metadata_length;
    char *metadata_data = NULL;
    uint32_t *metadata_offset_data = NULL;
    void *population_data = NULL;
    void *individual_data = NULL;
    PyObject *time_input = NULL;
    PyArrayObject *time_array = NULL;
    PyObject *flags_input = NULL;
    PyArrayObject *flags_array = NULL;
    PyObject *population_input = NULL;
    PyArrayObject *population_array = NULL;
    PyObject *individual_input = NULL;
    PyArrayObject *individual_array = NULL;
    PyObject *metadata_input = NULL;
    PyArrayObject *metadata_array = NULL;
    PyObject *metadata_offset_input = NULL;
    PyArrayObject *metadata_offset_array = NULL;

    /* Get the input values */
    flags_input = get_table_dict_value(dict, "flags", true);
    if (flags_input == NULL) {
        goto out;
    }
    time_input = get_table_dict_value(dict, "time", true);
    if (time_input == NULL) {
        goto out;
    }
    population_input = get_table_dict_value(dict, "population", false);
    if (population_input == NULL) {
        goto out;
    }
    individual_input = get_table_dict_value(dict, "individual", false);
    if (individual_input == NULL) {
        goto out;
    }
    metadata_input = get_table_dict_value(dict, "metadata", false);
    if (metadata_input == NULL) {
        goto out;
    }
    metadata_offset_input = get_table_dict_value(dict, "metadata_offset", false);
    if (metadata_offset_input == NULL) {
        goto out;
    }

    /* Create the arrays */
    flags_array = table_read_column_array(flags_input, NPY_UINT32, &num_rows, false);
    if (flags_array == NULL) {
        goto out;
    }
    time_array = table_read_column_array(time_input, NPY_FLOAT64, &num_rows, true);
    if (time_array == NULL) {
        goto out;
    }
    if (population_input != Py_None) {
        population_array = table_read_column_array(population_input, NPY_INT32,
                &num_rows, true);
        if (population_array == NULL) {
            goto out;
        }
        population_data = PyArray_DATA(population_array);
    }
    if (individual_input != Py_None) {
        individual_array = table_read_column_array(individual_input, NPY_INT32,
                &num_rows, true);
        if (individual_array == NULL) {
            goto out;
        }
        individual_data = PyArray_DATA(individual_array);
    }
    if ((metadata_input == Py_None) != (metadata_offset_input == Py_None)) {
        PyErr_SetString(PyExc_TypeError,
                "metadata and metadata_offset must be specified together");
        goto out;
    }
    if (metadata_input != Py_None) {
        metadata_array = table_read_column_array(metadata_input, NPY_INT8,
                &metadata_length, false);
        if (metadata_array == NULL) {
            goto out;
        }
        metadata_data = PyArray_DATA(metadata_array);
        metadata_offset_array = table_read_offset_array(metadata_offset_input, &num_rows,
                metadata_length, true);
        if (metadata_offset_array == NULL) {
            goto out;
        }
        metadata_offset_data = PyArray_DATA(metadata_offset_array);
    }
    if (clear_table) {
        err = tsk_node_table_clear(table);
        if (err != 0) {
            handle_library_error(err);
            goto out;
        }
    }
    err = tsk_node_table_append_columns(table, num_rows,
            PyArray_DATA(flags_array), PyArray_DATA(time_array), population_data,
            individual_data, metadata_data, metadata_offset_data);
    if (err != 0) {
        handle_library_error(err);
        goto out;
    }
    ret = 0;
out:
    Py_XDECREF(flags_array);
    Py_XDECREF(time_array);
    Py_XDECREF(population_array);
    Py_XDECREF(individual_array);
    Py_XDECREF(metadata_array);
    Py_XDECREF(metadata_offset_array);
    return ret;
}

static int
parse_edge_table_dict(tsk_edge_table_t *table, PyObject *dict, bool clear_table)
{
    int ret = -1;
    int err;
    size_t num_rows = 0;
    PyObject *left_input = NULL;
    PyArrayObject *left_array = NULL;
    PyObject *right_input = NULL;
    PyArrayObject *right_array = NULL;
    PyObject *parent_input = NULL;
    PyArrayObject *parent_array = NULL;
    PyObject *child_input = NULL;
    PyArrayObject *child_array = NULL;

    /* Get the input values */
    left_input = get_table_dict_value(dict, "left", true);
    if (left_input == NULL) {
        goto out;
    }
    right_input = get_table_dict_value(dict, "right", true);
    if (right_input == NULL) {
        goto out;
    }
    parent_input = get_table_dict_value(dict, "parent", true);
    if (parent_input == NULL) {
        goto out;
    }
    child_input = get_table_dict_value(dict, "child", true);
    if (child_input == NULL) {
        goto out;
    }

    /* Create the arrays */
    left_array = table_read_column_array(left_input, NPY_FLOAT64, &num_rows, false);
    if (left_array == NULL) {
        goto out;
    }
    right_array = table_read_column_array(right_input, NPY_FLOAT64, &num_rows, true);
    if (right_array == NULL) {
        goto out;
    }
    parent_array = table_read_column_array(parent_input, NPY_INT32, &num_rows, true);
    if (parent_array == NULL) {
        goto out;
    }
    child_array = table_read_column_array(child_input, NPY_INT32, &num_rows, true);
    if (child_array == NULL) {
        goto out;
    }

    if (clear_table) {
        err = tsk_edge_table_clear(table);
        if (err != 0) {
            handle_library_error(err);
            goto out;
        }
    }
    err = tsk_edge_table_append_columns(table, num_rows,
            PyArray_DATA(left_array), PyArray_DATA(right_array),
            PyArray_DATA(parent_array), PyArray_DATA(child_array));
    if (err != 0) {
        handle_library_error(err);
        goto out;
    }
    ret = 0;
out:
    Py_XDECREF(left_array);
    Py_XDECREF(right_array);
    Py_XDECREF(parent_array);
    Py_XDECREF(child_array);
    return ret;
}

static int
parse_migration_table_dict(tsk_migration_table_t *table, PyObject *dict, bool clear_table)
{
    int err;
    int ret = -1;
    size_t num_rows;
    PyObject *left_input = NULL;
    PyArrayObject *left_array = NULL;
    PyObject *right_input = NULL;
    PyArrayObject *right_array = NULL;
    PyObject *node_input = NULL;
    PyArrayObject *node_array = NULL;
    PyObject *source_input = NULL;
    PyArrayObject *source_array = NULL;
    PyObject *dest_input = NULL;
    PyArrayObject *dest_array = NULL;
    PyObject *time_input = NULL;
    PyArrayObject *time_array = NULL;

    /* Get the input values */
    left_input = get_table_dict_value(dict, "left", true);
    if (left_input == NULL) {
        goto out;
    }
    right_input = get_table_dict_value(dict, "right", true);
    if (right_input == NULL) {
        goto out;
    }
    node_input = get_table_dict_value(dict, "node", true);
    if (node_input == NULL) {
        goto out;
    }
    source_input = get_table_dict_value(dict, "source", true);
    if (source_input == NULL) {
        goto out;
    }
    dest_input = get_table_dict_value(dict, "dest", true);
    if (dest_input == NULL) {
        goto out;
    }
    time_input = get_table_dict_value(dict, "time", true);
    if (time_input == NULL) {
        goto out;
    }

    /* Build the arrays */
    left_array = table_read_column_array(left_input, NPY_FLOAT64, &num_rows, false);
    if (left_array == NULL) {
        goto out;
    }
    right_array = table_read_column_array(right_input, NPY_FLOAT64, &num_rows, true);
    if (right_array == NULL) {
        goto out;
    }
    node_array = table_read_column_array(node_input, NPY_INT32, &num_rows, true);
    if (node_array == NULL) {
        goto out;
    }
    source_array = table_read_column_array(source_input, NPY_INT32, &num_rows, true);
    if (source_array == NULL) {
        goto out;
    }
    dest_array = table_read_column_array(dest_input, NPY_INT32, &num_rows, true);
    if (dest_array == NULL) {
        goto out;
    }
    time_array = table_read_column_array(time_input, NPY_FLOAT64, &num_rows, true);
    if (time_array == NULL) {
        goto out;
    }

    if (clear_table) {
        err = tsk_migration_table_clear(table);
        if (err != 0) {
            handle_library_error(err);
            goto out;
        }
    }
    err = tsk_migration_table_append_columns(table, num_rows,
        PyArray_DATA(left_array), PyArray_DATA(right_array), PyArray_DATA(node_array),
        PyArray_DATA(source_array), PyArray_DATA(dest_array), PyArray_DATA(time_array));
    if (err != 0) {
        handle_library_error(err);
        goto out;
    }
    ret = 0;
out:
    Py_XDECREF(left_array);
    Py_XDECREF(right_array);
    Py_XDECREF(node_array);
    Py_XDECREF(source_array);
    Py_XDECREF(dest_array);
    Py_XDECREF(time_array);
    return ret;
}

static int
parse_site_table_dict(tsk_site_table_t *table, PyObject *dict, bool clear_table)
{
    int err;
    int ret = -1;
    size_t num_rows = 0;
    size_t ancestral_state_length, metadata_length;
    PyObject *position_input = NULL;
    PyArrayObject *position_array = NULL;
    PyObject *ancestral_state_input = NULL;
    PyArrayObject *ancestral_state_array = NULL;
    PyObject *ancestral_state_offset_input = NULL;
    PyArrayObject *ancestral_state_offset_array = NULL;
    PyObject *metadata_input = NULL;
    PyArrayObject *metadata_array = NULL;
    PyObject *metadata_offset_input = NULL;
    PyArrayObject *metadata_offset_array = NULL;
    char *metadata_data;
    uint32_t *metadata_offset_data;

    /* Get the input values */
    position_input = get_table_dict_value(dict, "position", true);
    if (position_input == NULL) {
        goto out;
    }
    ancestral_state_input = get_table_dict_value(dict, "ancestral_state", true);
    if (ancestral_state_input == NULL) {
        goto out;
    }
    ancestral_state_offset_input = get_table_dict_value(dict, "ancestral_state_offset", true);
    if (ancestral_state_offset_input == NULL) {
        goto out;
    }
    metadata_input = get_table_dict_value(dict, "metadata", false);
    if (metadata_input == NULL) {
        goto out;
    }
    metadata_offset_input = get_table_dict_value(dict, "metadata_offset", false);
    if (metadata_offset_input == NULL) {
        goto out;
    }

    /* Get the arrays */
    position_array = table_read_column_array(position_input, NPY_FLOAT64, &num_rows, false);
    if (position_array == NULL) {
        goto out;
    }
    ancestral_state_array = table_read_column_array(ancestral_state_input, NPY_INT8,
            &ancestral_state_length, false);
    if (ancestral_state_array == NULL) {
        goto out;
    }
    ancestral_state_offset_array = table_read_offset_array(ancestral_state_offset_input,
            &num_rows, ancestral_state_length, true);
    if (ancestral_state_offset_array == NULL) {
        goto out;
    }

    metadata_data = NULL;
    metadata_offset_data = NULL;
    if ((metadata_input == Py_None) != (metadata_offset_input == Py_None)) {
        PyErr_SetString(PyExc_TypeError,
                "metadata and metadata_offset must be specified together");
        goto out;
    }
    if (metadata_input != Py_None) {
        metadata_array = table_read_column_array(metadata_input, NPY_INT8,
                &metadata_length, false);
        if (metadata_array == NULL) {
            goto out;
        }
        metadata_data = PyArray_DATA(metadata_array);
        metadata_offset_array = table_read_offset_array(metadata_offset_input, &num_rows,
                metadata_length, false);
        if (metadata_offset_array == NULL) {
            goto out;
        }
        metadata_offset_data = PyArray_DATA(metadata_offset_array);
    }

    if (clear_table) {
        err = tsk_site_table_clear(table);
        if (err != 0) {
            handle_library_error(err);
            goto out;
        }
    }
    err = tsk_site_table_append_columns(table, num_rows,
        PyArray_DATA(position_array), PyArray_DATA(ancestral_state_array),
        PyArray_DATA(ancestral_state_offset_array), metadata_data, metadata_offset_data);
    if (err != 0) {
        handle_library_error(err);
        goto out;
    }
    ret = 0;
out:
    Py_XDECREF(position_array);
    Py_XDECREF(ancestral_state_array);
    Py_XDECREF(ancestral_state_offset_array);
    Py_XDECREF(metadata_array);
    Py_XDECREF(metadata_offset_array);
    return ret;
}

static int
parse_mutation_table_dict(tsk_mutation_table_t *table, PyObject *dict, bool clear_table)
{
    int err;
    int ret = -1;
    size_t num_rows = 0;
    size_t derived_state_length = 0;
    size_t metadata_length = 0;
    PyObject *site_input = NULL;
    PyArrayObject *site_array = NULL;
    PyObject *derived_state_input = NULL;
    PyArrayObject *derived_state_array = NULL;
    PyObject *derived_state_offset_input = NULL;
    PyArrayObject *derived_state_offset_array = NULL;
    PyObject *node_input = NULL;
    PyArrayObject *node_array = NULL;
    PyObject *parent_input = NULL;
    PyArrayObject *parent_array = NULL;
    tsk_id_t *parent_data;
    PyObject *metadata_input = NULL;
    PyArrayObject *metadata_array = NULL;
    PyObject *metadata_offset_input = NULL;
    PyArrayObject *metadata_offset_array = NULL;
    char *metadata_data;
    uint32_t *metadata_offset_data;

    /* Get the input values */
    site_input = get_table_dict_value(dict, "site", true);
    if (site_input == NULL) {
        goto out;
    }
    node_input = get_table_dict_value(dict, "node", true);
    if (node_input == NULL) {
        goto out;
    }
    parent_input = get_table_dict_value(dict, "parent", false);
    if (parent_input == NULL) {
        goto out;
    }
    derived_state_input = get_table_dict_value(dict, "derived_state", true);
    if (derived_state_input == NULL) {
        goto out;
    }
    derived_state_offset_input = get_table_dict_value(dict, "derived_state_offset", true);
    if (derived_state_offset_input == NULL) {
        goto out;
    }
    metadata_input = get_table_dict_value(dict, "metadata", false);
    if (metadata_input == NULL) {
        goto out;
    }
    metadata_offset_input = get_table_dict_value(dict, "metadata_offset", false);
    if (metadata_offset_input == NULL) {
        goto out;
    }

    /* Get the arrays */
    site_array = table_read_column_array(site_input, NPY_INT32, &num_rows, false);
    if (site_array == NULL) {
        goto out;
    }
    derived_state_array = table_read_column_array(derived_state_input, NPY_INT8,
            &derived_state_length, false);
    if (derived_state_array == NULL) {
        goto out;
    }
    derived_state_offset_array = table_read_offset_array(derived_state_offset_input,
            &num_rows, derived_state_length, true);
    if (derived_state_offset_array == NULL) {
        goto out;
    }
    node_array = table_read_column_array(node_input, NPY_INT32, &num_rows, true);
    if (node_array == NULL) {
        goto out;
    }

    parent_data = NULL;
    if (parent_input != Py_None) {
        parent_array = table_read_column_array(parent_input, NPY_INT32, &num_rows, true);
        if (parent_array == NULL) {
            goto out;
        }
        parent_data = PyArray_DATA(parent_array);
    }

    metadata_data = NULL;
    metadata_offset_data = NULL;
    if ((metadata_input == Py_None) != (metadata_offset_input == Py_None)) {
        PyErr_SetString(PyExc_TypeError,
                "metadata and metadata_offset must be specified together");
        goto out;
    }
    if (metadata_input != Py_None) {
        metadata_array = table_read_column_array(metadata_input, NPY_INT8,
                &metadata_length, false);
        if (metadata_array == NULL) {
            goto out;
        }
        metadata_data = PyArray_DATA(metadata_array);
        metadata_offset_array = table_read_offset_array(metadata_offset_input, &num_rows,
                metadata_length, false);
        if (metadata_offset_array == NULL) {
            goto out;
        }
        metadata_offset_data = PyArray_DATA(metadata_offset_array);
    }

    if (clear_table) {
        err = tsk_mutation_table_clear(table);
        if (err != 0) {
            handle_library_error(err);
            goto out;
        }
    }
    err = tsk_mutation_table_append_columns(table, num_rows,
            PyArray_DATA(site_array), PyArray_DATA(node_array),
            parent_data, PyArray_DATA(derived_state_array),
            PyArray_DATA(derived_state_offset_array),
            metadata_data, metadata_offset_data);
    if (err != 0) {
        handle_library_error(err);
        goto out;
    }
    ret = 0;
out:
    Py_XDECREF(site_array);
    Py_XDECREF(derived_state_array);
    Py_XDECREF(derived_state_offset_array);
    Py_XDECREF(metadata_array);
    Py_XDECREF(metadata_offset_array);
    Py_XDECREF(node_array);
    Py_XDECREF(parent_array);
    return ret;
}

static int
parse_population_table_dict(tsk_population_table_t *table, PyObject *dict, bool clear_table)
{
    int err;
    int ret = -1;
    size_t num_rows, metadata_length;
    PyObject *metadata_input = NULL;
    PyArrayObject *metadata_array = NULL;
    PyObject *metadata_offset_input = NULL;
    PyArrayObject *metadata_offset_array = NULL;

    /* Get the inputs */
    metadata_input = get_table_dict_value(dict, "metadata", true);
    if (metadata_input == NULL) {
        goto out;
    }
    metadata_offset_input = get_table_dict_value(dict, "metadata_offset", true);
    if (metadata_offset_input == NULL) {
        goto out;
    }

    /* Get the arrays */
    metadata_array = table_read_column_array(metadata_input, NPY_INT8,
            &metadata_length, false);
    if (metadata_array == NULL) {
        goto out;
    }
    metadata_offset_array = table_read_offset_array(metadata_offset_input, &num_rows,
            metadata_length, false);
    if (metadata_offset_array == NULL) {
        goto out;
    }

    if (clear_table) {
        err = tsk_population_table_clear(table);
        if (err != 0) {
            handle_library_error(err);
            goto out;
        }
    }
    err = tsk_population_table_append_columns(table, num_rows,
            PyArray_DATA(metadata_array), PyArray_DATA(metadata_offset_array));
    if (err != 0) {
        handle_library_error(err);
        goto out;
    }
    ret = 0;
out:
    Py_XDECREF(metadata_array);
    Py_XDECREF(metadata_offset_array);
    return ret;
}

static int
parse_provenance_table_dict(tsk_provenance_table_t *table, PyObject *dict, bool clear_table)
{
    int err;
    int ret = -1;
    size_t num_rows, timestamp_length, record_length;
    PyObject *timestamp_input = NULL;
    PyArrayObject *timestamp_array = NULL;
    PyObject *timestamp_offset_input = NULL;
    PyArrayObject *timestamp_offset_array = NULL;
    PyObject *record_input = NULL;
    PyArrayObject *record_array = NULL;
    PyObject *record_offset_input = NULL;
    PyArrayObject *record_offset_array = NULL;

    /* Get the inputs */
    timestamp_input = get_table_dict_value(dict, "timestamp", true);
    if (timestamp_input == NULL) {
        goto out;
    }
    timestamp_offset_input = get_table_dict_value(dict, "timestamp_offset", true);
    if (timestamp_offset_input == NULL) {
        goto out;
    }
    record_input = get_table_dict_value(dict, "record", true);
    if (record_input == NULL) {
        goto out;
    }
    record_offset_input = get_table_dict_value(dict, "record_offset", true);
    if (record_offset_input == NULL) {
        goto out;
    }

    timestamp_array = table_read_column_array(timestamp_input, NPY_INT8,
            &timestamp_length, false);
    if (timestamp_array == NULL) {
        goto out;
    }
    timestamp_offset_array = table_read_offset_array(timestamp_offset_input, &num_rows,
            timestamp_length, false);
    if (timestamp_offset_array == NULL) {
        goto out;
    }
    record_array = table_read_column_array(record_input, NPY_INT8,
            &record_length, false);
    if (record_array == NULL) {
        goto out;
    }
    record_offset_array = table_read_offset_array(record_offset_input, &num_rows,
            record_length, true);
    if (record_offset_array == NULL) {
        goto out;
    }

    if (clear_table) {
        err = tsk_provenance_table_clear(table);
        if (err != 0) {
            handle_library_error(err);
            goto out;
        }
    }
    err = tsk_provenance_table_append_columns(table, num_rows,
            PyArray_DATA(timestamp_array), PyArray_DATA(timestamp_offset_array),
            PyArray_DATA(record_array), PyArray_DATA(record_offset_array));
    if (err != 0) {
        handle_library_error(err);
        goto out;
    }
    ret = 0;
out:
    Py_XDECREF(timestamp_array);
    Py_XDECREF(timestamp_offset_array);
    Py_XDECREF(record_array);
    Py_XDECREF(record_offset_array);
    return ret;
}

static int
parse_table_collection_dict(tsk_table_collection_t *tables, PyObject *tables_dict)
{
    int ret = -1;
    PyObject *value = NULL;

    value = get_table_dict_value(tables_dict, "sequence_length", true);
    if (value == NULL) {
        goto out;
    }
    if (!PyNumber_Check(value)) {
        PyErr_Format(PyExc_TypeError, "'sequence_length' is not number");
        goto out;
    }
    tables->sequence_length = PyFloat_AsDouble(value);

    /* individuals */
    value = get_table_dict_value(tables_dict, "individuals", true);
    if (value == NULL) {
        goto out;
    }
    if (!PyDict_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "not a dictionary");
        goto out;
    }
    if (parse_individual_table_dict(&tables->individuals, value, true) != 0) {
        goto out;
    }

    /* nodes */
    value = get_table_dict_value(tables_dict, "nodes", true);
    if (value == NULL) {
        goto out;
    }
    if (!PyDict_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "not a dictionary");
        goto out;
    }
    if (parse_node_table_dict(&tables->nodes, value, true) != 0) {
        goto out;
    }

    /* edges */
    value = get_table_dict_value(tables_dict, "edges", true);
    if (value == NULL) {
        goto out;
    }
    if (!PyDict_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "not a dictionary");
        goto out;
    }
    if (parse_edge_table_dict(&tables->edges, value, true) != 0) {
        goto out;
    }

    /* migrations */
    value = get_table_dict_value(tables_dict, "migrations", true);
    if (value == NULL) {
        goto out;
    }
    if (!PyDict_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "not a dictionary");
        goto out;
    }
    if (parse_migration_table_dict(&tables->migrations, value, true) != 0) {
        goto out;
    }

    /* sites */
    value = get_table_dict_value(tables_dict, "sites", true);
    if (value == NULL) {
        goto out;
    }
    if (!PyDict_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "not a dictionary");
        goto out;
    }
    if (parse_site_table_dict(&tables->sites, value, true) != 0) {
        goto out;
    }

    /* mutations */
    value = get_table_dict_value(tables_dict, "mutations", true);
    if (value == NULL) {
        goto out;
    }
    if (!PyDict_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "not a dictionary");
        goto out;
    }
    if (parse_mutation_table_dict(&tables->mutations, value, true) != 0) {
        goto out;
    }

    /* populations */
    value = get_table_dict_value(tables_dict, "populations", true);
    if (value == NULL) {
        goto out;
    }
    if (!PyDict_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "not a dictionary");
        goto out;
    }
    if (parse_population_table_dict(&tables->populations, value, true) != 0) {
        goto out;
    }

    /* provenances */
    value = get_table_dict_value(tables_dict, "provenances", true);
    if (value == NULL) {
        goto out;
    }
    if (!PyDict_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "not a dictionary");
        goto out;
    }
    if (parse_provenance_table_dict(&tables->provenances, value, true) != 0) {
        goto out;
    }

    ret = 0;
out:
    return ret;
}

static int
write_table_arrays(tsk_table_collection_t *tables, PyObject *dict)
{
    struct table_col {
        const char *name;
        void *data;
        npy_intp num_rows;
        int type;
    };
    struct table_desc {
        const char *name;
        struct table_col *cols;
    };
    int ret = -1;
    PyObject *array = NULL;
    PyObject *table_dict = NULL;
    size_t j;
    struct table_col *col;

    struct table_col individual_cols[] = {
        {"flags",
            (void *) tables->individuals.flags, tables->individuals.num_rows, NPY_UINT32},
        {"location",
            (void *) tables->individuals.location, tables->individuals.location_length,
            NPY_FLOAT64},
        {"location_offset",
            (void *) tables->individuals.location_offset, tables->individuals.num_rows + 1,
            NPY_UINT32},
        {"metadata",
            (void *) tables->individuals.metadata, tables->individuals.metadata_length,
            NPY_INT8},
        {"metadata_offset",
            (void *) tables->individuals.metadata_offset, tables->individuals.num_rows + 1,
            NPY_UINT32},
        {NULL},
    };

    struct table_col node_cols[] = {
        {"time",
            (void *) tables->nodes.time, tables->nodes.num_rows, NPY_FLOAT64},
        {"flags",
            (void *) tables->nodes.flags, tables->nodes.num_rows, NPY_UINT32},
        {"population",
            (void *) tables->nodes.population, tables->nodes.num_rows, NPY_INT32},
        {"individual",
            (void *) tables->nodes.individual, tables->nodes.num_rows, NPY_INT32},
        {"metadata",
            (void *) tables->nodes.metadata, tables->nodes.metadata_length, NPY_INT8},
        {"metadata_offset",
            (void *) tables->nodes.metadata_offset, tables->nodes.num_rows + 1, NPY_UINT32},
        {NULL},
    };

    struct table_col edge_cols[] = {
        {"left", (void *) tables->edges.left, tables->edges.num_rows, NPY_FLOAT64},
        {"right", (void *) tables->edges.right, tables->edges.num_rows, NPY_FLOAT64},
        {"parent", (void *) tables->edges.parent, tables->edges.num_rows, NPY_INT32},
        {"child", (void *) tables->edges.child, tables->edges.num_rows, NPY_INT32},
        {NULL},
    };

    struct table_col migration_cols[] = {
        {"left",
            (void *) tables->migrations.left, tables->migrations.num_rows,  NPY_FLOAT64},
        {"right",
            (void *) tables->migrations.right, tables->migrations.num_rows,  NPY_FLOAT64},
        {"node",
            (void *) tables->migrations.node, tables->migrations.num_rows,  NPY_INT32},
        {"source",
            (void *) tables->migrations.source, tables->migrations.num_rows,  NPY_INT32},
        {"dest",
            (void *) tables->migrations.dest, tables->migrations.num_rows,  NPY_INT32},
        {"time",
            (void *) tables->migrations.time, tables->migrations.num_rows,  NPY_FLOAT64},
        {NULL},
    };

    struct table_col site_cols[] = {
        {"position",
            (void *) tables->sites.position, tables->sites.num_rows, NPY_FLOAT64},
        {"ancestral_state",
            (void *) tables->sites.ancestral_state, tables->sites.ancestral_state_length,
            NPY_INT8},
        {"ancestral_state_offset",
            (void *) tables->sites.ancestral_state_offset, tables->sites.num_rows + 1,
            NPY_UINT32},
        {"metadata",
            (void *) tables->sites.metadata, tables->sites.metadata_length, NPY_INT8},
        {"metadata_offset",
            (void *) tables->sites.metadata_offset, tables->sites.num_rows + 1, NPY_UINT32},
        {NULL},
    };

    struct table_col mutation_cols[] = {
        {"site",
            (void *) tables->mutations.site, tables->mutations.num_rows, NPY_INT32},
        {"node",
            (void *) tables->mutations.node, tables->mutations.num_rows, NPY_INT32},
        {"parent",
            (void *) tables->mutations.parent, tables->mutations.num_rows, NPY_INT32},
        {"derived_state",
            (void *) tables->mutations.derived_state,
            tables->mutations.derived_state_length, NPY_INT8},
        {"derived_state_offset",
            (void *) tables->mutations.derived_state_offset,
            tables->mutations.num_rows + 1, NPY_UINT32},
        {"metadata",
            (void *) tables->mutations.metadata,
            tables->mutations.metadata_length, NPY_INT8},
        {"metadata_offset",
            (void *) tables->mutations.metadata_offset,
            tables->mutations.num_rows + 1, NPY_UINT32},
        {NULL},
    };

    struct table_col population_cols[] = {
        {"metadata", (void *) tables->populations.metadata,
            tables->populations.metadata_length, NPY_INT8},
        {"metadata_offset", (void *) tables->populations.metadata_offset,
            tables->populations.num_rows+ 1, NPY_UINT32},
        {NULL},
    };

    struct table_col provenance_cols[] = {
        {"timestamp", (void *) tables->provenances.timestamp,
            tables->provenances.timestamp_length, NPY_INT8},
        {"timestamp_offset", (void *) tables->provenances.timestamp_offset,
            tables->provenances.num_rows+ 1, NPY_UINT32},
        {"record", (void *) tables->provenances.record,
            tables->provenances.record_length, NPY_INT8},
        {"record_offset", (void *) tables->provenances.record_offset,
            tables->provenances.num_rows + 1, NPY_UINT32},
        {NULL},
    };

    struct table_desc table_descs[] = {
        {"individuals", individual_cols},
        {"nodes", node_cols},
        {"edges", edge_cols},
        {"migrations", migration_cols},
        {"sites", site_cols},
        {"mutations", mutation_cols},
        {"populations", population_cols},
        {"provenances", provenance_cols},
    };

    for (j = 0; j < sizeof(table_descs) / sizeof(*table_descs); j++) {
        table_dict = PyDict_New();
        if (table_dict == NULL) {
            goto out;
        }
        col = table_descs[j].cols;
        while (col->name != NULL) {
            array = PyArray_SimpleNewFromData(1, &col->num_rows, col->type, col->data);
            if (array == NULL) {
                goto out;
            }
            if (PyDict_SetItemString(table_dict, col->name, array) != 0) {
                goto out;
            }
            Py_DECREF(array);
            array = NULL;
            col++;
        }
        if (PyDict_SetItemString(dict, table_descs[j].name, table_dict) != 0) {
            goto out;
        }
        Py_DECREF(table_dict);
        table_dict = NULL;
    }
    ret = 0;
out:
    Py_XDECREF(array);
    Py_XDECREF(table_dict);
    return ret;
}

/* Returns a dictionary encoding of the specified table collection */
static PyObject*
dump_tables_dict(tsk_table_collection_t *tables)
{
    PyObject *ret = NULL;
    PyObject *dict = NULL;
    PyObject *val = NULL;
    int err;

    dict = PyDict_New();
    if (dict == NULL) {
        goto out;
    }
    val = Py_BuildValue("d", tables->sequence_length);
    if (val == NULL) {
        goto out;
    }
    if (PyDict_SetItemString(dict, "sequence_length", val) != 0) {
        goto out;
    }
    Py_DECREF(val);
    val = NULL;

    err = write_table_arrays(tables, dict);
    if (err != 0) {
        goto out;
    }
    ret = dict;
    dict = NULL;
out:
    Py_XDECREF(dict);
    Py_XDECREF(val);
    return ret;
}

/*===================================================================
 * LightweightTableCollection
 *===================================================================
 */

static int
LightweightTableCollection_check_state(LightweightTableCollection *self)
{
    int ret = 0;
    if (self->tables == NULL) {
        PyErr_SetString(PyExc_SystemError, "LightweightTableCollection not initialised");
        ret = -1;
    }
    return ret;
}

static void
LightweightTableCollection_dealloc(LightweightTableCollection* self)
{
    if (self->tables != NULL) {
        tsk_table_collection_free(self->tables);
        PyMem_Free(self->tables);
        self->tables = NULL;
    }
    Py_TYPE(self)->tp_free((PyObject*)self);
}

static int
LightweightTableCollection_init(LightweightTableCollection *self, PyObject *args, PyObject *kwds)
{
    int ret = -1;
    int err;
    static char *kwlist[] = {"sequence_length", NULL};
    double sequence_length = -1;

    self->tables = NULL;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|d", kwlist, &sequence_length)) {
        goto out;
    }
    self->tables = PyMem_Malloc(sizeof(*self->tables));
    if (self->tables == NULL) {
        PyErr_NoMemory();
        goto out;
    }
    err = tsk_table_collection_init(self->tables, 0);
    if (err != 0) {
        handle_library_error(err);
        goto out;
    }
    self->tables->sequence_length = sequence_length;
    ret = 0;
out:
    return ret;
}

static PyObject *
LightweightTableCollection_asdict(LightweightTableCollection *self)
{
    PyObject *ret = NULL;

    if (LightweightTableCollection_check_state(self) != 0) {
        goto out;
    }
    ret = dump_tables_dict(self->tables);
out:
    return ret;
}

static PyObject *
LightweightTableCollection_fromdict(LightweightTableCollection *self, PyObject *args)
{
    int err;
    PyObject *ret = NULL;
    PyObject *dict = NULL;

    if (!PyArg_ParseTuple(args, "O!", &PyDict_Type, &dict)) {
        goto out;
    }
    err = parse_table_collection_dict(self->tables, dict);
    if (err != 0) {
        goto out;
    }
    ret = Py_BuildValue("");
out:
    return ret;
}

static PyMemberDef LightweightTableCollection_members[] = {
    {NULL}  /* Sentinel */
};

static PyMethodDef LightweightTableCollection_methods[] = {
    {"asdict", (PyCFunction) LightweightTableCollection_asdict,
        METH_NOARGS, "Returns the tables encoded as a dictionary."},
    {"fromdict", (PyCFunction) LightweightTableCollection_fromdict,
        METH_VARARGS, "Populates the internal tables using the specified dictionary."},
    {NULL}  /* Sentinel */
};

static PyTypeObject LightweightTableCollectionType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "_msprime.LightweightTableCollection",             /* tp_name */
    sizeof(LightweightTableCollection),             /* tp_basicsize */
    0,                         /* tp_itemsize */
    (destructor)LightweightTableCollection_dealloc, /* tp_dealloc */
    0,                         /* tp_print */
    0,                         /* tp_getattr */
    0,                         /* tp_setattr */
    0,                         /* tp_reserved */
    0,                         /* tp_repr */
    0,                         /* tp_as_number */
    0,                         /* tp_as_sequence */
    0,                         /* tp_as_mapping */
    0,                         /* tp_hash  */
    0,                         /* tp_call */
    0,                         /* tp_str */
    0,                         /* tp_getattro */
    0,                         /* tp_setattro */
    0,                         /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT,        /* tp_flags */
    "LightweightTableCollection objects",           /* tp_doc */
    0,                     /* tp_traverse */
    0,                     /* tp_clear */
    0,                     /* tp_richcompare */
    0,                     /* tp_weaklistoffset */
    0,                     /* tp_iter */
    0,                     /* tp_iternext */
    LightweightTableCollection_methods,             /* tp_methods */
    LightweightTableCollection_members,             /* tp_members */
    0,                         /* tp_getset */
    0,                         /* tp_base */
    0,                         /* tp_dict */
    0,                         /* tp_descr_get */
    0,                         /* tp_descr_set */
    0,                         /* tp_dictoffset */
    (initproc)LightweightTableCollection_init,      /* tp_init */
};

/*===================================================================
 * RandomGenerator
 *===================================================================
 */

static int
RandomGenerator_check_state(RandomGenerator *self)
{
    int ret = 0;
    if (self->rng == NULL) {
        PyErr_SetString(PyExc_SystemError, "RandomGenerator not initialised");
        ret = -1;
    }
    return ret;
}

static void
RandomGenerator_dealloc(RandomGenerator* self)
{
    if (self->rng != NULL) {
        gsl_rng_free(self->rng);
        self->rng = NULL;
    }
    Py_TYPE(self)->tp_free((PyObject*)self);
}

static int
RandomGenerator_init(RandomGenerator *self, PyObject *args, PyObject *kwds)
{
    int ret = -1;
    static char *kwlist[] = {"seed", NULL};
    unsigned long long seed = 0;

    self->rng  = NULL;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "K", kwlist, &seed)) {
        goto out;
    }
    if (seed == 0 || seed >= (1ULL<<32)) {
        PyErr_Format(PyExc_ValueError,
            "seeds must be greater than 0 and less than 2^32");
        goto out;
    }
    self->seed = seed;
    self->rng = gsl_rng_alloc(gsl_rng_default);
    gsl_rng_set(self->rng, self->seed);
    ret = 0;
out:
    return ret;
}

static PyObject *
RandomGenerator_get_seed(RandomGenerator *self)
{
    PyObject *ret = NULL;

    if (RandomGenerator_check_state(self) != 0) {
        goto out;
    }
    ret = Py_BuildValue("k", self->seed);
out:
    return ret;
}

static PyObject *
RandomGenerator_flat(RandomGenerator *self, PyObject *args)
{
    PyObject *ret = NULL;
    double a, b;

    if (RandomGenerator_check_state(self) != 0) {
        goto out;
    }
    if (!PyArg_ParseTuple(args, "dd", &a, &b)) {
        goto out;
    }
    ret = Py_BuildValue("d", gsl_ran_flat(self->rng, a, b));
out:
    return ret;
}

static PyObject *
RandomGenerator_poisson(RandomGenerator *self, PyObject *args)
{
    PyObject *ret = NULL;
    double mu;

    if (RandomGenerator_check_state(self) != 0) {
        goto out;
    }
    if (!PyArg_ParseTuple(args, "d", &mu)) {
        goto out;
    }
    ret = Py_BuildValue("I", gsl_ran_poisson(self->rng, mu));
out:
    return ret;
}

static PyObject *
RandomGenerator_uniform_int(RandomGenerator *self, PyObject *args)
{
    PyObject *ret = NULL;
    unsigned long n;

    if (RandomGenerator_check_state(self) != 0) {
        goto out;
    }
    if (!PyArg_ParseTuple(args, "k", &n)) {
        goto out;
    }
    ret = Py_BuildValue("k", gsl_rng_uniform_int(self->rng, n));
out:
    return ret;
}

static PyMemberDef RandomGenerator_members[] = {
    {NULL}  /* Sentinel */
};

static PyMethodDef RandomGenerator_methods[] = {
    {"get_seed", (PyCFunction) RandomGenerator_get_seed,
        METH_NOARGS, "Returns the random seed for this generator."},
    {"flat", (PyCFunction) RandomGenerator_flat,
        METH_VARARGS, "Interface for gsl_ran_flat"},
    {"poisson", (PyCFunction) RandomGenerator_poisson,
        METH_VARARGS, "Interface for gsl_ran_poisson"},
    {"uniform_int", (PyCFunction) RandomGenerator_uniform_int,
        METH_VARARGS, "Interface for gsl_rng_uniform_int"},
    {NULL}  /* Sentinel */
};

static PyTypeObject RandomGeneratorType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "_msprime.RandomGenerator",             /* tp_name */
    sizeof(RandomGenerator),             /* tp_basicsize */
    0,                         /* tp_itemsize */
    (destructor)RandomGenerator_dealloc, /* tp_dealloc */
    0,                         /* tp_print */
    0,                         /* tp_getattr */
    0,                         /* tp_setattr */
    0,                         /* tp_reserved */
    0,                         /* tp_repr */
    0,                         /* tp_as_number */
    0,                         /* tp_as_sequence */
    0,                         /* tp_as_mapping */
    0,                         /* tp_hash  */
    0,                         /* tp_call */
    0,                         /* tp_str */
    0,                         /* tp_getattro */
    0,                         /* tp_setattro */
    0,                         /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT,        /* tp_flags */
    "RandomGenerator objects",           /* tp_doc */
    0,                     /* tp_traverse */
    0,                     /* tp_clear */
    0,                     /* tp_richcompare */
    0,                     /* tp_weaklistoffset */
    0,                     /* tp_iter */
    0,                     /* tp_iternext */
    RandomGenerator_methods,             /* tp_methods */
    RandomGenerator_members,             /* tp_members */
    0,                         /* tp_getset */
    0,                         /* tp_base */
    0,                         /* tp_dict */
    0,                         /* tp_descr_get */
    0,                         /* tp_descr_set */
    0,                         /* tp_dictoffset */
    (initproc)RandomGenerator_init,      /* tp_init */
};

/*===================================================================
 * IntervalMap
 *===================================================================
 */

static int
IntervalMap_check_state(IntervalMap *self)
{
    int ret = 0;
    if (self->interval_map == NULL) {
        PyErr_SetString(PyExc_SystemError, "IntervalMap not initialised");
        ret = -1;
    }
    return ret;
}

static void
IntervalMap_dealloc(IntervalMap* self)
{
    if (self->interval_map != NULL) {
        interval_map_free(self->interval_map);
        PyMem_Free(self->interval_map);
        self->interval_map = NULL;
    }
    Py_TYPE(self)->tp_free((PyObject*)self);
}

static int
IntervalMap_init(IntervalMap *self, PyObject *args, PyObject *kwds)
{
    int ret = -1;
    int err;
    static char *kwlist[] = {"position", "value", NULL};
    Py_ssize_t size;
    PyObject *py_position = NULL;
    PyObject *py_value = NULL;

    self->interval_map = NULL;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O&O&", kwlist,
            double_PyArray_converter, &py_position,
            double_PyArray_converter, &py_value)) {
        goto out;
    }

    size = PyObject_Size(py_position);
    if (size != PyObject_Size(py_value)) {
        PyErr_SetString(PyExc_ValueError,
            "position and  list must be the same length");
        goto out;
    }
    self->interval_map = PyMem_Malloc(sizeof(interval_map_t));
    if (self->interval_map == NULL) {
        PyErr_NoMemory();
        goto out;
    }
    err = interval_map_alloc(self->interval_map, size,
            PyArray_DATA((PyArrayObject *) py_position),
            PyArray_DATA((PyArrayObject *) py_value));
    if (err != 0) {
        handle_library_error(err);
        goto out;
    }
    ret = 0;
out:
    Py_XDECREF(py_position);
    Py_XDECREF(py_value);
    return ret;
}

static PyObject *
IntervalMap_get_position(IntervalMap *self, void *closure)
{
    PyObject *ret = NULL;
    PyArrayObject *array;
    size_t size = self->interval_map->size;
    npy_intp dims = (npy_intp) size;

    array = (PyArrayObject *) PyArray_EMPTY(1, &dims, NPY_FLOAT64, 0);
    if (array == NULL) {
        goto out;
    }
    memcpy(PyArray_DATA(array), self->interval_map->position, size * sizeof(double));
    ret = (PyObject *) array;
out:
    return ret;
}

static PyObject *
IntervalMap_get_value(IntervalMap *self, void *closure)
{
    PyObject *ret = NULL;
    PyArrayObject *array;
    size_t size = self->interval_map->size;
    npy_intp dims = (npy_intp) size;

    array = (PyArrayObject *) PyArray_EMPTY(1, &dims, NPY_FLOAT64, 0);
    if (array == NULL) {
        goto out;
    }
    memcpy(PyArray_DATA(array), self->interval_map->value, size * sizeof(double));
    ret = (PyObject *) array;
out:
    return ret;
}

static PyGetSetDef IntervalMap_getsetters[] = {
    {"position", (getter) IntervalMap_get_position, NULL,
        "A copy of the position array"},
    {"value", (getter) IntervalMap_get_value, NULL,
        "A copy of the value array"},
    {NULL}  /* Sentinel */
};

static PyMemberDef IntervalMap_members[] = {
    {NULL}  /* Sentinel */
};

static PyMethodDef IntervalMap_methods[] = {
    {NULL}  /* Sentinel */
};

static PyTypeObject IntervalMapType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "_msprime.IntervalMap",             /* tp_name */
    sizeof(IntervalMap),             /* tp_basicsize */
    0,                         /* tp_itemsize */
    (destructor)IntervalMap_dealloc, /* tp_dealloc */
    0,                         /* tp_print */
    0,                         /* tp_getattr */
    0,                         /* tp_setattr */
    0,                         /* tp_reserved */
    0,                         /* tp_repr */
    0,                         /* tp_as_number */
    0,                         /* tp_as_sequence */
    0,                         /* tp_as_mapping */
    0,                         /* tp_hash  */
    0,                         /* tp_call */
    0,                         /* tp_str */
    0,                         /* tp_getattro */
    0,                         /* tp_setattro */
    0,                         /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT,        /* tp_flags */
    "IntervalMap objects",           /* tp_doc */
    0,                     /* tp_traverse */
    0,                     /* tp_clear */
    0,                     /* tp_richcompare */
    0,                     /* tp_weaklistoffset */
    0,                     /* tp_iter */
    0,                     /* tp_iternext */
    IntervalMap_methods,             /* tp_methods */
    IntervalMap_members,             /* tp_members */
    IntervalMap_getsetters,          /* tp_getset */
    0,                         /* tp_base */
    0,                         /* tp_dict */
    0,                         /* tp_descr_get */
    0,                         /* tp_descr_set */
    0,                         /* tp_dictoffset */
    (initproc)IntervalMap_init,      /* tp_init */
};


/*===================================================================
 * MutationGenerator
 *===================================================================
 */

static int
MutationGenerator_check_state(MutationGenerator *self)
{
    int ret = 0;
    if (self->mutgen == NULL) {
        PyErr_SetString(PyExc_SystemError, "MutationGenerator not initialised");
        ret = -1;
        goto out;
    }
    ret = RandomGenerator_check_state(self->random_generator);
out:
    return ret;
}

static void
MutationGenerator_dealloc(MutationGenerator* self)
{
    if (self->mutgen != NULL) {
        mutgen_free(self->mutgen);
        PyMem_Free(self->mutgen);
        self->mutgen = NULL;
    }
    Py_XDECREF(self->random_generator);
    Py_XDECREF(self->rate_map);
    Py_TYPE(self)->tp_free((PyObject*)self);
}

static int
MutationGenerator_init(MutationGenerator *self, PyObject *args, PyObject *kwds)
{
    int ret = -1;
    int err;
    int alphabet = 0;
    static char *kwlist[] = {"random_generator", "rate_map", "alphabet",
        "start_time", "end_time", NULL};
    double start_time = -DBL_MAX;
    double end_time = DBL_MAX;
    RandomGenerator *random_generator = NULL;
    IntervalMap *rate_map = NULL;

    self->mutgen = NULL;
    self->random_generator = NULL;
    self->rate_map = NULL;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O!O!|idd", kwlist,
            &RandomGeneratorType, &random_generator,
            &IntervalMapType, &rate_map,
            &alphabet, &start_time, &end_time)) {
        goto out;
    }
    self->random_generator = random_generator;
    Py_INCREF(self->random_generator);
    if (RandomGenerator_check_state(self->random_generator) != 0) {
        goto out;
    }
    self->rate_map = rate_map;
    Py_INCREF(self->rate_map);
    if (IntervalMap_check_state(self->rate_map) != 0) {
        goto out;
    }
    if (alphabet != MSP_ALPHABET_BINARY && alphabet != MSP_ALPHABET_NUCLEOTIDE) {
        PyErr_Format(PyExc_ValueError, "Bad mutation alphabet");
        goto out;
    }
    self->mutgen = PyMem_Malloc(sizeof(mutgen_t));
    if (self->mutgen == NULL) {
        PyErr_NoMemory();
        goto out;
    }
    err = mutgen_alloc(self->mutgen, random_generator->rng,
            rate_map->interval_map, alphabet, 0);
    if (err != 0) {
        handle_library_error(err);
        goto out;
    }
    err = mutgen_set_time_interval(self->mutgen, start_time, end_time);
    if (err != 0) {
        handle_library_error(err);
        goto out;
    }
    ret = 0;
out:
    return ret;
}

static PyObject *
MutationGenerator_generate(MutationGenerator *self, PyObject *args, PyObject *kwds)
{
    PyObject *ret = NULL;
    int err;
    LightweightTableCollection *tables = NULL;
    int flags = 0;
    int keep = 0;
    static char *kwlist[] = {"tables", "keep", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O!|i", kwlist,
            &LightweightTableCollectionType, &tables, &keep)) {
        goto out;
    }
    if (MutationGenerator_check_state(self) != 0) {
        goto out;
    }
    if (keep) {
        flags = MSP_KEEP_SITES;
    }
    err = mutgen_generate(self->mutgen, tables->tables, flags);
    if (err != 0) {
        handle_library_error(err);
        goto out;
    }
    ret = Py_BuildValue("");
out:
    return ret;
}

static PyObject *
MutationGenerator_get_alphabet(MutationGenerator *self, void *closure)
{
    PyObject *ret = NULL;
    if (MutationGenerator_check_state(self) != 0) {
        goto out;
    }
    ret = Py_BuildValue("i", self->mutgen->alphabet);
out:
    return ret;
}

static PyGetSetDef MutationGenerator_getsetters[] = {
    {"alphabet", (getter) MutationGenerator_get_alphabet, NULL,
        "The alphabet for this generator"},
    {NULL}  /* Sentinel */
};

static PyMethodDef MutationGenerator_methods[] = {
    {"generate", (PyCFunction) MutationGenerator_generate,
        METH_VARARGS|METH_KEYWORDS,
        "Generate mutations and write to the specified table."},
    {NULL}  /* Sentinel */
};

static PyTypeObject MutationGeneratorType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "_msprime.MutationGenerator",             /* tp_name */
    sizeof(MutationGenerator),             /* tp_basicsize */
    0,                         /* tp_itemsize */
    (destructor)MutationGenerator_dealloc, /* tp_dealloc */
    0,                         /* tp_print */
    0,                         /* tp_getattr */
    0,                         /* tp_setattr */
    0,                         /* tp_reserved */
    0,                         /* tp_repr */
    0,                         /* tp_as_number */
    0,                         /* tp_as_sequence */
    0,                         /* tp_as_mapping */
    0,                         /* tp_hash  */
    0,                         /* tp_call */
    0,                         /* tp_str */
    0,                         /* tp_getattro */
    0,                         /* tp_setattro */
    0,                         /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT,        /* tp_flags */
    "MutationGenerator objects",           /* tp_doc */
    0,                     /* tp_traverse */
    0,                     /* tp_clear */
    0,                     /* tp_richcompare */
    0,                     /* tp_weaklistoffset */
    0,                     /* tp_iter */
    0,                     /* tp_iternext */
    MutationGenerator_methods,             /* tp_methods */
    0,                         /* tp_members */
    MutationGenerator_getsetters,          /* tp_getset */
    0,                         /* tp_base */
    0,                         /* tp_dict */
    0,                         /* tp_descr_get */
    0,                         /* tp_descr_set */
    0,                         /* tp_dictoffset */
    (initproc)MutationGenerator_init,      /* tp_init */
};

/*===================================================================
 * RecombinationMap
 *===================================================================
 */

static int
RecombinationMap_check_recomb_map(RecombinationMap *self)
{
    int ret = 0;
    if (self->recomb_map == NULL) {
        PyErr_SetString(PyExc_ValueError, "recomb_map not initialised");
        ret = -1;
    }
    return ret;
}

static void
RecombinationMap_dealloc(RecombinationMap* self)
{
    if (self->recomb_map != NULL) {
        recomb_map_free(self->recomb_map);
        PyMem_Free(self->recomb_map);
        self->recomb_map = NULL;
    }
    Py_TYPE(self)->tp_free((PyObject*)self);
}

static int
RecombinationMap_init(RecombinationMap *self, PyObject *args, PyObject *kwds)
{
    int ret = -1;
    int err;
    static char *kwlist[] = {"positions", "rates", "discrete", NULL};
    Py_ssize_t size;
    PyObject *py_positions = NULL;
    PyObject *py_rates = NULL;
    double *positions;
    double *rates;
    int discrete = false;

    self->recomb_map = NULL;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O&O&p", kwlist,
            double_PyArray_converter, &py_positions,
            double_PyArray_converter, &py_rates,
            &discrete)) {
        goto out;
    }

    size = PyObject_Size(py_positions);
    if (size != PyObject_Size(py_rates)) {
        PyErr_SetString(PyExc_ValueError,
            "positions and rates list must be the same length");
        goto out;
    }

    positions = PyArray_DATA((PyArrayObject *) py_positions);
    rates = PyArray_DATA((PyArrayObject *) py_rates);

    self->recomb_map = PyMem_Malloc(sizeof(recomb_map_t));
    if (self->recomb_map == NULL) {
        PyErr_NoMemory();
        goto out;
    }
    err = recomb_map_alloc(self->recomb_map, size, positions, rates, discrete);
    if (err != 0) {
        handle_library_error(err);
        goto out;
    }
    ret = 0;
out:
    Py_XDECREF(py_positions);
    Py_XDECREF(py_rates);
    return ret;
}

static PyObject *
RecombinationMap_get_total_recombination_rate(RecombinationMap *self)
{
    PyObject *ret = NULL;

    if (RecombinationMap_check_recomb_map(self) != 0) {
        goto out;
    }
    ret = Py_BuildValue("d",
        recomb_map_get_total_recombination_rate(self->recomb_map));
out:
    return ret;
}

static PyObject *
RecombinationMap_get_size(RecombinationMap *self)
{
    PyObject *ret = NULL;

    if (RecombinationMap_check_recomb_map(self) != 0) {
        goto out;
    }
    ret = Py_BuildValue("n",
        (Py_ssize_t) recomb_map_get_size(self->recomb_map));
out:
    return ret;
}

static PyObject *
RecombinationMap_get_sequence_length(RecombinationMap *self)
{
    PyObject *ret = NULL;

    if (RecombinationMap_check_recomb_map(self) != 0) {
        goto out;
    }
    ret = Py_BuildValue("d", recomb_map_get_sequence_length(self->recomb_map));
out:
    return ret;
}

static PyObject *
RecombinationMap_get_discrete(RecombinationMap *self)
{
    PyObject *ret = NULL;

    if (RecombinationMap_check_recomb_map(self) != 0) {
        goto out;
    }
    ret = Py_BuildValue("i", recomb_map_get_discrete(self->recomb_map));
out:
    return ret;
}

static PyObject *
RecombinationMap_get_positions(RecombinationMap *self)
{
    PyObject *ret = NULL;
    PyObject *arr = NULL;
    npy_intp size;
    int err;

    if (RecombinationMap_check_recomb_map(self) != 0) {
        goto out;
    }
    size = recomb_map_get_size(self->recomb_map);
    arr = PyArray_SimpleNew(1, &size, NPY_FLOAT64);
    if (arr == NULL) {
        goto out;
    }
    err = recomb_map_get_positions(self->recomb_map,
            PyArray_DATA((PyArrayObject *) arr));
    if (err != 0) {
        handle_library_error(err);
        goto out;
    }
    ret = arr;
    arr = NULL;
out:
    Py_XDECREF(arr);
    return ret;
}

static PyObject *
RecombinationMap_get_rates(RecombinationMap *self)
{
    PyObject *ret = NULL;
    PyObject *arr = NULL;
    npy_intp size;
    int err;

    if (RecombinationMap_check_recomb_map(self) != 0) {
        goto out;
    }
    size = recomb_map_get_size(self->recomb_map);
    arr = PyArray_SimpleNew(1, &size, NPY_FLOAT64);
    if (arr == NULL) {
        goto out;
    }
    err = recomb_map_get_rates(self->recomb_map, PyArray_DATA((PyArrayObject *) arr));
    if (err != 0) {
        handle_library_error(err);
        goto out;
    }
    ret = arr;
    arr = NULL;
out:
    Py_XDECREF(arr);
    return ret;
}

static PyMemberDef RecombinationMap_members[] = {
    {NULL}  /* Sentinel */
};

static PyMethodDef RecombinationMap_methods[] = {
    {"get_total_recombination_rate",
        (PyCFunction) RecombinationMap_get_total_recombination_rate, METH_NOARGS,
        "Returns the total product of physical distance times recombination rate"},
    {"get_size", (PyCFunction) RecombinationMap_get_size, METH_NOARGS,
        "Returns the number of physical  positions in this map."},
    {"get_sequence_length", (PyCFunction) RecombinationMap_get_sequence_length, METH_NOARGS,
        "Returns the physical sequence length defined by this map."},
    {"get_positions",
        (PyCFunction) RecombinationMap_get_positions, METH_NOARGS,
        "Returns the positions in this recombination map."},
    {"get_rates",
        (PyCFunction) RecombinationMap_get_rates, METH_NOARGS,
        "Returns the rates in this recombination map."},
    {"get_discrete",
        (PyCFunction) RecombinationMap_get_discrete, METH_NOARGS,
        "Returns the value of discrete in this recombination map."},
    {NULL}  /* Sentinel */
};

static PyTypeObject RecombinationMapType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "_msprime.RecombinationMap",             /* tp_name */
    sizeof(RecombinationMap),             /* tp_basicsize */
    0,                         /* tp_itemsize */
    (destructor)RecombinationMap_dealloc, /* tp_dealloc */
    0,                         /* tp_print */
    0,                         /* tp_getattr */
    0,                         /* tp_setattr */
    0,                         /* tp_reserved */
    0,                         /* tp_repr */
    0,                         /* tp_as_number */
    0,                         /* tp_as_sequence */
    0,                         /* tp_as_mapping */
    0,                         /* tp_hash  */
    0,                         /* tp_call */
    0,                         /* tp_str */
    0,                         /* tp_getattro */
    0,                         /* tp_setattro */
    0,                         /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT,        /* tp_flags */
    "RecombinationMap objects",           /* tp_doc */
    0,                     /* tp_traverse */
    0,                     /* tp_clear */
    0,                     /* tp_richcompare */
    0,                     /* tp_weaklistoffset */
    0,                     /* tp_iter */
    0,                     /* tp_iternext */
    RecombinationMap_methods,             /* tp_methods */
    RecombinationMap_members,             /* tp_members */
    0,                         /* tp_getset */
    0,                         /* tp_base */
    0,                         /* tp_dict */
    0,                         /* tp_descr_get */
    0,                         /* tp_descr_set */
    0,                         /* tp_dictoffset */
    (initproc)RecombinationMap_init,      /* tp_init */
};

/*===================================================================
 * Simulator
 *===================================================================
 */

static int
Simulator_check_sim(Simulator *self)
{
    int ret = 0;
    if (self->sim == NULL) {
        PyErr_SetString(PyExc_SystemError, "simulator not initialised");
        ret = -1;
    }
    return ret;
}

static int
Simulator_parse_population_configuration(Simulator *self, PyObject *py_pop_config)
{
    int ret = -1;
    Py_ssize_t j;
    double initial_size, growth_rate;
    int err;
    PyObject *item, *value;

    if (Simulator_check_sim(self) != 0) {
        goto out;
    }
    for (j = 0; j < PyList_Size(py_pop_config); j++) {
        item = PyList_GetItem(py_pop_config, j);
        if (!PyDict_Check(item)) {
            PyErr_SetString(PyExc_TypeError, "not a dictionary");
            goto out;
        }
        value = get_dict_number(item, "initial_size");
        if (value == NULL) {
            goto out;
        }
        initial_size = PyFloat_AsDouble(value);
        value = get_dict_number(item, "growth_rate");
        if (value == NULL) {
            goto out;
        }
        growth_rate = PyFloat_AsDouble(value);
        err = msp_set_population_configuration(self->sim, j,
                initial_size, growth_rate);
        if (err != 0) {
            handle_input_error("population configuration", err);
            goto out;
        }
    }
    ret = 0;
out:
    return ret;
}

static int
Simulator_parse_pedigree(Simulator *self, PyObject *pedigree_dict)
{
    int ret = -1;
    size_t num_inds = 0;
    int ploidy;
    npy_intp *shape;
    int *inds = NULL;
    int *parents = NULL;
    double *times = NULL;
    uint32_t *is_sample = NULL;
    PyObject *inds_input = NULL;
    PyArrayObject *inds_array = NULL;
    PyObject *parents_input = NULL;
    PyArrayObject *parents_array = NULL;
    PyObject *times_input = NULL;
    PyArrayObject *times_array = NULL;
    PyObject *is_sample_input = NULL;
    PyArrayObject *is_sample_array = NULL;

    // Steps to loading arrays from dict (tentative) - taken from
    //    parse_node_table_dict()
    // 1) For each array, declare PyObject *arrayname_input,
    //    PyArrayObject *arrayname_array, and dtype* arrayname_data
    // 2) Using get_table_dict_value(), load array from dict into
    //    *arrayname_input
    // 3) Using table_read_column_array(), load arrayname_input
    //    into arrayname_array
    // 4) Set arrayname_data = PyArray_DATA(arrayname_array)
    // 5) In out: include Py_XDECREF(arrayname_array) for all arrays

    inds_input = get_table_dict_value(pedigree_dict, "individual", true);
    if (inds_input == NULL) {
        goto out;
    }
    parents_input = get_table_dict_value(pedigree_dict, "parents", true);
    if (parents_input == NULL) {
        goto out;
    }
    times_input = get_table_dict_value(pedigree_dict, "time", true);
    if (times_input == NULL) {
        goto out;
    }
    is_sample_input = get_table_dict_value(pedigree_dict, "is_sample", true);
    if (is_sample_input == NULL) {
        goto out;
    }

    /* Create the arrays */
    parents_array = (PyArrayObject *) PyArray_FROMANY(parents_input, NPY_INT32,
            2, 2, NPY_ARRAY_IN_ARRAY);
    if (parents_array == NULL) {
        goto out;
    }

    shape = PyArray_DIMS(parents_array);
    parents = PyArray_DATA(parents_array);
    num_inds = (size_t) shape[0];
    /* If NDIMS == 1 this is out of range - in that case set ploidy = 1 (see above) */
    ploidy = (int) shape[1];

    inds_array = table_read_column_array(inds_input, NPY_INT32, &num_inds, true);
    if (inds_array == NULL) {
        goto out;
    }
    inds = PyArray_DATA(inds_array);

    times_array = table_read_column_array(times_input, NPY_FLOAT64, &num_inds, true);
    if (times_array == NULL) {
        goto out;
    }
    times = PyArray_DATA(times_array);

    is_sample_array = table_read_column_array(is_sample_input, NPY_UINT32, &num_inds,
            true);
    if (is_sample_array == NULL) {
        goto out;
    }
    is_sample = PyArray_DATA(is_sample_array);

    ret = msp_alloc_pedigree(self->sim, num_inds, ploidy);
    if (ret != 0) {
        handle_input_error("pedigree", ret);
        goto out;
    }
    ret = msp_set_pedigree(self->sim, num_inds, inds, parents, times, is_sample);
    if (ret != 0) {
        /* TODO: Is this right to set here? */
        handle_input_error("pedigree", ret);
        goto out;
    }
    ret = 0;
out:
    Py_XDECREF(inds_array);
    Py_XDECREF(parents_array);
    Py_XDECREF(times_array);
    Py_XDECREF(is_sample_array);

    return ret;
}

static int
Simulator_parse_migration_matrix(Simulator *self, PyObject *py_migration_matrix)
{
    int ret = -1;
    int err;
    size_t num_populations, j, size;
    PyObject *value;
    double *migration_matrix = NULL;

    size = PyList_Size(py_migration_matrix);
    migration_matrix = PyMem_Malloc(size * sizeof(double));
    if (migration_matrix == NULL) {
        PyErr_NoMemory();
        goto out;
    }
    if (Simulator_check_sim(self) != 0) {
        goto out;
    }
    num_populations = msp_get_num_populations(self->sim);
    if (num_populations * num_populations != size) {
        PyErr_Format(PyExc_ValueError,
            "Migration matrix must be a flattened "
            "num_populations*num_populations square array");
        goto out;
    }
    for (j = 0; j < size; j++) {
        value = PyList_GetItem(py_migration_matrix, j);
        if (!PyNumber_Check(value)) {
            PyErr_Format(PyExc_TypeError, "Migration rate not a number");
            goto out;
        }
        migration_matrix[j] = PyFloat_AsDouble(value);
        if (migration_matrix[j] < 0.0) {
            PyErr_Format(PyExc_ValueError, "Negative values not permitted");
            goto out;
        }
    }
    err = msp_set_migration_matrix(self->sim, size, migration_matrix);
    if (err != 0) {
        handle_input_error("migration matrix", err);
        goto out;
    }
    ret = 0;
out:
    if (migration_matrix != NULL) {
        PyMem_Free(migration_matrix);
    }
    return ret;
}

static int
Simulator_parse_sweep_genic_selection_model(Simulator *self, PyObject *py_model,
        double reference_size)
{
    int ret = -1;
    int err;
    double position, start_frequency, end_frequency, alpha, dt;
    PyObject *value;

    value = get_dict_number(py_model, "position");
    if (value == NULL) {
        goto out;
    }
    position = PyFloat_AsDouble(value);

    value = get_dict_number(py_model, "start_frequency");
    if (value == NULL) {
        goto out;
    }
    start_frequency = PyFloat_AsDouble(value);

    value = get_dict_number(py_model, "end_frequency");
    if (value == NULL) {
        goto out;
    }
    end_frequency = PyFloat_AsDouble(value);

    value = get_dict_number(py_model, "alpha");
    if (value == NULL) {
        goto out;
    }
    alpha = PyFloat_AsDouble(value);

    value = get_dict_number(py_model, "dt");
    if (value == NULL) {
        goto out;
    }
    dt = PyFloat_AsDouble(value);

    err = msp_set_simulation_model_sweep_genic_selection(self->sim, reference_size,
            position, start_frequency, end_frequency, alpha, dt);
    if (err != 0) {
        handle_input_error("sweep genic selection", err);
        goto out;
    }
    ret = 0;
out:
    return ret;
}

static int
Simulator_parse_simulation_model(Simulator *self, PyObject *py_model)
{
    int ret = -1;
    int err = 0;
    PyObject *py_name = NULL;
    PyObject *hudson_s = NULL;
    PyObject *smc_s = NULL;
    PyObject *smc_prime_s = NULL;
    PyObject *dtwf_s = NULL;
    PyObject *wf_ped_s = NULL;
    PyObject *dirac_s = NULL;
    PyObject *beta_s = NULL;
    PyObject *sweep_genic_selection_s = NULL;
    PyObject *value;
    int is_hudson, is_dtwf, is_smc, is_smc_prime, is_dirac, is_beta, is_sweep_genic_selection;
    int is_wf_ped;
    double reference_size, psi, c, alpha, truncation_point;

    if (Simulator_check_sim(self) != 0) {
        goto out;
    }
    hudson_s = Py_BuildValue("s", "hudson");
    if (hudson_s == NULL) {
        goto out;
    }
    dtwf_s = Py_BuildValue("s", "dtwf");
    if (dtwf_s == NULL) {
        goto out;
    }
    wf_ped_s = Py_BuildValue("s", "wf_ped");
    if (wf_ped_s == NULL) {
        goto out;
    }
    smc_s = Py_BuildValue("s", "smc");
    if (smc_s == NULL) {
        goto out;
    }
    smc_prime_s = Py_BuildValue("s", "smc_prime");
    if (smc_prime_s == NULL) {
        goto out;
    }
    dirac_s = Py_BuildValue("s", "dirac");
    if (dirac_s == NULL) {
        goto out;
    }
    beta_s = Py_BuildValue("s", "beta");
    if (beta_s == NULL) {
        goto out;
    }
    sweep_genic_selection_s = Py_BuildValue("s", "sweep_genic_selection");
    if (sweep_genic_selection_s == NULL) {
        goto out;
    }

    value = get_dict_number(py_model, "reference_size");
    if (value == NULL) {
        goto out;
    }
    reference_size = PyFloat_AsDouble(value);
    if (reference_size <= 0) {
        PyErr_SetString(PyExc_ValueError, "population size must be >= 0");
        goto out;
    }
    py_name = get_dict_value(py_model, "name");
    if (py_name == NULL) {
        goto out;
    }
    /* We need to go through this tedious rigmarole because of string
     * handling in Python 3. By pushing the comparison up into Python
     * we don't need to worry about encodings, etc, etc.
     */
    is_hudson = PyObject_RichCompareBool(py_name, hudson_s, Py_EQ);
    if (is_hudson == -1) {
        goto out;
    }
    if (is_hudson) {
        err = msp_set_simulation_model_hudson(self->sim, reference_size);
    }

    is_dtwf = PyObject_RichCompareBool(py_name, dtwf_s, Py_EQ);
    if (is_dtwf == -1) {
        goto out;
    }
    if (is_dtwf) {
        err = msp_set_simulation_model_dtwf(self->sim, reference_size);
    }
    is_wf_ped = PyObject_RichCompareBool(py_name, wf_ped_s, Py_EQ);
    if (is_wf_ped == -1) {
        goto out;
    }
    if (is_wf_ped) {
        err = msp_set_simulation_model_wf_ped(self->sim, reference_size);
    }

    is_smc = PyObject_RichCompareBool(py_name, smc_s, Py_EQ);
    if (is_smc == -1) {
        goto out;
    }
    if (is_smc) {
        err = msp_set_simulation_model_smc(self->sim, reference_size);
    }

    is_smc_prime = PyObject_RichCompareBool(py_name, smc_prime_s, Py_EQ);
    if (is_smc_prime == -1) {
        goto out;
    }
    if (is_smc_prime) {
        err = msp_set_simulation_model_smc_prime(self->sim, reference_size);
    }

    is_dirac = PyObject_RichCompareBool(py_name, dirac_s, Py_EQ);
    if (is_dirac == -1) {
        goto out;
    }
    if (is_dirac) {
        value = get_dict_number(py_model, "psi");
        if (value == NULL) {
            goto out;
        }
        psi = PyFloat_AsDouble(value);
        value = get_dict_number(py_model, "c");
        if (value == NULL) {
            goto out;
        }
        c = PyFloat_AsDouble(value);
        if (psi <= 0 || psi >= 1.0) {
            PyErr_SetString(PyExc_ValueError, "Must have 0 < psi < 1");
            goto out;
        }
        if (c < 0) {
            PyErr_SetString(PyExc_ValueError, "c >= 0");
            goto out;
        }
        err = msp_set_simulation_model_dirac(self->sim, reference_size, psi, c);
    }

    is_beta = PyObject_RichCompareBool(py_name, beta_s, Py_EQ);
    if (is_beta == -1) {
        goto out;
    }
    if (is_beta) {
        value = get_dict_number(py_model, "alpha");
        if (value == NULL) {
            goto out;
        }
        alpha = PyFloat_AsDouble(value);
        value = get_dict_number(py_model, "truncation_point");
        if (value == NULL) {
            goto out;
        }
        truncation_point = PyFloat_AsDouble(value);
        /* TODO range checking on alpha and truncation_point */
        err = msp_set_simulation_model_beta(self->sim, reference_size,
                alpha, truncation_point);
    }

    is_sweep_genic_selection = PyObject_RichCompareBool(py_name,
            sweep_genic_selection_s, Py_EQ);
    if (is_sweep_genic_selection == -1) {
        goto out;
    }
    if (is_sweep_genic_selection) {
        ret = Simulator_parse_sweep_genic_selection_model(self, py_model, reference_size);
        if (ret != 0) {
            goto out;
        }
    }

    if (! (is_hudson || is_dtwf || is_smc || is_smc_prime || is_dirac
                || is_beta || is_sweep_genic_selection || is_wf_ped)) {
        PyErr_SetString(PyExc_ValueError, "Unknown simulation model");
        goto out;
    }
    if (err != 0) {
        handle_input_error("simulation model", err);
        goto out;
    }
    ret = 0;
out:
    Py_XDECREF(hudson_s);
    Py_XDECREF(dtwf_s);
    Py_XDECREF(wf_ped_s);
    Py_XDECREF(smc_s);
    Py_XDECREF(smc_prime_s);
    Py_XDECREF(beta_s);
    Py_XDECREF(dirac_s);
    Py_XDECREF(sweep_genic_selection_s);
    return ret;
}

static int
Simulator_parse_demographic_events(Simulator *self, PyObject *py_events)
{
    int ret = -1;
    Py_ssize_t j;
    double time, initial_size, growth_rate, migration_rate, proportion,
           strength;
    int err, population_id, matrix_index, source, destination;
    int is_population_parameter_change, is_migration_rate_change, is_mass_migration,
        is_simple_bottleneck, is_instantaneous_bottleneck, is_census_event;
    PyObject *item, *value, *type;
    PyObject *population_parameter_change_s = NULL;
    PyObject *migration_rate_change_s = NULL;
    PyObject *mass_migration_s = NULL;
    PyObject *simple_bottleneck_s = NULL;
    PyObject *instantaneous_bottleneck_s = NULL;
    PyObject *census_event_s = NULL;
    PyObject *initial_size_s = NULL;
    PyObject *growth_rate_s = NULL;

    if (Simulator_check_sim(self) != 0) {
        goto out;
    }
    /* Create the Python strings for comparison with the types and
     * dictionary lookups */
    population_parameter_change_s = Py_BuildValue("s",
            "population_parameters_change");
    if (population_parameter_change_s == NULL) {
        goto out;
    }
    migration_rate_change_s = Py_BuildValue("s", "migration_rate_change");
    if (migration_rate_change_s == NULL) {
        goto out;
    }
    mass_migration_s = Py_BuildValue("s", "mass_migration");
    if (mass_migration_s == NULL) {
        goto out;
    }
    simple_bottleneck_s = Py_BuildValue("s", "simple_bottleneck");
    if (simple_bottleneck_s == NULL) {
        goto out;
    }
    instantaneous_bottleneck_s = Py_BuildValue("s", "instantaneous_bottleneck");
    if (instantaneous_bottleneck_s == NULL) {
        goto out;
    }
    census_event_s = Py_BuildValue("s", "census_event");
    if (census_event_s == NULL) {
        goto out;
    }
    initial_size_s = Py_BuildValue("s", "initial_size");
    if (initial_size_s == NULL) {
        goto out;
    }
    growth_rate_s = Py_BuildValue("s", "growth_rate");
    if (growth_rate_s == NULL) {
        goto out;
    }

    for (j = 0; j < PyList_Size(py_events); j++) {
        item = PyList_GetItem(py_events, j);
        if (!PyDict_Check(item)) {
            PyErr_SetString(PyExc_TypeError, "not a dictionary");
            goto out;
        }
        value = get_dict_number(item, "time");
        if (value == NULL) {
            goto out;
        }
        time = PyFloat_AsDouble(value);
        if (time < 0) {
            PyErr_SetString(PyExc_ValueError, "negative times not valid");
            goto out;
        }
        type = get_dict_value(item, "type");
        if (type == NULL) {
            goto out;
        }
        /* We need to go through this tedious rigmarole because of string
         * handling in Python 3. By pushing the comparison up into Python
         * we don't need to worry about encodings, etc, etc.
         */
        is_population_parameter_change = PyObject_RichCompareBool(type,
                population_parameter_change_s, Py_EQ);
        if (is_population_parameter_change == -1) {
            goto out;
        }
        is_migration_rate_change = PyObject_RichCompareBool(type,
                migration_rate_change_s, Py_EQ);
        if (is_migration_rate_change == -1) {
            goto out;
        }
        is_mass_migration = PyObject_RichCompareBool(type, mass_migration_s,
                Py_EQ);
        if (is_mass_migration == -1) {
            goto out;
        }
        is_simple_bottleneck = PyObject_RichCompareBool(
                type, simple_bottleneck_s, Py_EQ);
        if (is_simple_bottleneck == -1) {
            goto out;
        }
        is_instantaneous_bottleneck = PyObject_RichCompareBool(
                type, instantaneous_bottleneck_s, Py_EQ);
        if (is_instantaneous_bottleneck == -1) {
            goto out;
        }
        is_census_event = PyObject_RichCompareBool(
                type, census_event_s, Py_EQ);
        if (is_census_event == -1) {
            goto out;
        }
        if (is_population_parameter_change) {
            initial_size = GSL_NAN;
            if (PyDict_Contains(item, initial_size_s)) {
                value = get_dict_number(item, "initial_size");
                if (value == NULL) {
                    goto out;
                }
                initial_size = PyFloat_AsDouble(value);
            }
            growth_rate = GSL_NAN;
            if (PyDict_Contains(item, growth_rate_s)) {
                value = get_dict_number(item, "growth_rate");
                if (value == NULL) {
                    goto out;
                }
                growth_rate = PyFloat_AsDouble(value);
            }
            value = get_dict_number(item, "population");
            if (value == NULL) {
                goto out;
            }
            population_id = (int) PyLong_AsLong(value);
            err = msp_add_population_parameters_change(self->sim, time,
                    population_id, initial_size, growth_rate);
        } else if (is_migration_rate_change) {
            value = get_dict_number(item, "migration_rate");
            if (value == NULL) {
                goto out;
            }
            migration_rate = PyFloat_AsDouble(value);
            value = get_dict_number(item, "matrix_index");
            if (value == NULL) {
                goto out;
            }
            matrix_index = (int) PyLong_AsLong(value);
            err = msp_add_migration_rate_change(self->sim, time, matrix_index,
                    migration_rate);
        } else if (is_mass_migration) {
            value = get_dict_number(item, "proportion");
            if (value == NULL) {
                goto out;
            }
            proportion = PyFloat_AsDouble(value);
            value = get_dict_number(item, "source");
            if (value == NULL) {
                goto out;
            }
            source = (int) PyLong_AsLong(value);
            value = get_dict_number(item, "dest");
            if (value == NULL) {
                goto out;
            }
            destination = (int) PyLong_AsLong(value);
            err = msp_add_mass_migration(self->sim, time, source, destination,
                    proportion);
        } else if (is_simple_bottleneck) {
            value = get_dict_number(item, "proportion");
            if (value == NULL) {
                goto out;
            }
            proportion = PyFloat_AsDouble(value);
            value = get_dict_number(item, "population");
            if (value == NULL) {
                goto out;
            }
            population_id = (int) PyLong_AsLong(value);
            err = msp_add_simple_bottleneck(self->sim, time, population_id,
                    proportion);
        } else if (is_instantaneous_bottleneck) {
            value = get_dict_number(item, "strength");
            if (value == NULL) {
                goto out;
            }
            strength = PyFloat_AsDouble(value);
            value = get_dict_number(item, "population");
            if (value == NULL) {
                goto out;
            }
            population_id = (int) PyLong_AsLong(value);
            err = msp_add_instantaneous_bottleneck(self->sim, time, population_id,
                    strength);
        } else if (is_census_event) {
            err = msp_add_census_event(self->sim, time);
        } else {
            PyErr_Format(PyExc_ValueError, "Unknown demographic event type");
            goto out;
        }
        if (err != 0) {
            PyErr_Format(MsprimeInputError,
                    "Input error in demographic_events[%d]: %s", j,
                    msp_strerror(err));
            goto out;
        }
    }
    ret = 0;
out:
    Py_XDECREF(population_parameter_change_s);
    Py_XDECREF(migration_rate_change_s);
    Py_XDECREF(mass_migration_s);
    Py_XDECREF(simple_bottleneck_s);
    Py_XDECREF(instantaneous_bottleneck_s);
    Py_XDECREF(initial_size_s);
    Py_XDECREF(growth_rate_s);
    Py_XDECREF(census_event_s);
    return ret;
}

static void
Simulator_dealloc(Simulator* self)
{
    if (self->sim != NULL) {
        msp_free(self->sim);
        PyMem_Free(self->sim);
        self->sim = NULL;
    }
    Py_XDECREF(self->random_generator);
    Py_XDECREF(self->recombination_map);
    Py_XDECREF(self->tables);
    Py_TYPE(self)->tp_free((PyObject*)self);
}

static int
Simulator_init(Simulator *self, PyObject *args, PyObject *kwds)
{
    int ret = -1;
    int sim_ret;
    static char *kwlist[] = {"samples", "recombination_map", "random_generator",
        "tables", "population_configuration", "pedigree", "migration_matrix",
        "demographic_events", "model", "avl_node_block_size", "segment_block_size",
        "node_mapping_block_size", "store_migrations", "start_time",
        "store_full_arg", "num_labels", "gene_conversion_rate",
        "gene_conversion_track_length", NULL};
    PyObject *py_samples = NULL;
    PyObject *migration_matrix = NULL;
    PyObject *population_configuration = NULL;
    PyObject *pedigree = Py_None;
    PyObject *demographic_events = NULL;
    PyObject *py_model = NULL;
    LightweightTableCollection *tables = NULL;
    RandomGenerator *random_generator = NULL;
    RecombinationMap *recombination_map = NULL;
    sample_t *samples = NULL;
    /* parameter defaults */
    Py_ssize_t num_samples = 2;
    Py_ssize_t avl_node_block_size = 10;
    Py_ssize_t segment_block_size = 10;
    Py_ssize_t node_mapping_block_size = 10;
    Py_ssize_t num_labels = 1;
    Py_ssize_t num_populations = 1;
    int store_migrations = 0;
    int store_full_arg = 0;
    double start_time = -1;
    double gene_conversion_rate = 0;
    double gene_conversion_track_length = 1.0;

    self->sim = NULL;
    self->random_generator = NULL;
    self->recombination_map = NULL;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O!O!O!O!|O!OO!O!O!nnnidindd", kwlist,
            &PyList_Type, &py_samples,
            &RecombinationMapType, &recombination_map,
            &RandomGeneratorType, &random_generator,
            &LightweightTableCollectionType, &tables,
            /* optional */
            &PyList_Type, &population_configuration,
            &pedigree,
            &PyList_Type, &migration_matrix,
            &PyList_Type, &demographic_events,
            &PyDict_Type, &py_model,
            &avl_node_block_size, &segment_block_size,
            &node_mapping_block_size, &store_migrations, &start_time,
            &store_full_arg, &num_labels,
            &gene_conversion_rate, &gene_conversion_track_length)) {
        goto out;
    }
    self->random_generator = random_generator;
    /* printf("Random seed: %lu\n", random_generator->seed); */
    self->recombination_map = recombination_map;
    self->tables = tables;
    Py_INCREF(self->random_generator);
    Py_INCREF(self->recombination_map);
    Py_INCREF(self->tables);

    if (RandomGenerator_check_state(self->random_generator) != 0) {
        goto out;
    }
    if (RecombinationMap_check_recomb_map(recombination_map) != 0) {
        goto out;
    }
    if (population_configuration != NULL) {
        num_populations = PyList_Size(population_configuration);
        if (num_populations == 0) {
            PyErr_SetString(PyExc_ValueError, "Empty population configuration");
            goto out;
        }
    }
    if (parse_samples(py_samples, num_populations, &num_samples, &samples) != 0) {
        goto out;
    }
    self->sim = PyMem_Malloc(sizeof(msp_t));
    if (self->sim == NULL) {
        PyErr_NoMemory();
        goto out;
    }
    sim_ret = msp_alloc(self->sim, (size_t) num_samples, samples,
            recombination_map->recomb_map, tables->tables,
            self->random_generator->rng);
    if (sim_ret != 0) {
        handle_input_error("simulator alloc", sim_ret);
        goto out;
    }
    if (py_model != NULL) {
        if (Simulator_parse_simulation_model(self, py_model) != 0) {
            goto out;
        }
    }
    if (start_time >= 0) {
        sim_ret = msp_set_start_time(self->sim, start_time);
        if (sim_ret != 0) {
            handle_input_error("start time", sim_ret);
            goto out;
        }
    }
    sim_ret = msp_set_store_migrations(self->sim, (bool) store_migrations);
    if (sim_ret != 0) {
        handle_input_error("store migrations", sim_ret);
        goto out;
    }
    sim_ret = msp_set_avl_node_block_size(self->sim,
            (size_t) avl_node_block_size);
    if (sim_ret != 0) {
        handle_input_error("avl_node_block_size", sim_ret);
        goto out;
    }
    sim_ret = msp_set_segment_block_size(self->sim,
            (size_t) segment_block_size);
    if (sim_ret != 0) {
        handle_input_error("segment_block_size", sim_ret);
        goto out;
    }
    sim_ret = msp_set_node_mapping_block_size(self->sim,
            (size_t) node_mapping_block_size);
    if (sim_ret != 0) {
        handle_input_error("node_mapping_block_size", sim_ret);
        goto out;
    }
    sim_ret = msp_set_gene_conversion_rate(self->sim, gene_conversion_rate,
            gene_conversion_track_length);
    if (sim_ret != 0) {
        handle_input_error("set_gene_conversion_rate", sim_ret);
        goto out;
    }

    sim_ret = msp_set_dimensions(self->sim, (size_t) num_populations, (size_t) num_labels);
    if (sim_ret != 0) {
        handle_input_error("set_dimensions", sim_ret);
        goto out;
    }
    if (pedigree != Py_None) {
        if (!PyDict_Check(pedigree)) {
            PyErr_SetString(PyExc_TypeError, "Pedigree must be a dictionary");
            goto out;
        }
        if (self->sim->model.type != MSP_MODEL_WF_PED) {
            PyErr_SetString(PyExc_ValueError,
                "A pedigree can only be supplied under the "
                "`msprime.WrightFisherPedigree` simulation model");
            goto out;
        }
        sim_ret = Simulator_parse_pedigree(self, pedigree);
        if (sim_ret != 0) {
            goto out;
        }
    }
    if (population_configuration != NULL) {
        if (Simulator_parse_population_configuration(self, population_configuration) != 0) {
            goto out;
        }
        if (migration_matrix == NULL) {
            PyErr_SetString(PyExc_ValueError,
                "A migration matrix must be provided when a non-default "
                "population configuration is used.");
            goto out;
        }
        if (Simulator_parse_migration_matrix(self,
                migration_matrix) != 0) {
            goto out;
        }
    } else if (migration_matrix != NULL) {
        PyErr_SetString(PyExc_ValueError,
            "Cannot supply migration_matrix without "
            "population_configuration.");
        goto out;
    }
    if (demographic_events != NULL) {
        if (Simulator_parse_demographic_events(self, demographic_events) != 0) {
            goto out;
        }
    }
    msp_set_store_full_arg(self->sim, store_full_arg);

    sim_ret = msp_initialise(self->sim);
    if (sim_ret != 0) {
        handle_input_error("initialise", sim_ret);
        goto out;
    }
    ret = 0;
out:
    if (samples != NULL) {
        PyMem_Free(samples);
    }
    return ret;
}

static PyMemberDef Simulator_members[] = {
    {NULL}  /* Sentinel */
};


static PyObject *
Simulator_get_model(Simulator *self)
{
    PyObject *ret = NULL;
    PyObject *d = NULL;
    PyObject *value = NULL;
    PyArrayObject *array = NULL;
    simulation_model_t *model;

    if (Simulator_check_sim(self) != 0) {
        goto out;
    }
    model = msp_get_model(self->sim);
    d = Py_BuildValue("{ss,sd}",
            "name", msp_get_model_name(self->sim),
            "reference_size", msp_get_model(self->sim)->reference_size);
    if (model->type == MSP_MODEL_DIRAC) {
        value = Py_BuildValue("d", model->params.dirac_coalescent.psi);
        if (value == NULL) {
            goto out;
        }
        if (PyDict_SetItemString(d, "psi", value) != 0) {
            goto out;
        }
        Py_DECREF(value);
        value = NULL;
        value = Py_BuildValue("d", model->params.dirac_coalescent.c);
        if (value == NULL) {
            goto out;
        }
        if (PyDict_SetItemString(d, "c", value) != 0) {
            goto out;
        }
        Py_DECREF(value);
        value = NULL;
    } else if (model->type == MSP_MODEL_BETA) {
        value = Py_BuildValue("d", model->params.beta_coalescent.alpha);
        if (value == NULL) {
            goto out;
        }
        if (PyDict_SetItemString(d, "alpha", value) != 0) {
            goto out;
        }
        Py_DECREF(value);
        value = NULL;
        value = Py_BuildValue("d", model->params.beta_coalescent.truncation_point);
        if (value == NULL) {
            goto out;
        }
        if (PyDict_SetItemString(d, "truncation_point", value) != 0) {
            goto out;
        }
        Py_DECREF(value);
        value = NULL;
    } else if (model->type == MSP_MODEL_SWEEP) {
        value = Py_BuildValue("d", model->params.sweep.locus);
        if (value == NULL) {
            goto out;
        }
        if (PyDict_SetItemString(d, "locus", value) != 0) {
            goto out;
        }
        Py_DECREF(value);
        value = NULL;
        /* TODO fill in the parameters for the different types of trajectories. */
    }
    ret = d;
    d = NULL;
out:
    Py_XDECREF(d);
    Py_XDECREF(value);
    Py_XDECREF(array);
    return ret;
}

static PyObject *
Simulator_set_model(Simulator *self, PyObject *args)
{
    PyObject *ret = NULL;
    PyObject *py_model = NULL;

    if (Simulator_check_sim(self) != 0) {
        goto out;
    }
    if (!PyArg_ParseTuple(args, "O!", &PyDict_Type, &py_model)) {
        goto out;
    }
    if (Simulator_parse_simulation_model(self, py_model) != 0) {
        goto out;
    }
    ret = Py_BuildValue("");
out:
    return ret;
}

static PyObject *
Simulator_get_store_migrations(Simulator *self)
{
    PyObject *ret = NULL;
    if (Simulator_check_sim(self) != 0) {
        goto out;
    }
    ret = Py_BuildValue("i", (Py_ssize_t) msp_get_store_migrations(self->sim));
out:
    return ret;
}

static PyObject *
Simulator_get_num_samples(Simulator *self)
{
    PyObject *ret = NULL;
    if (Simulator_check_sim(self) != 0) {
        goto out;
    }
    ret = Py_BuildValue("n", (Py_ssize_t) msp_get_num_samples(self->sim));
out:
    return ret;
}

static PyObject *
Simulator_get_num_populations(Simulator *self)
{
    PyObject *ret = NULL;
    if (Simulator_check_sim(self) != 0) {
        goto out;
    }
    ret = Py_BuildValue("n", (Py_ssize_t) msp_get_num_populations(self->sim));
out:
    return ret;
}

static PyObject *
Simulator_get_num_labels(Simulator *self)
{
    PyObject *ret = NULL;
    if (Simulator_check_sim(self) != 0) {
        goto out;
    }
    ret = Py_BuildValue("n", (Py_ssize_t) msp_get_num_labels(self->sim));
out:
    return ret;
}

static PyObject *
Simulator_get_sequence_length(Simulator *self)
{
    PyObject *ret = NULL;
    if (Simulator_check_sim(self) != 0) {
        goto out;
    }
    ret = Py_BuildValue("n", (Py_ssize_t) recomb_map_get_sequence_length(
                &self->sim->recomb_map));
out:
    return ret;
}

static PyObject *
Simulator_get_segment_block_size(Simulator  *self)
{
    PyObject *ret = NULL;
    if (Simulator_check_sim(self) != 0) {
        goto out;
    }
    ret = Py_BuildValue("n", (Py_ssize_t) self->sim->segment_block_size);
out:
    return ret;
}

static PyObject *
Simulator_get_avl_node_block_size(Simulator  *self)
{
    PyObject *ret = NULL;
    if (Simulator_check_sim(self) != 0) {
        goto out;
    }
    ret = Py_BuildValue("n", (Py_ssize_t) self->sim->avl_node_block_size);
out:
    return ret;
}

static PyObject *
Simulator_get_node_mapping_block_size(Simulator  *self)
{
    PyObject *ret = NULL;
    if (Simulator_check_sim(self) != 0) {
        goto out;
    }
    ret = Py_BuildValue("n", (Py_ssize_t) self->sim->node_mapping_block_size);
out:
    return ret;
}

static PyObject *
Simulator_get_time(Simulator  *self)
{
    PyObject *ret = NULL;
    if (Simulator_check_sim(self) != 0) {
        goto out;
    }
    ret = Py_BuildValue("d", msp_get_time(self->sim));
out:
    return ret;
}

static PyObject *
Simulator_get_num_ancestors(Simulator *self)
{
    PyObject *ret = NULL;
    if (Simulator_check_sim(self) != 0) {
        goto out;
    }
    ret = Py_BuildValue("n", (Py_ssize_t) msp_get_num_ancestors(self->sim));
out:
    return ret;
}

static PyObject *
Simulator_get_num_common_ancestor_events(Simulator  *self)
{
    PyObject *ret = NULL;
    if (Simulator_check_sim(self) != 0) {
        goto out;
    }
    ret = Py_BuildValue("n",
        (Py_ssize_t) msp_get_num_common_ancestor_events(self->sim));
out:
    return ret;
}

static PyObject *
Simulator_get_num_rejected_common_ancestor_events(Simulator  *self)
{
    PyObject *ret = NULL;
    if (Simulator_check_sim(self) != 0) {
        goto out;
    }
    ret = Py_BuildValue("n",
        (Py_ssize_t) msp_get_num_rejected_common_ancestor_events(self->sim));
out:
    return ret;
}

static PyObject *
Simulator_get_num_recombination_events(Simulator  *self)
{
    PyObject *ret = NULL;
    if (Simulator_check_sim(self) != 0) {
        goto out;
    }
    ret = Py_BuildValue("n",
        (Py_ssize_t) msp_get_num_recombination_events(self->sim));
out:
    return ret;
}

static PyObject *
Simulator_get_num_gene_conversion_events(Simulator  *self)
{
    PyObject *ret = NULL;
    if (Simulator_check_sim(self) != 0) {
        goto out;
    }
    ret = Py_BuildValue("n",
        (Py_ssize_t) msp_get_num_gene_conversion_events(self->sim));
out:
    return ret;
}

static PyObject *
Simulator_get_num_migration_events(Simulator  *self)
{
    PyObject *ret = NULL;
    PyObject *arr = NULL;
    int err;
    npy_intp size;

    if (Simulator_check_sim(self) != 0) {
        goto out;
    }
    size = msp_get_num_populations(self->sim);
    size *= size;
    arr = PyArray_SimpleNew(1, &size, NPY_UINTP);
    if (arr == NULL) {
        goto out;
    }
    err = msp_get_num_migration_events(self->sim, PyArray_DATA((PyArrayObject *) arr));
    if (err != 0) {
        handle_library_error(err);
        goto out;
    }
    ret = arr;
    arr = NULL;
out:
    Py_XDECREF(arr);
    return ret;
}

static PyObject *
Simulator_get_num_multiple_recombination_events(Simulator  *self)
{
    PyObject *ret = NULL;
    if (Simulator_check_sim(self) != 0) {
        goto out;
    }
    ret = Py_BuildValue("n", (Py_ssize_t) self->sim->num_multiple_re_events);
out:
    return ret;
}

static PyObject *
Simulator_get_num_avl_node_blocks(Simulator  *self)
{
    PyObject *ret = NULL;
    if (Simulator_check_sim(self) != 0) {
        goto out;
    }
    ret = Py_BuildValue("n", (Py_ssize_t) msp_get_num_avl_node_blocks(self->sim));
out:
    return ret;
}

static PyObject *
Simulator_get_num_node_mapping_blocks(Simulator  *self)
{
    PyObject *ret = NULL;
    if (Simulator_check_sim(self) != 0) {
        goto out;
    }
    ret = Py_BuildValue("n", (Py_ssize_t) msp_get_num_node_mapping_blocks(self->sim));
out:
    return ret;
}

static PyObject *
Simulator_get_num_segment_blocks(Simulator  *self)
{
    PyObject *ret = NULL;
    if (Simulator_check_sim(self) != 0) {
        goto out;
    }
    ret = Py_BuildValue("n", (Py_ssize_t) msp_get_num_segment_blocks(self->sim));
out:
    return ret;
}

static PyObject *
Simulator_get_num_breakpoints(Simulator  *self)
{
    PyObject *ret = NULL;
    if (Simulator_check_sim(self) != 0) {
        goto out;
    }
    ret = Py_BuildValue("n", (Py_ssize_t) msp_get_num_breakpoints(self->sim));
out:
    return ret;
}

static PyObject *
Simulator_get_num_edges(Simulator  *self)
{
    PyObject *ret = NULL;
    if (Simulator_check_sim(self) != 0) {
        goto out;
    }
    ret = Py_BuildValue("n", (Py_ssize_t) msp_get_num_edges(self->sim));
out:
    return ret;
}

static PyObject *
Simulator_get_num_nodes(Simulator  *self)
{
    PyObject *ret = NULL;
    if (Simulator_check_sim(self) != 0) {
        goto out;
    }
    ret = Py_BuildValue("n", (Py_ssize_t) msp_get_num_nodes(self->sim));
out:
    return ret;
}

static PyObject *
Simulator_get_num_migrations(Simulator  *self)
{
    PyObject *ret = NULL;
    if (Simulator_check_sim(self) != 0) {
        goto out;
    }
    ret = Py_BuildValue("n",
            (Py_ssize_t) msp_get_num_migrations(self->sim));
out:
    return ret;
}

static PyObject *
Simulator_individual_to_python(Simulator *self, segment_t *ind)
{
    PyObject *ret = NULL;
    PyObject *l = NULL;
    PyObject *t = NULL;
    size_t num_segments, j;
    segment_t *u;

    num_segments = 0;
    u = ind;
    while (u != NULL) {
        num_segments++;
        u = u->next;
    }
    l = PyList_New(num_segments);
    if (l == NULL) {
        goto out;
    }
    u = ind;
    j = 0;
    while (u != NULL) {
        t = Py_BuildValue("(d,d,I,I)", u->left, u->right, u->value,
                u->population_id);
        if (t == NULL) {
            Py_DECREF(l);
            goto out;
        }
        PyList_SET_ITEM(l, j, t);
        j++;
        u = u->next;
    }
    ret = l;
out:
    return ret;
}

static PyObject *
Simulator_get_ancestors(Simulator *self)
{
    PyObject *ret = NULL;
    PyObject *l = NULL;
    PyObject *py_ind = NULL;
    segment_t **ancestors = NULL;
    size_t num_ancestors, j;
    int err;

    if (Simulator_check_sim(self) != 0) {
        goto out;
    }
    num_ancestors = msp_get_num_ancestors(self->sim);
    ancestors = PyMem_Malloc(num_ancestors * sizeof(segment_t *));
    if (ancestors == NULL) {
        PyErr_NoMemory();
        goto out;
    }
    err = msp_get_ancestors(self->sim, ancestors);
    if (err != 0) {
        handle_library_error(err);
        goto out;
    }
    l = PyList_New(num_ancestors);
    if (l == NULL) {
        goto out;
    }
    for (j = 0; j < num_ancestors; j++) {
        py_ind = Simulator_individual_to_python(self, ancestors[j]);
        if (py_ind == NULL) {
            Py_DECREF(l);
            goto out;
        }
        PyList_SET_ITEM(l, j, py_ind);
    }
    ret = l;
out:
    if (ancestors != NULL) {
        PyMem_Free(ancestors);
    }
    return ret;
}

static PyObject *
Simulator_get_breakpoints(Simulator *self)
{
    PyObject *ret = NULL;
    PyObject *arr = NULL;
    npy_intp num_breakpoints;
    int err;

    if (Simulator_check_sim(self) != 0) {
        goto out;
    }
    num_breakpoints = msp_get_num_breakpoints(self->sim);
    arr = PyArray_SimpleNew(1, &num_breakpoints, NPY_UINTP);
    if (arr == NULL) {
        goto out;
    }
    err = msp_get_breakpoints(self->sim, PyArray_DATA((PyArrayObject *) arr));
    if (err != 0) {
        handle_library_error(err);
        goto out;
    }
    ret = arr;
    arr = NULL;
out:
    Py_XDECREF(arr);
    return ret;
}

static PyObject *
Simulator_get_migration_matrix(Simulator *self)
{
    PyObject *ret = NULL;
    PyObject *arr = NULL;
    npy_intp N2;
    int err;

    if (Simulator_check_sim(self) != 0) {
        goto out;
    }
    N2 = msp_get_num_populations(self->sim);
    N2 *= N2;
    // TODO return 2d numpy array and fix the breakage
    arr = PyArray_SimpleNew(1, &N2, NPY_FLOAT64);
    if (arr == NULL) {
        goto out;
    }
    err = msp_get_migration_matrix(self->sim, PyArray_DATA((PyArrayObject *)arr));
    if (err != 0) {
        handle_library_error(err);
        goto out;
    }
    ret = arr;
    arr = NULL;
out:
    Py_XDECREF(arr);
    return ret;
}

/* TODO these get_edge/nodes/migration methods are no longer necessary
 * once we have an direct reference to the underlying tables. They're
 * only used for testing, so remove and update the tests to work from the
 * tables instead.
 */
static PyObject *
Simulator_get_edges(Simulator *self)
{
    PyObject *ret = NULL;
    PyObject *l = NULL;
    PyObject *py_edge = NULL;
    tsk_edge_table_t *edges = NULL;
    tsk_edge_t edge;
    size_t num_edges, j;

    if (Simulator_check_sim(self) != 0) {
        goto out;
    }
    num_edges = msp_get_num_edges(self->sim);
    edges = &self->sim->tables->edges;
    l = PyList_New(num_edges);
    if (l == NULL) {
        goto out;
    }
    for (j = 0; j < num_edges; j++) {
        edge.left = edges->left[j];
        edge.right = edges->right[j];
        edge.parent = edges->parent[j];
        edge.child = edges->child[j];
        py_edge = make_edge(&edge);
        if (py_edge == NULL) {
            Py_DECREF(l);
            goto out;
        }
        PyList_SET_ITEM(l, j, py_edge);
    }
    ret = l;
out:
    return ret;
}

static PyObject *
Simulator_get_nodes(Simulator *self)
{
    PyObject *ret = NULL;
    PyObject *l = NULL;
    PyObject *py_node = NULL;
    tsk_node_table_t *nodes;
    tsk_node_t node;
    size_t num_nodes, j;

    if (Simulator_check_sim(self) != 0) {
        goto out;
    }
    num_nodes = msp_get_num_nodes(self->sim);
    nodes = &self->sim->tables->nodes;
    l = PyList_New(num_nodes);
    if (l == NULL) {
        goto out;
    }
    for (j = 0; j < num_nodes; j++) {
        node.flags = nodes->flags[j];
        node.time = nodes->time[j];
        node.population = nodes->population[j];
        node.individual = nodes->individual[j];
        node.metadata_length = 0;
        py_node = make_node(&node);
        if (py_node == NULL) {
            Py_DECREF(l);
            goto out;
        }
        PyList_SET_ITEM(l, j, py_node);
    }
    ret = l;
out:
    return ret;
}

static PyObject *
Simulator_get_migrations(Simulator *self)
{
    PyObject *ret = NULL;
    PyObject *l = NULL;
    PyObject *py_mr = NULL;
    tsk_migration_table_t *migrations = NULL;
    tsk_migration_t mr;
    size_t num_migrations, j;

    if (Simulator_check_sim(self) != 0) {
        goto out;
    }
    num_migrations = msp_get_num_migrations(self->sim);
    migrations = &self->sim->tables->migrations;
    l = PyList_New(num_migrations);
    if (l == NULL) {
        goto out;
    }
    for (j = 0; j < num_migrations; j++) {
        mr.left = migrations->left[j];
        mr.right = migrations->right[j];
        mr.node = migrations->node[j];
        mr.source = migrations->source[j];
        mr.dest = migrations->dest[j];
        mr.time = migrations->time[j];
        py_mr = make_migration(&mr);
        if (py_mr == NULL) {
            Py_DECREF(l);
            goto out;
        }
        PyList_SET_ITEM(l, j, py_mr);
    }
    ret = l;
out:
    return ret;
}

static PyObject *
Simulator_get_population_configuration(Simulator *self)
{
    PyObject *ret = NULL;
    PyObject *l = NULL;
    PyObject *d = NULL;
    size_t j = 0;
    size_t num_populations;
    int sim_ret = 0;
    double initial_size, growth_rate;

    if (Simulator_check_sim(self) != 0) {
        goto out;
    }
    num_populations = msp_get_num_populations(self->sim);
    l = PyList_New(num_populations);
    if (l == NULL) {
        goto out;
    }
    for (j = 0; j < num_populations; j++) {
        sim_ret = msp_get_population_configuration(self->sim, j,
            &initial_size, &growth_rate);
        if (sim_ret != 0) {
            handle_library_error(sim_ret);
            goto out;
        }
        d = Py_BuildValue("{s:d,s:d}",
               "initial_size", initial_size,
               "growth_rate", growth_rate);
        if (d == NULL) {
            goto out;
        }
        PyList_SET_ITEM(l, j, d);
    }
    ret = l;
    l = NULL;
out:
    Py_XDECREF(l);
    return ret;
}

static PyObject *
Simulator_get_samples(Simulator *self)
{
    PyObject *ret = NULL;
    PyObject *l = NULL;
    PyObject *t = NULL;
    sample_t *samples = NULL;
    size_t j = 0;
    size_t num_samples;
    int population;
    int sim_ret = 0;

    if (Simulator_check_sim(self) != 0) {
        goto out;
    }
    num_samples = msp_get_num_samples(self->sim);
    sim_ret = msp_get_samples(self->sim, &samples);
    if (sim_ret != 0) {
        handle_library_error(sim_ret);
        goto out;
    }
    l = PyList_New(num_samples);
    if (l == NULL) {
        goto out;
    }
    for (j = 0; j < num_samples; j++) {
        population = samples[j].population_id == TSK_NULL? -1:
            samples[j].population_id;
        t = Py_BuildValue("id", population, samples[j].time);
        if (t == NULL) {
            goto out;
        }
        PyList_SET_ITEM(l, j, t);
    }
    ret = l;
    l = NULL;
out:
    Py_XDECREF(l);
    return ret;
}


static PyObject *
Simulator_run(Simulator *self, PyObject *args)
{
    PyObject *ret = NULL;
    int status, not_done, coalesced;
    uint64_t chunk = 1024;
    double end_time = DBL_MAX;

    if (Simulator_check_sim(self) != 0) {
        goto out;
    }
    if (!PyArg_ParseTuple(args, "|d", &end_time)) {
        goto out;
    }
    if (end_time < 0) {
        PyErr_SetString(PyExc_ValueError, "end_time must be > 0");
        goto out;
    }

    not_done = 1;
    while (not_done) {
        Py_BEGIN_ALLOW_THREADS
        status = msp_run(self->sim, end_time, chunk);
        Py_END_ALLOW_THREADS
        if (status < 0) {
            handle_library_error(status);
            goto out;
        }
        not_done = status == MSP_EXIT_MAX_EVENTS;
        if (PyErr_CheckSignals() < 0) {
            goto out;
        }
    }
    coalesced = status == 0;
    /* return True if complete coalescence has occured */
    ret = coalesced ? Py_True : Py_False;

    Py_INCREF(ret);
out:
    return ret;
}

static PyObject *
Simulator_run_event(Simulator *self)
{
    PyObject *ret = NULL;
    int status, coalesced;

    if (Simulator_check_sim(self) != 0) {
        goto out;
    }
    status = msp_run(self->sim, DBL_MAX, 1);
    if (status < 0) {
        handle_library_error(status);
        goto out;
    }
    coalesced = status == 0;
    /* return True if complete coalescence has occured */
    ret = coalesced ? Py_True : Py_False;
    Py_INCREF(ret);
out:
    return ret;
}

static PyObject *
Simulator_finalise_tables(Simulator *self)
{
    PyObject *ret = NULL;
    int status;

    /* finalise the tables so that any uncoalesced segments are recorded */
    status = msp_finalise_tables(self->sim);
    if (status != 0) {
        handle_library_error(status);
        goto out;
    }
    ret = Py_BuildValue("");
out:
    return ret;
}

static PyObject *
Simulator_reset(Simulator *self)
{
    PyObject *ret = NULL;
    int status;

    if (Simulator_check_sim(self) != 0) {
        goto out;
    }
    status = msp_reset(self->sim);
    if (status < 0) {
        handle_library_error(status);
        goto out;
    }
    ret = Py_BuildValue("");
out:
    return ret;
}

static PyObject *
Simulator_debug_demography(Simulator *self)
{
    PyObject *ret = NULL;
    int status;
    double end_time;

    if (Simulator_check_sim(self) != 0) {
        goto out;
    }
    status = msp_debug_demography(self->sim, &end_time);
    if (status < 0) {
        handle_library_error(status);
        goto out;
    }
    ret = Py_BuildValue("d", end_time);
out:
    return ret;
}

static PyObject *
Simulator_compute_population_size(Simulator *self, PyObject *args)
{

    PyObject *ret = NULL;
    int sim_ret, population_id;
    double time, size;

    if (Simulator_check_sim(self) != 0) {
        goto out;
    }
    if (!PyArg_ParseTuple(args, "id", &population_id, &time)) {
        goto out;
    }
    sim_ret = msp_compute_population_size(self->sim, population_id, time, &size);
    if (sim_ret != 0) {
        handle_library_error(sim_ret);
        goto out;
    }
    ret = Py_BuildValue("d", size);
out:
    return ret;
}


static PyMethodDef Simulator_methods[] = {
    {"set_model", (PyCFunction) Simulator_set_model, METH_VARARGS,
            "Sets the simulation model." },
    {"get_model", (PyCFunction) Simulator_get_model, METH_NOARGS,
            "Returns the simulation model" },
    {"get_store_migrations",
            (PyCFunction) Simulator_get_store_migrations, METH_NOARGS,
            "Returns True if the simulator should store migration records." },
    {"get_num_samples", (PyCFunction) Simulator_get_num_samples, METH_NOARGS,
            "Returns the sample size" },
    {"get_num_populations", (PyCFunction) Simulator_get_num_populations, METH_NOARGS,
            "Returns the number of populations." },
    {"get_num_labels", (PyCFunction) Simulator_get_num_labels, METH_NOARGS,
            "Returns the number of labels." },
    {"get_sequence_length", (PyCFunction) Simulator_get_sequence_length, METH_NOARGS,
        "Returns the sequence length for this simulator."},
    {"get_segment_block_size",
            (PyCFunction) Simulator_get_segment_block_size, METH_NOARGS,
            "Returns segment block size." },
    {"get_avl_node_block_size",
            (PyCFunction) Simulator_get_avl_node_block_size, METH_NOARGS,
            "Returns avl_node block size" },
    {"get_node_mapping_block_size",
            (PyCFunction) Simulator_get_node_mapping_block_size, METH_NOARGS,
            "Returns node_mapping block size" },
    {"get_time", (PyCFunction) Simulator_get_time, METH_NOARGS,
            "Returns the current simulation time" },
    {"get_num_ancestors", (PyCFunction) Simulator_get_num_ancestors, METH_NOARGS,
            "Returns the number of ancestors" },
    {"get_num_common_ancestor_events",
            (PyCFunction) Simulator_get_num_common_ancestor_events, METH_NOARGS,
            "Returns the number of common_ancestor_events" },
    {"get_num_rejected_common_ancestor_events",
            (PyCFunction) Simulator_get_num_rejected_common_ancestor_events,
            METH_NOARGS, "Returns the number of rejected common_ancestor_events" },
    {"get_num_recombination_events",
            (PyCFunction) Simulator_get_num_recombination_events, METH_NOARGS,
            "Returns the number of recombination_events" },
    {"get_num_gene_conversion_events",
            (PyCFunction) Simulator_get_num_gene_conversion_events, METH_NOARGS,
            "Returns the number of gene_conversion_events" },
    {"get_num_migration_events",
            (PyCFunction) Simulator_get_num_migration_events, METH_NOARGS,
            "Returns the number of migration events" },
    {"get_num_multiple_recombination_events",
            (PyCFunction) Simulator_get_num_multiple_recombination_events,
            METH_NOARGS,
            "Returns the number of recombination_events that occur at an "
            "existing breakpoint" },
    {"get_num_avl_node_blocks",
            (PyCFunction) Simulator_get_num_avl_node_blocks, METH_NOARGS,
            "Returns the number of avl_node memory blocks"},
    {"get_num_node_mapping_blocks",
            (PyCFunction) Simulator_get_num_node_mapping_blocks, METH_NOARGS,
            "Returns the number of node_mapping memory blocks"},
    {"get_num_segment_blocks",
            (PyCFunction) Simulator_get_num_segment_blocks, METH_NOARGS,
            "Returns the number of segment memory blocks"},
    {"get_num_breakpoints", (PyCFunction) Simulator_get_num_breakpoints,
            METH_NOARGS, "Returns the number of recombination breakpoints" },
    {"get_num_nodes",
            (PyCFunction) Simulator_get_num_nodes,
            METH_NOARGS, "Returns the number of coalescence records" },
    {"get_num_edges",
            (PyCFunction) Simulator_get_num_edges,
            METH_NOARGS, "Returns the number of coalescence records" },
    {"get_num_migrations",
            (PyCFunction) Simulator_get_num_migrations,
            METH_NOARGS, "Returns the number of migration records" },
    {"get_ancestors", (PyCFunction) Simulator_get_ancestors, METH_NOARGS,
            "Returns the ancestors" },
    {"get_breakpoints", (PyCFunction) Simulator_get_breakpoints,
            METH_NOARGS, "Returns the list of breakpoints." },
    {"get_migration_matrix", (PyCFunction) Simulator_get_migration_matrix,
            METH_NOARGS, "Returns the migration matrix." },
    {"get_nodes", (PyCFunction) Simulator_get_nodes,
            METH_NOARGS, "Returns the coalescence records." },
    {"get_edges", (PyCFunction) Simulator_get_edges,
            METH_NOARGS, "Returns the coalescence records." },
    {"get_migrations", (PyCFunction) Simulator_get_migrations,
            METH_NOARGS, "Returns the migration records." },
    {"get_population_configuration",
            (PyCFunction) Simulator_get_population_configuration, METH_NOARGS,
            "Returns the population configurations"},
    {"get_samples",
            (PyCFunction) Simulator_get_samples, METH_NOARGS,
            "Returns the samples"},
    {"run", (PyCFunction) Simulator_run, METH_VARARGS,
            "Simulates until at most the specified time. Returns True\
            if sample has coalesced and False otherwise." },
    {"reset", (PyCFunction) Simulator_reset, METH_NOARGS,
            "Resets the simulation so it's ready for another replicate."},
    {"finalise_tables", (PyCFunction) Simulator_finalise_tables, METH_NOARGS,
            "Finalises the tables so they ready for export."},
    {"run_event", (PyCFunction) Simulator_run_event, METH_NOARGS,
            "Simulates exactly one event. Returns True "
            "if sample has coalesced and False otherwise." },
    {"debug_demography", (PyCFunction) Simulator_debug_demography, METH_NOARGS,
            "Runs the state of the simulator forward for one demographic event."},
    {"compute_population_size",
            (PyCFunction) Simulator_compute_population_size, METH_VARARGS,
            "Computes the size of a population at a given time. Debug method."},
    {NULL}  /* Sentinel */
};


static PyTypeObject SimulatorType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "_msprime.Simulator",             /* tp_name */
    sizeof(Simulator),             /* tp_basicsize */
    0,                         /* tp_itemsize */
    (destructor)Simulator_dealloc, /* tp_dealloc */
    0,                         /* tp_print */
    0,                         /* tp_getattr */
    0,                         /* tp_setattr */
    0,                         /* tp_reserved */
    0,                         /* tp_repr */
    0,                         /* tp_as_number */
    0,                         /* tp_as_sequence */
    0,                         /* tp_as_mapping */
    0,                         /* tp_hash  */
    0,                         /* tp_call */
    0,                         /* tp_str */
    0,                         /* tp_getattro */
    0,                         /* tp_setattro */
    0,                         /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT,        /* tp_flags */
    "Simulator objects",           /* tp_doc */
    0,                     /* tp_traverse */
    0,                     /* tp_clear */
    0,                     /* tp_richcompare */
    0,                     /* tp_weaklistoffset */
    0,                     /* tp_iter */
    0,                     /* tp_iternext */
    Simulator_methods,             /* tp_methods */
    Simulator_members,             /* tp_members */
    0,                         /* tp_getset */
    0,                         /* tp_base */
    0,                         /* tp_dict */
    0,                         /* tp_descr_get */
    0,                         /* tp_descr_set */
    0,                         /* tp_dictoffset */
    (initproc)Simulator_init,      /* tp_init */
};

/*===================================================================
 * Module level functions
 *===================================================================
 */

static PyObject *
msprime_log_likelihood_arg(PyObject *self, PyObject *args, PyObject *kwds)
{
    PyObject *ret = NULL;
    int err;
    LightweightTableCollection *tables = NULL;
    double recombination_rate, Ne, ret_likelihood;
    static char *kwlist[] = {"tables", "Ne", "recombination_rate", NULL};
    tsk_treeseq_t ts;

    memset(&ts, 0, sizeof(ts));
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O!dd", kwlist,
            &LightweightTableCollectionType, &tables, &Ne, &recombination_rate)) {
        goto out;
    }

    if (recombination_rate < 0) {
        PyErr_SetString(PyExc_ValueError, "recombination_rate must be >= 0");
        goto out;
    }

    /* Note: this will be inefficient here if we're building indexes for large
     * tables. */
    err = tsk_treeseq_init(&ts, tables->tables, TSK_BUILD_INDEXES);
    if (err != 0) {
        handle_tskit_library_error(err);
        goto out;
    }

    err = msp_log_likelihood_arg(&ts, recombination_rate, Ne, &ret_likelihood);
    if (err != 0) {
        handle_library_error(err);
        goto out;
    }
    ret = Py_BuildValue("d", ret_likelihood);
out:
    tsk_treeseq_free(&ts);
    return ret;
}

static PyObject *
msprime_get_gsl_version(PyObject *self)
{
    return Py_BuildValue("ii", GSL_MAJOR_VERSION, GSL_MINOR_VERSION);
}

static PyObject *
msprime_restore_gsl_error_handler(PyObject *self)
{
    gsl_set_error_handler(old_gsl_error_handler);
    return Py_BuildValue("");
}

static PyObject *
msprime_unset_gsl_error_handler(PyObject *self)
{
    /* turn off GSL error handler so we don't abort on errors. Can be restored
     * by calling restore_gsl_error_handler() */
    old_gsl_error_handler = gsl_set_error_handler_off();
    return Py_BuildValue("");
}

static PyMethodDef msprime_methods[] = {
    {"log_likelihood_arg", (PyCFunction) msprime_log_likelihood_arg,
            METH_VARARGS|METH_KEYWORDS,
            "Computes the log-likelihood of an ARG." },
    {"get_gsl_version", (PyCFunction) msprime_get_gsl_version, METH_NOARGS,
            "Returns the version of GSL we are linking against." },
    {"restore_gsl_error_handler", (PyCFunction) msprime_restore_gsl_error_handler,
            METH_NOARGS, "Restores the GSL error handler to its value before module import." },
    {"unset_gsl_error_handler", (PyCFunction) msprime_unset_gsl_error_handler,
            METH_NOARGS, "Unsets the GSL error handler (and stores the current value)." },
    {NULL}        /* Sentinel */
};

static struct PyModuleDef msprimemodule = {
    PyModuleDef_HEAD_INIT,
    "_msprime",
    "Low level interface for msprime",
    -1,
    msprime_methods,
    NULL, NULL, NULL, NULL
};

PyObject *
PyInit__msprime(void)
{
    PyObject *module = PyModule_Create(&msprimemodule);
    if (module == NULL) {
        return NULL;;
    }
    import_array();

    /* LightweightTableCollection type */
    LightweightTableCollectionType.tp_new = PyType_GenericNew;
    if (PyType_Ready(&LightweightTableCollectionType) < 0) {
        return NULL;;
    }
    Py_INCREF(&LightweightTableCollectionType);
    PyModule_AddObject(module, "LightweightTableCollection",
            (PyObject *) &LightweightTableCollectionType);

    /* RandomGenerator type */
    RandomGeneratorType.tp_new = PyType_GenericNew;
    if (PyType_Ready(&RandomGeneratorType) < 0) {
        return NULL;;
    }
    Py_INCREF(&RandomGeneratorType);
    PyModule_AddObject(module, "RandomGenerator", (PyObject *) &RandomGeneratorType);

    /* MutationGenerator type */
    MutationGeneratorType.tp_new = PyType_GenericNew;
    if (PyType_Ready(&MutationGeneratorType) < 0) {
        return NULL;;
    }
    Py_INCREF(&MutationGeneratorType);
    PyModule_AddObject(module, "MutationGenerator", (PyObject *) &MutationGeneratorType);

    /* Simulator type */
    SimulatorType.tp_new = PyType_GenericNew;
    if (PyType_Ready(&SimulatorType) < 0) {
        return NULL;;
    }
    Py_INCREF(&SimulatorType);
    PyModule_AddObject(module, "Simulator", (PyObject *) &SimulatorType);

    /* RecombinationMap type */
    RecombinationMapType.tp_new = PyType_GenericNew;
    if (PyType_Ready(&RecombinationMapType) < 0) {
        return NULL;;
    }
    Py_INCREF(&RecombinationMapType);
    PyModule_AddObject(module, "RecombinationMap", (PyObject *) &RecombinationMapType);

    /* IntervalMap type */
    IntervalMapType.tp_new = PyType_GenericNew;
    if (PyType_Ready(&IntervalMapType) < 0) {
        return NULL;;
    }
    Py_INCREF(&IntervalMapType);
    PyModule_AddObject(module, "IntervalMap", (PyObject *) &IntervalMapType);


    /* Errors and constants */
    MsprimeInputError = PyErr_NewException("_msprime.InputError", NULL, NULL);
    Py_INCREF(MsprimeInputError);
    PyModule_AddObject(module, "InputError", MsprimeInputError);
    MsprimeLibraryError = PyErr_NewException("_msprime.LibraryError", NULL, NULL);
    Py_INCREF(MsprimeLibraryError);
    PyModule_AddObject(module, "LibraryError", MsprimeLibraryError);

    PyModule_AddIntConstant(module, "NODE_IS_CA_EVENT", MSP_NODE_IS_CA_EVENT);
    PyModule_AddIntConstant(module, "NODE_IS_RE_EVENT", MSP_NODE_IS_RE_EVENT);
    PyModule_AddIntConstant(module, "NODE_IS_MIG_EVENT", MSP_NODE_IS_MIG_EVENT);
    PyModule_AddIntConstant(module, "NODE_IS_CEN_EVENT", MSP_NODE_IS_CEN_EVENT);

    /* The function unset_gsl_error_handler should be called at import time,
     * ensuring we capture the value of the handler. However, just in case
     * someone calls restore_gsl_error_handler before this is called, we
     * set it to null. */
    old_gsl_error_handler = NULL;

    return module;
}
