/** \file threads_async_tests.cpp
 * \brief Implementation of tests related to multi-threaded asynchronous preconditioning
 * \author Aditya Kashi
 * \date 2018-03
 */

#include <iostream>
#include <iomanip>
#include <string>
#include <omp.h>
#include <petscksp.h>

#include "../src/alinalg.hpp"
#include "../src/autilities.hpp"
#include "../src/aoutput.hpp"
#include "../src/aodesolver.hpp"
#include "../src/afactory.hpp"
#include "../src/ameshutils.hpp"

#include <blasted_petsc.h>

#include "threads_async_tests.hpp"

namespace benchmark {

using namespace acfd;

StatusCode test_speedup_sweeps(const FlowParseOptions& opts, const int numthreads,
		const std::vector<int>& sweep_seq, std::ofstream& outfile)
{
	StatusCode ierr = 0;
	
	// Read control file
	const FlowParserOptions opts = parse_flow_controlfile(argc, argv);

	// Set up mesh
	UMesh2dh m;
	m.readMesh(opts.meshfile);
	CHKERRQ(preprocessMesh(m));
	m.compute_periodic_map(opts.periodic_marker, opts.periodic_axis);
	std::cout << "\n***\n";

	// physical configuration
	const FlowPhysicsConfig pconf = extract_spatial_physics_config(opts);
	// numerics for main solver
	const FlowNumericsConfig nconfmain = extract_spatial_numerica_config(opts);
	// simpler numerics for startup
	const FlowNumericsConfig nconfstart {opts.invflux, opts.invfluxjac, "NONE", "NONE", false};

	std::cout << "Setting up main spatial scheme.\n";
	const Spatial<NVARS> *const prob = create_const_flowSpatialDiscretization(&m, pconf, nconfmain);
	std::cout << "\nSetting up spatial scheme for the initial guess.\n";
	const Spatial<NVARS> *const startprob
		= create_const_flowSpatialDiscretization(&m, pconf, nconfstart);
	std::cout << "\n***\n";

	// solution vector
	Vec u;

	// Initialize Jacobian for implicit schemes
	Mat M;
	ierr = setupSystemMatrix<NVARS>(&m, &M); CHKERRQ(ierr);
	ierr = MatCreateVecs(M, &u, NULL); CHKERRQ(ierr);

	// setup matrix-free Jacobian if requested
	Mat A;
	MatrixFreeSpatialJacobian<NVARS> mfjac;
	PetscBool mf_flg = PETSC_FALSE;
	ierr = PetscOptionsHasName(NULL, NULL, "-matrix_free_jacobian", &mf_flg); CHKERRQ(ierr);
	if(mf_flg) {
		std::cout << " Allocating matrix-free Jac\n";
		ierr = setup_matrixfree_jacobian<NVARS>(&m, &mfjac, &A); 
		CHKERRQ(ierr);
	}

	// initialize solver
	KSP ksp;
	ierr = KSPCreate(PETSC_COMM_WORLD, &ksp); CHKERRQ(ierr);
	if(mf_flg) {
		ierr = KSPSetOperators(ksp, A, M); 
		CHKERRQ(ierr);
	}
	else {
		ierr = KSPSetOperators(ksp, M, M); 
		CHKERRQ(ierr);
	}
	ierr = KSPSetFromOptions(ksp); CHKERRQ(ierr);

	// set up time discrization

	const SteadySolverConfig maintconf {
		opts.lognres, opts.logfile+".tlog",
		opts.initcfl, opts.endcfl, opts.rampstart, opts.rampend,
		opts.tolerance, opts.maxiter,
	};

	const SteadySolverConfig starttconf {
		opts.lognres, opts.logfile+"-init.tlog",
		opts.firstinitcfl, opts.firstendcfl, opts.firstrampstart, opts.firstrampend,
		opts.firsttolerance, opts.firstmaxiter,
	};

	SteadySolver<NVARS> * starttime=nullptr, * time=nullptr;

	if(opts.usestarter != 0) {
		starttime = new SteadyBackwardEulerSolver<NVARS>(startprob, starttconf, ksp);
		std::cout << "Set up backward Euler temporal scheme for initialization solve.\n";
	}

	// Ask the spatial discretization context to initialize flow variables
	startprob->initializeUnknowns(u);

	// setup BLASTed preconditioning
	Blasted_data bctx = newBlastedDataContext();
	ierr = setup_blasted<NVARS>(ksp,u,startprob,bctx); CHKERRQ(ierr);

	std::cout << "\n***\n";

	// starting computation
	if(opts.usestarter != 0) {

		mfjac.set_spatial(startprob);

		// solve the starter problem to get the initial solution
		ierr = starttime->solve(u); CHKERRQ(ierr);
	}

	// Benchmarking runs

	if(mpirank == 0) {
		outf << std::setw(10) << "# num-cells = " << m.gnelem() << "\n# "
			<< std::setw(6) << "sweeps " << std::setw(10) << "wall-time " 
			<< std::setw(10) << "cpu-time " 
			<< std::setw(10) << "total-lin-iters " << std::setw(10) << "avg-lin-iters "
			<< std::setw(10) << " time-steps\n";
	}

	omp_set_num_threads(1);

	TimingData tdata = run_sweeps(startprob, prob, maintconf, sweep_seq, ksp, u, A, M, bctx);
	
	if(mpirank == 0) {
			outf << std::setw(6) << "sweeps " << std::setw(10) << "wall-time " 
			<< std::setw(10) << "cpu-time " 
			<< std::setw(10) << "total-lin-iters " << std::setw(10) << "avg-lin-iters "
			<< std::setw(10) << " time-steps\n";
	}

	omp_set_num_threads(numthreads);
	
	// Carry out multi-thread run
	for (const int nswp : sweep_seq)
	{
		TimingData tdata = run_sweeps(startprob, prob, maintconf, nswp, ksp, u, A, M, bctx);
	}

	delete time;
	delete starttime;
	delete prob;
	delete startprob;
	
	return ierr;
}

TimingData run_sweeps(const Spatial<NVARS> *const startprob, const Spatial<NVARS> *const prob,
		const SteadySolverConfig& maintconf, const int nswps,
		KSP ksp, Vec u, Mat A, Mat M, Blasted_data& bctx)
{
	// add options
	std::string value = std::to_string(nswps) + "," + std::to_string(nswps);
	ierr = PetscOptionsSetValue(NULL, "-blasted_async_sweeps", value.c_str());
	petsc_throw(ierr, "run_sweeps: Couldn't set PETSc option for sweeps");

	// Check
	int checksweeps[2];
	PetscBool set = PETSC_FALSE;
	ierr = PetscOptionsGetIntArray(NULL,NULL,"-blasted_async_sweeps",checksweeps,&nmax,&set);
	fvens_throw(checksweeps[0] != nswps || checksweeps[1] != nswps, 
			"run_sweeps: Async sweeps not set properly!");
	
	// Reset the KSP
	ierr = KSPDestroy(&ksp); petsc_throw(ierr, "run_sweeps: Couldn't destroy KSP");
	ierr = KSPCreate(PETSC_COMM_WORLD, &ksp); petsc_throw(ierr, "run_sweeps: Couldn't create KSP");
	if(mf_flg) {
		ierr = KSPSetOperators(ksp, A, M); 
		petsc_throw(ierr, "run_sweeps: Couldn't set KSP operators");
	}
	else {
		ierr = KSPSetOperators(ksp, M, M); 
		petsc_throw(ierr, "run_sweeps: Couldn't set KSP operators");
	}
	ierr = KSPSetFromOptions(ksp); petsc_throw(ierr, "run_sweeps: Couldn't set KSP from options");
	
	bctx = newBlastedDataContext();
	ierr = setup_blasted<NVARS>(ksp,u,startprob,bctx);
	fvens_throw(ierr, "run_sweeps: Couldn't setup BLASTed");

	// setup nonlinear ODE solver for main solve
	SteadyBackwardEulerSolver<NVARS>* time = new SteadyBackwardEulerSolver<NVARS>(prob, maintconf, ksp);
	std::cout << " Set up backward Euler temporal scheme for main solve.\n";

	mfjac.set_spatial(prob);

	Vec ut;
	ierr = MatCreateVecs(M, &ut, NULL); petsc_throw(ierr, "Line 194: Couldn't create vec");
	ierr = VecCopy(u, ut); petsc_throw(ierr, "run_sweeps: Couldn't copy vec");
	
	ierr = time->solve(ut); fvens_throw(ierr, "run_sweeps: Couldn't solve ODE");
	const TimingData tdata = time->getTimingData();

	delete time;
	ierr = VecDestroy(&ut); petsc_throw(ierr, "run_sweeps: Couldn't delete vec");

	return tdata;
}

}

