/** @file aspatial.cpp
 * @brief Finite volume spatial discretization of Euler/Navier-Stokes equations.
 * @author Aditya Kashi
 * @date Feb 24, 2016
 */

#include "aspatial.hpp"
#include "alinalg.hpp"

namespace acfd {

template<short nvars>
Spatial<nvars>::Spatial(const UMesh2dh *const mesh) : m(mesh), eps{sqrt(ZERO_TOL)/10.0}
{
	rc.resize(m->gnelem(),m->gndim());
	rcg.resize(m->gnbface(),m->gndim());
	gr = new amat::Array2d<a_real>[m->gnaface()];
	for(int i = 0; i <  m->gnaface(); i++)
		gr[i].resize(NGAUSS, m->gndim());

	// get cell centers (real and ghost)
	
	for(a_int ielem = 0; ielem < m->gnelem(); ielem++)
	{
		for(short idim = 0; idim < m->gndim(); idim++)
		{
			rc(ielem,idim) = 0;
			for(int inode = 0; inode < m->gnnode(ielem); inode++)
				rc(ielem,idim) += m->gcoords(m->ginpoel(ielem, inode), idim);
			rc(ielem,idim) = rc(ielem,idim) / (a_real)(m->gnnode(ielem));
		}
	}

	a_real x1, y1, x2, y2;

	compute_ghost_cell_coords_about_midpoint();
	//compute_ghost_cell_coords_about_face();

	//Calculate and store coordinates of Gauss points
	// Gauss points are uniformly distributed along the face.
	for(a_int ied = 0; ied < m->gnaface(); ied++)
	{
		x1 = m->gcoords(m->gintfac(ied,2),0);
		y1 = m->gcoords(m->gintfac(ied,2),1);
		x2 = m->gcoords(m->gintfac(ied,3),0);
		y2 = m->gcoords(m->gintfac(ied,3),1);
		for(short ig = 0; ig < NGAUSS; ig++)
		{
			gr[ied](ig,0) = x1 + (a_real)(ig+1.0)/(a_real)(NGAUSS+1.0) * (x2-x1);
			gr[ied](ig,1) = y1 + (a_real)(ig+1.0)/(a_real)(NGAUSS+1.0) * (y2-y1);
		}
	}
}

template<short nvars>
Spatial<nvars>::~Spatial()
{
	delete [] gr;
}

template<short nvars>
void Spatial<nvars>::compute_ghost_cell_coords_about_midpoint()
{
	for(a_int iface = 0; iface < m->gnbface(); iface++)
	{
		a_int ielem = m->gintfac(iface,0);
		a_int ip1 = m->gintfac(iface,2);
		a_int ip2 = m->gintfac(iface,3);
		a_real midpoint[NDIM];

		for(short idim = 0; idim < NDIM; idim++)
		{
			midpoint[idim] = 0.5 * (m->gcoords(ip1,idim) + m->gcoords(ip2,idim));
		}

		for(short idim = 0; idim < NDIM; idim++)
			rcg(iface,idim) = 2*midpoint[idim] - rc(ielem,idim);
	}
}

/** The ghost cell is a reflection of the boundary cell about the boundary-face.
 * It is NOT the reflection about the midpoint of the boundary-face.
 */
template<short nvars>
void Spatial<nvars>::compute_ghost_cell_coords_about_face()
{
	a_real x1, y1, x2, y2, xs, ys, xi, yi;

	for(a_int ied = 0; ied < m->gnbface(); ied++)
	{
		a_int ielem = m->gintfac(ied,0);
		a_real nx = m->ggallfa(ied,0);
		a_real ny = m->ggallfa(ied,1);

		xi = rc(ielem,0);
		yi = rc(ielem,1);

		x1 = m->gcoords(m->gintfac(ied,2),0);
		x2 = m->gcoords(m->gintfac(ied,3),0);
		y1 = m->gcoords(m->gintfac(ied,2),1);
		y2 = m->gcoords(m->gintfac(ied,3),1);

		// check if nx != 0 and ny != 0
		if(fabs(nx)>A_SMALL_NUMBER && fabs(ny)>A_SMALL_NUMBER)		
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

template <short nvars>
void Spatial<nvars>::compute_jac_vec(const MVector& resu, const MVector& u, 
	const MVector& v, const bool add_time_deriv, const amat::Array2d<a_real>& dtm,
	MVector& __restrict aux,
	MVector& __restrict prod)
{
	const a_int N = m->gnelem()*nvars;
	a_real vnorm = dot(N, v.data(),v.data());
	vnorm = sqrt(vnorm);
	
	// compute the perturbed state and store in aux
	axpbypcz(N, 0.0,aux.data(), 1.0,u.data(), eps/vnorm,v.data());
	
	// compute residual at the perturbed state and store in the output variable prod
	amat::Array2d<a_real> _dtm;		// dummy
	compute_residual(aux, prod, false, _dtm);
	
	// compute the Jacobian vector product
#pragma omp parallel for simd default(shared)
	for(a_int i = 0; i < m->gnelem()*nvars; i++)
		prod.data()[i] = (prod.data()[i] - resu.data()[i]) / (eps/vnorm);

	// add time term to the output vector if necessary
	if(add_time_deriv) {
#pragma omp parallel for simd default(shared)
		for(a_int iel = 0; iel < m->gnelem(); iel++)
			for(int ivar = 0; ivar < nvars; ivar++)
				prod(iel,ivar) += m->garea(iel)/dtm(iel)*v(iel,ivar);
	}
}

// Computes a([M du/dt +] dR/du) v + b w and stores in prod
template <short nvars>
void Spatial<nvars>::compute_jac_gemv(const a_real a, const MVector& resu, 
		const MVector& u, const MVector& v,
		const bool add_time_deriv, const amat::Array2d<a_real>& dtm,
		const a_real b, const MVector& w,
		MVector& __restrict aux,
		MVector& __restrict prod)
{
	const a_int N = m->gnelem()*nvars;
	a_real vnorm = dot(N, v.data(),v.data());
	vnorm = sqrt(vnorm);
	
	// compute the perturbed state and store in aux
	axpbypcz(N, 0.0,aux.data(), 1.0,u.data(), eps/vnorm,v.data());
	
	// compute residual at the perturbed state and store in the output variable prod
	amat::Array2d<a_real> _dtm;		// dummy
	compute_residual(aux, prod, false, _dtm);
	
	// compute the Jacobian vector product and vector add
#pragma omp parallel for simd default(shared)
	for(a_int i = 0; i < m->gnelem()*nvars; i++)
		prod.data()[i] = a*(prod.data()[i] - resu.data()[i]) / (eps/vnorm) + b*w.data()[i];

	// add time term to the output vector if necessary
	if(add_time_deriv) {
#pragma omp parallel for simd default(shared)
		for(a_int iel = 0; iel < m->gnelem(); iel++)
			for(int ivar = 0; ivar < nvars; ivar++)
				prod(iel,ivar) += a*m->garea(iel)/dtm(iel)*v(iel,ivar);
	}
}

FlowFV::FlowFV(const UMesh2dh *const mesh, 
		const a_real g, const a_real Minf, const a_real Tinf, const a_real Reinf, const a_real Pr,
		const a_real a, 
		const int isothermal_marker, const int isothermalbaric_marker, 
		const int adiabatic_marker, const int slip_marker, const int inflowoutflow_marker, 
		const a_real isothermal_Temperature, const a_real isothermal_TangVel,
		const a_real isothermalbaric_Temperature, const a_real isothermalbaric_TangVel,
		const a_real isothermalbaric_Pressure, const a_real adiabatic_TangVel,
		std::string invflux, std::string jacflux, std::string reconst, std::string limiter,
		const bool reconstructPrim)
	: 
	Spatial<NVARS>(mesh), physics(g, Minf, Tinf, Reinf, Pr),
	isothermal_wall_id{isothermal_marker}, isothermalbaric_wall_id{isothermalbaric_marker},
	adiabatic_wall_id{adiabatic_marker},
	slip_wall_id{slip_marker}, inflow_outflow_id{inflowoutflow_marker},
	isothermal_wall_temperature{isothermal_Temperature/Tinf},
	isothermal_wall_tangvel{isothermal_TangVel}, 
	isothermalbaric_wall_temperature{isothermalbaric_Temperature},
	isothermalbaric_wall_tangvel{isothermalbaric_TangVel},
	isothermalbaric_wall_pressure{isothermalbaric_Pressure},
	adiabatic_wall_tangvel{adiabatic_TangVel},
	reconstructPrimitive{reconstructPrim}
{
#ifdef DEBUG
	std::cout << "Boundary markers:\n";
	std::cout << "Farfield " << inflow_outflow_id << ", slip wall " << slip_wall_id << '\n';
#endif
	// set inviscid flux scheme
	if(invflux == "VANLEER") {
		inviflux = new VanLeerFlux(&physics);
		std::cout << "  FlowFV: Using Van Leer fluxes." << std::endl;
	}
	else if(invflux == "ROE")
	{
		inviflux = new RoeFlux(&physics);
		std::cout << "  FlowFV: Using Roe fluxes." << std::endl;
	}
	else if(invflux == "HLL")
	{
		inviflux = new HLLFlux(&physics);
		std::cout << "  FlowFV: Using HLL fluxes." << std::endl;
	}
	else if(invflux == "HLLC")
	{
		inviflux = new HLLCFlux(&physics);
		std::cout << "  FlowFV: Using HLLC fluxes." << std::endl;
	}
	else if(invflux == "LLF")
	{
		inviflux = new LocalLaxFriedrichsFlux(&physics);
		std::cout << "  FlowFV: Using LLF fluxes." << std::endl;
	}
	else
		std::cout << "  FlowFV: ! Flux scheme not available!" << std::endl;
	
	// set inviscid flux scheme for Jacobian
	allocflux = false;
	if(jacflux == "VANLEER") {
		jflux = new VanLeerFlux(&physics);
		allocflux = true;
	}
	else if(jacflux == "ROE")
	{
		jflux = new RoeFlux(&physics);
		std::cout << "  FlowFV: Using Roe fluxes for Jacobian." << std::endl;
		allocflux = true;
	}
	else if(jacflux == "HLL")
	{
		jflux = new HLLFlux(&physics);
		std::cout << "  FlowFV: Using HLL fluxes for Jacobian." << std::endl;
		allocflux = true;
	}
	else if(jacflux == "HLLC")
	{
		jflux = new HLLCFlux(&physics);
		std::cout << "  FlowFV: Using HLLC fluxes for Jacobian." << std::endl;
		allocflux = true;
	}
	else if(jacflux == "LLF")
	{
		jflux = new LocalLaxFriedrichsFlux(&physics);
		std::cout << "  FlowFV: Using LLF fluxes for Jacobian." << std::endl;
		allocflux = true;
	}
	else
		std::cout << "  FlowFV: ! Flux scheme not available!" << std::endl;

	// set reconstruction scheme
	secondOrderRequested = true;
	std::cout << "  FlowFV: Selected reconstruction scheme is " << reconst << std::endl;
	if(reconst == "LEASTSQUARES")
	{
		rec = new WeightedLeastSquaresReconstruction<NVARS>(m, &rc, &rcg);
		std::cout << "  FlowFV: Weighted least-squares reconstruction will be used.\n";
	}
	else if(reconst == "GREENGAUSS")
	{
		rec = new GreenGaussReconstruction<NVARS>(m, &rc, &rcg);
		std::cout << "  FlowFV: Green-Gauss reconstruction will be used." << std::endl;
	}
	else /*if(reconst == "NONE")*/ {
		rec = new ConstantReconstruction<NVARS>(m, &rc, &rcg);
		std::cout << "  FlowFV: No reconstruction; first order solution." << std::endl;
		secondOrderRequested = false;
	}

	// set limiter
	if(limiter == "NONE")
	{
		lim = new NoLimiter(m, &rcg, &rc, gr);
		std::cout << "  FlowFV: No limiter will be used." << std::endl;
	}
	else if(limiter == "WENO")
	{
		lim = new WENOLimiter(m, &rcg, &rc, gr);
		std::cout << "  FlowFV: WENO limiter selected.\n";
	}
	else if(limiter == "VANALBADA")
	{
		lim = new VanAlbadaLimiter(m, &rcg, &rc, gr);
		std::cout << "  FlowFV: Van Albada limiter selected.\n";
	}
	else if(limiter == "BARTHJESPERSEN")
	{
		lim = new BarthJespersenLimiter(m, &rcg, &rc, gr);
		std::cout << "  FlowFV: Barth-Jespersen limiter selected.\n";
	}
	else if(limiter == "VENKATAKRISHNAN")
	{
		lim = new VenkatakrishnanLimiter(m, &rcg, &rc, gr, 3.75);
		std::cout << "  FlowFV: Venkatakrishnan limiter selected.\n";
	}
	
	// Set farfield: note that reference density and reference velocity are the values at infinity

	uinf.resize(1, NVARS);
	uinf(0,0) = 1.0;
	uinf(0,1) = cos(a);
	uinf(0,2) = sin(a);
	uinf(0,3) = 1.0/((physics.g-1)*physics.g*physics.Minf*physics.Minf) + 0.5;
}

FlowFV::~FlowFV()
{
	delete rec;
	delete inviflux;
	if(allocflux)
		delete jflux;
	delete lim;
}

void FlowFV::initializeUnknowns(const bool fromfile, const std::string file, MVector& u)
{

	if(fromfile)
	{
		/// TODO: read initial conditions from file
	}
	else
		//initial values are equal to boundary values
		for(a_int i = 0; i < m->gnelem(); i++)
			for(short j = 0; j < NVARS; j++)
				u(i,j) = uinf(0,j);

#ifdef DEBUG
	std::cout << "FlowFV: loaddata(): Initial data calculated.\n";
#endif
}

void FlowFV::compute_boundary_states(const amat::Array2d<a_real>& ins, amat::Array2d<a_real>& bs)
{
#pragma omp parallel for default(shared)
	for(a_int ied = 0; ied < m->gnbface(); ied++)
	{
		compute_boundary_state(ied, &ins(ied,0), &bs(ied,0));
	}
}

void FlowFV::compute_boundary_state(const int ied, const a_real *const ins, a_real *const bs)
{
	a_real nx = m->ggallfa(ied,0);
	a_real ny = m->ggallfa(ied,1);

	a_real vni = (ins[1]*nx + ins[2]*ny)/ins[0];

	if(m->ggallfa(ied,3) == slip_wall_id)
	{
		bs[0] = ins[0];
		bs[1] = ins[1] - 2*vni*nx*bs[0];
		bs[2] = ins[2] - 2*vni*ny*bs[0];
		bs[3] = ins[3];
	}

	if(m->ggallfa(ied,3) == isothermal_wall_id)
	{
		bs[0] = ins[0];
		bs[1] = -ins[1];
		bs[2] = -ins[2];
		a_real prim2state[] = {bs[0], bs[1]/bs[0], bs[2]/bs[0], isothermal_wall_temperature};
		bs[3] = physics.getEnergyFromPrimitive2(prim2state);
	}

	if(m->ggallfa(ied,3) == adiabatic_wall_id)
	{
		bs[0] = ins[0];
		bs[1] = -ins[1];
		bs[2] = -ins[2];
		a_real Tins = physics.getTemperatureFromConserved(ins);
		a_real prim2state[] = {bs[0], bs[1]/bs[0], bs[2]/bs[0], Tins};
		bs[3] = physics.getEnergyFromPrimitive2(prim2state);
	}

	/* Ghost cell values are always free-stream values.
	 */
	if(m->ggallfa(ied,3) == inflow_outflow_id)
	{
		for(int i = 0; i < NVARS; i++)
			bs[i] = uinf(0,i);
	}

	/* This BC is NOT TESTED and mostly DOES NOT WORK.
	 * Whether the flow is subsonic or supersonic at the boundary
	 * is decided by interior value of the Mach number.
	 * Commented below: Kind of according to FUN3D BCs paper
	 * TODO: Instead, the Mach number based on the Riemann solution state should be used.
	 */
	int characteristic_id = -1;
	if(m->ggallfa(ied,3) == characteristic_id)
	{
		a_real ci = physics.getSoundSpeedFromConserved(ins);
		a_real Mni = vni/ci;
		a_real pinf = physics.getPressureFromConserved(&uinf(0,0));
		/*a_real cinf = physics.getSoundSpeedFromConserved(&uinf(0,0));
		a_real vninf = (uinf(0,1)*nx + uinf(0,2)*ny)/uinf(0,0);
		a_real Mninf = vninf/cinf;
		a_real pi = physics.getPressureFromConserved(ins);*/

		if(Mni <= 0)
		{
			for(short i = 0; i < NVARS; i++)
				bs[i] = uinf(0,i);
		}
		else if(Mni <= 1)
		{
			bs[0] = ins[0];
			bs[1] = ins[1];
			bs[2] = ins[2];
			bs[3] = pinf/(physics.g-1.0) + 0.5*(ins[1]*ins[1]+ins[2]*ins[2])/ins[0];
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
			bs(ied,3) = pi/(g-1.0) 
						+ 0.5*( uinf(0,1)*uinf(0,1) + uinf(0,2)*uinf(0,2) )/uinf(0,0);
		}
		else if(Mni >= 0 && Mni < 1.0)
		{
			// subsonic ourflow, specify p accoording FUN3D BCs paper
			for(i = 0; i < NVARS-1; i++)
				bs(ied,i) = ins.get(ied,i);
			bs(ied,3) = pinf/(g-1.0) 
						+ 0.5*( ins(ied,1)*ins(ied,1) + ins(ied,2)*ins(ied,2) )/ins(ied,0);
		}
		else
			for(i = 0; i < NVARS; i++)
				bs(ied,i) = ins.get(ied,i);*/
	}
}

void FlowFV::compute_residual(const MVector& u, MVector& __restrict residual, 
		const bool gettimesteps, amat::Array2d<a_real>& __restrict dtm)
{
	amat::Array2d<a_real> integ, dudx, dudy, ug, uleft, uright;	
	integ.resize(m->gnelem(), 1);
	dudx.resize(m->gnelem(), NVARS);
	dudy.resize(m->gnelem(), NVARS);
	ug.resize(m->gnbface(),NVARS);
	uleft.resize(m->gnaface(), NVARS);
	uright.resize(m->gnaface(), NVARS);

#pragma omp parallel default(shared)
	{
#pragma omp for simd
		for(a_int iel = 0; iel < m->gnelem(); iel++)
		{
			integ(iel) = 0.0;
		}

		// first, set cell-centered values of boundary cells as left-side values of boundary faces
#pragma omp for
		for(a_int ied = 0; ied < m->gnbface(); ied++)
		{
			a_int ielem = m->gintfac(ied,0);
			for(short ivar = 0; ivar < NVARS; ivar++)
				uleft(ied,ivar) = u(ielem,ivar);
		}
	}

	if(secondOrderRequested)
	{
		// get cell average values at ghost cells using BCs
		compute_boundary_states(uleft, ug);

		if(reconstructPrimitive)
		{
			MVector up(m->gnelem(), NVARS);

			// convert everything to primitive variables
#pragma omp parallel default(shared)
			{
#pragma omp for
				for(a_int iface = 0; iface < m->gnbface(); iface++)
				{
					physics.convertConservedToPrimitive(&ug(iface,0), &ug(iface,0));
				}

#pragma omp for
				for(a_int iel = 0; iel < m->gnelem(); iel++)
					physics.convertConservedToPrimitive(&u(iel,0), &up(iel,0));
			}

			// reconstruct
			rec->compute_gradients(&up, &ug, &dudx, &dudy);
			lim->compute_face_values(up, ug, dudx, dudy, uleft, uright);

			// convert face values back to conserved variables - gradients stay primitive
#pragma omp parallel default(shared)
			{
#pragma omp for
				for(a_int iface = m->gnbface(); iface < m->gnaface(); iface++)
				{
					physics.convertPrimitiveToConserved(&uleft(iface,0), &uleft(iface,0));
					physics.convertPrimitiveToConserved(&uright(iface,0), &uright(iface,0));
				}
#pragma omp for
				for(a_int iface = 0; iface < m->gnbface(); iface++) {
					physics.convertPrimitiveToConserved(&uleft(iface,0), &uleft(iface,0));
				}
			}
		}
		else
		{
			rec->compute_gradients(&u, &ug, &dudx, &dudy);
			lim->compute_face_values(u, ug, dudx, dudy, uleft, uright);
		}
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
			for(short ivar = 0; ivar < NVARS; ivar++)
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
			const int lelem = m->gintfac(ied,0);
			const int relem = m->gintfac(ied,1);
			a_real fluxes[NVARS];

			inviflux->get_flux(&uleft(ied,0), &uright(ied,0), n, fluxes);

			// integrate over the face
			for(short ivar = 0; ivar < NVARS; ivar++)
					fluxes[ivar] *= len;

			//calculate presures from u
			const a_real pi = (physics.g-1)*(uleft(ied,3) 
					- 0.5*(pow(uleft(ied,1),2)+pow(uleft(ied,2),2))/uleft(ied,0));
			const a_real pj = (physics.g-1)*(uright(ied,3) 
					- 0.5*(pow(uright(ied,1),2)+pow(uright(ied,2),2))/uright(ied,0));
			//calculate speeds of sound
			const a_real ci = sqrt(physics.g*pi/uleft(ied,0));
			const a_real cj = sqrt(physics.g*pj/uright(ied,0));
			//calculate normal velocities
			const a_real vni = (uleft(ied,1)*n[0] +uleft(ied,2)*n[1])/uleft(ied,0);
			const a_real vnj = (uright(ied,1)*n[0] + uright(ied,2)*n[1])/uright(ied,0);

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
		if(gettimesteps)
#pragma omp for simd
			for(a_int iel = 0; iel < m->gnelem(); iel++)
			{
				dtm(iel) = m->garea(iel)/integ(iel);
			}
	} // end parallel region
}

#if HAVE_PETSC==1

void FlowFV::compute_jacobian(const MVector& u, const bool blocked, Mat A)
{
	if(blocked)
	{
		// TODO: construct blocked Jacobian
	}
	else
	{
		Array2d<a_real>* D = new Array2d<a_real>[m->gnelem()];
		for(int iel = 0; iel < m->gnelem(); iel++) {
			D[iel].resize(NVARS,NVARS);
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
 * \f$ D_{ii} \rightarrow D_{ii} -L_{ij}, D_{jj} \rightarrow D_{jj} -U_{ij} \f$.
 */
void FlowFV::compute_jacobian(const MVector& u, 
				LinearOperator<a_real,a_int> *const __restrict A)
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
		Matrix<a_real,NVARS,NVARS,RowMajor> left;
		Matrix<a_real,NVARS,NVARS,RowMajor> right;
		
		compute_boundary_state(iface, &u(lelem,0), uface);
		jflux->get_jacobian(&u(lelem,0), uface, n, &left(0,0), &right(0,0));
		
		// multiply by length of face and negate, as -ve of L is added to D
		left = -len*left;
		A->updateDiagBlock(lelem*NVARS, left.data(), NVARS);
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
		Matrix<a_real,NVARS,NVARS,RowMajor> L;
		Matrix<a_real,NVARS,NVARS,RowMajor> U;
	
		/// NOTE: the values of L and U get REPLACED here, not added to
		jflux->get_jacobian(&u(lelem,0), &u(relem,0), n, &L(0,0), &U(0,0));

		L *= len; U *= len;
		if(A->type()=='d') {
			A->submitBlock(relem*NVARS,lelem*NVARS, L.data(), 1,intface);
			A->submitBlock(lelem*NVARS,relem*NVARS, U.data(), 2,intface);
		}
		else {
			A->submitBlock(relem*NVARS,lelem*NVARS, L.data(), NVARS,NVARS);
			A->submitBlock(lelem*NVARS,relem*NVARS, U.data(), NVARS,NVARS);
		}

		// negative L and U contribute to diagonal blocks
		L *= -1.0; U *= -1.0;
		A->updateDiagBlock(lelem*NVARS, L.data(), NVARS);
		A->updateDiagBlock(relem*NVARS, U.data(), NVARS);
	}
}

#endif

void FlowFV::postprocess_point(const MVector& u, amat::Array2d<a_real>& scalars, 
		amat::Array2d<a_real>& velocities)
{
	std::cout << "FlowFV: postprocess_point(): Creating output arrays...\n";
	scalars.resize(m->gnpoin(),3);
	velocities.resize(m->gnpoin(),2);
	
	amat::Array2d<a_real> areasum(m->gnpoin(),1);
	amat::Array2d<a_real> up(m->gnpoin(), NVARS);
	up.zeros();
	areasum.zeros();

	for(a_int ielem = 0; ielem < m->gnelem(); ielem++)
	{
		for(int inode = 0; inode < m->gnnode(ielem); inode++)
			for(int ivar = 0; ivar < NVARS; ivar++)
			{
				up(m->ginpoel(ielem,inode),ivar) += u(ielem,ivar)*m->garea(ielem);
				areasum(m->ginpoel(ielem,inode)) += m->garea(ielem);
			}
	}

	for(a_int ipoin = 0; ipoin < m->gnpoin(); ipoin++)
		for(short ivar = 0; ivar < NVARS; ivar++)
			up(ipoin,ivar) /= areasum(ipoin);
	
	for(a_int ipoin = 0; ipoin < m->gnpoin(); ipoin++)
	{
		scalars(ipoin,0) = up(ipoin,0);
		velocities(ipoin,0) = up(ipoin,1)/up(ipoin,0);
		velocities(ipoin,1) = up(ipoin,2)/up(ipoin,0);
		//velocities(ipoin,0) = dudx(ipoin,1);
		//velocities(ipoin,1) = dudy(ipoin,1);
		a_real vmag2 = pow(velocities(ipoin,0), 2) + pow(velocities(ipoin,1), 2);
		scalars(ipoin,2) = physics.getPressureFromConserved(&up(ipoin,0));
		a_real c = physics.getSoundSpeedFromConserved(&up(ipoin,0));
		scalars(ipoin,1) = sqrt(vmag2)/c;
	}

	compute_entropy_cell(u);

	std::cout << "FlowFV: postprocess_point(): Done.\n";
}

void FlowFV::postprocess_cell(const MVector& u, amat::Array2d<a_real>& scalars, 
		amat::Array2d<a_real>& velocities)
{
	std::cout << "FlowFV: postprocess_cell(): Creating output arrays...\n";
	scalars.resize(m->gnelem(), 3);
	velocities.resize(m->gnelem(), 2);

	for(a_int iel = 0; iel < m->gnelem(); iel++) {
		scalars(iel,0) = u(iel,0);
	}

	for(a_int iel = 0; iel < m->gnelem(); iel++)
	{
		velocities(iel,0) = u(iel,1)/u(iel,0);
		velocities(iel,1) = u(iel,2)/u(iel,0);
		a_real vmag2 = pow(velocities(iel,0), 2) + pow(velocities(iel,1), 2);
		scalars(iel,2) = physics.getPressureFromConserved(&u(iel,0));
		a_real c = physics.getSoundSpeedFromConserved(&u(iel,0));
		scalars(iel,1) = sqrt(vmag2)/c;
	}
	compute_entropy_cell(u);
	std::cout << "FlowFV: postprocess_cell(): Done.\n";
}

a_real FlowFV::compute_entropy_cell(const MVector& u)
{
	a_real sinf = physics.getEntropyFromConserved(&uinf(0,0));

	amat::Array2d<a_real> s_err(m->gnelem(),1);
	a_real error = 0;
	for(a_int iel = 0; iel < m->gnelem(); iel++)
	{
		s_err(iel) = (physics.getEntropyFromConserved(&u(iel,0)) - sinf) / sinf;
		error += s_err(iel)*s_err(iel)*m->garea(iel);
	}
	error = sqrt(error);

	a_real h = 1.0/sqrt(m->gnelem());
 
	std::cout << "FlowFV:   " << log10(h) << "  " 
		<< std::setprecision(10) << log10(error) << std::endl;

	return error;
}


template<short nvars>
Diffusion<nvars>::Diffusion(const UMesh2dh *const mesh, const a_real diffcoeff, const a_real bvalue,
		std::function< 
		void(const a_real *const, const a_real, const a_real *const, a_real *const)
			> sourcefunc)
	: Spatial<nvars>(mesh), diffusivity{diffcoeff}, bval{bvalue}, source(sourcefunc)
{
	h.resize(m->gnelem());
	for(a_int iel = 0; iel < m->gnelem(); iel++) {
		h[iel] = 0;
		// max face length
		for(int ifael = 0; ifael < m->gnfael(iel); ifael++) {
			a_int face = m->gelemface(iel,ifael);
			if(h[iel] < m->ggallfa(face,2)) h[iel] = m->ggallfa(face,2);
		}
	}
}

template<short nvars>
Diffusion<nvars>::~Diffusion()
{ }

// Currently, all boundaries are constant Dirichlet
template<short nvars>
inline void Diffusion<nvars>::compute_boundary_state(const int ied, 
		const a_real *const ins, a_real *const bs)
{
	for(short ivar = 0; ivar < nvars; ivar++)
		bs[ivar] = 2.0*bval - ins[ivar];
}

template<short nvars>
void Diffusion<nvars>::compute_boundary_states(const amat::Array2d<a_real>& instates, 
                                                amat::Array2d<a_real>& bounstates)
{
	for(a_int ied = 0; ied < m->gnbface(); ied++)
		compute_boundary_state(ied, &instates(ied,0), &bounstates(ied,0));
}

template<short nvars>
void Diffusion<nvars>::postprocess_point(const MVector& u, amat::Array2d<a_real>& up)
{
	std::cout << "Diffusion: postprocess_point(): Creating output arrays\n";
	
	amat::Array2d<a_real> areasum(m->gnpoin(),1);
	up.resize(m->gnpoin(), nvars);
	up.zeros();
	areasum.zeros();

	for(a_int ielem = 0; ielem < m->gnelem(); ielem++)
	{
		for(int inode = 0; inode < m->gnnode(ielem); inode++)
			for(short ivar = 0; ivar < nvars; ivar++)
			{
				up(m->ginpoel(ielem,inode),ivar) += u(ielem,ivar)*m->garea(ielem);
				areasum(m->ginpoel(ielem,inode)) += m->garea(ielem);
			}
	}

	for(a_int ipoin = 0; ipoin < m->gnpoin(); ipoin++)
		for(short ivar = 0; ivar < nvars; ivar++)
			up(ipoin,ivar) /= areasum(ipoin);
}

	template<short nvars>
DiffusionMA<nvars>::DiffusionMA(const UMesh2dh *const mesh, 
		const a_real diffcoeff, const a_real bvalue,
	std::function<void(const a_real *const,const a_real,const a_real *const,a_real *const)> sf, 
		std::string reconst)
	: Diffusion<nvars>(mesh, diffcoeff, bvalue, sf)
{
	std::cout << "  DiffusionMA: Selected reconstruction scheme is " << reconst << std::endl;
	if(reconst == "LEASTSQUARES")
	{
		rec = new WeightedLeastSquaresReconstruction<nvars>(m, &rc, &rcg);
		std::cout << "  DiffusionMA: Weighted least-squares reconstruction will be used.\n";
	}
	else if(reconst == "GREENGAUSS")
	{
		rec = new GreenGaussReconstruction<nvars>(m, &rc, &rcg);
		std::cout << "  DiffusionMA: Green-Gauss reconstruction will be used." << std::endl;
	}
	else /*if(reconst == "NONE")*/ {
		rec = new ConstantReconstruction<nvars>(m, &rc, &rcg);
		std::cout << "  DiffusionMA: No reconstruction; first order solution." << std::endl;
	}
}

template<short nvars>
DiffusionMA<nvars>::~DiffusionMA()
{
	delete rec;
}

template<short nvars>
void DiffusionMA<nvars>::compute_residual(const MVector& u, 
                                          MVector& __restrict residual, 
                                          const bool gettimesteps, 
										  amat::Array2d<a_real>& __restrict dtm)
{
	amat::Array2d<a_real> dudx;
	amat::Array2d<a_real> dudy;
	amat::Array2d<a_real> uleft;
	amat::Array2d<a_real> ug;
	
	dudx.resize(m->gnelem(),nvars);
	dudy.resize(m->gnelem(),nvars);
	uleft.resize(m->gnaface(),nvars);
	ug.resize(m->gnbface(),nvars);

	for(a_int ied = 0; ied < m->gnbface(); ied++)
	{
		a_int ielem = m->gintfac(ied,0);
		for(short ivar = 0; ivar < nvars; ivar++)
			uleft(ied,ivar) = u(ielem,ivar);
	}
	
	compute_boundary_states(uleft, ug);
	rec->compute_gradients(&u, &ug, &dudx, &dudy);
	
	for(a_int iface = m->gnbface(); iface < m->gnaface(); iface++)
	{
		a_int lelem = m->gintfac(iface,0);
		a_int relem = m->gintfac(iface,1);
		a_real len = m->ggallfa(iface,2);
		a_real dr[NDIM], dist=0, sn=0, gradterm[nvars];
		for(int i = 0; i < NDIM; i++) {
			dr[i] = rc(relem,i)-rc(lelem,i);
			dist += dr[i]*dr[i];
		}
		dist = sqrt(dist);
		for(int i = 0; i < NDIM; i++) {
			sn += dr[i]/dist * m->ggallfa(iface,i);
		}

		// compute modified gradient
		for(short ivar = 0; ivar < nvars; ivar++) {
			gradterm[ivar] 
			 = 0.5*(dudx(lelem,ivar)+dudx(relem,ivar)) * (m->ggallfa(iface,0) - sn*dr[0]/dist)
			 + 0.5*(dudy(lelem,ivar)+dudy(relem,ivar)) * (m->ggallfa(iface,1) - sn*dr[1]/dist);
		}

		for(short ivar = 0; ivar < nvars; ivar++){
			a_real flux {diffusivity * 
				(gradterm[ivar] + (u(relem,ivar)-u(lelem,ivar))/dist * sn) * len};
#pragma omp atomic
			residual(lelem,ivar) -= flux;
#pragma omp atomic
			residual(relem,ivar) += flux;
		}
	}
	
	for(int iface = 0; iface < m->gnbface(); iface++)
	{
		a_int lelem = m->gintfac(iface,0);
		a_real len = m->ggallfa(iface,2);
		a_real dr[NDIM], dist=0, sn=0, gradterm[nvars];
		for(int i = 0; i < NDIM; i++) {
			dr[i] = rcg(iface,i)-rc(lelem,i);
			dist += dr[i]*dr[i];
		}
		dist = sqrt(dist);
		for(int i = 0; i < NDIM; i++) {
			sn += dr[i]/dist * m->ggallfa(iface,i);
		}
		
		// compute modified gradient
		for(short ivar = 0; ivar < nvars; ivar++)
			gradterm[ivar] = dudx(lelem,ivar) * (m->ggallfa(iface,0) - sn*dr[0]/dist)
							+dudy(lelem,ivar) * (m->ggallfa(iface,1) - sn*dr[1]/dist);

		for(int ivar = 0; ivar < nvars; ivar++){
#pragma omp atomic
			residual(lelem,ivar) -= diffusivity * 
				( (ug(iface,ivar)-u(lelem,ivar))/dist*sn + gradterm[ivar]) * len;
		}
	}

	for(int iel = 0; iel < m->gnelem(); iel++) {
		if(gettimesteps)
			dtm(iel) = h[iel]*h[iel]/diffusivity;

		// subtract source term
		a_real sourceterm;
		source(&rc(iel,0), 0, &u(iel,0), &sourceterm);
		residual(iel,0) -= sourceterm*m->garea(iel);
	}
}

/** For now, this is the same as the thin-layer Jacobian
 */
template<short nvars>
void DiffusionMA<nvars>::compute_jacobian(const MVector& u,
		LinearOperator<a_real,a_int> *const A)
{
	for(a_int iface = m->gnbface(); iface < m->gnaface(); iface++)
	{
		//a_int intface = iface-m->gnbface();
		a_int lelem = m->gintfac(iface,0);
		a_int relem = m->gintfac(iface,1);
		a_real len = m->ggallfa(iface,2);

		a_real dr[NDIM], dist=0, sn=0;
		for(int i = 0; i < NDIM; i++) {
			dr[i] = rc(relem,i)-rc(lelem,i);
			dist += dr[i]*dr[i];
		}
		dist = sqrt(dist);
		for(int i = 0; i < NDIM; i++) {
			sn += dr[i]/dist * m->ggallfa(iface,i);
		}

		a_real ll[nvars*nvars];
		for(short ivar = 0; ivar < nvars; ivar++) {
			for(short jvar = 0; jvar < nvars; jvar++)
				ll[ivar*nvars+jvar] = 0;
			
			ll[ivar*nvars+ivar] = -diffusivity * sn*len/dist;
		}

		a_int faceid = iface - m->gnbface();
		if(A->type() == 'd') {
			A->submitBlock(relem*nvars,lelem*nvars, ll, 1,faceid);
			A->submitBlock(lelem*nvars,relem*nvars, ll, 2,faceid);
		}
		else {
			A->submitBlock(relem*nvars,lelem*nvars, ll, nvars,nvars);
			A->submitBlock(lelem*nvars,relem*nvars, ll, nvars,nvars);
		}
		
		for(short ivar = 0; ivar < nvars; ivar++)
			ll[ivar*nvars+ivar] *= -1;

		A->updateDiagBlock(lelem*nvars, ll, nvars);
		A->updateDiagBlock(relem*nvars, ll, nvars);
	}
	
	for(a_int iface = 0; iface < m->gnbface(); iface++)
	{
		a_int lelem = m->gintfac(iface,0);
		a_real len = m->ggallfa(iface,2);

		a_real dr[NDIM], dist=0, sn=0;
		for(int i = 0; i < NDIM; i++) {
			dr[i] = rcg(iface,i)-rc(lelem,i);
			dist += dr[i]*dr[i];
		}
		dist = sqrt(dist);
		for(int i = 0; i < NDIM; i++) {
			sn += dr[i]/dist * m->ggallfa(iface,i);
		}

		a_real ll[nvars*nvars];
		for(short ivar = 0; ivar < nvars; ivar++) {
			for(short jvar = 0; jvar < nvars; jvar++)
				ll[ivar*nvars+jvar] = 0;
			
			ll[ivar*nvars+ivar] = diffusivity * sn*len/dist;
		}

		A->updateDiagBlock(lelem*nvars, ll, nvars);
	}
}

template class DiffusionMA<1>;

}	// end namespace
