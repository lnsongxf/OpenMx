/*
 *  Copyright 2007-2014 The OpenMx Project
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

#include <stdio.h>
#include <sys/types.h>
#include <errno.h>

#include <R.h>
#include <Rinternals.h>
#include <Rdefines.h>
#include <R_ext/Rdynload.h>
#include <R_ext/BLAS.h>
#include <R_ext/Lapack.h>

#include "omxDefines.h"
#include "types.h"
#include "glue.h"
#include "omxOpenmpWrap.h"
#include "omxState.h"
#include "omxMatrix.h"
#include "omxAlgebra.h"
#include "omxFitFunction.h"
#include "omxExpectation.h"
#include "omxNPSOLSpecific.h"
#include "omxImportFrontendState.h"
#include "omxExportBackendState.h"
#include "Compute.h"
#include "dmvnorm.h"
#include "npsolswitch.h"

static SEXP has_NPSOL()
{ return ScalarLogical(HAS_NPSOL); }

static R_CallMethodDef callMethods[] = {
	{"backend", (DL_FUNC) omxBackend, 10},
	{"callAlgebra", (DL_FUNC) omxCallAlgebra, 3},
	{"findIdenticalRowsData", (DL_FUNC) findIdenticalRowsData, 5},
	{"Dmvnorm_wrapper", (DL_FUNC) dmvnorm_wrapper, 3},
	{"hasNPSOL_wrapper", (DL_FUNC) has_NPSOL, 0},
	{NULL, NULL, 0}
};

#ifdef  __cplusplus
extern "C" {
#endif

void R_init_OpenMx(DllInfo *info) {
	R_registerRoutines(info, NULL, callMethods, NULL, NULL);

	// There is no code that will change behavior whether openmp
	// is set for nested or not. I'm just keeping this in case it
	// makes a difference with older versions of openmp. 2012-12-24 JNP
#if defined(_OPENMP) && _OPENMP <= 200505
	omp_set_nested(0);
#endif
}

void R_unload_OpenMx(DllInfo *) {
	// keep this stub in case we need it
}

#ifdef  __cplusplus
}
#endif

void string_to_try_error( const std::string& str )
{
	error("%s", str.c_str());
}

void exception_to_try_error( const std::exception& ex )
{
	string_to_try_error(ex.what());
}

SEXP MxRList::asR()
{
	// detect duplicate keys? TODO
	SEXP names, ans;
	int len = size();
	PROTECT(names = allocVector(STRSXP, len));
	PROTECT(ans = allocVector(VECSXP, len));
	for (int lx=0; lx < len; ++lx) {
		SET_STRING_ELT(names, lx, (*this)[lx].first);
		SET_VECTOR_ELT(ans,   lx, (*this)[lx].second);
	}
	namesgets(ans, names);
	return ans;
}

/* Main functions */
SEXP omxCallAlgebra2(SEXP matList, SEXP algNum, SEXP) {

	omxManageProtectInsanity protectManager;

	if(OMX_DEBUG) { mxLog("-----------------------------------------------------------------------");}
	if(OMX_DEBUG) { mxLog("Explicit call to algebra %d.", INTEGER(algNum)[0]);}

	int j,k,l;
	omxMatrix* algebra;
	int algebraNum = INTEGER(algNum)[0];
	SEXP ans, nextMat;
	char output[MAX_STRING_LEN];

	FitContext::setRFitFunction(NULL);
	Global = new omxGlobal;

	globalState = new omxState;
	omxInitState(globalState);
	if(OMX_DEBUG) { mxLog("Created state object at %p.", globalState);}

	/* Retrieve All Matrices From the MatList */

	if(OMX_DEBUG) { mxLog("Processing %d matrix(ces).", length(matList));}

	omxMatrix *args[length(matList)];
	for(k = 0; k < length(matList); k++) {
		PROTECT(nextMat = VECTOR_ELT(matList, k));	// This is the matrix + populations
		args[k] = omxNewMatrixFromRPrimitive(nextMat, globalState, 1, - k - 1);
		globalState->matrixList.push_back(args[k]);
		if(OMX_DEBUG) {
			mxLog("Matrix initialized at %p = (%d x %d).",
				globalState->matrixList[k], globalState->matrixList[k]->rows, globalState->matrixList[k]->cols);
		}
	}

	algebra = omxNewAlgebraFromOperatorAndArgs(algebraNum, args, length(matList), globalState);

	if(algebra==NULL) {
		error(globalState->statusMsg);
	}

	if(OMX_DEBUG) {mxLog("Completed Algebras and Matrices.  Beginning Initial Compute.");}
	omxStateNextEvaluation(globalState);

	omxRecompute(algebra);

	PROTECT(ans = allocMatrix(REALSXP, algebra->rows, algebra->cols));
	for(l = 0; l < algebra->rows; l++)
		for(j = 0; j < algebra->cols; j++)
			REAL(ans)[j * algebra->rows + l] =
				omxMatrixElement(algebra, l, j);

	if(OMX_DEBUG) { mxLog("All Algebras complete."); }

	output[0] = 0;
	if (isErrorRaised(globalState)) {
		strncpy(output, globalState->statusMsg, MAX_STRING_LEN);
	}

	omxFreeAllMatrixData(algebra);
	omxFreeState(globalState);
	delete Global;

	if(output[0]) error(output);

	return ans;
}

SEXP omxCallAlgebra(SEXP matList, SEXP algNum, SEXP options)
{
	try {
		return omxCallAlgebra2(matList, algNum, options);
	} catch( std::exception& __ex__ ) {
		exception_to_try_error( __ex__ );
	} catch(...) {
		string_to_try_error( "c++ exception (unknown reason)" );
	}
}

static void
friendlyStringToLogical(const char *key, const char *str, int *out)
{
	int understood = FALSE;
	int newVal;
	if (matchCaseInsensitive(str, "Yes")) {
		understood = TRUE;
		newVal = 1;
	} else if (matchCaseInsensitive(str, "No")) {
		understood = TRUE;
		newVal = 0;
	} else if (isdigit(str[0]) && (atoi(str) == 1 || atoi(str) == 0)) {
		understood = TRUE;
		newVal = atoi(str);
	}
	if (!understood) {
		warning("Expecting 'Yes' or 'No' for '%s' but got '%s', ignoring", key, str);
		return;
	}
	if(OMX_DEBUG) { mxLog("%s=%d", key, newVal); }
	*out = newVal;
}

// TODO: make member of omxGlobal class
static void readOpts(SEXP options, int *ciMaxIterations, int *numThreads,
		     int *analyticGradients)
{
		int numOptions = length(options);
		SEXP optionNames;
		PROTECT(optionNames = GET_NAMES(options));
		for(int i = 0; i < numOptions; i++) {
			const char *nextOptionName = CHAR(STRING_ELT(optionNames, i));
			const char *nextOptionValue = STRING_VALUE(VECTOR_ELT(options, i));
			if (matchCaseInsensitive(nextOptionName, "CI Max Iterations")) {
				int newvalue = atoi(nextOptionValue);
				if (newvalue > 0) *ciMaxIterations = newvalue;
			} else if(matchCaseInsensitive(nextOptionName, "Analytic Gradients")) {
				friendlyStringToLogical(nextOptionName, nextOptionValue, analyticGradients);
			} else if(matchCaseInsensitive(nextOptionName, "loglikelihoodScale")) {
				Global->llScale = atof(nextOptionValue);
			} else if(matchCaseInsensitive(nextOptionName, "Number of Threads")) {
				*numThreads = atoi(nextOptionValue);
				if (*numThreads < 1) {
					warning("Computation will be too slow with %d threads; using 1 thread instead", *numThreads);
					*numThreads = 1;
				}
			} else {
				// ignore
			}
		}
		UNPROTECT(1); // optionNames
}

SEXP omxBackend2(SEXP constraints, SEXP matList,
		 SEXP varList, SEXP algList, SEXP expectList, SEXP computeList,
		 SEXP data, SEXP intervalList, SEXP checkpointList, SEXP options)
{
	SEXP nextLoc;

	/* Sanity Check and Parse Inputs */
	/* TODO: Need to find a way to account for nullness in these.  For now, all checking is done on the front-end. */
//	if(!isVector(matList)) error ("matList must be a list");
//	if(!isVector(algList)) error ("algList must be a list");

	omxManageProtectInsanity protectManager;

	FitContext::setRFitFunction(NULL);
	Global = new omxGlobal;

	/* Create new omxState for current state storage and initialize it. */
	globalState = new omxState;
	omxInitState(globalState);
	if(OMX_DEBUG) { mxLog("Created state object at %p.", globalState);}

	readOpts(options, &Global->ciMaxIterations, &Global->numThreads, 
			&Global->analyticGradients);
#if HAS_NPSOL
	omxSetNPSOLOpts(options);
#endif

	if(OMX_DEBUG) mxLog("Protect depth at line %d: %d", __LINE__, protectManager.getDepth());
	omxProcessMxDataEntities(data);
	if (isErrorRaised(globalState)) error(globalState->statusMsg);
    
	if(OMX_DEBUG) mxLog("Protect depth at line %d: %d", __LINE__, protectManager.getDepth());
	omxProcessMxMatrixEntities(matList);
	if (isErrorRaised(globalState)) error(globalState->statusMsg);

	if(OMX_DEBUG) mxLog("Protect depth at line %d: %d", __LINE__, protectManager.getDepth());
	std::vector<double> startingValues;
	omxProcessFreeVarList(varList, &startingValues);
	if (isErrorRaised(globalState)) error(globalState->statusMsg);

	if(OMX_DEBUG) mxLog("Protect depth at line %d: %d", __LINE__, protectManager.getDepth());
	omxProcessMxExpectationEntities(expectList);
	if (isErrorRaised(globalState)) error(globalState->statusMsg);

	if(OMX_DEBUG) mxLog("Protect depth at line %d: %d", __LINE__, protectManager.getDepth());
	omxProcessMxAlgebraEntities(algList);
	if (isErrorRaised(globalState)) error(globalState->statusMsg);

	if(OMX_DEBUG) mxLog("Protect depth at line %d: %d", __LINE__, protectManager.getDepth());
	omxProcessMxFitFunction(algList);
	if (isErrorRaised(globalState)) error(globalState->statusMsg);

	if(OMX_DEBUG) mxLog("Protect depth at line %d: %d", __LINE__, protectManager.getDepth());
	omxProcessMxComputeEntities(computeList);
	if (isErrorRaised(globalState)) error(globalState->statusMsg);

	if(OMX_DEBUG) mxLog("Protect depth at line %d: %d", __LINE__, protectManager.getDepth());
	omxCompleteMxExpectationEntities();
	if (isErrorRaised(globalState)) error(globalState->statusMsg);

	if(OMX_DEBUG) mxLog("Protect depth at line %d: %d", __LINE__, protectManager.getDepth());
	omxCompleteMxFitFunction(algList);
	if (isErrorRaised(globalState)) error(globalState->statusMsg);

	// This is the chance to check for matrix
	// conformability, etc.  Any errors encountered should
	// be reported using R's error() function, not
	// omxRaiseErrorf.

	if(OMX_DEBUG) mxLog("Protect depth at line %d: %d", __LINE__, protectManager.getDepth());
	omxInitialMatrixAlgebraCompute();
	omxResetStatus(globalState);

	for(size_t index = 0; index < globalState->matrixList.size(); index++) {
		omxMarkDirty(globalState->matrixList[index]);
	}
	for(size_t index = 0; index < globalState->algebraList.size(); index++) {
		omxMarkDirty(globalState->algebraList[index]);
	}

	omxCompute *topCompute = NULL;
	if (Global->computeList.size()) topCompute = Global->computeList[0];

	/* Process Matrix and Algebra Population Function */
	/*
	  Each matrix is a list containing a matrix and the other matrices/algebras that are
	  populated into it at each iteration.  The first element is already processed, above.
	  The rest of the list will be processed here.
	*/
	if(OMX_DEBUG) mxLog("Protect depth at line %d: %d", __LINE__, protectManager.getDepth());
	for(int j = 0; j < length(matList); j++) {
		PROTECT(nextLoc = VECTOR_ELT(matList, j));		// This is the matrix + populations
		omxProcessMatrixPopulationList(globalState->matrixList[j], nextLoc);
	}

	if(OMX_DEBUG) mxLog("Protect depth at line %d: %d", __LINE__, protectManager.getDepth());
	omxProcessConstraints(constraints);

	if(OMX_DEBUG) mxLog("Protect depth at line %d: %d", __LINE__, protectManager.getDepth());
	omxProcessConfidenceIntervals(intervalList);

	omxProcessCheckpointOptions(checkpointList);

	for (size_t vg=0; vg < Global->freeGroup.size(); ++vg) {
		Global->freeGroup[vg]->cacheDependencies();
	}

	if(OMX_DEBUG) mxLog("Protect depth at line %d: %d", __LINE__, protectManager.getDepth());
	FitContext fc(startingValues);

	if (topCompute && !isErrorRaised(globalState)) {
		// switch varGroup, if necessary TODO
		topCompute->compute(&fc);
	}

	SEXP evaluations;
	PROTECT(evaluations = NEW_NUMERIC(2));

	REAL(evaluations)[0] = globalState->computeCount;

	if (topCompute && !isErrorRaised(globalState) && globalState->stale) {
		fc.copyParamToModel(globalState);
	}

	MxRList result;

	if(OMX_DEBUG) mxLog("Protect depth at line %d: %d", __LINE__, protectManager.getDepth());
	omxExportResults(globalState, &result); 

	REAL(evaluations)[1] = globalState->computeCount;

	double optStatus = NA_REAL;
	if (topCompute && !isErrorRaised(globalState)) {
		LocalComputeResult cResult;
		topCompute->collectResults(&fc, &cResult, &result);
		optStatus = topCompute->getOptimizerStatus();

		if (cResult.size()) {
			SEXP computes;
			PROTECT(computes = NEW_LIST(cResult.size() * 2));
			for (size_t cx=0; cx < cResult.size(); ++cx) {
				std::pair<int, MxRList*> c1 = cResult[cx];
				SET_VECTOR_ELT(computes, cx*2, ScalarInteger(c1.first));
				SET_VECTOR_ELT(computes, cx*2+1, c1.second->asR());
				delete c1.second;
			}
			result.push_back(std::make_pair(mkChar("computes"), computes));
		}

		if (fc.wanted & FF_COMPUTE_FIT) {
			result.push_back(std::make_pair(mkChar("minimum"), ScalarReal(fc.fit)));
			result.push_back(std::make_pair(mkChar("Minus2LogLikelihood"), ScalarReal(fc.fit)));
		}

		size_t numFree = Global->freeGroup[FREEVARGROUP_ALL]->vars.size();
		if (numFree) {
			// move other global reporting here TODO

			SEXP estimate;
			PROTECT(estimate = allocVector(REALSXP, numFree));
			memcpy(REAL(estimate), fc.est, sizeof(double)*numFree);
			result.push_back(std::make_pair(mkChar("estimate"), estimate));

			if (fc.wanted & FF_COMPUTE_GRADIENT) {
				SEXP Rgradient;
				PROTECT(Rgradient = allocVector(REALSXP, numFree));
				memcpy(REAL(Rgradient), fc.grad, sizeof(double) * numFree);
				result.push_back(std::make_pair(mkChar("gradient"), Rgradient));
			}
			if (fc.wanted & FF_COMPUTE_HESSIAN) {
				SEXP Rhessian;
				PROTECT(Rhessian = allocMatrix(REALSXP, numFree, numFree));
				memcpy(REAL(Rhessian), fc.hess, sizeof(double) * numFree * numFree);
				result.push_back(std::make_pair(mkChar("hessian"), Rhessian));
			}
			if (fc.wanted & FF_COMPUTE_IHESSIAN) {
				SEXP Rihessian;
				PROTECT(Rihessian = allocMatrix(REALSXP, numFree, numFree));
				memcpy(REAL(Rihessian), fc.ihess, sizeof(double) * numFree * numFree);
				result.push_back(std::make_pair(mkChar("ihessian"), Rihessian));
			}
			if (fc.stderrs) {
				SEXP stdErrors;
				PROTECT(stdErrors = allocMatrix(REALSXP, numFree, 1));
				memcpy(REAL(stdErrors), fc.stderrs, sizeof(double) * numFree);
				result.push_back(std::make_pair(mkChar("standardErrors"), stdErrors));
			}
			if (fc.wanted & (FF_COMPUTE_HESSIAN | FF_COMPUTE_IHESSIAN)) {
				result.push_back(std::make_pair(mkChar("infoDefinite"),
								ScalarLogical(fc.infoDefinite)));
				result.push_back(std::make_pair(mkChar("conditionNumber"),
								ScalarReal(fc.infoCondNum)));
			}
		}
	}

	if(OMX_DEBUG) mxLog("Protect depth at line %d: %d", __LINE__, protectManager.getDepth());
	MxRList backwardCompatStatus;
	backwardCompatStatus.push_back(std::make_pair(mkChar("code"), ScalarReal(optStatus)));
	backwardCompatStatus.push_back(std::make_pair(mkChar("status"),
						      ScalarInteger(-isErrorRaised(globalState))));

	if (isErrorRaised(globalState)) {
		SEXP msg;
		PROTECT(msg = allocVector(STRSXP, 1));
		SET_STRING_ELT(msg, 0, mkChar(globalState->statusMsg));
		result.push_back(std::make_pair(mkChar("error"), msg));
		backwardCompatStatus.push_back(std::make_pair(mkChar("statusMsg"), msg));
	}

	result.push_back(std::make_pair(mkChar("status"), backwardCompatStatus.asR()));
	result.push_back(std::make_pair(mkChar("evaluations"), evaluations));

	omxFreeState(globalState);
	delete Global;

	if(OMX_DEBUG) mxLog("Protect depth at line %d: %d", __LINE__, protectManager.getDepth());
	return result.asR();
}

SEXP omxBackend(SEXP constraints, SEXP matList,
		SEXP varList, SEXP algList, SEXP expectList, SEXP computeList,
		SEXP data, SEXP intervalList, SEXP checkpointList, SEXP options)
{
	try {
		return omxBackend2(constraints, matList,
				   varList, algList, expectList, computeList,
				   data, intervalList, checkpointList, options);
	} catch( std::exception& __ex__ ) {
		exception_to_try_error( __ex__ );
	} catch(...) {
		string_to_try_error( "c++ exception (unknown reason)" );
	}
}
