// ===========================================================
//
// saige_main.cpp: SAIGE association analysis for each variant
//
// Copyright (C) 2019    Xiuwen Zheng
//
// This file is part of SAIGEgds.
//
// SAIGEgds is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License Version 3 as published
// by the Free Software Foundation.
//
// SAIGEgds is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with SNPRelate.
// If not, see <http://www.gnu.org/licenses/>.

#include "vectorization.h"
#include <Rcpp.h>
#include <algorithm>


using namespace Rcpp;



// ========================================================================= //
// internal functions

/// SPAtest
extern "C" double Saddle_Prob(double q, double m1, double var1, size_t n_g,
	const double mu[], const double g[], double cutoff, bool &converged);

/// square
inline double sq(double v) { return v*v; }


// ========================================================================= //

static double threshold_maf = 0;  //< the threshold of MAF filter
static double threshold_mac = 0;  //< the threshold of MAC filter

static int mod_NSamp = 0;   //< the number of samples
static int mod_NCoeff = 0;  //< the number of beta coefficients

static double *mod_tau = NULL;           //< variance components: tau[0], tau[1]
static double *mod_y = NULL;             //< a n_samp-length vector
static double *mod_mu = NULL;            //< a n_samp-length vector
static double *mod_y_mu = NULL;          //< a n_samp-length vector, y-mu
static double *mod_mu2 = NULL;           //< a n_samp-length vector, mu*(1-mu)
static double *mod_t_XXVX_inv = NULL;    //< a K-by-n_samp matrix
static double *mod_XV = NULL;            //< a K-by-n_samp matrix
static double *mod_t_XVX_inv_XV = NULL;  //< a K-by-n_samp matrix
static double *mod_XVX = NULL;           //< a K-by-K matrix
static double *mod_t_X = NULL;           //< a K-by-n_samp matrix
static double *mod_S_a = NULL;           //< a K-length vector

static double mod_varRatio = 0;

static double *buf_coeff = NULL;
static double *buf_adj_g = NULL;
static int *buf_index = NULL;
static double *buf_B = NULL;
static double *buf_g_tilde = NULL;
static double *buf_tmp = NULL;

#define IDX_i    buf_index[i]


/// initialize internal parameters from the model object
RcppExport SEXP saige_score_test_init(SEXP model)
{
BEGIN_RCPP
	List M(model);
	// threshold setting
	threshold_maf = Rf_asReal(M["maf"]);
	if (!R_FINITE(threshold_maf)) threshold_maf = -1;
	threshold_mac = Rf_asReal(M["mac"]);
	if (!R_FINITE(threshold_mac)) threshold_mac = -1;
	// model parameters
	mod_NSamp = Rf_length(M["y"]);
	mod_NCoeff = NumericMatrix(wrap(M["XV"])).nrow();
	mod_tau = REAL(M["tau"]);
	mod_y = REAL(M["y"]);
	mod_mu = REAL(M["mu"]);
	mod_y_mu = REAL(M["y_mu"]);
	mod_mu2 = REAL(M["mu2"]);
	mod_t_XXVX_inv = REAL(M["t_XXVX_inv"]);
	mod_XV = REAL(M["XV"]);
	mod_t_XVX_inv_XV = REAL(M["t_XVX_inv_XV"]);
	mod_XVX = REAL(M["XVX"]);
	mod_t_X = REAL(M["t_X"]);
	mod_S_a = REAL(M["S_a"]);
	mod_varRatio = Rf_asReal(M["var.ratio"]);
	// buffer
	buf_coeff = REAL(M["buf_coeff"]);
	buf_adj_g = REAL(M["buf_adj_g"]);
	buf_index = INTEGER(M["buf_index"]);
	buf_B = REAL(M["buf_B"]);
	buf_g_tilde = REAL(M["buf_g_tilde"]);
	buf_tmp = REAL(M["buf_tmp"]);
END_RCPP
}



// ========================================================================= //

/// calculate p-values for quantitative outcome
RcppExport SEXP saige_score_test_quant(SEXP dosage)
{
BEGIN_RCPP

	// dosages
	NumericVector G(dosage);
	const size_t num_samp = G.size();
	// calc allele freq, and impute geno using the mean
	double AF, AC;
	int Num;
	f64_af_ac_impute(&G[0], num_samp, AF, AC, Num, buf_index);

	double maf = std::min(AF, 1-AF);
	double mac = std::min(AC, 2*Num - AC);
	if (Num>0 && maf>0 && maf>=threshold_maf && mac>=threshold_mac)
	{
		bool minus = (AF > 0.5);
		if (minus) f64_sub(mod_NSamp, 2, &G[0]);

		const double inv_sqrt_mac = 1.0 / sqrt(mac);
		const double inv_mac = 1.0 / mac;
		double pval, beta;
		// if (maf < 0.05)
		if (maf > -0.05)
		{
			// get the number of nonzeros and the nonzero indices
			size_t n_nonzero = f64_nonzero_index(mod_NSamp, &G[0], buf_index);
			// buf_coeff = XVX_inv_XV * G
			f64_mul_mat_vec_sp(n_nonzero, buf_index, mod_NCoeff,
				mod_t_XVX_inv_XV, &G[0], buf_coeff);
			// buf_B = t(X) * buf_coeff
			f64_mul_mat_vec_sub(n_nonzero, buf_index, mod_NCoeff, mod_t_X,
				buf_coeff, buf_B);
			// g_tilde = G - B
			for (size_t i=0; i < n_nonzero; i++)
				buf_g_tilde[i] = G[IDX_i] - buf_B[i];
			// var2 = t(buf_coeff) %*% XVX %*% buf_coeff - sum(B^2) + sum(g_tilde^2)
			double var2 = f64_sum_mat_vec(mod_NCoeff, mod_XVX, buf_coeff);
			for (size_t i=0; i < n_nonzero; i++)
				var2 += sq(buf_g_tilde[i]) - sq(buf_B[i]);
			double var1 = var2 * inv_mac * mod_varRatio;
			// S1 = sum(y_mu .* g_tilde)
			double S1 = 0;
			for (size_t i=0; i < n_nonzero; i++)
				S1 += mod_y_mu[IDX_i] * buf_g_tilde[i];
			S1 *= inv_sqrt_mac;
			// buf_tmp = t(X1) * (y-mu)
			f64_mul_mat_vec_sp(n_nonzero, buf_index, mod_NCoeff, mod_t_X,
				mod_y_mu, buf_tmp);
			// S2 = sum((buf_tmp - mod_S_a) .* buf_coeff)
			double S2 = 0;
			for (int i=0; i < mod_NCoeff; i++)
				S2 += (buf_tmp[i] - mod_S_a[i]) * buf_coeff[i];
			//
			double Tstat = (S1 + S2) / mod_tau[0];
			pval = ::Rf_pchisq(Tstat*Tstat/var1, 1, FALSE, FALSE);
			beta = (minus ? -1 : 1) * Tstat / var1 * inv_sqrt_mac;

		} else {
			// adj_g = G - XXVX_inv * (XV * G), adjusted genotypes
			// buf_coeff = XV * G
			f64_mul_mat_vec(mod_NSamp, mod_NCoeff, mod_XV, &G[0], buf_coeff);
			// buf_adj_g = G - XXVX_inv * buf_coeff
			f64_sub_mul_mat_vec(mod_NSamp, mod_NCoeff, &G[0], mod_t_XXVX_inv,
				buf_coeff, buf_adj_g);

			// inner product
			double S, var;
			// S = sum((y - mu) .* buf_adj_g
			// var = sum(buf_adj_g .* buf_adj_g)
			f64_dot_sp(mod_NSamp, mod_y_mu, buf_adj_g, S, var);
			double Tstat = S * inv_sqrt_mac / mod_tau[0];
			var *= inv_mac * mod_varRatio;

			// p-value and beta
			pval = ::Rf_pchisq(Tstat*Tstat/var, 1, FALSE, FALSE);
			beta = (minus ? -1 : 1) * Tstat / var * inv_sqrt_mac;
		}

		double SE = abs(beta/::Rf_qnorm5(pval/2, 0, 1, TRUE, FALSE));

		NumericVector ans(6);
		ans[0] = AF;    ans[1] = AC;    ans[2] = Num;
		ans[3] = beta;  ans[4] = SE;    ans[5] = pval;
		return ans;
	} else {
		return R_NilValue;
	}

END_RCPP
}


/// calculate p-values for binary outcome
RcppExport SEXP saige_score_test_bin(SEXP dosage)
{
BEGIN_RCPP

	// dosages
	NumericVector G(dosage);
	const size_t num_samp = G.size();
	// calc allele freq, and impute geno using the mean
	double AF, AC;
	int Num;
	f64_af_ac_impute(&G[0], num_samp, AF, AC, Num, buf_index);

	double maf = std::min(AF, 1-AF);
	double mac = std::min(AC, 2*Num - AC);
	if (Num>0 && maf>0 && maf>=threshold_maf && mac>=threshold_mac)
	{
		bool minus = (AF > 0.5);
		if (minus) f64_sub(mod_NSamp, 2, &G[0]);

		double pval_noadj, beta;
		if (maf < 0.05)
		{
			// get the number of nonzeros and the nonzero indices
			size_t n_nonzero = f64_nonzero_index(mod_NSamp, &G[0], buf_index);
			// buf_coeff = XVX_inv_XV * G
			f64_mul_mat_vec_sp(n_nonzero, buf_index, mod_NCoeff,
				mod_t_XVX_inv_XV, &G[0], buf_coeff);
			// buf_B = t(X) * buf_coeff
			f64_mul_mat_vec_sub(n_nonzero, buf_index, mod_NCoeff, mod_t_X,
				buf_coeff, buf_B);
			// g_tilde = G - B
			for (size_t i=0; i < n_nonzero; i++)
				buf_g_tilde[i] = G[IDX_i] - buf_B[i];
			// var2 = t(buf_coeff) %*% XVX %*% buf_coeff - sum(B^2 .* mu2) + sum(g_tilde^2 .* mu2)
			double var2 = f64_sum_mat_vec(mod_NCoeff, mod_XVX, buf_coeff);
			for (size_t i=0; i < n_nonzero; i++)
				var2 += (sq(buf_g_tilde[i]) - sq(buf_B[i])) * mod_mu2[IDX_i];
			double var1 = var2 * mod_varRatio;
			// S1 = sum(y_mu .* g_tilde)
			double S1 = 0;
			for (size_t i=0; i < n_nonzero; i++)
				S1 += mod_y_mu[IDX_i] * buf_g_tilde[i];
			// buf_tmp = t(X1) * (y-mu)
			f64_mul_mat_vec_sp(n_nonzero, buf_index, mod_NCoeff, mod_t_X,
				mod_y_mu, buf_tmp);
			// S2 = sum((buf_tmp - mod_S_a) .* buf_coeff)
			double S2 = 0;
			for (int i=0; i < mod_NCoeff; i++)
				S2 += (buf_tmp[i] - mod_S_a[i]) * buf_coeff[i];
			//
			double S = S1 + S2;
			pval_noadj = ::Rf_pchisq(S*S/var1, 1, FALSE, FALSE);
			beta = (minus ? -1 : 1) * S / var1;

		} else {
			// adj_g = G - XXVX_inv * (XV * G), adjusted genotypes
			// buf_coeff = XV * G
			f64_mul_mat_vec(mod_NSamp, mod_NCoeff, mod_XV, &G[0], buf_coeff);
			// buf_adj_g = G - XXVX_inv * buf_coeff
			f64_sub_mul_mat_vec(mod_NSamp, mod_NCoeff, &G[0], mod_t_XXVX_inv,
				buf_coeff, buf_adj_g);
			// inner product
			double S, var;
			// S = sum((y - mu) .* buf_adj_g)
			// var = sum(mu*(1-mu) .* buf_adj_g .* buf_adj_g)
			f64_dot_sp2(mod_NSamp, mod_y_mu, mod_mu2, buf_adj_g, S, var);
			var *= mod_varRatio;
			// p-value and beta
			pval_noadj = ::Rf_pchisq(S*S/var, 1, FALSE, FALSE);
			beta = (minus ? -1 : 1) * S / var;
		}

		double pval = pval_noadj;
		bool converged = true;

		// need SPAtest or not?
		if (R_FINITE(pval_noadj) && pval_noadj <= 0.05)
		{
			double AC2 = minus ? (2*Num - AC) : AC;
			// adj_g = adj_g / sqrt(AC2)
			f64_mul(mod_NSamp, 1/sqrt(AC2), buf_adj_g);
			// q = sum(y .* adj_g)
			double q = f64_dot(mod_NSamp, mod_y, buf_adj_g);
			double m1, var2;
			// m1 = sum(mu .* adj_g)
			// var2 = sum(mu*(1-mu) .* adj_g .* adj_g)
			f64_dot_sp2(mod_NSamp, mod_mu, mod_mu2, buf_adj_g, m1, var2);
			double var1 = var2 * mod_varRatio;
			double Tstat = q - m1;
			double qtilde = Tstat/sqrt(var1) * sqrt(var2) + m1;
			// call Saddle_Prob in SPAtest
			pval = Saddle_Prob(qtilde, m1, var2, mod_NSamp, mod_mu,
				buf_adj_g, 2, converged);
			beta = (Tstat / var1) / sqrt(AC2);
		}
		double SE = abs(beta/::Rf_qnorm5(pval/2, 0, 1, TRUE, FALSE));

		NumericVector ans(8);
		ans[0] = AF;    ans[1] = AC;    ans[2] = Num;
		ans[3] = beta;  ans[4] = SE;    ans[5] = pval;
		ans[6] = pval_noadj;
		ans[7] = converged ? 1 : 0;
		return ans;
	} else {
		return R_NilValue;
	}

END_RCPP
}


// ========================================================================= //

RcppExport SEXP saige_simd_version();
RcppExport SEXP saige_store_geno(SEXP, SEXP, SEXP, SEXP, SEXP);

/// initialize the package
RcppExport void R_init_SAIGEgds(DllInfo *info)
{
	#define CALL(name, num)	   { #name, (DL_FUNC)&name, num }

	static R_CallMethodDef callMethods[] =
	{
		CALL(saige_score_test_init, 1),
		CALL(saige_simd_version, 0),
		CALL(saige_store_geno, 5),
		{ NULL, NULL, 0 }
	};

	R_registerRoutines(info, NULL, callMethods, NULL, NULL);
}
