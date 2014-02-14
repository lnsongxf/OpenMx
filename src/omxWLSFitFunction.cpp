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

#include <R.h>
#include <Rinternals.h>
#include <Rdefines.h>
#include <R_ext/Rdynload.h>
#include <R_ext/BLAS.h>
#include <R_ext/Lapack.h>
#include "omxAlgebraFunctions.h"
#include "omxWLSFitFunction.h"

void flattenDataToVector(omxMatrix* cov, omxMatrix* means, omxThresholdColumn* thresholds, int nThresholds, omxMatrix* vector) {
    // TODO: vectorize data flattening
    // if(OMX_DEBUG) { mxLog("Flattening out data vectors: cov 0x%x, mean 0x%x, thresh 0x%x[n=%d] ==> 0x%x", 
    //         cov, means, thresholds, nThresholds, vector); }
    
    int nextLoc = 0;
    for(int j = 0; j < cov->rows; j++) {
        for(int k = 0; k <= j; k++) {
            omxSetVectorElement(vector, nextLoc, omxMatrixElement(cov, k, j)); // Use upper triangle in case of SYMM-style mat.
            nextLoc++;
        }
    }
    if (means != NULL) {
        for(int j = 0; j < cov->rows; j++) {
            omxSetVectorElement(vector, nextLoc, omxVectorElement(means, j));
            nextLoc++;
        }
    }
    if (thresholds != NULL) {
        for(int j = 0; j < nThresholds; j++) {
            omxThresholdColumn* thresh = thresholds + j;
            for(int k = 0; k < thresh->numThresholds; k++) {
                omxSetVectorElement(vector, nextLoc, omxMatrixElement(thresh->matrix, k, thresh->column));
                nextLoc++;
            }
        }
    }
}

void omxDestroyWLSFitFunction(omxFitFunction *oo) {

	if(OMX_DEBUG) {mxLog("Freeing WLS FitFunction.");}
    if(oo->argStruct == NULL) return;

	omxWLSFitFunction* owo = ((omxWLSFitFunction*)oo->argStruct);
    omxFreeMatrixData(owo->observedFlattened);
    omxFreeMatrixData(owo->expectedFlattened);
    omxFreeMatrixData(owo->weights);
    omxFreeMatrixData(owo->B);
    omxFreeMatrixData(owo->P);
}

static void omxCallWLSFitFunction(omxFitFunction *oo, int want, FitContext *) {
	if (want & (FF_COMPUTE_PREOPTIMIZE)) return;

	if(OMX_DEBUG) { mxLog("Beginning WLS Evaluation.");}
	// Requires: Data, means, covariances.

	double sum = 0.0;

	omxMatrix *oCov, *oMeans, *eCov, *eMeans, *P, *B, *weights, *oFlat, *eFlat;
	
    omxThresholdColumn *oThresh, *eThresh;

	omxWLSFitFunction *owo = ((omxWLSFitFunction*)oo->argStruct);
	
    /* Locals for readability.  Compiler should cut through this. */
	oCov 		= owo->observedCov;
	oMeans		= owo->observedMeans;
	oThresh		= owo->observedThresholds;
	eCov		= owo->expectedCov;
	eMeans 		= owo->expectedMeans;
	eThresh 	= owo->expectedThresholds;
	oFlat		= owo->observedFlattened;
	eFlat		= owo->expectedFlattened;
	weights		= owo->weights;
	B			= owo->B;
	P			= owo->P;
    int nThresh = owo->nThresholds;
    int onei    = 1;
	
	omxExpectation* expectation = oo->expectation;

    /* Recompute and recopy */
	if(OMX_DEBUG) { mxLog("WLSFitFunction Computing expectation"); }
	omxExpectationCompute(expectation, NULL);

    // TODO: Flatten data only once.
	flattenDataToVector(oCov, oMeans, oThresh, nThresh, oFlat);
	flattenDataToVector(eCov, eMeans, eThresh, nThresh, eFlat);

	omxCopyMatrix(B, oFlat);

	omxDAXPY(-1.0, eFlat, B);
	
    if(weights != NULL) {
        omxDGEMV(TRUE, 1.0, weights, B, 0.0, P);
    } else {
        // ULS Case: Memcpy faster than dgemv.
        // TODO: Better to use an omxMatrix duplicator here.
        memcpy(P, B, B->cols*sizeof(double));
    }

    sum = F77_CALL(ddot)(&(P->cols), P->data, &onei, B->data, &onei);

    oo->matrix->data[0] = sum;

	if(OMX_DEBUG) { mxLog("WLSFitFunction value comes to: %f.", oo->matrix->data[0]); }

}

void omxPopulateWLSAttributes(omxFitFunction *oo, SEXP algebra) {
    if(OMX_DEBUG) { mxLog("Populating WLS Attributes."); }

	omxWLSFitFunction *argStruct = ((omxWLSFitFunction*)oo->argStruct);
	omxMatrix *expCovInt = argStruct->expectedCov;	    		// Expected covariance
	omxMatrix *expMeanInt = argStruct->expectedMeans;			// Expected means
	omxMatrix *weightInt = argStruct->weights;			// Expected means

	SEXP expCovExt, expMeanExt, weightExt, gradients;
	PROTECT(expCovExt = allocMatrix(REALSXP, expCovInt->rows, expCovInt->cols));
	for(int row = 0; row < expCovInt->rows; row++)
		for(int col = 0; col < expCovInt->cols; col++)
			REAL(expCovExt)[col * expCovInt->rows + row] =
				omxMatrixElement(expCovInt, row, col);

	if (expMeanInt != NULL) {
		PROTECT(expMeanExt = allocMatrix(REALSXP, expMeanInt->rows, expMeanInt->cols));
		for(int row = 0; row < expMeanInt->rows; row++)
			for(int col = 0; col < expMeanInt->cols; col++)
				REAL(expMeanExt)[col * expMeanInt->rows + row] =
					omxMatrixElement(expMeanInt, row, col);
	} else {
		PROTECT(expMeanExt = allocMatrix(REALSXP, 0, 0));		
	}
	
	PROTECT(weightExt = allocMatrix(REALSXP, weightInt->rows, weightInt->cols));
	for(int row = 0; row < weightInt->rows; row++)
		for(int col = 0; col < weightInt->cols; col++)
			REAL(weightExt)[col * weightInt->rows + row] =
				omxMatrixElement(weightInt, row, col);
	
	
	if(0) {  /* TODO fix for new internal API
		int nLocs = Global->numFreeParams;
		double gradient[Global->numFreeParams];
		for(int loc = 0; loc < nLocs; loc++) {
			gradient[loc] = NA_REAL;
		}
		//oo->gradientFun(oo, gradient);
		PROTECT(gradients = allocMatrix(REALSXP, 1, nLocs));

		for(int loc = 0; loc < nLocs; loc++)
			REAL(gradients)[loc] = gradient[loc];
		 */
	} else {
		PROTECT(gradients = allocMatrix(REALSXP, 0, 0));
	}
    
	setAttrib(algebra, install("expCov"), expCovExt);
	setAttrib(algebra, install("expMean"), expMeanExt);
	setAttrib(algebra, install("weights"), weightExt);
	setAttrib(algebra, install("gradients"), gradients);
	
	setAttrib(algebra, install("SaturatedLikelihood"), ScalarReal(0));
	setAttrib(algebra, install("IndependenceLikelihood"), ScalarReal(0));
	setAttrib(algebra, install("ADFMisfit"), ScalarReal(omxMatrixElement(oo->matrix, 0, 0)));

	UNPROTECT(4);
}

void omxSetWLSFitFunctionCalls(omxFitFunction* oo) {
	
	/* Set FitFunction Calls to WLS FitFunction Calls */
	oo->computeFun = omxCallWLSFitFunction;
	oo->destructFun = omxDestroyWLSFitFunction;
	oo->populateAttrFun = omxPopulateWLSAttributes;
}

void omxInitWLSFitFunction(omxFitFunction* oo) {
    
	omxMatrix *cov, *means, *weights;
	
    if(OMX_DEBUG) { mxLog("Initializing WLS FitFunction function."); }
	
    int vectorSize = 0;
	
	omxSetWLSFitFunctionCalls(oo);
	
	if(OMX_DEBUG) { mxLog("Retrieving expectation.\n"); }
	if (!oo->expectation) { error("%s requires an expectation", oo->fitType); }
	
	if(OMX_DEBUG) { mxLog("Retrieving data.\n"); }
    omxData* dataMat = oo->expectation->data;
	
	if(strncmp(omxDataType(dataMat), "acov", 4) != 0 && strncmp(omxDataType(dataMat), "cov", 3) != 0) {
		char *errstr = (char*) calloc(250, sizeof(char));
		sprintf(errstr, "WLS FitFunction unable to handle data type %s.  Data must be of type 'acov'.\n", omxDataType(dataMat));
		omxRaiseError(oo->matrix->currentState, -1, errstr);
		free(errstr);
		if(OMX_DEBUG) { mxLog("WLS FitFunction unable to handle data type %s.  Aborting.", omxDataType(dataMat)); }
		return;
	}

	omxWLSFitFunction *newObj = (omxWLSFitFunction*) R_alloc(1, sizeof(omxWLSFitFunction));
	
    if(OMX_DEBUG) { mxLog("WLS being intialized is at %p (within %p).", oo, newObj); }

    /* Get Expectation Elements */
	newObj->expectedCov = omxGetExpectationComponent(oo->expectation, oo, "cov");
	newObj->expectedMeans = omxGetExpectationComponent(oo->expectation, oo, "means");
    newObj->nThresholds = oo->expectation->numOrdinal;
    newObj->expectedThresholds = oo->expectation->thresholds;
    // FIXME: threshold structure should be asked for by omxGetExpectationComponent

	/* Read and set expected means, variances, and weights */
    cov = omxDataMatrix(dataMat, NULL);
    means = omxDataMeans(dataMat, NULL, NULL);
    weights = omxDataAcov(dataMat, NULL);
	newObj->observedThresholds  = omxDataThresholds(dataMat);

    newObj->observedCov = cov;
    newObj->observedMeans = means;
    newObj->weights = weights;
    newObj->n = omxDataNumObs(dataMat);
    newObj->nThresholds = omxDataNumFactor(dataMat);
	UNPROTECT(1);
	
	// Error Checking: Observed/Expected means must agree.  
	// ^ is XOR: true when one is false and the other is not.
	if((newObj->expectedMeans == NULL) ^ (newObj->observedMeans == NULL)) {
	    if(newObj->expectedMeans != NULL) {
		    omxRaiseError(oo->matrix->currentState, OMX_ERROR,
			    "Observed means not detected, but an expected means matrix was specified.\n  If you  wish to model the means, you must provide observed means.\n");
		    return;
	    } else {
		    omxRaiseError(oo->matrix->currentState, OMX_ERROR,
			    "Observed means were provided, but an expected means matrix was not specified.\n  If you provide observed means, you must specify a model for the means.\n");
		    return;	        
	    }
	}

	if((newObj->expectedThresholds == NULL) ^ (newObj->observedThresholds == NULL)) {
	    if(newObj->expectedThresholds != NULL) {
		    omxRaiseError(oo->matrix->currentState, OMX_ERROR,
			    "Observed thresholds not detected, but an expected thresholds matrix was specified.\n   If you wish to model the thresholds, you must provide observed thresholds.\n ");
		    return;
	    } else {
		    omxRaiseError(oo->matrix->currentState, OMX_ERROR,
			    "Observed thresholds were provided, but an expected thresholds matrix was not specified.\nIf you provide observed thresholds, you must specify a model for the thresholds.\n");
		    return;	        
	    }
	}

    /* Error check weight matrix size */
    int ncol = newObj->observedCov->cols;
    vectorSize = (ncol * (ncol + 1) ) / 2;
    if(newObj->expectedMeans != NULL) {
        vectorSize = vectorSize + ncol;
    }
    if(newObj->observedThresholds != NULL) {
        for(int i = 0; i < newObj->nThresholds; i++) {
            vectorSize = vectorSize + newObj->observedThresholds[i].numThresholds;
        }
    }

    if(weights != NULL && weights->rows != weights->cols && weights->cols != vectorSize) {
        omxRaiseError(oo->matrix->currentState, OMX_DEVELOPER_ERROR,
         "Developer Error in WLS-based FitFunction object: WLS-based expectation specified an incorrectly-sized weight matrix.\nIf you are not developing a new expectation type, you should probably post this to the OpenMx forums.");
     return;
    }

	
	// FIXME: More error checking for incoming Fit Functions

	/* Temporary storage for calculation */
	newObj->observedFlattened = omxInitMatrix(NULL, vectorSize, 1, TRUE, oo->matrix->currentState);
	newObj->expectedFlattened = omxInitMatrix(NULL, vectorSize, 1, TRUE, oo->matrix->currentState);
	newObj->P = omxInitMatrix(NULL, 1, vectorSize, TRUE, oo->matrix->currentState);
	newObj->B = omxInitMatrix(NULL, vectorSize, 1, TRUE, oo->matrix->currentState);

    flattenDataToVector(newObj->observedCov, newObj->observedMeans, newObj->observedThresholds, newObj->nThresholds, newObj->observedFlattened);
    flattenDataToVector(newObj->expectedCov, newObj->expectedMeans, newObj->expectedThresholds, newObj->nThresholds, newObj->expectedFlattened);

    oo->argStruct = (void*)newObj;

}