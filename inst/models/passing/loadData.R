library(OpenMx)
set.seed(1)

if (mxOption(NULL,"Default optimizer") == 'NPSOL') stop("SKIP")
#mxOption(NULL, "Number of Threads", 1L)

jointData <- suppressWarnings(try(read.table("models/passing/data/jointdata.txt", header=TRUE), silent=TRUE))
jointData <- read.table("data/jointdata.txt", header=TRUE)

# specify ordinal columns as ordered factors
jointData[,c(2,4,5)] <- mxFactor(jointData[,c(2,4,5)], 
	levels=list(c(0,1), c(0, 1, 2, 3), c(0, 1, 2)))
	
satCov <- mxMatrix("Symm", 5, 5,
	free=TRUE, values=diag(5), name="C")
satCov$free[2,2] <- FALSE
satCov$free[4,4] <- FALSE
satCov$free[5,5] <- FALSE

loadings <- mxMatrix("Full", 1, 5,
	free=TRUE, values=1, name="L", lbound=0)
loadings$ubound[1,4] <- 2
loadings$ubound[1,5] <- 2
	
resid <- mxMatrix("Diag", 5, 5,
	free=c(TRUE, FALSE, TRUE, FALSE, FALSE), values=.5, name="U")
	
means <- mxMatrix("Full", 1, 5,
	free=c(TRUE, FALSE, TRUE, FALSE, FALSE), values=0, name="M")
	
thresh <- mxMatrix("Full", 3, 3, FALSE, 0, name="T")

thresh$free[,1] <- c(TRUE, FALSE, FALSE)
thresh$values[,1] <- c(0, NA, NA)
thresh$labels[,1] <- c("z2t1", NA, NA)

thresh$free[,2] <- TRUE
thresh$values[,2] <- c(-1, 0, 1)
thresh$labels[,2] <- c("z4t1", "z4t2", "z4t3")

thresh$free[,3] <- c(TRUE, TRUE, FALSE)
thresh$values[,3] <- c(-1, 1, NA)
thresh$labels[,3] <- c("z5t1", "z5t2", NA)

model1 <- mxModel("loadData",
	loadings, resid, means, thresh,
	mxAlgebra(t(L) %*% L + U, name="C"),
	mxFitFunctionWLS(),
	mxExpectationNormal("C", "M",
		dimnames=names(jointData)[1:5],
		thresholds="T",
		threshnames=c("z2", "z4", "z5")))

result1 <- c()
numSets <- 8
dsets <- list()
for (dx in 1:numSets) {
  df <- data.frame(z1=jointData$z1 + rnorm(nrow(jointData), mean=dx/10, sd=.1),
                   z2=ordered(sample.int(2, 10, replace=TRUE)-1L))
  dsets[[dx]] <- df
  for (cx in paste0('z',3:5)) df[[cx]] <- jointData[[cx]]
  model2 <- mxModel(model1, mxData(df, 'raw'), mxFitFunctionWLS())
  model2 <- mxRun(model2)
  result1 <- rbind(result1, c(coef(model2), model2$output$standardErrors))
}
flat <- as.data.frame(unlist(dsets, recursive=FALSE))

tdir <- paste0(tempdir(), "/")
write.table(flat, file=paste0(tdir, "testCols.csv"),
            quote=FALSE, row.names = FALSE, col.names=FALSE)

shuffle <- rev(1:numSets)
model3 <- mxModel(
  model1,
  mxData(jointData, 'raw'),
  mxComputeLoop(list(
    LD=mxComputeLoadData(
      'loadData',
      column=paste0('z',1:2),
      path=paste0(tdir, "testCols.csv"),
      byrow=FALSE, verbose=0L),
    mxComputeSetOriginalStarts(),
    mxComputeGradientDescent(),
    mxComputeStandardError(),
    CPT=mxComputeCheckpoint(toReturn=TRUE, standardErrors = TRUE)
  ), i=shuffle))

model3Fit <- mxRun(model3)

omxCheckEquals(model3Fit$compute$steps[['LD']]$debug$loadCounter, 2L)

discardCols <- c("OpenMxEvals", "iterations", "timestamp", "MxComputeLoop1", "objective", "statusCode")
thr <- c(7, 8, 7, 7, 8, 8, 7, 8, 8, 7, 7, 8, 7, 7, 4, 8, 8, 8, 9, 8,
         9, 9, 11, 10, 10, 9, 9, 8, 10, 10) - 2

log <- model3Fit$compute$steps[['CPT']]$log
for (col in discardCols) log[[col]] <- NULL
lmad <- -log10(apply(abs(as.matrix(log[shuffle,] - result1)), 2, max))
# names(lmad) <- c()
# cat(deparse(floor(lmad)))
print(lmad - thr)
omxCheckTrue(all(lmad - thr > 0))

model3$compute$steps[['LD']]$cacheSize <- 1L
model3Fit <- mxRun(model3)

omxCheckEquals(model3Fit$compute$steps[['LD']]$debug$loadCounter, 8L)

log <- model3Fit$compute$steps[['CPT']]$log
for (col in discardCols) log[[col]] <- NULL
lmad <- -log10(apply(abs(as.matrix(log[shuffle,] - result1)), 2, max))
omxCheckTrue(all(lmad - thr > 0))
