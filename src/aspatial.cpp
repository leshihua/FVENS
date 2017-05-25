/** @file aspatial.cpp
 * @brief Finite volume spatial discretization of Euler/Navier-Stokes equations.
 * @author Aditya Kashi
 * @date Feb 24, 2016
 */

#include "aspatial.hpp"

namespace acfd {

/** Solution for supersonic vortex case given by Krivodonova and Berger.
 * Lilia Krivodonova and Marsha Berger, "High-order accurate implementation of solid wall boundary conditions in curved geometries",
 * JCP 211, pp 492--512, 2006.
 */
void get_supersonicvortex_state(const a_real g, const a_real Mi, const a_real ri, const a_real rhoi, const a_real r,
		a_real& rho, a_real& rhov1, a_real& rhov2, a_real& rhoe)
{
	a_real p = 1.0 + (g-1.0)*0.5*Mi*Mi*(1-ri*ri/(r*r));
	rho = rhoi * pow(p, 1.0/(g-1));
	a_real ci = sqrt(pow(rhoi, g-1.0));
	a_real v = ci*Mi/r;
	rhov1 = rho*v; rhov2 = 0;
	p = pow(rho,g)/g;
	rhoe = p/(g-1.0) + 0.5*rho*v*v;
}

void get_supersonicvortex_initial_velocity(const a_real vmag, const a_real x, const a_real y, a_real& vx, a_real& vy)
{
	a_real theta = atan2(y,x) - PI/2.0;
	vx = vmag*cos(theta);
	vy = vmag*sin(theta);
}

EulerFV::EulerFV(const UMesh2dh* mesh, std::string invflux, std::string jacflux, std::string reconst, std::string limiter) 
	: aflux(1.4)
{
	m = mesh;
	g = 1.4;

	/// TODO: Take the two values below as input from control file, rather than hardcoding
	solid_wall_id = 2;
	inflow_outflow_id = 4;
	supersonic_vortex_case_inflow = 10;

	// allocation
	uinf.setup(1, NVARS);
	integ.setup(m->gnelem(), 1);
	dudx.setup(m->gnelem(), NVARS);
	dudy.setup(m->gnelem(), NVARS);
	rc.setup(m->gnelem(),m->gndim());
	rcg.setup(m->gnface(),m->gndim());
	ug.setup(m->gnface(),NVARS);
	gr = new amat::Array2d<a_real>[m->gnaface()];
	for(int i = 0; i <  m->gnaface(); i++)
		gr[i].setup(NGAUSS, m->gndim());
	uleft.setup(m->gnaface(), NVARS);
	uright.setup(m->gnaface(), NVARS);

	// set inviscid flux scheme
	if(invflux == "VANLEER") {
		inviflux = new VanLeerFlux(g, &aflux);
		std::cout << "  EulerFV: Using Van Leer fluxes." << std::endl;
	}
	else if(invflux == "ROE")
	{
		inviflux = new RoeFlux(g, &aflux);
		std::cout << "  EulerFV: Using Roe fluxes." << std::endl;
	}
	else if(invflux == "HLL")
	{
		inviflux = new HLLFlux(g, &aflux);
		std::cout << "  EulerFV: Using HLL fluxes." << std::endl;
	}
	else if(invflux == "HLLC")
	{
		inviflux = new HLLCFlux(g, &aflux);
		std::cout << "  EulerFV: Using HLLC fluxes." << std::endl;
	}
	else if(invflux == "LLF")
	{
		inviflux = new LocalLaxFriedrichsFlux(g, &aflux);
		std::cout << "  EulerFV: Using LLF fluxes." << std::endl;
	}
	else
		std::cout << "  EulerFV: ! Flux scheme not available!" << std::endl;
	
	// set inviscid flux scheme for Jacobian
	if(jacflux == "VANLEER")
		jflux = new VanLeerFlux(g, &aflux);
	else if(jacflux == "ROE")
	{
		jflux = new RoeFlux(g, &aflux);
		std::cout << "  EulerFV: Using Roe fluxes for Jacobian." << std::endl;
	}
	else if(jacflux == "HLL")
	{
		jflux = new HLLFlux(g, &aflux);
		std::cout << "  EulerFV: Using HLL fluxes for Jacobian." << std::endl;
	}
	else if(jacflux == "HLLC")
	{
		jflux = new HLLCFlux(g, &aflux);
		std::cout << "  EulerFV: Using HLLC fluxes for Jacobian." << std::endl;
	}
	else if(jacflux == "LLF")
	{
		jflux = new LocalLaxFriedrichsFlux(g, &aflux);
		std::cout << "  EulerFV: Using LLF fluxes for Jacobian." << std::endl;
	}
	else
		std::cout << "  EulerFV: ! Flux scheme not available!" << std::endl;

	// set reconstruction scheme
	secondOrderRequested = true;
	std::cout << "  EulerFV: Selected reconstruction scheme is " << reconst << std::endl;
	if(reconst == "LEASTSQUARES")
	{
		rec = new WeightedLeastSquaresReconstruction();
		std::cout << "  EulerFV: Weighted least-squares reconstruction will be used." << std::endl;
	}
	else if(reconst == "NONE") {
		rec = new ConstantReconstruction();
		std::cout << "  EulerFV: No reconstruction; first order solution." << std::endl;
		secondOrderRequested = false;
	}
	else //if(reconst == "GREENGAUSS")
	{
		rec = new GreenGaussReconstruction();
		std::cout << "  EulerFV: Green-Gauss reconstruction will be used." << std::endl;
	}

	// set limiter
	if(limiter == "NONE")
	{
		lim = new NoLimiter(m, &rcg, &rc, gr);
		std::cout << "  EulerFV: No limiter will be used." << std::endl;
	}
	else if(limiter == "WENO")
	{
		lim = new WENOLimiter(m, &rcg, &rc, gr);
		std::cout << "  EulerFV: WENO limiter selected.\n";
	}

	// Next, get cell centers (real and ghost)
	
	int idim, inode;

	for(int ielem = 0; ielem < m->gnelem(); ielem++)
	{
		for(idim = 0; idim < m->gndim(); idim++)
		{
			rc(ielem,idim) = 0;
			for(inode = 0; inode < m->gnnode(ielem); inode++)
				rc(ielem,idim) += m->gcoords(m->ginpoel(ielem, inode), idim);
			rc(ielem,idim) = rc(ielem,idim) / (a_real)(m->gnnode(ielem));
		}
	}

	int ied, ig;
	a_real x1, y1, x2, y2;

	compute_ghost_cell_coords_about_midpoint();
	//compute_ghost_cell_coords_about_face();

	//Calculate and store coordinates of Gauss points (general implementation)
	// Gauss points are uniformly distributed along the face.
	for(ied = 0; ied < m->gnaface(); ied++)
	{
		x1 = m->gcoords(m->gintfac(ied,2),0);
		y1 = m->gcoords(m->gintfac(ied,2),1);
		x2 = m->gcoords(m->gintfac(ied,3),0);
		y2 = m->gcoords(m->gintfac(ied,3),1);
		for(ig = 0; ig < NGAUSS; ig++)
		{
			gr[ied](ig,0) = x1 + (a_real)(ig+1.0)/(a_real)(NGAUSS+1.0) * (x2-x1);
			gr[ied](ig,1) = y1 + (a_real)(ig+1.0)/(a_real)(NGAUSS+1.0) * (y2-y1);
		}
	}

	rec->setup(m, &rc, &rcg);
}

EulerFV::~EulerFV()
{
	delete rec;
	delete inviflux;
	delete jflux;
	delete lim;
	delete [] gr;
}

void EulerFV::compute_ghost_cell_coords_about_midpoint()
{
	int iface, ielem, idim, ip1, ip2;
	std::vector<a_real> midpoint(m->gndim());
	for(iface = 0; iface < m->gnbface(); iface++)
	{
		ielem = m->gintfac(iface,0);
		ip1 = m->gintfac(iface,2);
		ip2 = m->gintfac(iface,3);

		for(idim = 0; idim < m->gndim(); idim++)
		{
			midpoint[idim] = 0.5 * (m->gcoords(ip1,idim) + m->gcoords(ip2,idim));
		}

		for(idim = 0; idim < m->gndim(); idim++)
			rcg(iface,idim) = 2*midpoint[idim] - rc(ielem,idim);
	}
}

void EulerFV::compute_ghost_cell_coords_about_face()
{
	int ied, ielem;
	a_real x1, y1, x2, y2, xs, ys, xi, yi;

	for(ied = 0; ied < m->gnbface(); ied++)
	{
		ielem = m->gintfac(ied,0); //int lel = ielem;
		//jelem = m->gintfac(ied,1); //int rel = jelem;
		a_real nx = m->ggallfa(ied,0);
		a_real ny = m->ggallfa(ied,1);

		xi = rc.get(ielem,0);
		yi = rc.get(ielem,1);

		// Note: The ghost cell is a direct reflection of the boundary cell about the boundary-face
		//       It is NOT the reflection about the midpoint of the boundary-face
		x1 = m->gcoords(m->gintfac(ied,2),0);
		x2 = m->gcoords(m->gintfac(ied,3),0);
		y1 = m->gcoords(m->gintfac(ied,2),1);
		y2 = m->gcoords(m->gintfac(ied,3),1);

		if(fabs(nx)>A_SMALL_NUMBER && fabs(ny)>A_SMALL_NUMBER)		// check if nx != 0 and ny != 0
		{
			xs = ( yi-y1 - ny/nx*xi + (y2-y1)/(x2-x1)*x1 ) / ((y2-y1)/(x2-x1)-ny/nx);
			ys = ny/nx*xs + yi - ny/nx*xi;
		}
		else if(fabs(nx)<=A_SMALL_NUMBER)
		{
			xs = xi;
			ys = y1;
		}
		else
		{
			xs = x1;
			ys = yi;
		}
		rcg(ied,0) = 2*xs-xi;
		rcg(ied,1) = 2*ys-yi;
	}
}

/// Function to feed needed data, compute cell-centers, and initialize u
/** \param Minf Free-stream Mach number
 * \param vinf Free stream velocity magnitude
 * \param a Angle of attack (radians)
 * \param rhoinf Free stream density
 * \param u conserved variable array
 */
void EulerFV::loaddata(const short inittype, a_real Minf, a_real vinf, a_real a, a_real rhoinf, Matrix& u)
{
	// Note that reference density and reference velocity are the values at infinity
	//std::cout << "EulerFV: loaddata(): Calculating initial data...\n";
	a_real vx = vinf*cos(a);
	a_real vy = vinf*sin(a);
	a_real p = rhoinf*vinf*vinf/(g*Minf*Minf);
	uinf(0,0) = rhoinf;		// should be 1
	uinf(0,1) = rhoinf*vx;
	uinf(0,2) = rhoinf*vy;
	uinf(0,3) = p/(g-1) + 0.5*rhoinf*vinf*vinf;

	if(inittype == 1)
		for(int i = 0; i < m->gnelem(); i++)
		{
			// call supersonic vortex initialzation
			a_real x = 0, y = 0;
			for(int inode = 0; inode < m->gnnode(i); inode++) {
				x += m->gcoords(m->ginpoel(i, inode), 0);
				y += m->gcoords(m->ginpoel(i, inode), 1);
			}
			x /= m->gnnode(i); y /= m->gnnode(i);

			get_supersonicvortex_initial_velocity(vinf, x, y, u(i,1), u(i,2));
			u(i,0) = rhoinf;
			u(i,1) *= rhoinf; u(i,2) *= rhoinf;
			u(i,3) = uinf(0,3);
		}
	else
		//initial values are equal to boundary values
		for(int i = 0; i < m->gnelem(); i++)
			for(int j = 0; j < NVARS; j++)
				u(i,j) = uinf(0,j);
	
	std::cout << "EulerFV: loaddata(): Initial data calculated.\n";
}

void EulerFV::compute_boundary_states(const amat::Array2d<a_real>& ins, amat::Array2d<a_real>& bs)
{
#pragma omp parallel for default(shared)
	for(int ied = 0; ied < m->gnbface(); ied++)
	{
		compute_boundary_state(ied, &ins(ied,0), &bs(ied,0));
	}
}

void EulerFV::compute_boundary_state(const int ied, const a_real *const ins, a_real *const bs)
{
	a_real nx = m->ggallfa(ied,0);
	a_real ny = m->ggallfa(ied,1);

	a_real vni = (ins[1]*nx + ins[2]*ny)/ins[0];
	a_real pi = (g-1.0)*(ins[3] - 0.5*(pow(ins[1],2)+pow(ins[2],2))/ins[0]);
	a_real ci = sqrt(g*pi/ins[0]);
	a_real Mni = vni/ci;
	/*a_real pinf = (g-1.0)*(uinf(0,3) - 0.5*(pow(uinf(0,1),2)+pow(uinf(0,2),2))/uinf(0,0));
	a_real cinf = sqrt(g*pinf/uinf(0,0));
	a_real vninf = (uinf(0,1)*nx + uinf(0,2)*ny)/uinf(0,0);
	a_real Mninf = vninf/cinf;*/

	if(m->ggallfa(ied,3) == solid_wall_id)
	{
		bs[0] = ins[0];
		bs[1] = ins[1] - 2*vni*nx*bs[0];
		bs[2] = ins[2] - 2*vni*ny*bs[0];
		bs[3] = ins[3];
	}

	if(m->ggallfa(ied,3) == inflow_outflow_id)
	{
		if(Mni < 1.0)
		{
			for(int i = 0; i < NVARS; i++)
				bs[i] = uinf(0,i);
		}
		else
		{
			for(int i = 0; i < NVARS; i++)
				bs[i] = ins[i];
		}
		/*if(Mni <= -1.0)
		{
			for(int i = 0; i < NVARS; i++)
				bs[i] = uinf(0,i);
		}
		else if(Mni > -1.0 && Mni < 0)
		{
			// subsonic inflow, specify rho and u according to FUN3D BCs paper
			for(i = 0; i < NVARS-1; i++)
				bs(ied,i) = uinf.get(0,i);
			bs(ied,3) = pi/(g-1.0) + 0.5*( uinf.get(0,1)*uinf.get(0,1) + uinf.get(0,2)*uinf.get(0,2) )/uinf.get(0,0);
		}
		else if(Mni >= 0 && Mni < 1.0)
		{
			// subsonic ourflow, specify p accoording FUN3D BCs paper
			for(i = 0; i < NVARS-1; i++)
				bs(ied,i) = ins.get(ied,i);
			bs(ied,3) = pinf/(g-1.0) + 0.5*( ins.get(ied,1)*ins.get(ied,1) + ins.get(ied,2)*ins.get(ied,2) )/ins.get(ied,0);
		}
		else
			for(i = 0; i < NVARS; i++)
				bs(ied,i) = ins.get(ied,i);*/
	}
	
	if(m->ggallfa(ied,3) == supersonic_vortex_case_inflow) {
		// y-coordinate of face center
		a_real r = 0.5*(m->gcoords(m->gintfac(ied,2),1) + m->gcoords(m->gintfac(ied,3),1));
		a_real ri = 1.0, Mi = 2.25, rhoi = 1.0;
		get_supersonicvortex_state(g, Mi, ri, rhoi, r, bs[0], bs[1], bs[2], bs[3]);
	}
}

void EulerFV::compute_residual(const Matrix& __restrict__ u, Matrix& __restrict__ residual, amat::Array2d<a_real>& __restrict__ dtm)
{
#pragma omp parallel default(shared)
	{
#pragma omp for simd
		for(int iel = 0; iel < m->gnelem(); iel++)
		{
			integ(iel) = 0.0;
		}

		// first, set cell-centered values of boundary cells as left-side values of boundary faces
#pragma omp for
		for(a_int ied = 0; ied < m->gnbface(); ied++)
		{
			a_int ielem = m->gintfac(ied,0);
			for(int ivar = 0; ivar < NVARS; ivar++)
				uleft(ied,ivar) = u(ielem,ivar);
		}
	}

	if(secondOrderRequested)
	{
		// get cell average values at ghost cells using BCs
		compute_boundary_states(uleft, ug);

		rec->compute_gradients(&u, &ug, &dudx, &dudy);
		lim->compute_face_values(&u, &ug, &dudx, &dudy, &uleft, &uright);
	}
	else
	{
		// if order is 1, set the face data same as cell-centred data for all faces
		
		// set both left and right states for all interior faces
#pragma omp parallel for default(shared)
		for(a_int ied = m->gnbface(); ied < m->gnaface(); ied++)
		{
			a_int ielem = m->gintfac(ied,0);
			a_int jelem = m->gintfac(ied,1);
			for(int ivar = 0; ivar < NVARS; ivar++)
			{
				uleft(ied,ivar) = u(ielem,ivar);
				uright(ied,ivar) = u(jelem,ivar);
			}
		}
	}

	// set right (ghost) state for boundary faces
	compute_boundary_states(uleft,uright);

	/** Compute fluxes.
	 * The integral of the maximum magnitude of eigenvalue over each face is also computed:
	 * \f[
	 * \int_{f_i} (|v_n| + c) \mathrm{d}l
	 * \f]
	 * so that time steps can be calculated for explicit time stepping.
	 */

#pragma omp parallel default(shared)
	{
#pragma omp for
		for(a_int ied = 0; ied < m->gnaface(); ied++)
		{
			a_real n[NDIM];
			n[0] = m->ggallfa(ied,0);
			n[1] = m->ggallfa(ied,1);
			a_real len = m->ggallfa(ied,2);
			int lelem = m->gintfac(ied,0);
			int relem = m->gintfac(ied,1);
			a_real fluxes[NVARS];

			inviflux->get_flux(&uleft(ied,0), &uright(ied,0), n, fluxes);

			// integrate over the face
			for(int ivar = 0; ivar < NVARS; ivar++)
					fluxes[ivar] *= len;

			//calculate presures from u
			a_real pi = (g-1)*(uleft(ied,3) - 0.5*(pow(uleft(ied,1),2)+pow(uleft(ied,2),2))/uleft(ied,0));
			a_real pj = (g-1)*(uright(ied,3) - 0.5*(pow(uright(ied,1),2)+pow(uright(ied,2),2))/uright(ied,0));
			//calculate speeds of sound
			a_real ci = sqrt(g*pi/uleft(ied,0));
			a_real cj = sqrt(g*pj/uright(ied,0));
			//calculate normal velocities
			a_real vni = (uleft(ied,1)*n[0] +uleft(ied,2)*n[1])/uleft(ied,0);
			a_real vnj = (uright(ied,1)*n[0] + uright(ied,2)*n[1])/uright(ied,0);

			for(int ivar = 0; ivar < NVARS; ivar++) {
#pragma omp atomic
				residual(lelem,ivar) += fluxes[ivar];
			}
			if(relem < m->gnelem()) {
				for(int ivar = 0; ivar < NVARS; ivar++) {
#pragma omp atomic
					residual(relem,ivar) -= fluxes[ivar];
				}
			}
#pragma omp atomic
			integ(lelem) += (fabs(vni)+ci)*len;
			if(relem < m->gnelem()) {
#pragma omp atomic
				integ(relem) += (fabs(vnj)+cj)*len;
			}
		}

#pragma omp barrier
#pragma omp for simd
		for(int iel = 0; iel < m->gnelem(); iel++)
		{
			dtm(iel) = m->garea(iel)/integ(iel);
		}
	} // end parallel region
}

#if HAVE_PETSC==1

void EulerFV::compute_jacobian(const Matrix& __restrict__ u, const bool blocked, Mat A)
{
	if(blocked)
	{
		// TODO: construct blocked Jacobian
	}
	else
	{
		Array2d<a_real>* D = new Array2d<a_real>[m->gnelem()];
		for(int iel = 0; iel < m->gnelem(); iel++) {
			D[iel].setup(NVARS,NVARS);
			D[iel].zeros();
		}

		for(a_int iface = 0; iface < m->gnbface(); iface++)
		{
			a_int lelem = m->gintfac(iface,0);
			a_real n[NDIM];
			n[0] = m->ggallfa(iface,0);
			n[1] = m->ggallfa(iface,1);
			a_real len = m->ggallfa(iface,2);
			a_real uface[NVARS];
			amat::Array2d<a_real> left(NVARS,NVARS);
			amat::Array2d<a_real> right(NVARS,NVARS);
			
			compute_boundary_state(iface, &u(lelem,0), uface);
			jflux->get_jacobian(&u(lelem,0), uface, n, &left(0,0), &right(0,0));
			
			for(int i = 0; i < NVARS; i++)
				for(int j = 0; j < NVARS; j++) {
					left(i,j) *= len;
#pragma omp atomic write
					D[lelem](i,j) -= left(i,j);
				}
		}

		for(a_int iface = m->gnbface(); iface < m->gnaface(); iface++)
		{
			a_int lelem = m->gintfac(iface,0);
			a_int relem = m->gintfac(iface,1);
			a_real n[NDIM];
			n[0] = m->ggallfa(iface,0);
			n[1] = m->ggallfa(iface,1);
			a_real len = m->ggallfa(iface,2);
			a_real uface[NVARS];
			amat::Array2d<a_real> left(NVARS,NVARS);
			amat::Array2d<a_real> right(NVARS,NVARS);
			
			jflux->get_jacobian(&u(lelem,0), &u(relem,0), n, &left(0,0), &right(0,0));

			for(int i = 0; i < NVARS; i++)
				for(int j = 0; j < NVARS; j++) {
					left(i,j) *= len;
					right(i,j) *= len;
#pragma omp atomic write
					D[lelem](i,j) -= left(i,j);
#pragma omp atomic write
					D[relem](i,j) -= right(i,j);
				}

			PetscInt* rindices = std::malloc(NVARS*NVARS*sizeof(PetscInt));
			PetscInt* cindices = std::malloc(NVARS*NVARS*sizeof(PetscInt));
			// insert upper block U = right
			for(int i = 0; i < NVARS; i++)
				for(int j = 0; j < NVARS; j++)
				{
					rindices[i*NVARS+j] = ielem*NVARS+i;
					cindices[i*NVARS+j] = jelem*NVARS+j;
				}
			MatSetValues(A, NVARS, rindices, NVARS, cindices, &right(0,0), INSERT_VALUES);

			// insert lower block L = left
			for(int i = 0; i < NVARS; i++)
				for(int j = 0; j < NVARS; j++)
				{
					rindices[i*NVARS+j] = jelem*NVARS+i;
					cindices[i*NVARS+j] = ielem*NVARS+j;
				}
			MatSetValues(A, NVARS, rindices, NVARS, cindices, &left(0,0), INSERT_VALUES);

			std::free(rindices);
			std::free(cindices);
		}

		// diagonal blocks
		for(a_int iel = 0; iel < m->gnelem(); iel++)
		{
			PetscInt* rindices = std::malloc(NVARS*NVARS*sizeof(PetscInt));
			PetscInt* cindices = std::malloc(NVARS*NVARS*sizeof(PetscInt));
			
			for(int i = 0; i < NVARS; i++)
				for(int j = 0; j < NVARS; j++)
				{
					rindices[i*NVARS+j] = iel*NVARS+i;
					cindices[i*NVARS+j] = iel*NVARS+j;
				}
			MatSetValues(A, NVARS, rindices, NVARS, cindices, &D[iel](0,0), ADD_VALUES);

			std::free(rindices);
			std::free(cindices);
		}
	}
}

#else

/** Computes the Jacobian in a block diagonal, lower and upper format.
 * If the (numerical) flux from cell i to cell j is \f$ F_{ij}(u_i, u_j, n_{ij}) \f$,
 * then \f$ L_{ij} = -\frac{\partial F_{ij}}{\partial u_i} \f$ and
 * \f$ U_{ij} = \frac{\partial F_{ij}}{\partial u_j} \f$.
 * Also, the contribution of face ij to diagonal blocks are 
 * \f$ D_{ii} \rightarrow D_{ii} -L_{ij}, D_{jj} \rigtharrow D_{jj} -U_{ij} \f$.
 */
void EulerFV::compute_jacobian(const Matrix& __restrict__ u, Matrix *const D, Matrix *const L, Matrix *const U)
{
#pragma omp parallel for default(shared)
	for(a_int iface = 0; iface < m->gnbface(); iface++)
	{
		a_int lelem = m->gintfac(iface,0);
		a_real n[NDIM];
		n[0] = m->ggallfa(iface,0);
		n[1] = m->ggallfa(iface,1);
		a_real len = m->ggallfa(iface,2);
		a_real uface[NVARS];
		Matrix left(NVARS,NVARS);
		Matrix right(NVARS,NVARS);
		
		compute_boundary_state(iface, &u(lelem,0), uface);
		jflux->get_jacobian(&u(lelem,0), uface, n, &left(0,0), &right(0,0));
		
		for(int i = 0; i < NVARS; i++)
			for(int j = 0; j < NVARS; j++) {
				left(i,j) *= len;
#pragma omp atomic update
				D[lelem](i,j) -= left(i,j);
			}
	}

#pragma omp parallel for default(shared)
	for(a_int iface = m->gnbface(); iface < m->gnaface(); iface++)
	{
		a_int intface = iface-m->gnbface();
		a_int lelem = m->gintfac(iface,0);
		a_int relem = m->gintfac(iface,1);
		a_real n[NDIM];
		n[0] = m->ggallfa(iface,0);
		n[1] = m->ggallfa(iface,1);
		a_real len = m->ggallfa(iface,2);
	
		jflux->get_jacobian(&u(lelem,0), &u(relem,0), n, &L[intface](0,0), &U[intface](0,0));

		for(int i = 0; i < NVARS; i++)
			for(int j = 0; j < NVARS; j++) {
				L[intface](i,j) *= len;
				U[intface](i,j) *= len;
#pragma omp atomic update
				D[lelem](i,j) -= L[intface](i,j);
#pragma omp atomic update
				D[relem](i,j) -= U[intface](i,j);
			}
	}
}

#endif

void EulerFV::postprocess_point(const Matrix& u, amat::Array2d<a_real>& scalars, amat::Array2d<a_real>& velocities)
{
	std::cout << "EulerFV: postprocess_point(): Creating output arrays...\n";
	scalars.setup(m->gnpoin(),3);
	velocities.setup(m->gnpoin(),2);
	
	amat::Array2d<a_real> areasum(m->gnpoin(),1);
	amat::Array2d<a_real> up(m->gnpoin(), NVARS);
	up.zeros();
	areasum.zeros();

	for(int ielem = 0; ielem < m->gnelem(); ielem++)
	{
		for(int inode = 0; inode < m->gnnode(ielem); inode++)
			for(int ivar = 0; ivar < NVARS; ivar++)
			{
				up(m->ginpoel(ielem,inode),ivar) += u(ielem,ivar)*m->garea(ielem);
				areasum(m->ginpoel(ielem,inode)) += m->garea(ielem);
			}
	}

	for(int ipoin = 0; ipoin < m->gnpoin(); ipoin++)
		for(int ivar = 0; ivar < NVARS; ivar++)
			up(ipoin,ivar) /= areasum(ipoin);
	
	for(int ipoin = 0; ipoin < m->gnpoin(); ipoin++)
	{
		scalars(ipoin,0) = up(ipoin,0);
		velocities(ipoin,0) = up(ipoin,1)/up(ipoin,0);
		velocities(ipoin,1) = up(ipoin,2)/up(ipoin,0);
		//velocities(ipoin,0) = dudx(ipoin,1);
		//velocities(ipoin,1) = dudy(ipoin,1);
		a_real vmag2 = pow(velocities(ipoin,0), 2) + pow(velocities(ipoin,1), 2);
		scalars(ipoin,2) = up(ipoin,0)*(g-1) * (up(ipoin,3)/up(ipoin,0) - 0.5*vmag2);		// pressure
		a_real c = sqrt(g*scalars(ipoin,2)/up(ipoin,0));
		scalars(ipoin,1) = sqrt(vmag2)/c;
	}

	compute_entropy_cell(u);

	std::cout << "EulerFV: postprocess_point(): Done.\n";
}

void EulerFV::postprocess_cell(const Matrix& u, amat::Array2d<a_real>& scalars, amat::Array2d<a_real>& velocities)
{
	std::cout << "EulerFV: postprocess_cell(): Creating output arrays...\n";
	scalars.setup(m->gnelem(), 3);
	velocities.setup(m->gnelem(), 2);

	for(int iel = 0; iel < m->gnelem(); iel++) {
		scalars(iel,0) = u(iel,0);
	}

	for(int iel = 0; iel < m->gnelem(); iel++)
	{
		velocities(iel,0) = u(iel,1)/u(iel,0);
		velocities(iel,1) = u(iel,2)/u(iel,0);
		a_real vmag2 = pow(velocities(iel,0), 2) + pow(velocities(iel,1), 2);
		scalars(iel,2) = (g-1) * (u(iel,3) - 0.5*u(iel,0)*vmag2);				// pressure
		a_real c = sqrt(g*scalars(iel,2)/u(iel,0));
		scalars(iel,1) = sqrt(vmag2)/c;
	}
	compute_entropy_cell(u);
	std::cout << "EulerFV: postprocess_cell(): Done.\n";
}

a_real EulerFV::compute_entropy_cell(const Matrix& u)
{
	a_real vmaginf2 = uinf(0,1)/uinf(0,0)*uinf(0,1)/uinf(0,0) + uinf(0,2)/uinf(0,0)*uinf(0,2)/uinf(0,0);
	a_real sinf = ( uinf(0,0)*(g-1) * (uinf(0,3)/uinf(0,0) - 0.5*vmaginf2) ) / pow(uinf(0,0),g);

	amat::Array2d<a_real> s_err(m->gnelem(),1);
	a_real error = 0;
	for(int iel = 0; iel < m->gnelem(); iel++)
	{
		a_real p = (g-1) * ( u(iel,3) - 0.5*(u(iel,1)*u(iel,1)+u(iel,2)*u(iel,2))/u(iel,0) );
		s_err(iel) = (p / pow(u(iel,0),g) - sinf) / sinf;
		error += s_err(iel)*s_err(iel)*m->garea(iel);
	}
	error = sqrt(error);

	//a_real h = sqrt((m->jacobians).max());
	a_real h = 1.0/sqrt(m->gnelem());
 
	std::cout << "EulerFV:   " << log10(h) << "  " << std::setprecision(10) << log10(error) << std::endl;

	return error;
}

}	// end namespace