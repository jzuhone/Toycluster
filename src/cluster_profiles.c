#include <gsl/gsl_integration.h>
#include <gsl/gsl_spline.h>
#include "globals.h"

#define NTABLE 1024

static void setup_internal_energy_profile(const int i);
static void setup_gas_potential_profile(const int i);
static void	setup_dm_mass_profile(const int i);
static void setup_dm_potential_profile(const int i);

static struct int_param {
	double rho0;
	double beta;
	double rc;
	double rcut;
	double cuspy;
} ip;

/* This file contains all physical profiles that describe the cluster models */

void Setup_Profiles(const int i)
{

	setup_dm_mass_profile(i);
	setup_dm_potential_profile(i);

	if (Cosmo.Baryon_Fraction > 0) {

		Setup_Gas_Mass_Profile(i);
		setup_gas_potential_profile(i);
		setup_internal_energy_profile(i);
	}
	return ;
}

double Hernquist_Density_Profile(const double m, const double a, const double r)
{
	return m / (2*pi) * a/(r*p3(r+a)); // Hernquist 1989, eq. 2 , eq. 17-19
}

double DM_Density_Profile(const int i, const float r)
{
    const double rs = Halo[i].Rs;
    const double rho0 = Halo[i].Rho0_nfw; 

	return rho0 / (r/rs * p2(1+r/rs));
}

double DM_Mass_Profile(const double r, const int i)
{
	double rs = Halo[i].Rs;
	double rho0 = Halo[i].Rho0_nfw; // NFW profile (Wiki)

	return 4*pi*rho0*p3(rs) * (log((rs+r)/rs) - r/(rs+r));
}


static gsl_spline *DMMinv_Spline = NULL;
static gsl_interp_accel *DMMinv_Acc = NULL;
#pragma omp threadprivate(DMMinv_Spline, DMMinv_Acc)

void setup_dm_mass_profile(const int iCluster)
{
	const double Mdm = Halo[iCluster].Mass[1];

	double m_table[NTABLE] = { 0 };
	double r_table[NTABLE] = { 0 };

	double rmin = 0.1;
	double rmax = Param.Boxsize/2;
	double log_dr = ( log10(rmax/rmin) ) / (NTABLE - 1);

	for (int i = 1; i < NTABLE; i++) {
	
		r_table[i] = rmin * pow(10, log_dr * i);
		m_table[i] = DM_Mass_Profile(r_table[i], iCluster) / Mdm;
	}
	m_table[0] = 0;

	#pragma omp parallel
	{

	DMMinv_Acc  = gsl_interp_accel_alloc();

	DMMinv_Spline = gsl_spline_alloc(gsl_interp_cspline, NTABLE);
	gsl_spline_init(DMMinv_Spline, m_table, r_table, NTABLE);

	} // omp parallel

	return ;
}

double Inverted_DM_Mass_Profile(double q, const int i)
{
	return gsl_spline_eval(DMMinv_Spline, q, DMMinv_Acc);
}

double DM_Potential_Profile(const int i, const float r)
{
    const double a = Halo[i].A_hernq;
    const double mDM = Halo[i].Mass[1]; 
	
	double psi = G * mDM / (r+a); // This is Psi = -Phi, i.e. Psi(r<inf) >= 0

	return psi;
}

static void setup_dm_potential_profile(const int i)
{
	return ;

}


/**********************************************************************/

/* 
 * Double beta profile at rc and rcut 
 */

double Gas_Density_Profile(const double r, const double rho0, 
						   const double beta, const double rc, 
						   const double rcut, const bool Is_Cuspy)
{
	double rho = rho0 * pow(1 + p2(r/rc), -3.0/2.0*beta) 
				/ (1 + p3(r/rcut) * (r/rcut));

#ifdef DOUBLE_BETA_COOL_CORES

	double rho0_cc = rho0 * Param.Rho0_Fac;
	double rc_cc = rc / Param.Rc_Fac;
	
	if (Is_Cuspy)
		rho += rho0_cc / (1 + p2(r/rc_cc)) / (1 + p3(r/rcut) * (r/rcut));

#endif // DOUBLE_BETA_COOL_CORES

	return rho;
}

/*
 * Mass profile from the gas density profile by numerical 
 * integration and cubic spline interpolation. This has to be called once 
 * before the mass profile of a halo is used, to set the spline variables.
 */

static gsl_spline *M_Spline = NULL;
static gsl_interp_accel *M_Acc = NULL;
#pragma omp threadprivate(M_Spline, M_Acc)

static gsl_spline *Minv_Spline = NULL;
static gsl_interp_accel *Minv_Acc = NULL;
#pragma omp threadprivate(Minv_Spline, Minv_Acc)

double Gas_Mass_Profile(const double r_in, const int i)
{
	double r = fmin(r_in, Halo[i].R_Sample[0]);

	return  gsl_spline_eval(M_Spline, r, M_Acc);
}

double Inverted_Gas_Mass_Profile(double M)
{
	return gsl_spline_eval(Minv_Spline, M, Minv_Acc);
}

double m_integrant(double r, void * param)
{
	struct int_param *ip = ((struct int_param *) param); 

	return 4*pi*r*r*Gas_Density_Profile(r, ip->rho0, ip->beta, ip->rc, 
										   ip->rcut, ip->cuspy);
}

static double Rmax = 0;

void Setup_Gas_Mass_Profile(const int j)
{
	const double rho0 = Halo[j].Rho0;
	const double rc = Halo[j].Rcore; 
	const double beta = Halo[j].Beta;
	const double rcut = Halo[j].Rcut;
	const double cuspy = Halo[j].Have_Cuspy;

	double m_table[NTABLE] = { 0 };
	double r_table[NTABLE] = { 0 };
	double dr_table[NTABLE] = { 0 };

	double rmin = 0.1;

	Rmax = Halo[j].R_Sample[0]*1.1; // include R_Sample

	double log_dr = ( log10(Rmax/rmin) ) / (NTABLE - 1);
	
	gsl_function gsl_F = { 0 };
	
	gsl_integration_workspace *gsl_workspace = NULL;
	gsl_workspace = gsl_integration_workspace_alloc(NTABLE);

	for (int i = 1; i < NTABLE; i++) {
		
		double error = 0;

		r_table[i] = rmin * pow(10, log_dr * i);

		struct int_param ip = { rho0, beta, rc, rcut, cuspy };

		gsl_F.function = &m_integrant;
		gsl_F.params = (void *) &ip;

		gsl_integration_qag(&gsl_F, 0, r_table[i], 0, 1e-6, NTABLE, 
				GSL_INTEG_GAUSS41, gsl_workspace, &m_table[i], &error);

		if (m_table[i] < m_table[i-1])
			m_table[i] = m_table[i-1]; // integrator may fluctuate

		printf("%g \n", m_table[i]);
			
	}

	#pragma omp parallel
	{

	M_Acc  = gsl_interp_accel_alloc();

	M_Spline = gsl_spline_alloc(gsl_interp_cspline, NTABLE);
	gsl_spline_init(M_Spline, r_table, m_table, NTABLE);

	Minv_Acc  = gsl_interp_accel_alloc();

	Minv_Spline = gsl_spline_alloc(gsl_interp_cspline, NTABLE);
	gsl_spline_init(Minv_Spline, m_table, r_table, NTABLE);

	} // omp parallel

	return ;
}

/* 
 * return M(<= R) of a beta profile with beta=2/3 
 */

double Mass_Profile_23(const double r, const int i)
{	
	double rho0 = Halo[i].Rho0;
	double rc = Halo[i].Rcore;
	double beta = Halo[i].Beta;
	double rcut = Halo[i].Rcut;
	int Is_Cuspy = Halo[i].Have_Cuspy;
	
	const double r2 = p2(r);
	const double rc2 = p2(rc);
	const double rcut2 = p2(rcut);

	double Mr = rho0 * rc2*rcut2*rcut/(8*(p2(rcut2)+p2(rc2))) * // fourth order
		( sqrt2 *( (rc2-rcut2) *( log(rcut2 - sqrt2*rcut*r+r2) 
								- log(rcut2 + sqrt2*rcut*r+r2)) 
				   - 2 * (rc2 + rcut2) * atan(1 - sqrt2*r/rcut) 
				   + 2 * (rc2 + rcut2) * atan(sqrt2 * r/rcut + 1))
		  - 8 * rc * rcut * atan(r/rc)); 
		
#ifdef DOUBLE_BETA_COOL_CORES

	double rc_cc = rc / Param.Rc_Fac;
	double rc2_cc = p2(rc_cc);
	double rho0_cc = rho0 * Param.Rho0_Fac;
	
	double Mr_cc = 0;

	if (Is_Cuspy)
		Mr += rho0_cc * rc2_cc*rcut2*rcut/(8*(p2(rcut2)+p2(rc2_cc))) * 
			( sqrt2 *( (rc2-rcut2) *( log(rcut2 - sqrt2*rcut*r+r2) 
									- log(rcut2 + sqrt2*rcut*r+r2)) 
					   - 2 * (rc2_cc + rcut2) * atan(1 - sqrt2*r/rcut) 
					   + 2 * (rc2_cc + rcut2) * atan(sqrt2 * r/rcut + 1))
			  - 8 * rc_cc * rcut * atan(r/rc));

#endif // DOUBLE_BETA_COOL_CORES

	return 4 * pi * Mr;
}

/*
 * The grav. potential from the gas density.
 */

static gsl_spline *Psi_Spline = NULL;
static gsl_interp_accel *Psi_Acc = NULL;
#pragma omp threadprivate(Psi_Spline, Psi_Acc)

double Gas_Potential_Profile(const int i, const double r)
{
	double r_max = Halo[i].R_Sample[0];

	if (r < r_max)
		return gsl_spline_eval(Psi_Spline, r, Psi_Acc);

	double psi_max = gsl_spline_eval(Psi_Spline,r_max, Psi_Acc);

	return psi_max*r_max/r;
}

double psi_integrant(double r, void *param)
{
	int i = *((int *) param);

	if (r == 0)
		return 0;

	return G/r/r * Gas_Mass_Profile(r, i);
}

static void setup_gas_potential_profile(const int i)
{
	double error = 0;

	gsl_function gsl_F = { 0 };
	gsl_F.function = &psi_integrant;
	gsl_F.params = (void *) &i;

	gsl_integration_workspace *gsl_workspace = NULL;
	gsl_workspace = gsl_integration_workspace_alloc(4096);

	double psi_table[NTABLE] = { 0 };
	double r_table[NTABLE] = { 0 };

	double rmin = 1;
	double rmax = Halo[i].R_Sample[0] * 1.1;
	double log_dr = ( log10(rmax/rmin) ) / (NTABLE - 1);

	double gauge = 0;
	
	gsl_integration_qag(&gsl_F, 0, 1e100, 0, 1e-6, 4096, 
			GSL_INTEG_GAUSS61, gsl_workspace, &gauge, &error);
	
	for (int j = 1; j < NTABLE; j++) {

		r_table[j] = rmin * pow(10, log_dr * j);
		
		gsl_integration_qag(&gsl_F, 0, r_table[j], 0, 1e-3, 4096, 
				GSL_INTEG_GAUSS61, gsl_workspace, &psi_table[j], &error);

		psi_table[j] = -1*(psi_table[j] - gauge);
	}
	
	r_table[0] = 0;
	psi_table[0] = gauge;

	#pragma omp parallel
	{

	Psi_Acc  = gsl_interp_accel_alloc();

	Psi_Spline = gsl_spline_alloc(gsl_interp_cspline, NTABLE);
	gsl_spline_init(Psi_Spline, r_table, psi_table, NTABLE);
		
	} // omp parallel

	return ;
}

double Gas_Potential_Profile_23(const int i, const float r)
{
	if (r > 2*Halo[i].Rcut)
		return 0;

	double rc = Halo[i].Rcore;
	const double rcut = Halo[i].Rcut;

	const double r2 = r*r;
	double rc2 = rc*rc;
	const double rcut2 = rcut*rcut;

	double psi = -1 * rc2*rcut2/(8*(rc2*rc2 + rcut2*rcut2)*r)
				*(8*rc*rcut2*atan(r/rc) + 4*rc2*r*atan(r2/rcut2) 
				+ rcut*(2*sqrt2*(rc2 + rcut2)*atan(1 - (sqrt2*r)/rcut) 
					- 2*sqrt2*(rc2 + rcut2)* atan(1 + (sqrt2*r)/rcut) 
					+ 4*rcut*r*log(rc2 + r2) 
					- sqrt2*rc2* log(rcut2 - sqrt2*rcut*r + r2) 
					+ sqrt2*rcut2*log(rcut2 - sqrt2*rcut*r + r2) 
					+ sqrt2*rc2*log(rcut2 + sqrt2*rcut*r + r2) 
					- sqrt2*rcut2*log(rcut2 + sqrt2*rcut*r + r2) 
					- 2*rcut*r*log(rcut2*rcut2 + r2*r2))) ; 

	psi *= Halo[i].Rho0;

#ifdef DOUBLE_BETA_COOL_CORES

	rc /= rc / Param.Rc_Fac;
	rc2 = p2(rc);

	double psi_cc = -1 * rc2*rcut2/(8*(rc2*rc2 + rcut2*rcut2)*r)
				*(8*rc*rcut2*atan(r/rc) + 4*rc2*r*atan(r2/rcut2) 
				+ rcut*(2*sqrt2*(rc2 + rcut2)*atan(1 - (sqrt2*r)/rcut) 
					- 2*sqrt2*(rc2 + rcut2)* atan(1 + (sqrt2*r)/rcut) 
					+ 4*rcut*r*log(rc2 + r2) 
					- sqrt2*rc2* log(rcut2 - sqrt2*rcut*r + r2) 
					+ sqrt2*rcut2*log(rcut2 - sqrt2*rcut*r + r2) 
					+ sqrt2*rc2*log(rcut2 + sqrt2*rcut*r + r2) 
					- sqrt2*rcut2*log(rcut2 + sqrt2*rcut*r + r2) 
					- 2*rcut*r*log(rcut2*rcut2 + r2*r2))) ; 

	psi += psi_cc * Halo[i].Rho0 * Param.Rho0_Fac;

#endif // DOUBLE_BETA_COOL_CORES

	//double psi =  p3(rc)*( 0.5/rc*log(rc*rc+r*r) + atan(r/rc)/r )
//			* p2(p2(1 - r/rcut)) // no cutoff

	return 4*pi*G * psi;
}

/**********************************************************************/

/* 
 * Standard analytical temperature profile from Donnert et al. 2014.
 * To avoid negative temperatures we define rmax*sqrt3 as outer radius
 */

static double F1(const double r, const double rc, const double a)
{
    const double rc2 = rc*rc;
    const double a2 = a*a;

    double result = (a2-rc2)*atan(r/rc) - rc*(a2+rc2)/(a+r) 
                + a*rc * log( (a+r)*(a+r) / (rc2 + r*r) );

    result *= rc / p2(a2 + rc2);

    return result;
}

static double F2(const double r, const double rc)
{
    return p2(atan(r/rc)) / (2*rc) + atan(r/rc)/r;
}

double Internal_Energy_Profile_Analytic(const int i, const double d) 
{
    double rho0 = Halo[i].Rho0; 
    double a = Halo[i].A_hernq;
    double rc = Halo[i].Rcore;
    double rmax = Param.Boxsize; // "open" T boundary
    double Mdm = Halo[i].Mass[1];

	double u = G / ( (adiabatic_index-1) ) * ( 1 + p2(d/rc) ) *
                ( Mdm * (F1(rmax, rc, a) - F1(d, rc, a))
                + 4*pi*rho0*p3(rc) * (F2(rmax, rc) - F2(d, rc) ) );
	return u;
}

/* 
 * Numerical solution for all kinds of gas densities. We spline interpolate 
 * a table of solutions for speed. We have to solve the hydrostatic equation
 * (eq. 9 in Donnert 2014).
 */

#define TABLESIZE 1024

static gsl_spline *U_Spline = NULL;
static gsl_interp_accel *U_Acc = NULL;
#pragma omp threadprivate(U_Spline, U_Acc)

double Internal_Energy_Profile(const int i, const double r)
{
	return gsl_spline_eval(U_Spline, r, U_Acc);;
}

static double u_integrant(double r, void *param) // Donnert 2014, eq. 9
{
	int i = *((int *) param);

	double rho0 = Halo[i].Rho0;
	double rc = Halo[i].Rcore;
	double beta = Halo[i].Beta;
	double rcut = Halo[i].Rcut;
	int is_cuspy = Halo[i].Have_Cuspy;
	double a = Halo[i].A_hernq;
	double Mdm = Halo[i].Mass[1];

#ifdef NO_RCUT_IN_T
	rcut = 1e5;
#endif

	double rho_gas = Gas_Density_Profile(r, rho0, beta, rc, rcut, is_cuspy);
	double Mr_Gas = Gas_Mass_Profile(r, i);
	double Mr_DM = DM_Mass_Profile(r,i);

	return rho_gas /(r*r) * (Mr_Gas + Mr_DM);
}

static void setup_internal_energy_profile(const int i)
{
	gsl_function gsl_F = { 0 };

	double u_table[TABLESIZE] = { 0 }, r_table[TABLESIZE] = { 0 };

	double rmin = 0.1;
	double rmax = Param.Boxsize * sqrt(3);
	double dr = ( log10(rmax/rmin) ) / (TABLESIZE-1);

	#pragma omp parallel 
	{
	
	gsl_integration_workspace *gsl_workspace = NULL;
	gsl_workspace = gsl_integration_workspace_alloc(2*TABLESIZE);
	
	#pragma omp for
	for (int j = 1; j < TABLESIZE;  j++) {
	
		double error = 0;

		double r = rmin * pow(10, dr * j);
		
		r_table[j] = r;

		gsl_F.function = &u_integrant;
		gsl_F.params = (void *) &i;

		gsl_integration_qag(&gsl_F, r, rmax, 0, 1e-5, 2*TABLESIZE, 
				GSL_INTEG_GAUSS41, gsl_workspace, &u_table[j], &error);

		double rho0 = Halo[i].Rho0;
		double rc = Halo[i].Rcore;
		double beta = Halo[i].Beta;
		double rcut = Halo[i].Rcut;
		int is_cuspy = Halo[i].Have_Cuspy;

#ifdef NO_RCUT_IN_T
		rcut = 1e6;
#endif
		double rho_gas = Gas_Density_Profile(r, rho0, beta, rc, rcut, is_cuspy);

		u_table[j] *= G/((adiabatic_index-1)*rho_gas); // Donnert 2014, eq. 9
		
		//printf("%d %g %g %g %g \n", j,r, rho_gas, u_table[j], 
		//						u_integrant(r, (void *)&i ));
	}

	u_table[0] = u_table[1];
	//u_table[TABLESIZE-1] = 0;

	gsl_integration_workspace_free(gsl_workspace);
	
	U_Acc = gsl_interp_accel_alloc();

	U_Spline = gsl_spline_alloc(gsl_interp_cspline, TABLESIZE);
	gsl_spline_init(U_Spline, r_table, u_table, TABLESIZE);
	
	} // omp parallel

	return ;
}
