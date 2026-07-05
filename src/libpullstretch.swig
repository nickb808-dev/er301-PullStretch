/* libpullstretch.swig — SWIG interface for the PullStretch ER-301 Object. */

%module pullstretch_libpullstretch

%include <od/glue/mod.cpp.swig>

%{
#undef SWIGLUA
#include <PullStretch.h>
#define SWIGLUA
using namespace pullstretch;
%}

%include <PullStretch.h>
