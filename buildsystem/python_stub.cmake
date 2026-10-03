# Copyright 2026-2026 the openage authors. See copying.md for legal info.

# XR fork: replacements for the functions of python.cmake when the Python and
# Cython parts are not built (OPENAGE_PYTHON=OFF, e.g. Android).
# Same names and signatures, but they do nothing. python_finalize() still runs
# the code generation, which then copies the output of a host build
# (OPENAGE_CODEGEN_DIR, see codegen.cmake).

function(python_init)
endfunction()

function(add_cython_modules)
endfunction()

function(pyext_link_libraries SOURCE)
endfunction()

function(pyext_include_directories SOURCE)
endfunction()

function(pxdgen)
endfunction()

function(add_pxds)
endfunction()

function(add_py_modules)
endfunction()

function(python_finalize)
	codegen_run()
endfunction()
