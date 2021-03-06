
/*----------------------------------------------------------------------+
 |                                                                      |
 |      floydmodule.c                                                   |
 |                                                                      |
 +----------------------------------------------------------------------*/

/*
 *  Copyright (C) 2015-2016, Marcel van Kervinck
 *  All rights reserved
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *  1. Redistributions of source code must retain the above copyright
 *  notice, this list of conditions and the following disclaimer.
 *
 *  2. Redistributions in binary form must reproduce the above copyright
 *  notice, this list of conditions and the following disclaimer in the
 *  documentation and/or other materials provided with the distribution.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 */

/*----------------------------------------------------------------------+
 |      Includes                                                        |
 +----------------------------------------------------------------------*/

// Python API (must come first)
#include "Python.h"

// C standard
#include <stdbool.h>
#include <stdint.h>

// C extension
#include "cplus.h"

// Other modules
#include "Board.h"
#include "Engine.h"
#include "uci.h"

/*----------------------------------------------------------------------+
 |      Module                                                          |
 +----------------------------------------------------------------------*/

// Module docstring
PyDoc_STRVAR(floyd_doc, "Chess engine study");

/*----------------------------------------------------------------------+
 |      evaluate(...)                                                   |
 +----------------------------------------------------------------------*/

PyDoc_STRVAR(evaluate_doc,
        "evaluate(fen) -> score\n"
);

static PyObject *
floydmodule_evaluate(PyObject *self, PyObject *args)
{
        unused(self);
        char *fen;

        if (!PyArg_ParseTuple(args, "s", &fen))
                return null;

        struct Board board;
        int len = setupBoard(&board, fen);
        if (len <= 0)
                return PyErr_Format(PyExc_ValueError, "Invalid FEN (%s)", fen);

        if (globalVectorChanged)
                resetEvaluate();
        int score = evaluate(&board);

        return PyFloat_FromDouble(score / 1000.0);
}

/*----------------------------------------------------------------------+
 |      setCoefficient(...)                                             |
 +----------------------------------------------------------------------*/

// TODO: create a proper object for the vector
// TODO: create coef name list

PyDoc_STRVAR(setCoefficient_doc,
        "setCoefficient(coef, newValue) -> oldValue, name\n"
        "\n"
        "!!! EXPERIMENTAL !!!\n"
);

static PyObject *
floydmodule_setCoefficient(PyObject *self, PyObject *args)
{
        unused(self);
        int coef, newValue;

        if (!PyArg_ParseTuple(args, "ii", &coef, &newValue))
                return null;

        if (coef < 0 || coef >= vectorLen)
                return PyErr_Format(PyExc_IndexError, "coef %d out of range", coef);

        long oldValue = globalVector[coef];
        globalVector[coef] = newValue;
        globalVectorChanged |= (newValue != oldValue); // invalidate evaluation caches

        PyObject *result = PyTuple_New(2);
        if (!result)
                return null;

        if (PyTuple_SetItem(result, 0, PyInt_FromLong(oldValue))) {
                Py_DECREF(result);
                return null;
        }

        if (PyTuple_SetItem(result, 1, PyString_FromString(vectorLabels[coef]))) {
                Py_DECREF(result);
                return null;
        }

        return result;
}

/*----------------------------------------------------------------------+
 |      search(...)                                                     |
 +----------------------------------------------------------------------*/

PyDoc_STRVAR(search_doc,
        "search(fen, depth=" quote2(maxDepth) ", movetime=0.0, info=None) -> score, move\n"
        "Valid options for `info' are:\n"
        "       None    : No info\n"
        "       'uci'   : Write UCI info lines to stdout\n"
//      "       'xboard': Write XBoard info lines to stdout\n"
);

static PyObject *
floydmodule_search(PyObject *self, PyObject *args, PyObject *keywords)
{
        unused(self);
        char *fen;
        int depth = maxDepth;
        double movetime = 0.0;
        char *info = null;

        static char *keywordList[] = { "fen", "depth", "movetime", "info", null };

        if (!PyArg_ParseTupleAndKeywords(args, keywords, "s|idz:search", keywordList,
                &fen, &depth, &movetime, &info))
                return null;

        struct Engine engine;
        initEngine(&engine);

        ttSetSize(&engine, (depth > 0) ? 4*1024*1024 : 0); // TODO: remove when we have a proper engine object

        int len = setupBoard(&engine.board, fen);
        if (len <= 0)
                return PyErr_Format(PyExc_ValueError, "Invalid FEN (%s)", fen);

        engine.board.eloDiff = atoi(fen + len);

        if (depth < 0 || depth > maxDepth)
                return PyErr_Format(PyExc_ValueError, "Invalid depth (%d)", depth);

        if (movetime < 0.0)
                return PyErr_Format(PyExc_ValueError, "Invalid movetime (%g)", movetime);

        searchInfo_fn *infoFunction = noInfoFunction;
        void *infoData = &engine;
        if (info != null) {
                if (!strcmp(info, "uci"))
                        infoFunction = uciSearchInfo;
                else
                        return PyErr_Format(PyExc_ValueError, "Invalid info type (%s)", info);
        }

        engine.target.depth = depth;
        engine.target.nodeCount = maxLongLong;
        engine.target.scores = (intPair) {{ -maxInt, maxInt }};;
        engine.target.time = 0.0;
        engine.target.maxTime = movetime;
        engine.pondering = false;
        engine.infoFunction = infoFunction;
        engine.infoData = infoData;

        if (globalVectorChanged)
                resetEvaluate();
        rootSearch(&engine);
        cleanupEngine(&engine);

        if (PyErr_Occurred())
                return null;

        PyObject *result = PyTuple_New(2);
        if (!result)
                return null;

        if (PyTuple_SetItem(result, 0, PyFloat_FromDouble(engine.score / 1000.0))) {
                Py_DECREF(result);
                return null;
        }

        PyObject *bestMove;
        if (engine.bestMove != 0) {
                char moveString[maxMoveSize];
                moveToUci(moveString, engine.bestMove);
                bestMove = PyString_FromString(moveString);
        } else {
                bestMove = Py_None;
                Py_INCREF(Py_None);
        }

        if (PyTuple_SetItem(result, 1, bestMove)) {
                Py_DECREF(bestMove);
                Py_DECREF(result);
                return null;
        }

        return result;
}

/*----------------------------------------------------------------------+
 |      Method table                                                    |
 +----------------------------------------------------------------------*/

static PyMethodDef floydMethods[] = {
        { "evaluate",       floydmodule_evaluate,            METH_VARARGS,               evaluate_doc },
        { "setCoefficient", floydmodule_setCoefficient,      METH_VARARGS,               setCoefficient_doc },
        { "search",         (PyCFunction)floydmodule_search, METH_VARARGS|METH_KEYWORDS, search_doc },
        { null, null, 0, null }
};

/*----------------------------------------------------------------------+
 |      initfloyd                                                       |
 +----------------------------------------------------------------------*/

PyMODINIT_FUNC
initfloyd(void)
{
        // Create the module and add the functions
        PyObject *module = Py_InitModule3("floyd", floydMethods, floyd_doc);
        if (!module)
                return;

        PyObject *versionString = PyString_FromString(quote2(floydVersion));
        if (versionString)
                PyModule_AddObject(module, "__version__", versionString);
}

/*----------------------------------------------------------------------+
 |                                                                      |
 +----------------------------------------------------------------------*/

