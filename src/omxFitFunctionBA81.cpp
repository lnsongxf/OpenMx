/*
  Copyright 2012-2013 Joshua Nathaniel Pritikin and contributors

  This is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <algorithm>

#include "omxFitFunction.h"
#include "omxExpectationBA81.h"
#include "omxOpenmpWrap.h"
#include "libifa-rpf.h"
#include "matrix.h"
#include "omxBuffer.h"

struct BA81FitState {

	int haveLatentMap;
	std::vector<int> latentMap;
	bool freeLatents;
	int ElatentVersion;

	int haveItemMap;
	size_t numFreeParam;
	int itemDerivPadSize;     // maxParam + maxParam*(1+maxParam)/2
	std::vector<int> paramPerItem;       // itemParam->cols
	std::vector<int> paramFlavor;        // freeParam
	std::vector<int> paramMap;           // itemParam->cols * itemDerivPadSize -> index of free parameter
	std::vector<int> itemGradMap;        // index of gradient -> index of free parameter
	std::vector<int> paramLocations;     // param# -> count of appearances in ItemParam
	std::vector<int> itemParamFree;      // itemParam->cols * itemParam->rows
	std::vector<int> ihessDivisor;       // freeParam * freeParam
	std::vector< matrixVectorProdTerm > hgProd;

	omxMatrix *itemParam;
	omxMatrix *latentMean;
	omxMatrix *latentCov;

	BA81FitState();
	~BA81FitState();
	void copyEstimates(BA81Expect *estate);
};

// writes to upper triangle of full matrix
static void addSymOuterProd(const double weight, const double *vec, const int len, double *out)
{
	for (int d1=0; d1 < len; ++d1) {
		for (int d2=0; d2 <= d1; ++d2) {
			out[d1 * len + d2] += weight * vec[d1] * vec[d2];
		}
	}
}

BA81FitState::BA81FitState()
{
	haveItemMap = FREEVARGROUP_INVALID;
	haveLatentMap = FREEVARGROUP_INVALID;
}

void BA81FitState::copyEstimates(BA81Expect *estate)
{
	omxCopyMatrix(itemParam, estate->itemParam);
	omxCopyMatrix(latentMean, estate->latentMeanOut);
	omxCopyMatrix(latentCov, estate->latentCovOut);
}

static void buildLatentParamMap(omxFitFunction* oo, FitContext *fc)
{
	FreeVarGroup *fvg = fc->varGroup;
	BA81FitState *state = (BA81FitState *) oo->argStruct;
	std::vector<int> &latentMap = state->latentMap;
	BA81Expect *estate = (BA81Expect*) oo->expectation->argStruct;
	int meanNum = estate->latentMeanOut->matrixNumber;
	int covNum = estate->latentCovOut->matrixNumber;
	int maxAbilities = estate->maxAbilities;
	int numLatents = maxAbilities + triangleLoc1(maxAbilities);

	if (state->haveLatentMap == fc->varGroup->id) return;
	if (estate->verbose) mxLog("%s: rebuild latent parameter map for %d", oo->matrix->name, fc->varGroup->id);

	state->freeLatents = false;
	latentMap.assign(numLatents, -1);

	int numParam = int(fvg->vars.size());
	for (int px=0; px < numParam; px++) {
		omxFreeVar *fv = fvg->vars[px];
		for (size_t lx=0; lx < fv->locations.size(); lx++) {
			omxFreeVarLocation *loc = &fv->locations[lx];
			int matNum = ~loc->matrix;
			if (matNum == meanNum) {
				latentMap[loc->row + loc->col] = px;
				state->freeLatents = true;
			} else if (matNum == covNum) {
				int a1 = loc->row;
				int a2 = loc->col;
				if (a1 < a2) std::swap(a1, a2);
				int cell = maxAbilities + triangleLoc1(a1) + a2;
				if (latentMap[cell] == -1) {
					latentMap[cell] = px;

					if (a1 == a2 && fv->lbound == NEG_INF) {
						fv->lbound = 1e-6;  // variance must be positive
						if (fc->est[px] < fv->lbound) {
							error("Starting value for variance %s is negative", fv->name);
						}
					}
				} else if (latentMap[cell] != px) {
					// doesn't detect similar problems in multigroup constraints TODO
					error("In covariance matrix, %s and %s must be constrained equal to preserve symmetry",
					      fvg->vars[latentMap[cell]]->name, fv->name);
				}
				state->freeLatents = true;
			}
		}
	}
	state->haveLatentMap = fc->varGroup->id;
}

static void buildItemParamMap(omxFitFunction* oo, FitContext *fc)
{
	FreeVarGroup *fvg = fc->varGroup;
	BA81FitState *state = (BA81FitState *) oo->argStruct;
	BA81Expect *estate = (BA81Expect*) oo->expectation->argStruct;

	if (state->haveItemMap == fc->varGroup->id) return;
	if (estate->verbose) mxLog("%s: rebuild item parameter map for %d", oo->matrix->name, fc->varGroup->id);

	omxMatrix *itemParam = estate->itemParam;
	int size = itemParam->cols * state->itemDerivPadSize;
	state->paramMap.assign(size, -1);  // matrix location to free param index
	state->itemParamFree.assign(itemParam->rows * itemParam->cols, FALSE);

	size_t numFreeParams = state->numFreeParam = fvg->vars.size();
	state->paramLocations.assign(numFreeParams, 0);
	state->paramFlavor.assign(numFreeParams, -1);

	int totalParam = 0;
	state->paramPerItem.resize(itemParam->cols);
	for (int cx=0; cx < itemParam->cols; ++cx) {
		const double *spec = estate->itemSpec[cx];
		const int id = spec[RPF_ISpecID];
		const int numParam = (*rpf_model[id].numParam)(spec);
		state->paramPerItem[cx] = numParam;
		totalParam += numParam;
	}
	state->itemGradMap.assign(totalParam, -1);

	for (size_t px=0; px < numFreeParams; px++) {
		omxFreeVar *fv = fvg->vars[px];
		state->paramLocations[px] = int(fv->locations.size());
		for (size_t lx=0; lx < fv->locations.size(); lx++) {
			omxFreeVarLocation *loc = &fv->locations[lx];
			int matNum = ~loc->matrix;
			if (matNum == itemParam->matrixNumber) {
				int at = loc->col * state->itemDerivPadSize + loc->row;
				state->paramMap[at] = px;
				state->itemParamFree[loc->col * itemParam->rows + loc->row] = TRUE;

				const double *spec = estate->itemSpec[loc->col];
				int id = spec[RPF_ISpecID];
				int flavor;
				double upper, lower;
				(*rpf_model[id].paramInfo)(spec, loc->row, &flavor, &upper, &lower);
				if (state->paramFlavor[px] < 0) {
					state->paramFlavor[px] = flavor;
				} else if (state->paramFlavor[px] != flavor) {
					error("Cannot equate %s with %s[%d,%d]", fv->name,
					      itemParam->name, loc->row, loc->col);
				}
				if (fv->lbound == NEG_INF && isfinite(lower)) {
					fv->lbound = lower;
					if (fc->est[px] < fv->lbound) {
						error("Starting value %s %f less than lower bound %f",
						      fv->name, fc->est[px], lower);
					}
				}
				if (fv->ubound == INF && isfinite(upper)) {
					fv->ubound = upper;
					if (fc->est[px] > fv->ubound) {
						error("Starting value %s %f greater than upper bound %f",
						      fv->name, fc->est[px], upper);
					}
				}
			}
		}
	}

	int gradOffset = 0;
	for (int cx=0; cx < itemParam->cols; ++cx) {
		for (int rx=0; rx < state->paramPerItem[cx]; ++rx) {
			int at = cx * state->itemDerivPadSize + rx;
			int px = state->paramMap[at];
			if (px >= 0) state->itemGradMap[gradOffset] = px;
			++gradOffset;
		}
	}

	state->ihessDivisor.resize(size);

	for (int cx=0; cx < itemParam->cols; ++cx) {
		int numParam = state->paramPerItem[cx];
		for (int p1=0; p1 < numParam; p1++) {
			int at1 = state->paramMap[cx * state->itemDerivPadSize + p1];
			if (at1 < 0) continue;

			for (int p2=0; p2 <= p1; p2++) {
				int at2 = state->paramMap[cx * state->itemDerivPadSize + p2];
				if (at2 < 0) continue;

				if (at1 < at2) std::swap(at1, at2);  // lower triangle

				//mxLog("Item %d param(%d,%d) -> H[%d,%d]", cx, p1, p2, at1, at2);
				int at = cx * state->itemDerivPadSize + numParam + triangleLoc1(p1) + p2;
				int hoffset = at1 * numFreeParams + at2;

				//mxLog("? H %d * g %d = p %d", hoffset, at2, at1);
				matrixVectorProdTerm mvpt(hoffset, at2, at1);
				state->hgProd.push_back(mvpt);

				if (at1 != at2) {
					matrixVectorProdTerm mvpt(hoffset, at1, at2);
					state->hgProd.push_back(mvpt);
				}

				state->paramMap[at] = numFreeParams + hoffset;

				state->ihessDivisor[at] =
					state->paramLocations[at1] * state->paramLocations[at2];
			}
		}
	}

	state->haveItemMap = fc->varGroup->id;
	//pia(state->paramMap.data(), state->itemDerivPadSize, itemParam->cols);
}

static double
ba81ComputeEMFit(omxFitFunction* oo, int want, FitContext *fc)
{
	const double Scale = Global->llScale;
	BA81FitState *state = (BA81FitState*) oo->argStruct;
	BA81Expect *estate = (BA81Expect*) oo->expectation->argStruct;
	omxMatrix *itemParam = estate->itemParam;
	std::vector<const double*> &itemSpec = estate->itemSpec;
        std::vector<int> &cumItemOutcomes = estate->cumItemOutcomes;
	const int maxDims = estate->maxDims;
	const size_t numItems = estate->itemSpec.size();
	const int do_fit = want & FF_COMPUTE_FIT;
	const int do_deriv = want & (FF_COMPUTE_GRADIENT | FF_COMPUTE_HESSIAN | FF_COMPUTE_IHESSIAN);

	if (estate->verbose) mxLog("%s: em.fit(want fit=%d deriv=%d)", oo->matrix->name, do_fit, do_deriv);

	if (do_fit) ba81OutcomeProb(estate, FALSE, TRUE);

	const int thrDerivSize = itemParam->cols * state->itemDerivPadSize;
	std::vector<double> thrDeriv(thrDerivSize * Global->numThreads);
	double *wherePrep = estate->wherePrep.data();

	double ll = 0;
#pragma omp parallel for num_threads(Global->numThreads) reduction(+:ll)
	for (size_t ix=0; ix < numItems; ix++) {
		const int thrId = omx_absolute_thread_num();
		const double *spec = estate->itemSpec[ix];
		const int id = spec[RPF_ISpecID];
		const rpf_dLL1_t dLL1 = rpf_model[id].dLL1;
		const int iOutcomes = estate->itemOutcomes[ix];
		const int outcomeBase = cumItemOutcomes[ix] * estate->totalQuadPoints;
		const double *weight = estate->expected + outcomeBase;
                const double *oProb = estate->outcomeProb + outcomeBase;
		const double *iparam = omxMatrixColumn(itemParam, ix);
		double *myDeriv = thrDeriv.data() + thrDerivSize * thrId + ix * state->itemDerivPadSize;

		for (long qx=0; qx < estate->totalQuadPoints; qx++) {
			if (do_fit) {
				for (int ox=0; ox < iOutcomes; ox++) {
					ll += weight[ox] * oProb[ox];
				}
			}
			if (do_deriv) {
				(*dLL1)(spec, iparam, wherePrep + qx * maxDims, weight, myDeriv);
			}
			weight += iOutcomes;
			oProb += iOutcomes;
		}
	}

	size_t excluded = 0;

	if (do_deriv) {
		double *deriv0 = thrDeriv.data();

		int perThread = itemParam->cols * state->itemDerivPadSize;
		for (int th=1; th < Global->numThreads; th++) {
			double *thrD = thrDeriv.data() + th * perThread;
			for (int ox=0; ox < perThread; ox++) deriv0[ox] += thrD[ox];
		}

		for (size_t ix=0; ix < numItems; ix++) {
			const double *spec = itemSpec[ix];
			int id = spec[RPF_ISpecID];
			double *iparam = omxMatrixColumn(itemParam, ix);
			double *pad = deriv0 + ix * state->itemDerivPadSize;
			(*rpf_model[id].dLL2)(spec, iparam, pad);
		}

		int numFreeParams = int(state->numFreeParam);
		int numParams = itemParam->cols * state->itemDerivPadSize;
		for (int ox=0; ox < numParams; ox++) {
			int to = state->paramMap[ox];
			if (to == -1) continue;

			// Need to check because this can happen if
			// lbounds/ubounds are not set appropriately.
			if (0 && !isfinite(deriv0[ox])) {
				int item = ox / itemParam->rows;
				mxLog("item parameters:\n");
				const double *spec = itemSpec[item];
				int id = spec[RPF_ISpecID];
				int numParam = (*rpf_model[id].numParam)(spec);
				double *iparam = omxMatrixColumn(itemParam, item);
				pda(iparam, numParam, 1);
				// Perhaps bounds can be pulled in from librpf? TODO
				error("Deriv %d for item %d is %f; are you missing a lbound/ubound?",
				      ox, item, deriv0[ox]);
			}

			if (to < numFreeParams) {
				if (want & FF_COMPUTE_GRADIENT) {
					fc->grad[to] -= Scale * deriv0[ox];
				}
			} else {
				if (want & FF_COMPUTE_HESSIAN) {
					int Hto = to - numFreeParams;
					fc->hess[Hto] -= Scale * deriv0[ox];
				}
			}
		}

		if (want & FF_COMPUTE_IHESSIAN) {
			for (size_t ix=0; ix < numItems; ix++) {
				const double *spec = itemSpec[ix];
				int id = spec[RPF_ISpecID];
				int iParams = (*rpf_model[id].numParam)(spec);
				double *pad = deriv0 + ix * state->itemDerivPadSize + iParams;
				int *mask = state->itemParamFree.data() + ix * itemParam->rows;
				double stress;
				omxApproxInvertPackedPosDefTriangular(iParams, mask, pad, &stress);
				// If items excluded then ihessDivisor is wrong TODO
				if (stress) ++excluded;
			}
			for (int ox=0; ox < numParams; ox++) {
				int to = state->paramMap[ox];
				if (to == -1) continue;
				if (to >= numFreeParams) {
					int Hto = to - numFreeParams;
					fc->ihess[Hto] -= deriv0[ox] / (Scale * state->ihessDivisor[ox]);
				}
			}
		}
	}

	if (excluded && estate->verbose >= 1) {
		mxLog("%s: Hessian not positive definite for %lu/%lu items",
		      oo->matrix->name, excluded, numItems);
	}
	if (excluded == numItems) {
		omxRaiseErrorf(globalState, "Hessian not positive definite for %lu/%lu items",
			       excluded, numItems);
	}

	return Scale * ll;
}

void ba81SetFreeVarGroup(omxFitFunction *oo, FreeVarGroup *fvg)
{}

static void sandwich(omxFitFunction *oo, FitContext *fc)
{
	const double abScale = fabs(Global->llScale);
	omxExpectation *expectation = oo->expectation;
	BA81FitState *state = (BA81FitState*) oo->argStruct;
	BA81Expect *estate = (BA81Expect*) expectation->argStruct;
	if (estate->verbose) mxLog("%s: sandwich", oo->matrix->name);

	ba81OutcomeProb(estate, FALSE, FALSE);

	const int numThreads = Global->numThreads;
	const int numUnique = estate->numUnique;
	const int numSpecific = estate->numSpecific;
	const int maxDims = estate->maxDims;
	omxData *data = estate->data;
	const int *rowMap = estate->rowMap;
	int *numIdentical = estate->numIdentical;
	const long totalQuadPoints = estate->totalQuadPoints;
	omxMatrix *itemParam = estate->itemParam;
	omxBuffer<double> patternLik(numUnique);

	const int totalOutcomes = estate->totalOutcomes;
	const size_t numItems = estate->itemSpec.size();
	const size_t numParam = fc->varGroup->vars.size();
	const double *wherePrep = estate->wherePrep.data();
	std::vector<double> thrBreadG(numThreads * numParam * numParam);
	std::vector<double> thrBreadH(numThreads * numParam * numParam);
	std::vector<double> thrMeat(numThreads * numParam * numParam);

	if (numSpecific == 0) {
		omxBuffer<double> thrLxk(totalQuadPoints * numThreads);

#pragma omp parallel for num_threads(numThreads)
		for (int px=0; px < numUnique; px++) {
			int thrId = omx_absolute_thread_num();
			double *lxk = thrLxk.data() + thrId * totalQuadPoints;
			omxBuffer<double> itemDeriv(state->itemDerivPadSize);
			omxBuffer<double> expected(totalOutcomes); // can use maxOutcomes instead TODO
			double *breadG = thrBreadG.data() + thrId * numParam * numParam; //a
			double *breadH = thrBreadH.data() + thrId * numParam * numParam; //a
			double *meat = thrMeat.data() + thrId * numParam * numParam;   //b
			std::vector<double> patGrad(numParam);

			ba81LikelihoodSlow2(estate, px, lxk);

			// If patternLik is already valid, maybe could avoid this loop TODO
			double patternLik1 = 0;
			for (long qx=0; qx < totalQuadPoints; qx++) {
				patternLik1 += lxk[qx];
			}
			patternLik[px] = patternLik1;

			// if (!validPatternLik(state, patternLik1))  complain

			double weight = 1 / patternLik[px];
			for (long qx=0; qx < totalQuadPoints; qx++) {
				double tmp = lxk[qx] * weight;
				double sqrtTmp = sqrt(tmp);

				std::vector<double> gradBuf(numParam);
				int gradOffset = 0;

				for (size_t ix=0; ix < numItems; ++ix) {
					if (ix) gradOffset += state->paramPerItem[ix-1];
					int pick = omxIntDataElementUnsafe(data, rowMap[px], ix);
					if (pick == NA_INTEGER) continue;
					pick -= 1;

					const int iOutcomes = estate->itemOutcomes[ix];
					OMXZERO(expected.data(), iOutcomes);
					expected[pick] = 1;
					const double *spec = estate->itemSpec[ix];
					double *iparam = omxMatrixColumn(itemParam, ix);
					const int id = spec[RPF_ISpecID];
					OMXZERO(itemDeriv.data(), state->itemDerivPadSize);
					(*rpf_model[id].dLL1)(spec, iparam, wherePrep + qx * maxDims,
							      expected.data(), itemDeriv.data());
					(*rpf_model[id].dLL2)(spec, iparam, itemDeriv.data());

					for (int par = 0; par < state->paramPerItem[ix]; ++par) {
						int to = state->itemGradMap[gradOffset + par];
						if (to >= 0) {
							gradBuf[to] -= itemDeriv[par] * sqrtTmp;
							patGrad[to] -= itemDeriv[par] * tmp;
						}
					}
					int derivBase = ix * state->itemDerivPadSize;
					for (int ox=0; ox < state->itemDerivPadSize; ox++) {
						int to = state->paramMap[derivBase + ox];
						if (to >= int(numParam)) {
							int Hto = to - numParam;
							breadH[Hto] += abScale * itemDeriv[ox] * tmp * numIdentical[px];
						}
					}
				}
				addSymOuterProd(abScale * numIdentical[px], gradBuf.data(), numParam, breadG);
			}
			addSymOuterProd(abScale * numIdentical[px], patGrad.data(), numParam, meat);
		}

	} else {
		const long totalPrimaryPoints = estate->totalPrimaryPoints;
		const long specificPoints = estate->quadGridSize;
		omxBuffer<double> thrLxk(totalQuadPoints * numSpecific * numThreads);
		omxBuffer<double> thrEi(totalPrimaryPoints * numThreads);
		omxBuffer<double> thrEis(totalPrimaryPoints * numSpecific * numThreads);

#pragma omp parallel for num_threads(numThreads)
		for (int px=0; px < numUnique; px++) {
			int thrId = omx_absolute_thread_num();
			omxBuffer<double> expected(totalOutcomes); // can use maxOutcomes instead TODO
			omxBuffer<double> itemDeriv(state->itemDerivPadSize);
			double *breadG = thrBreadG.data() + thrId * numParam * numParam; //a
			double *breadH = thrBreadH.data() + thrId * numParam * numParam; //a
			double *meat = thrMeat.data() + thrId * numParam * numParam;   //b
			std::vector<double> patGrad(numParam);
			double *lxk = thrLxk.data() + totalQuadPoints * numSpecific * thrId;
			double *Ei = thrEi.data() + totalPrimaryPoints * thrId;
			double *Eis = thrEis.data() + totalPrimaryPoints * numSpecific * thrId;
			cai2010EiEis(estate, px, lxk, Eis, Ei);

			// If patternLik is already valid, maybe could avoid this loop TODO
			double patternLik1 = 0;
			for (long qx=0; qx < totalPrimaryPoints; ++qx) {
				patternLik1 += Ei[qx];
			}
			patternLik[px] = patternLik1;

			// WARNING: I didn't work out the math. I just coded this the way
			// it seems to make sense.
			for (long qloc=0, eisloc=0, qx=0; eisloc < totalPrimaryPoints * numSpecific; eisloc += numSpecific) {
				for (long sx=0; sx < specificPoints; sx++) {
					for (int Sgroup=0; Sgroup < numSpecific; Sgroup++) {
						std::vector<double> gradBuf(numParam);
						int gradOffset = 0;
						double lxk1 = lxk[qloc + Sgroup];
						double Eis1 = Eis[eisloc + Sgroup];
						double tmp = Eis1 * lxk1 / patternLik1;
						double sqrtTmp = sqrt(tmp);
						for (size_t ix=0; ix < numItems; ++ix) {
							if (ix) gradOffset += state->paramPerItem[ix-1];
							if (estate->Sgroup[ix] != Sgroup) continue;
							int pick = omxIntDataElementUnsafe(data, rowMap[px], ix);
							if (pick == NA_INTEGER) continue;
							OMXZERO(expected.data(), estate->itemOutcomes[ix]);
							expected[pick-1] = 1;
							const double *spec = estate->itemSpec[ix];
							double *iparam = omxMatrixColumn(itemParam, ix);
							const int id = spec[RPF_ISpecID];
							OMXZERO(itemDeriv.data(), state->itemDerivPadSize);
							(*rpf_model[id].dLL1)(spec, iparam, wherePrep + qx * maxDims,
									      expected.data(), itemDeriv.data());
							(*rpf_model[id].dLL2)(spec, iparam, itemDeriv.data());

							for (int par = 0; par < state->paramPerItem[ix]; ++par) {
								int to = state->itemGradMap[gradOffset + par];
								if (to >= 0) {
									gradBuf[to] -= itemDeriv[par] * sqrtTmp;
									patGrad[to] -= itemDeriv[par] * tmp;
								}
							}
							int derivBase = ix * state->itemDerivPadSize;
							for (int ox=0; ox < state->itemDerivPadSize; ox++) {
								int to = state->paramMap[derivBase + ox];
								if (to >= int(numParam)) {
									int Hto = to - numParam;
									breadH[Hto] += (abScale * itemDeriv[ox] *
											tmp * numIdentical[px]);
								}
							}
						}
						addSymOuterProd(abScale * numIdentical[px], gradBuf.data(), numParam, breadG);
					}
					qloc += numSpecific;
					++qx;
				}
			}
			addSymOuterProd(abScale * numIdentical[px], patGrad.data(), numParam, meat);
		}
	}

	// only need upper triangle TODO
	for (int tx=1; tx < numThreads; ++tx) {
		double *th = thrBreadG.data() + tx * numParam * numParam;
		for (size_t en=0; en < numParam * numParam; ++en) {
			thrBreadG[en] += th[en];
		}
	}
	for (int tx=1; tx < numThreads; ++tx) {
		double *th = thrBreadH.data() + tx * numParam * numParam;
		for (size_t en=0; en < numParam * numParam; ++en) {
			thrBreadH[en] += th[en];
		}
	}
	for (int tx=1; tx < numThreads; ++tx) {
		double *th = thrMeat.data() + tx * numParam * numParam;
		for (size_t en=0; en < numParam * numParam; ++en) {
			thrMeat[en] += th[en];
		}
	}
	//pda(thrBreadG.data(), numParam, numParam);
	//pda(thrBreadH.data(), numParam, numParam);
	//pda(thrMeat.data(), numParam, numParam);
	if (fc->infoA) {
		for (size_t d1=0; d1 < numParam; ++d1) {
			for (size_t d2=0; d2 < numParam; ++d2) {
				int cell = d1 * numParam + d2;
				fc->infoA[cell] += thrBreadH[cell] - thrBreadG[cell] + thrMeat[cell];
			}
		}
	}
	if (fc->infoB) {
		for (size_t d1=0; d1 < numParam; ++d1) {
			for (size_t d2=0; d2 < numParam; ++d2) {
				int cell = d1 * numParam + d2;
				fc->infoB[cell] += thrMeat[cell];
			}
		}
	}
	fc->sampleSize += data->rows; // remove? TODO
}

static void setLatentStartingValues(omxFitFunction *oo, FitContext *fc)
{
	BA81FitState *state = (BA81FitState*) oo->argStruct;
	BA81Expect *estate = (BA81Expect*) oo->expectation->argStruct;
	std::vector<int> &latentMap = state->latentMap;
	std::vector<double> &ElatentMean = estate->ElatentMean;
	std::vector<double> &ElatentCov = estate->ElatentCov;
	int maxAbilities = estate->maxAbilities;

	if (!estate->Qpoint.size()) return; // if evaluating fit without estimating model
	if (state->ElatentVersion == estate->ElatentVersion) return;

	fc->changedEstimates = true;

	for (int a1 = 0; a1 < maxAbilities; ++a1) {
		if (latentMap[a1] >= 0) {
			int to = latentMap[a1];
			fc->est[to] = ElatentMean[a1];
		}

		for (int a2 = 0; a2 <= a1; ++a2) {
			int to = latentMap[maxAbilities + triangleLoc1(a1) + a2];
			if (to < 0) continue;
			fc->est[to] = ElatentCov[a1 * maxAbilities + a2];
		}
	}

	state->ElatentVersion = estate->ElatentVersion;
	//fc->log("setLatentStartingValues", FF_COMPUTE_ESTIMATE);
}

static void mapLatentDeriv(BA81FitState *state, BA81Expect *estate, double piece,
			   double *derivCoef, double *derivOut)
{
	const int maxAbilities = estate->maxAbilities;
	const int pmax = estate->numSpecific? estate->maxDims - 1 : estate->maxDims;

	int cx = 0;
	for (int d1=0; d1 < pmax; ++d1) {
		double amt1 = piece * derivCoef[d1];
		derivOut[d1] += amt1;
		for (int d2=0; d2 <= d1; ++d2) {
			int to = maxAbilities + cx;
			double amt2 = piece * derivCoef[pmax + cx];
			derivOut[to] += amt2;
			++cx;
		}
	}
}

static void mapLatentDerivS(BA81FitState *state, BA81Expect *estate, int sgroup, double piece,
			    double *derivCoef, double *derivOut)
{
	int maxAbilities = estate->maxAbilities;
	int maxDims = estate->maxDims;
	int pmax = maxDims;
	if (estate->numSpecific) pmax -= 1;

	int sdim = pmax + sgroup;
	double amt3 = piece * derivCoef[0];
	derivOut[sdim] += amt3;

	double amt4 = piece * derivCoef[1];
	int to = maxAbilities + triangleLoc0(sdim);
	derivOut[to] += amt4;
}

static void calcDerivCoef(BA81FitState *state, BA81Expect *estate, omxBuffer<double> &icov,
			  const double *where, double *derivCoef)
{
	omxMatrix *mean = estate->latentMeanOut;
	omxMatrix *cov = estate->latentCovOut;
	const int pDims = estate->numSpecific? estate->maxDims - 1 : estate->maxDims;
	const char R='R';
	const char L='L';
	const char U='U';
	const double alpha = 1;
	const double beta = 0;
	const int one = 1;

	std::vector<double> whereDiff(pDims);
	std::vector<double> whereGram(triangleLoc1(pDims));
	for (int d1=0; d1 < pDims; ++d1) {
		whereDiff[d1] = where[d1] - omxVectorElement(mean, d1);
	}
	gramProduct(whereDiff.data(), whereDiff.size(), whereGram.data());

	F77_CALL(dsymv)(&U, &pDims, &alpha, icov.data(), &pDims, whereDiff.data(), &one,
			&beta, derivCoef, &one);

	std::vector<double> covGrad1(pDims * pDims);
	std::vector<double> covGrad2(pDims * pDims);

	int cx=0;
	for (int d1=0; d1 < pDims; ++d1) {
		for (int d2=0; d2 <= d1; ++d2) {
			covGrad1[d2 * pDims + d1] = omxMatrixElement(cov, d2, d1) - whereGram[cx];
			++cx;
		}
	}

	F77_CALL(dsymm)(&R, &L, &pDims, &pDims, &alpha, covGrad1.data(), &pDims, icov.data(),
			&pDims, &beta, covGrad2.data(), &pDims);
	F77_CALL(dsymm)(&R, &L, &pDims, &pDims, &alpha, icov.data(), &pDims, covGrad2.data(),
			&pDims, &beta, covGrad1.data(), &pDims);

	for (int d1=0; d1 < pDims; ++d1) {
		covGrad1[d1 * pDims + d1] /= 2.0;
	}

	cx = pDims;
	for (int d1=0; d1 < pDims; ++d1) {
		int cell = d1 * pDims;
		for (int d2=0; d2 <= d1; ++d2) {
			derivCoef[cx] = -covGrad1[cell + d2];
			++cx;
		}
	}
}

static void calcDerivCoef1(BA81FitState *state, BA81Expect *estate,
			   const double *where, int sgroup, double *derivCoef)
{
	omxMatrix *mean = estate->latentMeanOut;
	omxMatrix *cov = estate->latentCovOut;
	const int maxDims = estate->maxDims;
	const int specific = maxDims - 1 + sgroup;
	double svar = omxMatrixElement(cov, specific, specific);
	double whereDiff = where[maxDims-1] - omxVectorElement(mean, specific);
	derivCoef[0] = whereDiff / svar;
	derivCoef[1] = -(svar - whereDiff * whereDiff) / (2 * svar * svar);
}

static void xpd_finish_1pat(const double weight, const int numIdentical, const size_t numItems,
			    const int numLatents, const size_t numParam,
			    BA81FitState *state, BA81Expect *estate, omxMatrix *itemParam,
			    std::vector<double> &deriv0, std::vector<double> &latentGrad,
			    const double Scale,
			    std::vector<double> &patGrad,
			    double *grad, double *meat)
{
	int gradOffset = 0;
	for (size_t ix=0; ix < numItems; ++ix) {
		const double *spec = estate->itemSpec[ix];
		double *iparam = omxMatrixColumn(itemParam, ix);
		const int id = spec[RPF_ISpecID];
		double *myDeriv = deriv0.data() + ix * state->itemDerivPadSize;
		(*rpf_model[id].dLL2)(spec, iparam, myDeriv);

		for (int par = 0; par < state->paramPerItem[ix]; ++par) {
			int to = state->itemGradMap[gradOffset];
			if (to >= 0) patGrad[to] -= weight * myDeriv[par];
			++gradOffset;
		}
	}

	for (int lx=0; lx < numLatents; ++lx) {
		int to = state->latentMap[lx];
		if (to >= 0) patGrad[to] += weight * latentGrad[lx];
	}
	for (size_t par=0; par < numParam; ++par) {
		grad[par] += patGrad[par] * Scale * numIdentical;
	}
	addSymOuterProd(fabs(Scale) * numIdentical, patGrad.data(), numParam, meat);
}

static bool xpd(omxFitFunction *oo, FitContext *fc)
{
	const double Scale = Global->llScale;
	omxExpectation *expectation = oo->expectation;
	BA81FitState *state = (BA81FitState*) oo->argStruct;
	BA81Expect *estate = (BA81Expect*) expectation->argStruct;
	if (estate->verbose) mxLog("%s: cross product approximation", oo->matrix->name);

	if (fc->infoMethod == INFO_METHOD_HESSIAN) {
		if (state->freeLatents) {
			omxRaiseErrorf(globalState, "Hessian not available with free latents");
			return FALSE;
		}

		ba81ComputeEMFit(oo, FF_COMPUTE_HESSIAN, fc);
		return TRUE;
	}

	if (fc->infoMethod != INFO_METHOD_MEAT) {
		omxRaiseErrorf(globalState, "Information matrix approximation method %d is not available",
			       fc->infoMethod);
		return FALSE;
	}

	ba81OutcomeProb(estate, FALSE, FALSE);

	const int numThreads = Global->numThreads;
	const int numUnique = estate->numUnique;
	const int numSpecific = estate->numSpecific;
	const int maxDims = estate->maxDims;
	const int pDims = numSpecific? maxDims-1 : maxDims;
	const int maxAbilities = estate->maxAbilities;
	omxMatrix *cov = estate->latentCovOut;
	omxData *data = estate->data;
	const int *rowMap = estate->rowMap;
	int *numIdentical = estate->numIdentical;
	const long totalQuadPoints = estate->totalQuadPoints;
	omxMatrix *itemParam = estate->itemParam;
	omxBuffer<double> patternLik(numUnique);

	omxBuffer<double> icovBuffer(pDims * pDims);
	for (int d1=0; d1 < pDims; ++d1) {
		for (int d2=0; d2 < pDims; ++d2) {
			icovBuffer[d1 * pDims + d2] = omxMatrixElement(cov, d1, d2);
		}
	}
	Matrix icovMat(icovBuffer.data(), pDims, pDims);
	int info = InvertSymmetricPosDef(icovMat, 'U');
	if (info) return FALSE;

	// fill in rest from upper triangle
	for (int rx=1; rx < pDims; ++rx) {
		for (int cx=0; cx < rx; ++cx) {
			icovBuffer[cx * pDims + rx] = icovBuffer[rx * pDims + cx];
		}
	}

	const int priDerivCoef = pDims + triangleLoc1(pDims);
	const int numLatents = maxAbilities + triangleLoc1(maxAbilities);
	const int thrDerivSize = itemParam->cols * state->itemDerivPadSize;
	const int totalOutcomes = estate->totalOutcomes;
	const size_t numItems = estate->itemSpec.size();
	const size_t numParam = fc->varGroup->vars.size();
	std::vector<double> thrGrad(numThreads * numParam);
	std::vector<double> thrMeat(numThreads * numParam * numParam);
	const double *wherePrep = estate->wherePrep.data();

	if (numSpecific == 0) {
		omxBuffer<double> thrLxk(totalQuadPoints * numThreads);
		omxBuffer<double> derivCoef(totalQuadPoints * priDerivCoef);

#pragma omp parallel for num_threads(numThreads)
		for (long qx=0; qx < totalQuadPoints; qx++) {
			const double *where = wherePrep + qx * maxDims;
			calcDerivCoef(state, estate, icovBuffer, where,
				      derivCoef.data() + qx * priDerivCoef);
		}

#pragma omp parallel for num_threads(numThreads)
		for (int px=0; px < numUnique; px++) {
			int thrId = omx_absolute_thread_num();
			double *lxk = thrLxk.data() + thrId * totalQuadPoints;
			omxBuffer<double> expected(totalOutcomes); // can use maxOutcomes instead TODO
			std::vector<double> deriv0(thrDerivSize);
			std::vector<double> latentGrad(numLatents);
			std::vector<double> patGrad(numParam);
			double *grad = thrGrad.data() + thrId * numParam;
			double *meat = thrMeat.data() + thrId * numParam * numParam;
			ba81LikelihoodSlow2(estate, px, lxk);

			// If patternLik is already valid, maybe could avoid this loop TODO
			double patternLik1 = 0;
			for (long qx=0; qx < totalQuadPoints; qx++) {
				patternLik1 += lxk[qx];
			}
			patternLik[px] = patternLik1;

			// if (!validPatternLik(state, patternLik1))  complain, TODO

			for (long qx=0; qx < totalQuadPoints; qx++) {
				double tmp = lxk[qx];
				mapLatentDeriv(state, estate, tmp, derivCoef.data() + qx * priDerivCoef,
					       latentGrad.data());

				for (size_t ix=0; ix < numItems; ++ix) {
					int pick = omxIntDataElementUnsafe(data, rowMap[px], ix);
					if (pick == NA_INTEGER) continue;
					OMXZERO(expected.data(), estate->itemOutcomes[ix]);
					expected[pick-1] = tmp;
					const double *spec = estate->itemSpec[ix];
					double *iparam = omxMatrixColumn(itemParam, ix);
					const int id = spec[RPF_ISpecID];
					double *myDeriv = deriv0.data() + ix * state->itemDerivPadSize;
					(*rpf_model[id].dLL1)(spec, iparam, wherePrep + qx * maxDims,
							      expected.data(), myDeriv);
				}
			}

			xpd_finish_1pat(1 / patternLik1, numIdentical[px], numItems, numLatents, numParam,
					state, estate, itemParam, deriv0, latentGrad, Scale, patGrad, grad, meat);
		}
	} else {
		const long totalPrimaryPoints = estate->totalPrimaryPoints;
		const long specificPoints = estate->quadGridSize;
		omxBuffer<double> thrLxk(totalQuadPoints * numSpecific * numThreads);
		omxBuffer<double> thrEi(totalPrimaryPoints * numThreads);
		omxBuffer<double> thrEis(totalPrimaryPoints * numSpecific * numThreads);
		const int derivPerPoint = priDerivCoef + 2 * numSpecific;
		omxBuffer<double> derivCoef(totalQuadPoints * derivPerPoint);

#pragma omp parallel for num_threads(numThreads)
		for (long qx=0; qx < totalQuadPoints; qx++) {
			const double *where = wherePrep + qx * maxDims;
			calcDerivCoef(state, estate, icovBuffer, where,
				      derivCoef.data() + qx * derivPerPoint);
			for (int Sgroup=0; Sgroup < numSpecific; ++Sgroup) {
				calcDerivCoef1(state, estate, where, Sgroup,
					       derivCoef.data() + qx * derivPerPoint + priDerivCoef + 2 * Sgroup);
			}
		}

#pragma omp parallel for num_threads(numThreads)
		for (int px=0; px < numUnique; px++) {
			int thrId = omx_absolute_thread_num();
			double *lxk = thrLxk.data() + totalQuadPoints * numSpecific * thrId;
			double *Ei = thrEi.data() + totalPrimaryPoints * thrId;
			double *Eis = thrEis.data() + totalPrimaryPoints * numSpecific * thrId;
			omxBuffer<double> expected(totalOutcomes); // can use maxOutcomes instead TODO
			std::vector<double> deriv0(thrDerivSize);
			std::vector<double> latentGrad(numLatents);
			std::vector<double> patGrad(numParam);
			double *grad = thrGrad.data() + thrId * numParam;
			double *meat = thrMeat.data() + thrId * numParam * numParam;
			cai2010EiEis(estate, px, lxk, Eis, Ei);

			for (long qloc=0, eisloc=0, qx=0; eisloc < totalPrimaryPoints * numSpecific; eisloc += numSpecific) {
				for (long sx=0; sx < specificPoints; sx++) {
					mapLatentDeriv(state, estate, Eis[eisloc] * lxk[qloc],
						       derivCoef.data() + qx * derivPerPoint,
						       latentGrad.data());

					for (int Sgroup=0; Sgroup < numSpecific; Sgroup++) {
						double lxk1 = lxk[qloc];
						double Eis1 = Eis[eisloc + Sgroup];
						double tmp = Eis1 * lxk1;
						mapLatentDerivS(state, estate, Sgroup, tmp,
								derivCoef.data() + qx * derivPerPoint + priDerivCoef + 2 * Sgroup,
								latentGrad.data());

						for (size_t ix=0; ix < numItems; ++ix) {
							if (estate->Sgroup[ix] != Sgroup) continue;
							int pick = omxIntDataElementUnsafe(data, rowMap[px], ix);
							if (pick == NA_INTEGER) continue;
							OMXZERO(expected.data(), estate->itemOutcomes[ix]);
							expected[pick-1] = tmp;
							const double *spec = estate->itemSpec[ix];
							double *iparam = omxMatrixColumn(itemParam, ix);
							const int id = spec[RPF_ISpecID];
							double *myDeriv = deriv0.data() + ix * state->itemDerivPadSize;
							(*rpf_model[id].dLL1)(spec, iparam, wherePrep + qx * maxDims,
									      expected.data(), myDeriv);
						}
						++qloc;
					}
					++qx;
				}
			}

			// If patternLik is already valid, maybe could avoid this loop TODO
			double patternLik1 = 0;
			for (long qx=0; qx < totalPrimaryPoints; ++qx) {
				patternLik1 += Ei[qx];
			}
			patternLik[px] = patternLik1;

			xpd_finish_1pat(1 / patternLik1, numIdentical[px], numItems, numLatents, numParam,
					state, estate, itemParam, deriv0, latentGrad, Scale, patGrad, grad, meat);
		}
	}

	for (int tx=1; tx < numThreads; ++tx) {
		double *th = thrGrad.data() + tx * numParam;
		for (size_t en=0; en < numParam; ++en) {
			thrGrad[en] += th[en];
		}
	}
	for (int tx=1; tx < numThreads; ++tx) {
		double *th = thrMeat.data() + tx * numParam * numParam;
		for (size_t en=0; en < numParam * numParam; ++en) {
			thrMeat[en] += th[en];
		}
	}
	for (size_t d1=0; d1 < numParam; ++d1) {
		fc->grad[d1] += thrGrad[d1];
		for (size_t d2=0; d2 < numParam; ++d2) {
			int cell = d1 * numParam + d2;
			fc->infoB[cell] += thrMeat[cell];
		}
	}

	return TRUE;
}

static double
ba81ComputeFit(omxFitFunction* oo, int want, FitContext *fc)
{
	BA81FitState *state = (BA81FitState*) oo->argStruct;
	BA81Expect *estate = (BA81Expect*) oo->expectation->argStruct;

	if (estate->type == EXPECTATION_AUGMENTED) {
		buildItemParamMap(oo, fc);

		if (want & FF_COMPUTE_PARAMFLAVOR) {
			for (size_t px=0; px < state->numFreeParam; ++px) {
				if (state->paramFlavor[px] < 0) continue;
				fc->flavor[px] = state->paramFlavor[px];
			}
			return 0;
		}

		if (want & FF_COMPUTE_HGPROD) {
			for (size_t px=0; px < state->hgProd.size(); ++px) {
				fc->hgProd.push_back(state->hgProd[px]);
			}
			return 0;
		}

		if (want & FF_COMPUTE_PREOPTIMIZE) {
			omxExpectationCompute(oo->expectation, NULL);
			// schilling_bock_2005_rescale(oo, fc); seems counterproductive
			return 0;
		}

		if (want & FF_COMPUTE_INFO) {
			buildLatentParamMap(oo, fc);
			buildItemParamMap(oo, fc);
			ba81SetupQuadrature(oo->expectation);
			if (!xpd(oo, fc)) {
				return INFINITY;
			}
			return 0;
		}

		double got = ba81ComputeEMFit(oo, want, fc);
		return got;
	} else if (estate->type == EXPECTATION_OBSERVED) {

		if (want & FF_COMPUTE_PREOPTIMIZE) {
			buildLatentParamMap(oo, fc);
			if (state->freeLatents) {
				setLatentStartingValues(oo, fc);
			}
			return 0;
		}

		if (want & FF_COMPUTE_INFO) {
			buildLatentParamMap(oo, fc); // only to check state->freeLatents
			buildItemParamMap(oo, fc);

			if (state->freeLatents) {
				omxRaiseErrorf(globalState, "Cannot approximate latent parameter gradients");
			} else {
				ba81SetupQuadrature(oo->expectation);
				sandwich(oo, fc);
			}
		}
		if (want & FF_COMPUTE_HESSIAN) {
			warning("%s: Hessian not available", oo->matrix->name);
		}

		if (want & FF_COMPUTE_MAXABSCHANGE) {
			double mac = std::max(omxMaxAbsDiff(state->itemParam, estate->itemParam),
					      omxMaxAbsDiff(state->latentMean, estate->latentMeanOut));
			fc->mac = std::max(mac, omxMaxAbsDiff(state->latentCov, estate->latentCovOut));
			state->copyEstimates(estate);
		}

		if (want & FF_COMPUTE_FIT) {
			omxExpectationCompute(oo->expectation, NULL);

			double *patternLik = estate->patternLik;
			int *numIdentical = estate->numIdentical;
			int numUnique = estate->numUnique;
			estate->excludedPatterns = 0;
			const double LogLargest = estate->LogLargestDouble;
			double got = 0;
#pragma omp parallel for num_threads(Global->numThreads) reduction(+:got)
			for (int ux=0; ux < numUnique; ux++) {
				if (!validPatternLik(estate, patternLik[ux])) {
#pragma omp atomic
					++estate->excludedPatterns;
					// somehow indicate that this -2LL is provisional TODO
					continue;
				}
				got += numIdentical[ux] * (log(patternLik[ux]) - LogLargest);
			}
			if (estate->verbose) mxLog("%s: fit (%d/%d excluded)",
						   oo->matrix->name, estate->excludedPatterns, numUnique);
			//mxLog("fit %.4f", -2 * got);
			return Global->llScale * got;
		}

		return 0;
	} else {
		error("%s: Select EM or regular mode before computing %d", oo->matrix->name, want);
	}
}

static void ba81Compute(omxFitFunction *oo, int want, FitContext *fc)
{
	if (!want) return;
	double got = ba81ComputeFit(oo, want, fc);
	if (got) oo->matrix->data[0] = got;
}

BA81FitState::~BA81FitState()
{
	omxFreeAllMatrixData(itemParam);
	omxFreeAllMatrixData(latentMean);
	omxFreeAllMatrixData(latentCov);
}

static void ba81Destroy(omxFitFunction *oo) {
	BA81FitState *state = (BA81FitState *) oo->argStruct;
	delete state;
}

void omxInitFitFunctionBA81(omxFitFunction* oo)
{
	if (!oo->argStruct) { // ugh!
		BA81FitState *state = new BA81FitState;
		oo->argStruct = state;
	}

	BA81FitState *state = (BA81FitState*) oo->argStruct;

	omxExpectation *expectation = oo->expectation;
	BA81Expect *estate = (BA81Expect*) expectation->argStruct;

	//newObj->data = oo->expectation->data;

	oo->computeFun = ba81Compute;
	oo->setVarGroup = ba81SetFreeVarGroup;
	oo->destructFun = ba81Destroy;
	oo->gradientAvailable = TRUE;
	oo->hessianAvailable = TRUE;
	oo->parametersHaveFlavor = TRUE;

	int maxParam = estate->itemParam->rows;
	state->itemDerivPadSize = maxParam + triangleLoc1(maxParam);

	int numItems = estate->itemParam->cols;
	for (int ix=0; ix < numItems; ix++) {
		const double *spec = estate->itemSpec[ix];
		int id = spec[RPF_ISpecID];
		if (id < 0 || id >= rpf_numModels) {
			error("ItemSpec %d has unknown item model %d", ix, id);
		}
	}

	state->itemParam = omxInitMatrix(NULL, 0, 0, TRUE, globalState);
	state->latentMean = omxInitMatrix(NULL, 0, 0, TRUE, globalState);
	state->latentCov = omxInitMatrix(NULL, 0, 0, TRUE, globalState);
	state->copyEstimates(estate);
	state->ElatentVersion = -1;
}