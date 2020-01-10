#include <iostream>
#include <unistd.h>
#include <random>
#include <Eigen/SVD>
#include "multithread/RGDWorker.h"


using namespace std;
using namespace SESync;


namespace AsynchPGO{

	RGDWorker::RGDWorker(RGDMaster* pMaster, unsigned pId){
		id = pId;
		master = pMaster;
		mFinishRequested = false;
		mFinished = false;

		d = master->problem->dimension();
		r = master->problem->relaxation_rank();

		manifold = new CartanSyncManifold(r,d,1);
		x = new CartanSyncVariable(r,d,1);
		xNext = new CartanSyncVariable(r,d,1);
		euclideanGradient = new CartanSyncVector(r,d,1);
		riemannianGradient = new CartanSyncVector(r,d,1);
		descentVector = new CartanSyncVector(r,d,1);

		cout << "Worker " << id << " initialized. "<< endl;

		
	}

	void RGDWorker::run(){
		std::random_device rd;  //Will be used to obtain a seed for the random number engine
	    std::mt19937 rng(rd()); //Standard mersenne_twister_engine seeded with rd()
	    std::uniform_int_distribution<> distribution(0, updateIndices.size()-1);

		while(true){

			// randomly select an index
			unsigned i = updateIndices[distribution(rng)];

			Matrix Yi, Gi, YiNext;
			readComponent(i, Yi);

			Gi.resize(Yi.rows(), Yi.cols());
			computeEuclideanGradient(i, Gi);

			gradientUpdate(Yi, Gi, YiNext);

			// writeComponent(i, YiNext);

			if(mFinishRequested) break;
			
			// use usleep for microsecond
			usleep(1000); 
		}

		mFinished = true;
		cout << "Worker " << id << " finished." << endl;
	}

	void RGDWorker::requestFinish(){
		mFinishRequested = true;
	}

	bool RGDWorker::isFinished(){
		return mFinished;
	}

	void RGDWorker::readComponent(unsigned i, Matrix& Yi){
		// obtain lock
		lock_guard<mutex> lock(master->mUpdateMutexes[i]);
		master->readComponent(i, Yi);
	}

    void RGDWorker::writeComponent(unsigned i, Matrix& Yi){
    	// obtain lock
		lock_guard<mutex> lock(master->mUpdateMutexes[i]);
		master->writeComponent(i, Yi);
    }

    void RGDWorker::computeEuclideanGradient(unsigned i, Matrix &Gi){
    	Gi.setZero();
    	// iterate over neighbors of i
    	for(unsigned k = 0; k < master->adjList[i].size(); ++k){
    		unsigned j = master->adjList[i][k];
    		Matrix Yj, Qji;
    		master->readComponent(j, Yj);
    		master->readDataMatrixBlock(j, i, Qji);
    		Gi = Gi + Yj * Qji;
    	}
    }

    void RGDWorker::gradientUpdate(Matrix& Yi, Matrix& Gi, Matrix& YiNext){
    	YiNext.setZero();
    	
    	// This does not have memory leak
    	// ROPTLIB::StieVector sv(r,d);
    	// ROPTLIB::EucVector ev(r);
    	// ROPTLIB::ProductElement elem(2,&sv,1,&ev,1);

    	// This does have memory leak
    	Mat2CartanProd(Yi, *x);
    	Mat2CartanProd(Gi, *euclideanGradient);
    	
    	// Compute Riemannian gradient
    	manifold->Projection(x, euclideanGradient, riemannianGradient);

    	// Debug
    	// Matrix RG;
    	// CartanProd2Mat(riemannianGradient, RG);
    	// cout << RG.norm() << endl;

    	// Compute descent direction
    	manifold->ScaleTimesVector(x, -0.001, riemannianGradient, descentVector);

    	// Update
    	manifold->Retraction(x, descentVector, xNext);
    	CartanProd2Mat(*xNext, YiNext);
    }
}