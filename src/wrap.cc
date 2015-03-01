#include <stdexcept>

#include <boost/variant/static_visitor.hpp>
#include <boost/variant/apply_visitor.hpp>
#include <boost/format.hpp>

#include "wrap.hh"

#include <frameobject.h>

// Python 3 support
#if PY_MAJOR_VERSION >= 3
# define PyInt_FromLong      PyLong_FromLong
# define PyInt_AsLong        PyLong_AsLong
# define PyInt_Check         PyLong_Check
# define PyString_FromString PyBytes_FromString
# define PyString_Check      PyBytes_Check
# define PyString_AsString   PyBytes_AsString
#endif //! PY_MAJOR_VERSION

namespace roboptim
{
  namespace core
  {
    namespace python
    {
      namespace
      {
        std::string toString (PyObject* obj)
        {
          // string
          if (PyString_Check (obj))
	    {
	      return PyString_AsString (obj);
	    }
          // unicode string
          else if (PyUnicode_Check (obj))
	    {
	      return PyString_AsString (PyUnicode_AsUTF8String (obj));
	    }
          // object
          else
	    {
	      PyObject* pyStr = PyObject_Str (obj);
	      std::string str = PyString_AsString (pyStr);
	      Py_DECREF (pyStr);
	      return str;
	    }
        }

        void checkPythonError ()
        {
          // Catch error (if any)
          // TODO: improve the trace
          PyObject *ptype, *pvalue, *ptraceback;
          PyErr_Fetch (&ptype, &pvalue, &ptraceback);
          if (ptype)
	    {
	      std::string strErrorMessage = toString (pvalue);
	      std::string strTraceback = "";

	      PyThreadState* tstate = PyThreadState_GET ();
	      if (tstate && tstate->frame)
		{
		  PyFrameObject* frame = tstate->frame;

		  strTraceback += "Python stack trace:\n";
		  while (frame)
		    {
		      int line = frame->f_lineno;
		      const char* filename = PyString_AsString (frame->f_code->co_filename);
		      const char* funcname = PyString_AsString (frame->f_code->co_name);
		      strTraceback += (boost::format ("    %1%(%2%): %3%\n")
				       % filename % line % funcname).str ();
		      frame = frame->f_back;
		    }
		}

	      throw std::runtime_error
		((boost::format ("Error occurred in Python code: %1%\n%2%")
		  % strErrorMessage % strTraceback).str ());
	    }
        }
      } // end of unnamed namespace

      Function::Function (size_type inputSize,
                          size_type outputSize,
                          const std::string& name)
        : roboptim::Function (inputSize, outputSize, name),
          computeCallback_ (0)
      {
      }

      Function::~Function ()
      {
        if (computeCallback_)
	  {
	    Py_DECREF (computeCallback_);
	    computeCallback_ = 0;
	  }
      }

      void Function::impl_compute (result_t& result, const argument_t& argument)
	const
      {
        if (!computeCallback_)
	  {
	    PyErr_SetString
	      (PyExc_TypeError,
	       "compute callback not set");
	    return;
	  }
        if (!PyFunction_Check (computeCallback_))
	  {
	    PyErr_SetString
	      (PyExc_TypeError,
	       "compute callback is not a function");
	    return;
	  }

        npy_intp inputSize = static_cast<npy_intp> (this->inputSize ());
        npy_intp outputSize = static_cast<npy_intp> (this->outputSize ());

        PyObject* resultNumpy =
          PyArray_SimpleNewFromData (1, &outputSize, NPY_DOUBLE, &result[0]);
        if (!resultNumpy)
	  {
	    PyErr_SetString (PyExc_TypeError, "cannot convert result");
	    return;
	  }

        PyObject* argNumpy =
          PyArray_SimpleNewFromData
          (1, &inputSize, NPY_DOUBLE, const_cast<double*> (&argument[0]));
        if (!argNumpy)
	  {
	    PyErr_SetString (PyExc_TypeError, "cannot convert argument");
	    return;
	  }

        PyObject* arglist = Py_BuildValue ("(OO)", resultNumpy, argNumpy);
        if (!arglist)
	  {
	    Py_DECREF (arglist);
	    PyErr_SetString
	      (PyExc_TypeError, "failed to build argument list");
	    return;
	  }

        PyObject* resultPy = PyEval_CallObject (computeCallback_, arglist);

        checkPythonError ();

        Py_DECREF (arglist);
        Py_XDECREF(resultPy);
      }

      void Function::setComputeCallback (PyObject* callback)
      {
        if (callback == computeCallback_)
          return;

        if (computeCallback_)
	  {
	    Py_DECREF (computeCallback_);
	    computeCallback_ = 0;
	  }

        Py_XINCREF (callback);
        computeCallback_ = callback;
      }

      PyObject* Function::getComputeCallback () const
      {
        return computeCallback_;
      }



      DifferentiableFunction::DifferentiableFunction (size_type inputSize,
                                                      size_type outputSize,
                                                      const std::string& name)
        : roboptim::DifferentiableFunction (inputSize, outputSize, name),
	  Function (inputSize, outputSize, name),
	  gradientCallback_ (0),
	  jacobianCallback_ (0)
      {
      }

      DifferentiableFunction::~DifferentiableFunction ()
      {
        if (gradientCallback_)
	  {
	    Py_DECREF (gradientCallback_);
	    gradientCallback_ = 0;
	  }
        if (jacobianCallback_)
	  {
	    Py_DECREF (jacobianCallback_);
	    jacobianCallback_ = 0;
	  }
      }

      DifferentiableFunction::size_type
      DifferentiableFunction::inputSize () const
      {
        return ::roboptim::DifferentiableFunction::inputSize ();
      }

      DifferentiableFunction::size_type
      DifferentiableFunction::outputSize () const
      {
        return ::roboptim::DifferentiableFunction::outputSize ();
      }

      void DifferentiableFunction::impl_compute
      (result_t& result, const argument_t& argument)
	const
      {
        ::roboptim::core::python::Function::impl_compute (result, argument);
      }

      void DifferentiableFunction::impl_gradient (gradient_t& gradient,
                                                  const argument_t& argument,
                                                  size_type functionId)
	const
      {
	if (!gradientCallback_)
          {
            PyErr_SetString
              (PyExc_TypeError,
               "gradient callback not set");
            return;
          }
	if (!PyFunction_Check (gradientCallback_))
          {
            PyErr_SetString
              (PyExc_TypeError,
               "gradient callback is not a function");
            return;
          }

	npy_intp inputSize = static_cast<npy_intp>
	  (::roboptim::core::python::Function::inputSize ());

	PyObject* gradientNumpy =
	  PyArray_SimpleNewFromData (1, &inputSize, NPY_DOUBLE, &gradient[0]);
	if (!gradientNumpy)
          {
            PyErr_SetString (PyExc_TypeError, "cannot convert result");
            return;
          }

	PyObject* argNumpy =
	  PyArray_SimpleNewFromData
	  (1, &inputSize, NPY_DOUBLE, const_cast<double*> (&argument[0]));
	if (!argNumpy)
          {
            PyErr_SetString (PyExc_TypeError, "cannot convert argument");
            return;
          }

	PyObject* arglist =
	  Py_BuildValue ("(OOi)", gradientNumpy, argNumpy, functionId);
	if (!arglist)
          {
            Py_DECREF (arglist);
            PyErr_SetString
              (PyExc_TypeError, "failed to build argument list");
            return;
          }

	PyObject* resultPy = PyEval_CallObject (gradientCallback_, arglist);

        checkPythonError ();

	Py_DECREF (arglist);
	Py_XDECREF(resultPy);
      }

      void DifferentiableFunction::impl_jacobian (jacobian_t& jacobian,
                                                  const argument_t& argument)
	const
      {
	// Jacobian callback not specified, we fallback on parent implementation
	if (!jacobianCallback_)
	  {
	    roboptim::DifferentiableFunction::impl_jacobian (jacobian, argument);
	    return;
	  }
	else // Use Jacobian callback defined in Python
	  {
	    if (!PyFunction_Check (jacobianCallback_))
	      {
		PyErr_SetString
		  (PyExc_TypeError,
		   "jacobian callback is not a function");
		return;
	      }

	    // TODO: check storage order (NumPy excepts C-style, but Eigen uses RowMajor as default)
	    npy_intp inputSize =
	      static_cast<npy_intp> (::roboptim::core::python::Function::inputSize ());
	    npy_intp outputSize =
	      static_cast<npy_intp> (::roboptim::core::python::Function::outputSize ());

	    npy_intp sizes[2] = {outputSize, inputSize};

	    PyObject* jacobianNumpy =
	      PyArray_SimpleNewFromData (2, &sizes[0], NPY_DOUBLE,
					 jacobian.data ());
	    if (!jacobianNumpy)
	      {
		PyErr_SetString (PyExc_TypeError, "cannot convert result");
		return;
	      }

	    PyObject* argNumpy =
	      PyArray_SimpleNewFromData
	      (1, &inputSize, NPY_DOUBLE, const_cast<double*> (&argument[0]));
	    if (!argNumpy)
	      {
		PyErr_SetString (PyExc_TypeError, "cannot convert argument");
		return;
	      }

	    PyObject* arglist =
	      Py_BuildValue ("(OO)", jacobianNumpy, argNumpy);
	    if (!arglist)
	      {
		Py_DECREF (arglist);
		PyErr_SetString
		  (PyExc_TypeError, "failed to build argument list");
		return;
	      }

	    PyObject* resultPy = PyEval_CallObject (jacobianCallback_, arglist);

	    checkPythonError ();

	    Py_DECREF (arglist);
	    Py_XDECREF(resultPy);
	  }
      }

      std::ostream& DifferentiableFunction::print (std::ostream& o) const
      {
        // Force the call to the proper print method
        return ::roboptim::DifferentiableFunction::print (o);
      }

      const std::string& DifferentiableFunction::getName () const
      {
        return ::roboptim::DifferentiableFunction::getName ();
      }

      void DifferentiableFunction::setGradientCallback (PyObject* callback)
      {
        if (gradientCallback_)
	  {
	    Py_DECREF (gradientCallback_);
	    gradientCallback_ = 0;
	  }

        Py_XINCREF (callback);
        gradientCallback_ = callback;
      }

      void DifferentiableFunction::setJacobianCallback (PyObject* callback)
      {
        if (jacobianCallback_)
	  {
	    Py_DECREF (jacobianCallback_);
	    jacobianCallback_ = 0;
	  }

        Py_XINCREF (callback);
        jacobianCallback_ = callback;
      }


      TwiceDifferentiableFunction::TwiceDifferentiableFunction (size_type inputSize,
								size_type outputSize,
								const std::string& name)
        : ::roboptim::TwiceDifferentiableFunction (inputSize, outputSize, name),
	  ::roboptim::DifferentiableFunction
	  (inputSize, outputSize, name),
	  ::roboptim::core::python::DifferentiableFunction
	  (inputSize, outputSize, name),
	  hessianCallback_ (0)
      {
      }

      TwiceDifferentiableFunction::~TwiceDifferentiableFunction ()
      {
        if (hessianCallback_)
	  {
	    Py_DECREF (hessianCallback_);
	    hessianCallback_ = 0;
	  }
      }

      void TwiceDifferentiableFunction::impl_compute
      (result_t& result, const argument_t& argument)
	const
      {
        ::roboptim::core::python::Function::impl_compute (result, argument);
      }

      void TwiceDifferentiableFunction::impl_gradient
      (gradient_t& gradient, const argument_t& argument, size_type functionId)
	const
      {
        ::roboptim::core::python::DifferentiableFunction::impl_gradient
          (gradient, argument, functionId);
      }

      void TwiceDifferentiableFunction::impl_hessian (hessian_t& /*hessian*/,
						      const argument_t& /*argument*/,
						      size_type /*functionId*/) const
      {
        //FIXME: implement this.
      }


      FunctionPool::FunctionPool (const callback_ptr callback,
                                  const functionList_t& functions,
                                  const std::string& name)
        : ::roboptim::DifferentiableFunction (pool_t::listInputSize (functions),
					      pool_t::listOutputSize (functions),
					      name),
	  pyFunction_t (pool_t::listInputSize (functions),
			pool_t::listOutputSize (functions),
			name),
	  pool_ (callback, functions, name)
      {
      }


      FunctionPool::~FunctionPool ()
      {
      }

      void FunctionPool::impl_compute (result_t& result, const argument_t& x) const
      {
        pool_.impl_compute (result, x);
      }

      void FunctionPool::impl_gradient (gradient_t& gradient,
                                        const argument_t& x,
                                        size_type functionId) const
      {
        pool_.impl_gradient (gradient, x, functionId);
      }

      void FunctionPool::impl_jacobian (jacobian_t& jacobian,
                                        const argument_t& x) const
      {
        pool_.impl_jacobian (jacobian, x);
      }

      std::ostream& FunctionPool::print (std::ostream& o) const
      {
        return pool_.print (o);
      }
    } // end of namespace python
  } // end of namespace core
} // end of namespace roboptim

using roboptim::core::python::Function;
using roboptim::core::python::DifferentiableFunction;
using roboptim::core::python::TwiceDifferentiableFunction;
using roboptim::core::python::FiniteDifferenceGradient;
using roboptim::core::python::FunctionPool;

namespace detail
{
  // See: http://www.boost.org/doc/libs/1_55_0/libs/smart_ptr/sp_techniques.html#static
  struct null_deleter
  {
    void operator () (void const *) const
    {
    }
  };

  struct pyobject_deleter
  {
  public:

    pyobject_deleter (PyObject* p): p_ (p)
    {
    }

    void operator () (void const *)
    {
      Py_DECREF (p_);
    }

  private:
    PyObject* p_;
  };

  template <typename T>
  boost::shared_ptr<T> to_shared_ptr (T* o, PyObject* py_o)
  {
    Py_INCREF (py_o);
    return boost::shared_ptr<T> (o, pyobject_deleter (py_o));
  }

  template <>
  void destructor<problem_t> (PyObject* obj)
  {
    problem_t* ptr = static_cast<problem_t*>
      (PyCapsule_GetPointer
       (obj, ROBOPTIM_CORE_PROBLEM_CAPSULE_NAME));
    assert (ptr && "failed to retrieve pointer from capsule");
    if (ptr)
      delete ptr;
  }

  template <>
  void destructor<factory_t> (PyObject* obj)
  {
    factory_t* ptr = static_cast<factory_t*>
      (PyCapsule_GetPointer
       (obj, ROBOPTIM_CORE_SOLVER_CAPSULE_NAME));
    assert (ptr && "failed to retrieve pointer from capsule");
    if (ptr)
      delete ptr;
  }

  template <>
  void destructor<rcp::Multiplexer<solver_t> > (PyObject* obj)
  {
    rcp::Multiplexer<solver_t>* ptr = static_cast<rcp::Multiplexer<solver_t>*>
      (PyCapsule_GetPointer
       (obj, ROBOPTIM_CORE_CALLBACK_MULTIPLEXER_CAPSULE_NAME));
    assert (ptr && "failed to retrieve pointer from capsule");
    if (ptr)
      delete ptr;
  }

  template <>
  void destructor<solverState_t> (PyObject* obj)
  {
    solverState_t* ptr = static_cast<solverState_t*>
      (PyCapsule_GetPointer
       (obj, ROBOPTIM_CORE_SOLVER_STATE_CAPSULE_NAME));
    assert (ptr && "failed to retrieve pointer from capsule");
    if (ptr)
      delete ptr;
  }

  template <>
  void destructor<logger_t> (PyObject* obj)
  {
    logger_t* ptr = static_cast<logger_t*>
      (PyCapsule_GetPointer
       (obj, ROBOPTIM_CORE_OPTIMIZATION_LOGGER_CAPSULE_NAME));
    assert (ptr && "failed to retrieve pointer from capsule");
    if (ptr)
      delete ptr;
  }

  template <>
  void destructor<result_t> (PyObject* obj)
  {
    result_t* ptr = static_cast<result_t*>
      (PyCapsule_GetPointer
       (obj, ROBOPTIM_CORE_RESULT_CAPSULE_NAME));
    assert (ptr && "failed to retrieve pointer from capsule");
    if (ptr)
      delete ptr;
  }

  template <>
  void destructor<resultWithWarnings_t> (PyObject* obj)
  {
    resultWithWarnings_t* ptr = static_cast<resultWithWarnings_t*>
      (PyCapsule_GetPointer
       (obj, ROBOPTIM_CORE_RESULT_WITH_WARNINGS_CAPSULE_NAME));
    assert (ptr && "failed to retrieve pointer from capsule");
    if (ptr)
      delete ptr;
  }

  template <>
  void destructor<solverError_t> (PyObject* obj)
  {
    solverError_t* ptr = static_cast<solverError_t*>
      (PyCapsule_GetPointer
       (obj, ROBOPTIM_CORE_SOLVER_ERROR_CAPSULE_NAME));
    assert (ptr && "failed to retrieve pointer from capsule");
    if (ptr)
      delete ptr;
  }

  template <typename T>
  void destructor (PyObject* obj)
  {
    T* ptr = static_cast<T*>
      (PyCapsule_GetPointer
       (obj, ROBOPTIM_CORE_FUNCTION_CAPSULE_NAME));
    assert (ptr && "failed to retrieve pointer from capsule");
    if (ptr)
      delete ptr;
  }

  int
  functionConverter (PyObject* obj, Function** address)
  {
    assert (address);

    if (!PyCapsule_CheckExact (obj))
      {
	PyErr_SetString (PyExc_TypeError, "Invalid Python Function given.");
	return 0;
      }

    Function* ptr = static_cast<Function*>
      (PyCapsule_GetPointer
       (obj, ROBOPTIM_CORE_FUNCTION_CAPSULE_NAME));

    if (!ptr)
      {
	PyErr_SetString
	  (PyExc_TypeError,
	   "Function object expected but another type was passed");
	return 0;
      }

    *address = ptr;

    return 1;
  }

  int
  functionListConverter (PyObject* obj, FunctionPool::functionList_t** v)
  {
    assert (v);

    if (!PyList_CheckExact (obj))
      {
	PyErr_SetString (PyExc_TypeError, "Invalid Function list given.");
	return 0;
      }

    // For each function
    for (int i = 0; i < PyList_Size (obj); ++i)
      {
	PyObject* fPy = PyList_GetItem (obj, i);

	Function* f = static_cast<Function*>
	  (PyCapsule_GetPointer
	   (fPy, ROBOPTIM_CORE_FUNCTION_CAPSULE_NAME));

	if (!f)
	  {
	    PyErr_SetString
	      (PyExc_TypeError,
	       "Function object expected but another type was passed");
	    return 0;
	  }

	DifferentiableFunction* df = dynamic_cast<DifferentiableFunction*> (f);

	if (!df)
	  {
	    PyErr_SetString
	      (PyExc_TypeError,
	       "DifferentiableFunction object expected but another type was passed");
	    return 0;
	  }

	// Convert ptr to shared_ptr
	(*v)->push_back (detail::to_shared_ptr<DifferentiableFunction> (df, fPy));
      }

    return 1;
  }

  int
  problemConverter (PyObject* obj, problem_t** address)
  {
    assert (address);
    problem_t* ptr = static_cast<problem_t*>
      (PyCapsule_GetPointer
       (obj, ROBOPTIM_CORE_PROBLEM_CAPSULE_NAME));
    if (!ptr)
      {
	PyErr_SetString
	  (PyExc_TypeError,
	   "Problem object expected but another type was passed");
	return 0;
      }
    *address = ptr;
    return 1;
  }

  int
  factoryConverter (PyObject* obj, factory_t** address)
  {
    assert (address);
    factory_t* ptr = static_cast<factory_t*>
      (PyCapsule_GetPointer
       (obj, ROBOPTIM_CORE_SOLVER_CAPSULE_NAME));
    if (!ptr)
      {
	PyErr_SetString
	  (PyExc_TypeError,
	   "Problem object expected but another type was passed");
	return 0;
      }
    *address = ptr;
    return 1;
  }

  int
  solverCallbackConverter (PyObject* obj, rcp::SolverCallback<solver_t>** address)
  {
    assert (address);
    rcp::SolverCallback<solver_t>* ptr = static_cast<rcp::SolverCallback<solver_t>*>
      (PyCapsule_GetPointer
       (obj, ROBOPTIM_CORE_SOLVER_CALLBACK_CAPSULE_NAME));
    if (!ptr)
      {
	PyErr_SetString
	  (PyExc_TypeError,
	   "Solver callback object expected but another type was passed");
	return 0;
      }
    *address = ptr;
    return 1;
  }

  int
  multiplexerConverter (PyObject* obj, rcp::Multiplexer<solver_t>** address)
  {
    assert (address);
    rcp::Multiplexer<solver_t>* ptr = static_cast<rcp::Multiplexer<solver_t>*>
      (PyCapsule_GetPointer
       (obj, ROBOPTIM_CORE_CALLBACK_MULTIPLEXER_CAPSULE_NAME));
    if (!ptr)
      {
	PyErr_SetString
	  (PyExc_TypeError,
	   "Callback multiplexer object expected but another type was passed");
	return 0;
      }
    *address = ptr;
    return 1;
  }

  int
  solverStateConverter (PyObject* obj, solverState_t** address)
  {
    assert (address);

    solverState_t* ptr = static_cast<solverState_t*>
      (PyCapsule_GetPointer
       (obj, ROBOPTIM_CORE_SOLVER_STATE_CAPSULE_NAME));

    if (!ptr)
      {
	PyErr_SetString
	  (PyExc_TypeError,
	   "SolverState object expected but another type was passed");
	return 0;
      }
    *address = ptr;
    return 1;
  }

  int
  resultConverter (PyObject* obj, result_t** address)
  {
    assert (address);

    const char* capsule_name = PyCapsule_GetName (obj);

    result_t* ptr = 0;

    if (std::strcmp (capsule_name,
                     ROBOPTIM_CORE_RESULT_CAPSULE_NAME) == 0)
      {
	ptr = static_cast<result_t*>
	  (PyCapsule_GetPointer
	   (obj, ROBOPTIM_CORE_RESULT_CAPSULE_NAME));
      }
    // Try upcasting
    // TODO: find a better way to handle that
    else if (std::strcmp (capsule_name,
                          ROBOPTIM_CORE_RESULT_WITH_WARNINGS_CAPSULE_NAME) == 0)
      {
	ptr = static_cast<result_t*>
	  (PyCapsule_GetPointer
	   (obj, ROBOPTIM_CORE_RESULT_WITH_WARNINGS_CAPSULE_NAME));
      }

    if (!ptr)
      {
	PyErr_SetString
	  (PyExc_TypeError,
	   "Result object expected but another type was passed");
	return 0;
      }

    *address = ptr;
    return 1;
  }

  int
  resultWithWarningsConverter (PyObject* obj, resultWithWarnings_t** address)
  {
    assert (address);

    resultWithWarnings_t* ptr = static_cast<resultWithWarnings_t*>
      (PyCapsule_GetPointer
       (obj, ROBOPTIM_CORE_RESULT_WITH_WARNINGS_CAPSULE_NAME));

    if (!ptr)
      {
	PyErr_SetString
	  (PyExc_TypeError,
	   "ResultWithWarnings object expected but another type was passed");
	return 0;
      }
    *address = ptr;
    return 1;
  }

  int
  solverErrorConverter (PyObject* obj, solverError_t** address)
  {
    assert (address);
    solverError_t* ptr = static_cast<solverError_t*>
      (PyCapsule_GetPointer
       (obj, ROBOPTIM_CORE_SOLVER_ERROR_CAPSULE_NAME));
    if (!ptr)
      {
	PyErr_SetString
	  (PyExc_TypeError,
	   "SolverError object expected but another type was passed");
	return 0;
      }
    *address = ptr;
    return 1;
  }

  struct ParameterValueVisitor : public boost::static_visitor<PyObject*>
  {
    PyObject* operator () (const roboptim::Function::value_type& p) const
    {
      return PyFloat_FromDouble (p);
    }

    PyObject* operator () (const int& p) const
    {
      return PyInt_FromLong (p);
    }

    PyObject* operator () (const std::string& p) const
    {
      return PyString_FromString (p.c_str ());
    }
  };

  parameter_t::parameterValues_t toParameterValue (PyObject* obj)
  {
    // Value can be: double, int, or std::string

    // String
    if (PyString_Check (obj))
      {
	return PyString_AsString (obj);
      }
    // Unicode string
    else if (PyUnicode_Check (obj))
      {
        PyObject* u = PyUnicode_AsUTF8String (obj);
        parameter_t::parameterValues_t str = PyString_AsString (u);
        Py_DECREF (u);
        return str;
      }
    // Integer
    else if (PyInt_Check (obj))
      {
        return static_cast<int> (PyInt_AsLong (obj));
      }
    // Double
    else if (PyFloat_Check (obj))
      {
        return PyFloat_AsDouble (obj);
      }

    PyErr_SetString
      (PyExc_TypeError,
       "invalid parameter value (should be double, int or string).");

    return 0;
  }

  struct StateParameterValueVisitor : public boost::static_visitor<PyObject*>
  {
    // TODO: inherit from ParameterValueVisitor instead
    PyObject* operator () (const roboptim::Function::value_type& p) const
    {
      return PyFloat_FromDouble (p);
    }

    PyObject* operator () (const int& p) const
    {
      return PyInt_FromLong (p);
    }

    PyObject* operator () (const std::string& p) const
    {
      return PyString_FromString (p.c_str ());
    }

    PyObject* operator () (bool b) const
    {
      return PyBool_FromLong (b);
    }

    PyObject* operator () (const roboptim::Function::vector_t& v) const
    {
      npy_intp n = static_cast<npy_intp> (v.size ());

      return PyArray_SimpleNewFromData
        (1, &n, NPY_DOUBLE, const_cast<double*> (&v[0]));
    }
  };

  stateParameter_t::stateParameterValues_t toStateParameterValue (PyObject* obj)
  {
    // Value can be: double, int, std::string, bool or NumPy vector.
    // Bool
    if (PyBool_Check (obj))
      {
        return bool (PyObject_RichCompareBool (obj, Py_True, Py_EQ) == 1);
      }
    // NumPy vector
    else if (PyArray_Check (obj))
      {
	// Directly map Eigen vector to the numpy array data.
	Eigen::Map<Function::vector_t> vecEigen
	  (static_cast<double*>
	   (PyArray_DATA (obj)), PyArray_ITEMSIZE (obj));
	return vecEigen;
      }
    // String, unicode string, integer, double
    else if (PyString_Check (obj) || PyUnicode_Check (obj) ||
	     PyInt_Check (obj) || PyFloat_Check (obj))
      {
	return toParameterValue (obj);
      }

    PyErr_SetString
      (PyExc_TypeError,
       "invalid parameter value (should be double, int, string,"
       "bool or NumPy vector).");

    return 0;
  }
} // end of namespace detail.

template <typename T>
static PyObject*
createFunction (PyObject*, PyObject* args)
{
  Function::size_type inSize = 0;
  Function::size_type outSize = 0;
  const char* name = 0;

  if (!PyArg_ParseTuple(args, "iis", &inSize, &outSize, &name))
    return 0;

  std::string name_ = (name) ? name : "";
  T* function = new T (inSize, outSize, name_);

  PyObject* functionPy =
    PyCapsule_New (function, ROBOPTIM_CORE_FUNCTION_CAPSULE_NAME,
		   &detail::destructor<T>);
  return functionPy;
}


template <>
PyObject*
createFunction<FunctionPool> (PyObject*, PyObject* args)
{
  const char* name = 0;
  FunctionPool::callback_t* callback = 0;
  FunctionPool::functionList_t* functions = new FunctionPool::functionList_t ();

  if (!PyArg_ParseTuple(args, "O&O&s",
                        &detail::functionConverter, &callback,
                        &detail::functionListConverter, &functions,
                        &name))
    return 0;

  std::string name_ = (name) ? name : "";
  FunctionPool::callback_ptr p_callback
    = detail::to_shared_ptr<FunctionPool::callback_t>
    (callback, PyTuple_GetItem (args, 0));
  FunctionPool* pool = new FunctionPool (p_callback, *functions, name_);
  delete functions;

  PyObject* poolPy =
    PyCapsule_New (pool, ROBOPTIM_CORE_FUNCTION_CAPSULE_NAME,
		   &detail::destructor<FunctionPool>);

  return poolPy;
}

template <typename T>
static PyObject*
createFDWrapper (PyObject*, PyObject* args)
{
  Function* function = 0;
  double eps = ::roboptim::finiteDifferenceEpsilon;
  if (!PyArg_ParseTuple(args, "O&|d", &detail::functionConverter, &function, &eps))
    return 0;
  if (!function)
    {
      PyErr_SetString
	(PyExc_TypeError,
	 "argument 1 should be a function object");
      return 0;
    }

  T* fdFunction = new T (*function, eps);

  PyObject* fdFunctionPy =
    PyCapsule_New (fdFunction, ROBOPTIM_CORE_FUNCTION_CAPSULE_NAME,
		   &detail::destructor<T>);
  return fdFunctionPy;
}

static PyObject*
inputSize (PyObject*, PyObject* args)
{
  Function* function = 0;
  if (!PyArg_ParseTuple(args, "O&", &detail::functionConverter, &function))
    return 0;
  if (!function)
    {
      PyErr_SetString
	(PyExc_TypeError,
	 "argument 1 should be a function object");
      return 0;
    }
  return Py_BuildValue("i", function->inputSize ());
}

static PyObject*
outputSize (PyObject*, PyObject* args)
{
  Function* function = 0;
  if (!PyArg_ParseTuple(args, "O&", &detail::functionConverter, &function))
    return 0;
  if (!function)
    {
      PyErr_SetString
	(PyExc_TypeError,
	 "argument 1 should be a function object");
      return 0;
    }
  return Py_BuildValue("i", function->outputSize ());
}

static PyObject*
getName (PyObject*, PyObject* args)
{
  Function* function = 0;

  if (!PyArg_ParseTuple(args, "O&", &detail::functionConverter, &function))
    return 0;

  if (!function)
    {
      PyErr_SetString
	(PyExc_TypeError,
	 "argument 1 should be a function object");
      return 0;
    }

  return Py_BuildValue("s", function->getName ().c_str ());
}

static PyObject*
createProblem (PyObject*, PyObject* args)
{
  Function* costFunction = 0;
  if (!PyArg_ParseTuple(args, "O&", &detail::functionConverter, &costFunction))
    return 0;

  DifferentiableFunction* dfunction =
    dynamic_cast<DifferentiableFunction*> (costFunction);

  if (!dfunction)
    {
      PyErr_SetString
	(PyExc_TypeError,
	 "argument 1 should be a differentiable function object");
      return 0;
    }

  problem_t* problem = new problem_t (*dfunction);
  PyObject* problemPy =
    PyCapsule_New (problem, ROBOPTIM_CORE_PROBLEM_CAPSULE_NAME,
		   &detail::destructor<problem_t>);

  return problemPy;
}

static PyObject*
createSolver (PyObject*, PyObject* args)
{
  char* pluginName = 0;
  problem_t* problem = 0;
  if (!PyArg_ParseTuple (args, "sO&",
			 &pluginName,
			 &detail::problemConverter, &problem))
    return 0;

  factory_t* factory = 0;

  try
    {
      factory = new factory_t (pluginName, *problem);
    }
  catch (...)
    {
      delete factory;
      Py_INCREF (Py_None);
      return Py_None;
    }

  PyObject* solverPy =
    PyCapsule_New (factory, ROBOPTIM_CORE_SOLVER_CAPSULE_NAME,
		   &detail::destructor<factory_t>);
  return solverPy;
}

static PyObject*
createMultiplexer (PyObject*, PyObject* args)
{
  factory_t* factory = 0;
  if (!PyArg_ParseTuple (args, "O&",
			 &detail::factoryConverter, &factory))
    return 0;

  ::roboptim::core::python::Multiplexer<solver_t>* multiplexer = 0;

  try
    {
      boost::shared_ptr<factory_t> factory_ptr
        = detail::to_shared_ptr<factory_t> (factory, PyTuple_GetItem (args, 0));
      assert (factory_ptr);
      multiplexer = new ::roboptim::core::python::Multiplexer<solver_t> (factory_ptr);
    }
  catch (...)
    {
      delete multiplexer;
      Py_INCREF (Py_None);
      return Py_None;
    }

  PyObject* multiplexerPy =
    PyCapsule_New (multiplexer, ROBOPTIM_CORE_CALLBACK_MULTIPLEXER_CAPSULE_NAME,
		   &detail::destructor< ::roboptim::core::python::Multiplexer<solver_t> >);
  return multiplexerPy;
}

template <typename S>
static PyObject*
createSolverCallback (PyObject*, PyObject* args)
{
  typedef ::roboptim::core::python::SolverCallback<S> solverCallback_t;

  PyObject* problem = 0;
  if (!PyArg_ParseTuple (args, "O", &problem))
    return 0;

  solverCallback_t* callback = 0;

  try
    {
      callback = new solverCallback_t (problem);
    }
  catch (...)
    {
      delete callback;
      Py_INCREF (Py_None);
      return Py_None;
    }

  PyObject* callbackPy =
    PyCapsule_New (callback, ROBOPTIM_CORE_SOLVER_CALLBACK_CAPSULE_NAME,
		   &detail::destructor<solverCallback_t>);
  return callbackPy;
}

static PyObject*
compute (PyObject*, PyObject* args)
{
  Function* function = 0;
  PyObject* x = 0;
  PyObject* result = 0;
  if (!PyArg_ParseTuple
      (args, "O&OO",
       detail::functionConverter, &function, &result, &x))
    return 0;
  if (!function)
    {
      PyErr_SetString
	(PyExc_TypeError,
	 "Failed to retrieve function object");
      return 0;
    }

  PyObject* resultNumpy =
    PyArray_FROM_OTF(result, NPY_DOUBLE, NPY_OUT_ARRAY & NPY_C_CONTIGUOUS);

  // Try to build an array type from x.
  // All types providing a sequence interface are compatible.
  // Tuples, sequences and Numpy types for instance.
  PyObject* xNumpy =
    PyArray_FROM_OTF(x, NPY_DOUBLE, NPY_IN_ARRAY & NPY_C_CONTIGUOUS);
  if (!xNumpy)
    {
      PyErr_SetString
	(PyExc_TypeError,
	 "Argument cannot be converted to NumPy object");
      return 0;
    }

  // Directly map Eigen vector over Numpy x.
  Eigen::Map<Function::argument_t> xEigen
    (static_cast<double*> (PyArray_DATA (xNumpy)), function->inputSize ());

  // Directly map Eigen result to the numpy array data.
  Eigen::Map<Function::result_t> resultEigen
    (static_cast<double*>
     (PyArray_DATA (resultNumpy)), function->outputSize ());

  resultEigen = (*function) (xEigen);
  if (PyErr_Occurred ())
    return 0;

  Py_INCREF(Py_None);
  return Py_None;
}

static PyObject*
gradient (PyObject*, PyObject* args)
{
  Function* function = 0;
  PyObject* x = 0;
  PyObject* gradient = 0;
  Function::size_type functionId = 0;
  if (!PyArg_ParseTuple
      (args, "O&OOi",
       detail::functionConverter, &function, &gradient, &x, &functionId))
    return 0;
  if (!function)
    {
      PyErr_SetString
	(PyExc_TypeError,
	 "Failed to retrieve function object");
      return 0;
    }

  DifferentiableFunction* dfunction =
    dynamic_cast<DifferentiableFunction*> (function);
  if (!dfunction)
    {
      PyErr_SetString
	(PyExc_TypeError,
	 "argument 1 should be a differentiable function object");
      return 0;
    }

  PyObject* gradientNumpy =
    PyArray_FROM_OTF(gradient, NPY_DOUBLE, NPY_OUT_ARRAY & NPY_C_CONTIGUOUS);

  // Try to build an array type from x.
  // All types providing a sequence interface are compatible.
  // Tuples, sequences and Numpy types for instance.
  PyObject* xNumpy =
    PyArray_FROM_OTF(x, NPY_DOUBLE, NPY_IN_ARRAY & NPY_C_CONTIGUOUS);
  if (!xNumpy)
    {
      PyErr_SetString
	(PyExc_TypeError,
	 "Argument cannot be converted to NumPy object");
      return 0;
    }

  // Directly map Eigen vector over NumPy x.
  Eigen::Map<Function::argument_t> xEigen
    (static_cast<double*> (PyArray_DATA (xNumpy)), function->inputSize ());

  // Directly map Eigen result to the numpy array data.
  Eigen::Map<Function::result_t> gradientEigen
    (static_cast<double*>
     (PyArray_DATA (gradientNumpy)), dfunction->gradientSize ());

  gradientEigen = dfunction->gradient (xEigen, functionId);
  if (PyErr_Occurred ())
    return 0;

  Py_INCREF(Py_None);
  return Py_None;
}


static PyObject*
jacobian (PyObject*, PyObject* args)
{
  Function* function = 0;
  PyObject* x = 0;
  PyObject* jacobian = 0;

  if (!PyArg_ParseTuple
      (args, "O&OO",
       detail::functionConverter, &function, &jacobian, &x))
    return 0;
  if (!function)
    {
      PyErr_SetString
	(PyExc_TypeError,
	 "Failed to retrieve function object");
      return 0;
    }

  DifferentiableFunction* dfunction =
    dynamic_cast<DifferentiableFunction*> (function);
  if (!dfunction)
    {
      PyErr_SetString
	(PyExc_TypeError,
	 "argument 1 should be a differentiable function object");
      return 0;
    }

  PyObject* jacobianNumpy =
    PyArray_FROM_OTF(jacobian, NPY_DOUBLE, NPY_OUT_ARRAY & NPY_C_CONTIGUOUS);

  // Try to build an array type from x.
  // All types providing a sequence interface are compatible.
  // Tuples, sequences and Numpy types for instance.
  PyObject* xNumpy =
    PyArray_FROM_OTF(x, NPY_DOUBLE, NPY_IN_ARRAY & NPY_C_CONTIGUOUS);
  if (!xNumpy)
    {
      PyErr_SetString
	(PyExc_TypeError,
	 "Argument cannot be converted to NumPy object");
      return 0;
    }

  // Directly map Eigen vector over NumPy x.
  Eigen::Map<Function::argument_t> xEigen
    (static_cast<double*> (PyArray_DATA (xNumpy)), function->inputSize ());

  // Directly map Eigen Jacobian to the numpy array data.
  Eigen::Map<DifferentiableFunction::jacobian_t> jacobianEigen
    (static_cast<double*> (PyArray_DATA (jacobianNumpy)),
     dfunction->jacobianSize ().first, dfunction->jacobianSize ().second);

  // Warning: this works as long as RobOptim uses row-major storage.
  jacobianEigen = dfunction->jacobian (xEigen);
  if (PyErr_Occurred ())
    return 0;

  Py_INCREF(Py_None);
  return Py_None;
}


static PyObject*
bindCompute (PyObject*, PyObject* args)
{
  Function* function = 0;
  PyObject* callback = 0;
  if (!PyArg_ParseTuple
      (args, "O&O:bindCompute",
       detail::functionConverter, &function, &callback))
    return 0;
  if (!function)
    {
      PyErr_SetString
	(PyExc_TypeError,
	 "Failed to retrieve function object");
      return 0;
    }
  if (!callback)
    {
      PyErr_SetString
	(PyExc_TypeError,
	 "Failed to retrieve callback object");
      return 0;
    }
  if (!PyCallable_Check (callback))
    {
      PyErr_SetString
	(PyExc_TypeError,
	 "2nd argument must be callable");
      return 0;
    }

  function->setComputeCallback (callback);

  Py_INCREF(Py_None);
  return Py_None;
}

static PyObject*
bindGradient (PyObject*, PyObject* args)
{
  Function* function = 0;
  PyObject* callback = 0;
  if (!PyArg_ParseTuple
      (args, "O&O:bindGradient",
       detail::functionConverter, &function, &callback))
    return 0;
  if (!function)
    {
      PyErr_SetString
	(PyExc_TypeError,
	 "Failed to retrieve function object");
      return 0;
    }

  DifferentiableFunction* dfunction
    = dynamic_cast<DifferentiableFunction*> (function);

  if (!dfunction)
    {
      PyErr_SetString
	(PyExc_TypeError,
	 "instance of DifferentiableFunction expected as first argument");
      return 0;
    }
  if (!callback)
    {
      PyErr_SetString (PyExc_TypeError, "Failed to retrieve callback object");
      return 0;
    }
  if (!PyCallable_Check (callback))
    {
      PyErr_SetString (PyExc_TypeError, "2nd argument must be callable");
      return 0;
    }

  dfunction->setGradientCallback (callback);

  Py_INCREF(Py_None);
  return Py_None;
}

static PyObject*
bindJacobian (PyObject*, PyObject* args)
{
  Function* function = 0;
  PyObject* callback = 0;
  if (!PyArg_ParseTuple
      (args, "O&O:bindJacobian",
       detail::functionConverter, &function, &callback))
    return 0;
  if (!function)
    {
      PyErr_SetString
	(PyExc_TypeError,
	 "Failed to retrieve function object");
      return 0;
    }

  DifferentiableFunction* dfunction
    = dynamic_cast<DifferentiableFunction*> (function);

  if (!dfunction)
    {
      PyErr_SetString
	(PyExc_TypeError,
	 "instance of DifferentiableFunction expected as first argument");
      return 0;
    }
  if (!callback)
    {
      PyErr_SetString (PyExc_TypeError, "Failed to retrieve callback object");
      return 0;
    }
  if (!PyCallable_Check (callback))
    {
      PyErr_SetString (PyExc_TypeError, "2nd argument must be callable");
      return 0;
    }

  dfunction->setJacobianCallback (callback);

  Py_INCREF(Py_None);
  return Py_None;
}

static PyObject*
getStartingPoint (PyObject*, PyObject* args)
{
  problem_t* problem = 0;
  if (!PyArg_ParseTuple (args, "O&", &detail::problemConverter, &problem))
    return 0;
  if (!problem)
    {
      PyErr_SetString (PyExc_TypeError, "1st argument must be a problem");
      return 0;
    }

  npy_intp inputSize =
    static_cast<npy_intp> (problem->function ().inputSize ());

  PyObject* startingPoint = PyArray_SimpleNew(1, &inputSize, NPY_DOUBLE);
  Eigen::Map<Function::vector_t> startingPointEigen
    (static_cast<double*>
     (PyArray_DATA (startingPoint)), problem->function ().inputSize ());

  if (!problem->startingPoint ())
    {
      Py_INCREF (Py_None);
      return Py_None;
    }

  startingPointEigen = *(problem->startingPoint ());
  return startingPoint;
}

static PyObject*
setStartingPoint (PyObject*, PyObject* args)
{
  problem_t* problem = 0;
  PyObject* startingPoint = 0;
  if (!PyArg_ParseTuple
      (args, "O&O", &detail::problemConverter, &problem, &startingPoint))
    return 0;
  if (!problem)
    {
      PyErr_SetString (PyExc_TypeError, "1st argument must be a problem");
      return 0;
    }
  PyObject* startingPointNumpy =
    PyArray_FROM_OTF
    (startingPoint, NPY_DOUBLE, NPY_IN_ARRAY & NPY_C_CONTIGUOUS);
  if (!startingPointNumpy)
    {
      PyErr_SetString (PyExc_TypeError,
		       "failed to build numpy array from 2st argument");
      return 0;
    }

  if (PyArray_DIM (startingPointNumpy, 0) != problem->function ().inputSize ())
    {
      PyErr_SetString (PyExc_TypeError, "invalid size");
      return 0;
    }

  Eigen::Map<Function::argument_t> startingPointEigen
    (static_cast<double*> (PyArray_DATA (startingPointNumpy)),
     problem->function ().inputSize ());

  problem->startingPoint () = startingPointEigen;

  Py_INCREF (Py_None);
  return Py_None;
}

static PyObject*
getArgumentBounds (PyObject*, PyObject* args)
{
  problem_t* problem = 0;
  if (!PyArg_ParseTuple (args, "O&", &detail::problemConverter, &problem))
    return 0;
  if (!problem)
    {
      PyErr_SetString (PyExc_TypeError, "1st argument must be a problem");
      return 0;
    }

  npy_intp sdims[2] =
    {
      static_cast<npy_intp> (problem->function ().inputSize ()),
      2
    };

  PyObject* bounds = PyArray_SimpleNew (2, sdims, NPY_DOUBLE);

  for (unsigned i = 0; i < problem->function ().inputSize (); ++i)
    {
      *static_cast<double*> (PyArray_GETPTR2 (bounds, i, 0)) =
	problem->argumentBounds ()[i].first;
      *static_cast<double*> (PyArray_GETPTR2 (bounds, i, 1)) =
	problem->argumentBounds ()[i].second;
    }

  return bounds;
}

static PyObject*
setArgumentBounds (PyObject*, PyObject* args)
{
  problem_t* problem = 0;
  PyObject* bounds = 0;
  if (!PyArg_ParseTuple
      (args, "O&O", &detail::problemConverter, &problem, &bounds))
    return 0;
  if (!problem)
    {
      PyErr_SetString (PyExc_TypeError, "1st argument must be a problem");
      return 0;
    }
  PyObject* boundsNumpy =
    PyArray_FROM_OTF
    (bounds, NPY_DOUBLE, NPY_IN_ARRAY & NPY_C_CONTIGUOUS);
  if (!boundsNumpy)
    {
      PyErr_SetString (PyExc_TypeError,
		       "failed to build numpy array from 2st argument");
      return 0;
    }

  if (PyArray_DIM (bounds, 0) != problem->function ().inputSize ())
    {
      PyErr_SetString (PyExc_TypeError, "invalid size");
      return 0;
    }

  if (PyArray_DIM (bounds, 1) != 2)
    {
      PyErr_SetString (PyExc_TypeError, "invalid size");
      return 0;
    }

  Eigen::Map<Function::argument_t> boundsEigen
    (static_cast<double*> (PyArray_DATA (boundsNumpy)),
     problem->function ().inputSize ());

  for (unsigned i = 0; i < problem->function ().inputSize (); ++i)
    {
      problem->argumentBounds ()[i].first =
	*static_cast<double*> (PyArray_GETPTR2 (bounds, i, 0));
      problem->argumentBounds ()[i].second =
	*static_cast<double*> (PyArray_GETPTR2 (bounds, i, 1));
    }

  Py_INCREF (Py_None);
  return Py_None;
}

static PyObject*
getArgumentScales (PyObject*, PyObject* args)
{
  problem_t* problem = 0;
  if (!PyArg_ParseTuple (args, "O&", &detail::problemConverter, &problem))
    return 0;
  if (!problem)
    {
      PyErr_SetString (PyExc_TypeError, "1st argument must be a problem");
      return 0;
    }

  npy_intp inputSize =
    static_cast<npy_intp> (problem->function ().inputSize ());
  PyObject* scalesNumpy = PyArray_SimpleNew (1, &inputSize, NPY_DOUBLE);

  Eigen::Map<Function::vector_t> scalesEigen
    (static_cast<double*> (PyArray_DATA (scalesNumpy)),
     problem->function ().inputSize ());

  for (Function::size_type i = 0; i < inputSize; ++i)
    scalesEigen[i] = problem->argumentScales ()[i];
  return scalesNumpy;
}

static PyObject*
setArgumentScales (PyObject*, PyObject* args)
{
  problem_t* problem = 0;
  PyObject* scales = 0;
  if (!PyArg_ParseTuple
      (args, "O&O", &detail::problemConverter, &problem, &scales))
    return 0;
  if (!problem)
    {
      PyErr_SetString (PyExc_TypeError, "1st argument must be a problem");
      return 0;
    }
  PyObject* scalesNumpy =
    PyArray_FROM_OTF
    (scales, NPY_DOUBLE, NPY_IN_ARRAY & NPY_C_CONTIGUOUS);
  if (!scalesNumpy)
    {
      PyErr_SetString (PyExc_TypeError,
		       "failed to build numpy array from 2st argument");
      return 0;
    }

  if (PyArray_DIM (scalesNumpy, 0) != problem->function ().inputSize ())
    {
      PyErr_SetString (PyExc_TypeError, "invalid size");
      return 0;
    }

  Eigen::Map<Function::argument_t> scalesEigen
    (static_cast<double*> (PyArray_DATA (scalesNumpy)),
     problem->function ().inputSize ());

  for (size_t i = 0; i < static_cast<size_t> (problem->function ().inputSize ()); ++i)
    problem->argumentScales ()[i] = scalesEigen[i];


  Py_INCREF (Py_None);
  return Py_None;
}


static PyObject*
addConstraint (PyObject*, PyObject* args)
{
  problem_t* problem = 0;
  Function* function = 0;
  PyObject* py_bounds = 0;

  if (!PyArg_ParseTuple
      (args, "O&O&O",
       &detail::problemConverter, &problem,
       &detail::functionConverter, &function,
       &py_bounds))
    return 0;

  if (!problem)
    {
      PyErr_SetString (PyExc_TypeError, "1st argument must be a problem");
      return 0;
    }

  if (!function)
    {
      PyErr_SetString (PyExc_TypeError, "2nd argument must be a function");
      return 0;
    }

  DifferentiableFunction* dfunction =
    dynamic_cast<DifferentiableFunction*> (function);

  if (!dfunction)
    {
      PyErr_SetString (PyExc_TypeError,
		       "2nd argument must be a differentiable function");
      return 0;
    }

  if (!py_bounds)
    {
      PyErr_SetString (PyExc_TypeError, "3rd argument is invalid.");
      return 0;
    }

  bool is_pair = (PyList_Check (py_bounds) && (PyList_Size (py_bounds) == 2));
  bool is_np_array = (PyArray_Check (py_bounds) && (PyArray_NDIM (py_bounds) == 2));

  if (!is_pair && !is_np_array)
    {
      PyErr_SetString (PyExc_TypeError,
                       "3rd argument must be a (n x 2) NumPy array or a list of size 2.");
      return 0;
    }

  // If we just used a boost::shared_ptr, the constraint would be freed when the
  // problem disappears, so we use a custom deleter that keeps track of the
  // Python object's reference counter to prevent that.
  boost::shared_ptr<DifferentiableFunction> constraint
    = detail::to_shared_ptr<DifferentiableFunction>
    (dfunction, PyTuple_GetItem (args, 1));


  // Bounds = pair
  if (is_pair && constraint->outputSize () == 1)
    {
      PyObject* py_min = PyList_GetItem (py_bounds, 0);
      PyObject* py_max = PyList_GetItem (py_bounds, 1);

      if (!py_min || !py_max || !PyFloat_Check (py_min) || !PyFloat_Check (py_max))
	{
	  PyErr_SetString (PyExc_TypeError,
			   "bounds should be floats.");
	  return 0;
	}

      problem->addConstraint (constraint,
			      Function::makeInterval (PyFloat_AsDouble (py_min),
						      PyFloat_AsDouble (py_max)));
    }
  // Bounds = vector of pairs
  else if (is_np_array
           && PyArray_DIMS (py_bounds)[0] == constraint->outputSize ()
           && PyArray_DIMS (py_bounds)[1] == 2)
    {
      typedef problem_t::intervals_t intervals_t;
      typedef problem_t::scales_t    scales_t;

      scales_t scales (constraint->outputSize (), 1.);
      intervals_t bounds (constraint->outputSize ());

      for (Function::size_type i = 0; i < constraint->outputSize (); ++i)
	{
	  // TODO: check array type
	  bounds[i].first = * (double*) (PyArray_GETPTR2 (py_bounds, i, 0));
	  bounds[i].second = * (double*) (PyArray_GETPTR2 (py_bounds, i, 1));
	}

      problem->addConstraint (constraint, bounds, scales);
    }
  else
    {
      PyErr_SetString (PyExc_TypeError,
                       "3rd argument's size must match the constraint's output size.");
      return 0;
    }

  Py_INCREF (Py_None);
  return Py_None;
}


static PyObject*
solve (PyObject*, PyObject* args)
{
  factory_t* factory = 0;
  if (!PyArg_ParseTuple (args, "O&",
			 &detail::factoryConverter, &factory))
    return 0;

  (*factory) ().solve ();
  Py_INCREF (Py_None);
  return Py_None;
}

static PyObject*
minimum (PyObject*, PyObject* args)
{
  factory_t* factory = 0;
  if (!PyArg_ParseTuple (args, "O&",
			 &detail::factoryConverter, &factory))
    return 0;

  solver_t::result_t result = (*factory) ().minimum ();

  switch (result.which ())
    {
      // should never happen
    case solver_t::SOLVER_NO_SOLUTION:
      {
	PyErr_SetString (PyExc_TypeError, "problem not yet solved");
	return 0;
      }
    case solver_t::SOLVER_VALUE:
      {
	result_t* result_ = new result_t (boost::get<result_t> (result));
	PyObject* resultPy =
	  PyCapsule_New (result_, ROBOPTIM_CORE_RESULT_CAPSULE_NAME,
			 &detail::destructor<result_t>);
	return Py_BuildValue
	  ("(s,N)", ROBOPTIM_CORE_RESULT_CAPSULE_NAME, resultPy);
      }
    case solver_t::SOLVER_VALUE_WARNINGS:
      {
	resultWithWarnings_t* warn =
	  new resultWithWarnings_t (boost::get<resultWithWarnings_t> (result));
	PyObject* warnPy =
	  PyCapsule_New (warn, ROBOPTIM_CORE_RESULT_WITH_WARNINGS_CAPSULE_NAME,
			 &detail::destructor<resultWithWarnings_t>);
	return Py_BuildValue
	  ("(s,N)", ROBOPTIM_CORE_RESULT_WITH_WARNINGS_CAPSULE_NAME, warnPy);
      }
    case solver_t::SOLVER_ERROR:
      {
	solverError_t* err = new solverError_t
	  (boost::get<solverError_t> (result));
	PyObject* errPy =
	  PyCapsule_New (err, ROBOPTIM_CORE_SOLVER_ERROR_CAPSULE_NAME,
			 &detail::destructor<solverError_t>);
	return Py_BuildValue
	  ("(s,N)", ROBOPTIM_CORE_SOLVER_ERROR_CAPSULE_NAME, errPy);
      }
    }
  Py_INCREF (Py_None);
  return Py_None;
}

static PyObject*
getParameter (const parameter_t& parameter)
{
  PyObject* description = PyString_FromString (parameter.description.c_str ());
  PyObject* value = boost::apply_visitor (detail::ParameterValueVisitor (),
                                          parameter.value);

  return PyTuple_Pack (2, description, value);
}

static PyObject*
getSolverParameters (PyObject*, PyObject* args)
{
  factory_t* factory = 0;
  if (!PyArg_ParseTuple (args, "O&",
			 &detail::factoryConverter, &factory))
    return 0;

  if (!factory)
    {
      PyErr_SetString (PyExc_TypeError, "1st argument must be a solver.");
      return 0;
    }

  solver_t& solver = (*factory) ();

  // In C++, parameters are: std::map<std::string, Parameter>
  PyObject* parameters = PyDict_New ();

  for (parameters_t::const_iterator iter = solver.parameters ().begin ();
       iter != solver.parameters ().end (); iter++)
    {
      // Insert object to Python dictionary
      PyDict_SetItemString (parameters, (iter->first).c_str (),
                            getParameter (iter->second));
    }

  return Py_BuildValue ("O", parameters);
}

static PyObject*
setSolverParameters (PyObject*, PyObject* args)
{
  factory_t* factory = 0;
  PyObject* py_parameters = 0;

  if (!PyArg_ParseTuple (args, "O&O",
			 &detail::factoryConverter, &factory, &py_parameters))
    return 0;

  if (!factory)
    {
      PyErr_SetString (PyExc_TypeError, "1st argument must be a solver.");
      return 0;
    }

  if (!PyDict_Check (py_parameters))
    {
      PyErr_SetString (PyExc_TypeError, "2nd argument must be a dictionary.");
      return 0;
    }

  solver_t& solver = (*factory) ();

  // In C++, parameters are: std::map<std::string, Parameter>
  parameters_t& parameters = solver.parameters ();
  parameters.clear ();

  PyObject *key, *value;
  Py_ssize_t pos = 0;
  parameter_t parameter;

  // Iterate over dictionary
  while (PyDict_Next (py_parameters, &pos, &key, &value))
    {
      std::string str_key = "";

      if (PyBytes_Check (key))
	{
	  str_key = PyBytes_AsString (key);
	}
      else if (PyUnicode_Check (key))
	{
	  str_key = PyBytes_AsString (PyUnicode_AsASCIIString (key));
	}
      else
	{
	  continue;
	}
      if (!PyTuple_Check (value) || PyTuple_Size (value) != 2)
        continue;

      PyObject* desc = PyTuple_GetItem (value, 0);

      if (PyBytes_Check (desc))
	{
	  parameter.description = PyBytes_AsString (desc);
	}
      else if (PyUnicode_Check (desc))
	{
	  parameter.description = PyBytes_AsString (PyUnicode_AsASCIIString (desc));
	}
      else
	{
	  continue;
	}

      parameter.value = detail::toParameterValue (PyTuple_GetItem (value, 1));
      parameters[str_key] = parameter;
    }

  Py_INCREF (Py_None);
  return Py_None;
}

static PyObject*
setSolverParameter (PyObject*, PyObject* args)
{
  factory_t* factory = 0;
  PyObject* key = 0;
  PyObject* value = 0;
  PyObject* desc = 0;

  if (!PyArg_ParseTuple (args, "O&OOO",
			 &detail::factoryConverter, &factory, &key, &value, &desc))
    return 0;

  if (!factory)
    {
      PyErr_SetString (PyExc_TypeError, "1st argument must be a solver.");
      return 0;
    }

  solver_t& solver = (*factory) ();

  parameter_t parameter;

  std::string str_key = "";

  if (PyBytes_Check (key))
    {
      str_key = PyBytes_AsString (key);
    }
  else if (PyUnicode_Check (key))
    {
      str_key = PyBytes_AsString (PyUnicode_AsASCIIString (key));
    }

  if (PyBytes_Check (desc))
    {
      parameter.description = PyBytes_AsString (desc);
    }
  else if (PyUnicode_Check (desc))
    {
      parameter.description = PyBytes_AsString (PyUnicode_AsASCIIString (desc));
    }

  parameter.value = detail::toParameterValue (value);

  solver.parameters ()[str_key] = parameter;

  Py_INCREF (Py_None);
  return Py_None;
}

static PyObject*
addIterationCallback (PyObject*, PyObject* args)
{
  ::roboptim::core::python::Multiplexer<solver_t>* multiplexer = 0;
  ::roboptim::core::python::SolverCallback<solver_t>* callback_wrapper = 0;

  if (!PyArg_ParseTuple
      (args, "O&O&:addIterationCallback",
       &detail::multiplexerConverter, &multiplexer,
       &detail::solverCallbackConverter, &callback_wrapper))
    return 0;

  multiplexer->add (detail::to_shared_ptr< ::roboptim::core::python::SolverCallback<solver_t> >
                    (callback_wrapper, PyTuple_GetItem (args, 1)));

  Py_INCREF(Py_None);
  return Py_None;
}

static PyObject*
removeIterationCallback (PyObject*, PyObject* args)
{
  ::roboptim::core::python::Multiplexer<solver_t>* multiplexer = 0;
  size_t index = 0;

  if (!PyArg_ParseTuple
      (args, "O&i:removeIterationCallback",
       &detail::multiplexerConverter, &multiplexer,
       &index))
    return 0;

  multiplexer->remove (index);

  Py_INCREF(Py_None);
  return Py_None;
}

static PyObject*
bindSolverCallback (PyObject*, PyObject* args)
{
  ::roboptim::core::python::SolverCallback<solver_t>* callback_wrapper = 0;
  PyObject* callback = 0;

  if (!PyArg_ParseTuple
      (args, "O&O:bindSolverCallback",
       &detail::solverCallbackConverter, &callback_wrapper, &callback))
    return 0;

  if (!callback_wrapper)
    {
      PyErr_SetString
	(PyExc_TypeError,
	 "Failed to retrieve solver callback object");
      return 0;
    }

  if (!callback)
    {
      PyErr_SetString
	(PyExc_TypeError,
	 "Failed to retrieve callback function object");
      return 0;
    }

  if (!PyCallable_Check (callback))
    {
      PyErr_SetString
	(PyExc_TypeError,
	 "2nd argument must be callable");
      return 0;
    }

  callback_wrapper->setCallback (callback);

  Py_INCREF(Py_None);
  return Py_None;
}

static PyObject*
addOptimizationLogger (PyObject*, PyObject* args)
{
  factory_t* factory = 0;
  ::roboptim::core::python::Multiplexer<solver_t>* multiplexer = 0;
  const char* log_dir = 0;

  if (!PyArg_ParseTuple
      (args, "O&O&s:addOptimizationLogger",
       &detail::factoryConverter, &factory,
       &detail::multiplexerConverter, &multiplexer,
       &log_dir))
    return 0;

  // Note: logging is completed when the OptimizationLogger object is
  // destroyed, so it should be created/destroyed in the same scope
  // as solve().
  logger_t* logger = new logger_t ((*factory) (), log_dir, false);

  PyObject* loggerPy =
    PyCapsule_New (logger, ROBOPTIM_CORE_OPTIMIZATION_LOGGER_CAPSULE_NAME,
                   &detail::destructor<logger_t>);

  // Register the callback to the multiplexer
  multiplexer->add (detail::to_shared_ptr<logger_t> (logger, loggerPy));

  return Py_BuildValue
    ("(s,N)", ROBOPTIM_CORE_OPTIMIZATION_LOGGER_CAPSULE_NAME, loggerPy);
}


static PyObject*
getStateParameter (const stateParameter_t& parameter)
{
  PyObject* description = PyString_FromString (parameter.description.c_str ());
  PyObject* value = boost::apply_visitor (detail::StateParameterValueVisitor (),
                                          parameter.value);

  return PyTuple_Pack (2, description, value);
}

static PyObject*
getSolverStateParameters (PyObject*, PyObject* args)
{
  solverState_t* state = 0;
  if (!PyArg_ParseTuple (args, "O&",
			 &detail::solverStateConverter, &state))
    return 0;

  if (!state)
    {
      PyErr_SetString (PyExc_TypeError, "1st argument must be a solver state.");
      return 0;
    }

  // In C++, parameters are: std::map<std::string, Parameter>
  PyObject* parameters = PyDict_New ();

  for (stateParameters_t::const_iterator iter = state->parameters ().begin ();
       iter != state->parameters ().end (); iter++)
    {
      // Insert object to Python dictionary
      PyDict_SetItemString (parameters, (iter->first).c_str (),
                            getStateParameter (iter->second));
    }

  return Py_BuildValue ("O", parameters);
}

static PyObject*
setSolverStateParameters (PyObject*, PyObject* args)
{
  solverState_t* state = 0;
  PyObject* py_parameters = 0;

  if (!PyArg_ParseTuple (args, "O&O",
			 &detail::solverStateConverter, &state, &py_parameters))
    return 0;

  if (!state)
    {
      PyErr_SetString (PyExc_TypeError, "1st argument must be a solver state.");
      return 0;
    }

  if (!PyDict_Check (py_parameters))
    {
      PyErr_SetString (PyExc_TypeError, "2nd argument must be a dictionary.");
      return 0;
    }

  // In C++, parameters are: std::map<std::string, Parameter>
  stateParameters_t& parameters = state->parameters ();
  parameters.clear ();

  PyObject *key, *value;
  Py_ssize_t pos = 0;
  stateParameter_t parameter;

  // Iterate over dictionary
  while (PyDict_Next (py_parameters, &pos, &key, &value))
    {
      std::string str_key = "";

      if (PyBytes_Check (key))
	{
	  str_key = PyBytes_AsString (key);
	}
      else if (PyUnicode_Check (key))
	{
	  str_key = PyBytes_AsString (PyUnicode_AsASCIIString (key));
	}
      else
	{
	  continue;
	}
      if (!PyTuple_Check (value) || PyTuple_Size (value) != 2)
        continue;

      PyObject* desc = PyTuple_GetItem (value, 0);

      if (PyBytes_Check (desc))
	{
	  parameter.description = PyBytes_AsString (desc);
	}
      else if (PyUnicode_Check (desc))
	{
	  parameter.description = PyBytes_AsString (PyUnicode_AsASCIIString (desc));
	}
      else
	{
	  continue;
	}

      parameter.value = detail::toStateParameterValue (PyTuple_GetItem (value, 1));
      parameters[str_key] = parameter;
    }

  Py_INCREF (Py_None);
  return Py_None;
}

static PyObject*
getSolverStateX (PyObject*, PyObject* args)
{
  solverState_t* state = 0;
  if (!PyArg_ParseTuple (args, "O&",
			 &detail::solverStateConverter, &state))
    return 0;

  if (!state)
    {
      PyErr_SetString (PyExc_TypeError, "1st argument must be a solver state.");
      return 0;
    }

  npy_intp n = static_cast<npy_intp> (state->x ().size ());
  PyObject* vec = PyArray_SimpleNewFromData (1, &n,
                                             NPY_DOUBLE, state->x ().data ());
  if (!vec)
    {
      PyErr_SetString (PyExc_TypeError, "cannot convert state.x");
      return 0;
    }

  return vec;
}

static PyObject*
setSolverStateX (PyObject*, PyObject* args)
{
  solverState_t* state = 0;
  PyObject* py_parameters = 0;

  if (!PyArg_ParseTuple (args, "O&O",
			 &detail::solverStateConverter, &state, &py_parameters))
    return 0;

  if (!state)
    {
      PyErr_SetString (PyExc_TypeError, "1st argument must be a solver state.");
      return 0;
    }

  if (!py_parameters || !PyArray_Check (py_parameters))
    {
      PyErr_SetString (PyExc_TypeError, "2nd argument must be a NumPy array.");
      return 0;
    }

  if (PyArray_NDIM (py_parameters) != 1 || state->x ().size () != PyArray_DIMS (py_parameters)[0])
    {
      PyErr_SetString (PyExc_TypeError, "x vector size is invalid.");
      return 0;
    }

  Eigen::Map<Function::vector_t> vecEigen
    (static_cast<double*> (PyArray_DATA (py_parameters)),
     PyArray_DIMS (py_parameters)[0]);
  state->x () = vecEigen;

  Py_INCREF (Py_None);
  return Py_None;
}

static PyObject*
getSolverStateCost (PyObject*, PyObject* args)
{
  solverState_t* state = 0;
  if (!PyArg_ParseTuple (args, "O&",
			 &detail::solverStateConverter, &state))
    return 0;

  if (!state)
    {
      PyErr_SetString (PyExc_TypeError, "1st argument must be a solver state.");
      return 0;
    }

  if (!state->cost ())
    {
      Py_INCREF (Py_None);
      return Py_None;
    }

  PyObject* cost = PyFloat_FromDouble (*(state->cost ()));

  if (!cost)
    {
      PyErr_SetString (PyExc_TypeError, "cannot convert state.cost");
      return 0;
    }

  return cost;
}

static PyObject*
setSolverStateCost (PyObject*, PyObject* args)
{
  solverState_t* state = 0;
  double cost = 0;

  if (!PyArg_ParseTuple (args, "O&d",
			 &detail::solverStateConverter, &state, &cost))
    return 0;

  if (!state)
    {
      PyErr_SetString (PyExc_TypeError, "1st argument must be a solver state.");
      return 0;
    }

  state->cost () = cost;

  Py_INCREF (Py_None);
  return Py_None;
}

static PyObject*
getSolverStateConstraintViolation (PyObject*, PyObject* args)
{
  solverState_t* state = 0;
  if (!PyArg_ParseTuple (args, "O&",
			 &detail::solverStateConverter, &state))
    return 0;

  if (!state)
    {
      PyErr_SetString (PyExc_TypeError, "1st argument must be a solver state.");
      return 0;
    }

  if (!state->constraintViolation ())
    {
      Py_INCREF (Py_None);
      return Py_None;
    }

  PyObject* violation = PyFloat_FromDouble (*(state->constraintViolation ()));

  if (!violation)
    {
      PyErr_SetString (PyExc_TypeError, "cannot convert state.constraintViolation");
      return 0;
    }

  return violation;
}

static PyObject*
setSolverStateConstraintViolation (PyObject*, PyObject* args)
{
  solverState_t* state = 0;
  double violation = 0;

  if (!PyArg_ParseTuple (args, "O&d",
			 &detail::solverStateConverter, &state, &violation))
    return 0;

  if (!state)
    {
      PyErr_SetString (PyExc_TypeError, "1st argument must be a solver state.");
      return 0;
    }

  state->constraintViolation () = violation;

  Py_INCREF (Py_None);
  return Py_None;
}


template <typename T>
PyObject*
toDict (T& obj);

template <>
PyObject*
toDict<result_t> (result_t& result)
{
  // In C++, parameters are: std::map<std::string, Parameter>
  PyObject* dict_result = PyDict_New ();

  PyDict_SetItemString (dict_result, "inputSize",
                        PyInt_FromLong (result.inputSize));
  PyDict_SetItemString (dict_result, "outputSize",
                        PyInt_FromLong (result.outputSize));


  npy_intp npy_size = static_cast<npy_intp> (result.x.size ());
  PyObject* xNumpy =
    PyArray_SimpleNewFromData (1, &npy_size,
			       NPY_DOUBLE, result.x.data ());
  if (!xNumpy)
    {
      PyErr_SetString (PyExc_TypeError, "cannot convert result.x");
      return 0;
    }
  PyDict_SetItemString (dict_result, "x", xNumpy);

  npy_size = static_cast<npy_intp> (result.value.size ());
  PyObject* valueNumpy =
    PyArray_SimpleNewFromData (1, &npy_size,
			       NPY_DOUBLE, result.value.data ());
  if (!valueNumpy)
    {
      PyErr_SetString (PyExc_TypeError, "cannot convert result.value");
      return 0;
    }
  PyDict_SetItemString (dict_result, "value", valueNumpy);

  npy_size = static_cast<npy_intp> (result.constraints.size ());
  PyObject* constraintsNumpy =
    PyArray_SimpleNewFromData (1, &npy_size,
			       NPY_DOUBLE, result.constraints.data ());
  if (!constraintsNumpy)
    {
      PyErr_SetString (PyExc_TypeError, "cannot convert result.constraints");
      return 0;
    }
  PyDict_SetItemString (dict_result, "constraints", constraintsNumpy);

  npy_size = static_cast<npy_intp> (result.lambda.size ());
  PyObject* lambdaNumpy =
    PyArray_SimpleNewFromData (1, &npy_size,
			       NPY_DOUBLE, result.lambda.data ());
  if (!lambdaNumpy)
    {
      PyErr_SetString (PyExc_TypeError, "cannot convert result.lambda");
      return 0;
    }
  PyDict_SetItemString (dict_result, "lambda", lambdaNumpy);

  return Py_BuildValue ("O", dict_result);
}


template <typename T>
PyObject*
toDict (PyObject*, PyObject* args);

template <>
PyObject*
toDict<result_t> (PyObject*, PyObject* args)
{
  result_t* result = 0;

  if (!PyArg_ParseTuple (args, "O&",
			 &detail::resultConverter, &result))
    return 0;

  if (!result)
    {
      PyErr_SetString (PyExc_TypeError, "1st argument must be a result.");
      return 0;
    }

  return toDict<result_t> (*result);
}

template <>
PyObject*
toDict<resultWithWarnings_t> (PyObject* obj, PyObject* args)
{
  PyObject* dict_result = toDict<result_t> (obj, args);

  if (!dict_result)
    {
      PyErr_SetString (PyExc_TypeError,
                       "1st argument must be inherited from result.");
      return 0;
    }

  resultWithWarnings_t* result = 0;

  if (!PyArg_ParseTuple (args, "O&",
			 &detail::resultWithWarningsConverter, &result))
    return 0;

  if (!result)
    {
      PyErr_SetString (PyExc_TypeError,
                       "1st argument must be a result with warnings.");
      return 0;
    }

  // Warnings stored as a list
  PyObject* warnings = PyList_New (result->warnings.size ());
  for (size_t i = 0; i < result->warnings.size (); ++i)
    {
      PyList_SetItem (warnings, i,
                      PyString_FromString (result->warnings[i].what ()));
    }
  PyDict_SetItemString (dict_result, "warnings", warnings);

  return Py_BuildValue ("O", dict_result);
}

template <>
PyObject*
toDict<solverError_t> (PyObject*, PyObject* args)
{
  solverError_t* error = 0;

  if (!PyArg_ParseTuple (args, "O&",
			 &detail::solverErrorConverter, &error))
    return 0;

  if (!error)
    {
      PyErr_SetString (PyExc_TypeError,
                       "1st argument must be a solver error.");
      return 0;
    }

  PyObject* dict_error = PyDict_New ();

  PyDict_SetItemString (dict_error, "error",
                        PyString_FromString (error->what ()));

  if (error->lastState ())
    {
      PyObject* lastState = toDict<result_t> (*(error->lastState ()));
      PyDict_SetItemString (dict_error, "lastState", lastState);
    }

  return Py_BuildValue ("O", dict_error);
}


template <typename T>
PyObject*
print (PyObject*, PyObject* args);

template <>
PyObject*
print<problem_t> (PyObject*, PyObject* args)
{
  problem_t* obj = 0;
  if (!PyArg_ParseTuple
      (args, "O&", &detail::problemConverter, &obj))
    return 0;
  if (!obj)
    {
      PyErr_SetString (PyExc_TypeError, "failed to retrieve object");
      return 0;
    }

  std::stringstream ss;
  ss << *obj;

  return Py_BuildValue ("s", ss.str ().c_str ());
}

template <>
PyObject*
print<factory_t> (PyObject*, PyObject* args)
{
  factory_t* obj = 0;
  if (!PyArg_ParseTuple
      (args, "O&", &detail::factoryConverter, &obj))
    return 0;
  if (!obj)
    {
      PyErr_SetString (PyExc_TypeError, "failed to retrieve object");
      return 0;
    }

  std::stringstream ss;
  ss << (*obj) ();

  return Py_BuildValue ("s", ss.str ().c_str ());
}

template <>
PyObject*
print<solverState_t> (PyObject*, PyObject* args)
{
  solverState_t* obj = 0;
  if (!PyArg_ParseTuple
      (args, "O&", &detail::solverStateConverter, &obj))
    return 0;
  if (!obj)
    {
      PyErr_SetString (PyExc_TypeError, "failed to retrieve object");
      return 0;
    }

  std::stringstream ss;
  ss << (*obj);

  return Py_BuildValue ("s", ss.str ().c_str ());
}

template <>
PyObject*
print<result_t> (PyObject*, PyObject* args)
{
  result_t* obj = 0;
  if (!PyArg_ParseTuple
      (args, "O&", &detail::resultConverter, &obj))
    return 0;
  if (!obj)
    {
      PyErr_SetString (PyExc_TypeError, "failed to retrieve object");
      return 0;
    }

  std::stringstream ss;
  ss << (*obj);

  return Py_BuildValue ("s", ss.str ().c_str ());
}

template <>
PyObject*
print<resultWithWarnings_t> (PyObject*, PyObject* args)
{
  resultWithWarnings_t* obj = 0;
  if (!PyArg_ParseTuple
      (args, "O&", &detail::resultWithWarningsConverter, &obj))
    return 0;
  if (!obj)
    {
      PyErr_SetString (PyExc_TypeError, "failed to retrieve object");
      return 0;
    }

  std::stringstream ss;
  ss << (*obj);

  return Py_BuildValue ("s", ss.str ().c_str ());
}

template <>
PyObject*
print<solverError_t> (PyObject*, PyObject* args)
{
  solverError_t* obj = 0;
  if (!PyArg_ParseTuple
      (args, "O&", &detail::solverErrorConverter, &obj))
    return 0;
  if (!obj)
    {
      PyErr_SetString (PyExc_TypeError, "failed to retrieve object");
      return 0;
    }

  std::stringstream ss;
  ss << (*obj);

  return Py_BuildValue ("s", ss.str ().c_str ());
}

template <typename T>
PyObject*
print (PyObject*, PyObject* args)
{
  T* obj = 0;
  if (!PyArg_ParseTuple
      (args, "O&", &detail::functionConverter, &obj))
    return 0;
  if (!obj)
    {
      PyErr_SetString (PyExc_TypeError, "failed to retrieve object");
      return 0;
    }

  std::stringstream ss;
  ss << *obj;

  return Py_BuildValue ("s", ss.str ().c_str ());
}

static PyMethodDef RobOptimCoreMethods[] =
  {
    {"Function", createFunction<Function>, METH_VARARGS,
     "Create a Function object."},
    {"inputSize", inputSize, METH_VARARGS,
     "Return function input size."},
    {"outputSize", outputSize, METH_VARARGS,
     "Return function output size."},
    {"getName", getName, METH_VARARGS,
     "Return function name."},

    {"DifferentiableFunction", createFunction<DifferentiableFunction>,
     METH_VARARGS, "Create a DifferentiableFunction object."},
    {"TwiceDifferentiableFunction", createFunction<TwiceDifferentiableFunction>,
     METH_VARARGS, "Create a TwiceDifferentiableFunction object."},
    {"Problem", createProblem, METH_VARARGS,
     "Create a Problem object."},
    {"Solver", createSolver, METH_VARARGS,
     "Create a Solver object through the solver factory."},
    {"compute", compute, METH_VARARGS,
     "Evaluate a function."},
    {"gradient", gradient, METH_VARARGS,
     "Evaluate a function gradient."},
    {"jacobian", jacobian, METH_VARARGS,
     "Evaluate a function Jacobian."},
    {"bindCompute", bindCompute, METH_VARARGS,
     "Bind a Python function to function computation."},
    {"bindGradient", bindGradient, METH_VARARGS,
     "Bind a Python function to gradient computation."},
    {"bindJacobian", bindJacobian, METH_VARARGS,
     "Bind a Python function to Jacobian computation."},

    {"getStartingPoint", getStartingPoint, METH_VARARGS,
     "Get the problem starting point."},
    {"setStartingPoint", setStartingPoint, METH_VARARGS,
     "Set the problem starting point."},
    {"getArgumentBounds", getArgumentBounds, METH_VARARGS,
     "Get the problem argument bounds."},
    {"setArgumentBounds", setArgumentBounds, METH_VARARGS,
     "Set the problem argument bounds."},
    {"getArgumentScales", getArgumentScales, METH_VARARGS,
     "Get the problem scales."},
    {"setArgumentScales", setArgumentScales, METH_VARARGS,
     "Set the problem scales."},
    {"addConstraint", addConstraint, METH_VARARGS,
     "Add a constraint to the problem."},

    // FunctionPool functions
    {"FunctionPool", createFunction<FunctionPool>,
     METH_VARARGS, "Create a FunctionPool object."},

    // Solver functions
    {"solve", solve, METH_VARARGS,
     "Solve the optimization problem."},
    {"minimum", minimum, METH_VARARGS,
     "Retrieve the optimization result."},
    {"getSolverParameters", getSolverParameters, METH_VARARGS,
     "Get the solver parameters."},
    {"setSolverParameters", setSolverParameters, METH_VARARGS,
     "Set the solver parameters."},
    {"setSolverParameter", setSolverParameter, METH_VARARGS,
     "Set a solver parameter."},
    {"addIterationCallback", addIterationCallback, METH_VARARGS,
     "Add a solver iteration callback."},
    {"removeIterationCallback", removeIterationCallback, METH_VARARGS,
     "Remove a solver iteration callback."},
    {"addOptimizationLogger", addOptimizationLogger, METH_VARARGS,
     "Add an optimization logger."},

    // SolverState functions
    {"getSolverStateX", getSolverStateX, METH_VARARGS,
     "Get the solver state x."},
    {"setSolverStateX", setSolverStateX, METH_VARARGS,
     "Set the solver state x."},
    {"getSolverStateCost", getSolverStateCost, METH_VARARGS,
     "Get the solver state cost."},
    {"setSolverStateCost", setSolverStateCost, METH_VARARGS,
     "Set the solver state cost."},
    {"getSolverStateConstraintViolation",
     getSolverStateConstraintViolation, METH_VARARGS,
     "Get the solver state constraint violation."},
    {"setSolverStateConstraintViolation",
     setSolverStateConstraintViolation, METH_VARARGS,
     "Set the solver state constraint violation."},
    {"getSolverStateParameters", getSolverStateParameters, METH_VARARGS,
     "Get the solver state parameters."},
    {"setSolverStateParameters", setSolverStateParameters, METH_VARARGS,
     "Set the solver state parameters."},

    // Solver callback
    {"Multiplexer", createMultiplexer, METH_VARARGS,
     "Create a solver callback multiplexer."},
    {"SolverCallback", createSolverCallback<solver_t>, METH_VARARGS,
     "Create a solver callback."},
    {"bindSolverCallback", bindSolverCallback, METH_VARARGS,
     "Bind a Python function that serves as a solver callback."},

    // Result functions
    {"resultToDict", toDict<result_t>, METH_VARARGS,
     "Convert a Result object to a Python dictionary."},
    {"resultWithWarningsToDict", toDict<resultWithWarnings_t>, METH_VARARGS,
     "Convert a ResultWithWarnings object to a Python dictionary."},
    {"solverErrorToDict", toDict<solverError_t>, METH_VARARGS,
     "Convert a SolverError object to a Python dictionary."},

    // Finite-differences functions
    {"SimpleFiniteDifferenceGradient",
     createFDWrapper<FiniteDifferenceGradient<simplePolicy_t> >,
     METH_VARARGS, "Create a FiniteDifferenceGradient with forward difference."},
    {"FivePointsFiniteDifferenceGradient",
     createFDWrapper<FiniteDifferenceGradient<fivePointsPolicy_t> >,
     METH_VARARGS, "Create a FiniteDifferenceGradient with the 5-points rule."},

    // Print functions
    {"strFunction", print<Function>, METH_VARARGS,
     "Print a function as a Python string."},
    {"strProblem", print<problem_t>, METH_VARARGS,
     "Print a problem as a Python string."},
    {"strSolver", print<factory_t>, METH_VARARGS,
     "Print a solver as a Python string."},
    {"strSolverState", print<solverState_t>, METH_VARARGS,
     "Print a solver state as a Python string."},
    {"strResult", print<result_t>, METH_VARARGS,
     "Print a result as a Python string."},
    {"strResultWithWarnings", print<resultWithWarnings_t>, METH_VARARGS,
     "Print a result as a Python string."},
    {"strSolverError", print<solverError_t>, METH_VARARGS,
     "Print a solver error as a Python string."},
    {0, 0, 0, 0}
  };

#if PY_MAJOR_VERSION >= 3
static struct PyModuleDef moduledef =
  {
    PyModuleDef_HEAD_INIT,
    "wrap",              /* m_name */
    "RobOptim wrapper",  /* m_doc */
    -1,                  /* m_size */
    RobOptimCoreMethods, /* m_methods */
    NULL,                /* m_reload */
    NULL,                /* m_traverse */
    NULL,                /* m_clear */
    NULL,                /* m_free */
  };
#endif

namespace {

#if PY_MAJOR_VERSION >= 3
  int
#else
  void
#endif
  init_numpy()
  {
    import_array ();

#if PY_MAJOR_VERSION >= 3
    return 0;
#endif
  }

  static PyObject *
  moduleinit(void)
  {
    PyObject* m = 0;

#if PY_MAJOR_VERSION >= 3
    m = PyModule_Create (&moduledef);
#else
    m = Py_InitModule ("wrap", RobOptimCoreMethods);
#endif //! PY_MAJOR_VERSION

    // Initialize numpy.
    init_numpy ();

    if (m == 0)
      return NULL;

    return m;
  }
} // end of namespace

#if PY_MAJOR_VERSION < 3
PyMODINIT_FUNC
initwrap(void)
{
  moduleinit ();
}
#else
PyMODINIT_FUNC
PyInit_wrap(void)
{
  return moduleinit ();
}
#endif
