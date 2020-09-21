#include "gtest/gtest.h"
#include <DPGO/QuadraticProblem.h>
#include <DPGO/PGOAgent.h>
#include <DPGO/DPGO_types.h>
#include <DPGO/DPGO_utils.h>

using namespace DPGO;

TEST(testDPGO, OptimizationThreadBasic)
{
    unsigned int d,r;
    d = 3;
    r = 3;
    ROPTALG algorithm = ROPTALG::RTR;
    bool verbose = false;
    PGOAgentParameters options(d,r,algorithm,verbose);

    PGOAgent agent(0, options);

    ASSERT_FALSE(agent.isOptimizationRunning());

    for(unsigned trial = 0; trial < 3; ++trial){
    	agent.startOptimizationLoop(5.0);
	    sleep(1);
	    ASSERT_TRUE(agent.isOptimizationRunning());
	    agent.endOptimizationLoop();
	    ASSERT_FALSE(agent.isOptimizationRunning());
    }
}


TEST(testDPGO, OptimizationThreadTriangleGraph)
{
    unsigned int id = 1;
    unsigned int d,r;
    d = 3;
    r = 3;
    ROPTALG algorithm = ROPTALG::RTR;
    bool verbose = false;
    PGOAgentParameters options(d,r,algorithm,verbose);
    PGOAgent agent(id, options);

    Matrix Tw0(d+1, d+1);
    Tw0 <<  1,    0,    0,  0, 
            0,    1,    0,  0, 
            0,    0,    1,  0, 
            0,    0,    0,  1;

    Matrix Tw1(d+1, d+1);
    Tw1 <<  0.1436,    0.7406,    0.6564,  1, 
           -0.8179,   -0.2845,    0.5000,  1, 
            0.5571,   -0.6087,    0.5649,  1, 
            0     ,         0,         0,  1;
    
    Matrix Tw2(d+1, d+1);
    Tw2 << -0.4069,   -0.4150,   -0.8138,  2,
            0.4049,    0.7166,   -0.5679,  2,
            0.8188,   -0.5606,   -0.1236,  2,
                 0,         0,         0,  1;

    Matrix Ttrue(d, 3*(d+1));
    Ttrue << 1,    0,    0,  0,   0.1436,    0.7406,    0.6564,  1,   -0.4069,   -0.4150,   -0.8138,  2,
             0,    1,    0,  0,  -0.8179,   -0.2845,    0.5000,  1,    0.4049,    0.7166,   -0.5679,  2,
             0,    0,    1,  0,   0.5571,   -0.6087,    0.5649,  1,    0.8188,   -0.5606,   -0.1236,  2;
    
    Matrix dT;
    dT = Tw0.inverse() * Tw1;
    RelativeSEMeasurement m01(id, id, 0, 1, dT.block(0,0,d,d), dT.block(0,d,d,1), 1.0, 1.0);
    agent.addOdometry(m01);

    dT = Tw1.inverse() * Tw2;
    RelativeSEMeasurement m12(id, id, 1, 2, dT.block(0,0,d,d), dT.block(0,d,d,1), 1.0, 1.0);
    agent.addOdometry(m12);

    dT = Tw0.inverse() * Tw2;
    RelativeSEMeasurement m02(id, id, 0, 2, dT.block(0,0,d,d), dT.block(0,d,d,1), 1.0, 1.0);
    agent.addPrivateLoopClosure(m02);
    
    Matrix Testimated;
    Testimated = agent.getTrajectoryInLocalFrame();
    ASSERT_LE((Ttrue - Testimated).norm(), 1e-4);

    agent.startOptimizationLoop(5.0);
    sleep(3);
    agent.endOptimizationLoop();

    ASSERT_EQ(agent.getID(), id);
    ASSERT_EQ(agent.getCluster(), id);
    ASSERT_EQ(agent.num_poses(), 3);
    ASSERT_EQ(agent.dimension(), d);
    ASSERT_EQ(agent.relaxation_rank(), r);

    Testimated = agent.getTrajectoryInLocalFrame();
    ASSERT_LE((Ttrue - Testimated).norm(), 1e-4);
}